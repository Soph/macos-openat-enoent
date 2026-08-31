# Spurious `ENOENT` from `openat(O_CREAT)` on macOS

On macOS, when several threads race to `openat(dirfd, name, O_CREAT)` the same
not-yet-existing name in the same directory, many of them get `ENOENT` — most of
them on a machine with enough cores to run them genuinely concurrently, a few
percent on a 3-core VM. One thread creates the file correctly; the others are
told the file does not exist — the file they themselves asked to have created,
and which by then does exist.

`O_CREAT` without `O_EXCL` means *create it, or open it if someone beat me to
it*. Every caller should get a descriptor. That is what `open(2)` on the
assembled full path does on the same machine, on the same filesystem, in an
identically created directory: 4000 out of 4000 calls succeed. Only the `openat`
form fails. (Each row gets its own fresh temporary directory, so the controls
share the filesystem and the creation path, not one directory inode.)

This repository is a reproduction, not a fix. It exists to be pasted into a bug
report.

```
$ ./probe.sh
  row                                            ok   ENOENT   EEXIST  other  integrity
  openat O_RDONLY          file exists         4000        0        0      0  fail-closed
  openat O_RDWR            file exists         4000        0        0      0  fail-closed
  openat O_RDWR|O_CREAT    file exists         4000        0        0      0  fail-closed
  openat O_RDWR|O_CREAT    file ABSENT         1099     2901        0      0  fail-closed
  openat O_RDWR|O_CREAT|O_EXCL  ABSENT          200        0     3800      0  fail-closed
  open   O_RDWR|O_CREAT    file ABSENT         4000        0        0      0  fail-closed

VERDICT: REPRODUCES -- 2901 of 4000 openat(O_CREAT) calls returned a spurious
         ENOENT; every control row clean
```

## Does the documentation say this should work?

Yes, and the `ENOENT` is not a permitted return value.

Apple's own `open(2)` man page covers `openat()` — "The `oflag` argument and the
optional fourth argument correspond exactly to the arguments for `open()`" — and
enumerates 26 distinct error conditions. Exactly two of them are `ENOENT`:

```
[ENOENT]  O_CREAT is not set and the named file does not exist.
[ENOENT]  A component of the path name that must exist does not exist.
```

Neither applies. `O_CREAT` *is* set, which disposes of the first. And in
`openat(dirfd, "n", O_RDWR|O_CREAT, 0600)` the only component is `n` itself —
the file being created, which is precisely the component that is *not* required
to exist. Its containing directory is pinned by an open descriptor for the
duration of the call, so nothing that "must exist" is missing. There is no third
`ENOENT` clause to fall back on.

POSIX is narrower still. It permits `ENOENT` when `O_CREAT` is set only where "a
component of the **path prefix** of `path` does not name an existing file" — a
missing *final* component with `O_CREAT` set is explicitly excluded.

`openat()` itself has been in POSIX since Issue 7 (POSIX.1-2008); the text above
is from the current edition.

