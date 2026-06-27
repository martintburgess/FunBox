# Atomic Cluster (work in progress)

A Funbox re-creation of the EHX Pico Atomic Cluster "spectral decomposer":
the input spectrum is analyzed periodically, the loudest peaks are picked, and
they are re-synthesized as a bank of sine oscillators, refreshed on a clock.

Full design + roadmap: [`plans/atomic-cluster.md`](../../plans/atomic-cluster.md).

## Layout

| File | Role |
|---|---|
| `atomic_cluster_core.h` | Portable, dependency-free DSP core (shared by firmware + desktop) |
| `shy_fft.h` | FFT (vendored from Venus/Saturn) |
| `desktop/main.cpp` | Offline WAV test harness — run the effect on your computer |
| `desktop/dr_wav.h` | Single-header WAV I/O (vendored) |

The core has **no Daisy/DaisySP dependency**, so the desktop tool runs the exact
DSP that will run on the pedal.

## Status

- [x] Phase 1 — portable core + desktop WAV tool (peak-pick → sine bank, SHARP/SMOOTH, mono)
- [x] Phase 1b — random peak selection each refresh (the "jumping" that defines the pedal)
- [x] Phase 1c — **decoupled tracking**: a fast analysis clock keeps partials following
      the input (peak continuation, smoothed amp/freq); a separate SPEED clock randomly
      re-chooses which are audible. Max ATOMS now tracks the input instead of stepping.
- [ ] Phase 2 — Funbox firmware wrapper (knobs 1–4 + bypass)
- [ ] Phase 3 — tap tempo (FS2)
- [ ] Phase 4+ — stereo / freeze / scale-quantize / extra knobs

## How it works (core)

Two decoupled clocks (see the header comment in `atomic_cluster_core.h`):

- **Analysis clock** — every `kHop` (512) samples, an overlapping FFT detects peaks
  and matches them to a set of persistent *tracks* by frequency. Each track is an
  oscillator whose amplitude/frequency continuously follow the input.
- **Selection clock** — every SPEED interval, K = ATOMS tracks are chosen at random
  (weighted by loudness) to be audible; audibility crossfades (SHARP/SMOOTH).

This is why max ATOMS approaches the input (all tracks audible, following the
envelope) while low ATOMS jumps between a few notes.

## Desktop usage

```
cd desktop
make
./cluster in.wav out.wav [atoms=16] [speed_ms=300] [blend=0.7] [vol=1.0] [sharp|smooth]
```

- `atoms` 1–64 — how many resonant oscillations sound at once. Max ≈ the input
  signal; min = a single note jumping around.
- `speed_ms` — how often the oscillators refresh (how fast the pitches jump).
- `mode` — `sharp` (instant jumps) or `smooth` (crossfaded jumps).

Selection is always random (weighted toward louder peaks). Mono in/out; stereo
inputs are downmixed.
