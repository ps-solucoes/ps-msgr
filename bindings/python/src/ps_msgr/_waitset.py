# SPDX-License-Identifier: Apache-2.0
"""The waitsets behind ``StateReader.wait_async``: one daemon thread per set
of up to PSMSGR_WAITSET_MAX readers, started on first use, waits for all of
them and hands each event to a callback. Without them, ``run_in_thread``
runs each wait on a daemon thread that the next wait reuses."""

from __future__ import annotations

import ctypes
import itertools
import os
import queue
import threading
import time
from collections.abc import Callable
from ctypes import byref, c_uint32
from typing import Any, TypeVar

from . import _native
from ._errors import error

# Called with (status, generation, sys_errno) on the set's thread.
Callback = Callable[[int, int, int], None]

# True once psmsgr_waitset_open returned NOTSUP (no futex_waitv): callers then
# wait on the threads of run_in_thread. Tests set it to exercise that path.
unsupported = False

# Guards the list of sets and each set's registrations.
_lock = threading.Lock()
_sets: list[_Set] = []
_opened: list[_Set] = []  # also the sets that failed and left _sets
_tokens = itertools.count(1)

T = TypeVar("T")
# A run_in_thread call: the work and what gets its result.
_Item = tuple[Callable[[], Any], Callable[[Any], None]]

# The threads of run_in_thread waiting for work, the most recently idle last,
# so that the others reach their timeout.
_idle_lock = threading.Lock()
_idle: list[_WaitThread] = []
# Seconds without work after which such a thread ends; the tests change it.
idle_timeout = 10.0
# Threads run_in_thread started, for the tests.
started = 0


class Registration:
    """A reader in a set. The set owns the reader until the callback runs or
    ``remove()`` returns: exactly one of the two happens."""

    __slots__ = ("callback", "done", "h", "lock", "set", "token")

    def __init__(self, s: _Set, h: Any, token: int, callback: Callback) -> None:
        self.set = s
        self.h = h
        self.token = token
        self.callback = callback
        # Set by the first remove() or by the event, under the lock: the set
        # finds readers by address, so a later remove could unregister the
        # reader's next registration, or another reader at that address.
        self.lock = threading.Lock()
        self.done = False

    def remove(self) -> bool:
        """Unregisters the reader; it is the caller's again when this returns.
        True: the callback will not run. False: the set reported the reader
        first, and its thread runs (or ran) the callback; or this was removed
        already."""
        if self.set.forked:
            # A fork's child: the set's thread is gone and its locks may be
            # held, so the reader just leaves the set behind.
            self.done = True
            return True
        with self.lock:
            if self.done:
                return False
            self.done = True
            rc = _native.waitset_remove(self.set.ws, self.h)
        if rc == _native.E_NODATA:
            return False
        if rc != _native.OK:
            raise error(rc)
        with _lock:
            del self.set.pending[self.token]
        return True


class _Set:
    __slots__ = ("forked", "pending", "thread", "ws")

    def __init__(self, ws: Any) -> None:
        self.ws = ws
        self.pending: dict[int, Registration] = {}
        self.thread: threading.Thread | None = None
        self.forked = False  # inherited by a fork's child

    def run(self) -> None:
        events = (_native.WaitsetEvent * _native.WAITSET_MAX)()
        n = c_uint32()
        while True:
            rc = _native.waitset_wait(self.ws, -1, events, len(events), byref(n))
            if rc == _native.OK:
                self._deliver(events, n.value)
            elif rc != _native.E_INTR:
                sys_errno = ctypes.get_errno()
                # New waits go to other sets; this thread still serves the
                # adds that raced the failure.
                with _lock:
                    if self in _sets:
                        _sets.remove(self)
                self._fail(rc, sys_errno)
                time.sleep(0.1)

    # Separate functions, so that their locals don't keep the registrations
    # (and through them tasks and readers) alive while run() waits.

    def _deliver(self, events: Any, n: int) -> None:
        with _lock:
            done = [
                (self.pending.pop(e.token), e.status, e.generation, e.sys_errno) for e in events[:n]
            ]
        for reg, status, generation, sys_errno in done:
            with reg.lock:
                reg.done = True
            _call(reg.callback, status, generation, sys_errno)

    def _fail(self, rc: int, sys_errno: int) -> None:
        # Not expected (the set is valid and only this thread waits): fail the
        # registrations instead of leaving them hanging.
        with _lock:
            regs = list(self.pending.values())
        for reg in regs:
            _call(_fail_one, reg, rc, sys_errno)


