// atomic_cluster_core.h
//
// Portable, header-only DSP core for the Atomic Cluster module — a "spectral
// decomposer". Two decoupled clocks:
//
//   * Analysis clock (fast, overlapping FFT every kHop samples): maintains a set
//     of tracked partials that continuously follow the input's frequency and
//     amplitude via peak continuation. This is what makes max-ATOMS track the
//     input smoothly instead of stepping/flickering.
//   * Selection clock (SPEED): every refresh, randomly picks which K tracks are
//     audible (weighted toward louder ones), crossfading their audibility.
//
//   ATOMS = K, how many oscillations sound at once (max -> close to the input,
//           min -> a single note jumping around).
//   SPEED = how often the audible set is re-chosen (how fast the pitches jump).
//   MODE  = SHARP (instant jumps) or SMOOTH (crossfaded jumps).
//
// Has NO Daisy / DaisySP dependency (only shy_fft.h + standard C++), so the
// exact same code compiles into the Funbox firmware and into the desktop WAV
// tool. See plans/atomic-cluster.md.

#ifndef ATOMIC_CLUSTER_CORE_H
#define ATOMIC_CLUSTER_CORE_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "shy_fft.h"

class AtomicClusterCore
{
  public:
    // ---- compile-time sizes ----
    static constexpr size_t kFftOrder  = 11;             // 2^11 = 2048-point FFT
    static constexpr size_t kN         = 1u << kFftOrder;
    static constexpr size_t kHop       = 512;            // analysis hop (4x overlap)
    static constexpr size_t kMaxTracks = 64;             // tracked partials / oscillators
    static constexpr size_t kDetectMax = 64;             // peaks detected per frame
    static constexpr size_t kTableBits = 10;             // sine table 1024 + guard
    static constexpr size_t kTableSize = 1u << kTableBits;

    enum class Mode
    {
        kSharp, // instant transition between atom sets (rhythmic)
        kSmooth // crossfade between atom sets (lush)
    };

    void init(float sample_rate)
    {
        sample_rate_ = sample_rate;

        // Hann analysis window.
        for(size_t i = 0; i < kN; i++)
            window_[i] = 0.5f * (1.0f - cosf(2.0f * kPi * i / (kN - 1)));

        // One shared sine period (+1 guard sample for linear interp).
        for(size_t i = 0; i <= kTableSize; i++)
            sine_[i] = sinf(2.0f * kPi * i / kTableSize);

        fft_.Init();

        std::memset(ring_, 0, sizeof(ring_));
        write_pos_ = 0;

        for(size_t i = 0; i < kMaxTracks; i++)
            tracks_[i] = Track{};

        // per-sample one-pole coefficients for amplitude/frequency tracking
        amp_coeff_  = 1.0f - expf(-1.0f / (0.020f * sample_rate_)); // ~20 ms
        freq_coeff_ = 1.0f - expf(-1.0f / (0.008f * sample_rate_)); // ~8 ms

        // sensible defaults; the host overrides these
        set_atoms(16);
        set_speed_ms(300.0f);
        set_blend(0.5f);
        set_vol(1.0f);
        mode_            = Mode::kSharp;
        analysis_count_  = 0;
        select_count_    = 0;
        alive_count_     = 0;
        max_alive_count_ = 0;
        rng_             = 0x2545f491u;
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
        if(ms < 5.0f) ms = 5.0f; // floor so the refresh never collapses to ~0
        size_t s = (size_t)(ms * 0.001f * sample_rate_);
        if(s < 1) s = 1;
        refresh_samples_ = s;
    }

    void set_blend(float b) { blend_ = clamp01(b); }   // 0 dry .. 1 wet
    void set_vol(float v)   { vol_   = v < 0.0f ? 0.0f : v; }
    void set_mode(Mode m)   { mode_  = m; }

    // diagnostics
    size_t debug_pool_count() const { return alive_count_; }
    size_t debug_max_pool_count() const { return max_alive_count_; }

    // ---- audio ----
    // Mono in / mono out. The firmware feeds the left channel and copies the
    // result to both outputs; the desktop tool downmixes to mono.
    void process_block(const float* in, float* out, size_t n)
    {
        for(size_t i = 0; i < n; i++)
        {
            const float dry = in[i];

            ring_[write_pos_] = dry;
            write_pos_        = (write_pos_ + 1) % kN;

            if(++analysis_count_ >= kHop)
            {
                analysis_count_ = 0;
                analyze_frame(); // track partials (follow the input)
            }
            if(++select_count_ >= refresh_samples_)
            {
                select_count_ = 0;
                reselect();      // re-choose the audible set
            }

            const float wet = synth_sample();
            out[i]          = (dry * (1.0f - blend_) + wet * blend_) * vol_;
        }
    }

  private:
    static constexpr float kPi = 3.14159265358979323846f;

