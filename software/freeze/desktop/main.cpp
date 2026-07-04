// Desktop offline test harness for the Freeze core.
//
// Reads a WAV, runs the exact pedal DSP (freeze_core.h) while simulating a
// footswitch press at [on_s] and release at [off_s], and writes a WAV -- so you
// can A/B the drone against EHX Freeze demos before touching the firmware.
// See plans/freeze.md.
//
// Build:
//   make
// Run:
//   ./freeze in.wav out.wav [on_s] [off_s] [fast|slow|latch] [vol] [dry] [jitter]
//
//   on_s   time (seconds) the freeze footswitch is pressed   (default 1.0)
//   off_s  time (seconds) it is released                     (default 3.0)
//          FAST/SLOW: drone sounds from on_s..off_s (+ release tail in SLOW).
//          LATCH: press at on_s captures and holds; off_s issues clear() (kill).
//   vol    frozen (wet) level    (default 1.0)
//   dry    live (dry) level      (default 1.0; set 0 to audition the wet alone)
//   jitter phase movement 0..1   (default 0.0)

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "freeze_core.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if(argc < 3)
    {
        std::fprintf(stderr,
                     "usage: %s in.wav out.wav [fast|slow|latch] "
                     "[vol=1.0] [dry=1.0] [jitter=0.0] "
                     "[hold_s=3.0] [gap_s=0.5] [start_s=1.0]\n"
                     "\n"
                     "  Re-captures the freeze repeatedly across the whole file:\n"
                     "  hold for hold_s, release for gap_s, repeat from start_s.\n"
                     "  So a chord progression gets a fresh snapshot every cycle.\n",
                     argv[0]);
        return 1;
    }

    const char* in_path  = argv[1];
    const char* out_path = argv[2];

    const std::string mode_str = argc > 3 ? argv[3] : "fast";
    const float vol     = argc > 4 ? (float)std::atof(argv[4]) : 1.0f;
    const float dry     = argc > 5 ? (float)std::atof(argv[5]) : 1.0f;
    const float jitter  = argc > 6 ? (float)std::atof(argv[6]) : 0.0f;
    const float hold_s  = argc > 7 ? (float)std::atof(argv[7]) : 3.0f;
    const float gap_s   = argc > 8 ? (float)std::atof(argv[8]) : 0.5f;
    const float start_s = argc > 9 ? (float)std::atof(argv[9]) : 1.0f;

    FreezeCore::Mode mode = FreezeCore::Mode::kFast;
    if(mode_str == "slow")  mode = FreezeCore::Mode::kSlow;
    if(mode_str == "latch") mode = FreezeCore::Mode::kLatch;
    const bool latch = (mode == FreezeCore::Mode::kLatch);

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

    const double dur = (double)frame_count / sample_rate;
    std::printf("in: %s  %u Hz  %u ch  %llu frames  %.1fs\n", in_path, sample_rate,
                channels, (unsigned long long)frame_count, dur);
    std::printf("params: mode=%s vol=%.2f dry=%.2f jitter=%.2f "
                "hold=%.2fs gap=%.2fs start=%.2fs\n",
                mode_str.c_str(), vol, dry, jitter, hold_s, gap_s, start_s);

    // ---- build a repeating press/release schedule across the whole file ----
    // Each cycle: press (capture) at `on`, release/clear at `off = on + hold`,
    // then wait `gap` and capture again -- so every chord in a progression gets
    // its own snapshot. A tiny gap forces a fresh capture each cycle.
    struct Event { size_t frame; bool press; };
    std::vector<Event> events;
    int captures = 0;
    for(double t = start_s; t < dur; t += (double)hold_s + gap_s)
    {
        const double off = std::min(t + hold_s, dur);
        events.push_back({(size_t)(t * sample_rate), true});
        events.push_back({(size_t)(off * sample_rate), false});
        captures++;
    }
    std::printf("schedule: %d captures across the file\n", captures);

    // ---- process ----
    FreezeCore core;
    core.init((float)sample_rate);
    core.set_mode(mode);
    core.set_volume(vol);
    core.set_dry(dry);
    core.set_jitter(jitter);

    std::vector<float> wet(mono.size());
    const size_t       block = 48; // same as the Funbox AudioCallback block size
    size_t             ev    = 0;
    for(size_t i = 0; i < mono.size(); i += block)
    {
        // fire any scheduled footswitch events in this block (~1 ms resolution)
        while(ev < events.size() && events[ev].frame < i + block)
        {
            if(events[ev].press)
            {
                // LATCH sustains hands-free through the gap, so on the next press
                // reset first to swap in the new chord cleanly (not layer it).
                if(latch) core.reset();
                core.set_freeze(true);
            }
            else if(!latch)
            {
                core.set_freeze(false); // FAST/SLOW: release; LATCH keeps holding
            }
            ev++;
        }

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

    std::printf("wrote: %s\n", out_path);
    return 0;
}
