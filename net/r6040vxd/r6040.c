/*
 * r6040.c - NDIS 3.0 miniport for the RDC R6040 Fast Ethernet MAC
 *           (PCI 17F3:6040) on the DM&P eBOX-3300MX, Windows 98 SE.
 *
 * NT-style miniport (.SYS) loaded by Win98's NDIS.VXD through NTKERN,
 * exactly like RDC's RDCPCI5.SYS, but written so that nothing can be left
 * outstanding at halt or shutdown:
 *
 *  - Send copies the frame into a pre-allocated DMA bounce buffer and
 *    returns NDIS_STATUS_SUCCESS at once.  No send is ever pending, so the
 *    driver never calls NdisMSendComplete.  When the TX ring is full it
 *    returns NDIS_STATUS_RESOURCES and calls NdisMSendResourcesAvailable
 *    once descriptors free up.
 *  - Receive uses NdisMEthIndicateReceive with the whole frame as
 *    lookahead (copy semantics).  No receive packet is ever owned by a
 *    protocol, so halt has nothing to wait for.
 *  - Every hardware wait is a bounded poll (MAC reset, MAC stop, MDIO).
 *    The driver never calls NdisMSleep or any other blocking service.
 *  - The (optional) shutdown handler only masks interrupts and stops the
 *    MAC's DMA.  It frees nothing, takes no lock and calls no NDIS function.
 *  - No spin locks: NDIS serializes an NDIS 3.0 miniport; the only state
 *    shared with the ISR is updated with locked instructions.
 *
 * Optional diagnostics (registry R6040Diag, default 1): progress codes in
 * three RTC alarm bytes of the CMOS (ports 70h/71h, outside the checksummed
 * range), decoded after a reset by R6040DBG.EXE.  See README.md.
 *
 * Register knowledge: FreeBSD vte(4) and Linux r6040 (descriptions only,
 * no code copied).  MIT License - see LICENSE.
 */
#include "ndis98.h"
#include "r6040hw.h"
#include "version.h"

/* compile-time layout checks */
typedef char check_desc_size[sizeof(R6040_DESC) == 32 ? 1 : -1];
typedef char check_chars_size[sizeof(NDIS_MINIPORT_CHARACTERISTICS) == 0x3C ? 1 : -1];
typedef char check_res_size[sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 16 ? 1 : -1];
typedef char check_param_size[sizeof(NDIS_CONFIGURATION_PARAMETER) == 12 ? 1 : -1];

/* ------------------------------------------------------------------ */
/* low level helpers (Open Watcom inline code)                         */

UCHAR  inb(USHORT port);
USHORT inw(USHORT port);
void   outb(USHORT port, UCHAR val);
void   outw(USHORT port, USHORT val);
#pragma aux inb  = "in al, dx"  parm [dx] value [al] modify exact [al];
#pragma aux inw  = "in ax, dx"  parm [dx] value [ax] modify exact [ax];
#pragma aux outb = "out dx, al" parm [dx] [al] modify exact [];
#pragma aux outw = "out dx, ax" parm [dx] [ax] modify exact [];

void  atomic_or(volatile ULONG *p, ULONG v);
ULONG atomic_xchg(volatile ULONG *p, ULONG v);
#pragma aux atomic_or   = "lock or [edx], eax" parm [edx] [eax] modify exact [];
#pragma aux atomic_xchg = "xchg [edx], eax" parm [edx] [eax] value [eax] modify exact [eax];

ULONG flags_cli(void);
void  flags_restore(ULONG f);
#pragma aux flags_cli     = "pushfd" "pop eax" "cli" value [eax] modify exact [eax];
#pragma aux flags_restore = "push eax" "popfd" parm [eax] modify exact [];

void mem_copy(void *dst, const void *src, ULONG n);
void mem_zero(void *dst, ULONG n);
/* dword copy plus byte tail: a quarter of the bus cycles of rep movsb for
 * the TX bounce copy */
#pragma aux mem_copy = "cld" "mov eax, ecx" "shr ecx, 2" "rep movsd" \
                       "mov ecx, eax" "and ecx, 3" "rep movsb" \
                       parm [edi] [esi] [ecx] modify exact [eax edi esi ecx];
#pragma aux mem_zero = "xor eax, eax" "rep stosb" parm [edi] [ecx] modify exact [eax edi ecx];

/*
 * Busy-wait for at least 'us' microseconds without any OS service: the
 * port 61h refresh bit toggles every 15 us on PC chipsets.  The read count
 * cap keeps the wait bounded (an I/O read takes ~0.5-1 us) even if the bit
 * never toggles.
 */
static void udelay(ULONG us)
{
    ULONG toggles = us / 15 + 1;
    ULONG reads = 0, max_reads = us * 8 + 16;
    UCHAR last = inb(0x61) & 0x10, v;

    while (toggles != 0 && reads < max_reads) {
        v = inb(0x61) & 0x10;
        reads++;
        if (v != last) {
            last = v;
            toggles--;
        }
    }
}

/* ------------------------------------------------------------------ */
/* adapter state                                                        */

#define TX_RING     16
#define RX_RING     32
#define BUF_SIZE    1536            /* per descriptor; R_MRBSR value */
#define ETH_HDR     14
#define ETH_MIN     60              /* without FCS */
#define ETH_MAX     1514            /* without FCS */
#define MCAST_MAX   32

typedef struct {
    NDIS_MINIPORT_INTERRUPT intr;   /* first: 8-byte aligned */
    NDIS_HANDLE mh;                 /* MiniportAdapterHandle */
    USHORT      io;                 /* I/O base */
    PVOID       io_off;
    UINT        irq_vector, irq_level;
    UCHAR       io_registered, intr_registered, shutdown_registered, halting;

    UCHAR       perm_mac[6], cur_mac[6];

    /* shared memory: [0] descriptors, [1] TX buffers, [2] RX buffers */
    PVOID       sh_va[3];
    NDIS_PHYSICAL_ADDRESS sh_pa[3];
    ULONG       sh_len[3];

    R6040_DESC *txd, *rxd;          /* 16-byte aligned */
    ULONG       txd_pa, rxd_pa;
    PUCHAR      txb, rxb;
    ULONG       txb_pa, rxb_pa;
    UINT        tx_head, tx_tail, tx_busy;
    UINT        rx_cur;
    UINT        rx_len[RX_RING];    /* frame length (no FCS) while indicated */
    UCHAR       tx_need_resources;
    UCHAR       tx_stall_ticks;
    UINT        tx_tail_at_check;
    USHORT      mier;               /* interrupt mask outside the ISR/DPC */

    volatile ULONG isr_status;      /* MISR bits collected by the ISR */
    volatile ULONG dpc_queued;      /* ISR asked for HandleInterrupt */

    ULONG       filter, lookahead;
    UCHAR       mc[MCAST_MAX][6];
    UINT        mc_count;

    USHORT      mcr0;               /* shadow of R_MCR0 */
    USHORT      mdcsc;
    UINT        phy;                /* MII address, 0..31, 32 = none */
    ULONG       conn_type;          /* 0 auto, 1 10HD, 2 100HD, 3 10FD, 4 100FD */
    UCHAR       link_up, speed100, full_duplex;
    UCHAR       link_reported;      /* state last indicated to NDIS */
    UCHAR       running;            /* Initialize finished */
    UCHAR       an_down_ticks;      /* CheckForHang ticks without link/AN */
    UCHAR       tx_err_seen;        /* CMOS warning for TX errors written */

    ULONG       diag, diag_cmos, use_shutdown, media_sense, phy_tweak, an_restart;
    ULONG       an_at_init, an_wait_ms;
    WCHAR       phy_boot[256];      /* PHY state left by the BIOS (registry) */
    WCHAR       phy_link[256];      /* state after our init (registry) */
    WCHAR       regs_boot[256];     /* MAC registers as the BIOS left them */
    WCHAR       regs_init[256];     /* MAC registers after our init */
    USHORT      mpsccr_bios;        /* MAC PHY-poller config to keep */
    ULONG       phy_poll, mpsr_wait_ms, tx_kick, dbg_regs;
    /* live counters, mirrored into the unused perfect-filter registers
     * MID1..MID3 (R6040DbgRegs) so R6040DBG /REGS can read them */
    USHORT      ticks;              /* CheckForHang calls (~2 s each) */
    USHORT      link_downs, link_ups, link_tick, busy_at_link;
    USHORT      res_returned;       /* NDIS_STATUS_RESOURCES given to NDIS */
    USHORT      tx_kicks;           /* TX_POLL re-kicks with frames pending */
    USHORT      tx_stale;           /* checks where TX made no progress */

    ULONG       xmit_ok, xmit_err, rcv_ok, rcv_err, rcv_crc, rcv_missed;
    ULONG       rcv_overrun, rcv_runt, rcv_long, rx_resync, an_restarts;
    ULONG       xmit_underrun, xmit_latecol;
} R6040;

static NDIS_HANDLE wrapper_handle;

/* ------------------------------------------------------------------ */
/* CMOS progress diagnostics                                           */

