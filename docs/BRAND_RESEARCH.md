# CPAP brand support research (2026-07-07)

Scoping doc for prioritizing future native-parser work, beyond what's already shipped/in
progress. Not an implementation spec — see `PHILIPS_FORMAT.md` in `hms-cpapdash-parser-philips`
for that level of rigor once a brand here gets greenlit.

**Status at time of writing:**
- ResMed (EDF) — supported.
- Löwenstein / Weinmann Prisma (WMEF) — supported.
- Philips Respironics (PRS1) — in progress, clean-room, in `hms-cpapdash-parser-philips`. A
  prior Philips parser was removed from the MIT repos and its git history scrubbed (v2026.1.2)
  after it was judged too close to OSCAR's GPLv3 code. Standing rule: never read or copy OSCAR
  (gitlab.com/CrimsonNape/OSCAR-code) source for format facts or logic. Independent
  documentation and independently-licensed third-party code only.

**Ground truth for "what's left":** OSCAR's `oscar/SleepLib/loader_plugins/` directory listing
(filenames only, not contents) is the authoritative list of machine families OSCAR supports.
Beyond ResMed/Löwenstein-Weinmann/Philips, the remaining CPAP-machine-specific loaders are:
Fisher & Paykel (two separate loaders — Icon and SleepStyle), DeVilbiss IntelliPAP, BMC
Medical, and Resvent.

---

## 1. Fisher & Paykel Icon

- **Format:** proprietary (SmartStick dump), not EDF.
- **Independent documentation:** Apnea Board Wiki, "Fisher & Paykel Icon Data Format" —
  community-authored, not OSCAR-derived. Safe clean-room source if pursued.
- **Non-GPL code:** `github.com/jieter/fph-parser` (.FPH files) exists but carries **no license
  file** — defaults to all-rights-reserved under GitHub ToS, cannot be legally reused or forked
  without contacting the author first.
- **SD card:** yes (SmartStick behaves like SD-card storage).
- **Market:** discontinued/legacy line; also had an FDA Class 2 recall. F&P's active product is
  SleepStyle.
