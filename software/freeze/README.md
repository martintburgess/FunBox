# Freeze

A Funbox (Daisy Seed) re-creation of the **EHX Freeze (Sound Retainer)**: take an
instantaneous snapshot of whatever is sounding and hold it as an infinite sustain
(a drone from any note or chord), with FAST / SLOW / LATCH behaviors.

See [`plans/freeze.md`](../../plans/freeze.md) for the full design.

## Status

- **Phase 1 (done):** portable DSP core + desktop offline tool.
- Phase 2+ (todo): firmware wrapper, extra controls. See the plan.

## Layout

```
freeze_core.h        portable, header-only DSP (no Daisy dep) -- the exact pedal DSP
shy_fft.h            vendored FFT (shared with atomic_cluster / Venus / Saturn)
desktop/main.cpp     offline WAV-in / WAV-out test harness
desktop/Makefile     builds the desktop tool
desktop/examples/    rendered demo clips (guitar progression through each mode)
```

## Algorithm (spectral phase-vocoder freeze)

Continuous STFT keeps a short energy-average of the input spectrum. On a freeze
trigger it snapshots those magnitudes and seeds each bin's phase from the live
signal; while frozen it rebuilds the spectrum every hop with the **magnitude held
but the phase advancing** at each bin's centre frequency (plus optional jitter),
then inverse-FFTs and overlap-adds. Holding magnitude while advancing phase is what
makes the drone live instead of comb-filtering. Dry passes through at zero latency;
only the wet drone carries the ~one-window STFT latency (irrelevant for a sustain).

Sizes: 2048-pt FFT, 512 hop (4x overlap), Hann window, COLA-normalized OLA.

## Desktop tool

```
cd desktop && make
./freeze in.wav out.wav [fast|slow|latch] [vol] [dry] [jitter] [hold_s] [gap_s] [start_s]
```

It re-captures the freeze repeatedly across the whole file (hold for `hold_s`,
release for `gap_s`, repeat from `start_s`), so a chord progression gets a fresh
snapshot every cycle. Set `dry 0` to audition the frozen layer alone; `jitter 0..1`
adds phase movement (0 = dead static, up = evolving pad).

Examples:

```
./freeze guitar.wav out.wav fast  1.0 1.0 0.0            # grab each chord, drone while "held"
./freeze guitar.wav out.wav slow  1.0 1.0 0.0 3.5 0.6    # pad-style fade in/out swells
./freeze guitar.wav out.wav latch 1.0 1.0 0.0           # holds hands-free through the gaps
./freeze guitar.wav out.wav fast  1.0 0.0 0.5           # wet only, with phase movement
```
