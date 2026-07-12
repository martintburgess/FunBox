# Funbox v3.2 — Bench Bring-Up & Test Procedure

A staged power-on and test guide, derived from `funbox_v3.kicad_pcb` /
`Power.kicad_sch`. The idea is to prove each stage before adding the next, so a
wiring fault can never take out an expensive part. **Do the stages in order.**

> **This board's regulator note:** U2 is populated with an **L78L05** (100 mA,
> TO-92), not the L7805 the silkscreen shows. Its pinout is reversed vs the
> L7805 — see [Regulator orientation](#regulator-orientation-reminder). The
> board pad functions are unchanged.

---

## 0. Tools & supplies

- Multimeter with **continuity/beep** and **DC volts** modes.
- 9V **center-negative** pedal supply. A **current-limited bench supply** (set
  ~9V, ~100 mA limit) is strongly preferred for first power-on.
- A 100 Ω resistor (for safe first power-up in-line, optional but nice).
- Guitar or a signal source, and an amp/headphones, for the audio test.
- USB cable for flashing the Daisy Seed.

**Meter conventions used below:** black probe on **GND** for all voltage
readings. "Beep" = continuity mode.

---

## 1. Reference — key nets, pads & expected voltages

Off-board jacks/power wire to the single-pad connection points on the board
(silkscreen labels in **bold**).

| Point | Silkscreen | Net | Expected V (powered, chips in) |
|-------|-----------|-----|-------------------------------|
| Raw DC in (before protection diode) | **9v** | `Net-(9v1-Pin_1)` | ~9.0 V |
| After protection diode D1 (cathode/band) | — | `VCC` | ~8.6 V |
| Regulator **input** pad (square pad of U2) | — | `VCC` | ~8.6 V |
| Regulator **output** pad of U2 | — | `5V` | **~5.0 V** |
| Ground | **Gnd / InG / OutG** | `GND` | 0 V |
| Daisy 3.3 V rail (Daisy's own `3V3` pin) | — | (Daisy internal) | ~3.3 V |

**Audio wiring pads:** `InL` `InR` `InG` (input jack tip-L / tip-R / sleeve),
`OutL` `OutR` `OutG` (output jack tip-L / tip-R / sleeve). For a **stereo**
audio jack: **Tip = Left, Ring = Right, Sleeve = ground**.

### Power/ground pins by chip (for probing)

| Chip | Ref | Package | V+ pin | GND pin |
|------|-----|---------|--------|---------|
| Quad op-amp (MCP6024) | **U1** | 14-pin DIP | **pin 4 → 5V** | **pin 11 → GND** |
| Dual op-amp (MCP6002) | **U3** | 8-pin DIP | **pin 8 → 5V** | **pin 4 → GND** |
| MIDI optocoupler (H11L1) | **U5** | 6-pin DIP | (via resistor) | **pin 5 → GND** |
| Daisy Seed | **A1** | module | **VIN → VCC (~8.6 V)** | **DGND → GND** |

> All three op-amps run on the **5 V** rail. The **Daisy Seed runs on ~9 V via
> its VIN pin** and makes its own 3.3 V internally — it is *not* powered from
> the 5 V regulator.

DIP pin numbering: with the notch/dot at the top, **pin 1 is top-left**, then
counts **down the left side and up the right**.

---

## 2. Stage 1 — Unpowered short check (NO chips, NO Daisy)

Power disconnected. Meter in continuity/beep.

1. **5V rail → GND:** probe the U2 output pad to a ground point. Should **not**
   hard-beep (you'll see resistance rise as caps charge; a solid 0 Ω = short).
2. **VCC → GND:** probe D1's banded end (or U2 input/square pad) to GND. Same —
   no hard short.
3. **3.3 V area / Daisy VIN pad → GND:** no hard short.
4. **Confirm grounds are common:** `Gnd`, `InG`, `OutG`, footswitch square
   pads, jack sleeves, and DC-jack barrel should all beep to each other.

Any dead short here → find and fix the solder bridge before applying power.

---

## 3. Stage 2 — Power rails, bare board (NO chips, NO Daisy)

The 5 V regulator works standalone, so you can validate both rails with nothing
installed. Keeping the Daisy out protects it.

1. Apply 9 V (correct polarity). If using a bench supply, watch current — a bare
   board should draw only a **few mA**. A spike = short → kill power.
2. Measure:
   - **9v pad:** ~9.0 V.
   - **D1 banded end / U2 input (square) pad:** ~8.6 V (9 V minus the Schottky
     drop). Confirms the protection diode is in and conducting.
   - **U2 output pad (5V rail):** **~5.0 V.** ✅ This confirms your **L78L05 is
     oriented correctly.** If you read ~0 V or ~9 V here, or U2 gets hot, the
     regulator is in backwards — **kill power immediately** and re-check
     ([orientation reminder](#regulator-orientation-reminder)).
3. Also confirm 5 V is present at the **op-amp V+ pins** in the empty sockets:
   **U1 pin 4** and **U3 pin 8** should both read ~5.0 V; **U1 pin 11** and
   **U3 pin 4** should read 0 V (GND). This proves the sockets are wired right
   before a chip goes in.
4. Power down.

---

## 4. Stage 3 — Install Daisy Seed, verify 3.3 V + alive

1. With power **off**, seat the Daisy Seed (mind its orientation — match the USB
   connector to the silkscreen outline).
2. Power up. Watch current: idle Daisy draws roughly ~50–100 mA.
3. Checks:
   - Daisy **onboard LED** should light / blink (it ships with a blink demo).
   - **Daisy VIN pin:** ~8.6 V.
   - **Daisy `3V3` pin:** ~3.3 V. ✅ 3.3 V rail is alive (this comes *from* the
     Daisy, which is why it was absent in Stage 2).
4. Power down.

---

## 5. Stage 4 — Install op-amps, verify power pins

1. Power **off**. Insert **U1 (MCP6024)**, **U3 (MCP6002)**, and MIDI opto **U5
   (H11L1)** — watch pin-1 orientation (notch/dot).
2. Power up. Current will rise slightly (op-amp quiescent draw, still small).
3. Verify power pins:
   - **U1:** pin 4 = ~5 V, pin 11 = 0 V.
   - **U3:** pin 8 = ~5 V, pin 4 = 0 V.
4. **Bias sanity check (optional):** on a single-5 V-supply audio buffer, op-amp
   output pins usually sit at roughly **half the rail (~2.5 V DC)**. Reading
   ~2.5 V on the buffer outputs (not 0 V and not stuck at 5 V) is a good sign
   the stage is biased and not railed. Exact value depends on the bias network.
5. Power down.

---

## 6. Stage 5 — Flash firmware

1. Connect the Daisy Seed via USB.
2. Flash a known-good Funbox program (via the Daisy **web programmer** or
   `make program-dfu` from the program folder). Put the Daisy in **DFU/bootloader
   mode** first if required (BOOT + RESET on the Daisy).
3. Confirm the flash completes and the program starts (LED behavior / serial).

---

## 7. Stage 6 — Audio I/O test

Firmware running (a simple passthrough or your target effect).

1. Wire in the input and output jacks if not already (**Tip = L, Ring = R,
   Sleeve = GND** for stereo).
2. Feed a signal into the **input jack** (guitar, or a signal generator).
3. **Trace the path** (scope ideal, or listen at each stage):
   - Signal present at **InL** / **InR** wiring pad.
   - Buffered signal at the corresponding **U1/U3 op-amp** output pins.
   - Processed audio out at **OutL** / **OutR**.
4. Listen at the amp/headphones — clean signal through = audio chain good.
   - No sound but rails all correct → check jack wiring (tip/sleeve swap) and
     that firmware I/O matches the hardware channel.

---

## 8. Stage 7 — Controls

- **Pots (POT1–POT6, 10K):** with a control-reactive firmware loaded, sweep each
  and confirm it changes its parameter. Electrically, the wiper pin should sweep
  smoothly between rail limits as you turn it.
- **Footswitches (FS1, FS2):** momentary SPST — **square pad = GND**, round pad =
  signal to a Daisy GPIO. Test: continuity between the two pads only when
  pressed. In firmware, each press should toggle bypass / its function.
- **DIP switch S1 (4-pos) & toggles SW1–SW3:** flip and confirm the mapped
  firmware behavior (mode/routing changes).
- **LED1 / LED2:** driven by Daisy GPIO — should follow bypass/function state.

## 9. Stage 8 — MIDI / Expression (optional)

- **MIDI in (J6, via opto U5 H11L1):** send MIDI from a controller; confirm the
  pedal responds. U5 pin 5 = GND; the opto isolates the MIDI input.
- **Expression (J7):** TRS expression pedal (needs a 1/4"→1/8" TRS adapter per
  the README). Sweep and confirm the mapped parameter moves.

---

## Regulator orientation reminder

The board footprint is a TO-220 wired for the **L7805** order — **square pad =
INPUT (VCC), middle = GND, far pad = OUTPUT (5V)**. The **L78L05 (TO-92) pinout
is reversed**: Pin 1 = OUT, Pin 2 = GND, Pin 3 = IN.

- Put the L78L05's **IN leg (pin 3)** into the **square pad**, GND (pin 2) in the
  middle, OUT (pin 1) in the far pad.
- To identify pin 3 on the part: hold it as the datasheet **bottom view** shows
  (leg tips toward you, flat edge up) — the **rightmost leg is pin 3 = INPUT**.
- Confirm on the board: continuity from your input leg to **D1's banded end**
  (same `VCC` node, pure trace) should beep.

---

## Troubleshooting quick table

| Symptom | Likely cause |
|---------|--------------|
| No voltage anywhere | DC jack polarity, D1 in backwards, or open at `9v` pad |
| ~9 V at input pad but **0 V / 9 V at 5V rail**, U2 hot | L78L05 reversed (IN/OUT swapped) |
| High current draw on power-up | Solder bridge / short — recheck Stage 1 |
| No 3.3 V | Daisy not seated / not powered / VIN missing |
| Op-amp V+ not 5 V | Bad socket solder joint or 5 V rail not reaching pin |
| Op-amp output stuck at 0 V or 5 V | Chip in backwards, or missing bias/ground |
| No audio, rails OK | Jack tip/sleeve wiring, or firmware I/O mismatch |
| MIDI dead | Opto U5 orientation, or J6 tip/ring wiring |

*Voltages are approximate; a few tenths of a volt either way on the diode drop
and bias points is normal.*
