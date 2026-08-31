# Spurious `ENOENT` from `openat(O_CREAT)` on macOS

On macOS, when several threads race to `openat(dirfd, name, O_CREAT)` the same
not-yet-existing name in the same directory, most of them get `ENOENT`. One
thread creates the file correctly; the others are told the file does not exist —
the file they themselves asked the kernel to create, and which by then does
exist.

`O_CREAT` without `O_EXCL` means *create it, or open it if someone beat me to
it*. Every caller should get a descriptor. That is what `open(2)` on the
assembled full path does on the same machine, in the same directory, on the same
filesystem: 4000 out of 4000 calls succeed. Only the `openat` form fails.

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
cc -O2 -pthread -std=gnu11 -o openat_race c/openat_race.c && ./openat_race 20 200
cc -O2 -pthread -std=gnu11 -o other_syscalls c/other_syscalls.c && ./other_syscalls 20 200
cd go && go run . -threads 20 -trials 50
```

### Reading a clean result

A clean run is weaker evidence than a reproducing one. The rate depends on how
much true parallelism the machine has, so a 3-core CI runner can score zero at a
size where a laptop scores 70%. `probe.sh` therefore climbs a ladder — 20×200,
then 20×1000, then 40×1000 — and stops at the first size that reproduces. The
`size` column of the results table says which stage answered, and a `stage 1`
clean row means only that the smallest size found nothing.

A run is also only interpretable if the controls are clean. Both C probes and the
Go probe carry their own controls and say so in their verdict; a control failing
is reported as `ANOMALOUS` and means something is wrong with the harness or the
environment. Do not quote a number from an `ANOMALOUS` run.

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
from one run of `.github/workflows/matrix.yml` on 2026-08-31:

| runner | OS | build | arch | cores | CPU | size | spurious `ENOENT` | verdict | Go 1.27.0 |
| --- | --- | --- | --- | --: | --- | --- | --: | --- | --- |
| `macos-14` | macOS 14.8.7 | 23J520 | arm64 | 3 | Apple M1 (Virtual) | 20×200 | 362 / 4000 (9%) | **REPRODUCES** | 45 / 2000 |
| `macos-15` | macOS 15.7.7 | 24G720 | arm64 | 3 | Apple M1 (Virtual) | 20×200 | 223 / 4000 (6%) | **REPRODUCES** | 57 / 2000 |
| `macos-26` | macOS 26.5.2 | 25F84 | arm64 | 3 | Apple M1 (Virtual) | 20×200 | 288 / 4000 (7%) | **REPRODUCES** | 71 / 2000 |
| `macos-15-intel` | macOS 15.7.9 | 24G830 | x86_64 | 4 | Core i7-8700B | 20×200 | 231 / 4000 (6%) | **REPRODUCES** | 41 / 2000 |
| `macos-26-intel` | macOS 26.6.1 | 25G76 | x86_64 | 4 | Core i7-8700B | 20×200 | 220 / 4000 (6%) | **REPRODUCES** | 35 / 2000 |
| `ubuntu-24.04` | Ubuntu 24.04.4, 6.17.0 | — | x86_64 | 4 | Xeon Platinum 8573C | 40×1000 | 0 / 40000 | clean | 0 / 2000 |
| `ubuntu-24.04-arm` | Ubuntu 24.04.4, 6.17.0 | — | arm64 | 4 | — | 40×1000 | 0 / 40000 | clean | 0 / 2000 |

Every control row was clean and every row was fail-closed, on all seven.

Four things follow.

**It is not architecture-specific.** Both `-intel` legs are real Intel hardware —
a Core i7-8700B, not a virtualised M1 — and both reproduce, at the same rate as
the arm64 legs on the same OS version. arm64's weaker memory model is therefore
not needed to explain this, which makes a missing barrier the less likely story
and an architecture-independent logic bug in the create-or-open fallback the
more likely one. The two same-OS pairs are what settle it: 15.7.7/arm64 at 6%
against 15.7.9/x86_64 at 6%, and 26.5.2/arm64 at 7% against 26.6.1/x86_64 at 6%.

**It is not a macOS 26 regression.** It reproduces on Sonoma 14.8.7, on Sequoia
15.7.7 and 15.7.9, and on Tahoe 26.5.2, 26.6.1, 26.6 and 26.6.2 — every macOS
version testable here. macOS 13 and earlier cannot be tested on GitHub-hosted
runners any more, so how much further back it goes is still open.

**Linux is unaffected at ten times the volume.** Both Linux legs climbed the
ladder to 40 threads × 1000 trials and returned 0 spurious `ENOENT` out of
40000 calls, on both architectures, running the same source. That is the control
that rules out the harness.

**The rate tracks available parallelism, not severity.** The runners score 6–9%
on 3–4 cores where a 16-core laptop scores 66–79%. A low number on a small
machine is not a mild version of the bug; it is the same bug with fewer chances
to land.

### It is one operation, not path resolution

`openat` with `O_CREAT` is not the only `*at` syscall that creates a name in a
directory, and not the only one that can lose a race for that name. Racing 20
threads on one name through each of them, correct behaviour is exactly one
winner, `EEXIST` for everyone else, and no `ENOENT` anywhere:

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

### It is fail-closed

Worth stating explicitly, because "the kernel says a file does not exist when it
does" invites the assumption that something worse is available. Every probe
checks for it, on every row, and none of it happens:

- some thread always won — never a trial with zero successes;
- the file always existed afterwards;
- every successful opener saw the same inode — no inode confusion;
- the mode was always the one requested — no permission corruption.

Two variants shaped like the classic `/tmp` attack were probed separately, at
200 trials × 20 threads: with the raced name a **dangling symlink pointing out of
the directory**, the symlink was never clobbered into a regular file, the target
was always the file that got created, and no opener ever received a non-target
inode. The same held in a **world-writable sticky (`01777`) directory**.

So the defect forges one errno — `ENOENT`, "does not exist" — and nothing else.
It is an amplifier for callers that treat "does not exist" as a benign, safe
answer, and not a primitive on its own. That is why this belongs on a public
tracker rather than a security address.

### Why Go programs hit this

`os.Root` (Go 1.24+) resolves every path component itself, with `openat`, so
that a path cannot escape the root. That is the entire point of the type, and it
is documented as safe for concurrent use. It also means `os.Root` inherits this
defect, while plain `os.OpenFile` — which hands the assembled path to `open(2)`
once — does not:

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
runtime. To keep the harness itself from being the story:

- both C probes build warning-free under Apple clang and GCC 16, at
  `-Wall -Wextra`, and under strict `-std=c11` as well as `-std=gnu11`;
- `openat_race.c` under `-fsanitize=thread` reports **no data races**, and still
  reproduces at 105 out of 120 calls;
- the threads are released through a two-phase barrier — arrive on a condition
  variable, then a short hot spin on an atomic gate — so that a thread already
  awake cannot starve one still being created. An earlier version hot-spun
  through the whole wake-up cascade, which stretched the release across a
  scheduler quantum and burned 14 cores to do it.

## Runner matrix

`.github/workflows/matrix.yml` runs the probes on every macOS image GitHub
currently offers, plus Linux on both architectures as a negative control, and
collects the records into one table in the run summary.

| leg | why it is there |
| --- | --- |
| `macos-26`, `macos-26-intel` | same OS, both architectures |
| `macos-15`, `macos-15-intel` | same OS, both architectures, one release back |
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
the last x86_64 image Actions will offer, retiring in Fall 2027, after which this
matrix loses its architecture pairs.

Standard runners are free and unmetered on public repositories, macOS included;
the run above reported 0 billable milliseconds for all five macOS legs. Only
larger (`-large` / `-xlarge`) runners are billed on a public repository.

Adding the paid `macos-14-large` leg (Intel, macOS 14) would complete the grid;
it is left out because larger runners are billed even for public repositories.
It is one line in the matrix.

## Not tested

- **Cross-uid racing.** Everything here is threads in one process under one uid.
- **Separate processes.** Likewise: every probe races threads, not processes.
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

## Layout

```
probe.sh                       build and run everything; prints one record line
c/openat_race.c                the headline probe: 1 suspect row, 5 controls
c/other_syscalls.c             mkdirat / symlinkat / linkat / renameat for contrast
go/main.go                     os.Root vs plain os.*, with controls
ci/summarize.sh                records -> markdown table
.github/workflows/matrix.yml   the runner matrix
```

Each C file carries its own copy of the release barrier so that either one can
be read, built or pasted on its own.

## License

MIT. See [LICENSE](LICENSE).
