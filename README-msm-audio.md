# Bounded ARM32 MSM AAC descriptor capture

The MSM AAC decoder emits PCM through a kernel metadata prefix and DSP frame
records. Generic ioctl decoding sees an encoded size of four bytes, while
AUDIO_GET_EVENT actually returns forty bytes. The opt-in decoder captures the
full event and its completed buffer so a codec investigation can compare the
kernel descriptor lengths, sample counts and timestamps with OMX output.
The decoder preserves every captured byte, including suspected stale tails.

## Admission and ownership

Set `STRACE_MSM_AUDIO_CAPTURE_PID` to the decimal TGID of the selected service.
The decoder accepts ARM32 personality, the exact `/dev/msm_aac` fd path, a
character-device stat identity, and the target TGID/starttime. Ordinary tracing
keeps the existing ioctl decoder when the option is absent or admission fails.
Attach all existing service threads with `-f -p` before codec creation and keep
future clones attached. Keep ioctl, close, dup2 and dup3 in the trace selection;
using the unfiltered default trace provides the complete fd lifecycle.

The decoder records successful REGISTER_ION, readable mapping extent and
successful ASYNC_READ before admitting a READ_DONE completion. The mapping
extent bounds readable capacity; the extent does **not** prove ION allocation
capacity. Submitted address/private_data, length, metadata size and fd
generation must match. The code checks each mapping before and after each
snapshot and requires two identical complete snapshots. Completion after a
missed submit exit fails admission even when the real driver accepts the read.

One active AAC fd is the supported capture surface. A second observed AAC fd
makes the tracer reject further captures until a fresh tracer starts. Device
inode identity cannot distinguish duplicated file descriptions. Traced close
and dup2/dup3 destination replacement invalidate the recorded fd session.
A successful ioctl with invalid ownership or metadata poisons the session
until traced close/reopen. Start, stop, flush or abort with pending reads also
poisons the session. The guard prevents late completions from acquiring a
fresh interpretation after pointer reuse. Failed syscalls remain explicit
errors and create zero accepted submissions.

The lifecycle contract requires observation of every relevant fd operation.
Unobserved fd transfer, exec, close_range, detach/re-attach, or thread coverage
loss invalidates the experiment externally; the decoder cannot certify their
absence from a filtered or incomplete trace. ARM Linux 3.4 provides the target
kernel boundary. Other audio device paths and ABI variants remain unhandled.

## Descriptor contract and limits

| Item | Layout or limit |
|---|---|
| `msm_audio_aio_buf` | 20 bytes; address, length, data_length, private_data u32; metadata size u16 at 16 |
| `msm_audio_event` | 40 bytes; event type at 0; payload union at 8 |
| Completed buffer prefix | 32 bytes; frame count u32 at 28 |
| Each descriptor | 24 bytes: offset, bytes, samples, timestamp word 0, timestamp word 1, flags |
| Payload base | allocation base + 32 + descriptor offset |
| Submission size | 56 through 65,536 bytes; metadata size exactly 32 |
| Registered and pending slots | 64 each |
| Captured raw bytes | 8 MiB per fd generation |

REGISTER_ION is `0x40046161`, DEREGISTER_ION is `0x40046162`, ASYNC_READ is
`0x40046112`, and GET_EVENT is `0x8004610d`. GET_EVENT type 4 exposes codec,
channel and sample-rate fields; type 3 exposes READ_DONE and the buffer.
The zero-frame case retains the completed envelope. The UINT32_MAX frame
count represents EOS and requires room for one descriptor.

