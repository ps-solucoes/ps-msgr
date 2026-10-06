# SPDX-License-Identifier: Apache-2.0
"""StateReader.wait_async: through the waitsets, and through the thread per
wait that replaces them where psmsgr_waitset_open returns NOTSUP."""

from __future__ import annotations

import asyncio
import errno
import gc
import os
import subprocess
import sys
import textwrap
import threading
import time
import warnings
import weakref
from collections.abc import Awaitable, Callable, Iterator
from ctypes import byref
from pathlib import Path
from typing import Any

import pytest
from conftest import CHAN, data_path

from ps_msgr import (
    ChannelFormatError,
    ErrorCode,
    PsMsgrError,
    StateReader,
    StateWriter,
    _native,
    _waitset,
)


def _have_waitsets() -> bool:
    ws = _native.WaitsetPtr()
    rc = _native.waitset_open(byref(ws))
    if rc == _native.OK:
        _native.waitset_close(ws)
    return rc == _native.OK


@pytest.fixture(params=["waitset", "threads"])
def mode(request: pytest.FixtureRequest, monkeypatch: pytest.MonkeyPatch) -> str:
    if request.param == "threads":
        monkeypatch.setattr(_waitset, "unsupported", True)
    elif not _have_waitsets():
        pytest.skip("psmsgr_waitset_open returns NOTSUP here (no futex_waitv)")
    return str(request.param)