/* byte A (index): last checkpoint code; byte B (index+2): sticky event
 * bits; byte C (index+4): sticky warning bits.  Default index 1 = the RTC
 * alarm seconds/minutes/hours bytes, unused unless the RTC alarm interrupt
 * is enabled, never part of the CMOS checksum. */
#define CP_ENTRY            0x10    /* DriverEntry: first statement */
#define CP_ENTRY_WRAPPER    0x11    /* NdisInitializeWrapper returned */
#define CP_ENTRY_OK         0x12    /* NdisMRegisterMiniport succeeded */
#define CP_ENTRY_FAIL       0x1F
#define CP_INIT_ENTER       0x20
#define CP_INIT_CONFIG      0x21
#define CP_INIT_RESOURCES   0x22
#define CP_INIT_IO          0x23
#define CP_INIT_SHMEM       0x24
#define CP_INIT_HWRESET     0x25
#define CP_INIT_PHY         0x26
#define CP_INIT_IRQ         0x27
#define CP_INIT_OK          0x2E
#define CP_INIT_FAIL        0x30    /* + step */
#define CP_HALT_ENTER       0x40
#define CP_HALT_MASKED      0x41
#define CP_HALT_MACSTOP     0x42
#define CP_HALT_DEREG_IRQ   0x43
#define CP_HALT_IRQ_DONE    0x44
#define CP_HALT_DEREG_SHUT  0x45
#define CP_HALT_FREED       0x46
#define CP_HALT_DONE        0x4E
#define CP_SHUT_ENTER       0x50
#define CP_SHUT_DONE        0x5E
#define CP_RESET_ENTER      0x60
#define CP_RESET_DONE       0x6E

#define EV_INIT_OK          0x01
#define EV_INIT_FAIL        0x02
#define EV_SHUT_ENTER       0x04
#define EV_SHUT_DONE        0x08
#define EV_HALT_ENTER       0x10
#define EV_HALT_DONE        0x20
#define EV_RESET            0x40
#define EV_DRIVER_LOADED    0x80

#define WARN_MAC_RESET_TO   0x01
#define WARN_MAC_STOP_TO    0x02
#define WARN_MDIO_TO        0x04
#define WARN_NO_PHY         0x08
#define WARN_DPC_AT_HALT    0x10
#define WARN_BAD_MAC        0x20
#define WARN_TX_HANG        0x40
#define WARN_TX_ERR         0x80    /* TX underrun or late collision seen */

static UCHAR cmos_read(UCHAR idx)
{
    ULONG f = flags_cli();
    UCHAR v;
    outb(0x70, idx);
    v = inb(0x71);
    flags_restore(f);
    return v;
}

static void cmos_write(UCHAR idx, UCHAR v)
{
    ULONG f = flags_cli();
    outb(0x70, idx);
    outb(0x71, v);
    flags_restore(f);
}

static void diag_cp(R6040 *a, UCHAR code)
{
    if (a->diag)
        cmos_write((UCHAR)a->diag_cmos, code);
}

static void diag_or(R6040 *a, UCHAR off, UCHAR bits)
{
    UCHAR idx;
    if (!a->diag)
        return;
    idx = (UCHAR)(a->diag_cmos + off);
    cmos_write(idx, (UCHAR)(cmos_read(idx) | bits));
}

#define diag_ev(a, b)   diag_or(a, 2, b)
#define diag_warn(a, b) diag_or(a, 4, b)

/* ------------------------------------------------------------------ */
/* chip access                                                         */

#define RD(a, r)        inw((USHORT)((a)->io + (r)))
#define WR(a, r, v)     outw((USHORT)((a)->io + (r)), (USHORT)(v))

static USHORT phy_read(R6040 *a, UINT phy, UINT reg)
{
    UINT i;

    WR(a, R_MMDIO, MMDIO_READ | (phy << 8) | reg);
    for (i = 0; i < 2000; i++) {
        if (!(RD(a, R_MMDIO) & MMDIO_READ))
            return RD(a, R_MMRD);
        udelay(5);
    }
    diag_warn(a, WARN_MDIO_TO);
    return 0xFFFF;
}

static void phy_write(R6040 *a, UINT phy, UINT reg, USHORT val)
{
    UINT i;

    WR(a, R_MMWD, val);
    WR(a, R_MMDIO, MMDIO_WRITE | (phy << 8) | reg);
    for (i = 0; i < 2000; i++) {
        if (!(RD(a, R_MMDIO) & MMDIO_WRITE))
            return;
        udelay(5);
    }
    diag_warn(a, WARN_MDIO_TO);
}

/* Disable RX/TX and wait (bounded, ~10 ms) until the MAC reports it. */
static void mac_stop(R6040 *a)
{
    UINT i;

    a->mcr0 &= ~(MCR0_RX_ENB | MCR0_TX_ENB);
    WR(a, R_MCR0, RD(a, R_MCR0) & ~(MCR0_RX_ENB | MCR0_TX_ENB));
    for (i = 0; i < 1000; i++) {
        if (!(RD(a, R_MCR0) & (MCR0_RX_ENB | MCR0_TX_ENB)))
            return;
        udelay(10);
    }
    diag_warn(a, WARN_MAC_STOP_TO);
}

/*
 * MAC reset.  The vendor says MCR1_MAC_RESET is not a reliable completion
 * flag, so the wait is bounded (~2 ms) and the internal state machine is
 * reset by hand afterwards, then the MDC clock setting is restored.
 */
static void mac_reset(R6040 *a)
{
    UINT i;

    WR(a, R_MCR1, RD(a, R_MCR1) | MCR1_MAC_RESET);
    for (i = 0; i < 200; i++) {
        udelay(10);
        if (!(RD(a, R_MCR1) & MCR1_MAC_RESET))
            break;
    }
    if (i == 200)
        diag_warn(a, WARN_MAC_RESET_TO);
    WR(a, R_MACSM, MACSM_RESET);
    WR(a, R_MACSM, 0);
    udelay(5000);
    if (a->mdcsc != MDCSC_DEFAULT && a->mdcsc != 0 && a->mdcsc != 0xFFFF)
        WR(a, R_MDCSC, a->mdcsc);
    /* The reset clears MPSCCR, the MAC's own PHY status poller, which the
     * BIOS had set up; without it MPSR (the MAC's view of link, speed and
     * duplex) stays 0.  Put the BIOS value back (R6040PhyPoll). */
    if (a->phy_poll && a->mpsccr_bios != 0 && a->mpsccr_bios != 0xFFFF &&
        RD(a, R_MPSCCR) != a->mpsccr_bios)
        WR(a, R_MPSCCR, a->mpsccr_bios);
}

/* Big-endian CRC-32 of a MAC address, as the hash filter uses it. */
static ULONG ether_crc_be(const UCHAR *p)
{
    ULONG crc = 0xFFFFFFFFUL;
    UINT i, b;

    for (i = 0; i < 6; i++) {
        UCHAR c = p[i];
        for (b = 0; b < 8; b++, c >>= 1) {
            ULONG top = crc >> 31;
            crc <<= 1;
            if (top ^ (c & 1))
                crc ^= 0x04C11DB7UL;
        }
    }
    return crc;
}

/* Program promiscuous/multicast filtering from a->filter and a->mc. */
static void set_rx_filter(R6040 *a)
{
    USHORT hash[4] = { 0, 0, 0, 0 };
    UINT i;

    a->mcr0 &= ~(MCR0_PROMISC | MCR0_MULTICAST | MCR0_BROADCAST_DIS);
    if (a->filter & NDIS_PACKET_TYPE_PROMISCUOUS) {
        a->mcr0 |= MCR0_PROMISC | MCR0_MULTICAST;
        hash[0] = hash[1] = hash[2] = hash[3] = 0xFFFF;
    } else if (a->filter & NDIS_PACKET_TYPE_ALL_MULTICAST) {
        a->mcr0 |= MCR0_MULTICAST;
        hash[0] = hash[1] = hash[2] = hash[3] = 0xFFFF;
    } else if ((a->filter & NDIS_PACKET_TYPE_MULTICAST) && a->mc_count) {
        a->mcr0 |= MCR0_MULTICAST;
        for (i = 0; i < a->mc_count; i++) {
            ULONG bit = ether_crc_be(a->mc[i]) >> 26;   /* 6-bit index */
            hash[bit >> 4] |= (USHORT)(1 << (bit & 0x0F));
        }
    }
    /* perfect filter slots 1..3 unused (NDIS filters in software too);
     * with R6040DbgRegs they carry the live counters instead */
    for (i = 0; i < 3 && !a->dbg_regs; i++) {
        WR(a, R_MID1L + 8 * i + 0, 0);
        WR(a, R_MID1L + 8 * i + 2, 0);
        WR(a, R_MID1L + 8 * i + 4, 0);
    }
    for (i = 0; i < 4; i++)
        WR(a, R_MAR0 + 2 * i, hash[i]);
    WR(a, R_MCR0, a->mcr0);
}

static void write_station_address(R6040 *a)
{
    WR(a, R_MID0L + 0, a->cur_mac[0] | (a->cur_mac[1] << 8));
    WR(a, R_MID0L + 2, a->cur_mac[2] | (a->cur_mac[3] << 8));
    WR(a, R_MID0L + 4, a->cur_mac[4] | (a->cur_mac[5] << 8));
}

