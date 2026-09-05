# Brightness CLI investigation

## Scope and constraints

- Target: ASUS ProArt PA249CGV, connected by USB-C to an M4 Mac mini running macOS Tahoe.
- User initially reported an OSD scale of 0–200, then checked and corrected it to 0–400. m1ddc had returned 0 when the OSD showed 10.
- Priority: minimize the possibility of sending unintended monitor commands.
- ASUS artifacts are reference data only. Neither the executable nor the dynamic library was executed or loaded into a process.
- Initial work was static analysis. A source-built diagnostic was subsequently used for two authorized brightness queries. An approved brightness change from 20 to 21 was confirmed by a subsequent validated query and by the user in the OSD. The setter is implemented with bounded readback recovery and tested offline.

## Reference artifacts

Both files contain x86_64 and ARM64 Mach-O slices. The analysis below concerns ARM64, matching the target Mac.

| File | SHA-256 |
| --- | --- |
| `reference/dwc` | `541992fea0405aa0e2ccd295f31539e74d7e03e7dad593f080ca0014a2ad64ec` |
| `reference/libVCPLibrary.dylib` | `6e8103bdab1e0a271c12bde3e7bd1e1f0ef4f697c81b0e0f7a5aa494ae576b45` |

Tools used: `file`, `shasum`, `otool`, `nm`, `strings`, and Apple's `swift-demangle`, plus text processing. Existing tools were sufficient; no installation was needed.

## Findings from the supplied files

1. `dwc` links to `@rpath/libVCPLibrary.dylib`, with `@executable_path` as a runtime search path.
2. Its embedded help identifies version 0.1.0 and describes brightness as 0–100. That is help text, not evidence of this monitor's native range.
3. `dwc`'s `_SetBrightness` at ARM64 virtual address `0x100001e38` passes the supplied value to `SetVCPInternal` with feature code `0x10`. `_GetBrightness` at `0x100002cd8` uses the same feature code.
4. The ordinary Mac path reaches `VCPLib_SetVCP` / `VCPLib_GetVCP` in the library. The inspected wrapper path does not rescale brightness.
5. In the library, `VCPLib_SetVCP` at `0x2564c` forwards the value to `Arm64DDC.SetVCPFeature`. Instructions at `0x257b0`–`0x257cc` check that the feature fits in 8 bits and the value fits in 16 bits. This is not a monitor-specific range check.
6. `Arm64DDC.SetVCPFeature` at `0xb7a8` constructs a payload containing the feature followed by the high and low bytes of the value. `PerformDDCCommunication` at `0xad18` constructs the DDC framing and XOR checksum. At `0xaf84`–`0xaf98`, it invokes the I2C write layer with destination `0x37` and source `0x51`.
7. The ARM64 transport calls Apple's `IOAVServiceWriteI2C` and `IOAVServiceReadI2C`. Its code includes a hardware-dependent alternate-address path involving `0x137`. That behavior should not be copied indiscriminately into a CLI intended for one USB-C connection.
8. `Arm64DDC.GetVCPFeature` at `0xb688` requests an 11-byte response and extracts maximum and current values from bytes 6–7 and 8–9 respectively, as big-endian 16-bit values. The communication routine checks the reply checksum.

No 0–100 clamp or percentage conversion was found in the inspected brightness transport path. These findings establish how the supplied code encodes requests; they do not establish the monitor firmware's range, the OSD-to-DDC mapping, or successful operation on this specific connection. This was a focused investigation, not an audit of every library function.

## Relevant upstream comparison

Current upstream m1ddc supports querying the maximum and passes absolute set values as 16-bit integers. Its relative-change operation uses the reported maximum, rather than a fixed limit of 100. This does not establish which version the user previously ran or explain the observed mismatch.

