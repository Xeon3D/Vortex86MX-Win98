R6040.SYS 2.0 (build 10) - open-source Ethernet driver for the DM&P
eBOX-3300MX (RDC R6040 MAC, PCI 17F3:6040, Vortex86MX) - Windows 98 SE
======================================================================

Replaces RDC's RDCPCI5.SYS 8.01.08, which hangs Windows 98 at "Windows is
shutting down". Tested on the real eBOX-3300MX (2026-10-19/20):
  - Start -> Shut Down / Restart complete by themselves
  - 32 MB round trip after a fresh boot: 9.0 s down / 6.1 s up, data
    byte-identical (build 8 behaviour; RDC-era builds 3-7: 12.8 s up)

Files
  R6040.SYS     the driver (NDIS 3.0 miniport, MIT license, 16,384 bytes,
                md5 f3847d7490e96fed9ea5080ca9b6edda)
  R6040.INF     clean install through Device Manager (version shown in the
                device name: "... (open R6040.SYS 2.0 build 10)")
  INSTALL.BAT   offline install: swaps R6040.SYS in under the existing
                RDCPCI5.SYS entry (keeps bindings and TCP/IP settings);
                INSTALL CLEAN prepares a Device Manager install on a machine
                that never had RDC's driver; INSTALL DIAG adds the CMOS log
  NETUNDO.BAT   fallback: puts RDC's original driver back (offline)
  RDCPCI5.ORG   RDC's original RDCPCI5.SYS 8.01.08, used by NETUNDO
  R6040DBG.EXE  DOS tool: CMOS shutdown record; /REGS [/PHY] live registers
                and driver counters
  NSET.BAT      set a driver switch: NSET name value (adapter key 0000)
  UNDIAG.BAT    remove the R6040DBG line from AUTOEXEC.BAT again
  OWNNET.BAT,   lab versions that fetch the files over FTP from the build
  NETSEND.BAT   PC (10.0.0.239, exchange\ftpserver.py) and upload results
  src\          sources, makefile, emulator tests, full README.md + changelog

Install (recommended: swap under RDC's entry)
  1. RDC's driver installed and working for the adapter
     (R6040-Ethernet-Win98SE.zip).
  2. Copy this folder to the eBOX, open a DOS box in it, run  INSTALL
     The machine reboots; WININIT puts R6040.SYS in place as
     C:\WINDOWS\SYSTEM\RDCPCI5.SYS.
  3. Check: regedit, HKLM\System\CurrentControlSet\Services\Class\Net\000n,
     value R6040Version = "2.0 build 10" (written by the driver at start).

Install (alternative: clean)
  INSTALL CLEAN, then Device Manager -> the network adapter -> Update
  Driver -> list all drivers -> Have Disk -> C:\WINDOWS\INF -> "RDC R6040
  Fast Ethernet (open R6040.SYS 2.0 build 10)". NETUNDO does not cover
  this route; to go back, remove the adapter or reinstall RDC's package.

Fallback - KEEP THIS
  C:\M2010\NETUNDO      restores RDC's original at the next boot (offline)
  If Windows does not start: boot "Command prompt only" (F8) and type
      copy C:\M2010\NET\RDCPCI5.ORG C:\WINDOWS\SYSTEM\RDCPCI5.SYS
  Safe Mode does not load network drivers.

Settings (strings in the adapter key; NSET.BAT writes them; next boot)
  ConnectionType   0 auto (default), 1 10 half, 2 100 half, 3 10 full,
                   4 100 full (RDC's values)
  R6040TxKick      1 = TX completion interrupt + re-kick (default; the
                   upload fix), 0 = off
  R6040MediaSense  1 = report cable unplug/replug to the protocols (default)
  R6040AnAtInit    1 = renegotiate the link at start (default)
  R6040AnRestart   1 = retry auto-negotiation after ~10 s without link
  R6040PhyPoll     1 = keep the MAC's PHY poller across its reset (default)
  R6040Shutdown    1 = shutdown handler stops DMA at power-off (default)
  R6040Diag        1 = CMOS progress bytes 01h/03h/05h (default), 0 = off
  R6040DbgRegs     1 = live counters for R6040DBG /REGS (default)
  R6040PhyTweak    0 = RDC's PHY tuning off (default; experimental)
  Written by the driver: R6040Version, R6040PhyBoot, R6040Link,
  R6040RegsBoot, R6040Regs (status, for support)

Diagnostics
  R6040DBG            record of the last session (run before Windows
                      starts, e.g. "Command prompt only")
  R6040DBG /LOG file  append it to a file and clear it (AUTOEXEC.BAT)
  R6040DBG /REGS /PHY live MAC/PHY registers and driver counters (also in
                      a Windows DOS box)
  Codes: 1x DriverEntry, 2x Initialize (2E = running), 4x Halt (4E = done),
  5x shutdown handler, 6x reset. A clean restart shows "cp=4E ... halt
  completed: the driver finished cleanly". Details: src\README.md.
