// SPDX-License-Identifier: Apache-2.0

package psmsgr_test

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/ps-solucoes/ps-msgr/bindings/go/psmsgr"
)

// bothWays runs f with waitsets and with a thread per reader, the fallback
// without futex_waitv. Under qemu-user both are the fallback.
func bothWays(t *testing.T, f func(t *testing.T)) {
	t.Run("waitset", f)
	t.Run("thread per reader", func(t *testing.T) {
		defer psmsgr.SetThreadPerReader(true)()
		f(t)
	})
}

// result receives a WaitChan result, failing after 10 s.
func result(t *testing.T, ch <-chan psmsgr.WaitResult) psmsgr.WaitResult {
	t.Helper()
	select {
	case res := <-ch:
		return res
	case <-time.After(10 * time.Second):
		t.Fatal("WaitChan delivered nothing")
		return psmsgr.WaitResult{}
	}
}

// changed receives a result that must be a change.
func changed(t *testing.T, ch <-chan psmsgr.WaitResult) uint32 {
	t.Helper()
	res := result(t, ch)
	if res.Err != nil || !res.Changed {
		t.Fatalf("result %+v", res)
	}
	return res.Generation
}

// pending checks that nothing has been delivered on ch, after a moment.
func pending(t *testing.T, ch <-chan psmsgr.WaitResult) {
	t.Helper()
	select {
	case res := <-ch:
		t.Fatalf("unexpected result %+v", res)
	case <-time.After(50 * time.Millisecond):
	}
}

func TestWaitChanWakesOnPublish(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
		r := c.reader()
		ctx := context.Background()
		gen := must[uint32](t)(w.Publish([]byte("a")))
		// A value that differs from lastGeneration is delivered at once.
		if g := changed(t, r.WaitChan(ctx, 0, psmsgr.NoTimeout)); g != gen {
			t.Fatalf("generation %d, want %d", g, gen)
		}
		if g := changed(t, r.WaitChan(ctx, genAfter(gen, 1), 0)); g != gen {
			t.Fatalf("generation %d, want %d", g, gen)
		}
		// Follows the publishes, re-armed with the delivered generation.
		for i := 1; i <= 5; i++ {
			ch := r.WaitChan(ctx, gen, 10*time.Second)
			pending(t, ch)
			must[uint32](t)(w.Publish([]byte("b")))
			want := genAfter(gen, 1)
			if gen = changed(t, ch); gen != want {
				t.Fatalf("generation %d, want %d", gen, want)
			}
			// The reader is the caller's again.
			if info := ok[psmsgr.Info](t)(r.Peek()); info.Generation != gen {
				t.Fatalf("peek %d, delivered %d", info.Generation, gen)
			}
		}
	})
}

func TestWaitChanAttachesLater(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		r := c.reader()
		ch := r.WaitChan(context.Background(), 0, 10*time.Second)
		pending(t, ch)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
		gen := must[uint32](t)(w.Publish([]byte("a")))
		if g := changed(t, ch); g != gen {
			t.Fatalf("generation %d, want %d", g, gen)
		}
		// The wait doesn't consume the attach: the read reports it.
		if _, info, _, _ := r.Read(nil); !info.Attached {
			t.Fatal("not attached")
		}
	})
}

func TestWaitChanTimeouts(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
		r := c.reader()
		ctx := context.Background()
		if res := result(t, r.WaitChan(ctx, 0, 0)); res != (psmsgr.WaitResult{}) { // no value: unchanged
			t.Fatalf("result %+v", res)
		}
		gen := must[uint32](t)(w.Publish([]byte("a")))
		if res := result(t, r.WaitChan(ctx, gen, 0)); res != (psmsgr.WaitResult{}) {
			t.Fatalf("result %+v", res)
		}
		for _, timeout := range []time.Duration{120 * time.Millisecond, time.Nanosecond} {
			start := time.Now()
			if res := result(t, r.WaitChan(ctx, gen, timeout)); res != (psmsgr.WaitResult{}) {
				t.Fatalf("result %+v", res)
			}
			// Below a millisecond: rounded up, not a poll.
			if elapsed := time.Since(start); elapsed < max(timeout, time.Millisecond) {
				t.Fatalf("WaitChan(%v) delivered after %v", timeout, elapsed)
			}
		}
		absent := must[*psmsgr.Reader](t)(psmsgr.OpenReader("absent", c.dir))
		defer absent.Close()
		start := time.Now()
		if res := result(t, absent.WaitChan(ctx, 0, 50*time.Millisecond)); res != (psmsgr.WaitResult{}) ||
			time.Since(start) < 50*time.Millisecond {
			t.Fatalf("result %+v after %v", res, time.Since(start))
		}

		// The longest timeout still waits (for a publish here).
		ch := r.WaitChan(ctx, gen, time.Duration(1<<63-1))
		pending(t, ch)
		must[uint32](t)(w.Publish([]byte("b")))
		changed(t, ch)
	})
}

