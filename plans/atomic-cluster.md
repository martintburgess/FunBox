# Atomic Cluster — Funbox Module Plan

A Funbox (Daisy Seed) re-creation of the **EHX Pico Atomic Cluster** "spectral
decomposer": analyze the input spectrum, pick the loudest peaks, and re-synthesize
them as a bank of sine oscillators, refreshed on a clock.

Refs: [EHX product page](https://www.ehx.com/products/pico-atomic-cluster/),
[Premier Guitar review](https://www.premierguitar.com/reviews/pedals/electro-harmonix-atomic-cluster-review).

---

## 1. What the effect is (algorithm)

```
input ─► N-sample ring buffer
            │  every REFRESH samples (rate from SPEED / tap tempo):
            ▼
      window (Hann) ─► FFT (shy_fft) ─► magnitudes[1..N/2]
            │
            ▼
      local-maxima peak pick ─► top-K by magnitude   (K = ATOMS)
            │  → (freq_hz, amp) per peak, parabolic-interpolated freq
            ▼
      sine oscillator bank (shared wavetable, up to MAX_ATOMS)
            │  SHARP: snap freq/amp to new targets (keep phase continuous)
            │  SMOOTH: glide freq/amp toward targets over the refresh interval
            ▼
      Σ partials ─► wet ──┐
      dry ────────────────┴─► BLEND ─► VOL ─► out
```

Key properties:
- The FFT is used for **analysis only** — output is synthesized from oscillators,
  not an inverse FFT. So there is **no overlap-add machinery** (unlike Venus/Saturn,
  which reconstruct via IFFT and need overlapping frames for the COLA condition).
- "Almost uneffected at max ATOMS" is expected: with ~64–128 partials you reconstruct
  most of the harmonic content. It will never be *perfectly* transparent — sines can't
  reproduce pick attack, string/breath noise, or transients. That glassy quality is
  inherent to sinusoidal resynthesis and matches the real pedal.

---

## 2. Oscillator decision (why a shared wavetable, not DaisySP)

- **DaisySP `Oscillator`** sine = `sinf(phase*2π)` per sample (`oscillator.cpp:12`).
  `sinf` is software on the Cortex-M7 (~50–100 cyc). At 128 voices ≈ ~50% CPU just for
  the transcendental.
- **Shared sine wavetable** (one table, linear interp, read by all partials) ≈ ~8–12
  cyc/voice → ~15% CPU at 128 voices. This is the standard additive-synthesis approach.
- **DaisySP `OscillatorBank`** is *not* a generic additive bank — it's a fixed
  7-waveform octave-locked divide-down organ voice. Wrong tool.
- **Portability:** a wavetable core has zero deps → trivial desktop build. (DaisySP does
  compile on host, but this keeps the host Makefile minimal.)

**Decision:** custom shared sine wavetable in the portable core. Revisit DaisySP
`Oscillator` only if/when we add band-limited non-sine partials (saw/square need
PolyBLEP to avoid aliasing), accepting a lower voice count there.

---

## 3. Architecture — one portable core, two build targets

The DSP lives in a **header-only core with no Daisy/DaisySP dependency**, compiled
identically into the firmware and a desktop tool. What you hear on the laptop is what
the pedal does (modulo samplerate / block size, which are passed in).

```
AtomicClusterCore.h        portable C++: ring buffer + shy_fft + peak-pick + sine bank
   init(sample_rate, block_size)
   setAtoms(int) / setSpeedMs(float) / setBlend(float) / setVol(float) / setMode(SHARP|SMOOTH)
   tap()                                 // tap-tempo hook (sets refresh interval)
   processBlock(const float* in, float* out, size_t n)
        │                                            │
        ▼                                            ▼
  software/AtomicCluster/AtomicCluster.cpp     software/AtomicCluster/desktop/main.cpp
  (firmware: Funbox knobs/FS → core,           (offline: dr_wav reads in.wav →
   core called inside AudioCallback)            processBlock → writes out.wav)
```

`shy_fft.h` (copied from Venus/Saturn) is plain C++ and builds on macOS.

---

## 4. Core DSP detail

### Parameters & sizes
| Symbol | Start value | Notes |
|---|---|---|
| `sample_rate` | 48 kHz | drop to 32 kHz (like Venus) if CPU-bound |
| `N` (FFT size) | 2048 | bin spacing 23 Hz @48k; snapshot latency ~43 ms. Bump to 4096 for finer pitch |
| `MAX_ATOMS` | 64 (build up to 128) | sized array of oscillator slots |
| `REFRESH` | from SPEED: ~1000 ms → ~30 ms | independent of FFT size |

### Data
- `float ring[N]` — input history (mono = left channel for MVP).
- `float window[N]` — precomputed Hann.
- `float sine_table[TABLE+1]` — one shared sine period, linear-interpolated.
- `struct Partial { float phase, phase_inc, amp; float target_inc, target_amp; }` ×`MAX_ATOMS`.

### Per refresh tick
1. Copy ring (rotated to time order) × Hann into FFT input.
2. `shy_fft` forward → real/imag; `mag[k] = hypotf(re,im)` for `k = 1..N/2-1`.
3. **Peak pick:** keep bin `k` only if `mag[k] > mag[k-1] && mag[k] >= mag[k+1]`
   (local max). Collect maxima, partial-select **top-K** by magnitude (`K = ATOMS`).
4. For each chosen peak: **parabolic interpolation** on `mag[k-1,k,k+1]` for sub-bin
   freq → `freq_hz`; `amp` from interpolated peak height (normalized by N and window
   gain). Convert `freq_hz` → `target_inc` (cycles/sample).
5. Assign to oscillator slots `0..K-1`; set slots `K..MAX-1` target amp = 0.
   - **SHARP:** `phase_inc = target_inc; amp = target_amp` immediately (keep `phase` —
     don't reset — to avoid clicks; apply a short amp ramp if needed).
   - **SMOOTH:** leave `phase_inc/amp`, glide them toward targets over `REFRESH` samples.

### Per sample
- For each active slot: `s = lerp(sine_table, phase); phase += phase_inc (wrap);
  wet += s * amp;` (SMOOTH also nudges `phase_inc`/`amp` toward targets).
- `out = dry*(1-blend) + wet*blend; out *= vol;` write to L and R.

---

## 5. Control mapping

### MVP (4 knobs + bypass)
| Control | Param | Range |
|---|---|---|
| Knob 1 | ATOMS | 1 → MAX_ATOMS |
| Knob 2 | SPEED (refresh) | ~1000 ms → ~30 ms |
| Knob 3 | BLEND | 0 → 1 |
| Knob 4 | VOL | 0 → ~1.5 |
| FS1 / LED1 | Bypass | — |

### Fast-follows (use the extra Funbox controls)
| Control | Param |
|---|---|
| Switch 1 | MODE: SHARP / (mid) / SMOOTH |
| FS2 / LED2 | Tap tempo (blink LED2 at tempo); hold = freeze cluster |
| Knob 5 | partial decay/release, or analysis tilt |
| Knob 6 | octave/pitch shift of partials, or stereo spread |
| Dips | mono / MISO / stereo; stereo-spread on/off; scale-quantize on/off |

---

## 6. Phased roadmap

1. **Core + desktop offline tool.** Sine bank, peak-pick, continuous refresh, SHARP,
   mono. Process a dry-guitar WAV; A/B against EHX demo clips. *(Most tuning happens here.)*
2. **Firmware wrapper.** Copy `software/Template/` as the shell; wire Knobs 1–4 + FS1
   bypass to the core; call `processBlock` in `AudioCallback`. Target FLASH first.
3. **Tap tempo** (FS2): timestamp `FallingEdge()` with `System::GetNow()`; interval =
   gap between last two taps; timeout > 2 s resets; blink LED2. Decide knob-vs-tap
   interaction (simplest: tap overrides knob until knob moves).
4. **SMOOTH mode** (Switch 1) — glide partials over the refresh interval.
5. **Stereo / extras** — partial scatter across L/R, freeze (hold cluster),
   scale-quantize of partial frequencies, Knob 5/6 roles.
6. Optional: real-time desktop app (RtAudio) or JUCE plugin for live in-DAW testing.

---

## 7. Build

### Desktop (fast iteration loop)
```
clang++ -std=c++17 -O2 software/AtomicCluster/desktop/main.cpp -o /tmp/cluster
/tmp/cluster in.wav out.wav            # then listen / A-B
```
(Single-header `dr_wav.h` for WAV I/O; core + `shy_fft.h` are header-only.)

### Firmware
- Apply the `mod/` libDaisy patch first: copy `mod/daisy_pedal.{h,cpp}` →
  `libDaisy/src/`, then `make -C libDaisy && make -C DaisySP`.
- `cd software/AtomicCluster && make` → `make program-dfu` (FLASH). Move to
  SRAM/bootloader only if buffers + bank don't fit.

---

## 8. Risks / things to tune
- **Peak-pick quality** is the make-or-break detail. Naive top-N bins sounds harsh;
  local-maxima + parabolic interp is the minimum bar. May also want a magnitude floor
  (ignore noise-level peaks) and min-spacing between picked peaks.
- **Click/zipper noise** on refresh, especially in SHARP at high ATOMS — keep oscillator
  phase continuous across refreshes; short amp ramps; SMOOTH largely solves it.
- **CPU**: profile the sine bank at target voice count and samplerate; 32 kHz / N=2048
  is the safe fallback.
- **Low-note pitch accuracy**: 23 Hz bins are coarse for the low E (82 Hz); parabolic
  interpolation matters, or use N=4096.

---

## 9. Open decisions
- MAX_ATOMS final value (64 vs 128) — set after CPU profiling.
- Samplerate (48 k vs 32 k).
- Tap-vs-SPEED-knob interaction (override vs subdivision multiplier).
- Whether partials are pure sine only, or add a waveform switch later (drives the
  DaisySP-Oscillator-vs-band-limited-wavetable question).
