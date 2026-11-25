# Audio roadmap: removing `X2W_AUDIO_PERMISSIVE`

Draft, 2026-11-24, against upstream `main` at `2a27507a`. This is a plan, not a measurement. Every
"seen" below is one local observation unless a PR or issue is cited; layouts are observations, not
facts.

## Goal and definition of done

`X2W_AUDIO_PERMISSIVE` makes the three console audio stand-ins (`AcpHal.dll`, `xapu.dll`,
`chad.dll`) accept calls they cannot serve, so startup continues with no console audio. The goal is
to replace that with a real implementation. The flag is gone when all of these hold:

1. No `permissive()` branch is left in `acphal.cpp`, `xapu.cpp` or `chad.cpp`, and `run.ps1` no
   longer sets the flag.
2. Every export the title reaches either implements its contract, checked by a source-built caller,
   or returns a documented failure. For a documented failure, a run must show the title handles it.
   An export the title never reaches still traps.
3. A startup run's process-loopback capture ([startup-audio-timeline.md](startup-audio-timeline.md))
   shows non-silent audio in windows known to be audible. The #1508 pacing metric shows no stutter.
4. The three tests that use the flag's trap arm as a negative control keep an equivalent control
   (see Phase 6). No test is weakened or deleted.

## What the flag does today (upstream `main`)

