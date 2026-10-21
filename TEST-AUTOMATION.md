# eBOX-3300MX test automation plan

Goal: a build goes from `exchange/out/<ID>` to a pass/fail report with nobody at the
eBOX. Today every step needs a human: type `TRY <ID>`, restart, look at the screen, run
`SEND`/`NETSEND`/`BIGTEST`, and power-cycle after a hang.

## Starting point (checked 2026-10-21)

| Fact | Consequence |
| --- | --- |
| eBOX = 10.0.0.108, answers ping; only TCP 139 (Win98 file sharing) open | No remote shell. Win11's SMB1 client is off; don't re-enable it (security downgrade). |
| PC 10.0.0.239 runs `exchange/ftpserver.py`, logs every RETR/STOR with `seconds=` to `ftplog.txt` | Keep FTP as the transport: the box already pulls through it, and the log gives free timings. |
| The batch toolkit (TRY/BACK/KEEP/GOOD/RESCUE, SEND, NETSEND, BIGTEST) does install + log collection | The agent drives these batches; it doesn't replace them. |
| Drivers are swapped through WININIT.INI at boot; AUTOEXEC.BAT runs `R6040DBG /LOG` | The real-mode AUTOEXEC phase can run code before Windows loads: that's where rollback goes. |
| No COM port, no reset button, power-off only; logs live in CMOS 01/03/05 and the DBGMEM VRAM record | A hang can be undone only by a watchdog or by switching the power. |
| Open Watcom 2.0 builds Win32 PE + DOS binaries here | We can write the on-box tools ourselves (no untrusted downloads). |

## Architecture

```
 PC (10.0.0.239)                                    eBOX (10.0.0.108, Win98 SE)
 ─────────────────                                  ───────────────────────────
 harness.py ── writes job ──> exchange/out/JOB/     EBOXAGT.EXE (Run key, hidden window)
     │                        <seq>.JOB  <──poll── every 5 s: FTP GET JOB/NEXT.TXT
     │                                              runs the steps, writes RESULT.TXT,
     │  <── results, heartbeats ── exchange/in/RUN/<seq>/   screenshots, logs (FTP PUT)
     │
     ├── ping / heartbeat watcher (boot, hang, restart timing)
     ├── power control (smart plug HTTP)  ────────> eBOX mains   [phase 4]
     └── VGA capture (ffmpeg) <──────────────────── eBOX VGA out [phase 4]

 AUTOEXEC.BAT (real mode, before WIN.COM):  BOOTGUARD.EXE
     counts boot attempts; after 2 without "boot OK" from the agent it installs the GOOD set
     (the RESCUE logic) and records why. Re-arms/disarms the Vortex86 watchdog.
```

The agent **polls** the PC instead of listening on a port: no inbound firewall rules on
either side, and a reboot looks like a gap in the heartbeats, nothing else.

## Components

### 1. EBOXAGT.EXE: on-box agent (Win32, Open Watcom, wsock32 only)
- It starts from `HKLM\...\Run` and sends a heartbeat (`in/HB.TXT`: uptime, build IDs from
  `C:\M2010\B\CURV.TXT/CURN.TXT`, driver file sizes) every 30 s.
- It polls `out/JOB/NEXT.TXT` and runs job files, one command per line:
  - `RUN <cmd>`: runs a batch or exe with `CreateProcess` and waits (this covers TRY, SEND,
    NETSEND, BIGTEST, DOSTEST unchanged).
  - `GET`/`PUT`: file transfer.
  - `SHOT <name>`: copies the desktop with `BitBlt` from the screen DC to a BMP (8/16/32 bpp)
    and uploads it.
  - `REG <key>`: exports a registry key.
  - `CMOS`: runs R6040DBG.
  - `MOUSE`/`KEY`: `mouse_event`/`keybd_event` scripts (pointer moves, window drags, Alt+Enter).
  - `RESTART`/`SHUTDOWN`: `ExitWindowsEx`.
  - `WAIT <s>`.
- It writes `RESULT.TXT` with the exit code and time of every step, and marks `C:\M2010\BOOTOK.TXT`
  60 s after the desktop comes up (the signal BOOTGUARD checks).
- Safety: it accepts jobs only from the hard-coded PC address and only from `out/JOB`. It has
  no listening socket. `INSTAGT OFF` (it creates `C:\M2010\AGT\OFF.TXT`) keeps it from starting.

**Phase 1 status (2026-10-21):** EBOXAGT 1.0 + harness.py are in `testauto/` (see its README) and
staged in `exchange/out` (EBOXAGT.EXE, INSTAGT.BAT, AGTON.REG; GET fetches INSTAGT.BAT). They are tested locally only,
not yet on the eBOX. MOUSE/KEY are deferred to phase 3, and the WDT/BOOTOK consumer to phase 2. The agent
already writes `C:\M2010\BOOTOK.TXT`.

### 2. BOOTGUARD.EXE: real-mode rollback + watchdog (DOS, runs from AUTOEXEC.BAT)
- If `BOOTOK.TXT` is missing, it increments `C:\M2010\BOOTTRY.TXT`. At 2 failed attempts it
  copies `GOOD V`/`GOOD N` straight into `C:\WINDOWS\SYSTEM` (the files aren't in use in
  real mode), writes `ROLLBACK.TXT`, and the agent reports it on the next heartbeat.
- **Vortex86MX hardware watchdog (WDT0, index/data ports 22h/23h, registers 37h-3Ch)**: the agent arms
  it (for example 180 s, action = system reset) right before every RESTART/SHUTDOWN job, and
  BOOTGUARD disarms it on the next boot. A freeze in real mode (the NUSB-EHCI kind of
  restart hang) then resets the box by itself instead of waiting for a human. *The register
  layout is from Vortex86 SX/DX docs; check it on the MX first with a harmless test: arm it
  for 60 s in DOS and don't disarm.* The DBGMEM VRAM record survives a WDT reset (no
  power loss), so the `PREV-SHUTDOWN` tracer from builds 18-20 becomes readable again.

