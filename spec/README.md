# ps-msgr specifications

Status: **final**. Changes follow the versioning rules below.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in
RFC 2119. Sections marked *non-normative* are guidance only.

## Documents

| Document | Contents |
|---|---|
| [state-channel.md](state-channel.md) | Shared-memory layout and concurrency protocol of a state channel (the cross-language contract) |
| [c-api.md](c-api.md) | Public C API of `libpsmsgr` |
| [bindings.md](bindings.md) | Python, C# and Go bindings |
| [build-and-test.md](build-and-test.md) | Repository layout, build, packaging, tests, CI |

## Goals

- Exchange **latest-value state** between processes on one Linux host.
  Primary target: BeagleBone Black (TI AM335x, single-core Cortex-A8,
  ARMv7-A, armhf, 512 MiB RAM) running **Debian 13 (trixie)**. That is the
  only supported target OS; older releases are not supported, and boards
  still on older images get reflashed.
- One implementation of everything concurrency-sensitive, in C
  (`libpsmsgr`). The Python and C# packages are thin bindings over it; they
  MUST NOT reimplement the shared-memory protocol.
- Readers never block the writer, and the writer never blocks on readers.
- No syscalls on the read/peek hot path. Publish makes at most two: one
  optional futex wake, and `clock_gettime` where the vDSO can't serve it
  (the case on the AM335x).
- Correct on multi-core and weakly ordered CPUs too (x86-64 and AArch64
  development machines, CI runners, other boards), even though the primary
  target is single-core.

## Non-goals (for now)

- Message queues: every message delivered, in order. Use D-Bus for
  commands, events and requests that need a reply, or a Unix socket for a
  high-rate stream; ps-msgr carries latest-value state only.
- Communication across hosts, or across containers that don't share the
  shared-memory directory.
- Payload serialization. The payload is an opaque byte string owned by the
  application layer. The library carries a 32-bit `payload_type` tag so
  applications can detect schema mismatches, but never interprets it.
- Persistence across reboots. Channels live in tmpfs.
- Big-endian hosts.

## Naming

| Thing | Name |
|---|---|
| Repository | `ps-msgr` |
| C library / SONAME | `libpsmsgr` / `libpsmsgr.so.1` |
| C headers | `<psmsgr/psmsgr.h>` (umbrella), `<psmsgr/state.h>`, `<psmsgr/waitset.h>` |
| C symbol prefix | `psmsgr_`, `PSMSGR_` |
| Python distribution / import | `ps-msgr` / `ps_msgr` |
| C# package / namespace | `PsMsgr` |
| Channel file | `<dir>/psmsgr.<name>.state` (see state-channel.md) |

## Versioning

Three independent version numbers:

1. **Channel format version** (`version_major.version_minor` in the channel
   header). A minor bump MAY only add fields in reserved space, and those
   fields MUST read as zero on older files. Anything else is a major bump.
   Readers MUST reject a different major version.
2. **Library version** (SemVer). The library major version equals the
   SONAME number (`libpsmsgr.so.1`) and changes only on C ABI breaks. The
   first release is 1.0.0; until then nothing is stable.
3. **Binding package versions** (SemVer), independent of the library's.
   Each binding's first release is 1.0.0, together with library 1.0.0.
   A binding's major version changes on a breaking change to its own API,
   and whenever the library major version it loads changes, since it loads
   `libpsmsgr.so.<major>` by name. Each binding declares the minimum
   library version it needs and checks it at load time.
