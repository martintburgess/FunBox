// freeze_core.h
//
// Portable DSP core for the Funbox "Freeze" -- an EHX Freeze (Sound Retainer)
// style infinite sustain. It takes an instantaneous snapshot of whatever is
// sounding and holds it as a drone until released, with FAST / SLOW / LATCH
// behaviors. See plans/freeze.md.
//
// Algorithm: spectral phase-vocoder freeze.
//   * Analysis (every kHop samples, always while primed): window the latest kN
//     input samples, forward-FFT, and keep a short exponential average of each
//     bin's ENERGY. This "recent spectrum" is always ready to be captured.
//   * Capture (on freeze trigger): snapshot the averaged magnitudes into
//     frozen_mag_[] and seed each bin's synthesis phase from the live spectrum.
//   * Synthesis (every kHop while frozen): rebuild the spectrum from the frozen
//     magnitudes with each bin's phase ADVANCING at its centre frequency (plus
//     optional jitter), inverse-FFT, window, and overlap-add. Freezing the
//     magnitude but advancing the phase is what makes the drone live instead of
//     comb-filtering.
//
// The dry signal passes straight through at zero latency (you play over the
// drone). Only the wet/frozen layer carries the STFT latency (~one window),
// which is irrelevant for a sustained sound.
//
// No Daisy/DaisySP dependency -- this compiles identically into the firmware
// and the desktop offline tool.

#ifndef FREEZE_CORE_H
#define FREEZE_CORE_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "shy_fft.h"

class FreezeCore
{
  public:
    // ---- compile-time sizes (match the atomic_cluster mask core) ----
    static constexpr size_t kFftOrder = 11;          // 2^11 = 2048-point FFT
    static constexpr size_t kN        = 1u << kFftOrder;
    static constexpr size_t kHop      = 512;          // analysis/synth hop (4x overlap)
    static constexpr size_t kBins     = kN / 2;
    static constexpr size_t kFifoSize = 2 * kN;       // wet output FIFO ring
    static constexpr size_t kFifoMask = kFifoSize - 1;

    enum class Mode
    {
        kFast,  // capture + sustain instantly; cut instantly on release
        kSlow,  // captured sound fades in, then fades out slowly on release
        kLatch  // capture persists after release; re-press layers; clear() kills
    };

    void init(float sample_rate)
    {
        sample_rate_ = sample_rate;

        // Hann window, used for BOTH analysis and synthesis.
        for(size_t i = 0; i < kN; i++)
            window_[i] = 0.5f * (1.0f - cosf(kTwoPi * i / (kN - 1)));

        // COLA constant: steady-state sum of (analysis*synth) windows across the
        // overlapping hops. Output is scaled by 1/(kN * cola) -- the kN undoes the
        // unnormalized inverse FFT, the cola undoes the double-windowing overlap.
        double cola = 0.0;
        const long p0 = (long)(kN / 2);
        for(long k = -16; k <= 16; k++)
        {
            const long idx = p0 - k * (long)kHop;
            if(idx >= 0 && idx < (long)kN)
                cola += (double)window_[idx] * (double)window_[idx];
        }
        total_scale_ = (float)(1.0 / ((double)kN * cola));

        // Per-hop phase increment for each bin = its centre frequency, reduced
        // modulo 2*pi so the running phase never grows unbounded.
        for(size_t k = 0; k < kBins; k++)
        {
            double inc = kTwoPi * (double)k * (double)kHop / (double)kN;
            inc        = fmod(inc, (double)kTwoPi);
            phase_inc_[k] = (float)inc;
        }

        fft_.Init();

        std::memset(in_ring_, 0, sizeof(in_ring_));
        std::memset(ola_, 0, sizeof(ola_));
        std::memset(avg_e_, 0, sizeof(avg_e_));
        std::memset(frozen_mag_, 0, sizeof(frozen_mag_));
        std::memset(phase_, 0, sizeof(phase_));

        in_write_    = 0;
        in_count_    = 0;
        hop_count_   = 0;
        frame_index_ = 0;
        fifo_read_   = 0;
        fifo_write_  = 0;
        fifo_count_  = 0;

        frozen_         = false;
        freeze_held_    = false;
        capture_pending_ = false;
        layer_pending_  = false;
        env_            = 0.0f;
        env_target_     = 0.0f;
        rng_            = 0x2545f491u;

        set_volume(1.0f);
        set_dry(1.0f);
        set_jitter(0.0f);
        fade_ms_ = 600.0f;
        set_mode(Mode::kFast);
    }

    // ---- parameter setters (host maps knobs/switch to these) ----
    void set_volume(float v) { volume_ = v < 0.0f ? 0.0f : v; }  // frozen level
    void set_dry(float d)    { dry_    = clamp01(d); }           // live level (1 = unity)
    void set_jitter(float j) { jitter_rad_ = clamp01(j) * kMaxJitter; }

