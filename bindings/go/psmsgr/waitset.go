// SPDX-License-Identifier: Apache-2.0

package psmsgr

// #cgo noescape psmsgr_waitset_open
// #cgo nocallback psmsgr_waitset_open
// #cgo nocallback psmsgr_waitset_close
// #cgo nocallback psmsgr_waitset_add
// #cgo nocallback psmsgr_waitset_remove
// #cgo noescape psmsgr_waitset_wait
// #cgo nocallback psmsgr_waitset_wait
// #cgo nocallback psmsgr_waitset_wake
// #include <psmsgr/psmsgr.h>
import "C"

import (
	"context"
	"runtime"
	"slices"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// Readers per waitset.
const waitsetMax = C.PSMSGR_WAITSET_MAX

// eventLayout is psmsgr_waitset_event as cgo sees it, which the tests
// compare with tests/interop_helper layout.
func eventLayout() map[string]uintptr {
	var e C.psmsgr_waitset_event
	return map[string]uintptr{
		"sizeof psmsgr_waitset_event":              unsafe.Sizeof(e),
		"offsetof psmsgr_waitset_event.token":      unsafe.Offsetof(e.token),
		"offsetof psmsgr_waitset_event.status":     unsafe.Offsetof(e.status),
		"offsetof psmsgr_waitset_event.generation": unsafe.Offsetof(e.generation),
		"offsetof psmsgr_waitset_event.sys_errno":  unsafe.Offsetof(e.sys_errno),
		"offsetof psmsgr_waitset_event.reserved":   unsafe.Offsetof(e.reserved),
	}
}

// WaitResult is the result of [Reader.WaitChan]: what [Reader.Wait] would
// return, plus the generation.
type WaitResult struct {
	// Changed is true when the generation differs from lastGeneration, and
	// false when the timeout passed.
	Changed bool
	// Generation is the generation when Changed: the lastGeneration for the
	// next wait. 0 (no value) if the value was gone again by then.
	Generation uint32
	Err        error
}

// WaitChan is [Reader.Wait] for a select: it returns at once, and the
// channel delivers one result. The arguments, the results and the errors
// are Wait's.
//
// Until the result is received, the reader belongs to the wait: the only
// call allowed on it is Close, which ends the wait with ErrClosed. Wait,
// WriterAlive and another WaitChan fail with ErrState meanwhile; the other
// calls must not be made. To stop a wait, cancel ctx, then receive.
//
// The waits share waitsets (one per 127 readers), each served by a
// goroutine that the first wait starts and that ends after 10 s without
// waits. Where the kernel lacks futex_waitv (Linux < 5.16, qemu-user),
// each wait runs on a goroutine of its own instead, which ctx and Close
// stop within 100 ms, as they stop Wait. The goroutines don't keep the
// program from exiting.
func (r *Reader) WaitChan(ctx context.Context, lastGeneration uint32, timeout time.Duration) <-chan WaitResult {
	ch := make(chan WaitResult, 1)
	h, err := r.acquire("wait", true)
	if err != nil {
		ch <- WaitResult{Err: err}
		return ch
	}
	w := &asyncWait{r: r, h: h, last: lastGeneration, ch: ch}
	if err := ctx.Err(); err != nil {
		w.finish(WaitResult{Err: err})
		return ch
	}
	timeout = roundTimeout(timeout)
	if timeout == 0 { // a poll needs no thread
		w.finish(r.waitGeneration(ctx, h, lastGeneration, 0))
		return ch
	}
	registered, err := register(w, ctx, timeout)
	switch {
	case err != nil:
		w.finish(WaitResult{Err: err})
	case !registered: // no futex_waitv: a goroutine per wait
		go func() { w.finish(r.waitGeneration(ctx, h, lastGeneration, timeout)) }()
	default:
		// Either Close sees the waiter and cancels it, or this sees the Close.
		r.mu.Lock()
		closed := r.closed
		if !closed {
			r.waiter = w
		}
		r.mu.Unlock()
		if closed {
			w.cancel(WaitResult{Err: ErrClosed})
		}
	}
	return ch
}

// waitGeneration is Reader.wait with the generation: a thread per reader.
func (r *Reader) waitGeneration(ctx context.Context, h *C.psmsgr_state_reader, lastGeneration uint32,
	timeout time.Duration) WaitResult {
	changed, err := r.wait(ctx, h, lastGeneration, timeout)
	if !changed || err != nil {
		return WaitResult{Err: err}
	}
	var ni C.psmsgr_state_info
	rc, errno := C.psmsgr_state_peek(h, &ni)
	switch rc {
	case C.PSMSGR_OK:
		// The reader is this wait's until the result is received.
		r.pending = r.pending || ni.flags&C.PSMSGR_INFO_ATTACHED != 0
		return WaitResult{Changed: true, Generation: uint32(ni.generation)}
	case C.PSMSGR_E_NODATA:
		return WaitResult{Changed: true}
	}
	return WaitResult{Err: newError("peek", r.name, rc, errno)}
}

// asyncWait is a WaitChan in progress.
type asyncWait struct {
	r    *Reader
	h    *C.psmsgr_state_reader
	last uint32
	ch   chan WaitResult

	// Set under set.mu by register.
	set       *waitset
	token     uint64
	stopTimer func() bool
	stopCtx   func() bool
}

// finish delivers the result once the reader is the caller's again.
func (w *asyncWait) finish(res WaitResult) {
	if w.stopTimer != nil {
		w.stopTimer()
	}
	if w.stopCtx != nil {
		w.stopCtx()
	}
	w.r.releaseAsync(w.h)
	w.ch <- res
}

// cancel ends a registered wait with res (ctx or Close), unless it ended
// already. If the set has reported the reader by now, its event is the
// answer, and the set's goroutine delivers it.
func (w *asyncWait) cancel(res WaitResult) {
	if w.remove() {
		w.finish(res)
	}
}

// expire ends a registered wait at its timeout, unless it ended already.
// The set may not have scanned the reader since a publish, so it checks
// once more, as Wait does at its deadline. The handle is still the wait's.
func (w *asyncWait) expire() {
	if w.remove() {
		w.finish(w.r.waitGeneration(context.Background(), w.h, w.last, 0))
	}
}

// remove takes w's reader out of its set; false if the wait ended already
// or the set has reported it (NODATA).
func (w *asyncWait) remove() bool {
	s := w.set
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.waits[w.token] != w {
		return false
	}
	// May block while the set's wait scans the readers.
	if C.psmsgr_waitset_remove(s.ws, w.h) != C.PSMSGR_OK { // NODATA: reported
		return false
	}
	delete(s.waits, w.token)
	if len(s.waits) == 0 {
		// The set's wait has no timeout while it has readers: wake it so
		// that it starts the idle one. s.mu keeps take, so the close, out.
		C.psmsgr_waitset_wake(s.ws)
	}
	return true
}

// waitset is a psmsgr_waitset and the goroutine that waits on it.
type waitset struct {
	ws    *C.psmsgr_waitset
	mu    sync.Mutex
	waits map[uint64]*asyncWait // by token: registered in ws, not yet reported
}

// The sets in use. Lock order: sets.mu, then a waitset's mu.
var sets struct {
	mu     sync.Mutex
	list   []*waitset
	token  uint64
	notsup bool // psmsgr_waitset_open returned NOTSUP: a thread per reader
}

var (
	// waitsetIdle is how long a set's goroutine waits without registered
	// readers before it closes the set and ends. Tests shorten it.
	waitsetIdle atomic.Int64
	// noWaitset makes WaitChan use a thread per reader, as without
	// futex_waitv. For the tests.
	noWaitset atomic.Bool
	// beforeWaitsetWait, if set, runs on a set's goroutine before each
	// psmsgr_waitset_wait. For the tests.
	beforeWaitsetWait atomic.Pointer[func()]
)

func init() { waitsetIdle.Store(int64(10 * time.Second)) }

// wakeWaitsets makes every set's goroutine look at its timeout again.
func wakeWaitsets() {
	sets.mu.Lock()
	defer sets.mu.Unlock()
	for _, s := range sets.list {
		C.psmsgr_waitset_wake(s.ws)
	}
}

// register adds w to a set with room, opening one if needed, and arms its
// timeout and ctx. false without futex_waitv.
func register(w *asyncWait, ctx context.Context, timeout time.Duration) (bool, error) {
	sets.mu.Lock()
	defer sets.mu.Unlock()
	if sets.notsup || noWaitset.Load() {
		return false, nil
	}
	var s *waitset
	for _, c := range sets.list {
		c.mu.Lock()
		if len(c.waits) < waitsetMax {
			s = c
			break
		}
		c.mu.Unlock()
	}
	if s == nil {
		var ws *C.psmsgr_waitset
		rc, errno := C.psmsgr_waitset_open(&ws)
		if rc == C.PSMSGR_E_NOTSUP {
			sets.notsup = true
			return false, nil
		}
		if rc != C.PSMSGR_OK {
			return true, newError("wait", w.r.name, rc, errno)
		}
		s = &waitset{ws: ws, waits: make(map[uint64]*asyncWait)}
		sets.list = append(sets.list, s)
		go s.run()
		s.mu.Lock()
	}
	defer s.mu.Unlock()
	sets.token++
	w.set, w.token = s, sets.token
	// The token is a number, not a Go pointer: C keeps it.
	rc := C.psmsgr_waitset_add(s.ws, w.h, C.uint32_t(w.last), C.uint64_t(w.token))
	if rc != C.PSMSGR_OK {
		return true, newError("wait", w.r.name, rc, nil)
	}
	s.waits[w.token] = w
	// Their callbacks wait for s.mu, so they see the registration.
	if timeout > 0 {
		w.stopTimer = time.AfterFunc(timeout, w.expire).Stop
	}
	w.stopCtx = context.AfterFunc(ctx, func() { w.cancel(WaitResult{Err: ctx.Err()}) })
	return true, nil
}

// run is the set's goroutine: it waits and delivers the events, until the
// set has been idle for waitsetIdle.
func (s *waitset) run() {
	// The set's own thread while it lives: futex_waitv blocks it anyway.
	// Ending locked ends the thread too.
	runtime.LockOSThread()
	var events [waitsetMax]C.psmsgr_waitset_event
	for {
		if f := beforeWaitsetWait.Load(); f != nil {
			(*f)()
		}
		s.mu.Lock()
		timeout := C.int32_t(-1)
		if len(s.waits) == 0 {
			timeout = C.int32_t(time.Duration(waitsetIdle.Load()) / time.Millisecond)
		}
		s.mu.Unlock()
		var n C.uint32_t
		rc, errno := C.psmsgr_waitset_wait(s.ws, timeout, &events[0], C.uint32_t(len(events)), &n)
		switch rc {
		case C.PSMSGR_OK:
			s.dispatch(events[:n])
		case C.PSMSGR_E_TIMEOUT:
			if s.retire() {
				return
			}
		case C.PSMSGR_E_INTR: // a signal handler without SA_RESTART: wait again
		default: // not expected: the set's waits fail with it
			for _, w := range s.drain() {
				w.finish(WaitResult{Err: newError("wait", w.r.name, rc, errno)})
			}
			return
		}
	}
}

// dispatch delivers the reported readers' results.
func (s *waitset) dispatch(events []C.psmsgr_waitset_event) {
	type result struct {
		w   *asyncWait
		res WaitResult
	}
	done := make([]result, 0, len(events))
	s.mu.Lock()
	for i := range events {
		ev := &events[i]
		w := s.waits[uint64(ev.token)]
		if w == nil { // not expected: a removed reader is not reported
			continue
		}
		delete(s.waits, w.token)
		res := WaitResult{Changed: true, Generation: uint32(ev.generation)}
		if ev.status != C.PSMSGR_OK { // NOTSUP, FORMAT or SYS: as from Wait
			res = WaitResult{Err: newError("wait", w.r.name, C.int(ev.status), syscall.Errno(ev.sys_errno))}
		}
		done = append(done, result{w, res})
	}
	s.mu.Unlock()
	for _, d := range done {
		d.w.finish(d.res)
	}
}

// retire takes the set out of use and closes it, if it has no readers.
func (s *waitset) retire() bool {
	if _, ok := s.take(false); !ok {
		return false
	}
	C.psmsgr_waitset_close(s.ws)
	return true
}

// drain closes the set after a failed wait and returns its readers' waits.
func (s *waitset) drain() []*asyncWait {
	taken, _ := s.take(true)
	C.psmsgr_waitset_close(s.ws)
	return taken
}

// take takes the set out of use, if it has no readers or with force, and
// takes the readers out of it. Nothing calls the set afterwards: cancel
// finds no registration, register doesn't find the set.
func (s *waitset) take(force bool) ([]*asyncWait, bool) {
	sets.mu.Lock()
	defer sets.mu.Unlock()
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.waits) > 0 && !force {
		return nil, false
	}
	sets.list = slices.DeleteFunc(sets.list, func(c *waitset) bool { return c == s })
	taken := make([]*asyncWait, 0, len(s.waits))
	for token, w := range s.waits {
		C.psmsgr_waitset_remove(s.ws, w.h) // no wait in progress: at once
		delete(s.waits, token)
		taken = append(taken, w)
	}
	return taken, true
}
