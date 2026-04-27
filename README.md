# JTDX (yanjz124 fork)

A personal fork of [JTDX](https://github.com/jtdx-project/jtdx) with extra passive-mode automation, observability, and integrations. Same GPL-3.0 license; same modes (FT8, FT4, JT65, JT9, T10, WSPR); same authors of all core code (Joe Taylor K1JT, Igor Chernikov UA3DJY, Arvo Järve ES1JA, plus everyone else credited in [THANKS](THANKS)). This fork adds bot-grade passive-mode behavior and a few quality-of-life integrations on top.

> **Upstream:** <https://github.com/jtdx-project/jtdx> — please use upstream for general JTDX support and questions. This fork is not endorsed by the upstream maintainers.

## What's different from upstream

### Passive-mode overhaul
- **Scored candidate ranking** combining JTDX's existing priority (new DXCC, new grid, new prefix, new call, etc.) with: recency of last decode, signal strength, region heatmap from PSK Reporter, observed reply-to-others count, retry fatigue, busy-with-others penalty.
- **Visible candidate list** under the Band Activity pane — table of every station the bot is considering with rank, country, grid, SNR, last-heard, score, and status (CALLING / cooldown / busy / pinned).
- **Manual override** — double-click any row to call that station immediately; right-click for explicit ignore durations (5 min / 10 min / 30 min / 1 h / 4 h / 1 d / session).
- **Iterative selection** — when the top candidate is rejected (cooldown, region, busy, retry fatigue) the next one is evaluated instead of sitting idle.
- **Exponential cooldown with persistence** — repeat-deadbeat stations get longer cooldowns (1.5× per consecutive give-up, capped 30 min) and cooldowns survive JTDX restarts via `cooldowns.json`.
- **Cooldown breakthrough** — a station on cooldown that calls *us* directly is taken off cooldown and switched to as top priority.
- **Region targeting** — Phase-4 filter that demotes basic-priority stations from continents that PSK Reporter says aren't currently hearing us. Sample-size guarded; never blocks new-DXCC/new-call categories.
- **Busy-with-others handling** — pauses retry counter while target is mid-QSO with a different station instead of burning retries.
- **Staleness give-up** — bails on a station early if no decode from them in the last 2 TR periods.

### Integrations
- **PSK Reporter self-monitor** — periodic poll of `pskreporter.info` for reception reports of your own callsign. Status box at bottom of main window with TX-success rate (X/N TX heard %), unique RX count, DXCC count, last-spot age, best SNR, continent breakdown. Audible alert if nobody hears you despite recent TX.
- **Wavelog direct upload** — POSTs each logged QSO directly to a Wavelog server. Replaces the need for a separate WaveLogGate process. Reads existing WaveLogGate `config.json` automatically on first run; full UI under Settings → Reporting.

### Observability
- `enableTx_mode()` calls always log to `ALL.TXT` — diagnoses unwanted-TX scenarios without enabling the global debug checkbox.
- PSK Reporter polls write a rolling log (`%APPDATA%\JTDX\psk_self_monitor.log`) plus the latest raw response (`psk_self_monitor_last.xml`).
- Wavelog uploads write to `%APPDATA%\JTDX\wavelog_uploader.log` with call, exit code, server status, response body.
- `jtdx_cli.py` — Python WSJT-X-protocol UDP debug bridge for inspecting state and sending commands from a shell.

### Other fixes
- Right-side "Rx Frequency" window includes any message involving the current DX target, not just messages within ±10 Hz of RX freq.
- Fixed end-of-QSO `m_ntx` reset so passive mode doesn't keep retransmitting the last RR73/73 message.
- Fixed iterator-after-remove crash in the cooldown cleanup path.

## Building from source

Same as upstream JTDX. Tested on Windows 11 with MSYS2 mingw64 toolchain (Qt 5.15, Hamlib 4.x, OpenSSL 3.x):

```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=C:/msys64/mingw64 \
      -DCMAKE_BUILD_TYPE=Release -DWSJT_GENERATE_DOCS=OFF \
      -DCMAKE_INSTALL_PREFIX=C:/JTDX ..
mingw32-make -j8
mingw32-make install
```

For Linux/macOS see the upstream JTDX [README](README) and [INSTALL](INSTALL).

## Releases

Pre-built Windows installers attached to each tagged release. See [Releases](https://github.com/yanjz124/jtdx/releases). CI builds via GitHub Actions on every tag matching `v*`.

## License

GPL-3.0-or-later, same as upstream JTDX. See [COPYING](COPYING).
