// SPDX-License-Identifier: Apache-2.0

// Package psmsgr is a cgo binding of libpsmsgr: state channels publish the
// latest value of a payload from one writer to many readers through a
// shared-memory file.
//
// A [Writer] owns a channel; any number of [Reader]s, in any process and in
// any language with a binding, read its latest value without locks and
// without syscalls. Readers may start before the writer.
//
//	w, err := psmsgr.OpenWriter("motor", 24, &psmsgr.WriterOptions{PayloadType: 0x0001_0001})
//	gen, err := psmsgr.PublishValue(w, &status)
//
//	r, err := psmsgr.OpenReader("motor", "")
//	for {
//		changed, err := r.Wait(ctx, seen, 500*time.Millisecond)
//		info, ok, err := psmsgr.ReadValue(r, &status)
//		...
//	}
//
// The package links libpsmsgr (-lpsmsgr, from the libpsmsgr-dev package)
// at build time, like a C program; the executable then needs
// libpsmsgr.so.1. With the build tag psmsgr_static it links libpsmsgr.a
// instead, and needs no libpsmsgr at run time.
//
// Results: "no value yet" is not an error, but ok == false. Every failure
// is an [*Error] whose Code is the library's result code, so
// errors.Is(err, psmsgr.ErrWriterExists) tells them apart, and
// errors.Is(err, fs.ErrNotExist) works on system errors. [ErrBusy] is
// transient: retry.
//
// [Reader.WaitChan] is the wait for a select: a channel delivers its
// result. The waits share libpsmsgr waitsets, one goroutine per 127
// readers.
//
// Like the C handles, a Writer or Reader is not safe for concurrent use,
// with one exception: another goroutine may Close a Reader during
// [Reader.Wait], [Reader.WriterAlive] or [Reader.WaitChan]. A handle that
// is garbage collected without Close is closed then.
//
// The interface is specified in spec/bindings.md of the ps-msgr repository.
package psmsgr