/* ------------------------------------------------------------------ */
/* PHY / link                                                          */

static int phy_present(R6040 *a, UINT phy)
{
    USHORT id = phy_read(a, phy, MII_PHYID1);
    return id != 0 && id != 0xFFFF;
}

static void phy_find(R6040 *a)
{
    UINT bios = (RD(a, R_MPSCCR) & MPSCCR_PHY_ADDR_MASK) >> MPSCCR_PHY_ADDR_SHIFT;
    UINT i;

    if (bios != 0 && phy_present(a, bios)) {
        a->phy = bios;
        return;
    }
    for (i = 1; i <= 32; i++) {         /* 1..31, then 0 */
        UINT p = i & 31;
        if (phy_present(a, p)) {
            a->phy = p;
            return;
        }
    }
    a->phy = 32;
    diag_warn(a, WARN_NO_PHY);
}

/* Apply ConnectionType.  Auto-negotiation is only restarted when the PHY
 * is not already negotiating with our advertisement, so a link the BIOS
 * brought up stays up through boot (DHCP). */
/* tiny WCHAR formatting for the registry diagnostics (no C runtime) */
#define WSTR_MAX 255
static UINT wput(WCHAR *b, UINT n, const char *t)
{
    while (*t && n < WSTR_MAX)
        b[n++] = (WCHAR)*t++;
    b[n] = 0;
    return n;
}

static UINT whex(WCHAR *b, UINT n, USHORT v)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 12; i >= 0 && n < WSTR_MAX; i -= 4)
        b[n++] = (WCHAR)hx[(v >> i) & 15];
    b[n] = 0;
    return n;
}

static UINT wdec(WCHAR *b, UINT n, ULONG v)
{
    char t[12];
    int i = 0;
    do {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i < 11);
    while (i > 0 && n < WSTR_MAX)
        b[n++] = (WCHAR)t[--i];
    b[n] = 0;
    return n;
}

/* "phy=1 bmcr=1100 bmsr=786d anar=01e1 lpa=45e1 mpsr=0007" */
static UINT phy_snapshot(R6040 *a, WCHAR *b)
{
    UINT n = 0;

    if (a->phy > 31) {
        n = wput(b, n, "no MII PHY mpsr=");
        return whex(b, n, RD(a, R_MPSR));
    }
    phy_read(a, a->phy, MII_BMSR);
    n = wput(b, n, "phy=");
    n = wdec(b, n, a->phy);
    n = wput(b, n, " bmcr=");
    n = whex(b, n, phy_read(a, a->phy, MII_BMCR));
    n = wput(b, n, " bmsr=");
    n = whex(b, n, phy_read(a, a->phy, MII_BMSR));
    n = wput(b, n, " anar=");
    n = whex(b, n, phy_read(a, a->phy, MII_ANAR));
    n = wput(b, n, " lpa=");
    n = whex(b, n, phy_read(a, a->phy, MII_ANLPAR));
    n = wput(b, n, " mpsr=");
    return whex(b, n, RD(a, R_MPSR));
}

/* MAC register snapshot for the registry.  MISR (read clears) and the
 * event counters are left out. */
static void regs_snapshot(R6040 *a, WCHAR *b)
{
    static const struct { char name[8]; UCHAR reg; } r[] = {
        { "mcr0", R_MCR0 }, { "mcr1", R_MCR1 }, { "mbcr", R_MBCR },
        { "mticr", R_MTICR }, { "mricr", R_MRICR }, { "mrbsr", R_MRBSR },
        { "mrdcr", R_MRDCR }, { "mlsr", R_MLSR }, { "mier", R_MIER },
        { "mecier", R_MECIER }, { "mpsccr", R_MPSCCR }, { "mpsr", R_MPSR },
        { "macsm", R_MACSM }, { "mdcsc", R_MDCSC }, { "macid", R_MACID }
    };
    UINT n, i;

    n = wput(b, 0, "io=");
    n = whex(b, n, a->io);
    for (i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
        n = wput(b, n, " ");
        n = wput(b, n, r[i].name);
        n = wput(b, n, "=");
        n = whex(b, n, RD(a, r[i].reg));
    }
}

/* append " mpsr_wait=NNNms mpsr=xxxx" to R6040Link */
static void n_mpsr_note(R6040 *a)
{
    UINT n = 0;
    while (a->phy_link[n])
        n++;
    n = wput(a->phy_link, n, " mpsr_wait=");
    n = wdec(a->phy_link, n, a->mpsr_wait_ms);
    n = wput(a->phy_link, n, "ms mpsr=");
    whex(a->phy_link, n, RD(a, R_MPSR));
}

/* Wait (bounded, 4 s) for link + auto-negotiation complete after a restart. */
static void phy_wait_an(R6040 *a)
{
    ULONG ms;
    USHORT bmsr;

    for (ms = 0; ms < 4000; ms += 20) {
        phy_read(a, a->phy, MII_BMSR);          /* latched bits */
        bmsr = phy_read(a, a->phy, MII_BMSR);
        if (bmsr == 0xFFFF)
            break;
        if ((bmsr & (BMSR_LINK | BMSR_ANCOMP)) == (BMSR_LINK | BMSR_ANCOMP))
            break;
        udelay(20000);
    }
    a->an_wait_ms = ms;
}

static void phy_setup(R6040 *a)
{
    USHORT bmcr, anar, want;

    if (a->phy > 31)
        return;
    bmcr = phy_read(a, a->phy, MII_BMCR);
    if (bmcr & (BMCR_ISOLATE | 0x0800))         /* isolate / power down */
        bmcr &= ~(BMCR_ISOLATE | 0x0800);

    switch (a->conn_type) {
    case 1: want = 0;                               break;  /* 10 half */
    case 2: want = BMCR_SPEED100;                   break;  /* 100 half */
    case 3: want = BMCR_FULLDPLX;                   break;  /* 10 full */
    case 4: want = BMCR_SPEED100 | BMCR_FULLDPLX;   break;  /* 100 full */
    default:
        anar = phy_read(a, a->phy, MII_ANAR);
        /* Restart auto-negotiation at every init (R6040AnAtInit, default):
         * the link the BIOS left behind came up half duplex on the eBOX
         * (uploads at half speed until a cable replug renegotiated it). */
        if (a->an_at_init || (anar & ANAR_ALL) != ANAR_ALL || !(bmcr & BMCR_ANEN)) {
            phy_write(a, a->phy, MII_ANAR, (USHORT)((anar & ~0x01FF) | ANAR_ALL));
            phy_write(a, a->phy, MII_BMCR,
                      (USHORT)((bmcr & ~(BMCR_SPEED100 | BMCR_FULLDPLX)) |
                               BMCR_ANEN | BMCR_RESTART_AN));
            udelay(1000);
            phy_wait_an(a);
        }
        return;
    }
    phy_write(a, a->phy, MII_BMCR,
              (USHORT)((bmcr & ~(BMCR_ANEN | BMCR_SPEED100 | BMCR_FULLDPLX)) | want));
}

/* Interrupt mask outside the ISR/DPC: RX always; TX_DONE always with
 * R6040TxKick (each completion re-kicks the TX engine), otherwise only
 * while the ring is full. */
static USHORT base_mier(R6040 *a)
{
    return (USHORT)(R6040_RX_INTS | (a->tx_kick ? MISR_TX_DONE : 0));
}

