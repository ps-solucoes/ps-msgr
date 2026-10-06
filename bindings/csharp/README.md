# ps-msgr for C#

`PsMsgr` is a P/Invoke binding of `libpsmsgr`: state channels publish the
latest value of a payload from one writer to many readers through shared
memory. One `netstandard2.1` assembly without dependencies, for .NET Core 3.x
and .NET 5+, including Native AOT apps. Mono is not supported. The interface
is specified in [`spec/bindings.md`](../../spec/bindings.md).

## Install

The binding needs the C library, `libpsmsgr.so.1`: on the target, install
the `libpsmsgr1` package. Then reference the package (CI builds
`PsMsgr.<version>.nupkg`; it is not published to nuget.org), or the project:

```xml
<ProjectReference Include="path/to/bindings/csharp/PsMsgr/PsMsgr.csproj" />
```

At the first call, `PsMsgr` loads `libpsmsgr.so.1` through the normal
library search path. To use another copy, such as a build tree, set
`PSMSGR_LIBRARY` to its path:

```sh
export PSMSGR_LIBRARY=$PWD/build/release/libpsmsgr.so.1
```

If the library is missing, or its major version differs, or its minor
version is older than the binding needs (1.1), the first call throws a
`TypeInitializationException` whose inner exception says what is wrong.

## Usage

```csharp
using System.Runtime.InteropServices;
using PsMsgr;

const uint MotorStatusV1 = 0x0001_0001; // schema 1, version 1 (payload_type)

// Writer
using (var w = StateWriter.Open("motor", new StateOptions(24) { PayloadType = MotorStatusV1 }))
    w.Publish(new MotorStatus { Sequence = 1, SpeedRpm = 1500f, CurrentA = 2.5f, TemperatureC = 41f });

// Reader: may start before the writer
using (var r = StateReader.Open("motor"))
{
    uint seen = 0;
    while (r.Wait(seen, TimeSpan.FromMilliseconds(500))) // false after 0.5 s without a change
    {
        if (!r.TryRead(out MotorStatus status, out StateInfo info)) // copies straight into the struct
            continue;
        if (info.Attached && r.Describe()?.PayloadType != MotorStatusV1)
            throw new InvalidDataException("unexpected payload type");
        seen = info.Generation;
        Console.WriteLine($"{status.Sequence} {status.SpeedRpm}");
    }
}

// Mirrors the C struct motor_status: same fields, same natural alignment,
// 24 bytes (padded to the 8-byte alignment of Sequence).
[StructLayout(LayoutKind.Sequential)]
struct MotorStatus
{
    public ulong Sequence;
    public float SpeedRpm;
    public float CurrentA;
    public float TemperatureC;
}
```

[`examples/csharp/`](../../examples/csharp/) has the complete programs,
which work with the C and Python examples: a writer at a steady rate, a
reader that tells a stale writer from a dead one, Ctrl-C, and Native AOT
publishing for the BeagleBone Black.

- Payloads are plain structs: `Publish<T>` and `TryRead<T>` copy the raw
  `sizeof(T)` bytes of any `unmanaged` struct. Declare it with
  `[StructLayout(LayoutKind.Sequential)]` (and `Pack` if the C side packs)
  to match the C definition. `TryRead<T>` throws a `PsMsgrException` with
  code `Mismatch` if the payload has another length.
- For bytes: `Publish(ReadOnlySpan<byte>)`, `TryRead(Span<byte>, out info)`
  (no allocation) and `Read(out info)` (returns a copy). Reads return
  `false` or `null` while there is no value.
- Zero-copy publish: `using var scope = w.Begin();` write into
  `scope.Buffer`, then `scope.Commit(length)`. Disposing the scope without a
  commit aborts it. It pays off only when the value is built in
  `scope.Buffer` and is a few KiB or larger; otherwise use `Publish`.
- `TryPeek` returns the generation, length and timestamp without copying.
  `info.Age` is the value's age on `CLOCK_MONOTONIC` (`Clock.NowNs()`).
- `Wait` takes a `CancellationToken`; `Timeout.InfiniteTimeSpan` waits
  indefinitely.
- `await r.WaitAsync(seen, timeout, token)` waits the same way without
  blocking a thread per reader: one background thread per 127 waiting
  readers completes the tasks (libpsmsgr waitsets, Linux 5.16; older
  kernels get a thread per wait). Until the task completes, the reader
  belongs to it: other calls throw `InvalidOperationException`, except
  `Dispose`, which ends the wait with `ObjectDisposedException`.
- Channels live in `/dev/shm` unless `StateOptions.Directory` (or the
  `directory` argument) or `$PSMSGR_DIR` says otherwise.
- Errors throw `PsMsgrException` (an `IOException`); `Code` is the library's
  result code, `Errno` the system error for `PsMsgrError.Sys`. `Busy` is
  transient: retry.
- Handles are `IDisposable` and not thread-safe: use one per thread. The
  exception: disposing a reader from another thread stops its `Wait` with
  `ObjectDisposedException` within 100 ms.

## Native AOT

The library is Native AOT compatible: blittable P/Invoke signatures, no
reflection, and generic helpers constrained to `unmanaged`. CI publishes a
smoke app that calls every public API with zero trim or AOT warnings.
32-bit ARM (the BeagleBone Black) is a Native AOT target since .NET 9.

An AOT app can link the static library into its executable, so that no
`libpsmsgr.so.1` needs deploying:

```xml
<ItemGroup>
  <DirectPInvoke Include="libpsmsgr.so.1" />
  <NativeLibrary Include="/usr/lib/arm-linux-gnueabihf/libpsmsgr.a" />
</ItemGroup>
```

`libpsmsgr.a` comes with `libpsmsgr-dev`. Leave `PSMSGR_LIBRARY` unset
then: the calls go to the linked copy, but the binding still loads the file
it names, and the first call fails if it cannot.

Both the dynamic and the static build run on the BeagleBone Black
([on-target results](../../bench/results/2026-09-24-bbb-bindings-ebc2ba0/REPORT.md)).

## Development

The tests run against a build of the C library. In the build container:

```sh
docker/run.sh cmake --workflow --preset release
docker/run.sh bindings/csharp/check.sh
```

`check.sh` builds the solution, runs the xUnit tests with `PSMSGR_LIBRARY`
pointing at `build/release`, checks `dotnet format --verify-no-changes`,
publishes and runs the Native AOT smoke app, and packs the library into
`build/release/csharp/nupkg/`.
