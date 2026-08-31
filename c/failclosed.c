/*
 * failclosed -- is the openat(O_CREAT) race fail-closed, including in the two
 * directory shapes an attacker would pick?
 *
 * "The kernel says a file does not exist when it does" invites the assumption
 * that something worse is reachable. This program looks for it. A forged errno
 * is a bug; a wrong-but-successful open would be a vulnerability, and they need
 * different reports to different places.
 *
 * Three directory shapes, N threads racing openat(dirfd, "n", O_RDWR|O_CREAT):
 *
 *   1. a plain private directory -- the baseline.
 *   2. the raced name is already a DANGLING SYMLINK pointing OUT of the
 *      directory. This is the classic /tmp symlink shape: O_CREAT without
 *      O_EXCL must follow the link and create the target, and must never
 *      replace the link itself. If the race could clobber the symlink into a
 *      regular file, or hand any opener a descriptor to something other than
 *      the link target, that would be the real vulnerability.
 *   3. the directory is world-writable and sticky (01777), like /tmp.
 *
 * Every shape is checked for: a trial where nobody won, the name missing
 * afterwards, openers disagreeing about the inode, and a mode other than the
 * one requested. Shape 2 additionally checks that the symlink survived as a
 * symlink, that its target was the thing created, and that no opener saw a
 * non-target inode.
 *
 * Build: cc -O2 -pthread -std=gnu11 -o failclosed c/failclosed.c
 * Usage: ./failclosed [threads] [trials]        (default 20 200)
 * Env:   RACE_DIR=<dir>   parent for the temp dirs (default $TMPDIR, else /tmp)
 *
 * The arming barrier below is deliberately a copy of the one in
 * openat_race.c, so that either file can be read, built or pasted on its own.
 *
 * Exit: 0 fail-closed everywhere (the race forged an errno and nothing else)
 *       1 usage or setup error
 *       2 ANOMALOUS -- an unexpected errno appeared, so the run says nothing
 *       3 NOT fail-closed. Report this; it is a stronger finding than a forged
 *         errno and it changes where the report should go.
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

static int g_nt, g_dirfd;

static int g_ready, g_armed;
static atomic_int g_spinning, g_gate;
static pthread_mutex_t g_mu      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_arrive  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_release = PTHREAD_COND_INITIALIZER;

static atomic_long c_ok, c_enoent, c_other;
static atomic_int  c_last_errno;

static ino_t  *t_ino;
static mode_t *t_mode;

static void *worker(void *arg) {
    long idx = (long)arg;
    t_ino[idx] = 0;
    t_mode[idx] = 0;

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

    int fd = openat(g_dirfd, NAME, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        int e = errno;
        if (e == ENOENT) atomic_fetch_add(&c_enoent, 1);
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

enum shape { SHAPE_PLAIN, SHAPE_DANGLING_SYMLINK, SHAPE_STICKY };

struct result {
    long ok, enoent, other;
    int  last_errno;
    int  zero_winner;
    int  missing_after;
    int  inode_divergent;
    int  bad_mode;
    /* dangling-symlink shape only */
    int  link_clobbered;
    int  target_missing;
    int  wrong_object;
};

static const char *parent_dir(void) {
    const char *p = getenv("RACE_DIR");
    if (!p || !*p) p = getenv("TMPDIR");
    if (!p || !*p) p = "/tmp";
    return p;
}