    // A tracked partial: an oscillator that continuously follows one component
    // of the input, with an audibility gain set by the random selection.
    struct Track
    {
        bool  alive = false;
        float phase = 0.0f;       // 0..1
        float inc = 0.0f;         // cycles/sample (smoothed)
        float target_inc = 0.0f;  // latest detected frequency
        float amp = 0.0f;         // tracked amplitude (smoothed, follows input)
        float target_amp = 0.0f;  // latest detected magnitude (0 -> fading out)
        float sel_gain = 0.0f;    // 0..1 audibility
        float sel_target = 0.0f;  // 0 or 1
        float sel_step = 0.0f;    // per-sample crossfade increment
    };

    static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

    // ---- analysis: detect peaks and continue tracks ----
    void analyze_frame()
    {
        // Windowed copy of the input history in time order (oldest first).
        for(size_t i = 0; i < kN; i++)
            fft_in_[i] = ring_[(write_pos_ + i) % kN] * window_[i];

        fft_.Direct(fft_in_, fft_out_); // out: [0..N/2) real, [N/2..N) imag

        constexpr size_t kBins = kN / 2;
        for(size_t k = 0; k < kBins; k++)
        {
            const float re = fft_out_[k];
            const float im = fft_out_[k + kBins];
            energy_[k]     = re * re + im * im;
        }

        detect_peaks();
        continue_tracks();
    }

