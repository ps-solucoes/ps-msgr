# ps-msgr for Python

`ps_msgr` is a `ctypes` binding of `libpsmsgr`: state channels publish the
latest value of a payload from one writer to many readers through shared
memory. Pure Python, no dependencies, Python 3.11 or later. The interface is
specified in [`spec/bindings.md`](../../spec/bindings.md).

## Install

The binding needs the C library, `libpsmsgr.so.1`: on the target, install
the `libpsmsgr1` package. Then install the binding in a virtual
environment (Debian doesn't let `pip` install into the system Python; there
`python3 -m venv` needs the `python3-venv` package):

```sh
python3 -m venv .venv
.venv/bin/pip install bindings/python
```

and run programs with `.venv/bin/python`. Installing from the source tree
fetches `setuptools` and needs network; on an offline target, install
the wheel instead (see [`examples/README.md`](../../examples/README.md)).

At import, `ps_msgr` loads `libpsmsgr.so.1` through the normal library
search path. To use another copy, such as a build tree, set
`PSMSGR_LIBRARY` to its path:

```sh
export PSMSGR_LIBRARY=$PWD/build/release/libpsmsgr.so.1
```

The import fails with an `ImportError` if the library is missing or its
major version differs, or its minor version is older than the binding needs
(1.1).

## Usage

```python
import ctypes

from ps_msgr import StateReader, StateWriter


class MotorStatus(ctypes.Structure):
    """Mirrors the C `struct motor_status`: same fields, same natural alignment."""

    _fields_ = [
        ("sequence", ctypes.c_uint64),
        ("speed_rpm", ctypes.c_float),
        ("current_a", ctypes.c_float),
        ("temperature_c", ctypes.c_float),
    ]


MOTOR_STATUS_V1 = 0x0001_0001  # schema 1, version 1 (payload_type)

# Writer
with StateWriter("motor", ctypes.sizeof(MotorStatus), payload_type=MOTOR_STATUS_V1) as w:
    w.publish(memoryview(MotorStatus(1, 1500.0, 2.5, 41.0)))

# Reader: may start before the writer
with StateReader("motor") as r:
    status, seen = MotorStatus(), 0
    while r.wait(seen, timeout=0.5):  # False after 0.5 s without a change
        info = r.read_into(memoryview(status))  # copies straight into the struct
        if info is None:
            continue
        if info.attached and r.describe().payload_type != MOTOR_STATUS_V1:
            raise RuntimeError("unexpected payload type")
        seen = info.generation
        print(status.sequence, status.speed_rpm)
```

[`examples/python/`](../../examples/python/) has the complete programs,
which work with the C and C# examples: a writer at a steady rate, a reader
that tells a stale writer from a dead one, and Ctrl-C.

- Payloads are plain C structs mirrored with `ctypes.Structure`: declare the
  same fields in the same order and ctypes lays them out like the C compiler
  does. Pass `memoryview(obj)` to `publish()` and `read_into()`; with
  `read()`, use `MotorStatus.from_buffer_copy(snapshot.data)`.
- `read()` returns a `Snapshot` with a copy of the value; `read_into(buf)`
  copies into a `bytearray` or writable `memoryview` without allocating.
  Both return `None` while there is no value.
- `peek()` returns the generation, length and timestamp without copying.
  `info.age_ns` is the value's age on `CLOCK_MONOTONIC`.
- `wait()` releases the GIL while it blocks. Ctrl-C raises
  `KeyboardInterrupt` as usual.
- `await r.wait_async(seen, timeout=0.5)` is `wait()` for `asyncio`. One
  daemon thread waits for up to 127 readers (on kernels before 5.16, a
  thread per wait instead), so many readers need no thread each.
  Cancelling the task stops the wait; until it ends, `close()` is the only
  other call allowed on the reader. Starting, cancelling or closing a wait
  can block the loop while that thread scans its readers (attaching
  channels).
- Channels live in `/dev/shm` unless `directory=` or `$PSMSGR_DIR` says
  otherwise.
- Errors raise `PsMsgrError` (an `OSError`) or one of its subclasses; `code`
  is the library's result code.
- Handles are not thread-safe: use one per thread. The exception: closing
  a reader from another thread stops its `wait()` with `ValueError` within
  100 ms.

## Development

The tests run against a build of the C library. In the build container:

```sh
docker/run.sh cmake --workflow --preset release
docker/run.sh bindings/python/check.sh
```

`check.sh` builds and installs the wheel into a scratch directory, runs
pytest against it with `PSMSGR_LIBRARY` pointing at `build/release`, and
runs `ruff check` and `ruff format --check`.
