# SPDX-License-Identifier: Apache-2.0
"""The ctypes mirrors against the C header, and loading the library."""

from __future__ import annotations

import ctypes
import subprocess
import sys
from pathlib import Path

import pytest

from ps_msgr import ErrorCode, _native


@pytest.fixture(scope="module")
def c_layout(helper: Path) -> dict[tuple[str, str], int]:
    out = subprocess.run([helper, "layout"], check=True, capture_output=True, text=True).stdout
    layout = {}
    for line in out.splitlines():
        kind, key, value = line.split()
        layout[kind, key] = int(value)
    return layout


@pytest.mark.parametrize(
    ("c_name", "mirror"),
    [
        ("psmsgr_state_options", _native.StateOptions),
        ("psmsgr_state_info", _native.StateInfo),
        ("psmsgr_state_desc", _native.StateDesc),
        ("psmsgr_waitset_event", _native.WaitsetEvent),
    ],
)
def test_struct_layout(
    c_layout: dict[tuple[str, str], int], c_name: str, mirror: type[ctypes.Structure]
) -> None:
    assert ctypes.sizeof(mirror) == c_layout["sizeof", c_name]
    c_offsets = {
        key.split(".", 1)[1]: value
        for (kind, key), value in c_layout.items()
        if kind == "offsetof" and key.startswith(c_name + ".")
    }
    offsets = {name: getattr(mirror, name).offset for name, *_ in mirror._fields_}
    assert offsets == c_offsets


def test_constants(c_layout: dict[tuple[str, str], int]) -> None:
    consts = {key: value for (kind, key), value in c_layout.items() if kind == "const"}
    assert consts.pop("PSMSGR_VERSION_MAJOR") == _native.VERSION_MAJOR
    assert consts.pop("PSMSGR_VERSION_MINOR") >= _native.MIN_VERSION_MINOR
    for key, value in consts.items():
        assert getattr(_native, key.removeprefix("PSMSGR_")) == value, key
        if key.startswith("PSMSGR_E_"):
            assert ErrorCode[key.removeprefix("PSMSGR_E_")] == value


def test_options_init_sets_struct_size() -> None:
    opt = _native.StateOptions()
    ctypes.memset(ctypes.byref(opt), 0xA5, ctypes.sizeof(opt))
    _native.state_options_init_sized(ctypes.byref(opt), ctypes.sizeof(opt))
    assert opt.struct_size == ctypes.sizeof(opt)
    assert (opt.capacity, opt.slot_count, opt.payload_type, opt.mode, opt.flags, opt.dir) == (
        0,
        _native.STATE_DEFAULT_SLOTS,
        0,
        0o644,
        0,
        None,
    )


def test_version_check() -> None:
    _native.check_version(_native.version(), "lib")
    _native.check_version((1 << 16) | (_native.MIN_VERSION_MINOR << 8) | 7, "lib")
    for bad in (2 << 16, 0 << 16 | 9 << 8, 1 << 16 | (_native.MIN_VERSION_MINOR - 1) << 8 | 9):
        with pytest.raises(ImportError, match=r"needs libpsmsgr 1\.\d+ or a later 1\.x"):
            _native.check_version(bad, "lib")


def test_import_fails_clearly_without_library(tmp_path: Path, child_env: dict[str, str]) -> None:
    missing = tmp_path / "libpsmsgr.so.1"
    p = subprocess.run(
        [sys.executable, "-c", "import ps_msgr"],
        env={**child_env, "PSMSGR_LIBRARY": str(missing)},
        capture_output=True,
        text=True,
    )
    assert p.returncode != 0
    assert f"ImportError: ps_msgr cannot load {missing}" in p.stderr
