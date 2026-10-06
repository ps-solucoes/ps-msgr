// SPDX-License-Identifier: Apache-2.0

package psmsgr

// #cgo noescape psmsgr_state_reader_open
// #cgo nocallback psmsgr_state_reader_open
// #cgo nocallback psmsgr_state_reader_close
// #cgo noescape psmsgr_state_read
// #cgo nocallback psmsgr_state_read
// #cgo noescape psmsgr_state_peek
// #cgo nocallback psmsgr_state_peek
// #cgo nocallback psmsgr_state_wait
// #cgo nocallback psmsgr_state_writer_alive
// #cgo noescape psmsgr_state_describe_sized
// #cgo nocallback psmsgr_state_describe_sized
// #include <stdlib.h>
// #include <psmsgr/psmsgr.h>
import "C"

import (
	"context"
	"math"
	"runtime"
	"slices"
	"strconv"
	"sync"
	"time"
	"unsafe"
)

// Info describes a value: the result of a read or a peek.
type Info struct {
	Generation  uint32 // change token, never 0
	Length      uint32 // payload length in bytes
	TimestampNs uint64 // CLOCK_MONOTONIC at publish (NowNs)
	// Attached is set on the first result from a newly (re)attached channel
	// file: the time to check ChannelDesc.PayloadType with Describe.
	Attached bool
}

// Age is how long ago the value was published.
func (i Info) Age() time.Duration {
	return time.Duration(int64(NowNs()) - int64(i.TimestampNs))
}

// ChannelDesc holds the constant properties of an attached channel.
type ChannelDesc struct {
	Capacity    uint32
	SlotCount   uint32
	PayloadType uint32
	Notify      bool // false: created with NoNotify, Wait fails with ErrNotSup
}

// NoTimeout makes [Reader.Wait] wait until a change or the context ends.
const NoTimeout time.Duration = -1

const (
	// A TOOSMALL after growing to the reported length means the value was
	// replaced in between; more than a few in a row is not going to happen.
	readAttempts = 4
	// Wait waits in slices, so that a canceled context or a Close from
	// another goroutine stops it.
	waitSlice = 100 * time.Millisecond
)

// Reader reads a channel. It is not safe for concurrent use, except that
// another goroutine may Close it during Wait, WriterAlive or a WaitChan.
type Reader struct {
	// nil once closed. Only Wait, WriterAlive and WaitChan may run
	// concurrently with Close; they take the handle under mu, and the last
	// of them to return after a Close closes it.
	h       *C.psmsgr_state_reader
	name    string
	pending bool // an attach consumed by a result that failed: reported by the next
	cleanup runtime.Cleanup

	mu     sync.Mutex
	closed bool
	active int // Wait, WriterAlive and WaitChan calls in progress
	// A WaitChan owns the reader until it delivers its result. waiter is
	// its registration in a waitset; nil for a thread per reader.
	async  bool
	waiter *asyncWait
}

// OpenReader opens a reader of the channel name in dir ("": $PSMSGR_DIR,
// else /dev/shm). It succeeds for any valid name: the reader attaches when
// the channel appears, and follows it when it is recreated.
func OpenReader(name, dir string) (*Reader, error) {
	const op = "open reader"
	if err := checkLoaded(); err != nil {
		return nil, err
	}
	cname, err := cString(op, name, name, "name")
	if err != nil {
		return nil, err
	}
	defer C.free(unsafe.Pointer(cname))
	cdir, err := cDir(op, name, dir)
	if err != nil {
		return nil, err
	}
	defer C.free(unsafe.Pointer(cdir))
	var h *C.psmsgr_state_reader
	rc, errno := C.psmsgr_state_reader_open(cname, cdir, &h)
	if rc != C.PSMSGR_OK {
		return nil, newError(op, name, rc, errno)
	}
	r := &Reader{h: h, name: name}
	r.cleanup = runtime.AddCleanup(r, closeReader, h)
	return r, nil
}

func closeReader(h *C.psmsgr_state_reader) { C.psmsgr_state_reader_close(h) }

