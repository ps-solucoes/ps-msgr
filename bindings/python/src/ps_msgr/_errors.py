# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import ctypes
import os
from enum import IntEnum
from typing import Any

from . import _native


class ErrorCode(IntEnum):
    """The library's PSMSGR_E_* result codes."""

    INVAL = _native.E_INVAL
    SYS = _native.E_SYS
    NODATA = _native.E_NODATA
    TOOSMALL = _native.E_TOOSMALL
    TOOBIG = _native.E_TOOBIG
    BUSY = _native.E_BUSY
    TIMEOUT = _native.E_TIMEOUT
    INTR = _native.E_INTR
    WRITER_EXISTS = _native.E_WRITER_EXISTS
    MISMATCH = _native.E_MISMATCH
    FORMAT = _native.E_FORMAT
    NOTSUP = _native.E_NOTSUP
    STATE = _native.E_STATE


class PsMsgrError(OSError):
    """A failed library call. ``code`` is the PSMSGR_E_* result; for
    ``ErrorCode.SYS``, ``errno`` holds the error of the failing system call.
    ``filename`` is the channel name, where there is one."""

    code: int

    def __init__(
        self,
        code: int,
        message: str | None = None,
        errno: int | None = None,
        filename: str | None = None,
    ) -> None:
        if message is None:
            message = os.strerror(errno) if errno else strerror(code)
        if filename is None:
            super().__init__(errno, message)
        else:
            super().__init__(errno, message, filename)
        try:
            self.code = ErrorCode(code)
        except ValueError:
            self.code = code

    def __str__(self) -> str:
        s = self.strerror or ""
        if self.errno is not None:
            s = f"[Errno {self.errno}] {s}"
        if self.filename is not None:
            s = f"{s}: {self.filename!r}"
        return s

    def __repr__(self) -> str:
        args = ", ".join(map(repr, self.__reduce__()[1]))
        return f"{type(self).__name__}({args})"

    def __reduce__(self) -> tuple[Any, ...]:
        return type(self), (self.code, self.strerror, self.errno, self.filename)


class WriterExistsError(PsMsgrError):
    """Another writer holds the channel."""


class ChannelMismatchError(PsMsgrError):
    """The existing channel has another geometry; open with ``recreate=True``
    to replace it."""


class ChannelFormatError(PsMsgrError):
    """The channel file is not a valid channel of a supported format."""


class PayloadTooLargeError(PsMsgrError, ValueError):
    """The payload does not fit: larger than the channel's capacity
    (``ErrorCode.TOOBIG``), or than the buffer given to ``read_into``
    (``ErrorCode.TOOSMALL``)."""


class ChannelBusyError(PsMsgrError):
    """The writer kept overwriting the slot being read. Transient: retry."""


_CLASSES: dict[int, type[PsMsgrError]] = {
    _native.E_WRITER_EXISTS: WriterExistsError,
    _native.E_MISMATCH: ChannelMismatchError,
    _native.E_FORMAT: ChannelFormatError,
    _native.E_TOOBIG: PayloadTooLargeError,
    _native.E_TOOSMALL: PayloadTooLargeError,
    _native.E_BUSY: ChannelBusyError,
}


def strerror(code: int) -> str:
    return (_native.strerror(code) or b"").decode()


def error(
    code: int,
    filename: str | None = None,
    message: str | None = None,
    errno: int | None = None,
) -> PsMsgrError:
    """The exception for a result code. Without ``errno``, call right after
    the failing call: ``PSMSGR_E_SYS`` takes it from ctypes."""
    if code != _native.E_SYS:
        errno = None
    elif errno is None:
        errno = ctypes.get_errno()
    return _CLASSES.get(code, PsMsgrError)(code, message, errno, filename)
