/*
 * other_syscalls -- is the spurious ENOENT specific to openat(O_CREAT)?
 *
 * openat with O_CREAT is not the only *at syscall that creates a name in a
 * directory, and it is not the only one that can lose a race for that name.
 * This program races N threads on one name through each of them in turn.
 *
 * Correct behaviour differs by row, and the difference matters:
 *
 *   - openat with O_CREAT and no O_EXCL, and renameat, should let EVERY thread
 *     succeed. O_CREAT without O_EXCL means "create it, or open it if someone
 *     beat me to it", and renameat replaces its destination.
 *   - O_EXCL, mkdirat, symlinkat and linkat should produce exactly ONE winner
 *     and EEXIST for everyone else, because each is a create-or-fail.
 *
 * What no row should ever produce is ENOENT: the name did not exist when the
 * call started and the caller is the one creating it, so "no such file or
 * directory" is not a truthful answer for any of them.
 *
 * The point of the table is the contrast. openat O_CREAT|O_EXCL is the same
 * syscall, the same path resolution and the same directory, and it is clean.
 * So is every other name-creating *at call. The one misbehaving row is the
 * only operation in the set with a create-or-open fallback, where a failed
 * create is retried as an open -- which is where to look.
 *
 * Build: cc -O2 -pthread -o other_syscalls other_syscalls.c
 * Usage: ./other_syscalls [threads] [trials]     (default 20 200)
 * Env:   RACE_DIR=<dir>   parent for the temp dirs (default $TMPDIR, else /tmp)
 *
 * The arming barrier below is deliberately a copy of the one in
 * openat_race.c, so that either file can be read, built or pasted on its own.
 *
 * Exit: 0 ISOLATED (only non-EXCL openat misbehaves) or CLEAN (nothing does)
 *       1 usage or setup error
 *       2 ANOMALOUS -- an unexpected errno appeared, so the run says nothing
 *       3 NOT-ISOLATED -- a second syscall misbehaved too, which is a broader
 *         finding than the one being reported
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

enum op { OP_OPENAT, OP_OPENAT_EXCL, OP_MKDIRAT, OP_SYMLINKAT, OP_LINKAT, OP_RENAMEAT };

static int g_nt, g_dirfd;
static enum op g_op;

static int g_ready, g_armed;
static atomic_int g_spinning, g_gate;
static pthread_mutex_t g_mu      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_arrive  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_release = PTHREAD_COND_INITIALIZER;

static atomic_long c_ok, c_enoent, c_eexist, c_other;
static atomic_int  c_last_errno;

static void record(int ok) {
    if (ok) { atomic_fetch_add(&c_ok, 1); return; }
    int e = errno;
    if (e == ENOENT)      atomic_fetch_add(&c_enoent, 1);
    else if (e == EEXIST) atomic_fetch_add(&c_eexist, 1);
    else { atomic_fetch_add(&c_other, 1); atomic_store(&c_last_errno, e); }
}

static void *worker(void *arg) {
    long idx = (long)arg;

    pthread_mutex_lock(&g_mu);
    g_ready++;
    pthread_cond_signal(&g_arrive);
    while (!g_armed)
        pthread_cond_wait(&g_release, &g_mu);
    pthread_mutex_unlock(&g_mu);

    atomic_fetch_add(&g_spinning, 1);
    while (atomic_load(&g_spinning) < g_nt)
        sched_yield();
    while (!atomic_load_explicit(&g_gate, memory_order_acquire))
        spin_hint();

    int fd;
    char src[64];
    switch (g_op) {
    case OP_OPENAT:
        fd = openat(g_dirfd, NAME, O_RDWR | O_CREAT, 0600);
        record(fd >= 0);
        if (fd >= 0) close(fd);
        break;
    case OP_OPENAT_EXCL:
        fd = openat(g_dirfd, NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
        record(fd >= 0);
        if (fd >= 0) close(fd);
        break;
    case OP_MKDIRAT:
        record(mkdirat(g_dirfd, NAME, 0700) == 0);
        break;
    case OP_SYMLINKAT:
        record(symlinkat("a-target", g_dirfd, NAME) == 0);
        break;
    case OP_LINKAT:
        record(linkat(g_dirfd, "src", g_dirfd, NAME, 0) == 0);
        break;
    case OP_RENAMEAT:
        snprintf(src, sizeof src, "s%ld", idx);
        record(renameat(g_dirfd, src, g_dirfd, NAME) == 0);
        break;
    }
    return NULL;
}

static const char *parent_dir(void) {
    const char *p = getenv("RACE_DIR");
    if (!p || !*p) p = getenv("TMPDIR");
    if (!p || !*p) p = "/tmp";
    return p;
}

struct row {
    const char *label;
    enum op op;
    int all_win;           /* renameat replaces, so every thread may succeed */
};