    // Collect up to kDetectMax strongest local-maxima peaks for this frame.
    void detect_peaks()
    {
        constexpr size_t kBins = kN / 2;
        size_t           found = 0;
        float            worst = 0.0f;
        size_t           worst_idx = 0;

        for(size_t k = 2; k < kBins - 1; k++)
        {
            const float e = energy_[k];
            if(e <= energy_[k - 1] || e < energy_[k + 1]) // local maximum?
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
        {
            peak_freq_amp(det_bin_[i], det_inc_[i], det_mag_[i]);
            det_used_[i] = false;
        }
        det_count_ = found;
    }

    // Match peaks to existing tracks by frequency (peak continuation), update
    // matched tracks, fade unmatched tracks out, and birth new tracks.
    void continue_tracks()
    {
        // process strongest tracks first so they claim their peaks
        size_t na = 0;
        for(size_t t = 0; t < kMaxTracks; t++)
            if(tracks_[t].alive)
                order_[na++] = t;
        for(size_t i = 1; i < na; i++)
        {
            const size_t key = order_[i];
            const float  ka  = tracks_[key].amp;
            size_t       j   = i;
            while(j > 0 && tracks_[order_[j - 1]].amp < ka)
            {
                order_[j] = order_[j - 1];
                j--;
            }
            order_[j] = key;
        }

        constexpr float kTolInc = kMatchBins / (float)kN; // ~1.5 bins, in inc units
        for(size_t oi = 0; oi < na; oi++)
        {
            Track& T    = tracks_[order_[oi]];
            size_t best = SIZE_MAX;
            float  bestd = kTolInc;
            for(size_t d = 0; d < det_count_; d++)
            {
                if(det_used_[d])
                    continue;
                const float diff = fabsf(T.inc - det_inc_[d]);
                if(diff < bestd)
                {
                    bestd = diff;
                    best  = d;
                }
            }
            if(best != SIZE_MAX)
            {
                det_used_[best] = true;
                T.target_inc    = det_inc_[best];
                T.target_amp    = det_mag_[best];
            }
            else
            {
                T.target_amp = 0.0f; // no support -> fade out and eventually die
            }
        }

        // birth a track for each unclaimed peak (if a slot is free)
        for(size_t d = 0; d < det_count_; d++)
        {
            if(det_used_[d])
                continue;
            size_t slot = SIZE_MAX;
            for(size_t t = 0; t < kMaxTracks; t++)
                if(!tracks_[t].alive)
                {
                    slot = t;
                    break;
                }
            if(slot == SIZE_MAX)
                break; // bank full; drop the weakest new peaks

            Track& T     = tracks_[slot];
            T.alive      = true;
            T.inc        = det_inc_[d];
            T.target_inc = det_inc_[d];
            T.amp        = 0.0f;        // ramps up via amp tracking (soft attack)
            T.target_amp = det_mag_[d];
            T.phase      = rand_float(); // decorrelate simultaneous births
            T.sel_gain   = 0.0f;
            T.sel_target = 0.0f;
            T.sel_step   = 0.0f;
        }

        alive_count_ = 0;
        for(size_t t = 0; t < kMaxTracks; t++)
            if(tracks_[t].alive)
                alive_count_++;
        if(alive_count_ > max_alive_count_)
            max_alive_count_ = alive_count_;
    }

    // ---- selection: randomly choose which tracks are audible ----
    void reselect()
    {
        size_t na = 0;
        for(size_t t = 0; t < kMaxTracks; t++)
            if(tracks_[t].alive)
            {
                cand_idx_[na] = t;
                cand_w_[na]   = tracks_[t].amp;
                na++;
                tracks_[t].sel_target = 0.0f;
            }

        const size_t k = atoms_ < na ? atoms_ : na;
        for(size_t n = 0; n < k; n++)
        {
            float total = 0.0f;
            for(size_t i = 0; i < na; i++)
                total += cand_w_[i];
            if(total <= 0.0f)
                break;
            const float r      = rand_float() * total;
            float       acc    = 0.0f;
            size_t      chosen = na - 1;
            for(size_t i = 0; i < na; i++)
            {
                acc += cand_w_[i];
                if(acc >= r)
                {
                    chosen = i;
                    break;
                }
            }
            tracks_[cand_idx_[chosen]].sel_target = 1.0f;
            cand_w_[chosen]                       = 0.0f; // without replacement
        }

        const int fade_len
            = (mode_ == Mode::kSmooth) ? (int)refresh_samples_ : kSharpRampSamples;
        for(size_t i = 0; i < na; i++)
        {
            Track& T   = tracks_[cand_idx_[i]];
            T.sel_step = (T.sel_target - T.sel_gain) / (float)fade_len;
        }
    }

    // ---- synthesis ----
    float synth_sample()
    {
        float wet = 0.0f;
        for(size_t t = 0; t < kMaxTracks; t++)
        {
            Track& T = tracks_[t];
            if(!T.alive)
                continue;

            // follow the input: smooth amplitude and frequency toward targets
            T.amp += (T.target_amp - T.amp) * amp_coeff_;
            T.inc += (T.target_inc - T.inc) * freq_coeff_;

            // audibility crossfade toward the selection target
            if(T.sel_gain != T.sel_target)
            {
                T.sel_gain += T.sel_step;
                if((T.sel_step > 0.0f && T.sel_gain >= T.sel_target)
                   || (T.sel_step < 0.0f && T.sel_gain <= T.sel_target))
                    T.sel_gain = T.sel_target;
            }

            const float g = T.amp * T.sel_gain;
            if(g > 1e-7f)
                wet += osc_lookup(T.phase) * g;

            T.phase += T.inc;
            if(T.phase >= 1.0f)
                T.phase -= (float)(int)T.phase;

            // retire a faded-out, unselected, unsupported track to free its slot
            if(T.target_amp <= 0.0f && T.amp < 1e-6f && T.sel_gain < 1e-6f)
                T.alive = false;
        }
        return wet;
    }

    // linear-interpolated shared sine-table lookup, phase in [0,1)
    inline float osc_lookup(float phase) const
    {
        const float  fidx = phase * kTableSize;
        const size_t idx  = (size_t)fidx;
        const float  frac = fidx - (float)idx;
        const float  s0   = sine_[idx];
        const float  s1   = sine_[idx + 1];
        return s0 + (s1 - s0) * frac;
    }

    // Parabolic interpolation on magnitude for sub-bin frequency accuracy.
    void peak_freq_amp(size_t k, float& inc, float& amp) const
    {
        const float a     = sqrtf(energy_[k - 1]);
        const float b     = sqrtf(energy_[k]);
        const float c     = sqrtf(energy_[k + 1]);
        const float denom = a - 2.0f * b + c;
        float       delta = 0.0f;
        if(fabsf(denom) > 1e-12f)
            delta = 0.5f * (a - c) / denom;
        const float freq = ((float)k + delta) * sample_rate_ / (float)kN;
        const float mag  = b - 0.25f * (a - c) * delta;
        inc = freq / sample_rate_; // cycles/sample
        amp = mag * kAmpNorm;       // calibrated by ear
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

    // xorshift32 — portable, deterministic, returns [0,1).
    float rand_float()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (float)(rng_ & 0x00ffffffu) / (float)0x01000000u;
    }

    // ---- tuning constants (revisit by ear / profiling) ----
    static constexpr float kAmpNorm     = 4.0f / (float)kN; // sinusoid mag -> amp
    static constexpr float kEnergyFloor = 1e-6f;            // ignore noise-floor peaks
    static constexpr float kMatchBins   = 1.5f;             // peak-continuation tolerance
    static constexpr int   kSharpRampSamples = 48;          // anti-click selection ramp

    ShyFFT<float, kN, RotationPhasor> fft_;

    float  sample_rate_     = 48000.0f;
    size_t refresh_samples_ = 14400;
    size_t atoms_           = 16;
    float  blend_           = 0.5f;
    float  vol_             = 1.0f;
    Mode   mode_            = Mode::kSharp;

    float  amp_coeff_  = 0.001f;
    float  freq_coeff_ = 0.0025f;

    // analysis / selection clocks
    size_t analysis_count_ = 0;
    size_t select_count_   = 0;

    float  ring_[kN];
    size_t write_pos_ = 0;

    float window_[kN];
    float sine_[kTableSize + 1];
    float fft_in_[kN];
    float fft_out_[kN];
    float energy_[kN / 2];

    Track tracks_[kMaxTracks];

    // detected-peak scratch (per analysis frame)
    size_t det_bin_[kDetectMax];
    float  det_e_[kDetectMax];
    float  det_inc_[kDetectMax];
    float  det_mag_[kDetectMax];
    bool   det_used_[kDetectMax];
    size_t det_count_ = 0;

    // selection / ordering scratch
    size_t order_[kMaxTracks];
    size_t cand_idx_[kMaxTracks];
    float  cand_w_[kMaxTracks];

    size_t   alive_count_     = 0;
    size_t   max_alive_count_ = 0;
    uint32_t rng_             = 0x2545f491u;
};

#endif // ATOMIC_CLUSTER_CORE_H
