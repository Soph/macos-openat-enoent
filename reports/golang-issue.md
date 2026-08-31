# os: Root.OpenFile returns ENOENT when goroutines concurrently create the same path on macOS

### Go version

`go1.26.6 darwin/arm64` for the local runs quoted below. Also reproduced on `go1.26.7` and `go1.27.0`, and on `darwin/amd64`. The CI matrix linked below runs `go1.27.0`. Not fixed in any of them.

### Output of `go env` in your module/workspace:

```shell
AR='ar'
CC='clang'
CGO_CFLAGS='-O2 -g'
CGO_CPPFLAGS=''
CGO_CXXFLAGS='-O2 -g'
CGO_ENABLED='1'
CGO_FFLAGS='-O2 -g'
CGO_LDFLAGS='-O2 -g'
CXX='clang++'
GCCGO='gccgo'
GO111MODULE=''
GOARCH='arm64'
GOARM64='v8.0'
GOAUTH='netrc'
GOBIN='/Users/soph/.local/share/mise/installs/go/1.26.6/bin'
GOCACHE='/Users/soph/Library/Caches/go-build'
GOCACHEPROG=''
GODEBUG=''
GOENV='/Users/soph/Library/Application Support/go/env'
GOEXE=''
GOEXPERIMENT=''
GOFIPS140='off'
GOFLAGS=''
GOGCCFLAGS='-fPIC -arch arm64 -pthread -fno-caret-diagnostics -Qunused-arguments -fmessage-length=0 -ffile-prefix-map=/var/folders/gz/h7sjhvz13cb0gcrzzcncqtyw0000gn/T/go-build2816466184=/tmp/go-build -gno-record-gcc-switches -fno-common'
GOHOSTARCH='arm64'
GOHOSTOS='darwin'
GOINSECURE=''
GOMOD='/tmp/verify6/go.mod'
GOMODCACHE='/Users/soph/go/pkg/mod'
GONOPROXY=''
GONOSUMDB=''
GOOS='darwin'
GOPATH='/Users/soph/go'
GOPRIVATE=''
GOPROXY='https://proxy.golang.org,direct'
GOROOT='/Users/soph/.local/share/mise/installs/go/1.26.6'
GOSUMDB='sum.golang.org'
GOTELEMETRY='local'
GOTELEMETRYDIR='/Users/soph/Library/Application Support/go/telemetry'
GOTMPDIR=''
GOTOOLCHAIN='auto'
GOTOOLDIR='/Users/soph/.local/share/mise/installs/go/1.26.6/pkg/tool/darwin_arm64'
GOVCS=''
GOVERSION='go1.26.6'
GOWORK=''
PKG_CONFIG='pkg-config'
```

### What did you do?

Several goroutines call `Root.OpenFile` with `O_CREATE` on the same path through one shared `*os.Root`. The `os.Root` documentation says:

> Methods on Root are safe to be used from multiple goroutines simultaneously.

And `O_CREATE` without `O_EXCL` has two correct outcomes: the file was created, or the existing one was opened. So every caller should get a descriptor.

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

Most of the calls fail. Three consecutive runs on macOS 26.6 (25G72, M4 Max), `go1.26.6`:

```
592/1000 OpenFile calls failed; sample: openat f: no such file or directory
666/1000 OpenFile calls failed; sample: openat f: no such file or directory
615/1000 OpenFile calls failed; sample: openat f: no such file or directory
```

The error wraps `fs.ErrNotExist`, which is the error a caller reads as "not there yet".

Variants, 50 trials of 20 goroutines each on the same machine:

| variant | failures |
| --- | --: |
| `Root.OpenFile("f", O_CREATE)`, same path | **692 / 1000** |
| `Root.OpenFile("d/f", O_CREATE)`, same path, nested | **574 / 1000** |
| `Root.OpenFile(...)`, a distinct path per goroutine | 0 / 1000 |
| `Root.OpenFile("f", O_CREATE\|O_EXCL)` | 0 / 1000, correct `EEXIST` |
| `Root.Mkdir("d")`, same name | 0 / 1000, correct `EEXIST` |
| `os.OpenFile(filepath.Join(dir,"f"), O_CREATE)` | 0 / 1000 |

Nesting does not matter. A flat single-component name fails as hard as `d/f`. Creating the parent concurrently does not matter either. The trigger is concurrent `O_CREATE` on one path through one shared `*os.Root`.

### What did you expect to see?

`0/1000`. Every caller should get a descriptor, because `O_CREATE` without `O_EXCL` means create the file, or open it if someone else created it first. That is what the plain `os.OpenFile` row does on the same machine, on the same filesystem, in an identically created directory.

`ENOENT` is not a correct answer for any of these callers. The file exists by the time the error comes back, the caller is the one that asked for it to be created, and the directory holding it is pinned by an open descriptor for the whole call. Apple's own `open(2)`, which covers `openat()`, lists exactly two `ENOENT` conditions, and neither of them applies:

```
[ENOENT]  O_CREAT is not set and the named file does not exist.
[ENOENT]  A component of the path name that must exist does not exist.
```

