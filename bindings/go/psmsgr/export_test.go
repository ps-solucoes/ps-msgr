// SPDX-License-Identifier: Apache-2.0

package psmsgr

import "time"

// The waitset internals, for the tests in package psmsgr_test.

const WaitsetMax = waitsetMax

// WaitsetLayout returns "sizeof|offsetof key" -> value, as
// tests/interop_helper layout prints it.
func WaitsetLayout() map[string]uint64 {
	m := map[string]uint64{"const PSMSGR_WAITSET_MAX": waitsetMax}
	for k, v := range eventLayout() {
		m[k] = uint64(v)
	}
	return m
}

// SetThreadPerReader makes WaitChan use a goroutine per wait, as without
// futex_waitv, until restore.
func SetThreadPerReader(on bool) (restore func()) {
	old := noWaitset.Swap(on)
	return func() { noWaitset.Store(old) }
}

// SetWaitsetIdle sets how long an idle set's goroutine lives, until restore.
func SetWaitsetIdle(d time.Duration) (restore func()) {
	old := waitsetIdle.Swap(int64(d))
	wakeWaitsets()
	return func() { waitsetIdle.Store(old) }
}

// HoldWaitsets keeps every set's goroutine out of psmsgr_waitset_wait, so
// that nothing scans the readers, until release. held receives once per
// goroutine that stops.
func HoldWaitsets() (held <-chan struct{}, release func()) {
	ch := make(chan struct{}, 64)
	stop := make(chan struct{})
	f := func() {
		select {
		case ch <- struct{}{}:
		default:
		}
		<-stop
	}
	beforeWaitsetWait.Store(&f)
	wakeWaitsets()
	return ch, func() {
		beforeWaitsetWait.Store(nil)
		close(stop)
	}
}

// UsesWaitsets reports whether WaitChan uses waitsets: known after its
// first wait with a timeout.
func UsesWaitsets() bool {
	sets.mu.Lock()
	defer sets.mu.Unlock()
	return !sets.notsup && !noWaitset.Load()
}

// Waitsets returns the number of sets open.
func Waitsets() int {
	sets.mu.Lock()
	defer sets.mu.Unlock()
	return len(sets.list)
}
