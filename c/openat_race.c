/*
 * openat_race -- does openat(dirfd, name, O_CREAT) race on this OS?
 *
 * N threads are released simultaneously into the same openat() call, on the
 * same not-yet-existing name, in the same directory. O_CREAT is given WITHOUT
 * O_EXCL, so the portable, documented behaviour is that every thread gets a
 * descriptor: one thread creates the file, the others open the file the winner
 * created.
 *
 * On macOS most of the threads instead get ENOENT -- the kernel reports that a
 * file it has just created does not exist.
 *
 * Five control rows run alongside the suspect row, because a single run is
 * only interpretable if the controls are clean:
 *
 *   - openat on an existing file, with and without O_CREAT, isolates the
 *     create-or-open fallback from plain path resolution.
 *   - openat with O_CREAT|O_EXCL asks the kernel to fail on collision. One
 *     winner and EEXIST for everyone else is correct, and it shows the
 *     collision itself is detected reliably.
 *   - open(2) on an assembled full path is the same operation without a
 *     directory descriptor. It is the row that tells an openat bug apart from
 *     a genuinely missing directory.
 *
 * Every row also checks that the race is fail-closed, i.e. that a spurious
 * error is the *only* thing that goes wrong: that some thread always won, that
 * the file exists afterwards, that every successful opener got the same inode,
 * and that the mode is the one that was asked for.
 *
 * Build: cc -O2 -pthread -o openat_race openat_race.c
 * Usage: ./openat_race [threads] [trials]        (default 20 200)
 * Env:   RACE_DIR=<dir>   parent for the temp dirs (default $TMPDIR, else /tmp)
 *
 * Exit: 0 interpretable (REPRODUCES or CLEAN)
 *       1 usage or setup error
 *       2 ANOMALOUS -- a control row returned ENOENT too, or any row failed with
 *         an unexpected errno, so suspect the harness or the environment rather
 *         than the kernel
 *       3 INTEGRITY -- the race was not fail-closed. That is a new and worse
 *         finding than this program was written to look for; report it.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_THREADS 1024
#define NAME        "n"

static inline void spin_hint(void) {
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* --- what the racing threads share ------------------------------------- */

static int  g_nt;                     /* workers in this trial                */
static int  g_dirfd;
static int  g_flags;
static int  g_fullpath;               /* open(2) on g_full instead of openat  */
static char g_full[PATH_MAX + 64];   /* headroom so composing cannot truncate */

/* Arming handshake. g_ready and g_armed are only ever touched under g_mu, so
 * they are plain ints. The two condition variables are deliberately separate:
 * with one shared condvar a worker's signal to main can be consumed by another
 * worker, and the run deadlocks. */
static int g_ready;                   /* workers parked and ready to be released */
static int g_armed;                   /* every worker has arrived                */
static atomic_int g_spinning;         /* workers that have reached the spin      */
static atomic_int g_gate;             /* GO                                      */
static pthread_mutex_t g_mu      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_arrive  = PTHREAD_COND_INITIALIZER;  /* workers -> main */
static pthread_cond_t  g_release = PTHREAD_COND_INITIALIZER;  /* main -> workers */

static atomic_long c_ok, c_enoent, c_eexist, c_other;
static atomic_int  c_last_errno;

static ino_t  *t_ino;                 /* per-thread, so no locking needed */
static mode_t *t_mode;