`O_CREAT` is set, so the first does not apply. In `openat(dirfd, "f", O_RDWR|O_CREAT, 0600)` the only component is `f` itself, the file being created, which is precisely the component that is not required to exist, so the second does not either. There is no third `ENOENT` clause. POSIX is narrower still: with `O_CREAT` set it permits `ENOENT` only when a component of the *path prefix* does not name an existing file.

So the problem is the errno rather than the timing. A caller that reads "no such file or directory" as a benign or absent-file answer gets a wrong answer with no way to tell it apart from a true one.

The rate depends on how much real parallelism the machine has. A 3-core CI runner scores a few percent where a 16-core laptop scores 60% to 79%. A low number is the same bug with fewer chances to land, not a milder one.

### Why this reaches os.Root and not plain os

The defect is below Go. It reproduces in plain C with no Go involved. On macOS, `openat(dirfd, name, O_CREAT)` without `O_EXCL` returns a spurious `ENOENT` to most callers racing the first creation of one name.

`os.Root` resolves path components with `openat`, which is the point of the type, so it inherits this. Plain `os.OpenFile` hands an assembled path to `open(2)` once, so it does not. `CGO_ENABLED` makes no difference, because darwin routes syscalls through libSystem either way. There is no build-flag workaround.

The isolation is narrow, which is probably the most useful part for diagnosis. On the same machine, racing 20 threads on one name: `O_CREAT|O_EXCL`, `openat` with `O_CREAT` on an existing file, plain reads through `openat`, and `mkdirat`, `symlinkat`, `linkat` and `renameat` are all correct. Only the one operation with a create-or-open fallback is wrong, where a create that fails with `EEXIST` is retried as an open. That fallback is visible in Apple's published source (`vn_open_auth` in `bsd/vfs/vfs_vnops.c`), and the same function already retries this exact condition, bounded, in a sibling path.

Scope of the measurements:

- macOS 14.8.7, 15.7.7, 15.7.9, 26.5.2, 26.6, 26.6.1 and 26.6.2.
- Both `arm64` and `x86_64`, on real Intel hardware, not virtualised.
- 6% to 9% on 3-core and 4-core CI runners, 66% to 79% on a 16-core laptop.
- Forked processes racing one name fail at the same rate as threads, so this is not a scheduler artifact.
- Linux is unaffected: 0 out of 40,000 calls on both architectures, same source.

Probes, controls and a 7-runner CI matrix: **https://github.com/Soph/macos-openat-enoent**

### Possible fixes

Two options, both of which would sit in the darwin `openat` path.

1. Retry `ENOENT` when `O_CREATE` was requested, bounded. On the question of how large a bound needs to be, which came up on #75114, two data points that may help: xnu's own retry for this condition is bounded at 10 with progressive yielding, and a downstream project that hit this measured retry depth 1 always sufficing, with 0 failures in 14,400 attempts ([spiceai/spiceai#13232](https://github.com/spiceai/spiceai/issues/13232)).

2. Split create-or-open into `O_CREATE|O_EXCL` followed by a plain open, so the broken path is never taken. This is deterministic rather than probabilistic and needs no retry budget. It costs one extra syscall, and only on first creation. In our own codebase this took a lock-file open from 4806 failures in 6000 racing opens to 0, and a flaky test from 12 failures in 20 fresh processes to 0 in 20.

The second is what we used in our own code, for what that is worth. Either looks like it would work, and you will have a better sense than I do of which fits the package.

If a CL would be useful I am happy to write one, for whichever approach you prefer. I have a machine that reproduces this reliably and the CI matrix above, so I can verify a fix rather than only propose one.

One question I would want your view on first: a regression test for this can only be probabilistic, and only on darwin, since it depends on the scheduler leaving the callers overlapping. It might fit better as a stress test than a normal one, but I do not know what you would want there.

### Related

- #75114, `Root.MkdirAll` returning "file exists" when called concurrently on the same path. Same class, different operation. Accepted and fixed in CL 698215, backported to 1.24 and 1.25. Mentioning it in case it is a useful precedent, since that one was also an OS-level race handled on the Go side.
- #73077 and #73079, `RESOLVE_BENEATH` and `O_NOFOLLOW_ANY`. Current macOS does document `O_RESOLVE_BENEATH`, but it would not avoid this, because the flat single-component case still goes through `openat` with `O_CREAT`.

### A note on the documentation

`os.Root` already documents a list of platform-specific caveats, and one of them is a race condition:

> Root's behavior differs on some platforms:
> [...]
> - On Unix, Root.Chmod, Root.Chown, and Root.Chtimes are vulnerable to a race condition. If the target of the operation is changed from a regular file to a symlink while the operation is in progress, the operation may be performed on the link rather than the link target.

I mention it because that list looks like the natural home for this if the behaviour cannot be changed. `Root.OpenFile` with `O_CREATE` on darwin is not in it today, and the concurrency sentence sits just above it, so a caller on macOS currently has no way to find out.

A fix would help callers more than a doc note would, but which of those makes sense is your call. I will report this to Apple too, since the defect is theirs. The only thing I would add for prioritisation is that an OS fix would reach future macOS versions, while a change in Go would reach people on the versions they are running today.

### Disclosure

I used an AI assistant to research and write this up. Every number in it comes from a run on a machine I control or from the CI matrix linked above, the reproducer was compiled and run as it appears here, and the man page and kernel source quotes are from the versions named.
