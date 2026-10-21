/*
 * r6040dbg.c - read the R6040.SYS progress bytes from CMOS (real-mode DOS).
 *
 *   R6040DBG              show the record of the last Windows session
 *   R6040DBG /LOG file    append it to 'file' and clear it (AUTOEXEC.BAT)
 *   R6040DBG /C           clear it
 *   R6040DBG /I:hh ...    CMOS index (hex) if R6040DiagCmos was changed
 *   R6040DBG /REGS        dump the live MAC registers (also in a DOS box)
 *            /REGS /PHY   ... plus the PHY's MII registers (via MDIO)
 *            /IO:hhhh     I/O base, if the PCI scan does not find the MAC
 *
 * Run it before Windows starts (AUTOEXEC.BAT or "Command prompt only"):
 * the driver starts a new record as soon as it initializes.
 *
 * MIT License - see LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>

static unsigned idx_base = 1;

/* 32-bit port I/O for the PCI configuration mechanism (needs a 386) */
unsigned long ind(unsigned port);
void outd(unsigned port, unsigned long v);
#pragma aux ind = ".386" "in eax, dx" "mov edx, eax" "shr edx, 16" \
    parm [dx] value [dx ax] modify [ax dx];
#pragma aux outd = ".386" "shl ecx, 16" "mov cx, bx" "mov eax, ecx" \
    "out dx, eax" parm [dx] [cx bx] modify [ax cx];

static unsigned find_io(void)
{
    unsigned dev, fn;
    unsigned long id, bar;

    for (dev = 0; dev < 32; dev++)
        for (fn = 0; fn < 8; fn++) {
            outd(0xCF8, 0x80000000UL | ((unsigned long)dev << 11) | (fn << 8));
            id = ind(0xCFC);
            if (id != 0x604017F3UL)
                continue;
            outd(0xCF8, 0x80000000UL | ((unsigned long)dev << 11) | (fn << 8) | 0x10);
            bar = ind(0xCFC);
            outd(0xCF8, 0);
            if (bar & 1)
                return (unsigned)(bar & 0xFFFC);
        }
    outd(0xCF8, 0);
    return 0;
}

static unsigned phy_rd(unsigned io, unsigned phy, unsigned reg)
{
    unsigned i;
    outpw(io + 0x20, 0x2000 | (phy << 8) | reg);
    for (i = 0; i < 20000; i++)
        if (!(inpw(io + 0x20) & 0x2000))
            return inpw(io + 0x24);
    return 0xFFFF;
}

static int dump_regs(unsigned io, int with_phy)
{
    static const struct { const char *name; unsigned reg; } r[] = {
        { "mcr0", 0x00 }, { "mcr1", 0x04 }, { "mbcr", 0x08 }, { "mticr", 0x0C },
        { "mricr", 0x10 }, { "mrbsr", 0x18 }, { "mrdcr", 0x1A }, { "mlsr", 0x1C },
        { "mtdsa0", 0x2C }, { "mtdsa1", 0x30 }, { "mrdsa0", 0x34 }, { "mrdsa1", 0x38 },
        { "mier", 0x40 }, { "mecier", 0x48 }, { "mar0", 0x60 }, { "mar1", 0x62 },
        { "mar2", 0x64 }, { "mar3", 0x66 }, { "mid0l", 0x68 }, { "mid0m", 0x6A },
        { "mid0h", 0x6C }, { "mpsccr", 0x88 }, { "mpsr", 0x8A }, { "macsm", 0xAC },
        { "mdcsc", 0xB6 }, { "macidrev", 0xBC }, { "macid", 0xBE }
    };
    unsigned i, phy;

    if (io == 0)
        io = find_io();
    if (io == 0) {
        printf("R6040 not found on PCI bus 0 - use /IO:hhhh\n");
        return 1;
    }
    printf("io=%04X", io);
    for (i = 0; i < sizeof(r) / sizeof(r[0]); i++)
        printf("%s%s=%04X", (i % 8 == 7) ? "\n" : " ", r[i].name, inpw(io + r[i].reg));
    printf("\n");
    /* R6040.SYS 1.6+ (R6040DbgRegs=1): live counters in MID1..MID3 */
    if (inpw(io + 0x70) & 1) {
        unsigned w74 = inpw(io + 0x74), w84 = inpw(io + 0x84);
        printf("driver: ticks=%u (x2s) rxresync=%u linkdown=%u linkup=%u lastlink@%u "
               "sent=%u resources=%u kicks=%u stale=%u busy@link=%u busy=%u\n",
               inpw(io + 0x70) >> 1, inpw(io + 0x72), w74 & 0xFF, w74 >> 8,
               inpw(io + 0x78) >> 1, inpw(io + 0x7A), inpw(io + 0x7C),
               inpw(io + 0x80) >> 1, inpw(io + 0x82), w84 & 0xFF, w84 >> 8);
    }
    if (with_phy) {
        phy = (inpw(io + 0x88) >> 8) & 31;
        if (phy == 0)
            phy = 1;
        printf("phy=%u", phy);
        for (i = 0; i < 8; i++)
            printf(" r%u=%04X", i, phy_rd(io, phy, i));
        printf("\n");
    }
    return 0;
}

