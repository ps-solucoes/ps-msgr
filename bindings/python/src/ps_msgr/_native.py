# SPDX-License-Identifier: Apache-2.0
"""ctypes declarations for libpsmsgr.so.1 (include/psmsgr/psmsgr.h, state.h,
waitset.h)."""

from __future__ import annotations

import ctypes
import os
from ctypes import (
    CDLL,
    POINTER,
    PyDLL,
    Structure,
    c_char_p,
    c_int,
    c_int32,
    c_uint32,
    c_uint64,
    c_void_p,
)
from typing import Any

SONAME = "libpsmsgr.so.1"
VERSION_MAJOR = 1
MIN_VERSION_MINOR = 1

OK = 0
E_INVAL = -1
E_SYS = -2
E_NODATA = -3
E_TOOSMALL = -4
E_TOOBIG = -5
E_BUSY = -6
E_TIMEOUT = -7
E_INTR = -8
E_WRITER_EXISTS = -9
E_MISMATCH = -10
E_FORMAT = -11
E_NOTSUP = -12
E_STATE = -13

NAME_MAX = 64
STATE_MAX_CAPACITY = 16 << 20
STATE_DEFAULT_SLOTS = 3
STATE_RECREATE = 1 << 0
STATE_NO_NOTIFY = 1 << 1
INFO_ATTACHED = 1 << 0
WAITSET_MAX = 127


class StateOptions(Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("capacity", c_uint32),
        ("slot_count", c_uint32),
        ("payload_type", c_uint32),
        ("mode", c_uint32),
        ("flags", c_uint32),
        ("dir", c_char_p),
    ]


class StateInfo(Structure):
    _fields_ = [
        ("generation", c_uint32),
        ("length", c_uint32),
        ("timestamp_ns", c_uint64),
        ("flags", c_uint32),
        ("reserved", c_uint32),
    ]


class StateDesc(Structure):
    _fields_ = [
        ("capacity", c_uint32),
        ("slot_count", c_uint32),
        ("payload_type", c_uint32),
        ("flags", c_uint32),
    ]


class WaitsetEvent(Structure):
    _fields_ = [
        ("token", c_uint64),
        ("status", c_int32),
        ("generation", c_uint32),
        ("sys_errno", c_int32),
        ("reserved", c_uint32),
    ]


class Writer(Structure):
    """Opaque psmsgr_state_writer."""


class Reader(Structure):
    """Opaque psmsgr_state_reader."""


class Waitset(Structure):
    """Opaque psmsgr_waitset."""


WriterPtr = POINTER(Writer)
ReaderPtr = POINTER(Reader)
WaitsetPtr = POINTER(Waitset)


def check_version(version: int, path: str) -> None:
    major, minor, patch = version >> 16, (version >> 8) & 0xFF, version & 0xFF
    if major != VERSION_MAJOR or minor < MIN_VERSION_MINOR:
        raise ImportError(
            f"ps_msgr needs libpsmsgr {VERSION_MAJOR}.{MIN_VERSION_MINOR} or a later "
            f"{VERSION_MAJOR}.x, but {path} is {major}.{minor}.{patch}"
        )


def _load() -> tuple[CDLL, PyDLL, str]:
    path = os.environ.get("PSMSGR_LIBRARY") or SONAME
    try:
        # The same library twice: PyDLL calls keep the GIL, CDLL calls release it.
        return CDLL(path, use_errno=True), PyDLL(path, use_errno=True), path
    except OSError as e:
        raise ImportError(
            f"ps_msgr cannot load {path}: {e} (install libpsmsgr1, or set "
            "PSMSGR_LIBRARY to the path of libpsmsgr.so.1)"
        ) from e


_blocking, _fast, library_path = _load()


def _fn(lib: CDLL, name: str, restype: Any, *argtypes: Any) -> Any:
    try:
        f = getattr(lib, name)
    except AttributeError as e:
        raise ImportError(f"ps_msgr: {library_path} lacks {name}") from e
    f.restype = restype
    f.argtypes = argtypes
    return f