- **SleepHQ:** not listed as supported (SleepHQ's F&P support is SleepStyle only).
- **Verdict:** low priority. Declining installed base, no usable third-party code.

## 2. Fisher & Paykel SleepStyle

- **Format:** mixed — part public EDF, part encrypted proprietary (per Apnea Board).
- **Independent documentation:** none found for the encrypted portion.
- **Non-GPL code:** none found.
- **SD card:** **no.** Uses a proprietary "InfoUSB" / InfoSmart USB stick instead of an SD card.
  This breaks the SD-slot assumption behind the push-c3 mule/miner hardware bridge — there's no
  SD slot to put an ezShare-style adapter into. Software (parser) support and hardware-bridge
  support are two separate efforts for this machine.
- **Market:** F&P's current actively-sold APAP line — real relevance, unlike Icon.
- **SleepHQ:** supported.
- **Verdict:** implement the EDF-exposed channels directly (zero GPL risk, current product).
  The encrypted portion has no independent documentation anywhere — real reverse-engineering
  effort, not a quick win. Worth doing the EDF half regardless.

## 3. DeVilbiss IntelliPAP / IntelliPAP2

- **Format:** proprietary (SmartLink module + data card).
- **Independent documentation:** Apnea Board Wiki, "GuyScharf: DeVilbiss BLUE (DV6x) support in
  OSCAR 1.2.1 — current status" — a named community member's independent reverse-engineering
  writeup, published separately from OSCAR's own implementation. Safe clean-room source if ever
  pursued.
- **Non-GPL code:** none found.
- **SD card:** yes, confirmed (SD Data Card compatible with IntelliPAP/IntelliPAP2).
- **Market:** Drive/DeVilbiss exited CPAP hardware entirely in Dec 2021 — legacy-only, shrinking
  installed base.
- **SleepHQ:** not listed as supported.
- **Verdict:** low priority.

## 4. BMC Medical

- **Format:** proprietary, but the best-covered of the four outside OSCAR.
- **Independent documentation / non-GPL code:** `github.com/headrotor/BMC_RESmart` —
  "Decode data files from BMC Medical RESmart GII systems. Reverse-engineered from undocumented
  data." **MIT licensed**, actively maintained (last updated Oct 2025). A second repo,
  `pauleaster/bmc_cpap`, builds SD-card reading on top of it but itself carries **no license
  file** — treat that one as unusable-as-is until its terms are clarified; the underlying MIT
  decoder is the real asset.
- **SD card:** model-dependent, not universal. Some BMC units have an SD slot; newer models lean
  on an iCode QR-scan flow or a web portal instead.
- **Market:** one of the faster-growing Chinese manufacturers.
- **SleepHQ:** not currently listed as supported. One secondary (unconfirmed) source claims
  SleepHQ may be building direct cloud-API ingestion for BMC/Resvent, bypassing SD/EDF parsing
  entirely — speculative, don't treat as settled without checking sleephq.com directly.
- **Verdict:** highest priority of the four. Real permissive license, real growth, real prior
  art. Needs verification that BMC_RESmart's GII-era reverse-engineering still covers current
  G2S/G3 model output before assuming drop-in compatibility.

## 5. Resvent iBreeze

- **Format:** proprietary. No documentation of the actual session-data/SD-card format found
  anywhere independent of OSCAR — only firmware-update file references (`csp35.bin`), unrelated
  to therapy data.
- **Non-GPL code:** none found.
- **SD card:** yes, confirmed — a standard SD-card version exists, plus a separate WiFi
  auto-upload version.
- **Market:** newer, fast-growing Chinese entrant gaining real share against ResMed/Philips.
- **SleepHQ:** not currently listed as supported (same speculative direct-API caveat as BMC,
  above).
- **Verdict:** hardest case — no prior art at all. Would require the same from-raw-files
  reverse-engineering approach as the current Philips effort. Highest cost, but market growth
  argues for eventually funding it.

---

## Net prioritization

1. **BMC** — cheapest (MIT-licensed decoder exists), real growth market.
2. **F&P SleepStyle (EDF half only)** — cheap, current product, zero GPL risk.
3. **Resvent** — expensive (no prior art), but real and growing relevance.
4. **F&P Icon / DeVilbiss** — cheap-ish if pursued, but declining/legacy installed base. Low
   priority.

## OSCAR-as-blackbox-parser: explored and deprioritized

Considered running OSCAR itself in Docker (Xvfb/noVNC, e.g. `rogerrum/docker-oscar`) as a
generic "blackbox parser" for brands we don't support directly, combined with:
- `dwin/oscar-export` (GPLv3 Go CLI) — reads OSCAR's on-disk cache directly and reproduces its
  Summary/Sessions/Details CSV exports, no GUI needed.
- `ecwilsonaz/oscar-etl` (MIT) — reads raw EDF directly, full 2-second-resolution timeseries,
  but **ResMed-only today** (no Philips/F&P signal mappings built yet).

**Conclusion: not worth it as a production concurrent-ingestion path.** OSCAR is
one-profile-per-running-instance (not built for N simultaneous automated tenants), there's no
confirmed CLI/silent-import flag (would need human-like GUI automation via `xdotool`, fragile,
no clean success/failure signal), and the per-job resource/latency footprint (full Qt GUI app +
virtual X server) is heavy versus a native parser subprocess call. Only viable as a low-volume,
asynchronous triage tool for whatever lands in the `unknown_uploads` admin queue (SDD-016) — not
as the primary/hot-path parser for concurrent uploads.

## Sources

- OSCAR loader directory listing: `gitlab.com/CrimsonNape/OSCAR-code/oscar/SleepLib/loader_plugins/`
  (filenames only, retrieved 2026-07-07; contents not consulted).
- Apnea Board Wiki: "Fisher & Paykel Icon Data Format", "GuyScharf: DeVilbiss BLUE (DV6x)
  support in OSCAR 1.2.1", "PR System One Data Format" (Philips, see `PHILIPS_FORMAT.md`).
- `github.com/jieter/fph-parser`, `github.com/headrotor/BMC_RESmart`,
  `github.com/pauleaster/bmc_cpap`, `github.com/dwin/oscar-export`,
  `github.com/ecwilsonaz/oscar-etl`.
- SleepHQ (sleephq.com) machine-compatibility claims are secondary-source (search-derived); the
  actual "Supported Machines" page did not resolve via direct fetch at research time — reconfirm
  against the primary page before treating as settled.