static void *worker(void *arg) {
    long idx = (long)arg;
    t_ino[idx] = 0;
    t_mode[idx] = 0;

    /* Park on a condvar rather than spinning, so that threads which are
     * already up do not starve the ones still being created. That matters on
     * the 3-4 core machines this tends to get run on. */
    pthread_mutex_lock(&g_mu);
    g_ready++;
    pthread_cond_signal(&g_arrive);
    while (!g_armed)
        pthread_cond_wait(&g_release, &g_mu);
    pthread_mutex_unlock(&g_mu);

    /* Wait cooperatively for the rest of the wake-up cascade. Hot-spinning
     * here is what two earlier versions of this file did, and it was wrong
     * both times: the workers already awake hold every core and starve the
     * ones still inside pthread_cond_wait, so the release ends up spread over
     * a scheduler quantum -- ~10ms -- instead of microseconds, and every
     * worker burns a core for the duration. */
    atomic_fetch_add(&g_spinning, 1);
    while (atomic_load(&g_spinning) < g_nt)
        sched_yield();

    /* Every worker is out of the mutex now, so a hot spin costs little and
     * lands them all in the syscall together. */
    while (!atomic_load_explicit(&g_gate, memory_order_acquire))
        spin_hint();

    int fd = g_fullpath ? open(g_full, g_flags, 0600)
                        : openat(g_dirfd, NAME, g_flags, 0600);
    if (fd < 0) {
        int e = errno;
        if (e == ENOENT)      atomic_fetch_add(&c_enoent, 1);
        else if (e == EEXIST) atomic_fetch_add(&c_eexist, 1);
        else { atomic_fetch_add(&c_other, 1); atomic_store(&c_last_errno, e); }
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) == 0) {
        t_ino[idx]  = st.st_ino;
        t_mode[idx] = st.st_mode & 07777;
    }
    close(fd);
    atomic_fetch_add(&c_ok, 1);
    return NULL;
}

/* --- one row ----------------------------------------------------------- */

struct row {
    const char *label;
    int flags;
    int precreate;        /* create NAME before the threads race           */
    int fullpath;         /* open(2) on an assembled path, not openat      */
    int one_winner;       /* O_EXCL: exactly one success is correct        */
    int suspect;          /* the row under investigation                   */
    int key;              /* kept by the "key" row subset                  */
};

struct result {
    long ok, enoent, eexist, other;
    int  last_errno;
    int  zero_winner;     /* trials where nobody got a descriptor          */
    int  missing_after;   /* trials where NAME did not exist afterwards    */
    int  inode_divergent; /* openers that saw a different inode            */
    int  bad_mode;        /* openers that saw a mode other than 0600       */
};

static const char *parent_dir(void) {
    const char *p = getenv("RACE_DIR");
    if (!p || !*p) p = getenv("TMPDIR");
    if (!p || !*p) p = "/tmp";
    return p;
}

static struct result run_row(const struct row *r, int nt, int trials) {
    struct result res;
    memset(&res, 0, sizeof res);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);   /* 1024 threads is fine */

    for (int t = 0; t < trials; t++) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/oarXXXXXX", parent_dir());
        if (!mkdtemp(dir)) { perror("mkdtemp"); exit(1); }
        snprintf(g_full, sizeof g_full, "%s/%s", dir, NAME);

        g_dirfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (g_dirfd < 0) { perror("open dir"); exit(1); }

        if (r->precreate) {
            int fd = openat(g_dirfd, NAME, O_RDWR | O_CREAT, 0600);
            if (fd < 0) { perror("pre-create"); exit(1); }
            close(fd);
        }

        g_nt = nt;
        g_flags = r->flags;
        g_fullpath = r->fullpath;
        atomic_store(&c_ok, 0); atomic_store(&c_enoent, 0);
        atomic_store(&c_eexist, 0); atomic_store(&c_other, 0);
        g_ready = 0; g_armed = 0;
        atomic_store(&g_spinning, 0); atomic_store(&g_gate, 0);

        pthread_t th[MAX_THREADS];
        for (long i = 0; i < nt; i++) {
            int e = pthread_create(&th[i], &attr, worker, (void *)i);
            if (e) { fprintf(stderr, "pthread_create: %s\n", strerror(e)); exit(1); }
        }

        pthread_mutex_lock(&g_mu);
        while (g_ready < nt)
            pthread_cond_wait(&g_arrive, &g_mu);
        g_armed = 1;
        pthread_cond_broadcast(&g_release);
        pthread_mutex_unlock(&g_mu);

        /* release them the instant the last one is out of the mutex */
        while (atomic_load(&g_spinning) < nt)
            sched_yield();
        atomic_store_explicit(&g_gate, 1, memory_order_release);

        for (int i = 0; i < nt; i++) {
            int e = pthread_join(th[i], NULL);
            if (e) { fprintf(stderr, "pthread_join: %s\n", strerror(e)); exit(1); }
        }

        long ok = atomic_load(&c_ok);
        res.ok += ok;
        res.enoent += atomic_load(&c_enoent);
        res.eexist += atomic_load(&c_eexist);
        long other = atomic_load(&c_other);
        res.other += other;
        if (other) res.last_errno = atomic_load(&c_last_errno);

        /* --- was it fail-closed? --- */
        if (ok == 0) res.zero_winner++;
        struct stat st;
        if (fstatat(g_dirfd, NAME, &st, 0) != 0) res.missing_after++;
        ino_t first = 0;
        for (int i = 0; i < nt; i++) {
            if (!t_ino[i]) continue;
            if (!first) first = t_ino[i];
            else if (t_ino[i] != first) res.inode_divergent++;
            if (t_mode[i] != 0600) res.bad_mode++;
        }

        unlink(g_full);
        close(g_dirfd);
        rmdir(dir);
    }
    pthread_attr_destroy(&attr);
    return res;
}

