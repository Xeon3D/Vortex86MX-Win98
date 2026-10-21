/*
 * r6040hw.h - RDC R6040 Fast Ethernet MAC (PCI 17F3:6040, Vortex86 SoCs)
 *
 * Register facts as described by the FreeBSD vte(4) driver (if_vtereg.h,
 * BSD-2-Clause, Pyun YongHyeon) and the Linux r6040 driver.  Names follow
 * FreeBSD.  256-byte I/O window, all registers 16 bit.
 *
 * MIT License - see LICENSE.
 */
#ifndef R6040HW_H
#define R6040HW_H

#define R6040_IO_SIZE       0x100
#define R6040_PCI_ID        0x604017F3UL     /* device << 16 | vendor */

#define R_MCR0      0x00    /* MAC control 0 */
#define  MCR0_RX_ENB        0x0002
#define  MCR0_PROMISC       0x0020
#define  MCR0_BROADCAST_DIS 0x0040
#define  MCR0_MULTICAST     0x0100  /* hash table enable */
#define  MCR0_FC_ENB        0x0200
#define  MCR0_TX_ENB        0x1000
#define  MCR0_FULL_DUPLEX   0x8000
#define R_MCR1      0x04    /* MAC control 1 */
#define  MCR1_MAC_RESET     0x0001
#define R_MBCR      0x08    /* bus control */
#define  MBCR_DEFAULT       0x012A  /* value the Linux driver programs */
#define R_MTICR     0x0C    /* TX interrupt moderation */
#define R_MRICR     0x10    /* RX interrupt moderation */
#define R_TX_POLL   0x14
#define  TX_POLL_START      0x0001
#define R_MRBSR     0x18    /* RX buffer size (multiple of 4) */
#define R_MRDCR     0x1A    /* RX descriptor control */
#define R_MLSR      0x1C    /* last status */
#define R_MMDIO     0x20    /* MDIO control */
#define  MMDIO_READ         0x2000
#define  MMDIO_WRITE        0x4000
#define R_MMRD      0x24    /* MDIO read data */
#define R_MMWD      0x28    /* MDIO write data */
#define R_MTDSA0    0x2C    /* TX ring base, low 16 bits */
#define R_MTDSA1    0x30    /* TX ring base, high 16 bits */
#define R_MRDSA0    0x34    /* RX ring base, low */
#define R_MRDSA1    0x38    /* RX ring base, high */
#define R_MISR      0x3C    /* interrupt status, read clears */
#define R_MIER      0x40    /* interrupt enable */
#define  MISR_RX_DONE       0x0001
#define  MISR_RX_DESC_UNAVAIL 0x0002
#define  MISR_RX_FIFO_FULL  0x0004
#define  MISR_RX_EARLY      0x0008
#define  MISR_TX_DONE       0x0010
#define  MISR_TX_EARLY      0x0080
#define  MISR_EVENT_CNT_OFLOW 0x0100
#define  MISR_PHY_MEDIA_CHG 0x0200
#define  R6040_RX_INTS (MISR_RX_DONE | MISR_RX_DESC_UNAVAIL | MISR_RX_FIFO_FULL)
#define  R6040_INTRS   (R6040_RX_INTS | MISR_TX_DONE)   /* all we ever handle */
#define R_MECISR    0x44    /* event counter interrupt status */
#define R_MECIER    0x48    /* event counter interrupt enable */
#define R_CNT_RX_DONE 0x50  /* MAC event counters, reading clears */
#define R_CNT_MECNT0  0x52  /* hi: broadcast  lo: multicast */
#define R_CNT_MECNT1  0x54  /* hi: runt       lo: CRC error */
#define R_CNT_MECNT2  0x56  /*                lo: too long */
#define R_CNT_MECNT3  0x58  /* hi: FIFO full  lo: no descriptor */
#define R_CNT_TX_DONE 0x5A
#define R_CNT_MECNT4  0x5C  /* hi: underrun   lo: late collision */
#define R_CNT_PAUSE   0x5E
#define R_MAR0      0x60    /* multicast hash table, 4 x 16 bit */
#define R_MID0L     0x68    /* station address: L/M/H at +0/+2/+4 */
#define R_MID1L     0x70    /* perfect filter 1..3 at 0x70/0x78/0x80 */
#define R_MPSCCR    0x88    /* PHY status change config (BIOS sets PHY addr) */
#define  MPSCCR_PHY_ADDR_MASK  0x1F00
#define  MPSCCR_PHY_ADDR_SHIFT 8
#define R_MPSR      0x8A    /* PHY status as seen by the MAC */
#define  MPSR_LINK_UP       0x0001
#define  MPSR_SPEED_100     0x0002
#define  MPSR_FULL_DUPLEX   0x0004
#define R_MACSM     0xAC    /* MAC state machine (undocumented) */
#define  MACSM_RESET        0x0002
#define R_MDCSC     0xB6    /* MDC speed control */
#define  MDCSC_DEFAULT      0x0030
#define R_MACID     0xBE

/* Descriptors, linked through 'next'.  The MAC uses the first 18 bytes of
 * an RX descriptor (hash index and two reserved words after 'next'), so each
 * descriptor gets 32 bytes, like the Linux driver, 16-byte aligned. */
typedef struct {
    volatile USHORT status;     /* drst / dtst */
    volatile USHORT len;        /* drlen / dtlen */
    volatile ULONG  buf;        /* physical buffer address */
    volatile ULONG  next;       /* physical address of the next descriptor */
    volatile USHORT hidx;       /* RX: hash index */
    volatile USHORT rsvd[9];
} R6040_DESC;                   /* 32 bytes */

#define DESC_OWN            0x8000  /* owned by the MAC */
#define DRST_RX_OK          0x4000
#define DRST_PHY_ERR        0x0800
#define DRST_DRIBBLE        0x0400
#define DRST_TRUNC          0x0200
#define DRST_LONG           0x0100
#define DRST_RUNT           0x0080
#define DRST_CRC_ERR        0x0040
#define DRST_BROADCAST      0x0020
#define DRST_MULTICAST      0x0010
#define DTST_TX_OK          0x4000
#define DTST_UNDERRUN       0x0040
#define DTST_LATE_COL       0x0020
#define DTST_EXCESS_COL     0x0010
#define DESC_LEN_MASK       0x07FF

/* MII (IEEE 802.3 clause 22) */
#define MII_BMCR            0x00
#define  BMCR_RESET         0x8000
#define  BMCR_SPEED100      0x2000
#define  BMCR_ANEN          0x1000
#define  BMCR_ISOLATE       0x0400
#define  BMCR_RESTART_AN    0x0200
#define  BMCR_FULLDPLX      0x0100
#define MII_BMSR            0x01
#define  BMSR_ANCOMP        0x0020
#define  BMSR_LINK          0x0004
#define MII_PHYID1          0x02
#define MII_ANAR            0x04
#define MII_ANLPAR          0x05
#define  ANAR_100FD         0x0100
#define  ANAR_100HD         0x0080
#define  ANAR_10FD          0x0040
#define  ANAR_10HD          0x0020
#define  ANAR_CSMA          0x0001
#define  ANAR_ALL           (ANAR_100FD | ANAR_100HD | ANAR_10FD | ANAR_10HD | ANAR_CSMA)

#endif
