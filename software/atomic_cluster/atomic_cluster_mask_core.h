// atomic_cluster_mask_core.h
//
// Alternate synthesis engine for the Atomic Cluster: instead of re-building the
// chosen partials with a sine-oscillator bank (see atomic_cluster_core.h), this
// keeps the input's own spectrum and just MASKS it -- the selected peaks' bins
// pass through, everything else is zeroed -- then inverse-FFTs with overlap-add.
//
// Why: a sine bank discards everything except each peak's frequency+amplitude,
// so at max ATOMS it can never equal the input (no transients, no noise, no
// partial shape). Masking keeps the real bins, so:
//
//   * max ATOMS -> keeps (nearly) all peaks -> output approaches the dry input,
//   * min ATOMS -> one peak's lobe survives -> a single note with its NATURAL
//     timbre (not a bare sine).
//
// Two decoupled clocks, mirroring the sine core:
//   * Analysis clock (every kHop samples): FFT, detect peaks, rebuild the bin
//     mask around the currently-selected partials (so it follows pitch drift).
//   * Selection clock (SPEED): randomly re-choose which K=ATOMS peaks are kept
//     (weighted by loudness). SHARP snaps the mask; SMOOTH crossfades it.
//
// Same public API as AtomicClusterCore, so the desktop tool and firmware can
// switch engines with no other changes. NO Daisy/DaisySP dependency.
//
// Latency: ~kN samples (one window, ~43 ms at 48 kHz) -- inherent to STFT-OLA.
// The dry path is delayed by the same amount so DRY/WET stay phase-aligned.

#ifndef ATOMIC_CLUSTER_MASK_CORE_H
#define ATOMIC_CLUSTER_MASK_CORE_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "shy_fft.h"

class AtomicClusterMaskCore
{
  public:
    // ---- compile-time sizes (match the sine core where it matters) ----
    static constexpr size_t kFftOrder  = 11;             // 2^11 = 2048-point FFT
    static constexpr size_t kN         = 1u << kFftOrder;
    static constexpr size_t kHop       = 512;            // analysis hop (4x overlap)
    static constexpr size_t kBins      = kN / 2;
    static constexpr size_t kMaxTracks = 64;             // max ATOMS (peaks kept)
    static constexpr size_t kDetectMax = 64;             // peaks detected per frame
    static constexpr size_t kFifoSize  = 2 * kN;         // output FIFO ring
    static constexpr size_t kFifoMask  = kFifoSize - 1;

    enum class Mode
    {
        kSharp, // snap the mask to the new selection
        kSmooth // crossfade the mask between selections
    };

    // How the random selection weights which peaks are kept.
    enum class Weighting
    {
        kLoud,     // weight by energy (stable, favors fundamentals) -- default
        kBalanced, // sqrt of energy (flatter, more variety)
        kEven      // uniform (picks freely across all peaks, max movement)
    };

    void init(float sample_rate)
    {
        sample_rate_ = sample_rate;

        // Hann window, used for BOTH analysis and synthesis.
        for(size_t i = 0; i < kN; i++)
            window_[i] = 0.5f * (1.0f - cosf(2.0f * kPi * i / (kN - 1)));

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

        fft_.Init();

        std::memset(in_ring_, 0, sizeof(in_ring_));
        std::memset(dry_ring_, 0, sizeof(dry_ring_));
        std::memset(ola_, 0, sizeof(ola_));
        std::memset(mask_gain_, 0, sizeof(mask_gain_));
        std::memset(mask_target_, 0, sizeof(mask_target_));

        in_write_      = 0;
        in_count_      = 0;
        hop_count_     = 0;
        frame_index_   = 0;
        select_accum_  = 0;
        fifo_read_     = 0;
        fifo_write_    = 0;
        fifo_count_    = 0;
        selected_count_= 0;
        max_peak_count_= 0;
        rng_           = 0x2545f491u;
        nan_guard_tripped_ = false;

        set_atoms(16);
        set_speed_ms(300.0f);
        set_blend(0.5f);
        set_vol(1.0f);
        mode_      = Mode::kSharp;
        weighting_ = Weighting::kLoud;
    }

