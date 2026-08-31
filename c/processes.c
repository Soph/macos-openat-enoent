/*
 * processes -- does the race survive across separate processes?
 *
 * Every other probe here races threads inside one process, which invites the
 * obvious dismissal: that this is a pthreads or a Go-scheduler artifact rather
 * than a kernel one. It also leaves out the case that matters most in practice.
 * Concurrent creation of a single name is usually not several threads in one
 * program; it is several *processes* reaching for the same lock file, log file
 * or queue file at once -- git hooks, CI steps, an editor and a build running
 * together.
 *
 * So this one forks. N children are released simultaneously into the same
 * openat() call on the same not-yet-existing name, and they share nothing but
 * an mmap'd counter block and the directory descriptor they inherited.
 *
 * Build: cc -O2 -pthread -std=gnu11 -o processes c/processes.c
 * Usage: ./processes [children] [trials]        (default 20 200)
 * Env:   RACE_DIR=<dir>   parent for the temp dirs (default $TMPDIR, else /tmp)
 *
 * Exit: 0 interpretable (REPRODUCES or CLEAN)
 *       1 usage or setup error
 *       2 ANOMALOUS -- a control row failed too, or an unexpected errno
 *         appeared, so suspect the harness or the environment
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_CHILDREN 512
#define NAME         "n"

static inline void spin_hint(void) {
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* Shared across the fork, so the children can be released together and can
 * report without a pipe per child. MAP_SHARED|MAP_ANON survives fork. */
struct shared {
    atomic_int  ready;
    atomic_int  gate;
    atomic_long ok, enoent, eexist, other;
    atomic_int  last_errno;
};

static const char *parent_dir(void) {
    const char *p = getenv("RACE_DIR");
    if (!p || !*p) p = getenv("TMPDIR");
    if (!p || !*p) p = "/tmp";
    return p;
}

struct row {
    const char *label;
    int flags;
    int fullpath;      /* open(2) on an assembled path instead of openat */
    int suspect;
};

struct result {
    long ok, enoent, eexist, other;
    int  last_errno;
    int  zero_winner;
    int  missing_after;
};

static struct result run_row(const struct row *r, int nkids, int trials) {
    struct result res;
    memset(&res, 0, sizeof res);

    struct shared *sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANON, -1, 0);
    if (sh == MAP_FAILED) { perror("mmap"); exit(1); }

    for (int t = 0; t < trials; t++) {
        char dir[PATH_MAX], full[PATH_MAX + 64];
        snprintf(dir, sizeof dir, "%s/prcXXXXXX", parent_dir());
        if (!mkdtemp(dir)) { perror("mkdtemp"); exit(1); }
        snprintf(full, sizeof full, "%s/%s", dir, NAME);

        int dirfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (dirfd < 0) { perror("open dir"); exit(1); }

        atomic_store(&sh->ready, 0);
        atomic_store(&sh->gate, 0);
        atomic_store(&sh->ok, 0);
        atomic_store(&sh->enoent, 0);
        atomic_store(&sh->eexist, 0);
        atomic_store(&sh->other, 0);

        pid_t kids[MAX_CHILDREN];
        int spawned = 0;
        for (int i = 0; i < nkids; i++) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); exit(1); }
            if (pid == 0) {
                /* --- child --- */
                atomic_fetch_add(&sh->ready, 1);
                /* Cooperative until everyone has arrived, then a hot spin, so
                 * that children still being forked are not starved by children
                 * already spinning. */
                while (atomic_load(&sh->ready) < nkids)
                    sched_yield();
                while (!atomic_load_explicit(&sh->gate, memory_order_acquire))
                    spin_hint();

                int fd = r->fullpath ? open(full, r->flags, 0600)
                                     : openat(dirfd, NAME, r->flags, 0600);
                if (fd < 0) {
                    int e = errno;
                    if (e == ENOENT)      atomic_fetch_add(&sh->enoent, 1);
                    else if (e == EEXIST) atomic_fetch_add(&sh->eexist, 1);
                    else { atomic_fetch_add(&sh->other, 1); atomic_store(&sh->last_errno, e); }
                } else {
                    close(fd);
                    atomic_fetch_add(&sh->ok, 1);
                }
                _exit(0);   /* _exit: no atexit handlers, no flushing the parent's buffers */
            }
            kids[spawned++] = pid;
        }

        while (atomic_load(&sh->ready) < nkids)
            sched_yield();
        atomic_store_explicit(&sh->gate, 1, memory_order_release);

        for (int i = 0; i < spawned; i++) {
            int status = 0;
            if (waitpid(kids[i], &status, 0) < 0) { perror("waitpid"); exit(1); }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fprintf(stderr, "child %d did not exit cleanly (status %d)\n", i, status);
                exit(1);
            }
        }

        long ok = atomic_load(&sh->ok);
        res.ok += ok;
        res.enoent += atomic_load(&sh->enoent);
        res.eexist += atomic_load(&sh->eexist);
        long o = atomic_load(&sh->other);
        res.other += o;
        if (o) res.last_errno = atomic_load(&sh->last_errno);
        if (ok == 0) res.zero_winner++;

        struct stat st;
        if (fstatat(dirfd, NAME, &st, 0) != 0) res.missing_after++;

        unlink(full);
        close(dirfd);
        rmdir(dir);
    }
    if (munmap(sh, sizeof *sh) != 0) { perror("munmap"); exit(1); }
    return res;
}

