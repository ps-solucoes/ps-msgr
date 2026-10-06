// SPDX-License-Identifier: Apache-2.0

package psmsgr

// #cgo nocallback psmsgr_version
// #cgo nocallback psmsgr_strerror
// #cgo nocallback psmsgr_now_ns
// #cgo nocallback psmsgr_state_unlink
// #include <stdlib.h>
// #include <psmsgr/psmsgr.h>
import "C"

import (
	"fmt"
	"math"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

// The library version the binding needs: this major, at least this minor.
const (
	versionMajor    = 1
	minVersionMinor = 1
)

// Compile-time checks: the header is the major the binding is written for,
// and the Go constants are the header's. A difference fails the build with
// an out-of-range index.
var (
	_ = [1]struct{}{}[C.PSMSGR_VERSION_MAJOR-versionMajor]
	_ = [1]struct{}{}[ErrInval-C.PSMSGR_E_INVAL]
	_ = [1]struct{}{}[ErrSys-C.PSMSGR_E_SYS]
	_ = [1]struct{}{}[ErrNoData-C.PSMSGR_E_NODATA]
	_ = [1]struct{}{}[ErrTooSmall-C.PSMSGR_E_TOOSMALL]
	_ = [1]struct{}{}[ErrTooBig-C.PSMSGR_E_TOOBIG]
	_ = [1]struct{}{}[ErrBusy-C.PSMSGR_E_BUSY]
	_ = [1]struct{}{}[ErrTimeout-C.PSMSGR_E_TIMEOUT]
	_ = [1]struct{}{}[ErrIntr-C.PSMSGR_E_INTR]
	_ = [1]struct{}{}[ErrWriterExists-C.PSMSGR_E_WRITER_EXISTS]
	_ = [1]struct{}{}[ErrMismatch-C.PSMSGR_E_MISMATCH]
	_ = [1]struct{}{}[ErrFormat-C.PSMSGR_E_FORMAT]
	_ = [1]struct{}{}[ErrNotSup-C.PSMSGR_E_NOTSUP]
	_ = [1]struct{}{}[ErrState-C.PSMSGR_E_STATE]
)

// checkLoaded checks the version of the library the executable runs with,
// once: the dynamic linker only guarantees the major (the SONAME).
var checkLoaded = sync.OnceValue(func() error {
	return checkVersion(uint32(C.psmsgr_version()))
})

func checkVersion(v uint32) error {
	major, minor, patch := v>>16, (v>>8)&0xFF, v&0xFF
	if major == versionMajor && minor >= minVersionMinor {
		return nil
	}
	return &Error{Op: "load", Code: ErrNotSup, Msg: fmt.Sprintf(
		"the binding needs libpsmsgr %d.%d or a later %d.x, but libpsmsgr.so.1 is %d.%d.%d",
		versionMajor, minVersionMinor, versionMajor, major, minor, patch)}
}

func strerror(c Code) string {
	return C.GoString(C.psmsgr_strerror(C.int(c)))
}

// NowNs returns CLOCK_MONOTONIC in nanoseconds: the clock of
// Info.TimestampNs, in every process on the host.
func NowNs() uint64 {
	return uint64(C.psmsgr_now_ns())
}

// Unlink retires and deletes the channel name in dir ("": $PSMSGR_DIR, else
// /dev/shm). Readers attached to it see no value until a writer creates it
// again. False if there was no channel; fails with ErrWriterExists while a
// writer holds it.
func Unlink(name, dir string) (bool, error) {
	const op = "unlink"
	if err := checkLoaded(); err != nil {
		return false, err
	}
	cname, err := cString(op, name, name, "name")
	if err != nil {
		return false, err
	}
	defer C.free(unsafe.Pointer(cname))
	cdir, err := cDir(op, name, dir)
	if err != nil {
		return false, err
	}
	defer C.free(unsafe.Pointer(cdir))
	rc, errno := C.psmsgr_state_unlink(cname, cdir)
	switch rc {
	case C.PSMSGR_OK:
		return true, nil
	case C.PSMSGR_E_NODATA:
		return false, nil
	}
	return false, newError(op, name, rc, errno)
}

// newError returns the error for a failed call; errno is the second result
// of a cgo call, used for ErrSys.
func newError(op, channel string, rc C.int, errno error) *Error {
	e := &Error{Op: op, Channel: channel, Code: Code(rc)}
	if e.Code == ErrSys {
		e.Errno, _ = errno.(syscall.Errno)
	}
	return e
}

// cString copies s to C memory, which the caller frees. C would stop at a
// NUL, so a NUL is an error.
func cString(op, channel, s, what string) (*C.char, error) {
	if strings.IndexByte(s, 0) >= 0 {
		return nil, &Error{Op: op, Channel: channel, Code: ErrInval, Msg: what + " contains a NUL character"}
	}
	return C.CString(s), nil
}

// cDir is cString for a directory; "" (the default) is NULL.
func cDir(op, channel, dir string) (*C.char, error) {
	if dir == "" {
		return nil, nil
	}
	return cString(op, channel, dir, "directory")
}

// cLen converts a buffer length to the C API's uint32_t, saturating: a
// buffer larger than 4 GiB is larger than any channel's capacity anyway.
func cLen(n int) C.uint32_t {
	if uint64(n) > math.MaxUint32 {
		return math.MaxUint32
	}
	return C.uint32_t(n)
}