func TestWaitChanContext(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
		r := c.reader()
		gen := must[uint32](t)(w.Publish([]byte("a")))

		canceled, cancel := context.WithCancel(context.Background())
		cancel()
		for _, timeout := range []time.Duration{0, psmsgr.NoTimeout} {
			if res := result(t, r.WaitChan(canceled, 0, timeout)); !errors.Is(res.Err, context.Canceled) {
				t.Fatalf("result %+v", res)
			}
		}

		// A deadline ends the wait on time, with any timeout.
		for _, timeout := range []time.Duration{psmsgr.NoTimeout, 30 * time.Second} {
			start := time.Now()
			ctx, cancel := context.WithTimeout(context.Background(), 150*time.Millisecond)
			res := result(t, r.WaitChan(ctx, gen, timeout))
			elapsed := time.Since(start)
			cancel()
			if !errors.Is(res.Err, context.DeadlineExceeded) || res.Changed {
				t.Fatalf("result %+v", res)
			}
			if elapsed < 150*time.Millisecond || elapsed > 2*time.Second {
				t.Fatalf("delivered after %v", elapsed)
			}
		}

		// A cancel from another goroutine stops it.
		ctx, cancel := context.WithCancel(context.Background())
		start := time.Now()
		time.AfterFunc(150*time.Millisecond, cancel)
		if res := result(t, r.WaitChan(ctx, gen, psmsgr.NoTimeout)); !errors.Is(res.Err, context.Canceled) {
			t.Fatalf("result %+v", res)
		}
		if elapsed := time.Since(start); elapsed < 150*time.Millisecond || elapsed > 2*time.Second {
			t.Fatalf("delivered after %v", elapsed)
		}

		// A cancelable wait still ends on its timeout, and on a publish.
		ctx, cancel = context.WithCancel(context.Background())
		defer cancel()
		start = time.Now()
		if res := result(t, r.WaitChan(ctx, gen, 250*time.Millisecond)); res != (psmsgr.WaitResult{}) ||
			time.Since(start) < 250*time.Millisecond {
			t.Fatalf("result %+v after %v", res, time.Since(start))
		}
		ch := r.WaitChan(ctx, gen, psmsgr.NoTimeout)
		time.AfterFunc(150*time.Millisecond, func() { w.Publish([]byte("b")) })
		changed(t, ch)
	})
}

// In a select, with a cancel to take the reader back.
func TestWaitChanSelect(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
		r := c.reader()
		gen := must[uint32](t)(w.Publish([]byte("a")))
		ctx, cancel := context.WithCancel(context.Background())
		ch := r.WaitChan(ctx, gen, psmsgr.NoTimeout)
		select {
		case res := <-ch:
			t.Fatalf("result %+v", res)
		case <-time.After(50 * time.Millisecond):
		}
		cancel()
		if res := result(t, ch); !errors.Is(res.Err, context.Canceled) {
			t.Fatalf("result %+v", res)
		}
		if !must[bool](t)(r.Wait(context.Background(), 0, 0)) {
			t.Fatal("Wait returned false")
		}
	})
}

// The wait owns the reader: the calls that may run concurrently with Close
// fail with ErrState meanwhile.
func TestWaitChanOwnsReader(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
		r := c.reader()
		gen := must[uint32](t)(w.Publish([]byte("a")))
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		ch := r.WaitChan(ctx, gen, psmsgr.NoTimeout)
		_, err := r.Wait(context.Background(), 0, 0)
		wantCode(t, err, psmsgr.ErrState)
		_, err = r.WriterAlive()
		wantCode(t, err, psmsgr.ErrState)
		wantCode(t, result(t, r.WaitChan(ctx, 0, 0)).Err, psmsgr.ErrState)
		pending(t, ch)
		must[uint32](t)(w.Publish([]byte("b")))
		changed(t, ch)
		if !must[bool](t)(r.WriterAlive()) {
			t.Fatal("no writer")
		}
	})
}

func TestCloseStopsWaitChan(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{})
		gen := must[uint32](t)(w.Publish([]byte("a")))
		for _, timeout := range []time.Duration{psmsgr.NoTimeout, 30 * time.Second} {
			r := must[*psmsgr.Reader](t)(psmsgr.OpenReader(chanName, c.dir))
			ch := r.WaitChan(context.Background(), gen, timeout)
			pending(t, ch)
			start := time.Now()
			r.Close()
			if res := result(t, ch); !errors.Is(res.Err, psmsgr.ErrClosed) {
				t.Fatalf("result %+v", res)
			}
			if elapsed := time.Since(start); elapsed > 2*time.Second {
				t.Fatalf("delivered after %v", elapsed)
			}
			if res := result(t, r.WaitChan(context.Background(), gen, timeout)); !errors.Is(res.Err, psmsgr.ErrClosed) {
				t.Fatalf("result %+v", res)
			}
			if _, err := r.Wait(context.Background(), gen, 0); !errors.Is(err, psmsgr.ErrClosed) {
				t.Fatalf("Wait: %v", err)
			}
		}
	})
}