@pytest.fixture
def channel(tmp_path: Path) -> Iterator[tuple[StateWriter, StateReader]]:
    with (
        StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        yield w, r


def run(main: Callable[[], Awaitable[Any]]) -> Any:
    return asyncio.run(main())  # type: ignore[arg-type]


def wait_threads() -> list[threading.Thread]:
    return [t for t in threading.enumerate() if t.name == "ps_msgr wait"]


def registered() -> int:
    return sum(len(s.pending) for s in _waitset._sets)


async def until(cond: Callable[[], bool], timeout: float = 5) -> None:
    deadline = time.monotonic() + timeout
    while not cond():
        assert time.monotonic() < deadline
        await asyncio.sleep(0.005)


def test_wakes_on_publish(mode: str, channel: tuple[StateWriter, StateReader]) -> None:
    w, r = channel

    async def main() -> None:
        gen = w.publish(b"a")
        # A value that differs from last_generation completes at once.
        assert await r.wait_async() is True
        assert await r.wait_async(gen ^ 1, None) is True
        await until(lambda: not wait_threads())
        task = asyncio.create_task(r.wait_async(gen, 10))
        await asyncio.sleep(0.05)
        assert not task.done()
        assert len(wait_threads()) == (mode == "threads")  # the waitset's thread is shared
        new = w.publish(b"b")
        assert await task is True
        assert r.peek().generation == new
        assert await r.wait_async(gen) is True  # the reader is free again
        await until(lambda: not wait_threads())

    run(main)


def test_timeouts(mode: str, channel: tuple[StateWriter, StateReader], tmp_path: Path) -> None:
    w, r = channel

    async def main() -> None:
        assert await r.wait_async(0, 0) is False  # polls once
        gen = w.publish(b"a")
        assert await r.wait_async(gen, 0) is False
        assert await r.wait_async(0, 0) is True

        t0 = time.monotonic_ns()
        assert await r.wait_async(gen, 0.12) is False
        # A loop timer may run up to the clock's resolution early.
        assert time.monotonic_ns() - t0 >= 119_000_000
        with StateReader("absent", directory=tmp_path) as none:
            t0 = time.monotonic_ns()
            assert await none.wait_async(0, 0.05) is False
            assert time.monotonic_ns() - t0 >= 49_000_000

        for bad in (-1, -0.001, float("nan")):
            with pytest.raises(ValueError):
                await r.wait_async(gen, bad)
        with pytest.raises(ValueError):
            await r.wait_async(-1, 0)
        with pytest.raises(ValueError):
            await r.wait_async(1 << 32)

    run(main)


def test_huge_timeout(mode: str, channel: tuple[StateWriter, StateReader]) -> None:
    w, r = channel

    async def main() -> None:
        gen = w.publish(b"a")
        task = asyncio.create_task(r.wait_async(gen, sys.float_info.max))
        await asyncio.sleep(0.05)
        assert not task.done()
        w.publish(b"b")
        assert await task is True
        assert await r.wait_async(0, float("inf")) is True

    run(main)


def test_cancel(mode: str, channel: tuple[StateWriter, StateReader]) -> None:
    w, r = channel

    async def main() -> None:
        gen = w.publish(b"a")
        for timeout in (None, 30):
            task = asyncio.create_task(r.wait_async(gen, timeout))
            await asyncio.sleep(0.05)
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task
            assert registered() == 0
            assert r.peek().generation == gen  # the reader is the caller's again
        with pytest.raises(TimeoutError):
            async with asyncio.timeout(0.05):
                await r.wait_async(gen)
        assert r.peek().generation == gen
        task = asyncio.create_task(r.wait_async(gen))
        await asyncio.sleep(0.02)
        w.publish(b"b")
        assert await task is True
        await until(lambda: not wait_threads())

    run(main)


def test_busy_while_waiting(mode: str, channel: tuple[StateWriter, StateReader]) -> None:
    w, r = channel

    async def main() -> None:
        gen = w.publish(b"a")
        task = asyncio.create_task(r.wait_async(gen))
        await asyncio.sleep(0.02)
        for call in (
            r.read,
            lambda: r.read_into(bytearray(8)),
            r.peek,
            r.describe,
            r.writer_alive,
            lambda: r.wait(0, 0),
        ):
            with pytest.raises(RuntimeError, match="busy"):
                call()
        with pytest.raises(RuntimeError, match="busy"):
            await r.wait_async(gen)
        assert not task.done()
        w.publish(b"b")
        assert await task is True
        assert r.read().data == b"b"

    run(main)


@pytest.mark.parametrize("closer", ["loop", "thread"])
def test_close_during_wait(
    mode: str, closer: str, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    closed: list[object] = []
    close = StateReader._close_fn

    def recording_close(h: object) -> None:
        closed.append(h)
        close(h)

    monkeypatch.setattr(StateReader, "_close_fn", staticmethod(recording_close))

    async def main() -> None:
        with StateWriter(CHAN, 8, directory=tmp_path) as w:
            gen = w.publish(b"a")
            for timeout in (None, 30):
                closed.clear()
                r = StateReader(CHAN, directory=tmp_path)
                task = asyncio.create_task(r.wait_async(gen, timeout))
                await asyncio.sleep(0.05)
                t0 = time.monotonic()
                if closer == "loop":
                    r.close()
                else:
                    await asyncio.to_thread(r.close)
                assert r.closed
                with pytest.raises(ValueError, match="closed"):
                    await task
                assert time.monotonic() - t0 < 2
                assert registered() == 0
                await until(lambda: len(closed) == 1)  # thread fallback: when it leaves
                r.close()
                assert len(closed) == 1
                with pytest.raises(ValueError, match="closed"):
                    await r.wait_async(gen)

    run(main)


def test_many_readers(mode: str, channel: tuple[StateWriter, StateReader], tmp_path: Path) -> None:
    w, _ = channel
    n = 2 * _native.WAITSET_MAX + 10  # three sets

    async def main() -> None:
        gen = w.publish(b"a")
        readers = [StateReader(CHAN, directory=tmp_path) for _ in range(n)]
        try:
            tasks = [asyncio.create_task(r.wait_async(gen, 30)) for r in readers]
            await asyncio.sleep(0.1)
            assert not any(t.done() for t in tasks)
            if mode == "waitset":
                assert registered() == n
                assert all(len(s.pending) <= _native.WAITSET_MAX for s in _waitset._sets)
                assert not wait_threads()
            else:
                assert len(wait_threads()) == n
            new = w.publish(b"b")
            assert await asyncio.gather(*tasks) == [True] * n
            assert {r.peek().generation for r in readers} == {new}
        finally:
            for r in readers:
                r.close()

    run(main)
    if mode == "waitset":
        assert len(_waitset._sets) >= 3
        assert all(s.thread is not None and s.thread.daemon for s in _waitset._sets)


def test_errors_match_wait(mode: str, tmp_path: Path) -> None:
    def sync_error(r: StateReader) -> PsMsgrError:
        with pytest.raises(PsMsgrError) as e:
            r.wait(0, 0.1)
        return e.value

    async def main() -> None:
        with (
            StateWriter("nonotify", 8, notify=False, directory=tmp_path) as w,
            StateReader("nonotify", directory=tmp_path) as r,
        ):
            w.publish(b"a")
            with pytest.raises(PsMsgrError) as e:
                await r.wait_async(0, 1)
            assert type(e.value) is PsMsgrError and e.value.code == ErrorCode.NOTSUP
            assert repr(e.value) == repr(sync_error(r))

        data_path(tmp_path, "junk").write_bytes(b"\xa5" * 4096)
        with StateReader("junk", directory=tmp_path) as r:
            with pytest.raises(ChannelFormatError) as e:
                await r.wait_async(0, 1)
            assert repr(e.value) == repr(sync_error(r))

        os.symlink(tmp_path / "elsewhere", data_path(tmp_path, "loop"))
        with StateReader("loop", directory=tmp_path) as r:
            with pytest.raises(PsMsgrError) as e:
                await r.wait_async(0, 1)
            assert (e.value.code, e.value.errno) == (ErrorCode.SYS, errno.ELOOP)
            assert repr(e.value) == repr(sync_error(r))

    run(main)


def test_loop_closed_while_waiting(mode: str, channel: tuple[StateWriter, StateReader]) -> None:
    w, r = channel
    gen = w.publish(b"a")
    loop = asyncio.new_event_loop()
    task = loop.create_task(r.wait_async(gen))
    loop.run_until_complete(asyncio.sleep(0.05))
    loop.close()
    w.publish(b"b")  # the result has no loop to go to
    deadline = time.monotonic() + 5
    while True:
        try:
            r.peek()
            break
        except RuntimeError:
            assert time.monotonic() < deadline
            time.sleep(0.005)
    ref = weakref.ref(task)
    del task
    gc.collect()  # nothing keeps the task: collecting it closes the coroutine
    assert ref() is None
    assert r.wait(0, 0) is True


def test_run_cancels_pending_wait(mode: str, channel: tuple[StateWriter, StateReader]) -> None:
    w, r = channel
    gen = w.publish(b"a")
    tasks = []

    async def main() -> None:
        tasks.append(asyncio.create_task(r.wait_async(gen)))
        await asyncio.sleep(0.05)

    run(main)  # cancels the task at the end
    assert tasks[0].cancelled()
    assert registered() == 0
    assert r.peek().generation == gen


def test_several_loops(mode: str, channel: tuple[StateWriter, StateReader], tmp_path: Path) -> None:
    w, _ = channel
    gen = w.publish(b"a")
    started = threading.Barrier(5)

    def in_loop() -> list[bool]:
        async def main() -> list[bool]:
            with (
                StateReader(CHAN, directory=tmp_path) as r1,
                StateReader(CHAN, directory=tmp_path) as r2,
            ):
                waits = asyncio.gather(r1.wait_async(gen, 10), r2.wait_async(gen, 10))
                await asyncio.sleep(0.02)
                await asyncio.to_thread(started.wait)
                return await waits

        return asyncio.run(main())

    results: list[list[bool]] = []
    threads = [threading.Thread(target=lambda: results.append(in_loop())) for _ in range(4)]
    for t in threads:
        t.start()
    started.wait()
    time.sleep(0.05)
    w.publish(b"b")
    for t in threads:
        t.join(10)
    assert results == [[True, True]] * 4


def test_notsup_falls_back_to_threads(
    channel: tuple[StateWriter, StateReader], monkeypatch: pytest.MonkeyPatch
) -> None:
    w, r = channel
    opened = []

    def notsup(out: object) -> int:
        opened.append(out)
        return _native.E_NOTSUP

    monkeypatch.setattr(_waitset, "unsupported", False)
    monkeypatch.setattr(_waitset, "_sets", [])
    monkeypatch.setattr(_native, "waitset_open", notsup)

    async def main() -> None:
        gen = w.publish(b"a")
        task = asyncio.create_task(r.wait_async(gen, 10))
        await asyncio.sleep(0.05)
        assert _waitset.unsupported
        assert len(wait_threads()) == 1
        w.publish(b"b")
        assert await task is True
        assert await r.wait_async(0, 1) is True

    run(main)
    assert len(opened) == 1  # tried once


def test_waiting_threads_do_not_keep_the_process(
    mode: str, tmp_path: Path, child_env: dict[str, str]
) -> None:
    # A set's thread waits for ever, and a fallback thread until its timeout.
    script = textwrap.dedent(
        """
        import asyncio, sys
        from ps_msgr import StateReader, _waitset
        _waitset.unsupported = sys.argv[2] == "threads"
        r = StateReader("chan", directory=sys.argv[1])
        loop = asyncio.new_event_loop()
        loop.create_task(r.wait_async(0, 3600))
        loop.run_until_complete(asyncio.sleep(0.05))
        print("waiting")
        """
    )
    p = subprocess.run(
        [sys.executable, "-W", "ignore", "-c", script, str(tmp_path), mode],
        env=child_env,
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert p.returncode == 0, p.stderr
    assert p.stdout == "waiting\n"


def test_fork_starts_over(channel: tuple[StateWriter, StateReader], tmp_path: Path) -> None:
    if not _have_waitsets():
        pytest.skip("psmsgr_waitset_open returns NOTSUP here (no futex_waitv)")
    w, r = channel
    w.publish(b"a")
    assert run(lambda: r.wait_async(0, 1)) is True
    assert _waitset._sets  # a set and its thread, which the child does not get
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)  # fork with threads
        pid = os.fork()
    if pid == 0:
        ok = False
        try:
            with StateReader(CHAN, directory=tmp_path) as child:
                ok = asyncio.run(child.wait_async(0, 5)) and len(_waitset._sets) == 1
        finally:
            os._exit(0 if ok else 1)
    _, status = os.waitpid(pid, 0)
    assert os.waitstatus_to_exitcode(status) == 0


def test_deadline_checks_once(mode: str, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    # A timeout that ends before the set's thread has looked at the reader
    # still checks it once, as wait() does.
    if mode == "waitset":  # the set's thread never sees the reader
        monkeypatch.setattr(_native, "waitset_add", lambda ws, h, last, token: _native.OK)
        monkeypatch.setattr(_native, "waitset_remove", lambda ws, h: _native.OK)

    async def main() -> None:
        with (
            StateWriter(CHAN, 8, directory=tmp_path) as w,
            StateReader(CHAN, directory=tmp_path) as r,
        ):
            old = w.publish(b"a")
            w.publish(b"b")
            assert r.wait(old, 0.0005) is True
            assert await r.wait_async(old, 0.0005) is True
            gen = r.peek().generation
            assert await r.wait_async(gen, 0.0005) is False
            assert registered() == 0
        with (
            StateWriter("nonotify", 8, notify=False, directory=tmp_path) as w,
            StateReader("nonotify", directory=tmp_path) as r,
        ):
            w.publish(b"a")
            with pytest.raises(PsMsgrError) as e:
                await r.wait_async(0, 0.0005)
            assert e.value.code == ErrorCode.NOTSUP

    run(main)


def test_thread_start_fails(
    channel: tuple[StateWriter, StateReader], monkeypatch: pytest.MonkeyPatch
) -> None:
    if not _have_waitsets():
        pytest.skip("psmsgr_waitset_open returns NOTSUP here (no futex_waitv)")
    w, r = channel
    closed = []
    close = _native.waitset_close

    class NoThread(threading.Thread):
        def start(self) -> None:
            raise RuntimeError("can't start new thread")

    def recording_close(ws: object) -> None:
        closed.append(ws)
        close(ws)

    monkeypatch.setattr(_waitset, "_sets", [])
    monkeypatch.setattr(_native, "waitset_close", recording_close)
    monkeypatch.setattr(_waitset.threading, "Thread", NoThread)

    async def main() -> None:
        gen = w.publish(b"a")
        with pytest.raises(RuntimeError, match="can't start"):
            await r.wait_async(gen, 10)
        assert _waitset._sets == [] and len(closed) == 1
        monkeypatch.undo()
        monkeypatch.setattr(_waitset, "_sets", [])
        task = asyncio.create_task(r.wait_async(gen, 10))
        await asyncio.sleep(0.05)
        w.publish(b"b")
        assert await task is True

    run(main)


@pytest.fixture
def held_events(monkeypatch: pytest.MonkeyPatch) -> Iterator[threading.Event]:
    """Holds each event the sets' threads report until the event is set: the
    reader is then out of the set, and remove() gets NODATA."""
    if not _have_waitsets():
        pytest.skip("psmsgr_waitset_open returns NOTSUP here (no futex_waitv)")
    gate = threading.Event()
    deliver = _waitset._Set._deliver

    def held(s: Any, events: Any, n: int) -> None:
        gate.wait(10)
        deliver(s, events, n)

    monkeypatch.setattr(_waitset, "_sets", [])
    monkeypatch.setattr(_waitset._Set, "_deliver", held)
    yield gate
    gate.set()


def removals(monkeypatch: pytest.MonkeyPatch) -> list[int]:
    results: list[int] = []
    remove = _native.waitset_remove

    def recording_remove(ws: object, h: object) -> int:
        rc: int = remove(ws, h)
        results.append(rc)
        return rc

    monkeypatch.setattr(_native, "waitset_remove", recording_remove)
    return results


def test_event_wins_over_timeout(
    held_events: threading.Event,
    channel: tuple[StateWriter, StateReader],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    w, r = channel
    rcs = removals(monkeypatch)

    async def main() -> None:
        gen = w.publish(b"a")
        task = asyncio.create_task(r.wait_async(gen, 0.1))
        await asyncio.sleep(0.02)
        w.publish(b"b")
        await asyncio.sleep(0.2)  # the timer fired while the event is held
        assert rcs == [_native.E_NODATA]
        assert not task.done()
        with pytest.raises(RuntimeError, match="busy"):
            r.peek()  # the event still owns the reader
        held_events.set()
        assert await task is True

    run(main)


def test_event_wins_over_close(
    held_events: threading.Event, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    rcs = removals(monkeypatch)

    async def main() -> None:
        with StateWriter(CHAN, 8, directory=tmp_path) as w:
            gen = w.publish(b"a")
            r = StateReader(CHAN, directory=tmp_path)
            task = asyncio.create_task(r.wait_async(gen))
            await asyncio.sleep(0.02)
            w.publish(b"b")
            await asyncio.sleep(0.05)
            r.close()
            assert rcs == [_native.E_NODATA] and r.closed
            held_events.set()
            assert await task is True

    run(main)


def test_cancel_drops_raced_event(
    held_events: threading.Event,
    channel: tuple[StateWriter, StateReader],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    w, r = channel
    rcs = removals(monkeypatch)

    async def main() -> None:
        gen = w.publish(b"a")
        task = asyncio.create_task(r.wait_async(gen))
        await asyncio.sleep(0.02)
        old = r._waiter.reg  # type: ignore[union-attr]
        assert old is not None
        new = w.publish(b"b")
        await asyncio.sleep(0.05)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert rcs == [_native.E_NODATA]
        assert r.peek().generation == new  # the reader is the caller's again

        # Waits again at once: the stale registration's remove must not take
        # the reader out of the new one.
        again = asyncio.create_task(r.wait_async(new))
        await asyncio.sleep(0.02)
        assert old.remove() is False
        assert rcs == [_native.E_NODATA]
        held_events.set()  # the raced event goes to the cancelled wait
        await asyncio.sleep(0.05)
        assert not again.done()
        assert registered() == 1
        w.publish(b"c")
        assert await again is True

    run(main)


def test_set_failure_fails_its_waits(
    channel: tuple[StateWriter, StateReader], monkeypatch: pytest.MonkeyPatch
) -> None:
    if not _have_waitsets():
        pytest.skip("psmsgr_waitset_open returns NOTSUP here (no futex_waitv)")
    w, r = channel
    wait = _native.waitset_wait
    broken = threading.Event()
    broken.set()

    def failing_wait(*args: Any) -> int:
        if broken.is_set():
            time.sleep(0.01)
            return _native.E_INVAL
        rc: int = wait(*args)
        return rc

    monkeypatch.setattr(_waitset, "_sets", [])
    monkeypatch.setattr(_native, "waitset_wait", failing_wait)

    async def main() -> None:
        gen = w.publish(b"a")
        try:
            with pytest.raises(PsMsgrError) as e:
                await r.wait_async(gen, 10)
            assert e.value.code == ErrorCode.INVAL
            assert registered() == 0
            assert r.peek().generation == gen  # the reader is the caller's again
        finally:
            broken.clear()
        task = asyncio.create_task(r.wait_async(gen, 10))
        await asyncio.sleep(0.2)  # the set's thread is back in waitset_wait
        w.publish(b"b")
        assert await task is True

    run(main)
