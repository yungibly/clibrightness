# clibrightness

A small brightness-only CLI for an ASUS ProArt PA249CGV connected directly by
USB-C to an M4 Mac mini. Values are **0–400 in the monitor's own units**, without
percentage conversion. It uses no ASUS runtime or third-party dependencies.

## Install

Requires an Apple Silicon Mac running **macOS Tahoe (26) or later**. Hardware
operation has been verified on an M4 Mac mini running Tahoe with a directly
connected PA249CGV. Other monitors and unrecognized transport routes are refused.

```sh
brew install yungibly/tap/clibrightness
```

Prebuilt ARM64 binaries and SHA-256 checksums are also available on the
[releases page](https://github.com/yungibly/clibrightness/releases).

To build from source with Apple's Command Line Tools:

```sh
make
make test
make analyze
```

No package downloads or ASUS libraries are required. Tests run entirely offline.
The source-built executable is `build/clibrightness`.

## Usage

```sh
clibrightness identify
clibrightness get
clibrightness set 20
clibrightness get --diagnose
```

`identify` uses cached macOS display metadata and sends no DDC request. `get`
sends one fixed brightness query and prints `current/maximum` in raw DDC units.
`set` first validates the current brightness reply and requires maximum 400.
It checks the display identity again, sends at most one brightness setting
request, waits for settling, and verifies the result with up to three brightness
queries. Only reads are retried; a setting write is never resent. Setting the current value
skips the setting write. `--diagnose` works with `get` and `set` and additionally
prints the target identity and request/reply bytes to stderr.

## Safeguards and limits

Malformed replies, unknown ranges, and ambiguous or changing display identity
produce an error. A failed or unverified setting write is never automatically
retried or rolled back: check the OSD. A nonzero exit status after an attempted
write does not imply that the monitor's value stayed unchanged.
Neither no arguments nor `--help` accesses the display. Unknown commands and
extra arguments are rejected before display discovery.

Hardware validation sampled brightness values **10, 20, and 21**, including
successful setting changes and readback. Offline tests cover encoding and
decoding all 401 values, malformed replies, changing identity, rejected writes,
and bounded verification after a single setting write. This is not exhaustive
hardware testing across all brightness levels, modes, firmware, or macOS versions.

The CLI deliberately supports only the observed native USB-C transport,
requires one uniquely identified PA249CGV, and refuses other/ambiguous routes.
It uses undocumented Apple IOAVService APIs, so macOS changes may require an
update. It does not require root; an application sandbox may deny device access.
Its lock serializes this CLI's own processes, not unrelated monitor-control apps.
The identity checks reduce hotplug risk but cannot make device changes or other
apps' activity atomic with an I2C transaction.

ASUS reference binaries were used for static analysis only. They are not
distributed, executed, loaded, or linked by this project.

See [docs/INVESTIGATION.md](docs/INVESTIGATION.md) for disassembly evidence, query
bytes, limitations, and implemented safeguards.

## Builds and releases

[GitHub Actions](https://github.com/yungibly/clibrightness/actions/workflows/build.yml)
compiles on macOS 26, runs the offline tests with AddressSanitizer and
UndefinedBehaviorSanitizer, runs Clang's static analyzer, and uploads an ARM64
archive. CLI smoke checks use only help and invalid arguments, without display IO.

To release, update `VERSION`, commit and push to `main`, then push a matching tag
such as `v0.1.0`. The same workflow publishes the tested binary and checksum,
then installs and tests the Homebrew formula on a GitHub macOS runner before
updating `Formula/clibrightness.rb` in `yungibly/homebrew-tap`.

The repository secret `HOMEBREW_TAP_TOKEN` must be a fine-grained token restricted
to `yungibly/homebrew-tap` with **Contents: read and write**. Local `.env` files
are ignored and are never part of the build or release artifacts.

Licensed under the [MIT License](LICENSE).