# Calls that only touch the mapping, or make a short syscall, keep the GIL:
# releasing and reacquiring it costs more than the call, and with other busy
# threads reacquiring can take a whole switch interval. Calls that block
# (wait, and the waitset's add and remove, which wait for its scan) or do file
# system work (open, unlink, writer_alive) release it.
version = _fn(_fast, "psmsgr_version", c_uint32)
check_version(version(), library_path)

strerror = _fn(_fast, "psmsgr_strerror", c_char_p, c_int)
now_ns = _fn(_fast, "psmsgr_now_ns", c_uint64)

state_options_init_sized = _fn(
    _fast, "psmsgr_state_options_init_sized", None, POINTER(StateOptions), c_uint32
)
state_writer_open = _fn(
    _blocking,
    "psmsgr_state_writer_open",
    c_int,
    c_char_p,
    POINTER(StateOptions),
    POINTER(WriterPtr),
)
state_writer_close = _fn(_fast, "psmsgr_state_writer_close", None, WriterPtr)
state_publish = _fn(
    _fast, "psmsgr_state_publish", c_int, WriterPtr, c_void_p, c_uint32, POINTER(c_uint32)
)
state_begin = _fn(_fast, "psmsgr_state_begin", c_int, WriterPtr, POINTER(c_void_p))
state_commit = _fn(_fast, "psmsgr_state_commit", c_int, WriterPtr, c_uint32, POINTER(c_uint32))
state_abort = _fn(_fast, "psmsgr_state_abort", c_int, WriterPtr)
state_writer_capacity = _fn(_fast, "psmsgr_state_writer_capacity", c_uint32, WriterPtr)

state_reader_open = _fn(
    _blocking, "psmsgr_state_reader_open", c_int, c_char_p, c_char_p, POINTER(ReaderPtr)
)
state_reader_close = _fn(_fast, "psmsgr_state_reader_close", None, ReaderPtr)
state_read = _fn(
    _fast, "psmsgr_state_read", c_int, ReaderPtr, c_void_p, c_uint32, POINTER(StateInfo)
)
state_peek = _fn(_fast, "psmsgr_state_peek", c_int, ReaderPtr, POINTER(StateInfo))
state_wait = _fn(_blocking, "psmsgr_state_wait", c_int, ReaderPtr, c_uint32, c_int32)
state_writer_alive = _fn(_blocking, "psmsgr_state_writer_alive", c_int, ReaderPtr)
state_describe_sized = _fn(
    _fast, "psmsgr_state_describe_sized", c_int, ReaderPtr, POINTER(StateDesc), c_uint32
)
state_unlink = _fn(_blocking, "psmsgr_state_unlink", c_int, c_char_p, c_char_p)

waitset_open = _fn(_fast, "psmsgr_waitset_open", c_int, POINTER(WaitsetPtr))
waitset_close = _fn(_fast, "psmsgr_waitset_close", None, WaitsetPtr)
waitset_add = _fn(_blocking, "psmsgr_waitset_add", c_int, WaitsetPtr, ReaderPtr, c_uint32, c_uint64)
waitset_remove = _fn(_blocking, "psmsgr_waitset_remove", c_int, WaitsetPtr, ReaderPtr)
waitset_wait = _fn(
    _blocking,
    "psmsgr_waitset_wait",
    c_int,
    WaitsetPtr,
    c_int32,
    POINTER(WaitsetEvent),
    c_uint32,
    POINTER(c_uint32),
)
waitset_wake = _fn(_fast, "psmsgr_waitset_wake", None, WaitsetPtr)

# Runs pending Python signal handlers; raises what a handler raised.
check_signals = ctypes.pythonapi.PyErr_CheckSignals
check_signals.restype = c_int
check_signals.argtypes = ()