    // ---- parameter setters (host maps knobs to these) ----
    void set_atoms(int atoms)
    {
        if(atoms < 1) atoms = 1;
        if(atoms > (int)kMaxTracks) atoms = (int)kMaxTracks;
        atoms_ = (size_t)atoms;
    }

    void set_speed_ms(float ms)
    {
        if(ms < 5.0f) ms = 5.0f;
        size_t s = (size_t)(ms * 0.001f * sample_rate_);
        if(s < 1) s = 1;
        refresh_samples_ = s;
    }

    void set_blend(float b) { blend_ = clamp01(b); }
    void set_vol(float v)   { vol_   = v < 0.0f ? 0.0f : v; }
    void set_mode(Mode m)   { mode_  = m; }
    void set_weighting(Weighting w) { weighting_ = w; }

    // diagnostics (parallel to the sine core)
    size_t debug_pool_count() const { return selected_count_; }
    size_t debug_max_pool_count() const { return max_peak_count_; }
    bool   debug_nan_tripped() const { return nan_guard_tripped_; }

    // ---- audio: mono in / mono out ----
    void process_block(const float* in, float* out, size_t n)
    {
        for(size_t i = 0; i < n; i++)
        {
            const float dry = in[i];

            in_ring_[in_write_]  = dry;
            dry_ring_[in_write_] = dry;
            in_write_            = (in_write_ + 1) & (kN - 1);
            in_count_++;

            // emit one finished sample (vol applied here; blend done at release)
            float o = 0.0f;
            if(fifo_count_ > 0)
            {
                o          = fifo_[fifo_read_];
                fifo_read_ = (fifo_read_ + 1) & kFifoMask;
                fifo_count_--;
            }
            out[i] = o * vol_;

            // fire the analysis/synthesis hop on the kHop grid (once primed)
            if(in_count_ == kN)
            {
                do_hop(); // first full frame
            }
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
    static constexpr float kPi = 3.14159265358979323846f;
    static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

    // One analysis + synthesis hop for frame `frame_index_`.
    void do_hop()
    {
        // windowed copy of the latest kN samples, oldest first
        for(size_t i = 0; i < kN; i++)
            fft_in_[i] = in_ring_[(in_write_ + i) & (kN - 1)] * window_[i];

        fft_.Direct(fft_in_, fft_spec_); // [0..kBins) real, [kBins..kN) imag

        for(size_t k = 0; k < kBins; k++)
        {
            const float re = fft_spec_[k];
            const float im = fft_spec_[k + kBins];
            energy_[k]     = re * re + im * im;
        }

        detect_peaks();

        // selection clock
        if(selected_count_ == 0 || (select_accum_ += kHop) >= refresh_samples_)
        {
            select_accum_ = 0;
            reselect();
        }

        build_mask_target(); // bins to keep for the current selection
        advance_mask();      // crossfade mask_gain_ toward target (sharp/smooth)

        // apply the mask to the spectrum (magnitude scaled, phase kept)
        for(size_t k = 0; k < kBins; k++)
        {
            const float g     = mask_gain_[k];
            fft_spec_[k]        *= g;
            fft_spec_[k + kBins] *= g;
        }

        fft_.Inverse(fft_spec_, fft_time_);

        // overlap-add this frame into the ring at absolute position k*kHop
        const size_t base = frame_index_ * kHop;
        for(size_t i = 0; i < kN; i++)
            ola_[(base + i) & (kN - 1)]
                += fft_time_[i] * window_[i] * total_scale_;

        // the first kHop samples of this frame are now final: blend with the
        // time-aligned dry sample and push to the output FIFO
        for(size_t i = 0; i < kHop; i++)
        {
            const size_t pos  = base + i;
            const size_t slot = pos & (kN - 1);
            float wet  = ola_[slot];
            ola_[slot] = 0.0f; // clear for reuse when the ring wraps
            if(!std::isfinite(wet))
            {
                wet                 = 0.0f;
                nan_guard_tripped_ = true;
            }

            const float dry   = dry_ring_[pos & (kN - 1)];
            const float mixed = dry * (1.0f - blend_) + wet * blend_;

            fifo_[fifo_write_] = mixed;
            fifo_write_        = (fifo_write_ + 1) & kFifoMask;
            if(fifo_count_ < kFifoSize)
                fifo_count_++;
            else
                fifo_read_ = (fifo_read_ + 1) & kFifoMask; // overflow guard
        }

        frame_index_++;
    }

    // Collect up to kDetectMax strongest local-maxima peaks (bin + energy + Hz).
    void detect_peaks()
    {
        size_t found     = 0;
        float  worst     = 0.0f;
        size_t worst_idx = 0;

        for(size_t k = 2; k < kBins - 1; k++)
        {
            const float e = energy_[k];
            if(e <= energy_[k - 1] || e < energy_[k + 1])
                continue;
            if(e < kEnergyFloor)
                continue;

            if(found < kDetectMax)
            {
                det_bin_[found] = k;
                det_e_[found]   = e;
                found++;
                if(found == kDetectMax)
                    worst_of(det_e_, found, worst, worst_idx);
            }
            else if(e > worst)
            {
                det_bin_[worst_idx] = k;
                det_e_[worst_idx]   = e;
                worst_of(det_e_, found, worst, worst_idx);
            }
        }

        for(size_t i = 0; i < found; i++)
            det_freq_[i] = peak_freq(det_bin_[i]);
        det_count_ = found;

        if(found > max_peak_count_)
            max_peak_count_ = found;
    }

    // Randomly choose K = ATOMS peaks (weighted by energy, without replacement)
    // and store their centre frequencies as the persistent selection.
    void reselect()
    {
        for(size_t i = 0; i < det_count_; i++)
        {
            float w = det_e_[i]; // base weight = energy
            if(weighting_ == Weighting::kBalanced) w = sqrtf(w);
            else if(weighting_ == Weighting::kEven) w = 1.0f;
            cand_w_[i] = w;
        }

        const size_t k = atoms_ < det_count_ ? atoms_ : det_count_;
        selected_count_ = 0;
        for(size_t n = 0; n < k; n++)
        {
            float total = 0.0f;
            for(size_t i = 0; i < det_count_; i++)
                total += cand_w_[i];
            if(total <= 0.0f)
                break;
            const float r      = rand_float() * total;
            float       acc    = 0.0f;
            size_t      chosen = det_count_ - 1;
            for(size_t i = 0; i < det_count_; i++)
            {
                acc += cand_w_[i];
                if(acc >= r)
                {
                    chosen = i;
                    break;
                }
            }
            selected_freq_[selected_count_++] = det_freq_[chosen];
            cand_w_[chosen]                   = 0.0f; // without replacement
        }
    }

    // Build the 0/1 target mask: keep each selected partial's spectral lobe,
    // re-located against the CURRENT spectrum so it follows pitch drift.
    void build_mask_target()
    {
        std::memset(mask_target_, 0, sizeof(mask_target_));
        for(size_t s = 0; s < selected_count_; s++)
        {
            int cb = (int)lroundf(selected_freq_[s] * (float)kN / sample_rate_);
            if(cb < 2) cb = 2;
            if(cb > (int)kBins - 2) cb = (int)kBins - 2;

            // snap to the nearest local energy peak (handles small drift)
            int pb = cb;
            for(int d = -kSnap; d <= kSnap; d++)
            {
                const int j = cb + d;
                if(j < 1 || j >= (int)kBins - 1)
                    continue;
                if(energy_[j] > energy_[pb])
                    pb = j;
            }

            // expand to the valleys on each side, capped at kLobeMax half-width
            int lo = pb, hi = pb;
            while(lo > 1 && pb - lo < kLobeMax && energy_[lo - 1] < energy_[lo])
                lo--;
            while(hi < (int)kBins - 2 && hi - pb < kLobeMax
                  && energy_[hi + 1] < energy_[hi])
                hi++;

            for(int j = lo; j <= hi; j++)
                mask_target_[j] = 1.0f;
        }
    }

    // Move mask_gain_ toward mask_target_ by one hop's worth of crossfade.
    void advance_mask()
    {
        const float step
            = (mode_ == Mode::kSmooth)
                  ? (float)kHop / (float)(refresh_samples_ ? refresh_samples_ : kHop)
                  : 1.0f; // sharp: snap
        for(size_t k = 0; k < kBins; k++)
        {
            const float t = mask_target_[k];
            float       g = mask_gain_[k];
            if(g < t)      { g += step; if(g > t) g = t; }
            else if(g > t) { g -= step; if(g < t) g = t; }
            mask_gain_[k] = g;
        }
    }

    // Parabolic-interpolated peak frequency (Hz) for sub-bin accuracy.
    float peak_freq(size_t k) const
    {
        const float a     = sqrtf(energy_[k - 1]);
        const float b     = sqrtf(energy_[k]);
        const float c     = sqrtf(energy_[k + 1]);
        const float denom = a - 2.0f * b + c;
        float       delta = 0.0f;
        if(fabsf(denom) > 1e-4f) // guard: near-flat triplets make this interpolation unstable
        {
            delta = 0.5f * (a - c) / denom;
            delta = delta < -0.5f ? -0.5f : (delta > 0.5f ? 0.5f : delta);
        }
        const float f = ((float)k + delta) * sample_rate_ / (float)kN;
        return std::isfinite(f) ? f : 0.0f;
    }

    static void worst_of(const float* e, size_t count, float& worst, size_t& worst_idx)
    {
        worst     = e[0];
        worst_idx = 0;
        for(size_t i = 1; i < count; i++)
            if(e[i] < worst)
            {
                worst     = e[i];
                worst_idx = i;
            }
    }

    // xorshift32 -> [0,1)
    float rand_float()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (float)(rng_ & 0x00ffffffu) / (float)0x01000000u;
    }

    // ---- tuning constants ----
    static constexpr float kEnergyFloor = 1e-6f; // ignore noise-floor peaks
    static constexpr int   kSnap        = 2;     // bins searched to re-find a peak
    static constexpr int   kLobeMax     = 6;     // max half-width of a kept lobe

    ShyFFT<float, kN, RotationPhasor> fft_;

    float  sample_rate_     = 48000.0f;
    size_t refresh_samples_ = 14400;
    size_t atoms_           = 16;
    float  blend_           = 0.5f;
    float  vol_             = 1.0f;
    Mode   mode_            = Mode::kSharp;
    Weighting weighting_    = Weighting::kLoud;
    float  total_scale_     = 1.0f;

    // input history + dry delay (both indexed by input position & (kN-1))
    float  in_ring_[kN];
    float  dry_ring_[kN];
    size_t in_write_ = 0;
    size_t in_count_ = 0;

    // clocks
    size_t hop_count_    = 0;
    size_t frame_index_  = 0;
    size_t select_accum_ = 0;

    // FFT scratch
    float window_[kN];
    float fft_in_[kN];
    float fft_spec_[kN];
    float fft_time_[kN];
    float energy_[kBins];

    // overlap-add accumulator + output FIFO
    float  ola_[kN];
    float  fifo_[kFifoSize];
    size_t fifo_read_  = 0;
    size_t fifo_write_ = 0;
    size_t fifo_count_ = 0;

    // detected peaks (per frame)
    size_t det_bin_[kDetectMax];
    float  det_e_[kDetectMax];
    float  det_freq_[kDetectMax];
    float  cand_w_[kDetectMax];
    size_t det_count_ = 0;

    // persistent selection (frequencies) + masks
    float  selected_freq_[kMaxTracks];
    size_t selected_count_ = 0;
    float  mask_target_[kBins];
    float  mask_gain_[kBins];

    size_t   max_peak_count_ = 0;
    uint32_t rng_            = 0x2545f491u;
    bool     nan_guard_tripped_ = false;
};

#endif // ATOMIC_CLUSTER_MASK_CORE_H
