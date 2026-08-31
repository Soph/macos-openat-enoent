# os: Root.OpenFile returns ENOENT when goroutines concurrently create the same path on macOS

### Go version

`go1.27.0 darwin/arm64`. Also reproduced on `go1.26.6` and `go1.26.7`, and on
`darwin/amd64`. Not fixed in any of them.

### What did you do?

Several goroutines call `Root.OpenFile` with `O_CREATE` on the same path through
one shared `*os.Root`. `os.Root` is documented as safe for concurrent use, and
`O_CREATE` without `O_EXCL` has two correct outcomes: the file was created, or
the existing one was opened. So every caller should get a descriptor.

Reproducer, stdlib only:

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

The error wraps `fs.ErrNotExist`, which is the error a caller reads as "not
there yet".

Variants, 50 trials of 20 goroutines each on the same machine:

| variant | failures |
| --- | --: |
| `Root.OpenFile("f", O_CREATE)`, same path | **692 / 1000** |
| `Root.OpenFile("d/f", O_CREATE)`, same path, nested | **574 / 1000** |
| `Root.OpenFile(...)`, a distinct path per goroutine | 0 / 1000 |
| `Root.OpenFile("f", O_CREATE\|O_EXCL)` | 0 / 1000, correct `EEXIST` |
| `Root.Mkdir("d")`, same name | 0 / 1000, correct `EEXIST` |
| `os.OpenFile(filepath.Join(dir,"f"), O_CREATE)` | 0 / 1000 |

Nesting does not matter. A flat single-component name fails as hard as `d/f`.
Creating the parent concurrently does not matter either. The trigger is
concurrent `O_CREATE` on one path through one shared `*os.Root`.

### What did you expect to see?

`0/1000`. Every caller should get a descriptor, because `O_CREATE` without
`O_EXCL` means create the file, or open it if someone else created it first. That
is what the plain `os.OpenFile` row does on the same machine, on the same
filesystem, in an identically created directory.

`ENOENT` is not a correct answer for any of these callers. The file exists by the
time the error comes back, the caller is the one that asked for it to be created,
and the directory holding it is pinned by an open descriptor for the whole call.
Apple's own `open(2)`, which covers `openat()`, lists exactly two `ENOENT`
conditions, and neither of them applies:

```
[ENOENT]  O_CREAT is not set and the named file does not exist.
[ENOENT]  A component of the path name that must exist does not exist.
```

`O_CREAT` is set, so the first does not apply. In
`openat(dirfd, "f", O_RDWR|O_CREAT, 0600)` the only component is `f` itself, the
file being created, which is precisely the component that is not required to
exist, so the second does not either. There is no third `ENOENT` clause. POSIX is
narrower still: with `O_CREAT` set it permits `ENOENT` only when a component of
the *path prefix* does not name an existing file.

So the bug is the errno. A caller that treats "no such file or directory" as a
benign, retryable, or absent-file answer gets a wrong answer it has no way to
tell apart from the truth.

The rate depends on how much real parallelism the machine has. A 3-core CI
runner scores a few percent where a 16-core laptop scores 60% to 79%. A low
number is the same bug with fewer chances to land, not a milder one.

### Why this reaches os.Root and not plain os

The defect is below Go. It reproduces in plain C with no Go involved. On macOS,
`openat(dirfd, name, O_CREAT)` without `O_EXCL` returns a spurious `ENOENT` to
most callers racing the first creation of one name.

`os.Root` resolves path components with `openat`, which is the point of the type,
so it inherits this. Plain `os.OpenFile` hands an assembled path to `open(2)`
once, so it does not. `CGO_ENABLED` makes no difference, because darwin routes
syscalls through libSystem either way. There is no build-flag workaround.

The isolation is narrow, and that is the useful part. On the same machine,
racing 20 threads on one name: `O_CREAT|O_EXCL`, `openat` with `O_CREAT` on an
existing file, plain reads through `openat`, and `mkdirat`, `symlinkat`,
`linkat` and `renameat` are all correct. Only the one operation with a
create-or-open fallback is wrong, where a create that fails with `EEXIST` is
retried as an open. That fallback is visible in Apple's published source
(`vn_open_auth` in `bsd/vfs/vfs_vnops.c`), and the same function already retries
this exact condition, bounded, in a sibling path.

Scope of the measurements:

- macOS 14.8.7, 15.7.7, 15.7.9, 26.5.2, 26.6, 26.6.1 and 26.6.2.
- Both `arm64` and `x86_64`, on real Intel hardware, not virtualised.
- 6% to 9% on 3-core and 4-core CI runners, 66% to 79% on a 16-core laptop.
- Forked processes racing one name fail at the same rate as threads, so this is
  not a scheduler artifact.
- Linux is unaffected: 0 out of 40,000 calls on both architectures, same source.

Probes, controls and a 7-runner CI matrix:
**https://github.com/Soph/macos-openat-enoent**

### Suggested fix

Two options, both confined to the darwin `openat` path.

1. Retry `ENOENT` when `O_CREATE` was requested, bounded. The concern about
   unbounded retries raised on #75114 has two answers here. xnu's own retry for
   this condition is bounded at 10 with progressive yielding. And a downstream
   project that hit this measured retry depth 1 always sufficing, with 0
   failures in 14,400 attempts ([spiceai/spiceai#13232](https://github.com/spiceai/spiceai/issues/13232)).

2. Split create-or-open into `O_CREATE|O_EXCL` followed by a plain open, so the
   broken path is never taken. This is deterministic rather than probabilistic
   and needs no retry budget. It costs one extra syscall, and only on first
   creation. In our own codebase this took a lock-file open from 4806 failures
   in 6000 racing opens to 0, and a flaky test from 12 failures in 20 fresh
   processes to 0 in 20.

We prefer the second, but either would fix it.

### Related

- #75114, `Root.MkdirAll` returning "file exists" when called concurrently on
  the same path. Same class, different operation. Accepted and fixed in CL
  698215, backported to 1.24 and 1.25. That is the precedent for treating this
  as worth working around in Go rather than only as an OS bug.
- #73077 and #73079, `RESOLVE_BENEATH` and `O_NOFOLLOW_ANY`. Current macOS does
  document `O_RESOLVE_BENEATH`, but it would not avoid this, because the flat
  single-component case still goes through `openat` with `O_CREAT`.

We intend to report this to Apple as well, since the defect is theirs. The reason
to also fix it in Go is that `os.Root`'s documented concurrency guarantee is
broken on a supported platform today, and an OS fix would reach only future
macOS versions, while a Go-side change reaches every user on every macOS version.
