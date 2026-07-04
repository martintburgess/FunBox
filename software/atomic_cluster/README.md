# Atomic Cluster (work in progress)

A Funbox re-creation of the EHX Pico Atomic Cluster "spectral decomposer":
the input spectrum is analyzed periodically, the loudest peaks are picked, and
they are re-synthesized as a bank of sine oscillators, refreshed on a clock.

Full design + roadmap: [`plans/atomic-cluster.md`](../../plans/atomic-cluster.md).

## Layout

| File | Role |
|---|---|
| `atomic_cluster_core.h` | **Sine engine** — oscillator-bank resynthesis (portable, dependency-free) |
| `atomic_cluster_mask_core.h` | **Mask engine** — spectral mask + inverse-FFT/overlap-add |
| `shy_fft.h` | FFT (vendored from Venus/Saturn) |
| `desktop/main.cpp` | Offline WAV test harness — run either engine on your computer |
| `desktop/dr_wav.h` | Single-header WAV I/O (vendored) |

The core has **no Daisy/DaisySP dependency**, so the desktop tool runs the exact
DSP that will run on the pedal.

## Status

- [x] Phase 1 — portable core + desktop WAV tool (peak-pick → sine bank, SHARP/SMOOTH, mono)
- [x] Phase 1b — random peak selection each refresh (the "jumping" that defines the pedal)
- [x] Phase 1c — **decoupled tracking**: a fast analysis clock keeps partials following
      the input (peak continuation, smoothed amp/freq); a separate SPEED clock randomly
      re-chooses which are audible. Max ATOMS now tracks the input instead of stepping.
- [x] Phase 1d — **mask engine**: spectral mask + inverse-FFT/OLA, second
      resynthesis option (max ATOMS reconstructs the input itself)
- [ ] Phase 2 — Funbox firmware wrapper (knobs 1–4 + bypass + engine toggle)
- [ ] Phase 3 — tap tempo (FS2)
- [ ] Phase 4+ — stereo / freeze / scale-quantize / extra knobs

## Two engines

Both implement the same control surface (ATOMS / SPEED / BLEND / VOL / SHARP-SMOOTH)
and the same two-clock design, but resynthesize differently. On the pedal they'll
be selectable with a toggle switch; in the desktop tool, the trailing
`sine|mask` argument picks one.

| | Sine (`atomic_cluster_core.h`) | Mask (`atomic_cluster_mask_core.h`) |
|---|---|---|
| Resynthesis | additive sine-oscillator bank | keep selected bins, inverse-FFT + overlap-add |
| Max ATOMS | *approaches* the input (spectrum only) | *is* the input — transients, noise, partial shape intact |
| Min ATOMS | a bare sine | one note with its **natural** timbre |
| Latency | low | ~kN (~43 ms, inherent to STFT-OLA) |
| Repitch/glide | possible (oscillators) | not possible (bins are fixed in place) |

Objective check on the test chord (waveform similarity to the dry input, 0–1):
mask at max ATOMS = **0.92**, sine = 0.07 (the sine bank uses random phases, so its
*spectrum* matches but its *waveform* doesn't). See `desktop/examples/cmp_*`.

### How it works (both)

Two decoupled clocks (see the header comments):

- **Analysis clock** — every `kHop` (512) samples, an overlapping FFT detects peaks.
  The sine engine matches them to persistent *tracks* (oscillators that follow the
  input); the mask engine rebuilds a bin **mask** around the selected partials
  (re-located each frame so it follows pitch drift).
- **Selection clock** — every SPEED interval, K = ATOMS peaks are chosen at random
  (weighted by loudness); the transition crossfades (SHARP snaps, SMOOTH glides).

## Desktop usage

```
cd desktop
make
./cluster in.wav out.wav [atoms=16] [speed_ms=300] [blend=0.7] [vol=1.0] [sharp|smooth] [sine|mask]
```

- `atoms` 1–64 — how many resonant oscillations sound at once. Max ≈ the input
  signal; min = a single note jumping around.
- `speed_ms` — how often the oscillators refresh (how fast the pitches jump).
- `mode` — `sharp` (instant jumps) or `smooth` (crossfaded jumps).

Selection is always random (weighted toward louder peaks). Mono in/out; stereo
inputs are downmixed.
