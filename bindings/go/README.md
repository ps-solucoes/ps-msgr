# ps-msgr for Go

`psmsgr` is a cgo binding of `libpsmsgr`: state channels publish the latest
value of a payload from one writer to many readers through shared memory.
Go 1.24 or later (trixie ships 1.24), no dependencies. The interface is
specified in [`spec/bindings.md`](../../spec/bindings.md).

## Install

```sh
go get github.com/ps-solucoes/ps-msgr/bindings/go/psmsgr
```

The package links `libpsmsgr` at build time, like a C program, so building
needs cgo and the `libpsmsgr-dev` package (headers, `libpsmsgr.so`,
`libpsmsgr.a`), and the executable needs `libpsmsgr.so.1` (the `libpsmsgr1`
package) at run time. For a library that isn't installed, such as a build
tree, point cgo and the loader at it:

```sh
export CGO_CFLAGS="-I$PWD/include" CGO_LDFLAGS="-L$PWD/build/release"
export LD_LIBRARY_PATH=$PWD/build/release   # at run time
```

The build tag `psmsgr_static` links `libpsmsgr.a` into the executable
instead, which then needs nothing but glibc on the device:

```sh
go build -tags psmsgr_static
```

Unlike the Python and C# bindings, the package doesn't read
`PSMSGR_LIBRARY`: the dynamic linker loads the library before any Go code
runs. `LD_LIBRARY_PATH` does that job.

## Usage

```go
import "github.com/ps-solucoes/ps-msgr/bindings/go/psmsgr"

const MotorStatusV1 = 0x0001_0001 // schema 1, version 1 (payload type)

// Mirrors the C struct motor_status: same fields, same offsets, 24 bytes.
// The padding field is needed on 32-bit ARM, where Go aligns a uint64 to
// 4 bytes but C to 8; without it the struct is 20 bytes there.
type MotorStatus struct {
	Sequence     uint64
	SpeedRpm     float32
	CurrentA     float32
	TemperatureC float32
	_            uint32
}

// Writer
w, err := psmsgr.OpenWriter("motor", 24, &psmsgr.WriterOptions{PayloadType: MotorStatusV1})
if err != nil {
	return err
}
defer w.Close()
status := MotorStatus{Sequence: 1, SpeedRpm: 1500, CurrentA: 2.5, TemperatureC: 41}
gen, err := psmsgr.PublishValue(w, &status)

// Reader: may start before the writer
r, err := psmsgr.OpenReader("motor", "")
if err != nil {
	return err
}
defer r.Close()
var seen uint32
for {
	changed, err := r.Wait(ctx, seen, 500*time.Millisecond) // false after 0.5 s without a change
	if err != nil {
		return err
	}
	if !changed {
		continue
	}
	var s MotorStatus
	info, ok, err := psmsgr.ReadValue(r, &s) // copies straight into the struct
	if err != nil {
		return err
	}
	if !ok {
		continue
	}
	if info.Attached {
		if desc, _, err := r.Describe(); err != nil || desc.PayloadType != MotorStatusV1 {
			return errors.New("unexpected payload type")
		}
	}
	seen = info.Generation
	fmt.Println(s.Sequence, s.SpeedRpm)
}
```

- Payloads are plain structs: `PublishValue` and `ReadValue` copy the raw
  `unsafe.Sizeof` bytes of any type without pointers (they refuse others
  with `ErrInval`). Use fixed-width fields (`int32`, not `int`) and explicit
  padding, so that the layout matches the C definition on every
  architecture. `ReadValue` fails with `ErrMismatch` if the payload has
  another length.
- For bytes: `Publish([]byte)`, and `Read(dst)`, which appends the value to
  `dst` like `append`: reusing the result (`buf, info, ok, err =
  r.Read(buf[:0])`) reads without allocating.
- Zero-copy publish: `PublishFunc(func(buf []byte) (int, error))` builds
  the value in the channel's slot and publishes the length it returns; an
  error or a panic aborts it. It pays off only when the value is built in
  `buf` and is a few KiB or larger; otherwise use `Publish`.
- `Peek` returns the generation, length and timestamp without copying.
  `info.Age()` is the value's age on `CLOCK_MONOTONIC` (`NowNs()`).
- No value yet is not an error: the reads return `ok == false`.
- `Wait` takes a `context.Context`, and `psmsgr.NoTimeout` waits until a
  change or the end of the context.
- `WaitChan` is the same wait for a `select`: it returns at once, and its
  channel delivers one `WaitResult` (`Changed`, the `Generation` to wait
  from next, or `Err`). Until then the reader belongs to the wait: cancel
  the context (then receive) or `Close` the reader to take it back. The
  waits share one goroutine per 127 readers (a libpsmsgr waitset), which
  ends after 10 s without waits; without `futex_waitv` (Linux < 5.16,
  qemu-user) each wait gets a goroutine of its own.

  ```go
  var ch <-chan psmsgr.WaitResult // the wait in progress, if any
  for {
      if ch == nil {
          ch = r.WaitChan(ctx, seen, psmsgr.NoTimeout)
      }
      select {
      case res := <-ch:
          ch = nil // the reader is ours again
          if res.Err != nil {
              return res.Err
          }
          seen = res.Generation // then read
      case cmd := <-commands:
          handle(cmd) // the wait goes on: don't use r here
      }
  }
  ```

- Channels live in `/dev/shm` unless `WriterOptions.Dir` (or the `dir`
  argument) or `$PSMSGR_DIR` says otherwise.
- Errors are `*psmsgr.Error`s: `Code` is the library's result code, `Errno`
  the system error for `ErrSys`. `errors.Is(err, psmsgr.ErrWriterExists)`
  and `errors.Is(err, fs.ErrNotExist)` work. `ErrBusy` is transient: retry.
- Handles are not safe for concurrent use: use one per goroutine. The
  exception: closing a reader from another goroutine stops its `Wait` with
  `ErrClosed` within 100 ms. A handle that is garbage collected without
  `Close` is closed then.
- The hot path (`Publish`, `PublishValue`, `PublishFunc`, `Read` into a
  reused buffer, `ReadValue`, `Peek`) allocates nothing; the tests check
  it. Each call is one cgo call.

## For the BeagleBone Black

Cross-build with Debian's armhf toolchain and the armhf `libpsmsgr-dev` (in
the build container, `build/armhf-release` has the library):

```sh
CGO_ENABLED=1 GOOS=linux GOARCH=arm GOARM=7 CC=arm-linux-gnueabihf-gcc \
    CGO_CFLAGS="-I$PWD/include" CGO_LDFLAGS="-L$PWD/build/armhf-release" \
    go build -tags psmsgr_static ./cmd/yourapp
```

## Development

The tests run against a build of the C library. In the build container:

```sh
docker/run.sh cmake --workflow --preset release
docker/run.sh bindings/go/check.sh
```

`check.sh` checks `gofmt` and `go vet`, and runs the tests linked
dynamically, with the race detector and linked statically, and the
benchmarks for a few iterations. If `build/armhf-release` has been built
(`cmake --preset armhf-release && cmake --build --preset armhf-release`),
it also cross-builds the tests for `linux/arm` and runs them under
`qemu-arm`. `go test -bench .` in `psmsgr/` measures the per-call cost.