Every descriptor fits inside the completed range and starts after the whole
descriptor table. Descending and overlapping ranges produce an error.
Ascending gaps remain visible as `contiguous=false`; the capture does not
concatenate, normalize or reinterpret them. Safe raw snapshots remain in the
trace on descriptor-validation failure, carrying `msm_error` and an invalidated
session. Consumers require an error-free ownership chain before accepting PCM.
A rejected snapshot adds `snapshot_error` with its one-based snapshot number,
`stage`, `read_attempted`, wrapper `read_result`, and immediately saved `errno`.
Stages distinguish mapping lookup, mapping identity before/after the read,
span bounds, and `umoven`. A mapping lookup failure adds `mapping_reason` for
open/read errors, malformed/overlong map lines, unmapped address, unsupported
span, or unreadable permissions. Errno zero means the check or wrapper supplies
zero OS errno; zero does not convert a failed snapshot into a successful read.
The diagnostics leave the generic capture error and session invalidation intact.
A snapshot failure also samples the matching VMA's `VmFlags` from smaps while
the returning ioctl thread remains stopped. The failure-only scan consumes at
most 4 MiB, bounds each line to 1023 bytes, requires the exact registered maps
header, and rechecks mapping identity after finding the flags. `failure_vma`
reports `status="observed"`, the flags and separate `io`/`pf` booleans, or
`status="unavailable"` with a reason and saved errno. Readable maps permission
and PFNMAP flags describe separate kernel admission conditions. The flags
sample remains a bounded observation rather than an atomic mapping guarantee.

`umoven` retains its existing transport selection; the extension adds zero
read mechanisms or PFNMAP exceptions.

Output uses strace text syntax rather than JSON; `raw_hex` contains the complete
captured range independently of the ordinary string abbreviation limit.

A pair of equal snapshots establishes bounded consistency. DSP DMA, another
thread, or a mapping replacement can change and restore memory between checks.
Ptrace suspends the returning thread rather than the DSP, and identical map
lines cannot exclude remapping with the same address and identity. Retain the
submission trace, thread census, source/blob hashes and consumer output beside
each capture. Tracing qualifies bytes and metadata; its timing cannot qualify
throughput or latency.

## Host verification and ARM build

The focused fixture runner uses Linux x86-64 MAP_32BIT for actual low-address
mappings and exercises the production ioctl glue with synthetic ioctl exits.
The host tests cover size/offset bounds, registration overlap, pending overlap,
private-data identity, missing and failed submissions, fd identity, unsupported
word size, session invalidation, mapping permission changes, unreadable and
changed snapshots, descriptor overlap/gaps, EOS and the total capture limit.
The runner also compiles the non-ARM stub.

```sh
CC=cc tests/run-msm-audio-capture.sh /absolute/output/gcc
CC=clang tests/run-msm-audio-capture.sh /absolute/output/clang
CC=clang SANITIZE=1 tests/run-msm-audio-capture.sh /absolute/output/sanitized
```

The sanitizer mode makes AddressSanitizer and UndefinedBehaviorSanitizer
failures fatal and enables leak checking. LeakSanitizer requires execution
outside a ptrace-based host sandbox. Synthetic passing fixtures establish
parser/glue behavior; the fixtures do not establish device ptrace permissions,
real DSP descriptor contents or a codec lifecycle result.

Android.bp includes both new translation units. Makefile.am and the shipped
Automake 1.14.1 Makefile.in carry matching sources, object rules and distribution
files. The full upstream syscall suite remains a separate configured-host
`make check` gate. An ARM helper build uses the exact target Ninja compile/link
commands with output paths redirected into an isolated output directory.
Record source, compiler, unchanged baseline object, linker-input and ELF hashes.
The linked helper still requires device attachment and stereo/multichannel
capture validation before decoder-repair decisions.

## Observed legacy ION read boundary

The job-710 Desire stereo probe exercises service attachment, native stream
metadata, read submission, completion observation and explicit detach. The
smaps-enabled helper returns an accepted ioctl envelope followed by a rejected
first snapshot: umoven=-1, errno=EFAULT, with the readable mapping identity
check passed. The completed ION range contributes zero admitted raw bytes.
The kernel's show_smap omits VmFlags, so failure_vma reports vmflags_missing.
A readable maps permission does not imply cross-process remote-read access.

Stereo output remains byte-identical to the retained baseline, and all twelve
observed surviving service threads have TracerPid=0 after tracer exit. The
capture establishes an observer boundary rather than DSP descriptor contents.
Use `-V` for the bundled strace version query. Owner-side descriptor observation
requires its own source/ABI and ownership gates; changing capture transport or
accepting rejected snapshots cannot discharge those gates.
