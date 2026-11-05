# iobox3 firmware review — to-do list

Re-verified against `src/main.cpp` **and** `how_to_md_files/AGENTS.md` (all 918 lines).
Line numbers are from the reviewed revision.

Legend:
- **APPLY** — checked against source, fix is correct.
- **HARDWARE** — proposed fix is wrong or unverified; measure / re-approach first.
- **HYGIENE** — hardening/cleanup, not a live defect.
- **SOLID** — reviewed, no action.
- **CONFLICT** — the drafted fix collides with a standing rule.

Status: **nothing applied.** This file is a proposal. Verify with `git status` /
`grep` before ever describing any of it as done (AGENTS `:17`, `:58`).

---

## 🚨 Read first — what AGENTS.md changes about this list

1. **FW LOCKDOWN (AGENTS `:131`).** One change, one verification; any larger
   change needs the user's explicit go-ahead. → This batch must be **staged and
   flashed in small groups**, not all at once. A batch produces an ambiguous
   outcome; a single change produces a known one (`:673`).
2. **A3 high beam (AGENTS `:830`, `:858-879`).** *"Do NOT just raise the
   threshold … Do not fix in firmware by moving the threshold."* The sanctioned
   fix is an **LM393 comparator** (precedent: the ABS speed input already has one
   on GPIO5). → Item **A** is not "reject the value", it is **do not touch the
   threshold at all; the fix is hardware.**
3. **Real hardware has no undo (AGENTS `:667`); no in-car key presses
   (AGENTS `:40`).** Every flash/verify step in this list is **parked only**.
4. **AGENTS.md's gas section WAS STALE — FIXED 2026-10-18.** It described the
   *3228…24109* 13-anchor table as "on the box". The live table is `kGasStockMv`
   = **`3228…18000`** (EMPTY re-derived `24109 → 19967 → 18000`, last change
   2026-10-11 / commit `7aa9d21`), and the car's **stored table is degenerate**
   (9 of 13, last four zeroed) so the gauge runs the fallback. AGENTS.md now carries
   a **CURRENT TABLE** block; `microsquirt_iobox_dash.md`,
   `gas_empty_baseline_2026-10-08.md` and `INDEX.md` were reconciled too.
   → Items **2/4/7** below now use **18000**, not 19967.

---

## APPLY — car-affecting, do first

### [ ] 1. Gas screen blanks after any P-command (real bug)
- `tftGasAlloc()` allocates the **gas** renderer on `GAS_CS`/`GAS_DC` (2/21) at
  `main.cpp:2206`. `initGasTft()` allocates it on `g_cfg.pin.tftCs`/`tftDc`
  (17/18) at `2090-2091`.
- `main.cpp:2088-2089`: *"The surviving panel is the CS17/DC18 one, and it now
  shows GAS. The panel on GAS_CS2/DC21 is not driven at all."* AGENTS `:137`
  agrees (single panel, CS17/DC18); wrong-claim #21 (`:33`) confirms 2/21 is the
  idle/main pair.
- Boot uses `initGasTft()` (fine); the re-init state machine reaches
  `TFT_STEP_GAS_ALLOC` → `tftGasAlloc()` and moves the renderer to 2/21 → blank.
  Triggered by `P RESET`, `P WIPE`, `P TFT 0|1`, and every pin remap. Bare `P`,
  `P DASH`/`P DIAG` do not.
- **Fix:**
  ```cpp
  static void tftGasAlloc() {
      if (s_gasTft == nullptr) {
          s_gasTft = new Adafruit_GC9A01A(g_cfg.pin.tftCs, g_cfg.pin.tftDc,
                                          GAS_MOSI, GAS_SCLK, -1);
      }
  }
  ```