/* --- reporting --------------------------------------------------------- */

static int integrity_anomalies(const struct result *r) {
    return r->zero_winner + r->missing_after + r->inode_divergent + r->bad_mode;
}

static void print_row(const struct row *r, const struct result *s) {
    char integ[160];
    if (integrity_anomalies(s) == 0) {
        snprintf(integ, sizeof integ, "fail-closed");
    } else {
        snprintf(integ, sizeof integ,
                 "NO-WINNER=%d MISSING-AFTER=%d INODE-DIVERGENT=%d BAD-MODE=%d",
                 s->zero_winner, s->missing_after, s->inode_divergent, s->bad_mode);
    }
    printf("  %-40s %8ld %8ld %8ld %6ld  %s\n",
           r->label, s->ok, s->enoent, s->eexist, s->other, integ);
    if (s->other)
        printf("  %-40s   (last unexpected errno: %d %s)\n",
               "", s->last_errno, strerror(s->last_errno));
}

int main(int argc, char **argv) {
    int nt = 20, trials = 200, keyonly = 0;
    if (argc > 1) nt = atoi(argv[1]);
    if (argc > 2) trials = atoi(argv[2]);
    if (argc > 3) {
        if (!strcmp(argv[3], "key")) keyonly = 1;
        else if (strcmp(argv[3], "all")) {
            fprintf(stderr, "third argument must be \"all\" or \"key\"\n");
            return 1;
        }
    }
    if (nt < 2 || nt > MAX_THREADS) {
        fprintf(stderr, "threads must be between 2 and %d\n", MAX_THREADS);
        return 1;
    }
    if (trials < 1) { fprintf(stderr, "trials must be >= 1\n"); return 1; }

    t_ino  = calloc((size_t)nt, sizeof *t_ino);
    t_mode = calloc((size_t)nt, sizeof *t_mode);
    if (!t_ino || !t_mode) { perror("calloc"); return 1; }

    /* "key" keeps the suspect row and the two controls that make it
     * interpretable, for when a run has to be repeated at a larger size. */
    static const struct row rows[] = {
        { "openat O_RDONLY          file exists", O_RDONLY,                  1, 0, 0, 0, 0 },
        { "openat O_RDWR            file exists", O_RDWR,                    1, 0, 0, 0, 0 },
        { "openat O_RDWR|O_CREAT    file exists", O_RDWR | O_CREAT,          1, 0, 0, 0, 0 },
        { "openat O_RDWR|O_CREAT    file ABSENT", O_RDWR | O_CREAT,          0, 0, 0, 1, 1 },
        { "openat O_RDWR|O_CREAT|O_EXCL  ABSENT", O_RDWR | O_CREAT | O_EXCL, 0, 0, 1, 0, 1 },
        { "open   O_RDWR|O_CREAT    file ABSENT", O_RDWR | O_CREAT,          0, 1, 0, 0, 1 },
    };
    const int nrows = (int)(sizeof rows / sizeof rows[0]);

    printf("openat_race: %d threads x %d trials = %d calls per %srow\n",
           nt, trials, nt * trials, keyonly ? "key " : "");
    printf("temp dirs under: %s\n\n", parent_dir());
    printf("  %-40s %8s %8s %8s %6s  %s\n",
           "row", "ok", "ENOENT", "EEXIST", "other", "integrity");

    long suspect_ok = 0, suspect_enoent = 0, control_enoent = 0, other_total = 0;
    int integ = 0, last_other_errno = 0;
    for (int i = 0; i < nrows; i++) {
        if (keyonly && !rows[i].key) continue;
        struct result s = run_row(&rows[i], nt, trials);
        print_row(&rows[i], &s);
        integ += integrity_anomalies(&s);
        /* An unexpected errno on ANY row makes the whole run uninterpretable,
         * not just a footnote. Exhaust the fd table with `ulimit -n 32` and
         * every row starts returning EMFILE; a verdict that only looked at
         * ENOENT counts called that "every control row clean" and reported
         * REPRODUCES, which is exactly the kind of number that should never
         * reach a bug tracker. */
        other_total += s.other;
        if (s.other) last_other_errno = s.last_errno;
        if (rows[i].suspect) { suspect_ok = s.ok; suspect_enoent = s.enoent; }
        else control_enoent += s.enoent;
    }

    const char *verdict;
    int rc;
    if (integ > 0) {
        verdict = "INTEGRITY";
        rc = 3;
    } else if (control_enoent > 0 || other_total > 0) {
        verdict = "ANOMALOUS";
        rc = 2;
    } else if (suspect_enoent > 0) {
        verdict = "REPRODUCES";
        rc = 0;
    } else {
        verdict = "CLEAN";
        rc = 0;
    }

    printf("\nSUMMARY threads=%d trials=%d calls_per_row=%d suspect_calls=%d suspect_ok=%ld"
           " suspect_enoent=%ld control_enoent=%ld unexpected_errno=%ld"
           " integrity_anomalies=%d verdict=%s\n",
           nt, trials, nt * trials, nt * trials, suspect_ok, suspect_enoent,
           control_enoent, other_total, integ, verdict);
    printf("VERDICT: ");
    switch (rc) {
    case 0:
        if (suspect_enoent)
            printf("REPRODUCES -- %ld of %d openat(O_CREAT) calls returned a spurious"
                   " ENOENT; every control row clean\n", suspect_enoent, nt * trials);
        else
            printf("CLEAN -- no spurious ENOENT at this thread count and trial count;"
                   " every control row clean\n");
        break;
    case 2:
        if (control_enoent > 0)
            printf("ANOMALOUS -- %ld control-row ENOENT", control_enoent);
        else
            printf("ANOMALOUS -- no control-row ENOENT, but %ld call(s) failed with an"
                   " unexpected errno (last: %d %s)", other_total, last_other_errno,
                   strerror(last_other_errno));
        printf(". Either means the harness or the environment is at fault rather than"
               " the kernel -- a resource limit (try `ulimit -n`) will do it."
               " Investigate before reporting anything.\n");
        break;
    case 3:
        printf("INTEGRITY -- the race was not fail-closed on this machine."
               " That is a stronger finding than a forged errno; see the"
               " integrity columns above and report it.\n");
        break;
    }
    free(t_ino);
    free(t_mode);
    return rc;
}
