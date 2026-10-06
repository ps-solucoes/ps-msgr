# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import asyncio
import contextlib
import math
import operator
import os
import threading
import time
import warnings
from ctypes import Array, byref, c_char, c_uint32, sizeof
from dataclasses import dataclass
from types import TracebackType
from typing import Any, Final, Self, cast

from . import _native, _waitset
from ._errors import PayloadTooLargeError, error

StrPath = str | os.PathLike[str]

_U32_MAX: Final = 0xFFFF_FFFF
# Caps finite timeouts (about 292 years) so int() never sees inf.
_TIMEOUT_NS_MAX: Final = (1 << 63) - 1
# wait() waits in slices so that a close() from another thread stops it.
_WAIT_SLICE_MS: Final = 100
# A TOOSMALL after resizing to the described capacity means the channel was
# replaced in between; more than a few in a row is not going to happen.
_READ_ATTEMPTS: Final = 4


def now_ns() -> int:
    """CLOCK_MONOTONIC in nanoseconds: the clock of ``timestamp_ns``."""
    return int(_native.now_ns())


@dataclass(frozen=True, slots=True)
class StateInfo:
    generation: int
    length: int
    timestamp_ns: int
    attached: bool
    """First result since the reader (re)attached to a channel file: re-check
    ``describe().payload_type`` before trusting the bytes."""

    @property
    def age_ns(self) -> int:
        return now_ns() - self.timestamp_ns


@dataclass(frozen=True, slots=True)
class Snapshot:
    data: bytes
    generation: int
    timestamp_ns: int
    attached: bool
    """As ``StateInfo.attached``."""


@dataclass(frozen=True, slots=True)
class ChannelDesc:
    capacity: int
    slot_count: int
    payload_type: int
    notify: bool


def _u32(what: str, value: int) -> int:
    value = operator.index(value)
    if not 0 <= value <= _U32_MAX:
        raise ValueError(f"{what} out of range: {value}")
    return value


def _encode_name(name: str) -> bytes:
    if not isinstance(name, str):
        raise TypeError(f"name must be str, not {type(name).__name__}")
    b = name.encode("utf-8", "surrogateescape")
    if b"\0" in b:
        raise ValueError("embedded null character in name")
    return b


def _encode_dir(directory: StrPath | None) -> bytes | None:
    if directory is None:
        return None
    b = os.fsencode(directory)
    if b"\0" in b:
        raise ValueError("embedded null byte in directory")
    return b


def _closed(obj: object) -> ValueError:
    return ValueError(f"operation on closed {type(obj).__name__}")


def _busy(obj: object) -> RuntimeError:
    return RuntimeError(f"{type(obj).__name__} is busy in a wait")


def _timeout_ns(timeout: float | None) -> int | None:
    """A wait's timeout in nanoseconds; None: no timeout."""
    if timeout is None:
        return None
    t = float(timeout)
    if not t >= 0:
        raise ValueError(f"timeout must be non-negative or None, not {timeout!r}")
    return None if t == math.inf else int(min(t * 1e9, _TIMEOUT_NS_MAX))


def unlink(name: str, *, directory: StrPath | None = None) -> bool:
    """Retires and deletes a channel. False if it does not exist; raises
    ``WriterExistsError`` while a writer holds it."""
    rc = _native.state_unlink(_encode_name(name), _encode_dir(directory))
    if rc == _native.OK:
        return True
    if rc == _native.E_NODATA:
        return False
    raise error(rc, name)