    void set_fade_ms(float ms)
    {
        fade_ms_ = ms < 1.0f ? 1.0f : ms;
        update_env_rates();
    }

    void set_mode(Mode m)
    {
        mode_ = m;
        update_env_rates();
    }

    // ---- freeze control (host maps footswitch to these) ----
    // held == true on press, false on release. LATCH ignores release; use
    // clear() (e.g. double-tap) to end a latched drone.
    void set_freeze(bool held)
    {
        if(held == freeze_held_)
            return;
        freeze_held_ = held;

        if(held) // press
        {
            if(mode_ == Mode::kLatch && frozen_)
                layer_pending_ = true;   // add this snapshot on top
            else
                capture_pending_ = true; // fresh capture
            env_target_ = 1.0f;
        }
        else // release
        {
            if(mode_ != Mode::kLatch)
                env_target_ = 0.0f;      // FAST/SLOW fade/cut out; LATCH holds
        }
    }

    // End a latched drone (double-tap on the pedal): fade it out.
    void clear()
    {
        env_target_  = 0.0f;
        freeze_held_ = false;
    }

    // Immediately drop the frozen layer and silence the wet path (no fade). Use
    // when replacing the held sound with a fresh capture cleanly.
    void reset()
    {
        frozen_          = false;
        freeze_held_     = false;
        capture_pending_ = false;
        layer_pending_   = false;
        env_             = 0.0f;
        env_target_      = 0.0f;
        fifo_read_       = 0;
        fifo_write_      = 0;
        fifo_count_      = 0;
        frame_index_     = 0;
        std::memset(ola_, 0, sizeof(ola_));
        std::memset(frozen_mag_, 0, sizeof(frozen_mag_));
    }

    // diagnostics
    bool debug_frozen() const { return frozen_; }

    // ---- audio: mono in / mono out ----
    void process_block(const float* in, float* out, size_t n)
    {
        for(size_t i = 0; i < n; i++)
        {
            const float dry = in[i];

            in_ring_[in_write_] = dry;
            in_write_           = (in_write_ + 1) & (kN - 1);
            in_count_++;

            update_env();

            // pull one finished wet sample from the FIFO
            float wet = 0.0f;
            if(fifo_count_ > 0)
            {
                wet        = fifo_[fifo_read_];
                fifo_read_ = (fifo_read_ + 1) & kFifoMask;
                fifo_count_--;
            }

            out[i] = dry * dry_ + wet * env_ * volume_;

            // fire the analysis/synthesis hop on the kHop grid (once primed)
            if(in_count_ == kN)
                do_hop();
            else if(in_count_ > kN)
            {
                if(++hop_count_ >= kHop)
                {
                    hop_count_ = 0;
                    do_hop();
                }
            }
        }
    }

  private:
    static constexpr float kPi        = 3.14159265358979323846f;
    static constexpr float kTwoPi     = 2.0f * kPi;
    static constexpr float kMaxJitter = 0.6f;   // radians of per-hop phase jitter at max
    static constexpr float kEnergyEma = 0.4f;   // energy average (~3-4 frames)

    static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

    // One hop: always analyze; capture/layer if pending; synthesize if frozen.
    void do_hop()
    {
        // ANALYSIS -- windowed copy of the latest kN samples, oldest first.
        for(size_t i = 0; i < kN; i++)
            fft_in_[i] = in_ring_[(in_write_ + i) & (kN - 1)] * window_[i];

        fft_.Direct(fft_in_, fft_spec_); // [0..kBins) real, [kBins..kN) imag

        // exponential average of each bin's energy -> the "recent spectrum"
        for(size_t k = 0; k < kBins; k++)
        {
            const float re = fft_spec_[k];
            const float im = fft_spec_[k + kBins];
            const float e  = re * re + im * im;
            avg_e_[k] += kEnergyEma * (e - avg_e_[k]);
        }

        if(capture_pending_)
        {
            capture(false);
            capture_pending_ = false;
        }
        if(layer_pending_)
        {
            capture(true);
            layer_pending_ = false;
        }

        if(frozen_)
            synth_hop();
    }

    // Snapshot the recent spectrum into the frozen layer.
    //  fresh: replace it and reset the synthesis stream + seed phases from the
    //         live spectrum. layer: add the new magnitudes on top (LATCH).
    void capture(bool layer)
    {
        if(in_count_ < kN)
            return; // not primed -- nothing meaningful to freeze yet

        if(!layer)
        {
            // fresh drone: clear the reconstruction state for a clean start
            std::memset(ola_, 0, sizeof(ola_));
            fifo_read_   = 0;
            fifo_write_  = 0;
            fifo_count_  = 0;
            frame_index_ = 0;

            for(size_t k = 0; k < kBins; k++)
            {
                frozen_mag_[k] = sqrtf(avg_e_[k]);
                phase_[k]      = atan2f(fft_spec_[k + kBins], fft_spec_[k]);
            }
            frozen_ = true;
        }
        else
        {
            // add this capture's magnitudes; keep the existing advancing phases
            for(size_t k = 0; k < kBins; k++)
                frozen_mag_[k] += sqrtf(avg_e_[k]);
        }
        frozen_mag_[0] = 0.0f; // no DC offset
    }