- Also stale in the same block:
  - `tftIdleAlloc()` comment `2198-2200` describes the old CS/DC layout, and its
    own alloc `2201` uses the configurable pair while `initTft()` `2118` uses
    `GAS_CS`/`GAS_DC` — they disagree. Moot while `IDLE_TFT_ENABLED = false`, but
    if the idle face is ever enabled, align both to `GAS_CS`/`GAS_DC`.
  - `GAS PANEL DISABLED` header `2050` sits directly above
    `GAS_TFT_ENABLED = true` (`2082`).

---

## APPLY — functional bugs (confirmed)

### [ ] 2. `gasCalMv` default has 9 of 13 anchors; stored table is degenerate
- `main.cpp:338-340` (9 values, old 24109-based set; last 4 slots zero).
- The source states the consequence at `273-277`: the box printed *"anchors
  off-scale/degenerate -> mapping STOCK span"* and the gauge ran on
  `kGasStockMv`. So the compiled default is what actually decides the needle.
- **Fix** (copy `kGasStockMv` verbatim, `main.cpp:280`; `sizeof(Cfg)` unchanged →
  existing blob still accepted):
  ```cpp
  uint16_t gasCalMv[GAS_ANCHORS] = {
      3228, 4459, 5690, 6921, 8152, 9383, 10614,
      11845, 13076, 14307, 15538, 16769, 18000
  };
  ```
- **`Q R` is safe here**: `3042` does `memcpy(g_cfg.gasCalMv, kGasStockMv, …)`, so
  it writes the *same* span the gauge already uses through the fallback — it does
  **not** restore the older 24109 table. Run it once to un-degenerate the stored
  table (needed before any `Q n` refinement, which reads neighbours at
  `3100-3109` and would otherwise compare against a zeroed `c[12]`).
- **Do NOT re-record anchors after** — AGENTS `:135`/`:153`. `Q R` restores the
  default; it is not a re-calibration.
- Update the `"12.5% per segment"` comment at `340` → 13 anchors = 12 segments =
  **8.33%** (span 3228…18000, 1231 mV/segment).

### [ ] 3. `Q 1/2/3` write the wrong slots (real bug)
- `main.cpp:3080`: `slot = (d <= 3) ? (GAS_SEG - 2*d) : d`. At `GAS_SEG=12` that
  is **10, 8, 6**, not the quarter points.
- The comment `3063-3067` states the intended slots are **9, 6, 3** (= `GAS_SEG/2`,
  `GAS_SEG/4`, `GAS_SEG/4*3`) but names the formula `GAS_SEG - 2*d`, which does not
  produce them at 13 anchors. The code, not the comment, is wrong.