class StateWriter:
    """The single writer of a channel: opens or creates it and takes the
    writer lock. Like the C handle, not thread-safe."""

    __slots__ = ("_capacity", "_gen", "_gen_ref", "_h", "_name")

    _close_fn = _native.state_writer_close  # still reachable from __del__ at exit

    def __init__(
        self,
        name: str,
        capacity: int,
        *,
        slot_count: int = _native.STATE_DEFAULT_SLOTS,
        payload_type: int = 0,
        mode: int = 0o644,
        recreate: bool = False,
        notify: bool = True,
        directory: StrPath | None = None,
    ) -> None:
        self._h: Any = None
        self._name = name
        opt = _native.StateOptions()
        _native.state_options_init_sized(byref(opt), sizeof(opt))
        opt.capacity = _u32("capacity", capacity)
        opt.slot_count = _u32("slot_count", slot_count)
        opt.payload_type = _u32("payload_type", payload_type)
        opt.mode = _u32("mode", mode)
        opt.flags = (_native.STATE_RECREATE if recreate else 0) | (
            0 if notify else _native.STATE_NO_NOTIFY
        )
        opt.dir = _encode_dir(directory)
        h = _native.WriterPtr()
        rc = _native.state_writer_open(_encode_name(name), byref(opt), byref(h))
        if rc != _native.OK:
            raise error(rc, name)
        self._h = h
        self._capacity = int(_native.state_writer_capacity(h))
        self._gen = c_uint32()
        self._gen_ref = byref(self._gen)

    def publish(self, data: bytes | bytearray | memoryview) -> int:
        """Copies and publishes a value; returns its generation. Any
        contiguous buffer works; a read-only one other than bytes is copied
        once more first."""
        h = self._h
        if h is None:
            raise _closed(self)
        buf: Any
        if type(data) is bytes:
            buf, n = data, len(data)
        elif type(data) is bytearray:
            n = len(data)
            buf = byref(c_char.from_buffer(data)) if n else None
        else:
            mv = memoryview(data)
            n = mv.nbytes
            if n == 0:
                buf = None
            elif mv.readonly or not mv.c_contiguous:
                buf = mv.tobytes()
            else:
                buf = byref(c_char.from_buffer(mv))
        if n > _U32_MAX:
            raise PayloadTooLargeError(_native.E_TOOBIG, filename=self._name)
        rc = _native.state_publish(h, buf, n, self._gen_ref)
        if rc != _native.OK:
            raise error(rc, self._name)
        return self._gen.value

    @property
    def capacity(self) -> int:
        if self._h is None:
            raise _closed(self)
        return self._capacity

    @property
    def closed(self) -> bool:
        return self._h is None

    def close(self) -> None:
        """Releases the writer lock; the channel and its last value remain."""
        h, self._h = self._h, None
        if h is not None:
            self._close_fn(h)

    def __enter__(self) -> Self:
        if self._h is None:
            raise _closed(self)
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_h", None) is not None:
            try:
                warnings.warn(f"unclosed {self!r}", ResourceWarning, source=self, stacklevel=1)
            finally:
                self.close()

    def __repr__(self) -> str:
        state = "closed" if self._h is None else f"capacity={self._capacity}"
        return f"<StateWriter {self._name!r} {state}>"