static struct result run_shape(enum shape sh, int nt, int trials) {
    struct result r;
    memset(&r, 0, sizeof r);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    for (int t = 0; t < trials; t++) {
        char dir[PATH_MAX], out[PATH_MAX], target[PATH_MAX + 64];
        snprintf(dir, sizeof dir, "%s/fcXXXXXX", parent_dir());
        if (!mkdtemp(dir)) { perror("mkdtemp"); exit(1); }

        out[0] = '\0';
        target[0] = '\0';
        if (sh == SHAPE_STICKY && chmod(dir, 01777) != 0) { perror("chmod 01777"); exit(1); }

        g_dirfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (g_dirfd < 0) { perror("open dir"); exit(1); }

        if (sh == SHAPE_DANGLING_SYMLINK) {
            snprintf(out, sizeof out, "%s/fcoutXXXXXX", parent_dir());
            if (!mkdtemp(out)) { perror("mkdtemp out"); exit(1); }
            snprintf(target, sizeof target, "%s/victim", out);
            /* dangling on purpose: victim does not exist yet */
            if (symlinkat(target, g_dirfd, NAME) != 0) { perror("symlinkat"); exit(1); }
        }

        g_nt = nt;
        g_ready = 0; g_armed = 0;
        atomic_store(&g_spinning, 0); atomic_store(&g_gate, 0);
        atomic_store(&c_ok, 0); atomic_store(&c_enoent, 0); atomic_store(&c_other, 0);

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

        long ok = atomic_load(&c_ok);
        r.ok += ok;
        r.enoent += atomic_load(&c_enoent);
        long o = atomic_load(&c_other);
        r.other += o;
        if (o) r.last_errno = atomic_load(&c_last_errno);
        if (ok == 0) r.zero_winner++;

        struct stat st;
        if (sh == SHAPE_DANGLING_SYMLINK) {
            /* the link must still be a link: O_CREAT follows it, never replaces it */
            if (fstatat(g_dirfd, NAME, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISLNK(st.st_mode))
                r.link_clobbered++;
            struct stat ts;
            if (stat(target, &ts) != 0) {
                r.target_missing++;
            } else {
                for (int i = 0; i < nt; i++)
                    if (t_ino[i] && t_ino[i] != ts.st_ino) r.wrong_object++;
            }
        } else {
            if (fstatat(g_dirfd, NAME, &st, 0) != 0) r.missing_after++;
        }

        ino_t first = 0;
        for (int i = 0; i < nt; i++) {
            if (!t_ino[i]) continue;
            if (!first) first = t_ino[i];
            else if (t_ino[i] != first) r.inode_divergent++;
            if (t_mode[i] != 0600) r.bad_mode++;
        }

        char p[PATH_MAX + 64];
        snprintf(p, sizeof p, "%s/%s", dir, NAME);
        unlink(p);
        close(g_dirfd);
        rmdir(dir);
        if (target[0]) unlink(target);
        if (out[0]) rmdir(out);
    }
    pthread_attr_destroy(&attr);
    return r;
}

static int anomalies(enum shape sh, const struct result *r) {
    int n = r->zero_winner + r->inode_divergent + r->bad_mode;
    if (sh == SHAPE_DANGLING_SYMLINK)
        n += r->link_clobbered + r->target_missing + r->wrong_object;
    else
        n += r->missing_after;
    return n;
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

    t_ino  = calloc((size_t)nt, sizeof *t_ino);
    t_mode = calloc((size_t)nt, sizeof *t_mode);
    if (!t_ino || !t_mode) { perror("calloc"); return 1; }

    static const struct { const char *label; enum shape sh; } shapes[] = {
        { "plain private directory",           SHAPE_PLAIN },
        { "raced name is a DANGLING SYMLINK",  SHAPE_DANGLING_SYMLINK },
        { "world-writable STICKY dir (01777)", SHAPE_STICKY },
    };
    const int n = (int)(sizeof shapes / sizeof shapes[0]);

    printf("failclosed: %d threads x %d trials = %d calls per shape\n",
           nt, trials, nt * trials);
    printf("temp dirs under: %s\n\n", parent_dir());

    int total_anom = 0;
    long total_other = 0;
    int last_other_errno = 0;
    for (int i = 0; i < n; i++) {
        struct result r = run_shape(shapes[i].sh, nt, trials);
        printf("  %-34s ok=%6ld spurious-ENOENT=%6ld other=%ld\n",
               shapes[i].label, r.ok, r.enoent, r.other);
        printf("      no-winner trials=%d  inode-divergent=%d  wrong-mode=%d",
               r.zero_winner, r.inode_divergent, r.bad_mode);
        if (shapes[i].sh == SHAPE_DANGLING_SYMLINK)
            printf("  symlink-clobbered=%d  target-not-created=%d  opener-saw-wrong-object=%d\n",
                   r.link_clobbered, r.target_missing, r.wrong_object);
        else
            printf("  name-missing-after=%d\n", r.missing_after);
        total_anom += anomalies(shapes[i].sh, &r);
        total_other += r.other;
        if (r.other) last_other_errno = r.last_errno;
    }

    const char *verdict;
    int rc;
    if (total_other > 0) { verdict = "ANOMALOUS"; rc = 2; }
    else if (total_anom > 0) { verdict = "NOT-FAIL-CLOSED"; rc = 3; }
    else { verdict = "FAIL-CLOSED"; rc = 0; }

    printf("\nSUMMARY threads=%d trials=%d calls_per_shape=%d integrity_anomalies=%d"
           " unexpected_errno=%ld verdict=%s\n",
           nt, trials, nt * trials, total_anom, total_other, verdict);
    printf("VERDICT: ");
    switch (rc) {
    case 0:
        printf("FAIL-CLOSED -- in all three shapes the race forged an errno and did"
               " nothing else: some thread always won, the object always ended up"
               " existing, no opener saw a different inode or an unrequested mode,"
               " and the symlink was never clobbered.\n");
        break;
    case 2:
        printf("ANOMALOUS -- %ld call(s) failed with an unexpected errno (last: %d %s),"
               " so this run says nothing about fail-closedness. A resource limit"
               " will do this; check `ulimit -n`.\n",
               total_other, last_other_errno, strerror(last_other_errno));
        break;
    case 3:
        printf("NOT FAIL-CLOSED -- %d integrity anomal%s above. This is a stronger and"
               " different finding than a forged errno, and it belongs in a security"
               " report rather than a public tracker. Re-run to confirm before acting.\n",
               total_anom, total_anom == 1 ? "y" : "ies");
        break;
    }
    free(t_ino);
    free(t_mode);
    return rc;
}
