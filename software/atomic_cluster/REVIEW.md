# Atomic Cluster — code review notes (2026-07-11)

Plain-language notes from a review focused on sound quality/distortion issues,
plus a look at why tap tempo doesn't seem to work.

## Tap tempo: it's not broken — it hasn't been built yet

There is no tap-tempo code anywhere in `atomic_cluster.cpp`. The right-hand
footswitch (FS2) isn't wired up to anything. This isn't a bug you're
triggering — it's just an unfinished part of the project. The project's own
roadmap agrees: `README.md` lists "tap tempo (FS2)" as a not-yet-done item,
and `plans/atomic-cluster.md` describes how it's *supposed* to work (tap
twice, measure the gap between taps, use that as the new speed) but that plan
was never turned into code.

Two smaller, related things:

- The blinking light (LED2) isn't a tap-tempo button indicator — it just
  blinks at whatever rate the SPEED knob is currently set to, like a
  metronome. It's easy to mistake this for "tap tempo isn't registering,"
  but it's a separate thing that is working as designed.
- At the very fastest SPEED knob setting, the blink is on for the *entire*
  interval, so the LED looks solid instead of blinking. Cosmetic, but worth
  fixing at the same time as tap tempo, since a future tap-driven light would
  hit the same issue.

## Sound-quality findings, most important first

### 1. When the sound gets briefly too loud, it's chopped off flat instead of rounded off

The pedal adds together a bunch of individual tones ("atoms"). Usually
that's fine, but occasionally, by chance, several of them line up and add up
to a signal that's louder than the pedal can output. Right now, the code
handles that by just chopping the top and bottom off the waveform flat
(`s > 1.0f ? 1.0f : ...` in `atomic_cluster.cpp`). Flat-topped chopping is
one of the harshest-sounding kinds of distortion there is — it's what you'd
expect from a cheap overload, not a musical effect. It also doesn't happen
gradually; a sample is either fine or it's slammed flat.

**Likely fix:** replace the flat chop with something that rounds off the
peaks smoothly instead of chopping them square (a "soft clip"). Same safety
net, much friendlier sound. This is probably the single most impactful
change if what you're hearing is harshness or grit, especially with the
ATOMS knob turned up or the dry/wet blend turned toward full wet (both make
the "several tones line up at once" problem more likely).

### 2. Turning the effect off and back on can play back a stale echo of old sound

While the pedal is bypassed (effect off), the code completely stops feeding
new audio into its internal analysis — it's not quietly listening in the
background, it just freezes. Everything it "remembers" about the sound
(recent history, and for one of the two engines, a small queue of
already-processed audio waiting to play) stays frozen in whatever state it
was in the moment you switched it off.

So when you switch it back on, before it catches up to the present moment,
it briefly plays back that frozen, stale snapshot — which can sound like a
little glitch, hiccup, or wrong note right as the effect kicks back in,
before it settles into tracking what you're actually playing.

**Likely fix:** keep feeding audio into the engine's internal analysis even
while bypassed, so its "memory" stays current — just don't send its output
to the amp until you switch it back on.

### 3. The volume-compensation math doesn't always match what's actually playing

The ATOMS knob controls how many individual tones can sound at once, and the
pedal turns the overall volume down a bit as ATOMS goes up (more tones
adding together would otherwise mean more overall loudness). The problem:
that volume compensation is based on the ATOMS knob's *setting*, not on how
many tones are *actually* sounding right now. If you're playing something
simple (like a single note) with ATOMS turned way up, the pedal may only
find a couple of real tones to play — but it still turns the volume down as
if all of them were playing, so the effect can end up sounding quieter than
expected. The perceived volume ends up depending on what you're playing, not
just the knob position.

**Likely fix:** base the volume compensation on how many tones are actually
playing at any given moment, not the knob's maximum setting.

(Side note: the other resynthesis engine — the "mask" engine — doesn't have
this problem at all, since it works by keeping/muting parts of your actual
signal rather than generating brand-new tones from scratch.)

### 4. The "mask" engine's frequency cuts are sharp-edged, which can add ringing

The mask engine works by keeping some frequency content from your input and
silencing the rest. Right now those cuts are hard on/off with no smoothing
at the edges — like cutting fabric with a rough blade instead of a smooth
one. Sharp cuts like this tend to add a subtle ringing/metallic quality to
the sound around each kept note.

**Likely fix:** taper the edges of each kept region slightly instead of
cutting it hard on/off. Small change, should soften that ringing without
changing the overall effect.

### Smaller, lower-priority notes

- A minor technical detail in how the mask engine's window function is
  shaped means the volume compensation isn't mathematically perfect at the
  edges. Very likely inaudible in practice, but easy to tidy up.
- The threshold used to decide "is this a real peak in the sound, worth
  tracking" is a fixed number, not adjusted for how loud your guitar/pickup
  runs. A hot pickup and a quiet one could make the effect feel more or less
  "active" even when you're playing at the same relative volume.
- The two resynthesis engines (and true bypass) each have different amounts
  of built-in delay. Switching between them *while playing* (via the panel
  toggle) can cause an audible timing jump. Low priority since it's a panel
  switch, not something you'd flip mid-performance, but it compounds with
  issue #2 above.

## Suggested next step

Of the above, #1 (soft clipping instead of hard chopping) is the highest
value for the least risk/effort. #2 (bypass freezing) is the next most
impactful but touches more of the code. Happy to implement either — just
say the word.
