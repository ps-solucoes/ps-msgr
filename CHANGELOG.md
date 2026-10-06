# Changelog

All notable changes to this project are documented in this file. The format
is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
library follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
as described in [`spec/README.md`](spec/README.md).

## [Unreleased]

### Added

- `libpsmsgr` 1.1: waitsets (`<psmsgr/waitset.h>`, symbol version
  `PSMSGR_1.1`). One caller-owned thread waits for up to 127 readers at
  once with `futex_waitv` and gets one event per finished reader, so that
  an event loop or async runtime doesn't need a blocked thread per reader.
  `psmsgr_waitset_open` returns `NOTSUP` without `futex_waitv` (Linux <
  5.16, qemu-user). The library still creates no threads.
- `ps_msgr`: `StateReader.wait_async`, an `asyncio` wait on waitsets (one
  daemon thread per 127 readers, a thread per wait where they are not
  supported). The binding now needs `libpsmsgr` 1.1.
- `psmsgr` (Go): `Reader.WaitChan`, a `Wait` for `select` that delivers
  its result on a channel. The waits share waitsets, one goroutine per
  127 readers, with a goroutine per wait where `futex_waitv` is missing.
  The binding now needs `libpsmsgr` 1.1.
- `release` workflow: a `v*` tag creates the GitHub release, with the
  `.deb` packages, the `.nupkg` and the `.whl` attached.
- `PsMsgr`: `StateReader.WaitAsync`, completed by one background thread per
  waitset of 127 readers instead of a blocked thread per reader (a thread
  per wait without `futex_waitv`). The binding now needs `libpsmsgr` 1.1.

## [1.0.0] - 2026-09-25

### Added

- `libpsmsgr`: state channels (channel format 1.0), which publish the latest
  value of a payload from one writer to many readers through a shared-memory
  file.
- `psmsgr-dump`: inspect a channel.
- `psmsgr-bench`: on-target latency benchmark.
- Debian packages for amd64 and armhf, a CMake package config and `psmsgr.pc`.
- `ps_msgr`: the Python binding (`ctypes`, Python ≥ 3.11, no dependencies),
  in `bindings/python/`.
- `PsMsgr`: the C# binding (P/Invoke, `netstandard2.1`, Native AOT
  compatible, no dependencies), in `bindings/csharp/`. CI builds its NuGet
  package.
- Build container: the .NET SDK 10 from Microsoft's apt repository.
- `psmsgr`: the Go binding (cgo, Go ≥ 1.24, no dependencies, optionally
  linked statically with the `psmsgr_static` build tag), in `bindings/go/`,
  and a CI job that also runs its tests for `linux/arm` under qemu. The
  build container gets trixie's `golang-go`.
- `interop/`: cross-language tests with a C, a Python and a C# (Native AOT)
  agent, every writer against every reader, and a CI job for them. They
  replace the interop tests of the Python and C# bindings.
- `examples/`: a writer and a reader of a motor status in C, Python and C#
  (Native AOT, also for `linux-arm`), all on one channel, so that any
  writer works with any reader. The C examples build in-tree
  (`PSMSGR_BUILD_EXAMPLES`) or against an installed library.
  `examples/check.sh` runs them against each other in CI.
- `bench/results/2026-09-24-bbb-bindings-ebc2ba0/`: first run of the Python
  and C# bindings (Native AOT for `linux-arm`), the interop suite and the
  examples on a BeagleBone Black.
- `bench/results/2026-09-25-bbb-go-803cba5/`: first run of the Go binding
  on a BeagleBone Black, linked dynamically and statically.
- `abi/`: `abidw` snapshots of the 1.0.0 public ABI for amd64 and armhf.

### On-target measurements

BeagleBone Black at `adea019`, `performance` governor at 1000 MHz, channels
in `/dev/shm`. `psmsgr-bench` without `chrt` (`bench.csv`):

```csv
# psmsgr: 1.0.0
# kernel: Linux 6.18.39-bone44-preditor.2 armv7l
# cpu: ARMv7 Processor rev 2 (v7l)
# hardware: Generic AM33XX (Flattened Device Tree)
# board: TI AM335x BeagleBone Black
# cpus: 1 online
# governor: performance, 1000 MHz
# clocksource: dmtimer
# cpuidle: arm_idle/ladder: WFI, mpu_gate
# dir: /dev/shm/psmsgr-bench-k0wB9N
# settings: iterations 100000, warm-up 1000, batch 1, 5.0 s at 500 Hz per run
test,size_bytes,n,min_ns,median_ns,p99_ns,max_ns,busy,missed
timer overhead,,100000,1291,1292,1334,185459,,
clock_gettime,,1000,1353,1353,1562,2404,,
clock_gettime syscall,,1000,1293,1293,1509,1749,,
getppid syscall,,1000,655,655,856,995,,
psmsgr_now_ns,,1000,1413,1413,1648,2725,,
publish NO_NOTIFY,16,100000,2958,3000,3000,372916,,
fill/publish NO_NOTIFY,16,100000,3000,3041,3042,191916,,
begin/fill/commit NO_NOTIFY,16,100000,3000,3000,3042,138084,,
begin/commit NO_NOTIFY,16,100000,2916,2959,3000,191042,,
publish,16,100000,5583,5708,5792,199500,,
read,16,100000,1750,1792,1834,220209,,
peek,16,100000,1709,1750,1792,138500,,
publish NO_NOTIFY,256,100000,3125,3166,3167,154458,,
fill/publish NO_NOTIFY,256,100000,3250,3292,3334,88917,,
begin/fill/commit NO_NOTIFY,256,100000,3083,3125,3167,501708,,
begin/commit NO_NOTIFY,256,100000,2958,3000,3000,139084,,
publish,256,100000,5833,5917,6042,189791,,
read,256,100000,1958,2000,2042,154291,,
peek,256,100000,1708,1750,1792,464333,,
publish NO_NOTIFY,4096,100000,6708,6958,7500,3004417,,
fill/publish NO_NOTIFY,4096,100000,7917,8167,8791,447708,,
begin/fill/commit NO_NOTIFY,4096,100000,5458,5500,5875,1270667,,
begin/commit NO_NOTIFY,4096,100000,2916,2959,3000,993083,,
publish,4096,100000,9666,10500,11125,134625,,
read,4096,100000,5833,6041,6208,6906583,,
peek,4096,100000,1708,1750,1750,53167,,
publish NO_NOTIFY,65536,100000,70083,73000,93583,3903833,,
fill/publish NO_NOTIFY,65536,100000,90084,91458,115292,3951958,,
begin/fill/commit NO_NOTIFY,65536,100000,45291,46375,65500,2200625,,
begin/commit NO_NOTIFY,65536,100000,2958,2959,3000,147959,,
publish,65536,100000,73292,75833,99500,6755917,,
read,65536,100000,71041,71917,92208,3230917,,
peek,65536,100000,1708,1750,1750,148417,,
read (writer active),16,1942155,1791,2500,2833,2245209,0,
read (writer active),256,1787603,2000,2708,3042,2593250,0,
read (writer active),4096,727379,5083,6666,7333,1537750,0,
read (writer active),65536,53492,71125,72417,383917,3408084,0,
wait wake-up,16,2499,43333,148291,158875,970042,,0
poll wake-up (100 us),16,2498,36875,103709,535833,2232250,,1
```

`bench-fifo.csv` is a second run under `chrt -f 50`, and
`bench-batch100.csv` one with `--batch 100`. The torture test passed all 7
tests at 1 s and at 60 s per variant. All of it is in
[`bench/results/2026-09-25-bbb-adea019/`](bench/results/2026-09-25-bbb-adea019/).