def _fail_one(reg: Registration, rc: int, sys_errno: int) -> None:
    if reg.remove():
        reg.callback(rc, 0, sys_errno)


def _call(f: Callable[..., None], *args: Any) -> None:
    # The set's thread must outlive any one wait: an error is reported like
    # one that ends a thread, and the thread goes on.
    try:
        f(*args)
    except Exception as e:
        args = (type(e), e, e.__traceback__, threading.current_thread())
        threading.excepthook(threading.ExceptHookArgs(args))


def register(h: Any, last_generation: int, callback: Callback) -> Registration | None:
    """Adds a reader to a set with room, opening a set and starting its
    thread when none has room. None if waitsets are not supported."""
    global unsupported
    with _lock:
        if unsupported:
            return None
        s = next((s for s in _sets if len(s.pending) < _native.WAITSET_MAX), None)
        if s is None:
            ws = _native.WaitsetPtr()
            rc = _native.waitset_open(byref(ws))
            if rc == _native.E_NOTSUP:
                unsupported = True
                return None
            if rc != _native.OK:
                raise error(rc)
            s = _Set(ws)
            thread = threading.Thread(target=s.run, name="ps_msgr waitset", daemon=True)
            try:
                thread.start()
            except BaseException:
                _native.waitset_close(ws)
                raise
            # Only now: a set without its thread would never deliver.
            s.thread = thread
            _sets.append(s)
            _opened.append(s)
        reg = Registration(s, h, next(_tokens), callback)
        # Before the add: the event can come before the add returns.
        s.pending[reg.token] = reg
    rc = _native.waitset_add(s.ws, h, last_generation, reg.token)
    if rc != _native.OK:
        with _lock:
            del s.pending[reg.token]
        raise error(rc)
    return reg


class _WaitThread:
    __slots__ = ("timeout", "work")

    def __init__(self) -> None:
        self.work: queue.SimpleQueue[_Item] = queue.SimpleQueue()
        # idle_timeout when the thread last became idle.
        self.timeout = idle_timeout

    def run(self) -> None:
        while self._run_next():
            pass

    # A separate function, so that its locals don't keep the last wait (and
    # through it the reader) alive while the thread is idle.
    def _run_next(self) -> bool:
        item = self._take()
        if item is None:
            return False
        work, done = item
        result: Any
        try:
            result = work()
        except BaseException as e:
            result = e
        # Before done: a wait that it leads to gets this thread.
        with _idle_lock:
            self.timeout = idle_timeout
            _idle.append(self)
        _call(done, result)
        return True

    def _take(self) -> _Item | None:
        try:
            return self.work.get(timeout=self.timeout)
        except queue.Empty:
            pass
        with _idle_lock:
            if self in _idle:
                _idle.remove(self)
                return None
        # run_in_thread took this thread meanwhile: its work is on the way.
        return self.work.get()


def run_in_thread(work: Callable[[], T], done: Callable[[T | BaseException], None]) -> None:
    """Calls ``work()`` on an idle daemon thread, or on a new one if none is
    idle, then ``done`` with its result or exception. The thread is idle
    again before ``done`` runs, and ends after ``idle_timeout`` without
    work. Raises if a new thread cannot start."""
    global started
    with _idle_lock:
        t = _idle.pop() if _idle else None
    if t is None:
        t = _WaitThread()
        threading.Thread(target=t.run, name="ps_msgr wait", daemon=True).start()
        with _idle_lock:
            started += 1
    t.work.put((work, done))


def _after_fork_in_child() -> None:
    # The sets' threads do not exist in the child: start over with new sets.
    # The old ones are left alone, since a thread may have held their locks.
    # The idle threads are gone too: forget them and their lock.
    global _lock, _sets, _opened, _idle_lock, _idle
    for s in _opened:
        s.forked = True
    _lock = threading.Lock()
    _sets = []
    _opened = []
    _idle_lock = threading.Lock()
    _idle = []


os.register_at_fork(after_in_child=_after_fork_in_child)
