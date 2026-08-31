// Command openatrace shows why this reaches Go programs through os.Root.
//
// os.Root (Go 1.24+) resolves every path component itself, with openat, so that
// a path cannot escape the root. That is the whole point of the type, and it is
// documented as safe for concurrent use. It also means os.Root inherits any
// defect in openat, while plain os.OpenFile -- which hands the assembled path
// to open(2) once -- does not.
//
// On macOS, several goroutines calling Root.OpenFile with O_CREATE on the SAME
// path through one shared *os.Root get "no such file or directory" from most of
// the calls. The same shape through plain os.OpenFile is clean, which is the
// control that tells this apart from a genuinely missing directory.
//
// Usage: go run . [-threads 20] [-trials 50]
// Env:   RACE_DIR=<dir>   parent for the temp dirs (default os.TempDir())
//
// Exit: 0 interpretable (REPRODUCES or CLEAN)
//
//	2 ANOMALOUS -- a control row failed too, so suspect the harness or the
//	  environment rather than the runtime
package main

import (
	"errors"
	"flag"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

type counts struct {
	ok       int
	notExist int // the bug: ENOENT from a call that was asked to create
	exists   int // correct for O_EXCL
	other    int
	sample   error
}

func (c *counts) add(err error) {
	switch {
	case err == nil:
		c.ok++
	case errors.Is(err, fs.ErrNotExist):
		c.notExist++
		if c.sample == nil {
			c.sample = err
		}
	case errors.Is(err, fs.ErrExist):
		c.exists++
	default:
		c.other++
		if c.sample == nil {
			c.sample = err
		}
	}
}

func (c *counts) merge(o counts) {
	c.ok += o.ok
	c.notExist += o.notExist
	c.exists += o.exists
	c.other += o.other
	if c.sample == nil {
		c.sample = o.sample
	}
}

// race releases n goroutines simultaneously into op and tallies the results.
// Every goroutine is parked on the same channel before any of them runs, so
// closing it releases them together.
func race(n int, op func(i int) error) counts {
	var (
		parked sync.WaitGroup
		done   sync.WaitGroup
		mu     sync.Mutex
		out    counts
	)
	start := make(chan struct{})
	parked.Add(n)
	done.Add(n)
	for i := range n {
		go func() {
			defer done.Done()
			parked.Done()
			<-start
			err := op(i)
			mu.Lock()
			out.add(err)
			mu.Unlock()
		}()
	}
	parked.Wait()
	close(start)
	done.Wait()
	return out
}

// fatal ends the run on a setup failure, so that the row functions do not each
// grow an error return that no caller could do anything useful with.
func fatal(err error) {
	fmt.Fprintln(os.Stderr, "setup failed:", err)
	os.Exit(1)
}

func tempDir() string {
	dir, err := os.MkdirTemp(os.Getenv("RACE_DIR"), "openatrace")
	if err != nil {
		fatal(err)
	}
	return dir
}

type row struct {
	label   string
	suspect bool
	// oneWinner marks a row where exactly one success is the correct outcome.
	oneWinner bool
	run       func(n int) counts
}

// --- the rows ------------------------------------------------------------

// rootSameName: n goroutines O_CREATE the same name through one *os.Root.
func rootSameName(name string, excl bool) func(int) counts {
	return func(n int) counts {
		dir := tempDir()
		defer os.RemoveAll(dir)
		if d := filepath.Dir(name); d != "." {
			if err := os.MkdirAll(filepath.Join(dir, d), 0o750); err != nil {
				fatal(err)
			}
		}
		root, err := os.OpenRoot(dir)
		if err != nil {
			fatal(err)
		}
		defer root.Close()

		flags := os.O_RDWR | os.O_CREATE
		if excl {
			flags |= os.O_EXCL
		}
		return race(n, func(int) error {
			f, err := root.OpenFile(name, flags, 0o600)
			if err != nil {
				return err
			}
			return f.Close()
		})
	}
}

// rootDistinctNames: the same thing, but every goroutine creates its own name.
func rootDistinctNames(n int) counts {
	dir := tempDir()
	defer os.RemoveAll(dir)
	root, err := os.OpenRoot(dir)
	if err != nil {
		fatal(err)
	}
	defer root.Close()
	return race(n, func(i int) error {
		f, err := root.OpenFile(fmt.Sprintf("f%03d", i), os.O_RDWR|os.O_CREATE, 0o600)
		if err != nil {
			return err
		}
		return f.Close()
	})
}

// rootMkdirSameName: os.Root.Mkdir is mkdirat, which does not have the fallback.
func rootMkdirSameName(n int) counts {
	dir := tempDir()
	defer os.RemoveAll(dir)
	root, err := os.OpenRoot(dir)
	if err != nil {
		fatal(err)
	}
	defer root.Close()
	return race(n, func(int) error { return root.Mkdir("d", 0o750) })
}

// plainSameName: the control. One assembled path, handed to open(2) once.
func plainSameName(n int) counts {
	dir := tempDir()
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "f")
	return race(n, func(int) error {
		f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o600)
		if err != nil {
			return err
		}
		return f.Close()
	})
}