class StateReader:
    """A reader of a channel. Opening always succeeds for a valid name: the
    reader attaches when the channel appears, and follows it when it is
    recreated. Like the C handle, not thread-safe."""

    __slots__ = (
        "_buf",
        "_desc",
        "_desc_ref",
        "_h",
        "_info",
        "_info_ref",
        "_lock",
        "_name",
        "_pending",
        "_recheck",
        "_users",
        "_waiter",
    )

    _close_fn = _native.state_reader_close

    def __init__(self, name: str, *, directory: StrPath | None = None) -> None:
        self._h: Any = None
        self._name = name
        self._lock = threading.Lock()
        # Calls in progress that release the GIL (wait, writer_alive): close()
        # leaves the handle open for the last of them to close.
        self._users = 0
        # The wait_async in progress, if any: it owns the handle.
        self._waiter: _AsyncWait | None = None
        h = _native.ReaderPtr()
        rc = _native.state_reader_open(_encode_name(name), _encode_dir(directory), byref(h))
        if rc != _native.OK:
            raise error(rc, name)
        self._h = h
        self._info = _native.StateInfo()
        self._info_ref = byref(self._info)
        self._desc = _native.StateDesc()
        self._desc_ref = byref(self._desc)
        # Receive buffer for read(), sized to the channel's capacity.
        self._buf: Array[c_char] | None = None
        # An attach that peek()/read_into() reported: read() re-checks the
        # capacity.
        self._recheck = False
        # An attach consumed by a result that raised: reported by the next
        # result instead.
        self._pending = False

    def _handle(self) -> Any:
        h = self._h
        if h is None:
            raise _closed(self)
        if self._waiter is not None:
            raise _busy(self)
        return h

    def _enter(self) -> Any:
        with self._lock:
            h = self._h
            if h is None:
                raise _closed(self)
            if self._waiter is not None:
                raise _busy(self)
            self._users += 1
            return h

    def _end_async(self, waiter: _AsyncWait, enter: bool = False) -> Any:
        """Ends ``waiter``'s ownership of the reader. With ``enter``, also
        starts a call as ``_enter`` does and returns the handle (None if
        closed); ``_leave`` ends that call."""
        with self._lock:
            if self._waiter is waiter:
                self._waiter = None
            h = self._h
            if not enter or h is None:
                return None
            self._users += 1
            return h

    def _leave(self, h: Any) -> None:
        with self._lock:
            self._users -= 1
            last = self._h is None and self._users == 0
        if last:
            self._close_fn(h)

    def _attached(self) -> bool:
        attached = self._pending or bool(self._info.flags & _native.INFO_ATTACHED)
        self._pending = False
        return attached

    def _fit(self, h: Any, need: int = 0) -> None:
        """Resizes the receive buffer to the channel's capacity."""
        self._recheck = False
        rc = _native.state_describe_sized(h, self._desc_ref, sizeof(self._desc))
        if rc == _native.OK:
            size = self._desc.capacity
        elif rc == _native.E_NODATA:
            size = need
        else:
            raise error(rc, self._name)
        size = max(size, need)
        if size != (len(self._buf) if self._buf is not None else 0):
            self._buf = (c_char * size)() if size else None

    def read(self) -> Snapshot | None:
        """A copy of the latest value, or None if there is none."""
        h = self._handle()
        info = self._info
        attached = False
        resized = False
        for _ in range(_READ_ATTEMPTS):
            buf = self._buf
            size = len(buf) if buf is not None else 0
            rc = _native.state_read(h, buf, size, self._info_ref)
            if rc == _native.OK:
                attached |= self._attached()
                n = info.length
                # c_char arrays slice to bytes: the one copy read() makes.
                data = cast(bytes, buf[:n]) if buf is not None and n else b""
                if (attached and not resized) or self._recheck:
                    self._fit(h)
                return Snapshot(data, info.generation, info.timestamp_ns, attached)
            if rc == _native.E_NODATA:
                self._pending |= attached
                return None
            if rc != _native.E_TOOSMALL:
                self._pending |= attached
                raise error(rc, self._name)
            attached |= self._attached()
            self._fit(h, info.length)
            resized = True
        self._pending |= attached
        raise error(_native.E_BUSY, self._name)

    def read_into(self, buf: bytearray | memoryview) -> StateInfo | None:
        """Copies the latest value to the start of ``buf``, a writable
        contiguous buffer, without allocating. None if there is no value;
        raises ``PayloadTooLargeError`` if ``buf`` is too small."""
        h = self._handle()
        n = len(buf) if type(buf) is bytearray else memoryview(buf).nbytes
        ref: Any = byref(c_char.from_buffer(buf)) if n else None
        rc = _native.state_read(h, ref, min(n, _U32_MAX), self._info_ref)
        info = self._info
        if rc == _native.OK:
            attached = self._attached()
            self._recheck |= attached
            return StateInfo(info.generation, info.length, info.timestamp_ns, attached)
        if rc == _native.E_NODATA:
            return None
        if rc == _native.E_TOOSMALL:
            self._pending |= bool(info.flags & _native.INFO_ATTACHED)
            self._recheck |= self._pending
            raise error(
                rc,
                self._name,
                f"payload of {info.length} bytes does not fit in a {n}-byte buffer",
            )
        raise error(rc, self._name)

    def peek(self) -> StateInfo | None:
        """Generation, length and timestamp of the latest value, without
        copying it and without syscalls while attached. None if there is no
        value."""
        h = self._handle()
        rc = _native.state_peek(h, self._info_ref)
        if rc == _native.OK:
            info = self._info
            attached = self._attached()
            self._recheck |= attached
            return StateInfo(info.generation, info.length, info.timestamp_ns, attached)
        if rc == _native.E_NODATA:
            return None
        raise error(rc, self._name)

    def wait(self, last_generation: int = 0, timeout: float | None = None) -> bool:
        """Blocks until the generation differs from ``last_generation`` (0:
        until there is any value). ``timeout`` in seconds: None waits
        indefinitely, 0 polls once. False on timeout. Signal handlers run
        while waiting (a KeyboardInterrupt propagates); the wait then
        resumes with the remaining time (PEP 475). A ``close()`` from
        another thread makes it raise ``ValueError`` within 100 ms."""
        last = _u32("last_generation", last_generation)
        t = _timeout_ns(timeout)
        deadline = None if t is None else time.monotonic_ns() + t
        h = self._enter()
        try:
            return self._wait(h, last, deadline)
        finally:
            self._leave(h)

    def _wait(
        self, h: Any, last: int, deadline: int | None, waiter: _AsyncWait | None = None
    ) -> bool:
        while True:
            if self._h is None:
                raise _closed(self)
            if waiter is not None and waiter.stopped:
                return False
            if deadline is None:
                ms = _WAIT_SLICE_MS
            else:
                remaining = deadline - time.monotonic_ns()
                ms = min(-(-remaining // 1_000_000), _WAIT_SLICE_MS) if remaining > 0 else 0
            rc = _native.state_wait(h, last, ms)
            if rc == _native.OK:
                return True
            if rc == _native.E_TIMEOUT:
                if deadline is not None and (ms == 0 or time.monotonic_ns() >= deadline):
                    return False
            elif rc == _native.E_INTR:
                _native.check_signals()
            else:
                raise error(rc, self._name)

    async def wait_async(self, last_generation: int = 0, timeout: float | None = None) -> bool:
        """``wait`` for asyncio, without a blocked thread per reader: one
        thread waits for up to 127 readers (where the kernel lacks
        ``futex_waitv``, a thread per wait instead). Same arguments and
        result. Cancelling the task takes the reader out of the wait. Until
        the wait ends, ``close()`` is the only other call allowed on the
        reader (others raise ``RuntimeError``); it makes the wait raise
        ``ValueError``."""
        last = _u32("last_generation", last_generation)
        t = _timeout_ns(timeout)
        if t == 0:
            return self.wait(last, 0)
        waiter = _AsyncWait(self, asyncio.get_running_loop(), last)
        # Under the lock, so that a close() from another thread finds the
        # registration complete.
        with self._lock:
            h = self._h
            if h is None:
                raise _closed(self)
            if self._waiter is not None or self._users:
                raise _busy(self)
            waiter.reg = _waitset.register(h, last, waiter.event)
            if waiter.reg is None:
                self._users += 1  # the thread fallback: a close() leaves the handle to it
            self._waiter = waiter
        if waiter.reg is None:
            return await waiter.in_thread(h, last, t)
        timer = None if t is None else waiter.loop.call_later(t / 1e9, waiter.timeout)
        try:
            return await waiter.future
        except BaseException:  # cancelled, or the coroutine was closed
            waiter.remove()
            raise
        finally:
            if timer is not None:
                timer.cancel()

    def writer_alive(self) -> bool:
        """Whether a writer holds the channel now. Makes syscalls; also
        reattaches if the channel file was replaced."""
        h = self._enter()
        try:
            rc: int = _native.state_writer_alive(h)
        finally:
            self._leave(h)
        if rc >= 0:
            return rc == 1
        raise error(rc, self._name)

    def describe(self) -> ChannelDesc | None:
        """The attached channel's constant properties; None if not attached."""
        rc = _native.state_describe_sized(self._handle(), self._desc_ref, sizeof(self._desc))
        if rc == _native.OK:
            d = self._desc
            return ChannelDesc(
                d.capacity,
                d.slot_count,
                d.payload_type,
                not d.flags & _native.STATE_NO_NOTIFY,
            )
        if rc == _native.E_NODATA:
            return None
        raise error(rc, self._name)

    @property
    def closed(self) -> bool:
        return self._h is None

    def close(self) -> None:
        """Closes the reader. Another thread may call it during ``wait`` or
        ``writer_alive``; the handle then closes when that call returns.
        During ``wait_async`` it takes the reader out of the waitset first,
        which can block briefly."""
        with self._lock:
            h, self._h = self._h, None
            deferred = self._users != 0
            waiter = self._waiter
        if h is not None:
            self._buf = None
            if waiter is not None:
                waiter.close()
            if not deferred:
                self._close_fn(h)

    def __enter__(self) -> Self:
        if self._h is None:
            raise _closed(self)
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_h", None) is not None:
            try:
                warnings.warn(f"unclosed {self!r}", ResourceWarning, source=self, stacklevel=1)
            finally:
                self.close()

    def __repr__(self) -> str:
        return f"<StateReader {self._name!r}{' closed' if self._h is None else ''}>"


class _AsyncWait:
    """A wait_async in progress: completes ``future`` once, on its loop."""

    __slots__ = ("future", "last", "loop", "reader", "reg", "stopped")

    def __init__(self, reader: StateReader, loop: asyncio.AbstractEventLoop, last: int) -> None:
        self.reader = reader
        self.loop = loop
        self.last = last
        self.future: asyncio.Future[bool] = loop.create_future()
        self.reg: _waitset.Registration | None = None
        self.stopped = False  # the thread fallback: cancelled

    def _post(self, result: bool | BaseException) -> None:
        with contextlib.suppress(RuntimeError):  # the loop is closed: nobody awaits it
            self.loop.call_soon_threadsafe(self._complete, result)

    def _complete(self, result: bool | BaseException) -> None:
        if self.future.done():
            return
        if isinstance(result, BaseException):
            self.future.set_exception(result)
        else:
            self.future.set_result(result)

    def event(self, status: int, generation: int, sys_errno: int) -> None:
        """The waitset reported the reader (on the set's thread)."""
        self.reader._end_async(self)
        if status == _native.OK:
            self._post(True)
        else:
            self._post(error(status, self.reader._name, errno=sys_errno))

    def remove(self) -> bool:
        """Takes the reader out of the set; True if no event will come."""
        assert self.reg is not None
        try:
            return self.reg.remove()
        finally:
            self.reader._end_async(self)

    def timeout(self) -> None:
        """The deadline. Only the set's thread checks the generation, and it
        may not have yet: once the reader is out of the set, check it once
        here, as ``wait`` does at its deadline."""
        assert self.reg is not None
        r = self.reader
        try:
            removed = self.reg.remove()
        except BaseException as e:
            r._end_async(self)
            self._complete(e)
            return
        if not removed:
            return  # the event is on its way: it is the answer, and frees the reader
        h = r._end_async(self, enter=True)
        if h is None:  # closed meanwhile; its remove() found the reader gone
            self._complete(_closed(r))
            return
        result: bool | BaseException
        try:
            result = r._wait(h, self.last, 0)
        except BaseException as e:
            result = e
        finally:
            r._leave(h)
        self._complete(result)

    def close(self) -> None:
        """The reader is being closed, from any thread. The thread fallback
        notices it before its next slice."""
        if self.reg is not None and self.remove():
            self._post(_closed(self.reader))

    async def in_thread(self, h: Any, last: int, timeout_ns: int | None) -> bool:
        """The fallback without waitsets: ``wait`` on a daemon thread."""
        r = self.reader
        deadline = None if timeout_ns is None else time.monotonic_ns() + timeout_ns

        def run() -> None:
            result: bool | BaseException
            try:
                result = r._wait(h, last, deadline, self)
            except BaseException as e:
                result = e
            finally:
                r._end_async(self)
                r._leave(h)
            self._post(result)

        try:
            threading.Thread(target=run, name="ps_msgr wait", daemon=True).start()
        except BaseException:
            r._end_async(self)
            r._leave(h)
            raise
        try:
            return await asyncio.shield(self.future)
        except asyncio.CancelledError:
            # The reader is the caller's again once the thread has left the
            # wait, within one slice (100 ms): wait for that, then cancel.
            self.stopped = True
            while not self.future.done():
                with contextlib.suppress(asyncio.CancelledError):
                    await asyncio.shield(self.future)
            self.future.exception()  # retrieved: the result no longer matters
            raise
        except BaseException:  # the coroutine was closed: the thread ends alone
            self.stopped = True
            raise
