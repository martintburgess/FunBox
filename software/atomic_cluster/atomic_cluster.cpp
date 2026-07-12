#include "daisy_petal.h"
#include "daisysp.h"
#include "funbox.h"

#include "atomic_cluster_core.h"
#include "atomic_cluster_mask_core.h"

using namespace daisy;
using namespace daisysp;
using namespace funbox; // This is important for mapping the correct controls to the Daisy Seed on Funbox PCB

// Declare a local daisy_petal for hardware access
DaisyPetal hw;

AtomicClusterCore     sine_;
AtomicClusterMaskCore mask_;

Parameter pAtoms, pSpeed, pBlend;

float samplerate = 48000;

bool bypass;
bool useMask;

bool pswitch1[2], pswitch2[2], pswitch3[2], pdip[4];
int  switch1[2], switch2[2], switch3[2], dip[4];

Led led1, led2;

// LED2 flashes once per SPEED interval (metronome) so the current rate is visible.
size_t tempo_counter_   = 0;
size_t flash_remaining_ = 0;

// Tap tempo (FS2): average the last few tap intervals, reset on a long gap. A
// tap overrides the SPEED knob until the knob is physically moved again.
uint32_t       last_tap_ms_     = 0;
float          tap_intervals_[4];
int            tap_count_       = 0;   // valid intervals currently averaged
float          tapped_speed_ms_ = 300.0f;
bool           tap_active_      = false;
float          last_knob_speed_ = -1.0f;
const uint32_t kTapTimeoutMs    = 2000; // gap beyond this starts a fresh average

// Wet buffer for whichever engine is active this block. Block size is 256 (see
// main): the per-hop 2048-pt FFT is too heavy a spike to finish within a
// 48-sample (~1 ms) callback deadline -- that underrun was the "distortion"
// heard in both engines. A 256 block gives the FFT ~5.3 ms of budget. wet_buf
// MUST be >= the block size or process_block overruns it.
float wet_buf[256];

void updateSwitch1() // MODE: left/center = SHARP, right = SMOOTH
{
    AtomicClusterCore::Mode     sineMode;
    AtomicClusterMaskCore::Mode maskMode;
    if(pswitch1[1]) // right
    {
        sineMode = AtomicClusterCore::Mode::kSmooth;
        maskMode = AtomicClusterMaskCore::Mode::kSmooth;
    }
    else // left or center
    {
        sineMode = AtomicClusterCore::Mode::kSharp;
        maskMode = AtomicClusterMaskCore::Mode::kSharp;
    }
    sine_.set_mode(sineMode);
    mask_.set_mode(maskMode);
}

void updateSwitch2() // WEIGHTING: left = loud, center = balanced, right = even
{
    AtomicClusterCore::Weighting     sineW;
    AtomicClusterMaskCore::Weighting maskW;
    if(pswitch2[0]) // left
    {
        sineW = AtomicClusterCore::Weighting::kLoud;
        maskW = AtomicClusterMaskCore::Weighting::kLoud;
    }
    else if(pswitch2[1]) // right
    {
        sineW = AtomicClusterCore::Weighting::kEven;
        maskW = AtomicClusterMaskCore::Weighting::kEven;
    }
    else // center
    {
        sineW = AtomicClusterCore::Weighting::kBalanced;
        maskW = AtomicClusterMaskCore::Weighting::kBalanced;
    }
    sine_.set_weighting(sineW);
    mask_.set_weighting(maskW);
}

void updateSwitch3() // ENGINE: left/center = sine, right = mask
{
    useMask = pswitch3[1]; // right
}

void UpdateButtons()
{
    // (De-)Activate bypass and toggle LED1 when left footswitch is let go.
    if(hw.switches[Funbox::FOOTSWITCH_1].FallingEdge())
    {
        bypass = !bypass;
        led1.Set(bypass ? 0.0f : 1.0f);
    }

    led1.Update();
    led2.Update();
}