int main(int argc, char **argv) {
    int nkids = 20, trials = 200;
    if (argc > 1) nkids = atoi(argv[1]);
    if (argc > 2) trials = atoi(argv[2]);
    if (nkids < 2 || nkids > MAX_CHILDREN) {
        fprintf(stderr, "children must be between 2 and %d\n", MAX_CHILDREN);
        return 1;
    }
    if (trials < 1) { fprintf(stderr, "trials must be >= 1\n"); return 1; }

    static const struct row rows[] = {
        { "openat O_RDWR|O_CREAT    file ABSENT", O_RDWR | O_CREAT,          0, 1 },
        { "openat O_RDWR|O_CREAT|O_EXCL  ABSENT", O_RDWR | O_CREAT | O_EXCL, 0, 0 },
        { "open   O_RDWR|O_CREAT    file ABSENT", O_RDWR | O_CREAT,          1, 0 },
    };
    const int nrows = (int)(sizeof rows / sizeof rows[0]);

    printf("processes: %d forked children x %d trials = %d calls per row\n",
           nkids, trials, nkids * trials);
    printf("temp dirs under: %s\n\n", parent_dir());
    printf("  %-40s %8s %8s %8s %6s  %s\n",
           "row", "ok", "ENOENT", "EEXIST", "other", "integrity");

    long suspect_enoent = 0, control_enoent = 0, other_total = 0;
    int last_other_errno = 0, structural = 0;
    for (int i = 0; i < nrows; i++) {
        struct result s = run_row(&rows[i], nkids, trials);
        printf("  %-40s %8ld %8ld %8ld %6ld  no-winner=%d missing-after=%d\n",
               rows[i].label, s.ok, s.enoent, s.eexist, s.other,
               s.zero_winner, s.missing_after);
        other_total += s.other;
        if (s.other) last_other_errno = s.last_errno;
        structural += s.zero_winner + s.missing_after;
        if (rows[i].suspect) suspect_enoent = s.enoent;
        else control_enoent += s.enoent;
    }

    const char *verdict;
    int rc;
    if (control_enoent > 0 || other_total > 0 || structural > 0) { verdict = "ANOMALOUS"; rc = 2; }
    else if (suspect_enoent > 0) { verdict = "REPRODUCES"; rc = 0; }
    else { verdict = "CLEAN"; rc = 0; }

    printf("\nSUMMARY children=%d trials=%d calls_per_row=%d suspect_calls=%d"
           " suspect_enoent=%ld control_enoent=%ld unexpected_errno=%ld"
           " structural_anomalies=%d verdict=%s\n",
           nkids, trials, nkids * trials, nkids * trials, suspect_enoent,
           control_enoent, other_total, structural, verdict);
    printf("VERDICT: ");
    switch (rc) {
    case 0:
        if (suspect_enoent)
            printf("REPRODUCES ACROSS PROCESSES -- %ld of %d openat(O_CREAT) calls from"
                   " separate processes returned a spurious ENOENT; both controls clean."
                   " So this is not a pthreads artifact.\n", suspect_enoent, nkids * trials);
        else
            printf("CLEAN -- no spurious ENOENT across processes at this size;"
                   " both controls clean\n");
        break;
    case 2:
        printf("ANOMALOUS -- control ENOENT=%ld, unexpected errno=%ld (last: %d %s),"
               " structural anomalies=%d. Suspect the harness or the environment"
               " rather than the kernel.\n",
               control_enoent, other_total, last_other_errno,
               strerror(last_other_errno), structural);
        break;
    }
    return rc;
}
