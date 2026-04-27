# JTDX (yanjz124 fork)

Personal fork of [JTDX](https://github.com/jtdx-project/jtdx) with passive-mode automation, observability, and integrations layered on top of the upstream digital-mode functionality.

> **Upstream:** <https://github.com/jtdx-project/jtdx> — for general JTDX support please use upstream. This fork is not endorsed by the upstream maintainers.

## What's added vs. upstream

### Passive-mode candidate ranking & control
- **Visible candidate panel** under the Band Activity pane — table of every station the bot is considering with rank, country, grid, distance (great-circle km/mi), SNR (best/avg), last-heard, score, and live status (CALLING try N/M / cooldown M:SS cycle X / busy with → CALL / pinned).
- **Composite scoring** combining JTDX's existing priority system (new DXCC, new grid, new prefix, etc.) with: recency of last decode, signal strength, region heatmap from PSK Reporter, observed reply-to-others count, retry fatigue, busy-with-others penalty.
- **Iterative selection** — if the top candidate is rejected the next is evaluated instead of sitting idle.
- **Manual override** — double-click any row to call that station immediately (clears cooldown, halts current call, drives selection); right-click for explicit ignore durations (5m / 10m / 30m / 1h / 4h / 1d / session) or to lift cooldown.
- **Already-worked filtering** — stations worked on this band+mode are filtered out of the panel and the selection logic entirely.
- **Exponential cooldown with persistence** — repeat-deadbeats get longer cooldowns (1.5× per consecutive give-up, capped 30 min); cooldowns survive JTDX restarts via `cooldowns.json`.
- **Cooldown breakthrough** — a station on cooldown that calls *us* directly is taken off cooldown and switched to as top priority.
- **Region targeting (Phase 4)** — demotes basic-priority stations from continents that PSK Reporter says aren't currently hearing us. Sample-size guarded; never blocks new-DXCC/new-call priorities.
- **Busy-with-others handling** — pauses the retry counter while target is mid-QSO with another station instead of burning retries.
- **Staleness give-up** — bails on a station early if no decode from them in the last 2 TR periods.

### Integrations
- **PSK Reporter self-monitor** — periodic poll of pskreporter.info for reception reports of your callsign. Status box at the bottom of the main window with TX-success rate (X/N TX heard %), unique RX count, DXCC count, last-spot age, best SNR, continent breakdown. Audible alert if nobody hears you despite recent TX.
- **Wavelog direct upload** — POSTs each logged QSO directly to a Wavelog server. Replaces the need for a separate WaveLogGate process. Reads existing WaveLogGate `config.json` automatically on first run; full UI in Settings → Reporting (URL, API key, station ID, radio name, Test button).

### Observability
- `enableTx_mode()` calls always log to `ALL.TXT` for diagnosing unwanted-TX scenarios.
- PSK Reporter polls write a rolling log to `%APPDATA%\JTDX\psk_self_monitor.log` plus the latest raw response (`psk_self_monitor_last.xml`).
- Wavelog uploads write to `%APPDATA%\JTDX\wavelog_uploader.log`.
- `jtdx_cli.py` — Python WSJT-X UDP debug bridge for inspecting state and sending commands from a shell.

### Other fixes
- Right-side "Rx Frequency" window includes any message involving the current DX target, not just messages within ±10 Hz of RX freq.
- Fixed end-of-QSO `m_ntx` reset so passive mode doesn't keep retransmitting the last RR73/73 message.
- Fixed iterator-after-remove crash in cooldown cleanup.

## Installation

Download the `.zip` below, extract anywhere, run `bin\jtdx.exe`. Or use the `.exe` installer if attached.

If you're upgrading from upstream JTDX, your existing settings (callsign, log, frequencies) are preserved automatically.

## License

GPL-3.0-or-later, same as upstream JTDX.
