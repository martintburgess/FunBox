# Freeze — Funbox Module Plan

A Funbox (Daisy Seed) re-creation of the **EHX Freeze (Sound Retainer)**: take an
instantaneous snapshot of whatever is sounding and hold it as an infinite sustain —
a drone from any note or chord, with Fast / Slow / Latch behaviors.

Refs: [EHX product page](https://www.ehx.com/products/freeze/),
[EHX manual PDF](https://www.ehx.com/wp-content/uploads/2021/01/freeze-manual.pdf),
[Premier Guitar review](https://www.premierguitar.com/gear/electro-harmonix-freeze-sound-retainer-pedal-review).

Decisions locked (2026-07-03): **spectral phase-vocoder freeze** (not time-domain
granular), **authentic controls first** (Volume + 3-way mode + Freeze footswitch),
extras as fast-follows.

---

## 1. What the effect is (algorithm)

The Freeze is *not* a looper — there is no rhythmic phrase. It captures the
**steady-state spectrum** at one instant and re-synthesizes it forever until released.
Spectral (phase-vocoder) freeze is the smoothest way to do this: no loop-point click,
no audible loop period, and Latch layering is just spectrum summing.

```
input ─► analysis ring buffer (Hann, N-sample, hop = N/OVERLAP)
            │  continuous STFT while running (like Venus)
            ▼
     shy_fft forward ─► re[k], im[k] ─► mag[k], phase[k]     (k = 0..N/2)
            │
    ┌───────┴─── on FREEZE trigger: latch a snapshot ───────┐
    │  frozen_mag[k]  = (average of last few frames' mag)    │
    │  frozen_phase[k]= current phase (seed for resynth)     │
    └────────────────────────────────────────────────────────┘
            │
            ▼  RESYNTH (runs every hop while frozen)
     for each bin k:
        mag  = frozen_mag[k]                      // held constant
        phase[k] += 2π·k·hop/N  (+ small jitter)  // advance at bin center freq
        re = mag·cos(phase), im = mag·sin(phase)
            │
     shy_fft inverse ─► Hann ─► overlap-add ─► wet
            │
     wet ─► envelope (Fast/Slow/Latch) ─► VOLUME ─┐
     dry (live playing) ──────────────────────────┴─► out
```

Key properties:
- **Magnitudes are frozen, phase keeps advancing** at each bin's nominal center
  frequency. Freezing phase too makes it static/comb-filtered and metallic; advancing
  phase (optionally with a touch of per-frame random jitter) gives a living, sustained
  tone. This is the standard phase-vocoder freeze and matches the pedal's character.
- **Overlap-add is required** (unlike atomic_cluster's oscillator bank): we reconstruct
  via IFFT, so frames must overlap for the COLA condition. Reuse Venus/Saturn machinery.
- **Dry passes through live** — you keep playing over the held drone (as on the real
  pedal; the knob only sets the *frozen* level, dry is unity).
- **Latch layering** = sum a new snapshot's magnitudes into `frozen_mag[]` (with
  headroom management), no extra voices needed.

---

## 2. Why spectral, not granular (decision record)

- **Smooth infinite sustain**: no loop period, no crossfade seam to tune. Granular
  needs multiple crossfaded heads and still risks periodicity artifacts.
- **Reuse**: `software/Venus/` already does a spectral freeze on this exact hardware
  (`shy_fft.h`, `fourier.h`, `wave.h`) — proven CPU-viable at 32 kHz.
- **Latch is trivial**: layering chords = adding spectra.
- **Cost**: higher CPU than granular and a slightly synthetic pad quality — which is
  *on-character* for the Freeze. Fallback if CPU-bound: 32 kHz samplerate, N=2048,
  OVERLAP=4 (Venus's operating point).

Revisit granular only if we later want a grittier, more "analog" freeze voice as an
alternate mode.

---

## 3. Architecture — one portable core, two build targets

Same shape as atomic_cluster: DSP in a **header-only core with no Daisy/DaisySP
dependency**, compiled identically into firmware and a desktop offline tool.

```
freeze_core.h              portable C++: STFT + freeze latch + phase-advance resynth + OLA
   init(sample_rate, block_size)
   setVolume(float) / setMode(FAST|SLOW|LATCH) / setBlend(float)
   trigger(bool pressed)                 // footswitch state → capture / release / latch logic
   doubleTapKill()                       // Latch: double-tap clears
   processBlock(const float* in, float* out, size_t n)
        │                                            │
        ▼                                            ▼
  software/freeze/freeze.cpp                   software/freeze/desktop/main.cpp
  (firmware: Funbox knobs/switch/FS → core,    (offline: dr_wav reads in.wav →
   core called inside AudioCallback)            processBlock → writes out.wav)
```

`shy_fft.h` is plain C++ and builds on macOS (copy from Venus). Directory + files use
**snake_case** per repo convention for new Funbox code.

---

## 4. Core DSP detail

### Parameters & sizes
| Symbol | Start value | Notes |
|---|---|---|
| `sample_rate` | 32 kHz (Venus's rate) | 48 kHz if CPU allows |
| `N` (FFT size) | 2048 | 64 ms window @32k; bin spacing ~16 Hz |
| `OVERLAP` | 4 | hop = N/4 = 512; satisfies Hann COLA |
| `AVG_FRAMES` | 2–4 | frames averaged at capture for a stable magnitude snapshot |

### Data
- `float in_ring[N]`, `float out_ring[N]` — analysis input history / OLA accumulator.
- `float window[N]` — precomputed Hann (used for analysis *and* synthesis).
- `float frozen_mag[N/2+1]`, `float phase[N/2+1]` — held magnitudes, advancing phase.
- `float re[N], im[N]` — FFT scratch.
- State: `enum Mode {FAST,SLOW,LATCH}`, `bool frozen`, `float env`, capture flags.

### Per hop (every N/OVERLAP samples)
1. Window `in_ring` × Hann → `re`; `im = 0`. `shy_fft` forward.
2. If a **capture** is pending: accumulate `mag[k]` over `AVG_FRAMES` hops, then latch
   into `frozen_mag[]`; seed `phase[k]` from current bin phase.
3. If `frozen`: rebuild spectrum — `mag = frozen_mag[k]`,
   `phase[k] += 2π·k·hop/N` (+ optional jitter), `re=mag·cosφ`, `im=mag·sinφ`.
   `shy_fft` inverse → × Hann → overlap-add into `out_ring`.

### Per sample
- `wet = out_ring[read]` (advance OLA read pointer);
- `wet *= env` (envelope from mode: see §5);
- `out = dry + wet * volume;` write L and R (mono core for MVP, dry = live input).

---

## 5. Mode / envelope logic (the three authentic modes)

Driven by the Freeze footswitch (momentary) + 3-way mode switch:

| Mode | On press | While held | On release |
|---|---|---|---|
| **Fast** | capture, `env` → 1 instantly | sustain | `env` → 0 instantly (gate) |
| **Slow** | capture, `env` ramps up over `fade_ms` | sustain | `env` ramps down over `fade_ms` |
| **Latch** | capture (or **layer** if already frozen); `env`→1 | sustain | **stays frozen** |
| Latch double-tap | — | — | clear `frozen`, `env`→0 |

- `fade_ms` default ~600 ms (Slow); later exposed on a knob.
- **Latch layering**: pressing while already frozen sums the new snapshot into
  `frozen_mag[]`; normalize/limit to avoid clipping as layers accumulate.
- **Double-tap detect**: two `FallingEdge()`s within ~400 ms → kill. Reuse the timing
  pattern from Venus/Pluto footswitch handling (`hw.switches[...].TimeHeldMs()`,
  `System::GetNow()`).

---

## 6. Control mapping

### MVP — authentic (mirrors the real pedal)
| Control | Param | Range |
|---|---|---|
| Knob 1 | VOLUME (frozen level) | 0 → ~1.5 |
| Switch 1 | MODE: FAST / SLOW / LATCH | 3-way |
| FS2 / LED2 | FREEZE (momentary capture); LED2 lit while frozen | — |
| FS1 / LED1 | Bypass | — |

### Fast-follows (use the extra Funbox controls)
| Control | Param |
|---|---|
| Knob 2 | FADE time (Slow attack/release; also softens Fast) |
| Knob 3 | DRY/WET BLEND (real pedal is dry=unity; make it adjustable) |
| Knob 4 | TONE — low-pass/high-pass tilt on the frozen layer |
| Knob 5 | SHIMMER / OCTAVE — add +12 (and/or −12) octave of the frozen spectrum |
| Knob 6 | PHASE JITTER / movement — 0 = static, up = evolving pad |
| Switch 2/3 | octave voicing select; capture-average length; mono vs stereo spread |
| Dips | mono / MISO / stereo; auto-fade on/off; Latch-layer limit on/off |

---

## 7. Phased roadmap

1. **Core + desktop offline tool.** STFT + OLA + freeze latch + phase-advance resynth,
   Fast mode, mono. Feed a dry-guitar WAV, trigger freeze at a timestamp, write out.wav;
   A/B against EHX Freeze demo clips. *(Most tuning happens here — phase jitter amount,
   AVG_FRAMES, window.)*
2. **Firmware wrapper.** Copy `software/Template/` as the shell; wire Knob 1 (Volume),
   Switch 1 (mode), FS2 (freeze) + LED2, FS1 bypass; call `processBlock` in
   `AudioCallback`. Profile CPU at 32 kHz / N=2048 / OVERLAP=4.
3. **Slow + Latch modes** — envelope ramps; Latch layering + double-tap kill.
4. **Tone + Blend + Fade knobs** (Knobs 2–4).
5. **Shimmer/octave + phase-jitter movement** (Knobs 5–6); stereo spread.
6. Optional: real-time desktop app (RtAudio) / JUCE plugin for live in-DAW A/B.

---

## 8. Build

### Desktop (fast iteration loop)
```
clang++ -std=c++17 -O2 software/freeze/desktop/main.cpp -o /tmp/freeze
/tmp/freeze in.wav out.wav            # then listen / A-B against EHX demos
```
(Single-header `dr_wav.h` for WAV I/O; core + `shy_fft.h` are header-only. Desktop tool
takes a freeze on/off timestamp arg to simulate the footswitch.)

### Firmware
- Apply the `mod/` libDaisy patch first (copy `mod/daisy_pedal.{h,cpp}` →
  `libDaisy/src/`), then `make -C libDaisy && make -C DaisySP`.
- `cd software/freeze && make` → `make program-dfu`. If FLASH is tight, follow Venus's
  memory placement (SRAM / QSPI for the large buffers).

---

## 9. Risks / things to tune
- **Metallic / static character**: pure frozen phase sounds like a comb filter. The
  phase-advance (§1) + a little jitter is the fix; jitter amount is the key taste knob.
- **Capture stability**: snapshotting a single frame during pick attack grabs transient
  junk. Average `AVG_FRAMES` hops and/or trigger capture slightly after attack.
- **CPU**: full STFT + IFFT per hop. 32 kHz / N=2048 / OVERLAP=4 is the safe target
  (Venus lives here); profile before adding shimmer (a second spectrum) on Knob 5.
- **Latch layering clip**: summing spectra grows magnitude — normalize or soft-limit,
  and cap layer count.
- **Latency**: N/2 window latency on the *wet* path is fine (it's a sustain); dry path
  stays zero-latency by passing live input straight through.
- **Bin resolution vs low notes**: 16 Hz bins @32k/N=2048 are coarse for low E (82 Hz)
  but freeze doesn't need precise pitch — magnitude envelope is what matters.

---

## 10. Open decisions
- Samplerate (32 k like Venus vs 48 k) — set after CPU profiling in Phase 2.
- Phase-advance vs light random-jitter vs a blend — decide by ear in Phase 1.
- Whether dry is fixed unity (authentic) or always on the Blend knob.
- Shimmer: +12 only, or ±12 / interval — Phase 5.
- Directory name: `software/freeze/` (assumed; confirm it shouldn't be a "Planet"
  name if this graduates to the polished Planet Series).