// Tap tempo on FS2: registers on press (RisingEdge). Averages up to the last 4
// intervals; a gap longer than kTapTimeoutMs discards the history so an old,
// out-of-context tap can't skew the tempo.
void UpdateTapTempo()
{
    if(!hw.switches[Funbox::FOOTSWITCH_2].RisingEdge())
        return;

    const uint32_t now = System::GetNow();
    if(last_tap_ms_ != 0)
    {
        const uint32_t gap = now - last_tap_ms_;
        if(gap > kTapTimeoutMs)
        {
            tap_count_ = 0; // stale -> start a fresh average with this tap
        }
        else if(gap >= 20) // ignore contact bounce / absurdly fast double-hits
        {
            if(tap_count_ < 4)
            {
                tap_intervals_[tap_count_++] = (float)gap;
            }
            else // shift the 4-slot window and append
            {
                for(int i = 1; i < 4; i++)
                    tap_intervals_[i - 1] = tap_intervals_[i];
                tap_intervals_[3] = (float)gap;
            }

            float sum = 0.0f;
            for(int i = 0; i < tap_count_; i++)
                sum += tap_intervals_[i];
            tapped_speed_ms_ = sum / (float)tap_count_;

            if(tapped_speed_ms_ < 20.0f)   tapped_speed_ms_ = 20.0f;
            if(tapped_speed_ms_ > 1000.0f) tapped_speed_ms_ = 1000.0f;
            tap_active_ = true;
        }
    }
    last_tap_ms_ = now;
}

void UpdateSwitches()
{
    // 3-way Switch 1 (MODE)
    bool changed1 = false;
    for(int i = 0; i < 2; i++)
    {
        if(hw.switches[switch1[i]].Pressed() != pswitch1[i])
        {
            pswitch1[i] = hw.switches[switch1[i]].Pressed();
            changed1    = true;
        }
    }
    if(changed1)
        updateSwitch1();

    // 3-way Switch 2 (WEIGHTING)
    bool changed2 = false;
    for(int i = 0; i < 2; i++)
    {
        if(hw.switches[switch2[i]].Pressed() != pswitch2[i])
        {
            pswitch2[i] = hw.switches[switch2[i]].Pressed();
            changed2    = true;
        }
    }
    if(changed2)
        updateSwitch2();

    // 3-way Switch 3 (ENGINE)
    bool changed3 = false;
    for(int i = 0; i < 2; i++)
    {
        if(hw.switches[switch3[i]].Pressed() != pswitch3[i])
        {
            pswitch3[i] = hw.switches[switch3[i]].Pressed();
            changed3    = true;
        }
    }
    if(changed3)
        updateSwitch3();

    // Dip switches (unused)
    for(int i = 0; i < 4; i++)
    {
        if(hw.switches[dip[i]].Pressed() != pdip[i])
            pdip[i] = hw.switches[dip[i]].Pressed();
    }
}

// LED2: flash once per SPEED interval, metronome-style, so the current rate is
// visible. Block-quantized (the flash lasts at least one block).
void UpdateTempoLed(float speed_ms, size_t block_size)
{
    size_t period_samples = (size_t)(speed_ms * 0.001f * samplerate);
    if(period_samples < 1)
        period_samples = 1;

    // Short fixed pulse, but never more than half the period -- so even the
    // fastest SPEED still blinks instead of sitting solid-on.
    size_t flash_len = (size_t)(0.020f * samplerate); // ~20 ms
    if(flash_len > period_samples / 2)
        flash_len = period_samples / 2;
    if(flash_len < 1)
        flash_len = 1;

    tempo_counter_ += block_size;
    if(tempo_counter_ >= period_samples)
    {
        tempo_counter_   = 0;
        flash_remaining_ = flash_len;
    }

    if(flash_remaining_ > 0)
    {
        flash_remaining_ = (flash_remaining_ > block_size) ? (flash_remaining_ - block_size) : 0;
        led2.Set(1.0f);
    }
    else
    {
        led2.Set(0.0f);
    }
}