- **Fix** (agrees with AGENTS `:196` "1/4, 1/2 and 3/4 tank points" and the
  comment's stated slots):
  ```cpp
  slot = (d <= 3) ? (uint8_t)(GAS_SEG - (GAS_SEG / 4) * d) : d;   // 9, 6, 3
  ```
- Also fix the comment's formula and the `"refine with Q 1..7"` hint at `3126`
  (should be `1..9`).

### [ ] 4. `Q n` cannot reach interior slots 1, 2, 10, 11, 12
- `3075-3081` + the digit gate `val[0] >= '1' && <= '9'` (`3058`). After item 3,
  reachable = {3,4,5,6,7,8,9}; `Q F`=0, `Q E`=12. Slots **1, 2, 10, 11, 12** are
  unreachable.
- Slot **11** was `c[GAS_SEG-1]`, the **0% cliff** — **that cut was removed
  2026-10-11** (`7aa9d21`), so slot 11 is now an ordinary interior anchor. Slots
  **1, 2, 10, 11, 12** are still reachable only via `Q F`/`Q E` re-linearisation or
  `Q R`.
- Comment `3069-3074` acknowledges "anything above is unreachable" yet still
  says `8/9/0` cover slots 10/11/12 — `0` is rejected by the digit gate.
- **Decide** the intended mapping (extend the digit range to accept `Q0` and/or map
  `Q10..Q12`), or accept that only `F/E/R` touch the ends. Low priority under
  lockdown — do not add commands without the user's go-ahead.

### [ ] 5. `S <rpm>` is a silent no-op in the shipped config
- `2694-2696` writes `outMode[0]`/`outRpm[0]` = output 1, which is the fan by
  default (`fanOut = 1`, `325`); the output loop skips the fan pin (`1251`).
  `main.cpp:61` says the old shift role is gone, so `S` is vestigial.
- **Fix (top of the `S` arm):**
  ```cpp
  if (g_cfg.fanOut == 1) { Serial.println("O1 is the fan pin - S refused"); break; }
  ```

### [ ] 6. `O<fanOut>` silently ignored
- Same fan-pin skip. **Fix after `n` is computed (`2706`):**
  ```cpp
  if ((n + 1) == g_cfg.fanOut) { Serial.printf("O%u is the fan pin - refused\n", n + 1); break; }
  ```
- Note `O<n>` also `saveCfg()`s on every accepted command even when nothing
  changed (`2806`); harmless, but a reason to prefer an explicit no-op.

---

## APPLY — behaviour

### [ ] 7. Low-fuel buzzer chirps forever with the sender unplugged
- `1376` has no sender-sanity gate. An unplugged sender rails the node
  (`gasSampleMv`, `1490-1499`: open → ~75k reported → clamped to 32000) → 0% →
  continuous chirp. Logged in `ms_iobox_dash_bug_report.md` L2.
- The sender range is **3228…18000** (EMPTY was 24109, then 19967) and *never 0*;
  32000 only ever means "unplugged / railed".
- **Fix in `updateBuzzer()`:**
  ```cpp
  bool senderOk = s_gasFilt >= 0 && s_gasFilt < 30000;
  bool lowFuel  = senderOk && gasPctFromFilt() <= g_cfg.lowFuelPct;
  ```
- A genuine empty (~18000) is < 30000, so the alarm still fires when it should.
- The red blink in `updateGasDisplay` (`2008`) still shows 0% on the bench — out
  of scope if you only want the beeper silenced.

### [ ] 8. LED colour writes flash on every step
- `L` arm `saveCfg()` at `2812` (L 0) and `2828` (colour). Air-reachable key.
- **Fix:** `static uint32_t s_ledSaveAt = 0;` (file scope); replace both
  `saveCfg()` calls with `s_ledSaveAt = millis() + 3000;`, and in `loop()`:
  ```cpp
  if (s_ledSaveAt && (int32_t)(millis() - s_ledSaveAt) >= 0) { s_ledSaveAt = 0; saveCfg(); }
  ```
- Caveat: a power cut within 3 s loses the last colour. Edge: if
  `millis()+3000` wraps to exactly 0 the save is dropped (1-in-2^32).

### [ ] 9. Fan runs 5 s at every power-up — see HARDWARE B (drafted fix is wrong)

---

## APPLY — input parsing

### [ ] 10. `P <pin>` accepts `13abc`
- `main.cpp:3446` uses raw `toInt()` (the `pin < 0 || pin > 39` check at `3452`
  cannot catch a truncated parse that lands in range).
- **Fix:**
  ```cpp
  String pa = val.substring(sp + 1); pa.trim();
  if (!argIsInt(pa)) { Serial.println("pin must be an integer"); break; }
  int pin = pa.toInt();
  ```

### [ ] 11. MAC octets above `0xff` are truncated
- `P DASH`/`P DIAG`: `sscanf("%x", …)` into `unsigned int`, then cast to
  `uint8_t` (`3312-3315`). `1ff` → 511 → `0xFF`.
- **Fix:** after `sscanf`, reject if any `b[i] > 0xFF` before copying.

---

## APPLY — radio / security

### [ ] 12. Bind the dash MAC now
- `P DASH <aa:bb:cc:dd:ee:ff>` (and `P DIAG` if used). `peerAllowed()` accepts
  **any** peer while nothing is bound (`816`), which is the default. Not real
  security, but it blocks casual spoofing and silences the boot hint (`830`).
- AGENTS `:348-352`: the allow-list still keeps `F 0` / `I 0` / `B 0` / `Q W 90`,
  so binding is a stopgap, not a fix. Real fix = encrypted/authenticated ESP-NOW
  (D4 / io-H2).

### [ ] 13. Drop the open AP — TEST WITH THE DASH FIRST
- `espnowInit()` (`991-1006`): `WiFi.mode(WIFI_AP)`,
  `softAP("iobox3-ms", NULL, ESP_NOW_CHANNEL)` (open AP), `peer.encrypt = false`,
  `peer.ifidx = WIFI_IF_AP`.
- **Draft** (matches AGENTS `:281-282`):
  ```cpp
  #include <esp_wifi.h>
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  // and peer.ifidx = WIFI_IF_STA;
  ```
- If the dash only talks to the iobox's AP interface the link breaks — keep the
  old code until tested. AGENTS `:282` also notes `esp_netif_init()` /
  `esp_event_loop_create_default()` ordering. Do **not** combine this with any
  other change in the same flash.

---

## HYGIENE

### [ ] 14. `strcpy` into fixed `s_last*` buffers
- `drawValue` (`1402`), `drawGasValue` (`1788`), `drawGasText` (`1799`).
- **Not a live bug**: the widest string reaching an 8-byte buffer today is ≤5
  chars; `"~4950 mi"` (9) goes into the 12-byte `s_lastGasMv`. Hardening only —
  add a size parameter and use `strlcpy(last, s, lastSz)`.

### [ ] 15. Stale comments / placeholders
- `fanOut` "defaults to 6" at `410`, `512`, `1229` — it is **1** (`325`,
  MEASURED 2026-10-04).
- `tgtName()` comment `510-512` repeats it.
- `"12.5% per segment"` at `340` (see item 2).
- `I B` described as removed at `308-312` but as "Legal from the air" at
  `2456-2458`; the `I` arm implements no `B`.
- `line 531-532`: *"an UNCONNECTED input reads 0V and will latch in low mode"* —
  contradicts `84-100` (undriven reads **3228**, not 0). Fix the comment (item A).
- `Q 1/2/3` comment `3063-3067` + `"Q 1..7"` hint `3126` (see item 3).
- "5 anchors" wording in gas comments (`1517`, `1569`, …).
- `tftIdleAlloc()` comment `2198-2200` and `GAS PANEL DISABLED` header `2050`
  (see item 1).

### [ ] 16. Speed calibration
- `PULSES_PER_MPH = 10.6f` (`926`) is the placeholder; AGENTS `:576` says set it
  from a measured-distance drive. Optional cheap check: inject a known square
  wave into GPIO5.

---

## HARDWARE — do NOT apply the drafted fix

### [ ] A. `kAnMinVoltsLow = kAnFloorMv/1000` (4.0 V) — REJECTED, and forbidden by AGENTS
- Semantics (`updateAnalogLatch` `597-599`): active-low is a **ceiling** —
  `raw = (mv <= t)`. Raising the *minimum* allowed ceiling to 4.0 V means an
  undriven input (**3228 ≤ 4000**) satisfies it and **latches ON** — the exact
  phantom the floor gate (`84-92`) exists to prevent, on a channel (A3) whose tap
  **floats when the relays open** (AGENTS `:830`, `:872`).
- AGENTS `:830`: *"Do not fix in firmware by moving the threshold."* `:879`:
  *"Do NOT just raise the threshold. The floor gate is already only 26 mV above the
  OFF state — moving the threshold moves where a noisy wire crosses, it does not
  widen the window."*
- AGENTS wrong-claim #24 (`:38`): *"Never use a code constant as ground truth
  about the hardware."* The 3228 figure is the **undriven floor** (`:864`), and
  the **A3** front end is unresolved. (A4's constant mismatch is **not** an open
  item — it is a pure scale factor that cancels, closed by the owner 2026-10-18,
  wrong-claim #33.)
- **Sanctioned fix (hardware):** LM393 comparator on A3, same as the ABS speed
  input on GPIO5 (AGENTS `:875`). Secondary options: 100 kΩ pull-down on the car
  wire (then ON reads ~6,100 → threshold ~3,000; **both or neither**), or PC817
  opto isolation (`:876-877`).
- **The one measurement that decides it (parked):** ground A3 and read its
  reported mV from `?`. `~0` → the divides work and low mode can latch; no code
  change. `~3228` → the divider cannot pull to ground and the input needs the
  comparator, not a threshold.
- **Comment fix at `531` is separate and correct** — an unconnected input reads
  ~3228, not 0.

### [ ] B. Fan 5 s run-on at boot — drafted fix does NOT work
- `withinRunOn` is computed **outside** the inner `if` (`1109`). Presetting
  `s_fanHeldAfterLinkLoss = true` skips `s_fanOffAtMs = millis()` (`1105`),
  leaving `s_fanOffAtMs = 0`, so `(millis() - 0) < 5000` is **still true** → same
  5 s. The one-liner is a no-op for the symptom.
- **Fix:** seed the timestamp at boot, e.g. `s_fanOffAtMs = millis() - FAN_RUNON_MS;`
  (or add a "link has been fresh at least once" flag and arm the run-on only after
  a real fresh→lost transition). Verify the real link-loss hold
  (`holdWhileRunning`, `1145`/`1158`) still engages afterwards.
- Tradeoff (as you noted): a car that boots hot with a dead dash then won't hold
  the fan. `cltHotForFail` (`1156`) and the ignition-switched design (`1125-1130`)
  are the safety backstops.

---

## SOLID — reviewed, no action

- [x] `pinOk` / `pinAliasFree` / `pinInUseAnywhere` / `applyPinConfig`
  (`3640-3862`): allow-set exact; iac/buzz/tft checks fail closed via `assigning`;
  old-pad release present.
- [x] Watchdog, failsafe, `argIsInt`/`argIsFloat` validation on `F`/`I`/`T`/`Q`
  and the `O` modes (`0|1|A|T|R` refused otherwise).
- [x] `O<n>` mode guard set is closed (no silent always-on).
- [x] ESP-NOW allow-list (`cmdAllowedFromLink`, `2452+`) is a prefix allow-list,
  so `P WIPE` is no longer link-reachable (was AGENTS `:332`). Keep it that way.

---

## Not yet reviewed
- [ ] TFT drawing internals beyond alloc/re-init (`updateDisplay`,
      `updateGasDisplay`, `drawGasFrame`) — skimmed, not fully audited.
- [ ] Remaining arms: `A` (full), `T`, `W`, `X`, `B`, gas-history code.

---

## Flash + verify sequence (PARKED ONLY — AGENTS `:40`)
1. Apply one group only: **#1** first (the only car-affecting defect).
2. `pio run` build; flash on **whichever `/dev/ttyUSB*` enumerates**, confirm the
   banner `MS2/Extra I/O box v3 (iobox3) ready` before flashing (AGENTS `:214`).
3. `P TFT 1` → confirm the gas screen survives. That alone verifies #1.
4. Then the next group (#2–#6) — but run `Q R` **once** and `Q` to confirm 13
   anchors; do **not** re-record anchors.
5. Leave radio (#12/#13) for its own flash so a link break is unambiguous.
6. ~~Update the stale AGENTS.md gas section~~ — **DONE 2026-10-18.** It now carries the
   `3228…18000` table, the removed cliff and the degenerate-stored-table reality (a **CURRENT
   TABLE** block at the top of the gas section). `microsquirt_iobox_dash.md`,
   `gas_empty_baseline_2026-10-08.md` and `INDEX.md` were reconciled in the same pass.
