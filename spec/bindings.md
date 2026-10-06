# Bindings — Python, C# and Go

Status: **final**.

The bindings are thin wrappers over `libpsmsgr.so.1`. They MUST NOT access
the channel files directly and MUST NOT reimplement any part of
[state-channel.md](state-channel.md). What they add is idiomatic types,
error mapping, and buffer management.

The rules that apply to all of them (Go differs where its section says so):

- Load `libpsmsgr.so.1` by SONAME, not `libpsmsgr.so`: the unversioned
  symlink only exists in dev packages. Honor an override path in the
  environment variable `PSMSGR_LIBRARY`. (Go links the library at build
  time instead.)
- At load time, check `psmsgr_version()`: same major version, and at least
  the minimum minor version the binding needs. Fail loudly otherwise.
- A reader owns one receive buffer of `capacity` bytes (Python and C#; Go
  reads into the caller's slice). When a result
  carries `PSMSGR_INFO_ATTACHED`, the binding calls `describe()` and
  reallocates the buffer if the capacity changed. Reads never allocate
  except for the returned copy (Python `bytes`, C# convenience overloads).
  - A result that doesn't fill the owned buffer (`peek`, reads into a
    caller's buffer) only marks the buffer for that check at the next read
    into it.
  - If a read into the owned buffer still gets `PSMSGR_E_TOOSMALL`, the
    binding calls `describe()`, resizes and retries (a few times, then
    `PSMSGR_E_BUSY`).
- `PSMSGR_INFO_ATTACHED` is exposed as `StateInfo.attached` / `Attached`
  (and on Python's `Snapshot`), so applications can re-check `payload_type`.
  Like the C flag it is reported once per attach. If the result that
  carried it raises instead (`PSMSGR_E_TOOSMALL` from a read into a
  caller's buffer), the next result reports it.
- `PSMSGR_E_NODATA` is not an error: it maps to `None` / `false` / `null` /
  `ok == false`. Every other
  negative code maps to an exception that carries the code and, for
  `PSMSGR_E_SYS`, the `errno`. `PSMSGR_E_BUSY` gets its own exception type
  (`ChannelBusyError` / `PsMsgrError.Busy`; in Go the code `ErrBusy`),
  documented as transient.
- `wait` handles `PSMSGR_E_INTR` itself. It checks for cancellation
  (Python signals, C# `CancellationToken`, Go `context.Context`) and then
  retries with the remaining timeout.
- Timestamps are `CLOCK_MONOTONIC` ns, and each binding exposes the library's
  `now_ns()`, so an age is always `now_ns() - info.timestamp_ns`, computed on
  the same clock.
- Handles are closed deterministically (context manager / `IDisposable` /
  `Close`), with a finalizer (Go: a cleanup) as a safety net.

## Python — `ps_msgr`

- Python ≥ 3.11. The target (trixie) ships 3.13; the lower floor only
  exists so that development hosts with an older Python still work. No
  runtime dependencies. `ctypes`, so it is a pure-Python wheel, built with
  setuptools.
- Every C function is declared with explicit `argtypes` and `restype`, and
  the three structs are mirrored as `ctypes.Structure`. Options are set up
  with `psmsgr_state_options_init_sized(opt, sizeof(opt))`, and `describe`
  calls `psmsgr_state_describe_sized(r, desc, sizeof(desc))`. The binding's
  tests compare every size, offset and constant with a helper compiled from
  the C headers (`tests/interop_helper.c`).
- Loading: `ctypes` opens `$PSMSGR_LIBRARY` if set, else `libpsmsgr.so.1`.
  A missing library, a missing symbol or an incompatible version raises
  `ImportError` at import, naming the library and what is wrong.
- The GIL: `wait` and the calls that do file system work (opening a
  writer or a reader, `writer_alive`, `unlink`) release it, so a blocking
  `wait` doesn't stall other threads. `publish`, `read`, `peek`,
  `describe`, `now_ns` and `close` keep it (`ctypes.PyDLL`): releasing and
  reacquiring the GIL would cost more than these calls, and with other busy
  threads reacquiring can take a whole switch interval.
- When `wait` gets `PSMSGR_E_INTR`, the binding returns to the interpreter
  so that pending signal handlers run (`KeyboardInterrupt` is raised
  normally), then retries with the remaining time, following PEP 475.
  Signals reach Python handlers only in the main thread.
- Timeouts are float seconds: `None` (or `math.inf`) waits indefinitely, `0`
  polls once, and a positive value is rounded up to whole milliseconds per
  call, so it never becomes a poll. `wait` waits in slices of at most
  100 ms. A finite timeout longer than about 292 years (2^63 - 1
  nanoseconds) is clamped to that. Negative or NaN raises `ValueError`.
- Integer arguments are checked against their C type (`uint32_t`) and
  raise `ValueError` out of range, instead of being truncated by `ctypes`.
  A channel name or directory with a NUL character raises `ValueError`. The
  library validates everything else (`PSMSGR_E_INVAL`).
- `publish` takes any C-contiguous buffer (`bytes`, `bytearray`, writable
  `memoryview`, `array`, `ctypes.Structure`, …) without copying it first;
  a read-only buffer that isn't `bytes`, or a non-contiguous one, is copied
  once. `read_into` needs a writable C-contiguous buffer (`TypeError` or
  `BufferError` otherwise) and writes the value to its start.
- Handles close deterministically with `close()` or `with`. `__del__`
  closes a forgotten handle and emits a `ResourceWarning`, like an unclosed
  file. Calls on a closed handle raise `ValueError`. Like the C handles,
  the objects are not thread-safe: use one per thread.
- The one exception: another thread may `close()` a reader during `wait` or
  `writer_alive`, which release the GIL. `close()` returns at once and the
  reader counts as closed; the call in progress closes the native handle
  when it returns, and `wait` raises `ValueError` before its next slice.
  This is the way to stop a thread blocked in `wait(timeout=None)`. A
  per-reader lock and a count of those calls in progress implement it.

```python
from ps_msgr import StateWriter, StateReader, Snapshot, StateInfo, now_ns, unlink

class StateWriter:                        # context manager
    def __init__(self, name: str, capacity: int, *, slot_count: int = 3,
                 payload_type: int = 0, mode: int = 0o644,
                 recreate: bool = False, notify: bool = True,
                 directory: str | os.PathLike | None = None) -> None: ...
    def publish(self, data: bytes | bytearray | memoryview) -> int: ...  # -> generation
    @property
    def capacity(self) -> int: ...
    @property
    def closed(self) -> bool: ...
    def close(self) -> None: ...

class StateReader:                        # context manager
    def __init__(self, name: str, *, directory: str | os.PathLike | None = None) -> None: ...
    def read(self) -> Snapshot | None: ...                    # None: no data
    def read_into(self, buf: bytearray | memoryview) -> StateInfo | None: ...  # zero-alloc
    def peek(self) -> StateInfo | None: ...                   # no copy, no syscall
    def wait(self, last_generation: int = 0,
             timeout: float | None = None) -> bool: ...       # False on timeout
    def writer_alive(self) -> bool: ...
    def describe(self) -> ChannelDesc | None: ...
    @property
    def closed(self) -> bool: ...
    def close(self) -> None: ...

@dataclass(frozen=True, slots=True)
class StateInfo:  generation: int; length: int; timestamp_ns: int; attached: bool
    # .age_ns property: now_ns() - timestamp_ns

@dataclass(frozen=True, slots=True)
class Snapshot:   data: bytes; generation: int; timestamp_ns: int; attached: bool

@dataclass(frozen=True, slots=True)
class ChannelDesc: capacity: int; slot_count: int; payload_type: int; notify: bool

def now_ns() -> int: ...
def unlink(name: str, *, directory: str | os.PathLike | None = None) -> bool: ...  # False: absent

class ErrorCode(IntEnum): INVAL = -1; SYS = -2; ...  # mirrors PSMSGR_E_*

class PsMsgrError(OSError):               # base; errno set for PSMSGR_E_SYS
    code: int                             # an ErrorCode for known codes
    def __init__(self, code: int, message: str | None = None,
                 errno: int | None = None, filename: str | None = None) -> None: ...
class WriterExistsError(PsMsgrError): ...
class ChannelMismatchError(PsMsgrError): ...
class ChannelFormatError(PsMsgrError): ...
class PayloadTooLargeError(PsMsgrError, ValueError): ...
class ChannelBusyError(PsMsgrError): ...      # transient: retry
```

- `filename` is the channel name. `strerror` is `os.strerror(errno)` for
  `PSMSGR_E_SYS`, else `psmsgr_strerror(code)`. The exceptions pickle.
- `PayloadTooLargeError` has code `TOOBIG` from `publish`, and `TOOSMALL`
  from `read_into` when `buf` is too small.
- Other codes, e.g. `INVAL`, or `NOTSUP` from `wait` on a channel without
  notification, raise `PsMsgrError` itself.

Payload encoding is up to the application: `struct`, `ctypes.Structure`,
`numpy.frombuffer`, etc.

## C# — `PsMsgr`

- Target framework: **`netstandard2.1`** only. That one build runs on
  .NET Core 3.x / .NET 5+ and can be compiled into Native AOT apps. No
  package dependencies.
  - Built with `LangVersion` `latest`, `Nullable` `enable`,
    `AllowUnsafeBlocks` and `TreatWarningsAsErrors`.
  - Compiler attributes that `netstandard2.1` lacks (`IsExternalInit` for
    records and `init`) are defined `internal`.
- **Mono is not supported.** Mono resolves a P/Invoke when it compiles the
  calling method, before the static constructor that honors
  `PSMSGR_LIBRARY` runs, and Mono 6.12's corlib (trixie's) lacks the
  `ref readonly` signature of `ReadOnlySpan<T>.GetPinnableReference`, which
  `fixed` on a span binds to.

### Native AOT compatibility

The library MUST work in Native AOT apps. The proof is that a Native AOT app
using it publishes with **zero** trim/AOT warnings (build-and-test.md).
Rules that keep it that way:

- No reflection, `dynamic`, `Type`-based marshalling
  (`Marshal.SizeOf(Type)`, `Marshal.PtrToStructure(IntPtr, Type)`),
  `Activator`, `Expression`, `Reflection.Emit` or runtime-generated code.
  Generic payload helpers are constrained to `unmanaged` and use
  `sizeof(T)` and pointers. They use no
  `Unsafe` class: on `netstandard2.1` that would pull in a package
  dependency, and the library has none.
- No P/Invoke callbacks or delegates. The C API has none, and it MUST NOT
  grow any that the binding would need.
- Blittable P/Invoke signatures only (see *Native interop*). Native AOT
  compiles these without marshalling logic.
- Native AOT on the BeagleBone Black needs a **.NET 9+** app, because
  linux-arm (32-bit) is a Native AOT target only since .NET 9. Use the
  current LTS runtime.
- **Static linking (MAY):** an AOT app can link `libpsmsgr.a` into its
  executable with `<DirectPInvoke Include="libpsmsgr.so.1" />` and
  `<NativeLibrary Include="…/libpsmsgr.a" />`, so no `.so` needs deploying on
  the device.

### Native interop

- P/Invoke uses `[DllImport("libpsmsgr.so.1")]` with the versioned name
  hard-coded, and
  **blittable signatures only**: raw pointers, integers, and `byte*` for
  strings. The three structs are mirrored with `[StructLayout(Sequential)]`,
  and the tests compare their sizes and offsets, and every constant, with
  `tests/interop_helper layout`. Options are set up with
  `psmsgr_state_options_init_sized(&opt, sizeof(opt))`, and `Describe` calls
  `psmsgr_state_describe_sized(r, &desc, sizeof(desc))`.
  - The binding encodes strings to NUL-terminated UTF-8 itself, only in
    `Open` and `Unlink` (not the hot path).
  - Native handles live in `SafeHandle` subclasses. The P/Invoke signatures
    take the raw pointer (`DangerousGetHandle()`), and the wrapper checks for
    disposal first. Wrapper objects are not thread-safe, which matches the C
    API contract, so this is sound, and it avoids SafeHandle marshalling on
    the hot path. Each call is followed by `GC.KeepAlive(this)`, so that the
    handle's finalizer cannot close it during the call (e.g. a blocking
    `Wait` on an otherwise unreferenced reader).
  - The exception: another thread may `Dispose` a reader during `Wait` or
    `IsWriterAlive`. These two hold a reference on the handle
    (`DangerousAddRef`/`DangerousRelease`, instead of `GC.KeepAlive`), so
    the native handle closes when the call returns. The reader keeps its
    own disposed flag, because a `SafeHandle` with a reference outstanding
    doesn't report `IsClosed`. `Wait` then throws `ObjectDisposedException`
    before its next slice.
  - Calls that can return `PSMSGR_E_SYS` according to the header
    (`writer_open`, `reader_open`, `read`, `peek`, `wait`, `writer_alive`,
    `describe`, `unlink`) use `SetLastError = true`. `errno` is read with
    `Marshal.GetLastWin32Error()`, which works on Unix and in AOT, and its
    text comes from libc's XSI `strerror_r` (`__xpg_strerror_r`).
- `PSMSGR_LIBRARY` override: `NativeLibrary` isn't available on
  `netstandard2.1`. The static
  constructor of the interop class calls
  `dlopen($PSMSGR_LIBRARY, RTLD_NOW | RTLD_GLOBAL)` through
  `[DllImport("libdl.so.2")]` when the variable is set. glibc reuses an
  already-loaded library whose SONAME matches a later `dlopen` of that name,
  so the `DllImport` resolves to the preloaded copy. This works identically
  under JIT and Native AOT: both bind a P/Invoke at its first call, after the
  static constructor has run (a bare P/Invoke such as `Clock.NowNs()` runs
  it too). Without the variable, the normal search path applies
  (`LD_LIBRARY_PATH`, `ld.so.cache`).
- Load failures surface at the first call as a `TypeInitializationException`
  whose inner exception is a `DllNotFoundException` naming the library and
  the loader's error (a missing library, or a missing `psmsgr_version`), or
  a `PsMsgrException` with code `NotSup` for an incompatible version.
- The NuGet package MAY bundle `runtimes/linux-arm/native/libpsmsgr.so.1` and
  `runtimes/linux-x64/native/libpsmsgr.so.1` (for development). It doesn't
  yet: the library comes from the system.

```csharp
namespace PsMsgr;

public sealed class StateWriter : IDisposable
{
    public static StateWriter Open(string name, StateOptions options);
    public uint Capacity { get; }
    public uint Publish(ReadOnlySpan<byte> data);                     // -> generation
    public uint Publish<T>(in T value) where T : unmanaged;           // sizeof(T) bytes
    public WriteScope Begin();
    public void Dispose();                                            // aborts an open WriteScope
}

public readonly ref struct WriteScope                                 // use with `using`
{
    public Span<byte> Buffer { get; }                                 // Capacity bytes, 32-byte aligned
    public uint Commit(int length);                                   // -> generation; TooBig keeps it open
    public void Dispose();                                            // aborts unless committed
}

public sealed class StateOptions
{
    public StateOptions(uint capacity) { Capacity = capacity; }
    public uint Capacity    { get; }
    public uint SlotCount   { get; set; } = 3;
    public uint PayloadType { get; set; }
    public uint Mode        { get; set; } = 0x1A4;                   // 0644 (C# has no octal literals)
    public bool Recreate    { get; set; }
    public bool Notify      { get; set; } = true;
    public string? Directory { get; set; }
}

public sealed class StateReader : IDisposable
{
    public static StateReader Open(string name, string? directory = null);
    public bool TryRead(Span<byte> destination, out StateInfo info); // false: no data; TooSmall if too small
    public bool TryRead<T>(out T value, out StateInfo info) where T : unmanaged; // Mismatch if length != sizeof(T)
    public byte[]? Read(out StateInfo info);                          // allocating convenience; null: no data
    public bool TryPeek(out StateInfo info);                          // no copy, no syscall
    public bool Wait(uint lastGeneration, TimeSpan timeout, CancellationToken cancellationToken = default);
    public bool IsWriterAlive { get; }
    public ChannelDesc? Describe();                                   // null: not attached
    public void Dispose();
}

public readonly record struct StateInfo(uint Generation, uint Length, ulong TimestampNs, bool Attached)
{
    public TimeSpan Age { get; }                                      // Clock.NowNs() - TimestampNs
}
public readonly record struct ChannelDesc(uint Capacity, uint SlotCount, uint PayloadType, bool Notify);

public static class Clock   { public static ulong NowNs(); }             // psmsgr_now_ns
public static class Channel { public static bool Unlink(string name, string? directory = null); } // false: absent

public class PsMsgrException : IOException
{
    public PsMsgrException(PsMsgrError code, string? message = null, int errno = 0, string? channelName = null);
    public PsMsgrError Code { get; }
    public int Errno { get; }                                         // PSMSGR_E_SYS only, else 0
    public string? ChannelName { get; }
}
public enum PsMsgrError { Inval = -1, Sys = -2, NoData = -3, TooSmall = -4, TooBig = -5, Busy = -6,
    Timeout = -7, Intr = -8, WriterExists = -9, Mismatch = -10, Format = -11, NotSup = -12, State = -13 }
```

- Every failing call throws `PsMsgrException`; there are no subclasses,
  the `Code` tells them apart. The message is `strerror_r(errno)` with
  `(errno N)` for `Sys`, else `psmsgr_strerror(code)` or the binding's own
  text, followed by `: 'channel'`, e.g.
  `No such file or directory (errno 2): 'chan'`.
- `TryRead<T>` fails with `Mismatch` when the payload length isn't
  `sizeof(T)`, in either direction. Like a `TooSmall` from `TryRead`, such a
  result passes its `Attached` on to the next result.
- `Wait`: `Timeout.InfiniteTimeSpan` means no timeout, `TimeSpan.Zero` polls
  once, and a positive timeout is rounded up to whole milliseconds per call,
  so it never becomes a poll. Timeouts beyond the C API's `int32_t`
  milliseconds (up to `TimeSpan.MaxValue`) are waited in chunks. It waits in
  slices of at most 100 ms, and before each slice checks the
  `CancellationToken` (`OperationCanceledException`) and whether the reader
  was disposed (`ObjectDisposedException`).
  `PSMSGR_E_INTR` is expected, not only from the application: the runtime
  signals threads too. `Wait` retries with the remaining time.
- `WriteScope` holds only an id; the writer holds the state. So copies of a
  scope, including the read-only variable of a `using`, stay coherent: a
  commit through one ends them all, and `Dispose` after a commit does
  nothing. `Buffer` and `Commit` on an ended scope throw
  `InvalidOperationException`.
- Argument validation: a `null` name or options throw
  `ArgumentNullException`, a NUL character in a name or directory
  `ArgumentException`, a negative timeout (other than `InfiniteTimeSpan`) or
  commit length `ArgumentOutOfRangeException`. The library validates
  everything else (`Inval`). The integer arguments are already `uint`.
- Calls on a disposed handle throw `ObjectDisposedException`; `Dispose` is
  idempotent. `SafeHandle`'s finalizer closes a forgotten handle.
- `Publish<T>` / `TryRead<T>` copy the raw bytes of `T`. Declare `T` with
  `[StructLayout(LayoutKind.Sequential, Pack = …)]`, matching the C
  definition.

## Go — `psmsgr`

Module `github.com/ps-solucoes/ps-msgr/bindings/go`, package `psmsgr`
(`bindings/go/psmsgr/`). Go ≥ 1.24, the version trixie ships (the
`#cgo noescape`/`nocallback` directives and `runtime.AddCleanup` need it).
No dependencies beyond the standard library. Released with tags
`bindings/go/vX.Y.Z`.

### Native interop

- **cgo, linked at build time**, like a C program: `#cgo LDFLAGS:
  -lpsmsgr`, against `libpsmsgr-dev`. The executable records
  `libpsmsgr.so.1` (the SONAME) as `NEEDED`, so the load-by-SONAME rule
  holds. The build tag `psmsgr_static` links `-l:libpsmsgr.a` instead, and
  the executable needs no libpsmsgr. A non-installed library is found
  through `CGO_CFLAGS`/`CGO_LDFLAGS` at build time and `LD_LIBRARY_PATH` at
  run time.
  - `PSMSGR_LIBRARY` is not honored: the dynamic linker has loaded the
    library before any Go code runs. Loading it with `dlopen` instead would
    lose the header's types, the static build and the linker's symbol
    version check.
  - The dynamic linker checks the major (SONAME) and the symbol versions.
    The first `OpenWriter`, `OpenReader` or `Unlink` also checks
    `psmsgr_version()` and returns an `*Error` with code `ErrNotSup` if the
    major differs or the minor is older than the binding needs (1.1, for
    the waitset). The waitset's symbols are versioned `PSMSGR_1.1`, so in
    practice the dynamic linker already refuses a 1.0 library.
- The structs and constants are the header's own (`C.psmsgr_state_info`,
  …), so there are no mirrors to compare with `interop_helper layout`.
  Still, a test compares `psmsgr_waitset_event` and `PSMSGR_WAITSET_MAX`
  as cgo sees them with it. The exported `Code` constants are written out for the documentation, and
  checked against the header at compile time: a difference fails the build.
  Options are set up with `psmsgr_state_options_init_sized(&opt,
  sizeof(opt))`, and `Describe` calls `psmsgr_state_describe_sized(r,
  &desc, sizeof(desc))`.
- Every C function is declared `#cgo nocallback` (the C API calls nothing
  back), and those that take pointers `#cgo noescape` (none keeps one), so
  the Go values passed stay on the stack: the hot path (`Publish`,
  `PublishValue`, `PublishFunc`, `Read` with room in `dst`, `ReadValue`,
  `Peek`, `NowNs`) allocates nothing, which the tests check. Each call is
  one cgo call.
- Payload bytes pass to C in place: Go memory without Go pointers, as cgo
  requires. Names and directories are copied to C strings (`C.CString`),
  only when opening and in `Unlink`.
- `errno` comes from cgo's two-result call form, for the calls that can
  return `PSMSGR_E_SYS`.
- `runtime.KeepAlive` follows each call on a handle, so that the handle's
  cleanup cannot close it during the call.

### Interface

```go
package psmsgr

type WriterOptions struct {                   // the zero value is the defaults
    SlotCount   uint32 // 0: 3
    PayloadType uint32
    Mode        uint32 // 0: 0o644
    Recreate    bool
    NoNotify    bool
    Dir         string // "": $PSMSGR_DIR, else /dev/shm
}

func OpenWriter(name string, capacity uint32, opts *WriterOptions) (*Writer, error) // opts nil: defaults
func (w *Writer) Capacity() uint32
func (w *Writer) Publish(data []byte) (generation uint32, err error)
func (w *Writer) PublishFunc(build func(buf []byte) (int, error)) (generation uint32, err error)
func (w *Writer) Close() error
func PublishValue[T any](w *Writer, v *T) (generation uint32, err error)

func OpenReader(name, dir string) (*Reader, error)
func (r *Reader) Read(dst []byte) (data []byte, info Info, ok bool, err error) // appends to dst
func (r *Reader) Peek() (info Info, ok bool, err error)                          // no copy, no syscall
func (r *Reader) Wait(ctx context.Context, lastGeneration uint32, timeout time.Duration) (bool, error)
func (r *Reader) WaitChan(ctx context.Context, lastGeneration uint32, timeout time.Duration) <-chan WaitResult
func (r *Reader) WriterAlive() (bool, error)
func (r *Reader) Describe() (desc ChannelDesc, ok bool, err error)              // ok false: not attached
func (r *Reader) Close() error
func ReadValue[T any](r *Reader, v *T) (info Info, ok bool, err error)

type Info struct { Generation, Length uint32; TimestampNs uint64; Attached bool }
func (i Info) Age() time.Duration                                                // NowNs() - TimestampNs
type ChannelDesc struct { Capacity, SlotCount, PayloadType uint32; Notify bool }
type WaitResult struct { Changed bool; Generation uint32; Err error }           // Wait's results

const NoTimeout time.Duration = -1
func NowNs() uint64
func Unlink(name, dir string) (bool, error)                                     // false: absent

type Code int32                               // PSMSGR_E_*; implements error
const ( ErrInval Code = -1; ErrSys = -2; /* … */ ErrState = -13 )
type Error struct {
    Op      string                            // e.g. "open writer", "publish"
    Channel string
    Code    Code
    Errno   syscall.Errno                     // ErrSys only, else 0
    Msg     string                            // the binding's own text, if any
}
func (e *Error) Is(target error) bool          // target == e.Code
func (e *Error) Unwrap() error                 // e.Errno, if any
var ErrClosed error
```

- Results: `ok == false` for `PSMSGR_E_NODATA`; every other failure is an
  `*Error`, so `errors.Is(err, psmsgr.ErrBusy)` and, through `Errno`,
  `errors.Is(err, fs.ErrNotExist)` work. The message is
  `psmsgr: <op> "<channel>": <text>`, where the text is `Msg`, else
  `Errno`'s for `ErrSys`, else `psmsgr_strerror(code)`. Calls on a closed
  handle return `ErrClosed`.
- The binding's own checks return an `*Error` with code `ErrInval`: a NUL
  character in a name or directory, a negative length from `PublishFunc`'s
  `build`, a `PublishValue`/`ReadValue` type with pointers. A payload
  longer than the capacity is `ErrTooBig` before any call, so that a
  length beyond `uint32` cannot wrap.
- Zero values are defaults: `WriterOptions{}`, `SlotCount` and `Mode` 0,
  and `Dir` `""` (passed as `NULL`).
- `Read(dst)` appends the value to `dst` and returns the extended slice,
  like `append`. It reads into the room between `len(dst)` and `cap(dst)`;
  on `PSMSGR_E_TOOSMALL` it grows `dst` by the reported length and retries
  (a few times, then `ErrBusy`). So there is no receive buffer to resize on
  attach, and a reused `dst` reads without allocating.
- `PublishValue`/`ReadValue` copy the `unsafe.Sizeof(*v)` raw bytes of a
  `T` without pointers (no pointer, slice, string, map, chan, func or
  interface anywhere in it), checked with `reflect` once per type:
  arbitrary bytes written into a pointer field would corrupt the Go heap.
  `ReadValue` reads straight into `*v`, and fails with `ErrMismatch` when
  the payload length isn't `unsafe.Sizeof(*v)`, in either direction;
  `*v` may then be overwritten. Like a `TooSmall` in the other bindings,
  such a result passes its `Attached` on to the next result.
- `PublishFunc` is the zero-copy publish: `begin`, then `build` on the
  slot (`Capacity` bytes, the C buffer as a slice), then `commit` of the
  length it returns. An error from `build`, a panic, or a length out of
  range aborts instead. The slice aliases shared memory and must not be
  used after `build` returns; a callback, rather than a begin/commit pair,
  makes that the natural scope.
- `Wait`: a timeout of 0 polls once, a positive one is rounded up to whole
  milliseconds (so it never becomes a poll), and a negative one
  (`NoTimeout`) waits indefinitely; any `time.Duration` works, since the
  wait runs in slices of at most 100 ms. Before each slice it checks `ctx`
  (returns `ctx.Err()`, or `context.DeadlineExceeded` at `ctx`'s deadline,
  which also bounds the slice) and whether the reader was closed
  (`ErrClosed`). `PSMSGR_E_INTR` is retried: Go installs its own handlers
  with `SA_RESTART`, so it comes only from handlers installed by C code.
- `WaitChan` is `Wait` for a `select`: it returns a channel (buffered, so
  nobody has to receive) that delivers one `WaitResult`, with `Wait`'s
  arguments, results and errors, plus the generation that ended the wait
  (the `lastGeneration` for the next one).
  - A timeout of 0 polls at once, in the call. Otherwise the reader goes
    into a waitset, with a `time.AfterFunc` for a positive timeout and a
    `context.AfterFunc` for `ctx`; either one takes it out
    (`psmsgr_waitset_remove`). `ctx` delivers `ctx.Err()`. The timeout
    then polls once, as `Wait` does at its deadline, because the set may
    not have scanned a publish yet: it delivers `false`, a change, or
    `Wait`'s error. If `remove` returns `NODATA`, the event is the answer.
  - An event with a status other than `OK` is the `*Error` that `Wait`
    returns for that code (op `"wait"`, `Errno` from `sys_errno`).
  - The sets are shared by the process, up to `PSMSGR_WAITSET_MAX` (127)
    readers each, and opened when no set has room: any number of readers
    can wait. Each set has a goroutine, locked to its OS thread, in
    `psmsgr_waitset_wait`; it starts with the set and, after 10 s without
    readers, closes the set and ends. It waits with no timeout while the
    set has readers, so a timeout, `ctx` or `Close` that takes out the
    last one wakes it (`psmsgr_waitset_wake`) to start the 10 s; a remove
    alone doesn't end the wait. Goroutines don't keep a Go program
    alive, so it needs no shutdown. The token is a counter, not a Go
    pointer.
  - Without `futex_waitv` (`psmsgr_waitset_open` returns `NOTSUP`: Linux <
    5.16, qemu-user), every `WaitChan` runs `Wait`'s loop on a goroutine
    of its own, a thread per reader. Then `ctx` and `Close` stop it
    within 100 ms, as they stop `Wait`, instead of at once, and the
    generation comes from a `peek` after the wait (0 if the value is gone
    by then; an `Attached` it carries goes to the next result).
  - Until the result is delivered, the reader belongs to the wait, as the
    C API requires: `Wait`, `WriterAlive` and another `WaitChan` fail with
    `ErrState`, and the other calls must not be made. `Close` is allowed:
    it takes the reader out of the set first (which can block while the
    set's goroutine scans its readers, not longer), and the wait delivers
    `ErrClosed`, unless the event was already reported.
  - A reader with a wait pending stays reachable from the set, so it is
    not garbage collected; cancel `ctx` or `Close` it.
- Handles are not safe for concurrent use, like the C handles. The
  exception: another goroutine may `Close` a reader during `Wait`,
  `WriterAlive` or `WaitChan`. These take the handle under the reader's
  mutex and count themselves; `Close` returns at once (after a `WaitChan`'s
  `remove`), and the last of them to return closes the native handle. The other calls read the handle
  without the mutex (a `Close` concurrent with them is a data race, as
  with any Go value), so the hot path takes no lock.
- `Close` is idempotent and returns `nil`. `runtime.AddCleanup` closes a
  handle that is garbage collected without `Close`; `Close` cancels it.
- Payload structs need fixed-width fields and explicit padding to match C
  on every architecture: on 32-bit ARM, Go aligns `uint64` to 4 bytes and
  C to 8 (the motor-status struct needs a trailing `_ uint32`).

## Payload conventions (non-normative)

The library never looks at the payload. Some suggestions for applications:

- For hot, fixed-size state, use plain structs with fixed-width
  little-endian fields and explicit padding, defined once in a C header and
  mirrored in `ctypes.Structure` / `[StructLayout]` / a Go struct.
- Put a schema identifier in `payload_type`, e.g. a 16-bit schema ID in the
  upper half and a 16-bit schema version in the lower half. Readers check it with
  `describe()` before trusting the bytes.
- Variable or evolving data can use a self-describing encoding (MessagePack,
  CBOR, protobuf, FlatBuffers), with the size cost counted against
  `capacity`.
