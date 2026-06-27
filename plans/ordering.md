# Funbox v3.2 — Parts Ordering Plan

## Context

Building **2–3 Funbox v3.2 units**, supplier-mapped from
`hardware/funbox_v3_midi_exp/funbox_v3_BOM.csv`. Tayda for generic parts + drilled
enclosure, DigiKey for specialty ICs + the 1µF coupling caps + Daisy headers, Electrosmith
for the Daisy Seed, Pedal Parts Australia (PPA) for the remaining guitar hardware,
PCBway/JLCPCB for the board.

Quantities target **3 builds** (per-unit → ×3); 2 enclosures ordered, cheap passives bought as spares.

---

## ✅ Ordered so far

### Tayda (cart #55205, $48.00)
Generic passives, semis, sockets, pots, toggles, 2× enclosure + 2× drill. Substitutions:
PCB-pin toggles (fit confirmed), X7R 100nF (decoupling — fine), 2 enclosures+drills (2 boxed builds).

<details><summary>Tayda items</summary>

| Item | Qty |
|---|---|
| 2M resistor (A-2609) | 10 |
| 2.2nF WIMA FKS2 (A-935) | 5 |
| 100nF X7R MLCC (A-5459) | 10 |
| 100pF MLCC (A-1359) | 10 |
| 1nF Kemet film (A-1987) | 10 |
| 1N4148 (A-157) | 6 |
| 1N5817 (A-159) | 5 |
| L78L05ACZ (A-176) | 6 |
| H11L1M (A-3697) | 5 |
| 16mm RA pot B10K (A-2969) | 18 |
| SPDT On-Off-On toggle, PCB pins (A-7418) | 9 |
| PJ-320A jack (A-4863) | 10 |
| IC socket 14/8/6-pin | 4 each |
| 125B enclosure (white-sand + plain) | 1 + 1 |
| 125B custom drill service | 2 |

</details>

### DigiKey
| Item | Qty | Note |
|---|---|---|
| MCP6024-I/P (U1) | 3 | quad op amp |
| MCP6002-I/P (U3) | 3 | dual op amp |
| DS03-254-04BE dip switch (S1) | 3 | Wurth |
| **B32529C0105J — 1µF 63V box film** (C1,C4,C5,C14,C15,C17) | 18 | ✅ the coupling caps — 5mm pitch, 4.5mm thick, 9.5mm tall (test-fit under Daisy) |
| WR-PHD 2.54mm 20-pos socket header (A1) | 6 | ✅ Daisy Seed sockets (2 per build) |
| 12B 1/4" stereo jack, panel mount | 2 | In/Out — enough for ~1 build (+ ones you already have) |
| IA-SS3563 audio adapter, 3.5mm | 2 | expression-input adapter |

### Electrosmith
| Item | Note |
|---|---|
| Daisy Seed (A1) | ✅ ordered — confirm **Soldered + 65 MB** |

---

## 🛒 STILL TO BUY

### Pedal Parts Australia (local guitar hardware)
| Part | Designator | Qty (×3) | Note |
|---|---|---|---|
| SPST **momentary, normally-open** footswitch | FS1, FS2 | 6 | |
| Knobs (fit **6.35 mm round** shaft) | POT1–6 | 18 | |
| **5 mm LEDs** | LED1, LED2 | 6 | not on Tayda order |
| 5 mm LED bezels | LED1, LED2 | 6 | size to drill template |
| Pot covers (recommended) | POT1–6 | 18 | prevents shorting on PCB back |
| 1/4" **stereo (TRS)** jacks | In + Out | top up to 6 | 2 ordered (DigiKey) + ones you have |
| DC power jack 2.1mm | 9v/Gnd | top up to 3 | *have some* |

### PCB — PCBway or JLCPCB
- Gerber zip: `hardware/funbox_v3_midi_exp/GerberSeed_PCBway_Funbox_v3p2.zip`
- Board **59.7 × 67.6 mm**, Standard Prototype. Recommended: **ENIG** + **black** mask. Min qty 5.

### Verify-then-buy (likely already have)
- **100 µF electrolytic (C6)** — ≤8 mm dia, ~3.5 mm lead pitch, ≥16 V. Buy if not on hand.
- **100 nF box film (C7–C10)** — your own, else the Tayda X7R MLCC covers them.

### Optional / consumables
- Hookup wire for off-board jacks + power (PCB pads: OutR/OutL/OutG, InL/InR/InG, 9v, Gnd).
- 3rd 125B enclosure + drill service (for a 3rd boxed unit).
- 1× more 2.2 nF (to make 6 for a 3rd build).

---

## Key watch-outs
1. **1 µF** coupling caps: test-fit the Daisy over them before final solder (9.5 mm is close to the header gap — lean a cap if it fouls).
2. Daisy Seed: **Soldered + 65 MB**.
3. **Socket all ICs** (done) + **2 Daisy headers** (done).
4. PJ-320A jacks may need locating bumps **filed off**.
5. Footswitches **momentary, NO**; toggles **On-Off-On SPDT** (PCB-pin variant confirmed OK).

## Verification
1. **Electrosmith:** Daisy is Soldered + 65 MB.
2. **PPA:** momentary-NO footswitches; 5mm LEDs + bezels matching drill hole; 6.35mm knobs; enough 1/4" jacks + power jacks for your build count.
3. **Caps on hand:** 100µF dia/pitch/voltage OK; 100nF box-film present (or rely on Tayda X7R).
4. **PCB:** dims 59.7×67.6 mm, uploaded the v3p2 Gerber zip.
5. **Test-fit** the Daisy Seed over the populated board before soldering it down.
</content>