static unsigned char cmos_rd(unsigned i)
{
    unsigned char v;
    _disable();
    outp(0x70, i);
    v = (unsigned char)inp(0x71);
    _enable();
    return v;
}

static void cmos_wr(unsigned i, unsigned char v)
{
    _disable();
    outp(0x70, i);
    outp(0x71, v);
    _enable();
}

static const char *cp_name(unsigned c)
{
    switch (c) {
    case 0x00: return "no record (driver did not run, or cleared)";
    case 0x10: return "DriverEntry: entered (PE load + relocation OK)";
    case 0x11: return "DriverEntry: NdisInitializeWrapper returned";
    case 0x12: return "DriverEntry: miniport registered (Initialize not called)";
    case 0x1F: return "DriverEntry: FAILED";
    case 0x20: return "Initialize: entered";
    case 0x21: return "Initialize: registry read";
    case 0x22: return "Initialize: PCI resources";
    case 0x23: return "Initialize: I/O ports";
    case 0x24: return "Initialize: DMA memory";
    case 0x25: return "Initialize: MAC reset";
    case 0x26: return "Initialize: PHY / link";
    case 0x27: return "Initialize: interrupt registration";
    case 0x2E: return "Initialize: done, adapter running";
    case 0x40: return "Halt: entered";
    case 0x41: return "Halt: interrupts masked";
    case 0x42: return "Halt: MAC stopped and reset";
    case 0x43: return "Halt: in NdisMDeregisterInterrupt";
    case 0x44: return "Halt: interrupt deregistered";
    case 0x45: return "Halt: in NdisMDeregisterAdapterShutdownHandler";
    case 0x46: return "Halt: memory and ports freed";
    case 0x4E: return "Halt: done";
    case 0x50: return "Shutdown handler: entered";
    case 0x5E: return "Shutdown handler: done";
    case 0x60: return "Reset: entered";
    case 0x6E: return "Reset: done";
    }
    if (c >= 0x31 && c <= 0x37)
        return "Initialize FAILED (step = low digit, see README)";
    return "unknown code";
}

static const char *ev_names[8] = {
    "init-ok", "init-FAILED", "shutdown-entered", "shutdown-done",
    "halt-entered", "halt-done", "reset", "driver-loaded"
};

static const char *warn_names[8] = {
    "MAC-reset-timeout", "MAC-stop-timeout", "MDIO-timeout", "no-PHY",
    "DPC-pending-at-halt", "BIOS-MAC-invalid", "TX-hang-reset",
    "TX-underrun/late-collision"
};

static void flags_str(char *out, unsigned v, const char **names)
{
    int i;
    out[0] = 0;
    for (i = 0; i < 8; i++)
        if (v & (1 << i)) {
            if (out[0])
                strcat(out, " ");
            strcat(out, names[i]);
        }
    if (!out[0])
        strcpy(out, "-");
}

