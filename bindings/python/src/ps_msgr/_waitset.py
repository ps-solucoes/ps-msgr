# SPDX-License-Identifier: Apache-2.0
"""The waitsets behind ``StateReader.wait_async``: one daemon thread per set
of up to PSMSGR_WAITSET_MAX readers, started on first use, waits for all of
them and hands each event to a callback."""

from __future__ import annotations

import ctypes
import itertools
import os
import threading
import time
from collections.abc import Callable
from ctypes import byref, c_uint32
from typing import Any

from . import _native
from ._errors import error

# Called with (status, generation, sys_errno) on the set's thread.
Callback = Callable[[int, int, int], None]

# True once psmsgr_waitset_open returned NOTSUP (no futex_waitv): callers then
# wait with a thread per reader. Tests set it to exercise that path.
unsupported = False

# Guards the list of sets and each set's registrations.
_lock = threading.Lock()
_sets: list[_Set] = []
_tokens = itertools.count(1)


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
    __slots__ = ("pending", "thread", "ws")

    def __init__(self, ws: Any) -> None:
        self.ws = ws
        self.pending: dict[int, Registration] = {}
        self.thread: threading.Thread | None = None

    def run(self) -> None:
        events = (_native.WaitsetEvent * _native.WAITSET_MAX)()
        n = c_uint32()
        while True:
            rc = _native.waitset_wait(self.ws, -1, events, len(events), byref(n))
            if rc == _native.OK:
                self._deliver(events, n.value)
            elif rc != _native.E_INTR:
                self._fail(rc, ctypes.get_errno())
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
            reg.callback(status, generation, sys_errno)

    def _fail(self, rc: int, sys_errno: int) -> None:
        # Not expected (the set is valid and only this thread waits): fail the
        # registrations instead of leaving them hanging.
        with _lock:
            regs = list(self.pending.values())
        for reg in regs:
            if reg.remove():
                reg.callback(rc, 0, sys_errno)


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
        reg = Registration(s, h, next(_tokens), callback)
        # Before the add: the event can come before the add returns.
        s.pending[reg.token] = reg
    rc = _native.waitset_add(s.ws, h, last_generation, reg.token)
    if rc != _native.OK:
        with _lock:
            del s.pending[reg.token]
        raise error(rc)
    return reg


def _after_fork_in_child() -> None:
    # The sets' threads do not exist in the child: start over with new sets.
    # The old ones are left alone, since a thread may have held their locks.
    global _lock, _sets
    _lock = threading.Lock()
    _sets = []


os.register_at_fork(after_in_child=_after_fork_in_child)