| Module | Under the flag | Without the flag |
|---|---|---|
| `AcpHal.dll` | Real heap: `ApuCreateHeap`, first-fit `ApuAlloc`/`ApuFree`, address map and validity. XMA shape contexts are allocated and cleared; other context types return `E_NOTIMPL`. The HAL object's slot 0 (connect) and slot 2 (submit) return `S_OK` and write nothing; other slots trap. | Traps |
| `xapu.dll` | Handles are tracked. Each `XApuEnqueueCommand` queues one completion. `XApuDequeueResult` returns an all-zero 0x58-byte placeholder result. The 16-byte output of `XApuConnect` is never written. A 1 ms timer is held while connected (#1448). | Traps |
| `chad.dll` | Connect validates its out-handle (#1871). `XDspActivate` clears its outputs and returns `XDSP_E_NO_DEVICE_RESOURCES`; submit refuses unknown handles (#1907). Deactivate and disconnect return 0 for any handle. | Traps |

`AcpHalReleaseShapeContexts` traps in both modes. Nothing in the console path produces samples. The
WASAPI sink (`audio_sink.{h,cpp}`, #1250) exists, but nothing feeds it.

## What is known about the title's audio path

Static findings come from local reads of the main image's imports, strings and call sites. Run
findings are single local observations.

- **Only the video DLL uses XAudio2.** The main image has no `XAudio2_9.dll` import and no XAudio2
  string, in ASCII or UTF-16. The video DLL imports `XAudio2_9.dll` ordinal 1
  ([experiments.md](experiments.md)). The XAudio2 stream reported in #1145 (audible, stuttering) is
  therefore most likely movie audio, not the game engine's.
- **The engine probably renders through Windows spatial audio.** The main image contains the GUIDs
  (taken from the Windows SDK 10.0.26100 headers) of these interfaces:
  - `MMDeviceEnumerator`, `IMMDeviceEnumerator` and `IMMNotificationClient`
  - `IAudioClient`
  - `ISpatialAudioClient` and `ISpatialAudioObjectRenderStream`

  It does not contain `IAudioRenderClient`. These are desktop APIs too, so the engine's output path
  probably needs no stand-in and no bridge to `audio_sink`. A run has not confirmed this yet.
- **The console accelerators are what is missing.** The engine imports `AcpHal.dll`, `xapu.dll` and
  `chad.dll` directly. One local run saw:
  - `ApuCreateHeap` with about 455 MiB cached and 2.2 MiB non-cached;
  - 350 XMA shape contexts and no other context type;
  - a HAL connect with two 4096-entry queues, and HAL submits of types 2 and 5;
  - XDsp convolution activations with 1024-frame mono blocks.
- **XApu is a packet-decoder command queue.** The command builders give these layouts:
  - `XApuEnqueueCommand(handle, command, 0)` takes three arguments. Earlier logs read `r9` and
    command bytes past the end of the command; both were stack and register leftovers.
  - Type 1 (0x38 bytes) opens a decoder slot: `{1, slot, 0, ..., u32 parameter at +0x18}`. Slots
    come from a pool of 256.
  - Type 2 (12 bytes) closes a slot: `{2, slot, 0}`.
  - Type 5 (0x30 bytes) resets a slot.
  - Type 3 (0x40 bytes) decodes one packet. It carries the input pointer and byte count (the packet
    minus a 2-byte header), 960 frames, a rate stored as a float, and an output buffer sized as
    channels × 960 × 4 bytes. The codec is not identified.
- **XApu results drive a per-slot state machine.** A result is `{type, slot}` with a value at +0x34.
  The title moves a slot from opening to ready on a type 1 result. On a type 3 result it stores the
  value and marks the packet done. A type 5 result returns a reset slot to ready, and a type 2 result
  frees the slot.
- **The current placeholder answers nothing, and the title times out.** Its all-zero result has
  type 0, which matches no case. One uncapped trace confirmed the effect: upstream `main` at
  `2a27507a` plus the trace, 218 s through the first-run screens.
  - It recorded 9 type 1 opens and 9 type 2 closes, and no type 3.
  - Each slot was closed 5.35 to 5.38 s after it was opened, so the title gives up waiting for the
    type 1 result.
  - The opens coincide with screen changes.
  - `XApuGetResultCount` was polled about every 5 ms.
- **On those screens the other two stand-ins are nearly idle.** In the same run:
  - the AcpHal stand-in saw one HAL submit, type 2, at startup;
  - all 350 XMA contexts stayed zero;
  - XDsp only connected.

  The earlier runs reached different stages (another contributor saw no HAL submit, a local run saw
  48), so XMA and XDsp use need a capture of the main load or gameplay.
- **Trace overhead was not visible in that run.** Its screen timing and frame rate were within the
  spread of runs without the trace.
- **XMA in experiment runs (local prototype with FFmpeg's decoder):**
  - After setup, the title starts many short XMA voices, each at a different start position inside a long chunk, in crossfading context pairs: granular playback.
  - The prototype's in-game decode matches an offline decode of the same captured packets sample for sample, including the title's start position.
  - The XApu Opus sounds on screen changes are audible.

## Open questions (Phase 0 answers these)

| # | Question | Why it decides the plan |
|---|---|---|
| Q1 | Does the title open a spatial audio stream on desktop, and does it render (process-loopback level during menus)? | If yes, output needs no new code; if not, the output path is the first gap. |
| Q2 | Does the title fill the XMA shape contexts and submit them through the HAL? Where does the decoded PCM land? Answered by static reading:<br>• The 64-byte XMA context matches the publicly documented Xbox 360 XMA context layout, as used by open-source emulators: input buffers 0 and 1, valid bits, packet counts, loops, and an output ring with write and read offsets in 256-byte blocks.<br>• Each context has a 0x1700-byte output ring in the non-cached APU block.<br>• The title reads decoded PCM out of the ring itself.<br>• HAL submits start a context (type 5), stop it (type 7), and consume output blocks (type 0x12, `{context, blocks}`). | Decides the SHAPE emulation scope. |
| Q3 | What codec do type 3 packets carry? What does the type 1 parameter mean? Partly answered:<br>• The result value at +0x34 is the number of decoded bytes, a multiple of 32. The output is 32-bit float PCM, and the title converts it to 16-bit.<br>• `XApuConnect` must return a shared buffer, of the size given in its input, in the first qword of its output.<br>• In an experiment run, every packet was 160 bytes for 960 frames, and its first byte fits the Opus TOC for one 20 ms, mono, fullband CELT frame.<br>• Two Opus decoders accept the packets.<br>• Each decoded packet shows a sound onset that is outside the spread of 200 control packets with the same first byte and random content.<br>• So the codec is most likely Opus, but no source states it. | Needed to implement the decoder and to produce truthful results. |
| Q4 | After `XDSP_E_NO_DEVICE_RESOURCES`, does the title fall back cleanly (dry signal, no reverb) or lose sound? Partly answered: in one local run without a debugger, on a build with #1907's refusal, 8 activations were refused, no submit followed, there was no crash, and the title stayed alive several minutes into the main load. What it does with the sound is unmeasured. | Decides whether refusal is an acceptable permanent behaviour or convolution must be implemented. |
| Q5 | What causes the stutter of the video DLL's XAudio2 audio? | Separate from the flag, but item 3 of the definition of done needs it. |

## Phases

### Phase 0: Evidence (blocks everything)

- Run a full capture with the cap lifted (`X2W_AUDIO_TRACE`, decoded by
  `tools/startup/audio_trace.py`). It records every stand-in call with its arguments across startup,
  menus and gameplay, plus the commit and build of the run. Do this locally and publish aggregates
  only.
- Continue the static analysis: the HAL method calls, the XMA context writes, and the spatial audio
  activation. Keep it local, with no dumps in issues or PRs.
- To make type 3 commands appear, run an experiment build that answers types 1, 2 and 5 with
  `{type, slot}`. The packets then show the codec (Q3). This changes behaviour, so it stays a local
  experiment until Phase 1 turns it into a PR.
- Answer Q1 to Q5 with a recorded commit for each answer.

### Phase 1: Stand-ins that do not lie (rest of #1887)

The remaining rows of #1887, one narrow PR each, each with a source-built caller and a negative
control that fails when the old behaviour returns:

- The `XApuConnect` 16-byte output: write it if the caller reads it, otherwise document that it does
  not (Q3).
- `XApuDequeueResult`: replace the zero placeholder with measured fields, or fail.
- AcpHal slot 0 and slot 2: write what the callers read back, or fail.
- `XDspDeactivate` and `XDspDisconnect`: refuse null and unknown handles.

#1902 is open on `acphal.cpp`. Wait for it to land or rebase on it.

### Phase 2: Video audio pacing (#1145, triggered02's track)

Find the root cause of the stutter in the video DLL's XAudio2 audio with the existing controls
(#1368, #1508, #1536).
Fix the cause, not the buffer size. This phase does not touch the flag but blocks item 3 of the
definition of done.

### Phase 3: XMA decode

- Implement a software XMA2 decoder inside the `AcpHal` stand-in (SHAPE emulation), scoped by Q2.
  - Type 5 starts a context, type 7 stops it, and type 0x12 advances its output read offset.
  - While a context runs, a worker decodes XMA packets from its input buffers into its output ring.
  - The worker advances the write offset and clears each input buffer's valid bit when the buffer
    drains, the way the Xbox 360 XMA context semantics define it.
  - The engine does not use XAudio2, so there is no XAudio2-side option.
- Two decisions belong to the user and the owner:
  - **Licence.** The project is MIT. FFmpeg's XMA/WMA Pro decoder is LGPL, and the external provider
    named in #870 is GPL. The options are a clean-room MIT decoder or an optional, separately loaded
    provider.
  - **Fixture source.** There is no public XMA encoder. NinosTech's decode oracle on #1145 matched
    FFmpeg on a generated stream; ask how that stream was produced and under what licence.

### Phase 4: XApu command queue

Implement the slot protocol above:

- Types 1, 2 and 5 complete with `{type, slot}`.
- Type 3 decodes the packet with a software decoder for the codec Q3 identifies, writes channels × 960
  float frames to the output buffer, and completes with the measured value at +0x34.

The decoder's licence must be MIT-compatible. Results are produced when the work is done, not
fabricated at enqueue time. XApu output stays inside the title's own mixer, which renders through
spatial audio (Q1).

### Phase 5: XDsp convolution

If Q4 shows a clean fallback, refusal can stay as the permanent, documented behaviour. If it does
not, implement CPU convolution: uniformly partitioned FFT overlap-save with 1024-frame blocks,
behind the documented `XDspActivate`/`XDspSubmitCommand`/status-block contract. A dry pass-through
is a stopgap, not done.

### Phase 6: Remove the flag

The touch list:

- `src/startup_stubs/{acphal,xapu,chad}.cpp`
- `tests/startup_acphal_apu_free.cpp`, `tests/startup_xapu_queue_check.cpp`,
  `tests/startup_chad_check.cpp`
- `tools/startup/run.ps1`
- `docs/wine.md`
- three places in `docs/startup-runtime.md`: the switch table, "Audio output" and "APU heap
  stand-in"

Each test's "flag unset → trap" arm becomes an "export not reached or not implemented → trap" arm,
for example `AcpHalReleaseShapeContexts`. Each test's implemented-path checks keep their own
negative control.

## Coordination

| Item | State | Relevance |
|---|---|---|
| #1145 | Open, owner's audio tracking issue | Phases 2 to 4 report here. triggered02 owns timing and validation, and offered to take the XMA decode path. Ask before claiming a slice. |
| #1887 | Open, opened by NiiRoZz | Phase 1. #1907 closed the XDsp rows; the rest are open. |
| #1902 | Open PR, Asimmimx | Touches `acphal.cpp`; avoid conflicting edits. |
| #1536 | Open draft, triggered02 | Real-XAudio2 endpoint validation through process loopback. |
| #870 | Open, P0 | Its audio section lists the slices claimed by others (ACP memory, XApu/XDsp queue, acphal). |

The owner assigns narrow, source-built, evidence-backed slices. Claim one on the issue before
starting it.

## Suggested order

1. Phase 0 capture and Q1 to Q5. This is local work; the only public output is aggregate findings.
2. Phase 1 slices on #1887, one PR each.
3. Phase 3 decisions (licence and fixture), raised on #1145 before any code.
4. Phases 3 to 5 in the order the Phase 0 answers dictate, then Phase 6.

## Findings, 2026-11-25

- PR 1 is R0Y00/Xbox2Windows#2288 (shared buffer from `XApuConnect`). Matched replay with typed completions on: upstream `xapu.dll` crashes writing to address 0 in `memcpy` right after the first dequeue (ap4); with the buffer there is no crash (ap2). With typed results, decodes answered with 0 bytes make the caller re-request decodes in a tight loop (ap2: 23 of the first 24 commands), so the Opus decode PR must return real byte counts.
- Process-loopback capture (local tool `out/audio-phase0/procloop`) of run ac1: the XApu Opus UI sounds match an offline libopus decode of the same packets (same band energy, 120 Hz peak). The loud 40–57 s audio is a Bink video soundtrack (title Bink decoder, real Windows XAudio2), not an XApu or XMA path. No XMA voice played in ac1; run ac2 is meant to reach them.
- ac2/ac3 (past Confirm): 64 XMA voices start and the title consumes every ring block, but nothing reaches the process output. With `X2W_ACPHAL_XMA_TONE=1` (1 kHz at -12 dBFS written into each ring block) the capture has no 1 kHz component (band/neighbours +1.3 dB, noise). So the XMA decode path is not the gap: ring audio is consumed and then dropped before output.
- Refuted: the spatial-object hypothesis. The title's only `ActivateSpatialAudioStream` call passes `MinDynamicObjectCount = MaxDynamicObjectCount = 0`, category `AudioCategory_GameEffects`, an event handle and a static-bed mask (read from the main image). So the output stream needs no dynamic objects, and 0 available on this desktop does not explain the silence. Open question: where the title mixes the XMA output ring after consuming it.
- XMA voices are granular reads (run a8: one 23-block consume per start, all within ~2 ms, then stop), not real-time playback, so silence of the tone in ac3 does not show a missing output path. The audible paths are XApu/Opus (correct vs libopus) and the video DLL's own XAudio2 stream.
- Corrected: the 40–57 s low-frequency audio in ac1 is not the video DLL. In ac1 the video is opened and closed 47 times with 3 frames decoded, and a local XAudio2_9 proxy that the title did load (run ax2) saw no `XAudio2Create` call. The segment starts with the account picker and is 90–99% below 200 Hz with a DC component at its start. The XApu/Opus UI sounds in the same capture keep the reference band split (about 10% below 200 Hz, 50% above 4 kHz), so the Opus path is correct end to end. Next: run ax3 with the audio trace, to map decode slots to the capture timeline.
- Run ax3 (audio trace on): the only XApu decodes are 69 packets on slot 0, in three bursts at screen changes. No XMA context ran, and XDsp is unused. The low-frequency segment (capture 35–48 s) overlaps the video DLL's open/pause/close cycles (47 opens in about 7 s, starting about 1 s after the first screen). The main image imports `BinkSetSoundSystem` but no Bink XAudio2 opener, and the video DLL never reaches `XAudio2Create`. So the segment is most likely the menu video's soundtrack, played through a title-provided Bink sound callback into the title's own mixer. Unverified: whether that soundtrack is meant to be this bass-heavy (needs a reference decode of the same file, or a listen).
- The menu background video does play: the run screenshots show it animating, and the three logged `BinkNextFrame` calls are the tracer's cap. The video is 15 s long, 3840x2160 at 30 fps, with one 48 kHz stereo Bink audio track. That matches the 13–17 s length of the low-frequency segment. No offline reference: FFmpeg 9.0 rejects this Bink 2 revision tag, and with the tag relabelled its `binkaudio_dct` decoder fails on the packets.
- An 87 dB noise floor in ac2/ac3 (166–171 s) shows no 1 kHz tone, so no XMA grain is audible above it. XMA reaching the output at lower gain is not excluded.

## Status (2026-11-25): investigation paused

Proven:
- The XApu/Opus path is correct end to end: offline libopus matches the captured output band split.
- PR #2288 (XApuConnect shared buffer) is open with CI green.
- The output stream requests no dynamic objects.
- The video DLL never calls `XAudio2Create`.

Best lead for the reported "weird sound": the menu video's soundtrack, which is 90–99% below 200 Hz with a DC component. It reaches the output through the title's own Bink sound callback (`BinkSetSoundSystem`, opener inside the main image).

Not yet established:
- whether that soundtrack is supposed to sound like that;
- which sound the user heard.

Next steps when resumed:
1. Ask the user which sound they meant.
2. Statically find the opener passed to `BinkSetSoundSystem` and how it maps Bink's stereo track into the static bed (channel mask and format; a wrong sample format or channel stride would leave mostly low frequencies).
3. Rerun the attach-logging XAudio2 proxy in a run that reaches the account picker, to close the XAudio2 question for good. ax2 did not reach that screen, and ax1's log path is unconfirmed.

Local tools for this: `out/audio-phase0/{procloop,spatialq,xa2proxy,opus_offline}`. Runs: `out/orch/runs/{ac1,ac2,ac3,ax1,ax2,ax3}`.