Apple does formally register macOS against a POSIX product standard — macOS 26.0
Tahoe is registered to UNIX 03 on
[Apple silicon](https://www.opengroup.org/openbrand/register/brand3725.htm) and
on [Intel](https://www.opengroup.org/openbrand/register/brand3720.htm), both
dated 29-Aug-2025, the same two architectures this reproduces on. Take that as
background rather than as proof, though: UNIX 03 maps to Issue 6, which predates
`openat()`'s standardisation, so the certification does not itself cover this
call. The load-bearing document here is Apple's own man page, which does.

**The counter-argument, stated fairly.** POSIX's explicit *atomicity* guarantee
is scoped to `O_CREAT|O_EXCL`: "The check for the existence of the file and the
creation of the file if it does not exist shall be atomic with respect to other
threads executing `open()` naming the same filename in the same directory **with
`O_EXCL` and `O_CREAT` set**." So one can argue POSIX never promised that
`O_CREAT` *without* `O_EXCL` is atomic, and that is correct.

It does not rescue the behaviour, because the complaint is not about atomicity.
Even a deliberately non-atomic implementation must either succeed or fail with
one of the enumerated errors, and `ENOENT` is not one of them for this case in
either Apple's documentation or POSIX's. The defect is the errno, not the
interleaving.

## Has this been reported before?

Not upstream. There is no golang/go issue for it (all 40 `os.Root` issues, open
and closed, checked 2026-08-31; the closest is
[#75114](https://github.com/golang/go/issues/75114), a different operation), and
no public Apple report — though Feedback Assistant is not publicly searchable, so
that shows only that nothing public exists.

It has been hit and fixed downstream at least once, independently:
[spiceai/spiceai#13232](https://github.com/spiceai/spiceai/issues/13232),
"Concurrent lock creation fails with a spurious ENOENT on macOS", found from
flaky lock-file tests, diagnosed to the same syscall on Darwin 25.6.0 with the
same controls (absolute path clean, `O_EXCL` clean), and worked around in
[#13231](https://github.com/spiceai/spiceai/pull/13231) with a bounded retry.
Their measurement is worth quoting for anyone weighing a retry: **one further
look always sufficed — 0 failures in 14,400 attempts across 8 threads, 32
threads and 8 processes, deepest retry depth 1.**

That independent confirmation, in a different language and a different codebase,
is stronger evidence than this repository alone. It also means the practical
answer is already established: callers can avoid this today, and some already do.

## Running it

```sh
./probe.sh          # builds and runs everything, prints a machine header
```

Needs a C compiler. The Go probe is skipped if there is no `go` on `PATH`, or
with `SKIP_GO=1`. `THREADS` and `TRIALS` set the size; `RACE_DIR` moves the
temporary directories onto another filesystem.

`probe.sh` exits `0` when the run is interpretable — whether it reproduced or
came out clean — and non-zero when it is not: `2` if a control row failed as
well (suspect the harness or the machine, not the kernel), `3` if the race
turned out not to be fail-closed, `1` on a build failure.

The probes can also be run individually:

```sh
cc -O2 -pthread -std=gnu11 -o openat_race    c/openat_race.c    && ./openat_race 20 200
cc -O2 -pthread -std=gnu11 -o other_syscalls c/other_syscalls.c && ./other_syscalls 20 200
cc -O2 -pthread -std=gnu11 -o failclosed     c/failclosed.c     && ./failclosed 20 200
cc -O2 -pthread -std=gnu11 -o processes      c/processes.c      && ./processes 20 100
cd go && go run . -threads 20 -trials 50
```

### Reading a clean result

A clean run is weaker evidence than a reproducing one. The rate depends on how
much true parallelism the machine has, so a 3-core CI runner can score zero at a
size where a laptop scores 70%. `probe.sh` therefore climbs a ladder — 20×200,
then 20×1000, then 40×1000 — and stops at the first size that reproduces. The
`size` column of the results table says which stage answered, and a `stage 1`
clean row means only that the smallest size found nothing.

Stages 2 and 3 run the key rows only — the suspect row plus the `O_EXCL` and
full-path `open(2)` controls. The two existing-file controls are checked at
stage 1, so a result reported from stage 2 or 3 has rechecked three of the five
controls, not all five.

A run is also only interpretable if the controls are clean. Every probe carries
its own controls and enforces them in its exit code: a control returning
`ENOENT`, **or any call failing with an unexpected errno**, is reported as
`ANOMALOUS`. That second half matters — squeeze the descriptor table with
`ulimit -n 16` and every row starts returning `EMFILE`, and an earlier version
of this probe called that "every control row clean" and printed `REPRODUCES`.
Do not quote a number from an `ANOMALOUS` run.

## What was measured

### Laptops

Both Apple silicon, 20 threads × 200 trials = 4000 calls:

| machine | macOS | build | cores | spurious `ENOENT` | controls |
| --- | --- | --- | --: | --: | --- |
| M4 Max | 26.6 | 25G72 | 16 | 2638–3147 / 4000 (66–79%) | clean |
| M5 | 26.6.2 | 25G83 | — | 1544 / 4000 (39%) | clean |

The rate swings a lot with scheduling — 966 to 3034 on one machine across runs —
so the absolute number means little. The controls are what make a run
interpretable, and they were clean on both.

### GitHub-hosted runners

Every macOS image GitHub currently offers, plus Linux on both architectures,
from [this run](https://github.com/Soph/macos-openat-enoent/actions/runs/33397596124)
of `.github/workflows/matrix.yml` on 2026-08-31, at commit
[`b884587`](https://github.com/Soph/macos-openat-enoent/commit/b8845879bd573105790f24a2c2352baca7344a73).
Per-runner logs and the raw records are attached to that run as `result-*`
artifacts:

| runner | OS | build | arch | cores | CPU | size | spurious `ENOENT` | verdict | isolation | fail-closed | Go 1.27.0 |
| --- | --- | --- | --- | --: | --- | --- | --: | --- | --- | --- | --- |
| `macos-14` | macOS 14.8.7 | 23J520 | arm64 | 3 | Apple M1 (Virtual) | 20×200 | 358 / 4000 (9%) | **REPRODUCES** | ISOLATED | FAIL-CLOSED | 48 / 2000 |
| `macos-15` | macOS 15.7.7 | 24G720 | arm64 | 3 | Apple M1 (Virtual) | 20×200 | 239 / 4000 (6%) | **REPRODUCES** | ISOLATED | FAIL-CLOSED | 9 / 2000 |
| `macos-26` | macOS 26.5.2 | 25F84 | arm64 | 3 | Apple M1 (Virtual) | 20×200 | 354 / 4000 (9%) | **REPRODUCES** | ISOLATED | FAIL-CLOSED | 38 / 2000 |
| `macos-15-intel` | macOS 15.7.9 | 24G830 | x86_64 | 4 | Core i7-8700B | 20×200 | 227 / 4000 (6%) | **REPRODUCES** | ISOLATED | FAIL-CLOSED | 11 / 2000 |
| `macos-26-intel` | macOS 26.6.1 | 25G76 | x86_64 | 4 | Core i7-8700B | 20×200 | 284 / 4000 (7%) | **REPRODUCES** | ISOLATED | FAIL-CLOSED | 26 / 2000 |
| `ubuntu-24.04` | Ubuntu 24.04.4, 6.17.0 | — | x86_64 | 4 | AMD EPYC 7763 | 40×1000 | 0 / 40000 | clean | CLEAN | FAIL-CLOSED | 0 / 2000 |
| `ubuntu-24.04-arm` | Ubuntu 24.04.4, 6.17.0 | — | arm64 | 4 | — | 40×1000 | 0 / 40000 | clean | CLEAN | FAIL-CLOSED | 0 / 2000 |

Every control row was clean, every row was fail-closed, and every run put all of
its spurious `ENOENT` in the one `openat` row, on all seven runners.

Four things follow.

**It is not architecture-specific.** Both `-intel` legs are real Intel hardware —
a Core i7-8700B, not a virtualised M1 — and both reproduce, at the same rate as
the arm64 legs of the same macOS generation. arm64's weaker memory model is
therefore not needed to explain this, which makes a missing barrier the less
likely story and an architecture-independent defect in the create-or-open
fallback the more likely one.

Across three runs of the matrix, the arm64 legs ranged 5.6–9.6% and the Intel
legs 5.5–7.1%. Those bands overlap, and the run-to-run spread on a single leg
(`macos-15` went 6%, 8%, 6%) is as large as the difference between the
architectures — so the honest claim is that Intel reproduces at the same order
of magnitude, not that it matches rate for rate. The pairs are also adjacent
patch builds rather than identical ones, because GitHub does not offer one build
on both architectures. All of which supports "arm64 is not required" rather than
a build-for-build comparison.

**It is not a macOS 26 regression.** It reproduces on Sonoma 14.8.7, on Sequoia
15.7.7 and 15.7.9, and on Tahoe 26.5.2, 26.6.1, 26.6 and 26.6.2 — every macOS
version testable here. macOS 13 and earlier cannot be tested on GitHub-hosted
runners any more, so how much further back it goes is still open.

**Linux did not reproduce it at ten times the volume.** Both Linux legs climbed
the ladder to 40 threads × 1000 trials and returned 0 spurious `ENOENT` out of
40000 calls, on both architectures, running the same source. That is the control
that rules out the harness. It is absence of observation at that size, not proof
of immunity.

**The rate tracks available parallelism, not severity.** The runners score 6–9%
on 3–4 cores where a 16-core laptop scores 66–79%. A low number on a small
machine is not a mild version of the bug; it is the same bug with fewer chances
to land.

### It is not a threading artifact

`c/processes.c` forks instead of spawning threads: N children, released
together, sharing nothing but an `mmap`'d counter block and an inherited
directory descriptor. Measured on macOS 26.6 (25G72), 20 children × 100 trials:

```
  row                                            ok   ENOENT   EEXIST  other
  openat O_RDWR|O_CREAT    file ABSENT          464     1536        0      0
  openat O_RDWR|O_CREAT|O_EXCL  ABSENT          100        0     1900      0
  open   O_RDWR|O_CREAT    file ABSENT         2000        0        0      0

VERDICT: REPRODUCES ACROSS PROCESSES -- 1536 of 2000 ... both controls clean
```

77%, which is the same order as the threaded rate on the same machine. So this is
not a pthreads or a Go-scheduler artifact — and it is the case that matters in
practice, because concurrent creation of one name is usually several *processes*
reaching for the same lock, log or queue file, not several threads in one program.

### It is one operation, not path resolution

`openat` with `O_CREAT` is not the only `*at` syscall that creates a name in a
directory, and not the only one that can lose a race for that name. Racing 20
threads on one name through each of them, correct behaviour depends on the row:
non-`O_EXCL` `openat` and `renameat` should let *every* thread through, while the
create-or-fail primitives should produce exactly one winner and `EEXIST` for the
rest. What no row should ever produce is `ENOENT`:

```
  syscall racing one name                ok   ENOENT   EEXIST  other
  openat O_CREAT (no O_EXCL)           1171     2829        0      0
  openat O_CREAT|O_EXCL                 200        0     3800      0
  mkdirat                               200        0     3800      0
  symlinkat                             200        0     3800      0
  linkat                                200        0     3800      0
  renameat (replaces, all win)         4000        0        0      0
```

`O_CREAT|O_EXCL` is the same syscall, the same path resolution, the same
directory, and it is clean. So is every other name-creating `*at` call. Reads on
an existing file through `openat` are clean, and so is `openat` with `O_CREAT`
when the file already exists.

The single misbehaving row is the only operation in the set with a
**create-or-open fallback** — where a create that fails with `EEXIST` is retried
as an open. That retry is the hypothesis worth testing; it is not path
resolution, because the row above it resolves the same path the same way.

### The mechanism, in Apple's published source

The create-or-open fallback the table above points at is visible in
`vn_open_auth()`, the function `openat(2)` reaches through `open1()`. Line
numbers are against tag
[`xnu-12377.121.6`](https://github.com/apple-oss-distributions/xnu/tree/xnu-12377.121.6)
— the nearest published tag to the `xnu-12377.161.13` this reproduces on, since
that exact build is not published. `main` differs, so do not cite line numbers
from it.

Losing the create race retries as an open
([`vfs_vnops.c:524`](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/vfs/vfs_vnops.c#L524)):

```c
if ((error == EEXIST) && !(fmode & O_EXCL)) {
        if (vp) { vnode_put(vp); }
        nameidone(ndp);
        goto again;
}
```

`goto again` re-enters the `O_CREAT` branch, whose own lookup exits without any
retry
([`vfs_vnops.c:480`](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/vfs/vfs_vnops.c#L480)):

```c
continue_create_lookup:
        if ((error = namei(ndp))) {
                goto out;
        }
```

Meanwhile the same function already treats this exact condition as retryable,
bounded at `max_retries = 10` with progressive yielding, in the sibling path
reached when a vnode was obtained and then reported gone
([`vfs_vnops.c:790`](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/vfs/vfs_vnops.c#L790)):

```c
if (((error == ENOENT) && (*fmodep & O_CREAT)) || (error == EREDRIVEOPEN) || ref_failed) {
```

So "`ENOENT` when the caller asked for `O_CREAT` means retry" is already Apple's
own policy, with a bound and a yield strategy worked out. It just does not cover
the `namei()` exit.

**This is an argument, not a measurement.** It is a code path consistent with what
the probes observe, plus an existing retry that does not cover it. Which of the
two `ENOENT` exits the probes actually take is not established here: the
bounded-retry path logs when it gives up, but no kernel-process messages are
visible to `log show` on the machine tested, so that check was inconclusive
rather than supportive. Settling it needs `dtrace`/`ktrace` or a KDK build.

### It is fail-closed

Worth stating explicitly, because "the system says a file does not exist when it
does" invites the assumption that something worse is available.
`c/openat_race.c` checks for it on every row, and `c/failclosed.c` checks again
in the two directory shapes an attacker would choose. None of it happens:

- some thread always won — never a trial with zero successes;
- the file always existed afterwards;
- every successful opener saw the same inode — no inode confusion;
- the mode was always the one requested — no permission corruption.

`c/failclosed.c` runs the same race in three directory shapes, including the two
shaped like the classic `/tmp` attack, and enforces the result in its exit code:

```
  plain private directory            ok=   803 spurious-ENOENT=  2197 other=0
      no-winner trials=0  inode-divergent=0  wrong-mode=0  name-missing-after=0
  raced name is a DANGLING SYMLINK   ok=   905 spurious-ENOENT=  2095 other=0
      no-winner trials=0  inode-divergent=0  wrong-mode=0  symlink-clobbered=0
      target-not-created=0  opener-saw-wrong-object=0
  world-writable STICKY dir (01777)  ok=   751 spurious-ENOENT=  2249 other=0
      no-winner trials=0  inode-divergent=0  wrong-mode=0  name-missing-after=0

VERDICT: FAIL-CLOSED
```

With the raced name a **dangling symlink pointing out of the directory**, the
symlink was never clobbered into a regular file, its target was always the thing
created, and no opener ever received a non-target inode. The same held in a
**world-writable sticky (`01777`) directory**. If any of that ever fails the
program exits 3 and says the finding belongs in a security report instead.

Scope, so this is not read as more than it is: the four integrity checks are
made by the two C probes. The Go probe only counts errors, and
`c/other_syscalls.c` checks only for a second winner and for the name going
missing.

So the defect forges one errno — `ENOENT`, "does not exist" — and nothing else.
It is an amplifier for callers that treat "does not exist" as a benign, safe
answer, and not a primitive on its own. That is why this belongs on a public
tracker rather than a security address.

### Why Go programs hit this

`os.Root` (Go 1.24+) resolves every path component itself, with `openat`, so
that a path cannot escape the root. That is the entire point of the type, and it
is documented as safe for concurrent use. It also means `os.Root` inherits this
defect, while plain `os.OpenFile` — which hands the assembled path to `open(2)`
once — did not reproduce it in any of these runs:

```
  row                                      ok   ENOENT   EEXIST  other
  os.Root.OpenFile  "f"    O_CREATE       308      692        0      0
  os.Root.OpenFile  "d/f"  O_CREATE       426      574        0      0
  os.Root.OpenFile  "f"    +O_EXCL         50        0      950      0
  os.Root.OpenFile  distinct names       1000        0        0      0
  os.Root.Mkdir     "d"                    50        0      950      0
  os.OpenFile       full path            1000        0        0      0
```

The error surfaces as `openat f: no such file or directory` wrapping
`fs.ErrNotExist`, which is exactly the error a caller reads as "not there yet".
Reproduced on Go 1.26.6, 1.26.7 and 1.27.0, on both `darwin/arm64` and
`darwin/amd64`; not fixed in any of them.

Nesting is irrelevant — a flat single-component name fails as hard as `d/f` —
and so is concurrent creation of the parent directory. The trigger is narrowly
concurrent `O_CREATE` on **one** path through **one** shared `*os.Root`.

`CGO_ENABLED` makes no difference (441/484/512 versus 467/453/455 per 1000):
macOS routes syscalls through libSystem either way. There is no build flag that
avoids it.

The practical workaround for Go code is to retry on `ENOENT` when `O_CREATE` was
requested, or to create the file once up front and open it without `O_CREATE`
thereafter. A lock file opened `O_RDWR|O_CREATE` by every participant is the
worst case, and the one that led here.

### The harness

The reproduction is in plain C with no Go involved, so the defect is below the Go
runtime — in the platform's `openat` path. Everything here goes through
libSystem, so these probes do not by themselves separate the kernel from the
libc wrapper; assigning it specifically to XNU would need syscall tracing, which
this repository does not do. To keep the harness itself from being the story:

- all four C probes build warning-free under Apple clang and GCC 16, at
  `-Wall -Wextra -Werror`, under strict `-std=c11` as well as `-std=gnu11`;
- every probe enforces its own claim in its exit code rather than leaving it to
  the reader: an unexpected errno anywhere makes a run `ANOMALOUS` (exit 2), a
  second misbehaving syscall makes `other_syscalls` `NOT-ISOLATED`, and a
  fail-closed violation makes `failclosed` `NOT-FAIL-CLOSED` (exit 3);
- `openat_race.c` under `-fsanitize=thread` reports **no data races**, and still
  reproduces at 105 out of 120 calls;
- the threads are released through a two-phase barrier — arrive on a condition
  variable, then a short hot spin on an atomic gate — so that a thread already
  awake cannot starve one still being created. An earlier version hot-spun
  through the whole wake-up cascade, which stretched the release across a
  scheduler quantum and burned 14 cores to do it.

## Runner matrix

`.github/workflows/matrix.yml` runs the probes on every *standard* macOS runner
image GitHub currently offers — the paid larger runners and the preview Xcode
images are left out — plus Linux on both architectures as a negative control,
and collects the records into one table in the run summary.

| leg | why it is there |
| --- | --- |
| `macos-26`, `macos-26-intel` | same macOS generation, both architectures |
| `macos-15`, `macos-15-intel` | same again, one generation back |
| `macos-14` | oldest image GitHub still offers |
| `ubuntu-24.04`, `ubuntu-24.04-arm` | same source, same probes, must come out clean |

The two same-OS architecture pairs are the point, and they came back
reproducing on both sides — see the table above. They exist because arm64 has a
weaker memory model than x86's TSO, so a missing barrier in the kernel would
surface on Apple silicon and could have been invisible on Intel. It was not, so
that is not the lead.

Two limits worth knowing. GitHub retired the `macos-13` image on 2025-12-04, so
macOS 13 and earlier cannot be tested here at all — `macos-14` is the floor, and
whether this predates Sonoma is not answerable from CI. And `macos-15-intel` is
scheduled to retire in Fall 2027, and GitHub has said x86_64 support on Actions
ends with it — though `macos-26-intel` has since appeared, so treat the exact end
date as GitHub's to move. Whenever it happens, this matrix loses its
architecture pairs.

Standard runners are free and unmetered on public repositories, macOS included;
the run above reported 0 billable milliseconds for all five macOS legs. Only
larger (`-large` / `-xlarge`) runners are billed on a public repository.

Adding the paid `macos-14-large` leg (Intel, macOS 14) would complete the grid;
it is left out because larger runners are billed even for public repositories.
It is one line in the matrix.

## Not tested

- **Cross-uid racing.** Every probe runs as a single uid.
- **macOS 13 and earlier.** Not available on GitHub-hosted runners any more, so
  the oldest version tested is 14.8.7.
- **Filesystems other than APFS**, and network or case-sensitive volumes.
- **Whether the create-or-open retry is really the mechanism.** That is a
  hypothesis from the syscall table, not something this repository demonstrates.

## Related

- [golang/go#75114](https://github.com/golang/go/issues/75114) —
  `Root.MkdirAll` returning "file exists" when called concurrently on the same
  path. The same class of problem on a different operation; accepted, fixed in
  CL 698215, backported to 1.24 and 1.25.
- [golang/go#73077](https://github.com/golang/go/issues/73077) — use `openat2`
  with `RESOLVE_BENEATH` on Linux. Still open, and part of why Linux walks the
  same component-wise path in the same Go code yet is unaffected.
- [golang/go#73079](https://github.com/golang/go/issues/73079) — use
  `O_NOFOLLOW_ANY` on macOS. `O_NOFOLLOW` is not the trigger here: it changes
  nothing either way.

Worth noting for both of those: current macOS `open(2)` documents
`O_NOFOLLOW_ANY`, `O_RESOLVE_BENEATH` and `O_UNIQUE`, so darwin does have a
confining-resolution flag of its own. It would not avoid this bug — the flat
single-component case still goes through `openat` with `O_CREAT` — but it is
relevant to how `os.Root` could resolve paths on darwin.

## Layout

```
probe.sh                       build and run everything; prints one record line
c/openat_race.c                the headline probe: 1 suspect row, 5 controls
c/other_syscalls.c             mkdirat / symlinkat / linkat / renameat for contrast
c/failclosed.c                 is it fail-closed? incl. the two attack shapes
c/processes.c                  the same race across forked processes, not threads
go/main.go                     os.Root vs plain os.*, with controls
ci/summarize.sh                records -> markdown table
.github/workflows/matrix.yml   the runner matrix
```

Each C file carries its own copy of the release barrier so that any one of them
can be read, built or pasted on its own.

## License

MIT. See [LICENSE](LICENSE).