static int last_row_errno;

static long run_row(const struct row *r, int nt, int trials,
                    long *enoent_out, long *other_out,
                    int *multi_winner_out, int *missing_out) {
    long ok = 0, enoent = 0, eexist = 0, other = 0;
    int last_errno = 0, multi_winner = 0, missing = 0;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    for (int t = 0; t < trials; t++) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/oscXXXXXX", parent_dir());
        if (!mkdtemp(dir)) { perror("mkdtemp"); exit(1); }
        g_dirfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (g_dirfd < 0) { perror("open dir"); exit(1); }

        if (r->op == OP_LINKAT) {          /* linkat needs an existing source */
            int fd = openat(g_dirfd, "src", O_RDWR | O_CREAT, 0600);
            if (fd < 0) { perror("linkat source"); exit(1); }
            close(fd);
        }
        if (r->op == OP_RENAMEAT) {        /* one source per thread           */
            for (long i = 0; i < nt; i++) {
                char s[64];
                snprintf(s, sizeof s, "s%ld", i);
                int fd = openat(g_dirfd, s, O_RDWR | O_CREAT, 0600);
                if (fd < 0) { perror("renameat source"); exit(1); }
                close(fd);
            }
        }

        g_nt = nt; g_op = r->op;
        g_ready = 0; g_armed = 0;
        atomic_store(&g_spinning, 0); atomic_store(&g_gate, 0);
        atomic_store(&c_ok, 0); atomic_store(&c_enoent, 0);
        atomic_store(&c_eexist, 0); atomic_store(&c_other, 0);

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
        while (atomic_load(&g_spinning) < nt)
            sched_yield();
        atomic_store_explicit(&g_gate, 1, memory_order_release);
        for (int i = 0; i < nt; i++) {
            int e = pthread_join(th[i], NULL);
            if (e) { fprintf(stderr, "pthread_join: %s\n", strerror(e)); exit(1); }
        }

        long trial_ok = atomic_load(&c_ok);
        ok += trial_ok;
        enoent += atomic_load(&c_enoent);
        eexist += atomic_load(&c_eexist);
        long o = atomic_load(&c_other);
        other += o;
        if (o) last_errno = atomic_load(&c_last_errno);
        if (!r->all_win && trial_ok > 1) multi_winner++;

        struct stat st;
        if (fstatat(g_dirfd, NAME, &st, AT_SYMLINK_NOFOLLOW) != 0) missing++;

        char p[PATH_MAX + 64];   /* headroom so composing cannot truncate */
        snprintf(p, sizeof p, "%s/%s", dir, NAME);
        if (r->op == OP_MKDIRAT) rmdir(p); else unlink(p);
        for (long i = 0; i < nt; i++) {
            char s[PATH_MAX + 64];
            snprintf(s, sizeof s, "%s/s%ld", dir, i);
            unlink(s);
        }
        snprintf(p, sizeof p, "%s/src", dir);
        unlink(p);
        close(g_dirfd);
        rmdir(dir);
    }
    pthread_attr_destroy(&attr);

    printf("  %-32s %8ld %8ld %8ld %6ld  multi-winner=%d missing-after=%d\n",
           r->label, ok, enoent, eexist, other, multi_winner, missing);
    if (other)
        printf("  %-32s   (last unexpected errno: %d %s)\n",
               "", last_errno, strerror(last_errno));
    *enoent_out = enoent;
    *other_out = other;
    *multi_winner_out = multi_winner;
    *missing_out = missing;
    last_row_errno = last_errno;
    return ok;
}