    // Rebuild the spectrum from frozen magnitudes + advancing phase, then
    // inverse-FFT and overlap-add one frame of the drone.
    void synth_hop()
    {
        fft_spec_[0]     = 0.0f; // DC
        fft_spec_[kBins] = 0.0f;
        for(size_t k = 1; k < kBins; k++)
        {
            float ph = phase_[k] + phase_inc_[k];
            if(jitter_rad_ > 0.0f)
                ph += jitter_rad_ * (rand_float() * 2.0f - 1.0f);
            if(ph >= kTwoPi) ph -= kTwoPi;
            else if(ph < 0.0f) ph += kTwoPi;
            phase_[k] = ph;

            const float m       = frozen_mag_[k];
            fft_spec_[k]        = m * cosf(ph);
            fft_spec_[k + kBins] = m * sinf(ph);
        }

        fft_.Inverse(fft_spec_, fft_time_);

        // overlap-add this frame into the ring at absolute position frame*kHop
        const size_t base = frame_index_ * kHop;
        for(size_t i = 0; i < kN; i++)
            ola_[(base + i) & (kN - 1)]
                += fft_time_[i] * window_[i] * total_scale_;

        // the first kHop samples of this frame are final -> push to the wet FIFO
        for(size_t i = 0; i < kHop; i++)
        {
            const size_t slot = (base + i) & (kN - 1);
            fifo_[fifo_write_] = ola_[slot];
            ola_[slot]         = 0.0f; // clear for reuse when the ring wraps
            fifo_write_        = (fifo_write_ + 1) & kFifoMask;
            if(fifo_count_ < kFifoSize)
                fifo_count_++;
            else
                fifo_read_ = (fifo_read_ + 1) & kFifoMask; // overflow guard
        }

        frame_index_++;
    }

    // Per-sample envelope ramp toward env_target_; drop the drone once a
    // FAST/SLOW release has faded fully out.
    void update_env()
    {
        if(env_ < env_target_)
        {
            env_ += attack_step_;
            if(env_ > env_target_) env_ = env_target_;
        }
        else if(env_ > env_target_)
        {
            env_ -= release_step_;
            if(env_ < env_target_) env_ = env_target_;
        }

        if(frozen_ && env_target_ <= 0.0f && env_ <= 0.0f)
            frozen_ = false; // release finished: stop synthesizing
    }

    void update_env_rates()
    {
        float atk_ms, rel_ms;
        switch(mode_)
        {
            case Mode::kSlow:  atk_ms = fade_ms_; rel_ms = fade_ms_; break;
            case Mode::kLatch: atk_ms = 5.0f;     rel_ms = 20.0f;    break;
            case Mode::kFast:
            default:           atk_ms = 5.0f;     rel_ms = 5.0f;     break;
        }
        attack_step_  = 1.0f / (atk_ms * 0.001f * sample_rate_);
        release_step_ = 1.0f / (rel_ms * 0.001f * sample_rate_);
    }

    // xorshift32 -> [0,1)
    float rand_float()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (float)(rng_ & 0x00ffffffu) / (float)0x01000000u;
    }

    ShyFFT<float, kN, RotationPhasor> fft_;

    float sample_rate_ = 48000.0f;
    float total_scale_ = 1.0f;

    // parameters
    float volume_     = 1.0f;
    float dry_        = 1.0f;
    float jitter_rad_ = 0.0f;
    float fade_ms_    = 600.0f;
    Mode  mode_       = Mode::kFast;

    // input history (indexed by input position & (kN-1))
    float  in_ring_[kN];
    size_t in_write_ = 0;
    size_t in_count_ = 0;

    // clocks
    size_t hop_count_   = 0;
    size_t frame_index_ = 0;

    // FFT scratch
    float window_[kN];
    float phase_inc_[kBins];
    float fft_in_[kN];
    float fft_spec_[kN];
    float fft_time_[kN];
    float avg_e_[kBins]; // recent-energy average (magnitude^2)

    // frozen layer
    float frozen_mag_[kBins];
    float phase_[kBins];

    // reconstruction
    float  ola_[kN];
    float  fifo_[kFifoSize];
    size_t fifo_read_  = 0;
    size_t fifo_write_ = 0;
    size_t fifo_count_ = 0;

    // freeze / envelope state
    bool  frozen_          = false;
    bool  freeze_held_     = false;
    bool  capture_pending_ = false;
    bool  layer_pending_   = false;
    float env_             = 0.0f;
    float env_target_      = 0.0f;
    float attack_step_     = 1.0f;
    float release_step_    = 1.0f;

    uint32_t rng_ = 0x2545f491u;
};

#endif // FREEZE_CORE_H
