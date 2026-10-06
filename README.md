# ps-msgr

Inter-process state exchange for embedded Linux, with a primary target of
the BeagleBone Black. The core is one C library (`libpsmsgr`) with thin
Python, C# and Go bindings.

**State channels** publish the latest value of an opaque payload from one
writer to many readers through a shared-memory file in `/dev/shm`:

- Lock-free reads that never see a torn value.
- No syscalls on the hot path.
- Generation and timestamp without copying the payload (`peek`).
- Optional blocking `wait`, also for many readers at once from one thread
  (waitset, Linux 5.16).
- Readers can start before the writer.

Status: the C library with state channels, `psmsgr-dump`, `psmsgr-bench`,
the Python binding and the C# binding are implemented and tested with the
CI presets in the build container (x86-64, armhf under qemu); the CI
workflow, which adds arm64, has not run for this release. On a BeagleBone
Black, the C library passed the torture test and was benchmarked
([`bench/results/2026-09-25-bbb-adea019/`](bench/results/2026-09-25-bbb-adea019/)),
and the bindings, the interop suite and the examples ran, with one
timing-sensitive interop test failing
([`bench/results/2026-09-24-bbb-bindings-ebc2ba0/`](bench/results/2026-09-24-bbb-bindings-ebc2ba0/REPORT.md)).
The Go binding is tested in the build container (x86-64, and linux/arm
under qemu) and passed its tests on the board
([`bench/results/2026-09-25-bbb-go-803cba5/`](bench/results/2026-09-25-bbb-go-803cba5/REPORT.md)).
See [`spec/`](spec/README.md).

| Directory | Contents |
|---|---|
| `spec/` | Channel format, protocol, APIs, build and test requirements |
| `include/`, `src/`, `tests/`, `cmake/` | `libpsmsgr`: headers, implementation, C tests, build support |
| `tools/` | `psmsgr-dump`: inspect a channel |
| `bench/` | `psmsgr-bench`: latency on the target ([how to run it](bench/README.md)) |
| `bindings/python/` | `ps_msgr`: ctypes binding ([usage](bindings/python/README.md)) |
| `bindings/csharp/` | `PsMsgr`: P/Invoke binding, Native AOT compatible ([usage](bindings/csharp/README.md)) |
| `bindings/go/` | `psmsgr`: cgo binding, optionally linked statically ([usage](bindings/go/README.md)) |
| `examples/` | A writer and a reader in C, Python and C#, all on one channel ([how to run them](examples/README.md)) |
| `docker/` | Build container and `run.sh` |

## Building

All builds run in a Debian trixie container (`docker/build.Dockerfile`),
the same one CI uses. `docker/run.sh` builds the image on first use and runs
a command in it with the repository mounted:

```sh
docker/run.sh cmake --workflow --preset dev            # host: debug, ASan+UBSan, tests
PSMSGR_NO_ASLR=1 docker/run.sh cmake --workflow --preset tsan  # ThreadSanitizer (needs ASLR off)
docker/run.sh cmake --workflow --preset armhf          # BeagleBone Black: tests under qemu
docker/run.sh cmake --workflow --preset armhf-release  # + libpsmsgr1, -dev and psmsgr-tools .debs
docker/run.sh bindings/python/check.sh                 # Python binding, after the release preset
docker/run.sh bindings/csharp/check.sh                 # C# binding, after the release preset
docker/run.sh bindings/go/check.sh                     # Go binding, after the release preset
docker/run.sh examples/check.sh                        # examples, after the release preset
```

Presets: `dev`, `dev-clang`, `tsan`, `release`, `armhf` and `armhf-release`.
See [`spec/build-and-test.md`](spec/build-and-test.md).

License: Apache-2.0.