/* Read the link state and program MCR0 duplex to match. */
static void link_update(R6040 *a)
{
    USHORT bmsr = 0xFFFF, mcr0;
    UCHAR up = 0, s100 = 0, fd = 0;

    if (a->phy <= 31) {
        phy_read(a, a->phy, MII_BMSR);          /* link bit is latched low */
        bmsr = phy_read(a, a->phy, MII_BMSR);
        if (bmsr != 0xFFFF && (bmsr & BMSR_LINK)) {
            up = 1;
            if (a->conn_type >= 1 && a->conn_type <= 4) {
                s100 = (a->conn_type == 2 || a->conn_type == 4);
                fd   = (a->conn_type == 3 || a->conn_type == 4);
            } else {
                USHORT lpa = phy_read(a, a->phy, MII_ANLPAR);
                USHORT adv = phy_read(a, a->phy, MII_ANAR);
                if ((bmsr & BMSR_ANCOMP) && lpa != 0xFFFF &&
                    !(lpa & (ANAR_100FD | ANAR_100HD | ANAR_10FD | ANAR_10HD))) {
                    /* AN complete but no partner abilities recorded: use the
                     * MAC's own view of the PHY */
                    USHORT st = RD(a, R_MPSR);
                    s100 = (st & MPSR_SPEED_100) != 0;
                    fd = (st & MPSR_FULL_DUPLEX) != 0;
                } else if (bmsr & BMSR_ANCOMP) {
                    USHORT c = adv & lpa;
                    if (c & ANAR_100FD)      { s100 = 1; fd = 1; }
                    else if (c & ANAR_100HD) { s100 = 1; }
                    else if (c & ANAR_10FD)  { fd = 1; }
                } else {
                    /* parallel detection: half duplex */
                    s100 = (lpa & (ANAR_100FD | ANAR_100HD)) != 0;
                }
            }
        }
    } else {
        USHORT st = RD(a, R_MPSR);              /* no MII: trust the MAC */
        up = (st & MPSR_LINK_UP) != 0;
        s100 = (st & MPSR_SPEED_100) != 0;
        fd = (st & MPSR_FULL_DUPLEX) != 0;
    }

    if (up != a->link_up && a->running) {
        if (up)
            a->link_ups++;
        else
            a->link_downs++;
        a->link_tick = a->ticks;
        a->busy_at_link = (USHORT)a->tx_busy;
    }
    a->link_up = up;
    if (up) {
        a->an_down_ticks = 0;
        a->speed100 = s100;
        a->full_duplex = fd;
        mcr0 = (USHORT)((a->mcr0 & ~MCR0_FULL_DUPLEX) | (fd ? MCR0_FULL_DUPLEX : 0));
        if (mcr0 != a->mcr0) {
            a->mcr0 = mcr0;
            WR(a, R_MCR0, a->mcr0);
        }
    } else if (a->an_restart && a->conn_type == 0 && a->phy <= 31 &&
               bmsr != 0xFFFF && !(bmsr & BMSR_ANCOMP)) {
        /* Auto-negotiation that never completes (some switches): restart it
         * after ~10 s without link, then every ~10 s while it stays down. */
        if (++a->an_down_ticks >= 5) {
            USHORT bmcr = phy_read(a, a->phy, MII_BMCR);
            if (bmcr != 0xFFFF)
                phy_write(a, a->phy, MII_BMCR,
                          (USHORT)(bmcr | BMCR_ANEN | BMCR_RESTART_AN));
            a->an_down_ticks = 0;
            a->an_restarts++;
        }
    } else {
        a->an_down_ticks = 0;
    }
}

/* RDC's PHY tuning ("improve performance", from the vendor's Linux driver):
 * internal PHY registers at MII addresses 30 and 0.  Off by default. */
static void phy_vendor_tweak(R6040 *a)
{
    USHORT v = phy_read(a, 30, 17);
    if (v == 0xFFFF)
        return;
    phy_write(a, 30, 17, (USHORT)(v | 0x4000));
    v = phy_read(a, 30, 17);
    phy_write(a, 30, 17, (USHORT)(v & ~0x2000));
    phy_write(a, 0, 19, 0x0000);
    phy_write(a, 0, 30, 0x01F0);
}

/* Report a link change to the protocols. */
static void link_indicate(R6040 *a)
{
    if (!a->running || a->halting || !a->media_sense ||
        a->link_up == a->link_reported)
        return;
    a->link_reported = a->link_up;
    NdisMIndicateStatus(a->mh, a->link_up ? NDIS_STATUS_MEDIA_CONNECT
                                          : NDIS_STATUS_MEDIA_DISCONNECT, NULL, 0);
    NdisMIndicateStatusComplete(a->mh);
}

/* Live counters in MID1..MID3 (perfect filter slots, unused by us).  The
 * first word of each slot keeps bit 0 set (a multicast address), so no
 * unicast frame can match.  Layout, read with R6040DBG /REGS:
 *   70 mid1l  ticks (CheckForHang calls, ~2 s)    72 RX resyncs
 *   74 link downs (lo) / ups (hi)                 78 tick of last link change
 *   7A frames sent (low 16 bits)                  7C RESOURCES returned
 *   80 TX re-kicks                                82 checks without TX progress
 *   84 tx_busy at last link change (lo) / now (hi)
 * 70/78/80 are shifted left by one with bit 0 set. */
static void dbg_regs_write(R6040 *a)
{
    if (!a->dbg_regs)
        return;
    WR(a, R_MID1L + 0, (USHORT)((a->ticks << 1) | 1));
    WR(a, R_MID1L + 2, (USHORT)a->rx_resync);
    WR(a, R_MID1L + 4, (USHORT)((a->link_downs & 0xFF) | (a->link_ups << 8)));
    WR(a, R_MID1L + 8, (USHORT)((a->link_tick << 1) | 1));
    WR(a, R_MID1L + 10, (USHORT)a->xmit_ok);
    WR(a, R_MID1L + 12, a->res_returned);
    WR(a, R_MID1L + 16, (USHORT)((a->tx_kicks << 1) | 1));
    WR(a, R_MID1L + 18, a->tx_stale);
    WR(a, R_MID1L + 20, (USHORT)((a->busy_at_link & 0xFF) | (a->tx_busy << 8)));
}

/* MAC event counters (8-bit fields, reading clears): sampled every ~2 s. */
static void hw_counters(R6040 *a)
{
    USHORT v;

    RD(a, R_MECISR);
    RD(a, R_CNT_RX_DONE);
    RD(a, R_CNT_MECNT0);
    v = RD(a, R_CNT_MECNT1);
    a->rcv_runt += v >> 8;
    v = RD(a, R_CNT_MECNT2);
    a->rcv_long += v & 0xFF;
    v = RD(a, R_CNT_MECNT3);
    a->rcv_overrun += v >> 8;           /* RX FIFO full */
    a->rcv_missed += v & 0xFF;          /* no free RX descriptor */
    RD(a, R_CNT_TX_DONE);
    v = RD(a, R_CNT_MECNT4);
    if (v && a->running) {
        a->xmit_underrun += v >> 8;
        a->xmit_latecol += v & 0xFF;
        if (!a->tx_err_seen) {
            a->tx_err_seen = 1;
            diag_warn(a, WARN_TX_ERR);
        }
    }
    RD(a, R_CNT_PAUSE);
}

/* ------------------------------------------------------------------ */
/* rings                                                               */

static void rings_init(R6040 *a)
{
    UINT i;

    for (i = 0; i < TX_RING; i++) {
        R6040_DESC *d = &a->txd[i];
        d->status = 0;
        d->len = 0;
        d->buf = a->txb_pa + i * BUF_SIZE;
        d->next = a->txd_pa + ((i + 1) % TX_RING) * sizeof(R6040_DESC);
        d->hidx = 0;
    }
    for (i = 0; i < RX_RING; i++) {
        R6040_DESC *d = &a->rxd[i];
        d->len = BUF_SIZE;
        d->buf = a->rxb_pa + i * BUF_SIZE;
        d->next = a->rxd_pa + ((i + 1) % RX_RING) * sizeof(R6040_DESC);
        d->hidx = 0;
        d->status = DESC_OWN;
    }
    a->tx_head = a->tx_tail = a->tx_busy = 0;
    a->tx_tail_at_check = 0;
    a->tx_stall_ticks = 0;
    a->rx_cur = 0;
}

/* Full chip (re)initialisation; leaves interrupts masked. */
static void hw_init(R6040 *a)
{
    WR(a, R_MIER, 0);
    mac_stop(a);
    mac_reset(a);
    RD(a, R_MISR);

    rings_init(a);

    WR(a, R_MBCR, MBCR_DEFAULT);
    WR(a, R_MRBSR, BUF_SIZE);
    WR(a, R_MTDSA0, a->txd_pa & 0xFFFF);
    WR(a, R_MTDSA1, a->txd_pa >> 16);
    WR(a, R_MRDSA0, a->rxd_pa & 0xFFFF);
    WR(a, R_MRDSA1, a->rxd_pa >> 16);
    WR(a, R_MTICR, 0);                  /* no interrupt moderation */
    WR(a, R_MRICR, 0);
    WR(a, R_MECIER, 0);                 /* no event counter interrupts */
    write_station_address(a);

    a->mcr0 = (USHORT)(MCR0_RX_ENB | MCR0_TX_ENB |
                       (a->full_duplex ? MCR0_FULL_DUPLEX : 0));
    set_rx_filter(a);                   /* writes MCR0: MAC running */
    RD(a, R_MISR);
    hw_counters(a);                     /* start the counters from zero */
    a->tx_need_resources = 0;
    a->mier = base_mier(a);
    dbg_regs_write(a);
}

/* TX descriptors the MAC has finished with. */
static void tx_reclaim(R6040 *a)
{
    while (a->tx_busy) {
        R6040_DESC *d = &a->txd[a->tx_tail];
        USHORT st = d->status;
        if (st & DESC_OWN)
            break;
        if (st & (DTST_UNDERRUN | DTST_LATE_COL | DTST_EXCESS_COL)) {
            a->xmit_err++;
            if (!a->tx_err_seen) {
                a->tx_err_seen = 1;
                diag_warn(a, WARN_TX_ERR);
            }
            if (st & DTST_UNDERRUN) a->xmit_underrun++;
            if (st & DTST_LATE_COL) a->xmit_latecol++;
        } else {
            a->xmit_ok++;
        }
        a->tx_tail = (a->tx_tail + 1) % TX_RING;
        a->tx_busy--;
    }
}

/* Indicate received frames; descriptors go back to the MAC only after the
 * receive-complete indication (a protocol may call TransferData until then). */