int main(int argc, char **argv) {
    int nt = 20, trials = 200;
    if (argc > 1) nt = atoi(argv[1]);
    if (argc > 2) trials = atoi(argv[2]);
    if (nt < 2 || nt > MAX_THREADS) {
        fprintf(stderr, "threads must be between 2 and %d\n", MAX_THREADS);
        return 1;
    }
    if (trials < 1) { fprintf(stderr, "trials must be >= 1\n"); return 1; }

    static const struct row rows[] = {
        { "openat O_CREAT (no O_EXCL)",   OP_OPENAT,      1 },
        { "openat O_CREAT|O_EXCL",        OP_OPENAT_EXCL, 0 },
        { "mkdirat",                      OP_MKDIRAT,     0 },
        { "symlinkat",                    OP_SYMLINKAT,   0 },
        { "linkat",                       OP_LINKAT,      0 },
        { "renameat (replaces, all win)", OP_RENAMEAT,    1 },
    };
    const int nrows = (int)(sizeof rows / sizeof rows[0]);

    printf("other_syscalls: %d threads x %d trials racing ONE name = %d calls per row\n",
           nt, trials, nt * trials);
    printf("temp dirs under: %s\n", parent_dir());
    printf("correct: all threads win for non-EXCL openat and renameat, exactly one\n");
    printf("wins for the create-or-fail rows -- and zero ENOENT for every row.\n\n");
    printf("  %-32s %8s %8s %8s %6s  %s\n",
           "syscall racing one name", "ok", "ENOENT", "EEXIST", "other", "integrity");

    char dirty[512] = "";
    long openat_enoent = 0, elsewhere_enoent = 0, other_total = 0;
    int structural = 0, last_other_errno = 0;
    for (int i = 0; i < nrows; i++) {
        long enoent, other; int multi, missing;
        run_row(&rows[i], nt, trials, &enoent, &other, &multi, &missing);
        other_total += other;
        if (other) last_other_errno = last_row_errno;
        structural += multi + missing;
        if (rows[i].op == OP_OPENAT) {
            openat_enoent += enoent;
        } else if (enoent > 0) {
            elsewhere_enoent += enoent;
            strncat(dirty, dirty[0] ? ", " : "", sizeof dirty - strlen(dirty) - 1);
            strncat(dirty, rows[i].label, sizeof dirty - strlen(dirty) - 1);
        }
    }

    /* The isolation claim is the point of this program, so it is enforced here
     * rather than left for a reader to eyeball. A second syscall misbehaving
     * would be a broader and more serious finding than the one being reported,
     * and must not exit 0 alongside it. */
    const char *verdict;
    int rc;
    if (other_total > 0)          { verdict = "ANOMALOUS";    rc = 2; }
    else if (elsewhere_enoent || structural) { verdict = "NOT-ISOLATED"; rc = 3; }
    else if (openat_enoent > 0)   { verdict = "ISOLATED";     rc = 0; }
    else                          { verdict = "CLEAN";        rc = 0; }

    printf("\nSUMMARY threads=%d trials=%d calls_per_row=%d openat_enoent=%ld"
           " other_syscall_enoent=%ld structural_anomalies=%d unexpected_errno=%ld"
           " verdict=%s\n",
           nt, trials, nt * trials, openat_enoent, elsewhere_enoent, structural,
           other_total, verdict);
    printf("VERDICT: ");
    switch (rc) {
    case 0:
        if (openat_enoent > 0)
            printf("ISOLATED -- %ld spurious ENOENT, all of it from openat O_CREAT"
                   " without O_EXCL; every other name-creating syscall clean\n",
                   openat_enoent);
        else
            printf("CLEAN -- no spurious ENOENT from any of these syscalls at this size\n");
        break;
    case 2:
        printf("ANOMALOUS -- %ld call(s) failed with an unexpected errno (last: %d %s),"
               " so this run says nothing about isolation. Check `ulimit -n`.\n",
               other_total, last_other_errno, strerror(last_other_errno));
        break;
    case 3:
        if (elsewhere_enoent)
            printf("NOT ISOLATED -- spurious ENOENT also from: %s. That is broader than"
                   " the reported bug; re-run and report it.\n", dirty);
        else
            printf("NOT ISOLATED -- %d structural anomal%s (a second winner, or the name"
                   " missing afterwards). Re-run and report it.\n",
                   structural, structural == 1 ? "y" : "ies");
        break;
    }
    return rc;
}
