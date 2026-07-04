// Desktop offline test harness for the Atomic Cluster cores.
//
// Reads a WAV, runs it through one of the two synthesis engines (the exact DSP
// the pedal runs), and writes a WAV -- so you can A/B them on a guitar clip
// before touching the firmware. See plans/atomic-cluster.md.
//
//   * sine -- oscillator-bank resynthesis      (atomic_cluster_core.h)
//   * mask -- spectral mask + inverse-FFT/OLA   (atomic_cluster_mask_core.h)
//
// On the pedal these will be selectable with a toggle switch.
//
// Build:
//   make
// Run:
//   ./cluster in.wav out.wav [atoms] [speed_ms] [blend] [vol] [sharp|smooth] [sine|mask]

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "atomic_cluster_core.h"
#include "atomic_cluster_mask_core.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct Params
{
    int   atoms;
    float speed_ms;
    float blend;
    float vol;
    bool  smooth;
};

// Runs either engine (same public API) over the mono buffer and returns the wet.
template <typename Core>
static std::vector<float>
run_engine(const std::vector<float>& mono, float sample_rate, const Params& p,
           size_t& last_pool, size_t& max_pool, size_t& cap)
{
    Core core;
    core.init(sample_rate);
    core.set_atoms(p.atoms);
    core.set_speed_ms(p.speed_ms);
    core.set_blend(p.blend);
    core.set_vol(p.vol);
    core.set_mode(p.smooth ? Core::Mode::kSmooth : Core::Mode::kSharp);

    std::vector<float> wet(mono.size());
    const size_t       block = 48; // same as the Funbox AudioCallback block size
    for(size_t i = 0; i < mono.size(); i += block)
    {
        const size_t n = std::min(block, mono.size() - i);
        core.process_block(&mono[i], &wet[i], n);
    }
    last_pool = core.debug_pool_count();
    max_pool  = core.debug_max_pool_count();
    cap       = Core::kMaxTracks;
    return wet;
}

int main(int argc, char** argv)
{
    if(argc < 3)
    {
        std::fprintf(stderr,
                     "usage: %s in.wav out.wav "
                     "[atoms=16] [speed_ms=300] [blend=0.7] [vol=1.0] "
                     "[sharp|smooth] [sine|mask]\n",
                     argv[0]);
        return 1;
    }

    const char* in_path  = argv[1];
    const char* out_path = argv[2];

    Params p;
    p.atoms          = argc > 3 ? std::atoi(argv[3]) : 16;
    p.speed_ms       = argc > 4 ? (float)std::atof(argv[4]) : 300.0f;
    p.blend          = argc > 5 ? (float)std::atof(argv[5]) : 0.7f;
    p.vol            = argc > 6 ? (float)std::atof(argv[6]) : 1.0f;
    p.smooth         = argc > 7 && std::string(argv[7]) == "smooth";
    const bool mask  = argc > 8 && std::string(argv[8]) == "mask";

    // ---- read input (de-interleaved to mono) ----
    unsigned int channels = 0, sample_rate = 0;
    drwav_uint64 frame_count = 0;
    float*       interleaved = drwav_open_file_and_read_pcm_frames_f32(
        in_path, &channels, &sample_rate, &frame_count, nullptr);
    if(!interleaved)
    {
        std::fprintf(stderr, "error: could not read '%s'\n", in_path);
        return 1;
    }

    std::vector<float> mono((size_t)frame_count);
    for(drwav_uint64 i = 0; i < frame_count; i++)
    {
        float acc = 0.0f;
        for(unsigned int c = 0; c < channels; c++)
            acc += interleaved[i * channels + c];
        mono[(size_t)i] = acc / (float)channels;
    }
    drwav_free(interleaved, nullptr);

    std::printf("in: %s  %u Hz  %u ch  %llu frames\n",
                in_path, sample_rate, channels,
                (unsigned long long)frame_count);
    std::printf("params: atoms=%d speed=%.1fms blend=%.2f vol=%.2f mode=%s engine=%s\n",
                p.atoms, p.speed_ms, p.blend, p.vol, p.smooth ? "smooth" : "sharp",
                mask ? "mask" : "sine");

    // ---- process ----
    size_t             last_pool = 0, max_pool = 0, cap = 0;
    std::vector<float> wet
        = mask ? run_engine<AtomicClusterMaskCore>(mono, (float)sample_rate, p,
                                                    last_pool, max_pool, cap)
               : run_engine<AtomicClusterCore>(mono, (float)sample_rate, p,
                                                last_pool, max_pool, cap);

    // ---- write mono output ----
    drwav_data_format fmt;
    fmt.container     = drwav_container_riff;
    fmt.format        = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels      = 1;
    fmt.sampleRate    = sample_rate;
    fmt.bitsPerSample = 32;

    drwav out;
    if(!drwav_init_file_write(&out, out_path, &fmt, nullptr))
    {
        std::fprintf(stderr, "error: could not open '%s' for write\n", out_path);
        return 1;
    }
    drwav_write_pcm_frames(&out, wet.size(), wet.data());
    drwav_uninit(&out);

    std::printf("wrote: %s  (selected/pool last frame: %zu)\n", out_path, last_pool);
    std::printf("max peaks detected over file: %zu  (atom cap = %zu)\n",
                max_pool, cap);
    return 0;
}