// This runs at a fixed rate, to prepare audio samples
static void AudioCallback(AudioHandle::InputBuffer  in,
                           AudioHandle::OutputBuffer out,
                           size_t                    size)
{
    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();

    UpdateButtons();
    UpdateTapTempo();
    UpdateSwitches();

    int   vAtoms = (int)pAtoms.Process();
    float vSpeed = pSpeed.Process();
    float vBlend = pBlend.Process();

    // SPEED source: a tap overrides the knob until the knob is moved > ~30 ms
    // (well above ADC jitter on the ~980 ms span), which hands control back.
    if(last_knob_speed_ < 0.0f)
        last_knob_speed_ = vSpeed;
    if(fabsf(vSpeed - last_knob_speed_) > 30.0f)
    {
        tap_active_      = false;
        last_knob_speed_ = vSpeed;
    }
    const float speed_ms = tap_active_ ? tapped_speed_ms_ : vSpeed;

    sine_.set_atoms(vAtoms);
    sine_.set_speed_ms(speed_ms);
    sine_.set_blend(vBlend);

    mask_.set_atoms(vAtoms);
    mask_.set_speed_ms(speed_ms);
    mask_.set_blend(vBlend);

    UpdateTempoLed(speed_ms, size); // LED2 = SPEED metronome (reflects tapped rate)

    if(bypass)
    {
        for(size_t i = 0; i < size; i++)
        {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
    }
    else
    {
        if(useMask)
            mask_.process_block(in[0], wet_buf, size);
        else
            sine_.process_block(in[0], wet_buf, size);

        for(size_t i = 0; i < size; i++)
        {
            float s = wet_buf[i];
            s = (s > 1.0f) ? 1.0f : ((s < -1.0f) ? -1.0f : s); // guard transient overs
            out[0][i] = s;
            out[1][i] = s;
        }
    }
}

int main(void)
{
    hw.Init();
    samplerate = hw.AudioSampleRate();

    // 256, not 48: the periodic 2048-pt FFT can't meet a ~1 ms (48-sample)
    // deadline and underruns -> the distortion we chased. 48 kHz / 256 was
    // verified clean by A/B-ing block sizes. Matches Venus's approach.
    hw.SetAudioBlockSize(256);

    switch1[0] = Funbox::SWITCH_1_LEFT;
    switch1[1] = Funbox::SWITCH_1_RIGHT;
    switch2[0] = Funbox::SWITCH_2_LEFT;
    switch2[1] = Funbox::SWITCH_2_RIGHT;
    switch3[0] = Funbox::SWITCH_3_LEFT;
    switch3[1] = Funbox::SWITCH_3_RIGHT;
    dip[0]     = Funbox::SWITCH_DIP_1;
    dip[1]     = Funbox::SWITCH_DIP_2;
    dip[2]     = Funbox::SWITCH_DIP_3;
    dip[3]     = Funbox::SWITCH_DIP_4;

    pswitch1[0] = false;
    pswitch1[1] = false;
    pswitch2[0] = false;
    pswitch2[1] = false;
    pswitch3[0] = false;
    pswitch3[1] = false;
    pdip[0]     = false;
    pdip[1]     = false;
    pdip[2]     = false;
    pdip[3]     = false;

    // CUBE + 32 ceiling: the effect is only distinct at low atom counts, so a
    // linear 1..64 sweep wasted ~75% of the travel. Cube law puts most of the
    // knob in the expressive 1..12 range (knob center ~= 5 atoms).
    pAtoms.Init(hw.knob[Funbox::KNOB_1], 1.0f, 32.0f, Parameter::CUBE);
    pSpeed.Init(hw.knob[Funbox::KNOB_2], 1000.0f, 20.0f, Parameter::LINEAR);
    pBlend.Init(hw.knob[Funbox::KNOB_3], 0.0f, 1.0f, Parameter::LINEAR);
    // Knob 4 unused -- no post-gain, matching Venus (dry/wet blend only, no volume stage).

    sine_.init(samplerate);
    mask_.init(samplerate);
    useMask = false;

    // Init the LEDs and activate bypass
    led1.Init(hw.seed.GetPin(Funbox::LED_1), false);
    led1.Update();
    bypass = true;

    led2.Init(hw.seed.GetPin(Funbox::LED_2), false);
    led2.Update();

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    while(1)
    {
        System::Delay(10);
    }
}
