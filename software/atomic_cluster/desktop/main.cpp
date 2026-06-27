// Desktop offline test harness for the Atomic Cluster core.
//
// Reads a WAV, runs it through AtomicClusterCore (the exact DSP the pedal runs),
// and writes a WAV — so you can tune the effect on a guitar clip before
// touching the firmware. See plans/atomic-cluster.md.
//
// Build:
//   clang++ -std=c++17 -O2 -I.. main.cpp -o /tmp/cluster
// Run:
//   /tmp/cluster in.wav out.wav [atoms] [speed_ms] [blend] [vol] [sharp|smooth]

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "atomic_cluster_core.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if(argc < 3)
    {
        std::fprintf(stderr,
                     "usage: %s in.wav out.wav "
                     "[atoms=16] [speed_ms=300] [blend=0.7] [vol=1.0] "
                     "[sharp|smooth]\n",
                     argv[0]);
        return 1;
    }

    const char* in_path  = argv[1];
    const char* out_path = argv[2];

    const int   atoms    = argc > 3 ? std::atoi(argv[3]) : 16;
    const float speed_ms = argc > 4 ? (float)std::atof(argv[4]) : 300.0f;
    const float blend    = argc > 5 ? (float)std::atof(argv[5]) : 0.7f;
    const float vol      = argc > 6 ? (float)std::atof(argv[6]) : 1.0f;
    const bool  smooth   = argc > 7 && std::string(argv[7]) == "smooth";

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
    std::printf("params: atoms=%d speed=%.1fms blend=%.2f vol=%.2f mode=%s\n",
                atoms, speed_ms, blend, vol, smooth ? "smooth" : "sharp");

    // ---- process in blocks (mirrors the firmware block loop) ----
    AtomicClusterCore core;
    core.init((float)sample_rate);
    core.set_atoms(atoms);
    core.set_speed_ms(speed_ms);
    core.set_blend(blend);
    core.set_vol(vol);
    core.set_mode(smooth ? AtomicClusterCore::Mode::kSmooth
                         : AtomicClusterCore::Mode::kSharp);

    std::vector<float> wet((size_t)frame_count);
    const size_t       block = 48; // same as the Funbox AudioCallback block size
    for(size_t i = 0; i < mono.size(); i += block)
    {
        const size_t n = std::min(block, mono.size() - i);
        core.process_block(&mono[i], &wet[i], n);
    }

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

    std::printf("wrote: %s  (peaks in pool last frame: %zu)\n",
                out_path, core.debug_pool_count());
    std::printf("max peaks detected over file: %zu  (oscillator cap = %zu)\n",
                core.debug_max_pool_count(), (size_t)AtomicClusterCore::kMaxTracks);
    return 0;
}