/* The MAC reported RX work but the descriptor we expect is still its own:
 * our index and the MAC's are out of step.  Jump to the oldest completed
 * frame (scanning forward from where we are). */
static void rx_resync(R6040 *a)
{
    UINT i;

    for (i = 1; i < RX_RING; i++) {
        UINT k = (a->rx_cur + i) % RX_RING;
        if (!(a->rxd[k].status & DESC_OWN)) {
            a->rx_cur = k;
            a->rx_resync++;
            return;
        }
    }
}

static UINT rx_process(R6040 *a)
{
    UINT first, n = 0, indicated = 0, i;

    if (a->rxd[a->rx_cur].status & DESC_OWN)
        rx_resync(a);
    first = a->rx_cur;
    while (n < RX_RING) {
        R6040_DESC *d = &a->rxd[a->rx_cur];
        USHORT st = d->status;
        UINT len;

        if (st & DESC_OWN)
            break;
        len = d->len & DESC_LEN_MASK;
        a->rx_len[a->rx_cur] = 0;
        if ((st & DRST_RX_OK) && len >= ETH_HDR + 4 && len <= BUF_SIZE) {
            PUCHAR f = a->rxb + a->rx_cur * BUF_SIZE;
            len -= 4;                   /* strip FCS */
            a->rx_len[a->rx_cur] = len;
            a->rcv_ok++;
            if (!a->halting) {
                NdisMEthIndicateReceive(a->mh, (NDIS_HANDLE)(a->rx_cur + 1),
                                        f, ETH_HDR, f + ETH_HDR,
                                        len - ETH_HDR, len - ETH_HDR);
                indicated++;
            }
        } else {
            a->rcv_err++;
            if (st & DRST_CRC_ERR)
                a->rcv_crc++;
        }
        a->rx_cur = (a->rx_cur + 1) % RX_RING;
        n++;
    }
    if (indicated)
        NdisMEthIndicateReceiveComplete(a->mh);

    for (i = 0; i < n; i++) {
        R6040_DESC *d = &a->rxd[(first + i) % RX_RING];
        a->rx_len[(first + i) % RX_RING] = 0;
        d->len = BUF_SIZE;
        d->status = DESC_OWN;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* interrupt handling                                                  */

static VOID __stdcall R6040Isr(PBOOLEAN recognized, PBOOLEAN queue_dpc,
                               NDIS_HANDLE ctx)
{
    R6040 *a = (R6040 *)ctx;
    USHORT st;

    WR(a, R_MIER, 0);                   /* mask first, then read: acknowledges */
    st = RD(a, R_MISR);
    if (st == 0xFFFF || !(st & R6040_INTRS)) {
        if (!a->dpc_queued && !a->halting)
            WR(a, R_MIER, a->mier);     /* shared IRQ, not ours */
        *recognized = FALSE;
        *queue_dpc = FALSE;
        return;
    }
    atomic_or(&a->isr_status, st);
    *recognized = TRUE;
    if (a->halting) {
        *queue_dpc = FALSE;
    } else {
        a->dpc_queued = 1;
        *queue_dpc = TRUE;
    }
}

static VOID __stdcall R6040HandleInterrupt(NDIS_HANDLE ctx)
{
    R6040 *a = (R6040 *)ctx;
    UINT loops;

    /* Bounded: at most 8 passes of at most RX_RING frames.  Work left over
     * re-asserts the interrupt as soon as MIER is restored. */
    for (loops = 0; loops < 8 && !a->halting; loops++) {
        ULONG st = atomic_xchg(&a->isr_status, 0) | RD(a, R_MISR);
        if (!(st & R6040_INTRS))
            break;
        if (st & R6040_RX_INTS)
            rx_process(a);
    }
    tx_reclaim(a);
    if (a->tx_need_resources && a->tx_busy < TX_RING && !a->halting) {
        a->tx_need_resources = 0;
        a->mier = base_mier(a);
        NdisMSendResourcesAvailable(a->mh);
    }
    if (a->tx_kick && a->tx_busy && !a->halting) {
        /* frames still queued after a completion: make sure the TX engine
         * looks at them (a TX_POLL written while it was busy can be lost) */
        WR(a, R_TX_POLL, TX_POLL_START);
        a->tx_kicks++;
    }
    a->dpc_queued = 0;
    if (!a->halting)
        WR(a, R_MIER, a->mier);
}

static VOID __stdcall R6040DisableInterrupt(NDIS_HANDLE ctx)
{
    WR((R6040 *)ctx, R_MIER, 0);
}

static VOID __stdcall R6040EnableInterrupt(NDIS_HANDLE ctx)
{
    R6040 *a = (R6040 *)ctx;
    if (!a->halting)
        WR(a, R_MIER, a->mier);
}

/* ------------------------------------------------------------------ */
/* send / transfer data                                                */

static NDIS_STATUS __stdcall R6040Send(NDIS_HANDLE ctx, PNDIS_PACKET pkt,
                                       UINT flags)
{
    R6040 *a = (R6040 *)ctx;
    PNDIS_BUFFER b;
    PVOID va;
    UINT blen, total, len = 0;
    PUCHAR dst;
    R6040_DESC *d;

    (void)flags;
    if (a->halting)
        return NDIS_STATUS_FAILURE;
    if (a->tx_busy)
        tx_reclaim(a);                  /* TX_DONE is normally not an IRQ */
    if (a->tx_busy == TX_RING) {
        /* ring full: let TX_DONE interrupt so NDIS can be resumed */
        a->tx_need_resources = 1;
        a->res_returned++;
        a->mier = R6040_RX_INTS | MISR_TX_DONE;
        if (!a->dpc_queued)
            WR(a, R_MIER, a->mier);
        return NDIS_STATUS_RESOURCES;
    }

    NdisGetFirstBufferFromPacket(pkt, &b, &va, &blen, &total);
    if (total > ETH_MAX || total < ETH_HDR) {
        a->xmit_err++;
        return NDIS_STATUS_INVALID_PACKET;
    }
    dst = a->txb + a->tx_head * BUF_SIZE;
    while (b != NULL) {
        NdisQueryBuffer(b, &va, &blen);
        if (blen > ETH_MAX - len)
            blen = ETH_MAX - len;
        if (blen) {
            mem_copy(dst + len, va, blen);
            len += blen;
        }
        b = b->Next;
    }
    if (len < ETH_MIN) {
        mem_zero(dst + len, ETH_MIN - len);
        len = ETH_MIN;
    }

    d = &a->txd[a->tx_head];
    d->len = (USHORT)len;
    d->buf = a->txb_pa + a->tx_head * BUF_SIZE;
    d->status = DESC_OWN;               /* hand over last */
    WR(a, R_TX_POLL, TX_POLL_START);
    a->tx_head = (a->tx_head + 1) % TX_RING;
    a->tx_busy++;
    return NDIS_STATUS_SUCCESS;         /* data copied: nothing pending */
}

static NDIS_STATUS __stdcall R6040TransferData(PUINT transferred,
        PNDIS_PACKET pkt, NDIS_HANDLE ctx, NDIS_HANDLE rx_ctx,
        UINT offset, UINT want)
{
    R6040 *a = (R6040 *)ctx;
    UINT idx = (UINT)rx_ctx - 1, avail, done = 0;
    PUCHAR src;
    PNDIS_BUFFER b;
    PVOID va;
    UINT blen, total;

    *transferred = 0;
    if (idx >= RX_RING || a->rx_len[idx] < ETH_HDR)
        return NDIS_STATUS_FAILURE;
    avail = a->rx_len[idx] - ETH_HDR;
    if (offset >= avail)
        return NDIS_STATUS_SUCCESS;
    avail -= offset;
    if (want > avail)
        want = avail;
    src = a->rxb + idx * BUF_SIZE + ETH_HDR + offset;

    NdisGetFirstBufferFromPacket(pkt, &b, &va, &blen, &total);
    while (b != NULL && done < want) {
        NdisQueryBuffer(b, &va, &blen);
        if (blen > want - done)
            blen = want - done;
        mem_copy(va, src + done, blen);
        done += blen;
        b = b->Next;
    }
    *transferred = done;
    return NDIS_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* query / set                                                         */

static const ULONG supported_oids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE, OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID, OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_DRIVER_VERSION, OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_PROTOCOL_OPTIONS,
    OID_GEN_MAC_OPTIONS, OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_XMIT_OK, OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER,
    OID_GEN_RCV_CRC_ERROR, OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION, OID_802_3_XMIT_MORE_COLLISIONS,
    OID_802_3_RCV_OVERRUN, OID_802_3_XMIT_UNDERRUN,
    OID_802_3_XMIT_LATE_COLLISIONS
};

static const char vendor_desc[] =
    "RDC R6040 Fast Ethernet (open R6040.SYS " R6040_VERSION_STR ")";

static NDIS_STATUS __stdcall R6040QueryInformation(NDIS_HANDLE ctx,
        NDIS_OID oid, PVOID buf, ULONG buflen, PULONG written, PULONG needed)
{
    R6040 *a = (R6040 *)ctx;
    ULONG v = 0;
    const void *src = &v;
    ULONG len = sizeof(ULONG);

    switch (oid) {
    case OID_GEN_SUPPORTED_LIST:
        src = supported_oids;
        len = sizeof(supported_oids);
        break;
    case OID_GEN_HARDWARE_STATUS:   v = NdisHardwareStatusReady;    break;
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:      v = NdisMedium802_3;            break;
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_MAXIMUM_FRAME_SIZE: v = ETH_MAX - ETH_HDR;         break;
    case OID_GEN_CURRENT_LOOKAHEAD: v = a->lookahead;               break;
    case OID_GEN_LINK_SPEED:        /* units of 100 bps */
        v = (a->link_up && !a->speed100) ? 100000UL : 1000000UL;
        break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE: v = TX_RING * BUF_SIZE;     break;
    case OID_GEN_RECEIVE_BUFFER_SPACE:  v = RX_RING * BUF_SIZE;     break;
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
    case OID_GEN_MAXIMUM_TOTAL_SIZE:    v = ETH_MAX;                break;
    case OID_GEN_VENDOR_ID:
        v = a->perm_mac[0] | (a->perm_mac[1] << 8) |
            ((ULONG)a->perm_mac[2] << 16);
        break;
    case OID_GEN_VENDOR_DESCRIPTION:
        src = vendor_desc;
        len = sizeof(vendor_desc);
        break;
    case OID_GEN_CURRENT_PACKET_FILTER: v = a->filter;              break;
    case OID_GEN_DRIVER_VERSION:
        v = 0x0300;
        len = 2;
        break;
    case OID_GEN_MAC_OPTIONS:
        v = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
            NDIS_MAC_OPTION_RECEIVE_SERIALIZED |
            NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
            NDIS_MAC_OPTION_NO_LOOPBACK;
        break;
    case OID_GEN_MEDIA_CONNECT_STATUS:
        v = a->link_up ? NdisMediaStateConnected : NdisMediaStateDisconnected;
        break;
    case OID_GEN_MAXIMUM_SEND_PACKETS:  v = 1;                      break;
    case OID_GEN_XMIT_OK:               v = a->xmit_ok;             break;
    case OID_GEN_RCV_OK:                v = a->rcv_ok;              break;
    case OID_GEN_XMIT_ERROR:            v = a->xmit_err;            break;
    case OID_GEN_RCV_ERROR:             v = a->rcv_err;             break;
    case OID_GEN_RCV_NO_BUFFER:         v = a->rcv_missed;          break;
    case OID_GEN_RCV_CRC_ERROR:         v = a->rcv_crc;             break;
    case OID_802_3_PERMANENT_ADDRESS:
        src = a->perm_mac;
        len = 6;
        break;
    case OID_802_3_CURRENT_ADDRESS:
        src = a->cur_mac;
        len = 6;
        break;
    case OID_802_3_MULTICAST_LIST:
        src = a->mc;
        len = a->mc_count * 6;
        break;
    case OID_802_3_MAXIMUM_LIST_SIZE:   v = MCAST_MAX;              break;
    case OID_802_3_XMIT_UNDERRUN:       v = a->xmit_underrun;       break;
    case OID_802_3_XMIT_LATE_COLLISIONS: v = a->xmit_latecol;       break;
    case OID_802_3_RCV_OVERRUN:         v = a->rcv_overrun;         break;
    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS: v = 0;                     break;
    default:
        *written = 0;
        *needed = 0;
        return NDIS_STATUS_INVALID_OID;
    }
    if (len > buflen) {
        *written = 0;
        *needed = len;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    mem_copy(buf, src, len);
    *written = len;
    *needed = 0;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS __stdcall R6040SetInformation(NDIS_HANDLE ctx,
        NDIS_OID oid, PVOID buf, ULONG buflen, PULONG read, PULONG needed)
{
    R6040 *a = (R6040 *)ctx;
    ULONG v;

    *read = 0;
    *needed = 0;
    switch (oid) {
    case OID_GEN_CURRENT_PACKET_FILTER:
        if (buflen < 4) {
            *needed = 4;
            return NDIS_STATUS_INVALID_LENGTH;
        }
        mem_copy(&v, buf, 4);
        if (v & ~(ULONG)(NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST |
                         NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST |
                         NDIS_PACKET_TYPE_PROMISCUOUS))
            return NDIS_STATUS_NOT_SUPPORTED;
        a->filter = v;
        set_rx_filter(a);
        *read = 4;
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_CURRENT_LOOKAHEAD:
        if (buflen < 4) {
            *needed = 4;
            return NDIS_STATUS_INVALID_LENGTH;
        }
        mem_copy(&v, buf, 4);
        if (v > ETH_MAX - ETH_HDR)
            return NDIS_STATUS_INVALID_DATA;
        a->lookahead = v;
        *read = 4;
        return NDIS_STATUS_SUCCESS;

    case OID_802_3_MULTICAST_LIST:
        if (buflen % 6)
            return NDIS_STATUS_INVALID_LENGTH;
        if (buflen / 6 > MCAST_MAX)
            return NDIS_STATUS_MULTICAST_FULL;
        mem_copy(a->mc, buf, buflen);
        a->mc_count = buflen / 6;
        set_rx_filter(a);
        *read = buflen;
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_PROTOCOL_OPTIONS:
        *read = buflen;
        return NDIS_STATUS_SUCCESS;
    }
    return NDIS_STATUS_INVALID_OID;
}

/* ------------------------------------------------------------------ */
/* check for hang / reset                                              */

static BOOLEAN __stdcall R6040CheckForHang(NDIS_HANDLE ctx)
{
    R6040 *a = (R6040 *)ctx;

    if (a->halting)
        return FALSE;
    a->ticks++;
    hw_counters(a);
    link_update(a);
    link_indicate(a);
    if (a->tx_busy)
        tx_reclaim(a);
    if (a->tx_need_resources && a->tx_busy < TX_RING) {
        a->tx_need_resources = 0;       /* in case the TX_DONE IRQ was lost */
        a->mier = base_mier(a);
        if (!a->dpc_queued)
            WR(a, R_MIER, a->mier);
        NdisMSendResourcesAvailable(a->mh);
    }
    if (a->tx_busy && a->tx_tail == a->tx_tail_at_check) {
        a->tx_stale++;
        if (a->tx_kick && a->link_up) {
            WR(a, R_TX_POLL, TX_POLL_START);
            a->tx_kicks++;
        }
    }
    dbg_regs_write(a);
    /* without link the MAC may hold frames: that is not a hang */
    if (a->link_up && a->tx_busy && a->tx_tail == a->tx_tail_at_check) {
        if (++a->tx_stall_ticks >= 3) {     /* ~6 s without TX progress */
            diag_warn(a, WARN_TX_HANG);
            return TRUE;                    /* NDIS calls Reset */
        }
    } else {
        a->tx_stall_ticks = 0;
    }
    a->tx_tail_at_check = a->tx_tail;
    return FALSE;
}

static NDIS_STATUS __stdcall R6040Reset(PBOOLEAN addressing_reset,
                                        NDIS_HANDLE ctx)
{
    R6040 *a = (R6040 *)ctx;

    diag_cp(a, CP_RESET_ENTER);
    diag_ev(a, EV_RESET);
    /* nothing is pending by design: just restart the chip */
    hw_init(a);
    link_update(a);
    if (!a->halting)
        WR(a, R_MIER, a->mier);
    *addressing_reset = FALSE;          /* filter and list were reprogrammed */
    diag_cp(a, CP_RESET_DONE);
    return NDIS_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* shutdown / halt                                                     */

/* System shutdown: quiesce DMA only.  No NDIS calls, no frees, no locks. */
static VOID __stdcall R6040Shutdown(PVOID ctx)
{
    R6040 *a = (R6040 *)ctx;

    diag_cp(a, CP_SHUT_ENTER);
    diag_ev(a, EV_SHUT_ENTER);
    WR(a, R_MIER, 0);
    mac_stop(a);
    mac_reset(a);
    RD(a, R_MISR);
    diag_cp(a, CP_SHUT_DONE);
    diag_ev(a, EV_SHUT_DONE);
}

static void free_resources(R6040 *a)
{
    UINT i;

    for (i = 0; i < 3; i++)
        if (a->sh_va[i] != NULL) {
            NdisMFreeSharedMemory(a->mh, a->sh_len[i], FALSE, a->sh_va[i],
                                  a->sh_pa[i]);
            a->sh_va[i] = NULL;
        }
    if (a->io_registered) {
        NdisMDeregisterIoPortRange(a->mh, a->io, R6040_IO_SIZE, a->io_off);
        a->io_registered = 0;
    }
}

static VOID __stdcall R6040Halt(NDIS_HANDLE ctx)
{
    R6040 *a = (R6040 *)ctx;

    diag_cp(a, CP_HALT_ENTER);
    diag_ev(a, EV_HALT_ENTER);
    a->halting = 1;
    WR(a, R_MIER, 0);
    diag_cp(a, CP_HALT_MASKED);
    mac_stop(a);
    mac_reset(a);                       /* DMA engines idle from here on */
    RD(a, R_MISR);
    diag_cp(a, CP_HALT_MACSTOP);

    /* Sends were completed synchronously and receives were indicated with
     * copy semantics, so there is nothing to complete or wait for. */
    if (a->dpc_queued)
        diag_warn(a, WARN_DPC_AT_HALT);
    if (a->intr_registered) {
        diag_cp(a, CP_HALT_DEREG_IRQ);
        NdisMDeregisterInterrupt(&a->intr);
        a->intr_registered = 0;
        diag_cp(a, CP_HALT_IRQ_DONE);
    }
    if (a->shutdown_registered) {
        diag_cp(a, CP_HALT_DEREG_SHUT);
        NdisMDeregisterAdapterShutdownHandler(a->mh);
        a->shutdown_registered = 0;
    }
    free_resources(a);
    diag_cp(a, CP_HALT_FREED);
    diag_cp(a, CP_HALT_DONE);
    diag_ev(a, EV_HALT_DONE);
    NdisFreeMemory(a, sizeof(R6040), 0);
}

/* ------------------------------------------------------------------ */
/* initialize                                                          */

static ULONG read_param(NDIS_HANDLE cfg, const WCHAR *name, USHORT bytes,
                        ULONG def)
{
    NDIS_STATUS st;
    PNDIS_CONFIGURATION_PARAMETER p;
    NDIS_STRING s;

    s.Length = bytes;
    s.MaximumLength = (USHORT)(bytes + 2);
    s.Buffer = (WCHAR *)name;
    NdisReadConfiguration(&st, &p, cfg, &s, NdisParameterInteger);
    if (st == NDIS_STATUS_SUCCESS && p != NULL)
        return p->ParameterData.IntegerData;
    return def;
}

/* Record the running build in the adapter's registry key (value
 * R6040Version), so it can be checked even when the file was swapped in
 * under RDC's entry and Device Manager still shows RDC's name. */
static void write_string(NDIS_HANDLE cfg, const WCHAR *key, USHORT key_bytes,
                         const WCHAR *val)
{
    NDIS_CONFIGURATION_PARAMETER v;
    NDIS_STRING k;
    NDIS_STATUS st;
    USHORT n = 0;

    while (val[n])
        n++;
    k.Length = key_bytes;
    k.MaximumLength = (USHORT)(key_bytes + 2);
    k.Buffer = (WCHAR *)key;
    v.ParameterType = NdisParameterString;
    v.ParameterData.StringData.Length = (USHORT)(2 * n);
    v.ParameterData.StringData.MaximumLength = (USHORT)(2 * n + 2);
    v.ParameterData.StringData.Buffer = (WCHAR *)val;
    NdisWriteConfiguration(&st, cfg, &k, &v);
}
#define WRITE_STRING(cfg, lit, val) \
    write_string(cfg, (const WCHAR *)(lit), (USHORT)(sizeof(lit) - 2), val)

/* R6040Version (the running build: swapped installs keep RDC's name in
 * Device Manager), R6040PhyBoot (PHY as the BIOS left it) and R6040Link
 * (negotiated result at init) in the adapter's registry key. */
static void write_status(R6040 *a, NDIS_HANDLE wcc)
{
    static const WCHAR ver[] = R6040_VERSION_WSTR;
    NDIS_HANDLE cfg;
    NDIS_STATUS st;

    NdisOpenConfiguration(&st, &cfg, wcc);
    if (st != NDIS_STATUS_SUCCESS)
        return;
    WRITE_STRING(cfg, L"R6040Version", ver);
    WRITE_STRING(cfg, L"R6040PhyBoot", a->phy_boot);
    WRITE_STRING(cfg, L"R6040Link", a->phy_link);
    WRITE_STRING(cfg, L"R6040RegsBoot", a->regs_boot);
    WRITE_STRING(cfg, L"R6040Regs", a->regs_init);
    NdisCloseConfiguration(cfg);
}

#define PARAM(cfg, lit, def) \
    read_param(cfg, (const WCHAR *)(lit), (USHORT)(sizeof(lit) - 2), def)

static int find_resources(R6040 *a, NDIS_HANDLE wcc)
{
    PNDIS_RESOURCE_LIST rl = NULL;
    ULONG local[64];
    UINT i, size;
    NDIS_STATUS st;
    int have_io = 0, have_irq = 0;

    if (NdisMPciAssignResources(a->mh, 0, &rl) != NDIS_STATUS_SUCCESS)
        rl = NULL;
    if (rl == NULL) {
        size = sizeof(local);
        NdisMQueryAdapterResources(&st, wcc, (PNDIS_RESOURCE_LIST)local, &size);
        if (st != NDIS_STATUS_SUCCESS)
            return 0;
        rl = (PNDIS_RESOURCE_LIST)local;
    }
    for (i = 0; i < rl->Count && i < 16; i++) {
        CM_PARTIAL_RESOURCE_DESCRIPTOR *d = &rl->PartialDescriptors[i];
        if (d->Type == CmResourceTypePort && !have_io &&
            d->u.Port.Length >= R6040_IO_SIZE && d->u.Port.StartLow < 0x10000UL) {
            a->io = (USHORT)d->u.Port.StartLow;
            have_io = 1;
        } else if (d->Type == CmResourceTypeInterrupt && !have_irq) {
            a->irq_level = d->u.Interrupt.Level;
            a->irq_vector = d->u.Interrupt.Vector;
            have_irq = 1;
        }
    }
    return have_io && have_irq;
}

static int alloc_shared(R6040 *a, UINT i, ULONG len)
{
    a->sh_len[i] = len;
    a->sh_va[i] = NULL;
    NdisMAllocateSharedMemory(a->mh, len, FALSE, &a->sh_va[i], &a->sh_pa[i]);
    if (a->sh_va[i] == NULL || a->sh_pa[i].HighPart != 0) {
        a->sh_va[i] = NULL;
        return 0;
    }
    mem_zero(a->sh_va[i], len);
    return 1;
}

static int mac_valid(const UCHAR *m)
{
    UINT i, zero = 1, ones = 1;
    for (i = 0; i < 6; i++) {
        if (m[i] != 0)    zero = 0;
        if (m[i] != 0xFF) ones = 0;
    }
    return !zero && !ones && !(m[0] & 1);
}

static NDIS_STATUS __stdcall R6040Initialize(PNDIS_STATUS open_error,
        PUINT selected, UINT *media, UINT media_count,
        NDIS_HANDLE mh, NDIS_HANDLE wcc)
{
    static const NDIS_PHYSICAL_ADDRESS any = { 0xFFFFFFFFUL, -1 };
    R6040 *a;
    NDIS_HANDLE cfg;
    NDIS_STATUS st;
    UINT i, step;
    ULONG pciid = 0, ofs, cmd;
    USHORT w;

    *open_error = NDIS_STATUS_SUCCESS;
    for (i = 0; i < media_count; i++)
        if (media[i] == NdisMedium802_3)
            break;
    if (i == media_count)
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    *selected = i;

    if (NdisAllocateMemory((PVOID *)&a, sizeof(R6040), 0, any) != NDIS_STATUS_SUCCESS)
        return NDIS_STATUS_RESOURCES;
    mem_zero(a, sizeof(R6040));
    a->mh = mh;
    a->phy = 32;
    a->lookahead = ETH_MAX - ETH_HDR;

    /* registry: diagnostics first, so the following steps are recorded */
    a->diag = 1;
    a->diag_cmos = 1;
    a->use_shutdown = 1;
    a->media_sense = 1;
    a->an_restart = 1;
    a->an_at_init = 1;
    a->phy_poll = 1;
    a->tx_kick = 1;
    a->dbg_regs = 1;
    NdisOpenConfiguration(&st, &cfg, wcc);
    if (st == NDIS_STATUS_SUCCESS) {
        a->diag = PARAM(cfg, L"R6040Diag", 1);
        a->diag_cmos = PARAM(cfg, L"R6040DiagCmos", 1);
        a->use_shutdown = PARAM(cfg, L"R6040Shutdown", 1);
        a->conn_type = PARAM(cfg, L"ConnectionType", 0);
        a->media_sense = PARAM(cfg, L"R6040MediaSense", 1);
        a->phy_tweak = PARAM(cfg, L"R6040PhyTweak", 0);
        a->an_restart = PARAM(cfg, L"R6040AnRestart", 1);
        a->an_at_init = PARAM(cfg, L"R6040AnAtInit", 1);
        a->phy_poll = PARAM(cfg, L"R6040PhyPoll", 1);
        a->tx_kick = PARAM(cfg, L"R6040TxKick", 1);
        a->dbg_regs = PARAM(cfg, L"R6040DbgRegs", 1);
        NdisCloseConfiguration(cfg);
    }
    /* index, index+2, index+4 must avoid the RTC registers 0Ah-0Dh and the
     * checksummed range 10h-2Fh: allow 1 (alarm bytes 1/3/5) or 34h-7Bh */
    if (a->diag_cmos != 1 && (a->diag_cmos < 0x34 || a->diag_cmos > 0x7B))
        a->diag_cmos = 1;
    if (a->conn_type > 4)
        a->conn_type = 0;

    /* fresh record for this boot: the reader saved the previous one */
    if (a->diag) {
        cmos_write((UCHAR)(a->diag_cmos + 2), EV_DRIVER_LOADED);
        cmos_write((UCHAR)(a->diag_cmos + 4), 0);
    } else {
        /* diagnostics off: remove DriverEntry's record, write nothing more */
        cmos_write(1, 0);
        cmos_write(3, 0);
        cmos_write(5, 0);
    }
    diag_cp(a, CP_INIT_ENTER);

    step = 1;
    diag_cp(a, CP_INIT_CONFIG);
    NdisMSetAttributes(mh, a, TRUE, NdisInterfacePci);

    step = 2;
    diag_cp(a, CP_INIT_RESOURCES);
    if (!find_resources(a, wcc)) {
        st = NDIS_STATUS_ADAPTER_NOT_FOUND;
        goto fail;
    }
    if (NdisReadPciSlotInformation(mh, 0, 0, &pciid, 4) == 4 &&
        pciid != R6040_PCI_ID) {
        st = NDIS_STATUS_ADAPTER_NOT_FOUND;
        goto fail;
    }
    /* make sure I/O decoding and bus mastering are on */
    if (NdisReadPciSlotInformation(mh, 0, 4, &cmd, 4) == 4 &&
        (cmd & 0x0005) != 0x0005) {
        cmd = (cmd & 0xFFFF) | 0x0005;
        NdisWritePciSlotInformation(mh, 0, 4, &cmd, 2);
    }

    step = 3;
    diag_cp(a, CP_INIT_IO);
    if (NdisMRegisterIoPortRange(&a->io_off, mh, a->io, R6040_IO_SIZE)
        != NDIS_STATUS_SUCCESS) {
        st = NDIS_STATUS_RESOURCES;
        goto fail;
    }
    a->io_registered = 1;

    /* station address as the BIOS programmed it, before any reset */
    for (i = 0; i < 3; i++) {
        w = RD(a, R_MID0L + 2 * i);
        a->perm_mac[2 * i] = (UCHAR)w;
        a->perm_mac[2 * i + 1] = (UCHAR)(w >> 8);
    }
    if (!mac_valid(a->perm_mac)) {
        /* locally administered fallback, stable per I/O base */
        diag_warn(a, WARN_BAD_MAC);
        a->perm_mac[0] = 0x02; a->perm_mac[1] = 0x17; a->perm_mac[2] = 0xF3;
        a->perm_mac[3] = 0x60; a->perm_mac[4] = (UCHAR)(a->io >> 8);
        a->perm_mac[5] = (UCHAR)a->io;
    }
    mem_copy(a->cur_mac, a->perm_mac, 6);
    a->mdcsc = RD(a, R_MDCSC);
    a->mpsccr_bios = RD(a, R_MPSCCR);
    regs_snapshot(a, a->regs_boot);

    step = 4;
    diag_cp(a, CP_INIT_SHMEM);
    if (!alloc_shared(a, 0, (TX_RING + RX_RING) * sizeof(R6040_DESC) + 16) ||
        !alloc_shared(a, 1, TX_RING * BUF_SIZE) ||
        !alloc_shared(a, 2, RX_RING * BUF_SIZE)) {
        st = NDIS_STATUS_RESOURCES;
        goto fail;
    }
    ofs = (16 - (a->sh_pa[0].LowPart & 15)) & 15;
    a->txd = (R6040_DESC *)((PUCHAR)a->sh_va[0] + ofs);
    a->txd_pa = a->sh_pa[0].LowPart + ofs;
    a->rxd = a->txd + TX_RING;
    a->rxd_pa = a->txd_pa + TX_RING * sizeof(R6040_DESC);
    a->txb = (PUCHAR)a->sh_va[1];
    a->txb_pa = a->sh_pa[1].LowPart;
    a->rxb = (PUCHAR)a->sh_va[2];
    a->rxb_pa = a->sh_pa[2].LowPart;

    step = 5;
    diag_cp(a, CP_INIT_HWRESET);
    hw_init(a);

    step = 6;
    diag_cp(a, CP_INIT_PHY);
    phy_find(a);
    if (a->phy_poll && a->mpsccr_bios == 0 && a->phy <= 31) {
        /* BIOS left the poller off: enable it for our PHY, slowest timer */
        a->mpsccr_bios = (USHORT)(0x8000 | (a->phy << MPSCCR_PHY_ADDR_SHIFT) | 7);
        WR(a, R_MPSCCR, a->mpsccr_bios);
    }
    phy_snapshot(a, a->phy_boot);
    if (a->phy_tweak)
        phy_vendor_tweak(a);
    phy_setup(a);
    link_update(a);
    a->link_reported = a->link_up;      /* no indications during Initialize */
    {
        UINT n = phy_snapshot(a, a->phy_link);
        if (a->link_up) {
            n = wput(a->phy_link, n, a->speed100 ? " -> 100 " : " -> 10 ");
            n = wput(a->phy_link, n, a->full_duplex ? "full" : "half");
        } else {
            n = wput(a->phy_link, n, " -> no link");
        }
        if (a->conn_type)
            n = wput(a->phy_link, n, " (forced)");
        n = wput(a->phy_link, n, " an_wait=");
        n = wdec(a->phy_link, n, a->an_wait_ms);
        wput(a->phy_link, n, "ms");
    }
    if (a->phy_poll && a->link_up) {
        /* give the MAC's poller time to latch the link (bounded, 1 s) */
        for (a->mpsr_wait_ms = 0; a->mpsr_wait_ms < 1000; a->mpsr_wait_ms += 10) {
            if (RD(a, R_MPSR) & MPSR_LINK_UP)
                break;
            udelay(10000);
        }
        n_mpsr_note(a);
    }
    regs_snapshot(a, a->regs_init);
    write_status(a, wcc);

    step = 7;
    diag_cp(a, CP_INIT_IRQ);
    st = NdisMRegisterInterrupt(&a->intr, mh, a->irq_vector, a->irq_level,
                                TRUE, TRUE, NdisInterruptLevelSensitive);
    if (st != NDIS_STATUS_SUCCESS)
        goto fail;
    a->intr_registered = 1;

    if (a->use_shutdown) {
        NdisMRegisterAdapterShutdownHandler(mh, a, R6040Shutdown);
        a->shutdown_registered = 1;
    }

    RD(a, R_MISR);
    a->running = 1;
    WR(a, R_MIER, a->mier);
    diag_cp(a, CP_INIT_OK);
    diag_ev(a, EV_INIT_OK);
    return NDIS_STATUS_SUCCESS;

fail:
    diag_cp(a, (UCHAR)(CP_INIT_FAIL + step));
    diag_ev(a, EV_INIT_FAIL);
    if (a->io_registered) {
        WR(a, R_MIER, 0);
        mac_stop(a);
        mac_reset(a);
    }
    free_resources(a);
    NdisFreeMemory(a, sizeof(R6040), 0);
    return st;
}

/* ------------------------------------------------------------------ */
/* driver entry                                                        */

/*
 * The DriverEntry checkpoints are always written to the default CMOS index
 * (the registry is not readable yet): they tell a PE-load/relocation failure
 * (record still 00) from a DriverEntry failure (1x).
 */
NDIS_STATUS __stdcall DriverEntry(PVOID driver_object, PVOID registry_path)
{
    NDIS_MINIPORT_CHARACTERISTICS c;
    NDIS_STATUS st;

    cmos_write(1, CP_ENTRY);
    cmos_write(3, EV_DRIVER_LOADED);
    cmos_write(5, 0);
    NdisInitializeWrapper(&wrapper_handle, driver_object, registry_path, NULL);
    cmos_write(1, CP_ENTRY_WRAPPER);
    if (wrapper_handle == NULL) {
        cmos_write(1, CP_ENTRY_FAIL);
        return NDIS_STATUS_FAILURE;
    }

    mem_zero(&c, sizeof(c));
    c.MajorNdisVersion = 3;
    c.MinorNdisVersion = 0;
    c.CheckForHangHandler = R6040CheckForHang;
    c.DisableInterruptHandler = R6040DisableInterrupt;
    c.EnableInterruptHandler = R6040EnableInterrupt;
    c.HaltHandler = R6040Halt;
    c.HandleInterruptHandler = R6040HandleInterrupt;
    c.InitializeHandler = R6040Initialize;
    c.ISRHandler = R6040Isr;
    c.QueryInformationHandler = R6040QueryInformation;
    c.ReconfigureHandler = NULL;
    c.ResetHandler = R6040Reset;
    c.SendHandler = R6040Send;
    c.SetInformationHandler = R6040SetInformation;
    c.TransferDataHandler = R6040TransferData;

    st = NdisMRegisterMiniport(wrapper_handle, &c, sizeof(c));
    if (st != NDIS_STATUS_SUCCESS) {
        cmos_write(1, CP_ENTRY_FAIL);
        NdisTerminateWrapper(wrapper_handle, NULL);
    } else {
        cmos_write(1, CP_ENTRY_OK);
    }
    return st;
}