// Read appends the latest value to dst and returns the extended slice, like
// append: it allocates only if dst lacks the room, so reusing the result as
// the next dst (e.g. buf[:0]) reads without allocating. ok is false if
// there is no value yet; dst is then returned unchanged.
func (r *Reader) Read(dst []byte) (_ []byte, _ Info, ok bool, _ error) {
	h := r.h
	if h == nil {
		return dst, Info{}, false, ErrClosed
	}
	attached := false
	for range readAttempts {
		n := len(dst)
		spare := dst[n:cap(dst)]
		var ni C.psmsgr_state_info
		rc, errno := C.psmsgr_state_read(h, unsafe.Pointer(unsafe.SliceData(spare)), cLen(len(spare)), &ni)
		runtime.KeepAlive(r)
		switch rc {
		case C.PSMSGR_OK:
			attached = r.takeAttached(&ni) || attached
			return dst[:n+int(ni.length)], info(&ni, attached), true, nil
		case C.PSMSGR_E_TOOSMALL:
			attached = r.takeAttached(&ni) || attached
			dst = slices.Grow(dst, int(ni.length))
		case C.PSMSGR_E_NODATA:
			r.pending = r.pending || attached
			return dst, Info{}, false, nil
		default:
			r.pending = r.pending || attached
			return dst, Info{}, false, newError("read", r.name, rc, errno)
		}
	}
	r.pending = r.pending || attached
	return dst, Info{}, false, &Error{Op: "read", Channel: r.name, Code: ErrBusy}
}

// Peek returns the generation, length and timestamp of the latest value
// without copying it, and without syscalls while attached. ok is false if
// there is no value yet.
func (r *Reader) Peek() (_ Info, ok bool, _ error) {
	h := r.h
	if h == nil {
		return Info{}, false, ErrClosed
	}
	var ni C.psmsgr_state_info
	rc, errno := C.psmsgr_state_peek(h, &ni)
	runtime.KeepAlive(r)
	switch rc {
	case C.PSMSGR_OK:
		return info(&ni, r.takeAttached(&ni)), true, nil
	case C.PSMSGR_E_NODATA:
		return Info{}, false, nil
	}
	return Info{}, false, newError("peek", r.name, rc, errno)
}

// Wait blocks until the channel's generation differs from lastGeneration
// (0: until there is any value), and returns true then. It returns false
// when timeout passes: a timeout of 0 checks once, a positive one is
// rounded up to whole milliseconds, and [NoTimeout] (any negative value)
// waits indefinitely. It returns ctx.Err() once ctx ends, and ErrClosed
// once another goroutine closes the reader; it checks both at least every
// 100 ms. It fails with ErrNotSup on a channel created with NoNotify, and
// with ErrState while a [Reader.WaitChan] owns the reader.
func (r *Reader) Wait(ctx context.Context, lastGeneration uint32, timeout time.Duration) (bool, error) {
	h, err := r.acquire("wait", false)
	if err != nil {
		return false, err
	}
	defer r.release(h)
	return r.wait(ctx, h, lastGeneration, roundTimeout(timeout))
}

// roundTimeout rounds a positive timeout up to whole milliseconds, so that
// it never becomes a poll.
func roundTimeout(timeout time.Duration) time.Duration {
	if timeout > 0 && timeout <= math.MaxInt64-time.Millisecond {
		timeout = (timeout + time.Millisecond - 1).Truncate(time.Millisecond)
	}
	return timeout
}

// wait is Wait on h, which the caller has acquired, with a rounded timeout.
func (r *Reader) wait(ctx context.Context, h *C.psmsgr_state_reader, lastGeneration uint32,
	timeout time.Duration) (bool, error) {
	start := time.Now()
	deadline, hasDeadline := ctx.Deadline()
	for {
		if err := ctx.Err(); err != nil {
			return false, err
		}
		if r.isClosed() {
			return false, ErrClosed
		}
		now := time.Now()
		if hasDeadline && !now.Before(deadline) {
			return false, context.DeadlineExceeded
		}
		// last: the slice ends with the timeout.
		slice, last := waitSlice, false
		if timeout >= 0 {
			if rem := timeout - now.Sub(start); rem <= slice {
				slice, last = max(rem, 0), true
			}
		}
		if hasDeadline {
			slice = min(slice, deadline.Sub(now))
		}
		// Rounded up, so that a positive remainder never becomes a poll.
		ms := C.int32_t((slice + time.Millisecond - 1) / time.Millisecond)
		rc, errno := C.psmsgr_state_wait(h, C.uint32_t(lastGeneration), ms)
		switch rc {
		case C.PSMSGR_OK:
			return true, nil
		case C.PSMSGR_E_TIMEOUT:
			if last && (ms == 0 || time.Since(start) >= timeout) {
				return false, nil
			}
		case C.PSMSGR_E_INTR:
			// A signal handler installed without SA_RESTART (Go's own
			// restart): retry with the remaining time.
		default:
			return false, newError("wait", r.name, rc, errno)
		}
	}
}