static void verdict(char *out, unsigned cp, unsigned ev)
{
    if (cp == 0 && ev == 0)
        strcpy(out, "nothing recorded");
    else if (cp == 0x4E || (ev & 0x20))
        strcpy(out, "halt completed: the driver finished cleanly");
    else if (cp == 0x5E && !(ev & 0x10))
        strcpy(out, "shutdown handler completed, halt not called");
    else if (cp == 0x2E)
        strcpy(out, "adapter was running; no shutdown/halt reached the driver");
    else if ((cp & 0xF0) == 0x40 || cp == 0x50)
        strcpy(out, "STOPPED INSIDE the step shown above");
    else
        strcpy(out, "see checkpoint");
}

int main(int argc, char **argv)
{
    unsigned cp, ev, wn, regb;
    int i, do_clear = 0, regs = 0, with_phy = 0;
    unsigned io = 0;
    const char *log = NULL;
    char evs[160], wns[160], ver[80];

    for (i = 1; i < argc; i++) {
        if (!stricmp(argv[i], "/C"))
            do_clear = 1;
        else if (!stricmp(argv[i], "/LOG") && i + 1 < argc) {
            log = argv[++i];
            do_clear = 1;
        } else if (!strnicmp(argv[i], "/I:", 3))
            idx_base = (unsigned)strtoul(argv[i] + 3, NULL, 16);
        else if (!stricmp(argv[i], "/REGS"))
            regs = 1;
        else if (!stricmp(argv[i], "/PHY"))
            with_phy = 1;
        else if (!strnicmp(argv[i], "/IO:", 4))
            io = (unsigned)strtoul(argv[i] + 4, NULL, 16);
        else {
            printf("usage: R6040DBG [/LOG file] [/C] [/I:hh] [/REGS [/PHY] [/IO:hhhh]]\n");
            return 1;
        }
    }
    if (regs)
        return dump_regs(io, with_phy);
    if (idx_base != 1 && (idx_base < 0x34 || idx_base > 0x7B)) {
        printf("index must be 1 or 34..7B (hex)\n");
        return 1;
    }

    cp = cmos_rd(idx_base);
    ev = cmos_rd(idx_base + 2);
    wn = cmos_rd(idx_base + 4);
    regb = cmos_rd(0x0B);
    flags_str(evs, ev, ev_names);
    flags_str(wns, wn, warn_names);
    verdict(ver, cp, ev);

    if (log == NULL) {
        printf("R6040.SYS record (CMOS %02Xh/%02Xh/%02Xh):\n",
               idx_base, idx_base + 2, idx_base + 4);
        printf("  last checkpoint : %02X  %s\n", cp, cp_name(cp));
        printf("  events          : %02X  %s\n", ev, evs);
        printf("  warnings        : %02X  %s\n", wn, wns);
        printf("  verdict         : %s\n", ver);
        if (idx_base == 1 && (regb & 0x20))
            printf("  note: RTC alarm interrupt is enabled (register B=%02X)\n", regb);
    } else {
        FILE *f = fopen(log, "a");
        struct dosdate_t d;
        struct dostime_t t;
        if (f == NULL) {
            printf("R6040DBG: cannot open %s\n", log);
            return 1;
        }
        _dos_getdate(&d);
        _dos_gettime(&t);
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u cp=%02X ev=%02X warn=%02X | %s | %s | %s | %s\n",
                d.year, d.month, d.day, t.hour, t.minute, t.second,
                cp, ev, wn, cp_name(cp), evs, wns, ver);
        fclose(f);
        printf("R6040DBG: cp=%02X ev=%02X warn=%02X - %s\n", cp, ev, wn, ver);
    }
    if (do_clear) {
        cmos_wr(idx_base, 0);
        cmos_wr(idx_base + 2, 0);
        cmos_wr(idx_base + 4, 0);
    }
    return 0;
}