### 3. harness.py: PC-side orchestrator (in `testauto/`)
- `harness.py run <suite> --v V33R --n N10` writes the job, watches heartbeats, ping and
  `ftplog.txt`, applies timeouts, and collects everything into `exchange/in/RUN/<seq>/` plus
  `report.md` (pass/fail per check, timings, diffs against the last good run).
- Hang handling: no heartbeat after a restart for 4 min → it records the hang, waits for
  the WDT reset, and if none comes it switches the power off/on through the plug (phase 4).
  Otherwise it stops and notifies the user.
- Claude watches long runs with `Monitor` on the report file. A finished build can be
  queued with a single command.

## Test suites (what each job checks automatically)

| Suite | Steps | Pass criteria (machine-checked) |
| --- | --- | --- |
| `install` | TRY V../N.. → RESTART | back within limit; sizes in sysdir match `out/<ID>`; DriverDesc shows the build string; no ROLLBACK |
| `boot` | ×5 RESTART | 5/5 heartbeats; boot time spread; bootlog.txt has no new `LoadFailed`/`InitCompleteFailed`; R6040 CMOS cp=4E halt every time |
| `net` | BIGTEST ×3, NETSEND | md5 BIGBACK = BIGTEST; down/up `seconds=` from ftplog within limits (≤ 7 s up after the first run); R6040 counters: no TX-underrun bit |
| `gdi` | GDITEST.EXE (new: fills, pattern brushes, ExtTextOut, BitBlt ROPs, lines, at 8/16/32 bpp and each mode from the INF) → SHOT each scene | pixel diff vs golden set (taken once from a confirmed build, e.g. V31R); any non-zero diff is flagged with a diff image |
| `cursor` | load each standard cursor, MOUSE moves, SHOT | screen readback excludes the HW cursor, so this needs the capture card (phase 4); until then the trace (`M2010_trace_cursor`) is checked for sane hide/show counts |
| `ddraw` | DDTEST.EXE (new: exclusive-mode flip, timed with QueryPerformanceCounter, windowed blit) | flips/s ≈ refresh; no lost/duplicated frames by counter; does not crash/hang |
| `dos` | Alt+Enter on a PIF running DOSTEST, back, SHOT | agent alive after; desktop shot = pre-shot; full-screen content needs capture |
| `modes` | SETRATES + change mode/bpp via `ChangeDisplaySettings`, RESTART where needed | mode reported = requested; refresh in HKCC; shot at each mode |
| `dpms` | `SC_MONITORPOWER` off/on | agent alive; capture shows blank then picture (phase 4) |
| `shutdown` | SHUTDOWN, plug off/on (AC-restore) | CMOS cp=4E; power came back; next boot clean (phase 4) |

Default for a new build: `install + boot + net + gdi` (~15 min). `full` adds the rest.

## Phases

| Phase | Work | Human steps |
| --- | --- | --- |
| **0. One-time box prep** | Check: auto-logon to desktop without a password dialog; BIOS "restore on AC power loss = On" (needed in phase 4); `GET` fetches EBOXAGT + BOOTGUARD; one INSTAGT.BAT adds the Run key + AUTOEXEC line | ~5 min, once |
| **1. Agent + harness** | EBOXAGT (RUN/GET/PUT/REG/CMOS/RESTART/heartbeat), harness.py, suites `install`, `boot`, `net`. Test the agent in an emulator first like emu_r6040, then on the box with `RUN SEND` only | none after phase 0 (except a hang) |
| **2. Self-healing** | BOOTGUARD rollback; WDT probe test, then arm-around-restart | none; every hang now ends in a reset + rollback |
| **3. Visual tests** | SHOT, GDITEST, DDTEST, golden-image diffs, MOUSE/KEY scripts; suites `gdi`, `ddraw`, `dos`, `modes` | the user approves the golden set once |
| **4. Hardware add-ons (optional, ~30-40 €)** | **Smart plug** with a local HTTP API (Shelly/Tasmota): power-cycle after hangs the WDT doesn't catch, plus real shutdown tests. **USB VGA capture** (VGA→HDMI converter + HDMI capture stick) recorded with ffmpeg: shows the real scan-out (HW cursor, flicker, DOS full screen, DPMS, POST, the "blinking cursor" freeze screen) instead of photos | plug in once |

## What stays manual
- Cable replugs and link-partner changes (unless a managed switch with port on/off is added).
- Judging "smoothness" by eye; the capture card plus frame-diff timing covers part of it.
- A box that can't get to real-mode DOS any more (BIOS/disk level): RESCUE from a boot floppy/USB.
- Approving a build as GOOD/release: the harness proposes it, the user decides.

## Risks
- Agent bugs stop all automated testing. Keep it tiny and Win32-only, with no ring-0 code. A broken
  agent can be fixed with `GET` by hand.
- Arming the WDT and forgetting to disarm it gives a reset loop. BOOTGUARD disarms it as the first
  thing in AUTOEXEC. The agent arms it only around RESTART/SHUTDOWN jobs and it never runs longer than 512 s.
- Screen readback goes through our own driver (DIB engine back buffer), so it can't see
  scan-out bugs (pitch, flip, HW cursor). That is why capture is listed in phase 4.
- Anonymous FTP with write access plus an agent that runs commands means anything on the LAN can run code
  on the eBOX. Acceptable for this lab LAN. If not: a shared secret in job files and an IP allow-list
  in ftpserver.py (just the box).
