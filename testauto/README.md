# testauto: unattended eBOX testing (phase 1 of ../TEST-AUTOMATION.md)

| File | What |
| --- | --- |
| `eboxagt.c` / `eboxagt.exe` | On-box agent (Win32, 58 KB, version 1.1). Started from `HKLM\...\Run`, talks FTP to `exchange/ftpserver.py`. Job commands are listed in its header comment. Tray icon: red = no PC connection, green = idle, yellow = job running; double-click = status, right-click = Status / Open log / Stop agent. |
| `harness.py` | PC side: posts jobs, waits for results/reboots, runs suites, writes `exchange/reports/<time>-<suites>/report.md`. |
| `INSTAGT.BAT`, `AGTON.REG` | One-time install on the box (GET fetches INSTAGT.BAT). `INSTAGT OFF` disables the agent. |

Build: `wcl386 -q -w3 -bt=nt -l=nt_win -bm eboxagt.c wsock32.lib shell32.lib` (Open Watcom env as for the drivers;
set `LIB=C:\WATCOM\lib386;C:\WATCOM\lib386\nt` when building outside this folder),
then copy `eboxagt.exe` to `exchange/out/EBOXAGT.EXE`. Imports only KERNEL32/USER32/GDI32/WSOCK32/SHELL32,
PE subsystem 4.0. The tray icon uses the 88-byte NOTIFYICONDATA that the Win98 shell expects.

## Install on the box (once)

    GET            (refreshes the tools, now including INSTAGT.BAT)
    INSTAGT        (downloads EBOXAGT.EXE to C:\M2010\AGT, adds the Run key, starts it)

Updating the agent later: run `INSTAGT` again on the box, or send a job from the PC:
`GET /out/EBOXAGT.EXE C:\M2010\AGT\EBOXAGT.NEW` + `QUEUE C:\M2010\AGT\EBOXAGT.EXE C:\M2010\AGT\EBOXAGT.NEW` + `RESTART`.

## Use (PC)

    python harness.py status                      heartbeat age, boot number, last job
    python harness.py job "RUN SEND.BAT" STAT     ad-hoc job; prints RESULT.TXT
    python harness.py run install --v V33R        TRY + restart + verify md5/CUR*/bootlog
    python harness.py run boot --boots 5          timed restarts + R6040 CMOS halt check
    python harness.py run net --nets 3            BIGTEST md5 + ftplog timings
    python harness.py run install boot net --v V33R --n N10

Job protocol: `exchange/out/JOB/JOB.TXT` starts with `SEQ n`. The agent runs it once if n is
greater than its `C:\M2010\AGT\LASTSEQ.TXT`, and writes results to `exchange/in/RUN/<n>/`.
The heartbeat is `exchange/in/HB.TXT` (60 s). Polling uses MDTM/SIZE, which pyftpdlib does not log.
Only the heartbeat STOR adds lines to `ftplog.txt`.

## Tested (2026-10-21, locally on Win11 against a pyftpdlib test server, `/dry`)
These all worked: RUN with output capture, GET, PUT, STAT md5s (checked against md5sum), SHOT (24-bit BMP),
unknown command, simulated RESTART + harness reboot detection, `run boot`, PORT fallback (`/active`), single instance,
QUIT, LASTSEQ persistence (no rerun after an agent restart), reconnect after a server restart, and OFF.TXT.
**Not yet run on the eBOX itself.** Real ExitWindowsEx, the DOS-box "close on exit" behaviour of RUN on Win98,
and PASV through the PC firewall are still to be checked there. PORT is the automatic fallback for PASV.
