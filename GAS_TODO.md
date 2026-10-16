# Gas gauge — gas-only to-do

_Updated 2026-10-11. Sources: `src/main.cpp`, `how_to_md_files/gas_empty_baseline_2026-10-08.md`, `how_to_md_files/AGENTS.md` gas section._

**Owner statement 2026-10-11: "we know empty n we know ull"** — the two measured ends
(FULL **3228 mV**, EMPTY **19967 mV**) are all that matter; the interior is even divisions
of that span. Ends verified on the car (100% / 0%).

**Changed 2026-10-11 (owner call): no more skipped numbers.** Owner: *"why would you make
zero value reading to 8... why the fuck skip numbers."* Made:
1. `kGasStockMv` EMPTY anchor **19967 → 18000** (just under the lowest low-tank reading,
   18239 — so every recorded low-tank reading 18239..21332 still reads 0%).
2. Removed the second-to-last-anchor 0% cut in `gasPctFromMv` — the last segment now
   interpolates **8%..0% continuously**, so the scale shows every percent (no 1–7 gap) and
   `Q W N` fires at a real N%. Build verified. **Not yet flashed/verified on the box.**

---

## 1. The empty-tank data you asked about — FOUND (no need to redo)

| where | what |
|---|---|
| `how_to_md_files/gas_empty_baseline_2026-10-08.md` | the write-up |
| `how_to_md_files/gas_log_20261008_214602.txt` | raw console, 88 lines |
| `how_to_md_files/gas_log_20261008_214808.txt` | raw console, 87 lines |
| `how_to_md_files/gas_log_20261008_215009.txt` | raw console, 87 lines |
| `how_to_md_files/gas_sender_readings_2026-10-03.md` | older: FULL 3228, old EMPTY 24109 |

- **3 runs × ~87 lines = 262 samples, EMPTY TANK.**
- A4 **median 19967 mV**, range 18239–21332, stdev ~440. The three runs agree within 62 mV.
- Gauge showed **18–23%** on a tank that was empty.
- Root cause: the box stored only **9 of 13 anchors** (last four zeroed) → the poison guard rejected it → the gauge ran on the compiled `kGasStockMv`, whose EMPTY was the old 24109.

---

## 2. Done and already flashed on 2026-10-09 (commit `65ec2c8`) — do not redo

- `kGasStockMv` re-divided evenly across **3228…19967** (`main.cpp:278-281`). FULL unchanged.
- `Q R`'s print un-hardcoded — no longer says `E=24109` (`main.cpp:3053-3054`).
- Verified on the box: `Q R -> F=3228 E=19967`; `Q -> 13 anchors, no WARN`; `? -> 4464=93%, 5250=88%`.
- Owner accepted 2026-10-09: _"it's perfect how it is."_

**So the "empty reads 18–23%" problem is fixed, and verified on the car (see item 1).** What's left are the small items below.

---

## 3. To-do

### [x] 1. Verify on the car at empty — CLOSED 2026-10-11
- Owner confirmed on the car: the gauge reads **0%** with the tank empty. The last open verification is done.
- The empty-tank problem (was 18–23%) is now fully closed.
- (If it ever drifts back off 0%, the only number to change is `kGasStockMv[GAS_SEG]`, `main.cpp:280` — one value, one reflash. **Do not re-take the anchors.**)

### [ ] 2. Low-fuel warning threshold sits inside the dead zone · MEDIUM
- Paths: buzzer `main.cpp:1367` and screen blink `main.cpp:1999`, both `gasPctFromFilt() <= g_cfg.lowFuelPct`.
- The gauge can only show **0%**, or **8%…100%** — the last segment runs 8.4→16.7% and the cliff at `c[11] = 18572` cuts it to 0. There is **no 1–7% reading**.
- Consequence: `Q W 1` … `Q W 8` all behave the same — the alarm only fires when the needle is exactly 0. A threshold of **6** (docs say the box is set to 6; shipped default is 20) can also **flicker**, because at empty the reading occasionally dips to 8% (below the cliff) and 8 > 6 turns it back off.
- Decide: leave at 6 (fires/flashes only at 0%), or bump `Q W` to **8–9** for a solid, non-flickering alarm at empty.

### [ ] 3. `Q 1/2/3` write the wrong anchors · PARKED · latent, not live
- `main.cpp:3080`: `slot = (d <= 3) ? (GAS_SEG - 2 * d) : d;` → at 13 anchors gives **10, 8, 6**; the correct 1/4/1/2/3/4 tank slots are **9, 6, 3**.
- Fix if ever needed: `slot = (d <= 3) ? (uint8_t)(GAS_SEG - (GAS_SEG / 4) * d) : d;`
- **No action now (2026-10-11):** USB-console-only, nothing calls it, gauge runs on the `Q R` stock table. It only bites if someone types `Q 1/2/3` to manually set a quarter mark. Left as a flagged trap, not fixed.

### [ ] 4. `Q n` cannot reach slots 1, 2, 10, 11, 12 · DESIGN GAP
- Slot **11 is the 0% cliff** (`c[GAS_SEG-1]`). Only `Q F`, `Q E`, `Q R` touch it. `Q0` is rejected outright.
- Leave it unless you want to set the cliff by hand — adding commands is exactly the churn the lockdown exists to stop.

### [ ] 5. Low-fuel buzzer chirps with the sender unplugged · REAL · small
- `main.cpp:1367` has no sanity gate. An open sender rails to the 32000 clamp → 0% → permanent chirp.
- Fix:
  ```cpp
  bool senderOk = s_gasFilt >= 0 && s_gasFilt < 30000;
  bool lowFuel  = senderOk && gasPctFromFilt() <= g_cfg.lowFuelPct;
  ```
- Real empty ≈ 19967 < 30000, so a genuine empty still chirps.

### [ ] 6. `gasCalMv` compiled default is still 9 of 13 · COSMETIC
- `main.cpp:338-340` is the old 9-value list ending 24109 with four zeroes.
- The poison guard (`main.cpp:1531-1533`) rejects it and falls back to `kGasStockMv`, which is correct now — so it is harmless.
- Optional tidy: fill it with the 13 stock values so a fresh / every-reset box is self-consistent without leaning on the guard.

### [ ] 7. Docs still say EMPTY = 24109 · STALE
- `AGENTS.md:151-210` and `microsquirt_iobox_dash.md:93-95` still describe the old 22369/24109 table.
- `main.cpp` (2026-10-08) supersedes it: EMPTY **19967**, stored table degenerate, gauge runs on `kGasStockMv`. Reconcile the docs.

---

## 4. Do NOT do (AGENTS lockdown, `:131`, `:153`)

- Don't re-derive the anchors and don't re-take them.
- Don't "fix" the empty end by moving the A3 threshold — that's the high-beam input; its fix is hardware (LM393).
- One change, one verification; flash parked only.
