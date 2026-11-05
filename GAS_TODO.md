# Gas gauge — gas-only to-do

_Updated 2026-10-18. Sources: `src/main.cpp` (`@ dc5fbac`), `how_to_md_files/gas_empty_baseline_2026-10-08.md`, `how_to_md_files/AGENTS.md` gas section._

**Owner statement 2026-10-11: "we know empty n we know ull"** — the two ends are all that matter; the
interior is even divisions between them. Ends: FULL **3228 mV**, EMPTY **18000 mV** (the line here used
to say 19967 — stale; the change header below is correct). Ends verified on the car (100% / 0%).

**Changed 2026-10-11 (owner call): no more skipped numbers.** Owner: *"why would you make
zero value reading to 8... why the fuck skip numbers."* Made:
1. `kGasStockMv` EMPTY anchor **19967 → 18000** (just under the lowest low-tank reading,
   18239 — so every recorded low-tank reading 18239..21332 still reads 0%).
2. Removed the second-to-last-anchor 0% cut in `gasPctFromMv` — the last segment now
   interpolates **8%..0% continuously**, so the scale shows every percent (no 1–7 gap) and
   `Q W N` fires at a real N%. Build verified. **Flashed and verified on the box — owner confirms
   the whole set is flashed and verified (2026-10-15).**

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

> ⚠️ **Superseded 2026-10-11:** EMPTY moved `19967 → 18000` (see the change header at the top of this file). The `19967` values in this section describe the 10-09 commit, not the live table.

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

### [x] 2. Low-fuel warning threshold — CLOSED 2026-10-11 (was "sits inside the dead zone")
- Paths: buzzer `main.cpp:1376` and screen blink `main.cpp:2008`, both `gasPctFromFilt() <= g_cfg.lowFuelPct`.
- **The dead zone this item described no longer exists.** It was written against the pre-10-11 table, where a cut at `c[11]` made the scale jump 8% → 0% and skip 1–7%. That cut was removed and EMPTY moved to 18000, so the last segment now interpolates **8.3% → 0% continuously** and `Q W N` fires at a real N%. At empty, `mv > 18000` reads exactly 0%, so the alarm is stable (no flicker).
- Remaining and unchanged: the **shipped default is `lowFuelPct = 20`**; docs recorded the in-car box set to `6`. That is a preference, not a defect. (Open-sender sanity gate is item 5.)

### [ ] 3. `Q 1/2/3` write the wrong anchors · PARKED · latent, not live
- `main.cpp:3080`: `slot = (d <= 3) ? (GAS_SEG - 2 * d) : d;` → at 13 anchors gives **10, 8, 6**; the correct 1/4/1/2/3/4 tank slots are **9, 6, 3**.
- Fix if ever needed: `slot = (d <= 3) ? (uint8_t)(GAS_SEG - (GAS_SEG / 4) * d) : d;`
- **No action now (2026-10-11):** USB-console-only, nothing calls it, gauge runs on the `Q R` stock table. It only bites if someone types `Q 1/2/3` to manually set a quarter mark. Left as a flagged trap, not fixed.

### [ ] 4. `Q n` cannot reach slots 1, 2, 10, 11, 12 · DESIGN GAP
- (Slot **11 was the 0% cliff** — that cut was **removed 2026-10-11**, so slot 11 is now an ordinary interior anchor.) Only `Q F`, `Q E`, `Q R` write the ends and re-linearise them; `Q0` is rejected outright.
- Leave it unless you want to set the cliff by hand — adding commands is exactly the churn the lockdown exists to stop.

### [ ] 5. Low-fuel buzzer chirps with the sender unplugged · REAL · small
- `main.cpp:1376` has no sanity gate. An open sender rails to the 32000 clamp → 0% → permanent chirp.
- Fix:
  ```cpp
  bool senderOk = s_gasFilt >= 0 && s_gasFilt < 30000;
  bool lowFuel  = senderOk && gasPctFromFilt() <= g_cfg.lowFuelPct;
  ```
- Real empty ≈ 18000 < 30000, so a genuine empty still chirps.

### [ ] 6. `gasCalMv` compiled default is still 9 of 13 · COSMETIC
- `main.cpp:338-340` is the old 9-value list ending 24109 with four zeroes.
- The poison guard (`main.cpp:1531-1533`) rejects it and falls back to `kGasStockMv`, which is correct now — so it is harmless.
- Optional tidy: fill it with the 13 stock values so a fresh / every-reset box is self-consistent without leaning on the guard.

### [x] 7. Docs said EMPTY = 24109 · RECONCILED 2026-10-18
- `AGENTS.md` gas section, `microsquirt_iobox_dash.md`, `gas_empty_baseline_2026-10-08.md` and `INDEX.md` now all carry the **current** `kGasStockMv` (`3228…18000`), the removed cliff, and the degenerate-stored-table explanation.
- `AGENTS.md` has a **CURRENT TABLE** block at the top of its gas section; the older 24109/22369 text is kept but explicitly marked superseded. (This item's own "19967" was the value at the time — the live value is now **18000**.)

---

## 4. Do NOT do (AGENTS lockdown, `:131`, `:153`)

- Don't re-derive the anchors and don't re-take them.
- Don't "fix" the empty end by moving the A3 threshold — that's the high-beam input; its fix is hardware (LM393).
- One change, one verification; flash parked only.