func TestWaitChanNotSupportedWithoutNotify(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2, NoNotify: true})
		r := c.reader()
		must[uint32](t)(w.Publish([]byte("a")))
		res := result(t, r.WaitChan(context.Background(), 0, 100*time.Millisecond))
		if e := wantCode(t, res.Err, psmsgr.ErrNotSup); e.Op != "wait" || e.Channel != chanName || res.Changed {
			t.Fatalf("result %+v", res)
		}
	})
}

// More readers than a waitset holds, on several channels.
func TestWaitChanManyReaders(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		n := 2*psmsgr.WaitsetMax + 10
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
		other := must[*psmsgr.Writer](t)(psmsgr.OpenWriter("other", 8, &psmsgr.WriterOptions{Dir: c.dir}))
		defer other.Close()
		gen := must[uint32](t)(w.Publish([]byte("a")))
		otherGen := must[uint32](t)(other.Publish([]byte("a")))
		ctx := context.Background()
		chans := make([]<-chan psmsgr.WaitResult, n)
		for i := range n {
			name, last := chanName, gen
			if i%2 == 1 {
				name, last = "other", otherGen
			}
			r := must[*psmsgr.Reader](t)(psmsgr.OpenReader(name, c.dir))
			defer r.Close()
			chans[i] = r.WaitChan(ctx, last, 10*time.Second)
		}
		if psmsgr.UsesWaitsets() && psmsgr.Waitsets() < 3 {
			t.Fatalf("%d readers in %d waitsets", n, psmsgr.Waitsets())
		}
		pending(t, chans[0])
		must[uint32](t)(w.Publish([]byte("b")))
		must[uint32](t)(other.Publish([]byte("b")))
		for i, ch := range chans {
			want := genAfter(gen, 1)
			if i%2 == 1 {
				want = genAfter(otherGen, 1)
			}
			if g := changed(t, ch); g != want {
				t.Fatalf("reader %d: generation %d, want %d", i, g, want)
			}
		}
	})
}

// Waits that end by publish, timeout, cancel and Close at the same time
// deliver exactly one result each.
func TestWaitChanRaces(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
		gen := must[uint32](t)(w.Publish([]byte("a")))
		const n = 64
		var results sync.WaitGroup
		for round := range 5 {
			readers := make([]*psmsgr.Reader, n)
			chans := make([]<-chan psmsgr.WaitResult, n)
			cancels := make([]context.CancelFunc, n)
			for i := range n {
				readers[i] = must[*psmsgr.Reader](t)(psmsgr.OpenReader(chanName, c.dir))
				var ctx context.Context
				ctx, cancels[i] = context.WithCancel(context.Background())
				chans[i] = readers[i].WaitChan(ctx, gen, 20*time.Millisecond)
			}
			time.Sleep(time.Duration(15+round) * time.Millisecond)
			results.Add(n)
			for i := range n {
				go func() {
					defer results.Done()
					switch i % 3 {
					case 0:
						readers[i].Close()
					case 1:
						cancels[i]()
					}
				}()
			}
			gen = must[uint32](t)(w.Publish([]byte("b")))
			results.Wait()
			for i, ch := range chans {
				res := result(t, ch)
				switch {
				case res.Changed && res.Err == nil && res.Generation != 0:
				case !res.Changed && res.Err == nil: // timeout
				case i%3 == 0 && errors.Is(res.Err, psmsgr.ErrClosed):
				case i%3 == 1 && errors.Is(res.Err, context.Canceled):
				default:
					t.Fatalf("reader %d: result %+v", i, res)
				}
			}
			time.Sleep(30 * time.Millisecond)
			for i, ch := range chans {
				select {
				case res := <-ch:
					t.Fatalf("reader %d: second result %+v", i, res)
				default:
				}
				readers[i].Close()
				cancels[i]()
			}
		}
	})
}

