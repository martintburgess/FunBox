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

// Tempo LED (LED2) — flashes at the current SPEED rate, metronome-style.
size_t tempo_counter_   = 0;
size_t flash_remaining_ = 0;
const size_t kFlashSamples = 48 * 20; // ~20 ms at 48kHz-ish (block-quantized below)

// Wet buffer for whichever engine is active this block (block size set to 48 in main()).
float wet_buf[48];

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

void updateSwitch2() // ENGINE: left/center = sine, right = mask
{
    useMask = pswitch2[1]; // right
}

void updateSwitch3()
{
    // unused
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

    // 3-way Switch 2 (ENGINE)
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

    // 3-way Switch 3 (unused)
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

// LED2: flashes once per SPEED interval, ~20ms pulse, metronome-style.
void UpdateTempoLed(float speed_ms, size_t block_size)
{
    size_t period_samples = (size_t)(speed_ms * 0.001f * samplerate);
    if(period_samples < 1)
        period_samples = 1;

    tempo_counter_ += block_size;
    if(tempo_counter_ >= period_samples)
    {
        tempo_counter_   = 0;
        flash_remaining_ = kFlashSamples;
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
    UpdateSwitches();

    int   vAtoms = (int)pAtoms.Process();
    float vSpeed = pSpeed.Process();
    float vBlend = pBlend.Process();

    sine_.set_atoms(vAtoms);
    sine_.set_speed_ms(vSpeed);
    sine_.set_blend(vBlend);

    mask_.set_atoms(vAtoms);
    mask_.set_speed_ms(vSpeed);
    mask_.set_blend(vBlend);

    UpdateTempoLed(vSpeed, size);

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
            float s   = wet_buf[i];
            s         = (s > 1.0f) ? 1.0f : ((s < -1.0f) ? -1.0f : s); // guard against
                                                                        // transient overs from
                                                                        // summed partials
            out[0][i] = s;
            out[1][i] = s;
        }
    }
}

int main(void)
{
    hw.Init();
    samplerate = hw.AudioSampleRate();

    hw.SetAudioBlockSize(48);

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

    pAtoms.Init(hw.knob[Funbox::KNOB_1], 1.0f, 64.0f, Parameter::LINEAR);
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