// WriterAlive reports whether a writer holds the channel now. It makes
// syscalls, and reattaches if the channel file was replaced.
func (r *Reader) WriterAlive() (bool, error) {
	h, err := r.acquire("writer alive", false)
	if err != nil {
		return false, err
	}
	defer r.release(h)
	rc, errno := C.psmsgr_state_writer_alive(h)
	if rc >= 0 {
		return rc == 1, nil
	}
	return false, newError("writer alive", r.name, rc, errno)
}

// Describe returns the attached channel's constant properties; ok is false
// if the reader is not attached (the channel doesn't exist yet).
func (r *Reader) Describe() (_ ChannelDesc, ok bool, _ error) {
	h := r.h
	if h == nil {
		return ChannelDesc{}, false, ErrClosed
	}
	var d C.psmsgr_state_desc
	rc, errno := C.psmsgr_state_describe_sized(h, &d, C.uint32_t(unsafe.Sizeof(d)))
	runtime.KeepAlive(r)
	switch rc {
	case C.PSMSGR_OK:
		return ChannelDesc{
			Capacity:    uint32(d.capacity),
			SlotCount:   uint32(d.slot_count),
			PayloadType: uint32(d.payload_type),
			Notify:      d.flags&C.PSMSGR_STATE_NO_NOTIFY == 0,
		}, true, nil
	case C.PSMSGR_E_NODATA:
		return ChannelDesc{}, false, nil
	}
	return ChannelDesc{}, false, newError("describe", r.name, rc, errno)
}

// Close closes the reader. It is idempotent and returns nil. Another
// goroutine may call it during Wait or WriterAlive: Close returns at once,
// and the native handle closes when that call returns. During a WaitChan,
// Close first takes the reader out of the waitset, which can block for as
// long as the waitset scans its readers, and the wait delivers ErrClosed.
func (r *Reader) Close() error {
	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return nil
	}
	r.closed = true
	r.cleanup.Stop()
	if r.active == 0 {
		C.psmsgr_state_reader_close(r.h)
	}
	r.h = nil
	w := r.waiter
	r.mu.Unlock()
	if w != nil {
		w.cancel(WaitResult{Err: ErrClosed})
	}
	return nil
}

// acquire takes the handle for a call that may run concurrently with Close.
// A WaitChan (async) owns it until releaseAsync; other calls fail meanwhile.
func (r *Reader) acquire(op string, async bool) (*C.psmsgr_state_reader, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return nil, ErrClosed
	}
	if r.async {
		return nil, &Error{Op: op, Channel: r.name, Code: ErrState, Msg: "a WaitChan is in progress"}
	}
	r.active++
	r.async = async
	return r.h, nil
}

// release ends such a call; the last one to end after a Close closes h.
func (r *Reader) release(h *C.psmsgr_state_reader) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.releaseLocked(h)
}

// releaseAsync ends a WaitChan: the reader is the caller's again.
func (r *Reader) releaseAsync(h *C.psmsgr_state_reader) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.async = false
	r.waiter = nil
	r.releaseLocked(h)
}

func (r *Reader) releaseLocked(h *C.psmsgr_state_reader) {
	r.active--
	if r.closed && r.active == 0 {
		C.psmsgr_state_reader_close(h)
	}
}

func (r *Reader) isClosed() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.closed
}

func (r *Reader) takeAttached(ni *C.psmsgr_state_info) bool {
	a := r.pending || ni.flags&C.PSMSGR_INFO_ATTACHED != 0
	r.pending = false
	return a
}

func (r *Reader) mismatch(op string, length uint32, size uintptr, typ string) error {
	return &Error{Op: op, Channel: r.name, Code: ErrMismatch,
		Msg: "payload of " + strconv.FormatUint(uint64(length), 10) + " bytes does not match the " +
			strconv.FormatUint(uint64(size), 10) + "-byte " + typ}
}

func info(ni *C.psmsgr_state_info, attached bool) Info {
	return Info{
		Generation:  uint32(ni.generation),
		Length:      uint32(ni.length),
		TimestampNs: uint64(ni.timestamp_ns),
		Attached:    attached,
	}
}
