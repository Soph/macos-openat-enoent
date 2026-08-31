# os: Root.OpenFile returns ENOENT when goroutines concurrently create the same path on macOS

### Go version

`go1.27.0 darwin/arm64` — also reproduced on `go1.26.6` and `go1.26.7`, and on
`darwin/amd64`. Not fixed in any of them.

### What did you do?

Several goroutines call `Root.OpenFile` with `O_CREATE` on the **same path**
through one shared `*os.Root`. `os.Root` documents itself as safe for concurrent
use, and `O_CREATE` without `O_EXCL` has two correct outcomes — the file was
created, or the existing one was opened — so every caller should get a
descriptor.

Minimal reproducer (stdlib only, no dependencies):

```go
package main

import (
	"fmt"
	"os"
	"sync"
)

// trial releases n goroutines simultaneously into O_CREATE on one path through
// one shared *os.Root, and reports how many of them failed.
func trial(n int) (failed int, sample error) {
	dir, err := os.MkdirTemp("", "rootrace")
	if err != nil {
		panic(err)
	}
	defer os.RemoveAll(dir)
	root, err := os.OpenRoot(dir)
	if err != nil {
		panic(err)
	}
	defer root.Close()

	var parked, done sync.WaitGroup
	var mu sync.Mutex
	start := make(chan struct{})
	parked.Add(n)
	done.Add(n)
	for range n {
		go func() {
			defer done.Done()
			parked.Done()
			<-start
			f, err := root.OpenFile("f", os.O_RDWR|os.O_CREATE, 0o600)
			if err != nil {
				mu.Lock()
				failed++
				if sample == nil {
					sample = err
				}
				mu.Unlock()
				return
			}
			f.Close()
		}()
	}
	parked.Wait() // every goroutine is at the channel before any of them proceeds
	close(start)
	done.Wait()
	return
}

func main() {
	const trials, n = 50, 20
	failed := 0
	var sample error
	for range trials {
		f, s := trial(n)
		failed += f
		if sample == nil {
			sample = s
		}
	}
	fmt.Printf("%d/%d OpenFile calls failed; sample: %v\n", failed, trials*n, sample)
}
```

### What did you see happen?

Most of the calls fail. Three consecutive runs on macOS 26.6 (25G72, M4 Max),
`go1.26.6`:

```
592/1000 OpenFile calls failed; sample: openat f: no such file or directory
666/1000 OpenFile calls failed; sample: openat f: no such file or directory
615/1000 OpenFile calls failed; sample: openat f: no such file or directory
```

The error wraps `fs.ErrNotExist`, which is exactly the error a caller reads as
"not there yet". Across 50 trials × 20 goroutines on macOS 26.6 (M4 Max):

| variant | failures |
| --- | --: |
| `Root.OpenFile("f", O_CREATE)` — same path | **692 / 1000** |
| `Root.OpenFile("d/f", O_CREATE)` — same path, nested | **574 / 1000** |
| `Root.OpenFile(...)` — a distinct path per goroutine | 0 / 1000 |
| `Root.OpenFile("f", O_CREATE\|O_EXCL)` | 0 / 1000 (correct `EEXIST`) |
| `Root.Mkdir("d")` — same name | 0 / 1000 (correct `EEXIST`) |
| `os.OpenFile(filepath.Join(dir,"f"), O_CREATE)` — control | 0 / 1000 |

### What did you expect to see?

`0/1000`, which is what the plain-`os` control row does on the same machine, on
the same filesystem, in an identically created directory.

The rate depends on how much true parallelism the machine has — a 3-core CI
runner scores a few percent where a 16-core laptop scores 60–79% — so a low
number is the same bug with fewer chances to land, not a milder one.

### Why this reaches `os.Root` and not plain `os`

The defect is below Go: it reproduces in plain C with no Go involved. On macOS,
`openat(dirfd, name, O_CREAT)` **without** `O_EXCL` returns a spurious `ENOENT`
to most callers racing the first creation of one name.

`os.Root` resolves path components with `openat`, which is the entire point of
the type, so it inherits this. Plain `os.OpenFile` hands an assembled path to
`open(2)` once, and is unaffected. `CGO_ENABLED` makes no difference (darwin
routes syscalls through libSystem either way), so there is no build-flag
workaround.

The isolation is narrow, and it is the useful part: `O_CREAT|O_EXCL`, `openat`
with `O_CREAT` on an existing file, plain reads through `openat`, and
`mkdirat` / `symlinkat` / `linkat` / `renameat` racing one name are all correct
on the same machine. Only the one operation with a *create-or-open fallback* is
wrong, which is consistent with the fallback being where it happens — and that
fallback is visible in Apple's published source.

Reproduced on macOS 14.8.7, 15.7.7, 15.7.9, 26.5.2, 26.6, 26.6.1 and 26.6.2, on
both `arm64` and `x86_64` (real Intel hardware, not virtualised), at 6–9% on
3–4 core CI runners and 66–79% on a 16-core laptop. Linux is unaffected: 0 out of
40,000 calls on both architectures running the same source. It is also not a
threading artifact — forked processes racing one name fail at the same rate.

Full probes, controls and a 7-runner CI matrix:
**https://github.com/Soph/macos-openat-enoent**

### Suggested fix

Two options, both confined to the darwin `openat` path:

1. **Retry `ENOENT` when `O_CREATE` was requested**, bounded. The concern raised
   on #75114 about unbounded retries has two answers here: xnu's own retry for
   this same condition is bounded at 10 with progressive yielding, and a
   downstream project that hit this measured retry **depth 1 always sufficing —
   0 failures in 14,400 attempts** ([spiceai/spiceai#13232](https://github.com/spiceai/spiceai/issues/13232)).
2. **Split create-or-open into `O_CREATE|O_EXCL` then a plain open**, so the
   broken path is never taken. This is deterministic rather than probabilistic
   and needs no retry budget. It costs one extra syscall only on first creation.
   We shipped this in our own codebase and it took the failure rate from 4806 in
   6000 racing opens to 0.

### Related

- #75114 — `Root.MkdirAll` returning "file exists" when called concurrently on
  the same path. Same class, different operation; accepted and fixed in CL 698215,
  backported to 1.24 and 1.25. That is the precedent for treating this as a Go
  bug worth working around rather than only an OS bug.
- #73077, #73079 — `RESOLVE_BENEATH` / `O_NOFOLLOW_ANY`. Note current macOS
  documents `O_RESOLVE_BENEATH`, but it would not avoid this: the flat
  single-component case still goes through `openat` with `O_CREAT`.

An Apple report is being filed separately, since the defect is theirs. The reason
to also fix it here is that `os.Root`'s documented concurrency guarantee is
broken on a supported platform today, and an OS fix — if it comes — reaches only
future macOS versions, while a Go-side change reaches every user on every macOS
version.