func main() {
	threads := flag.Int("threads", 20, "goroutines racing per trial")
	trials := flag.Int("trials", 50, "trials per row")
	flag.Parse()
	if *threads < 2 || *trials < 1 {
		fmt.Fprintln(os.Stderr, "-threads must be >= 2 and -trials >= 1")
		os.Exit(1)
	}

	rows := []row{
		{label: `os.Root.OpenFile  "f"    O_CREATE`, suspect: true, run: rootSameName("f", false)},
		{label: `os.Root.OpenFile  "d/f"  O_CREATE`, suspect: true, run: rootSameName("d/f", false)},
		{label: `os.Root.OpenFile  "f"    +O_EXCL `, oneWinner: true, run: rootSameName("f", true)},
		{label: `os.Root.OpenFile  distinct names `, run: rootDistinctNames},
		{label: `os.Root.Mkdir     "d"            `, oneWinner: true, run: rootMkdirSameName},
		{label: `os.OpenFile       full path      `, run: plainSameName},
	}

	total := *threads * *trials
	fmt.Printf("openatrace: %s %s/%s, %d goroutines x %d trials = %d calls per row\n",
		runtime.Version(), runtime.GOOS, runtime.GOARCH, *threads, *trials, total)
	fmt.Printf("temp dirs under: %s\n\n", orElse(os.Getenv("RACE_DIR"), os.TempDir()))
	fmt.Printf("  %-34s %8s %8s %8s %6s\n", "row", "ok", "ENOENT", "EEXIST", "other")

	var suspectNotExist, controlNotExist, otherErrs, suspectCalls int
	var suspectSample, controlSample error
	for _, r := range rows {
		var c counts
		for range *trials {
			c.merge(r.run(*threads))
		}
		fmt.Printf("  %-34s %8d %8d %8d %6d\n", r.label, c.ok, c.notExist, c.exists, c.other)
		if c.sample != nil {
			fmt.Printf("  %-34s   sample: %v\n", "", c.sample)
		}
		otherErrs += c.other
		if r.suspect {
			suspectCalls += total
			suspectNotExist += c.notExist
			if suspectSample == nil {
				suspectSample = c.sample
			}
		} else {
			controlNotExist += c.notExist
			if controlSample == nil {
				controlSample = c.sample
			}
		}
	}

	verdict, rc := "CLEAN", 0
	switch {
	case controlNotExist > 0 || otherErrs > 0:
		verdict, rc = "ANOMALOUS", 2
	case suspectNotExist > 0:
		verdict, rc = "REPRODUCES", 0
	}

	fmt.Printf("\nSUMMARY go=%s platform=%s/%s threads=%d trials=%d calls_per_row=%d"+
		" suspect_calls=%d suspect_enoent=%d control_enoent=%d other_errors=%d verdict=%s\n",
		strings.TrimPrefix(runtime.Version(), "go"), runtime.GOOS, runtime.GOARCH,
		*threads, *trials, total, suspectCalls, suspectNotExist, controlNotExist,
		otherErrs, verdict)
	fmt.Print("VERDICT: ")
	switch rc {
	case 0:
		if suspectNotExist > 0 {
			fmt.Printf("REPRODUCES -- %d of %d os.Root.OpenFile(O_CREATE) calls on a shared\n"+
				"         path returned a not-exist error; every control row clean\n",
				suspectNotExist, suspectCalls)
		} else {
			fmt.Printf("CLEAN -- no not-exist errors at this size; every control row clean\n")
		}
	case 2:
		fmt.Printf("ANOMALOUS -- %d control-row not-exist errors and %d unexpected errors.\n"+
			"         A control failing means the harness or the environment is at fault,\n"+
			"         not the runtime. Investigate before reporting anything.\n",
			controlNotExist, otherErrs)
	}
	os.Exit(rc)
}

func orElse(a, b string) string {
	if a != "" {
		return a
	}
	return b
}