- [m1ddc command handling](https://github.com/waydabber/m1ddc/blob/main/sources/m1ddc.m)
- [m1ddc DDC packet handling](https://github.com/waydabber/m1ddc/blob/main/sources/i2c.m)
- [ASUS CLI reference](https://github.com/ASUS-Display/asus-display-control/blob/main/CLI_REFERENCE.md)

## Implementation design

A small ARM64 CLI built from source, with pure C protocol and controller layers and a minimal macOS transport layer. The ASUS files are only protocol evidence, without linking or launching them.

Public operations are `get`, `set <integer>`, and metadata-only `identify`, with an optional `--diagnose` flag for get/set. The corrected input range is 0–400, in raw monitor units. Both hardware queries matched the OSD directly at their sampled settings.

Required safeguards:

- Restrict monitor-setting writes to brightness feature `0x10`; provide no generic feature selector, resets, calibration controls, input switching, or power controls.
- Identify the PA249CGV through OS-provided display identity and bind that identity to the specific transport service. Refuse absent, mismatched, or ambiguous targets; never fall back to the first display.
- Parse complete decimal integers strictly and reject negative, overflowing, trailing-junk, and out-of-range input before hardware access.
- Validate the full brightness reply: expected framing, length, checksum, result, feature echo, type, and plausible current/maximum values. Refuse writes on invalid reads or unexpected range.
- Use exact packet lengths and fixed brightness request construction. Do not probe arbitrary VCP codes or alternate I2C addresses.
- Serialize transactions, use bounded delays and read attempts, and avoid automatic repeated setting writes. Read back after a setting write; report verification failure without guessing additional commands or automatic rollback.

## Validation sequence

1. Implement and test without hardware access. Verify exact packet bytes across the full intended range, malformed replies, integer parsing, and identity checks. Invalid arguments must cause no device IO; rejected preflight checks must cause no setting write.
2. Review the source and the exact proposed diagnostic request. The first device operation should only query brightness `0x10`, after confirming the target using OS metadata. A DDC query still sends a request over I2C, although it does not request a setting change.
3. Compare raw current/maximum values with the OSD. If they differ, use additional readings at user-selected OSD settings to establish the mapping rather than assume a factor of two.
4. Once the mapping and range are verified, perform one explicitly selected, small brightness change and read it back. Routine CLI use can then remain simple.

Source and offline tests are implemented. Static inspection and offline tests alone cannot establish hardware correctness across the full range.

## Diagnostic implementation and first hardware result

Initially implemented `src/protocol.c` and `src/main.c`, built as `build/clibrightness`.
The initial executable linked only IOKit, CoreFoundation, and libSystem. Its only I2C
write site sent the fixed Get VCP Feature request for brightness, with no
setting-write builder or command. `identify` uses cached OS metadata without
opening an IOAVService or issuing a DDC request.

Target selection requires one PA249CGV with manufacturer AUS, vendor ID 0x06b3,
product ID 0xaa36, and a nonempty alphanumeric serial. It binds the framebuffer's
`dispextN` route to an exact `dispextN:dcpav-service-epic:0` endpoint with provider
`DCPDP13Service`, external location and unit 0. This intentionally supports only
the observed native USB-C topology. It checks the identity again before opening
the service. Other or ambiguous topologies fail rather than use a default device.

Offline protocol tests passed with address and undefined-behavior sanitizers.
Coverage includes the full 0–200 range, genuine zero, all-zero and all-0xff buffers,
truncated/oversized replies, every single-bit corruption, well-checksummed replies
with wrong fields, and 16-bit values above 255. CLI help and invalid argument
checks also passed; invalid arguments are rejected before discovery or device IO.

The initial sandboxed hardware attempt could not open the IOAVService and sent
no request. The user approved running the exact diagnostic outside the sandbox.
The subsequent single query succeeded:

```text
Chip address: 0x37; source/data address: 0x51
Request buffer: 82 01 10 ac
Read offset: 0; requested response length: 11
Reply: 6e 88 02 00 10 00 01 90 00 0a 3f
Decoded: current 10, maximum 400
```

The response has valid framing, checksum, result, brightness feature echo, and
continuous-control type. This establishes a working brightness query and a raw
reported maximum of 400 in the current monitor state. It does not establish the
full OSD mapping. The user subsequently corrected the OSD maximum to 400.
No setting change has been sent.

Current upstream m1ddc's read request uses checksum seed 0x6e without the 0x51
source term (producing 0xfd for this request), and uses read offset 0x51. The
inspected ASUS implementation uses checksum seed 0x3f (0x6e XOR 0x51), producing
0xac, and read offset 0. m1ddc's reply conversion extracts values without checking
the full response's validity. These are concrete differences and plausible
contributors to misleading zero output; the installed m1ddc version and its
actual request/response have not been inspected, so causation is not proven.

## Second query and completed setter implementation

The user confirmed the first OSD reading was 10, then manually selected 20.
A second approved query using the original read-only executable returned:

```text
Request: 82 01 10 ac
Reply: 6e 88 02 00 10 00 01 90 00 14 21
Decoded: current 20, maximum 400
```

The user also checked the OSD scale and confirmed its maximum is 400. The CLI
therefore uses direct values 0–400 rather than scaling values to percentages.

The final source adds `src/controller.c` and a fixed brightness setter packet
builder. The actual transport layer has two write sites: one fixed Get VCP
brightness request, and one fixed Set VCP brightness request. Neither accepts
arbitrary VCP features or I2C addresses. The controller validates identity and
the complete preflight reply, requires maximum 400, rechecks identity before
writing, sends at most one setting request, and validates the readback. If the
requested value already matches the current value, it performs no setting write.
Failures after a setting attempt report uncertainty and never trigger an
automatic retry or rollback.

`make all test` passes with strict compiler warnings as errors. The offline
protocol and controller tests run with address and undefined-behavior sanitizers.
They cover all 401 integer values, fixed packet/checksum fixtures including the
255/256 byte boundary, strict argument parsing, invalid ranges and replies,
identity changes, transport failures including a write-applied-but-error result,
stale readback, and confirmation that rejected preflight operations send no
setting write. Clang's static analyzer reported no findings. Native CLI invalid
argument and help checks passed without executing a get or set operation.

For the approved first setting test, raw brightness 21 used the following fixed
request buffer:

```text
84 03 10 00 15 bd
```

Only the exact requested value was sent; it was not a relative increase.

## First setting test and readback recovery

The user approved `set 21 --diagnose`. Preflight returned a valid 20/400, then
one brightness setting write was sent. The immediate readback was malformed:

```text
Preflight reply: 6e 88 02 00 10 00 01 90 00 14 21
Setting request: 84 03 10 00 15 bd
Immediate reply: 01 10 ac 00 00 00 00 00 00 00 00
```

The CLI exited with an uncertainty error and did not resend the setting. An
approved subsequent standalone brightness query returned:

```text
Reply: 6e 88 02 00 10 00 01 90 00 15 20
Decoded: current 21, maximum 400
```

The user independently confirmed that the OSD increased to 21. Setting control
works; the initial immediate verification was not reliable. To accommodate
settling/transient replies, verification now waits 200 ms before each of at most
three brightness queries (in addition to the query transport's 50 ms delays).
It rechecks identity before each query and stops on a validated range change.
Only verification reads are retried, never the setting write. Tests replay the
observed malformed response followed by a valid reply, and prove recovery with
exactly one setting write. Persistent errors stop after three readback attempts.
The updated build passes sanitized tests and Clang static analysis.

## Final hardware verification

The user approved a final `set 20 --diagnose` with the updated binary. It
completed successfully, requiring only the first readback attempt:

```text
Preflight reply: 6e 88 02 00 10 00 01 90 00 15 20
Setting request: 84 03 10 00 14 bc
Readback reply: 6e 88 02 00 10 00 01 90 00 14 21
Verified: 21 -> 20 (maximum 400)
Stdout: 20/400
Exit status: 0
```

The completed CLI supports get and set in direct units 0–400, with matching,
validation, single setting writes and bounded readback verification. The final
hardware test left the monitor at 20/400. No ASUS artifact was executed, loaded,
or linked. Hardware checks sampled brightness values 10, 20 and 21; they do not
constitute exhaustive hardware testing across the full range or other modes,
ports, adapters, firmware, or macOS versions.