// A publisher and readers that follow it, re-armed with each delivered
// generation, as an event loop would.
func TestWaitChanFollowsPublisher(t *testing.T) {
	bothWays(t, func(t *testing.T) {
		c := newChannel(t)
		w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
		const publishes, n = 300, 8
		var last uint32
		done := make(chan error, n)
		start := make(chan struct{})
		for range n {
			r := c.reader()
			go func() {
				var seen uint32
				<-start
				for {
					res := <-r.WaitChan(context.Background(), seen, 10*time.Second)
					if res.Err != nil || !res.Changed {
						done <- errors.New("result " + strconv.Quote(errString(res.Err)))
						return
					}
					seen = res.Generation
					var buf [8]byte
					data, _, ok, err := r.Read(buf[:0])
					if err != nil || !ok {
						done <- err
						return
					}
					if bytes.Equal(data, []byte("last")) {
						done <- nil
						return
					}
				}
			}()
		}
		close(start)
		for i := range publishes {
			v := []byte("v")
			if i == publishes-1 {
				v = []byte("last")
			}
			last = must[uint32](t)(w.Publish(v))
			if i%16 == 0 {
				time.Sleep(time.Millisecond)
			}
		}
		for range n {
			select {
			case err := <-done:
				if err != nil {
					t.Fatal(err)
				}
			case <-time.After(20 * time.Second):
				t.Fatalf("a reader did not see generation %d", last)
			}
		}
	})
}

func errString(err error) string {
	if err == nil {
		return "<nil>"
	}
	return err.Error()
}

// noWaitsets waits until every waitset has closed, failing after limit.
func noWaitsets(t *testing.T, limit time.Duration) {
	t.Helper()
	deadline := time.Now().Add(limit)
	for psmsgr.Waitsets() > 0 {
		if time.Now().After(deadline) {
			t.Fatalf("%d waitsets still open", psmsgr.Waitsets())
		}
		time.Sleep(10 * time.Millisecond)
	}
}

// An idle waitset closes, however its last wait ended, and the next wait
// opens another.
func TestWaitChanIdleWaitsetCloses(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	r := c.reader()
	gen := must[uint32](t)(w.Publish([]byte("a")))
	result(t, r.WaitChan(context.Background(), gen, time.Millisecond))
	if !psmsgr.UsesWaitsets() {
		t.Skip("no futex_waitv: a thread per reader")
	}
	// This wakes the sets open, to start the shorter idle timeout.
	defer psmsgr.SetWaitsetIdle(20 * time.Millisecond)()
	noWaitsets(t, 10*time.Second)

	// From here on nothing wakes the sets but the waits: a set whose last
	// wait ends without an event must still start its idle timeout.
	t.Run("timeout", func(t *testing.T) {
		result(t, r.WaitChan(context.Background(), gen, 30*time.Millisecond))
		noWaitsets(t, 2*time.Second)
	})
	t.Run("ctx", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		ch := r.WaitChan(ctx, gen, psmsgr.NoTimeout)
		pending(t, ch)
		cancel()
		result(t, ch)
		noWaitsets(t, 2*time.Second)
	})
	t.Run("close", func(t *testing.T) {
		r := c.reader()
		ch := r.WaitChan(context.Background(), gen, psmsgr.NoTimeout)
		pending(t, ch)
		r.Close()
		result(t, ch)
		noWaitsets(t, 2*time.Second)
	})
	t.Run("several sets", func(t *testing.T) {
		chans := make([]<-chan psmsgr.WaitResult, psmsgr.WaitsetMax+1)
		for i := range chans {
			r := c.reader()
			chans[i] = r.WaitChan(context.Background(), gen, time.Second)
		}
		if psmsgr.Waitsets() != 2 {
			t.Fatalf("%d waitsets", psmsgr.Waitsets())
		}
		for _, ch := range chans {
			result(t, ch)
		}
		noWaitsets(t, 2*time.Second)
	})

	ch := r.WaitChan(context.Background(), gen, 10*time.Second)
	if psmsgr.Waitsets() != 1 {
		t.Fatalf("%d waitsets", psmsgr.Waitsets())
	}
	time.Sleep(50 * time.Millisecond) // longer than idle: a set with a reader stays
	must[uint32](t)(w.Publish([]byte("b")))
	changed(t, ch)
}

// The event and the constant as cgo sees them are the C compiler's.
func TestWaitsetLayout(t *testing.T) {
	build := os.Getenv("PSMSGR_BUILD_DIR")
	if build == "" {
		t.Skip("PSMSGR_BUILD_DIR is not set, so there is no build tree with tests/interop_helper")
	}
	out := must[[]byte](t)(exec.Command(filepath.Join(build, "tests", "interop_helper"), "layout").Output())
	got := map[string]uint64{}
	lines := bufio.NewScanner(bytes.NewReader(out))
	for lines.Scan() {
		f := strings.Fields(lines.Text())
		if len(f) == 3 {
			got[f[0]+" "+f[1]], _ = strconv.ParseUint(f[2], 10, 64)
		}
	}
	want := psmsgr.WaitsetLayout()
	if len(want) != 7 {
		t.Fatalf("layout %v", want)
	}
	for k, v := range want {
		if g, ok := got[k]; !ok || g != v {
			t.Errorf("%s: interop_helper %d (present %v), cgo %d", k, g, ok, v)
		}
	}
}
