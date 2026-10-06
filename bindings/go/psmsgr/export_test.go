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
