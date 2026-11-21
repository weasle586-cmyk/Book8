#ifndef BOOK8_AHCI_H
#define BOOK8_AHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AHCI 1.3.1 register layout. All structures are MMIO: access through
 * volatile pointers only. */

/* GHC bits */
#define AHCI_GHC_HR   (1u << 0)
#define AHCI_GHC_IE   (1u << 1)
#define AHCI_GHC_AE   (1u << 31)

/* CAP bits */
#define AHCI_CAP_NP(cap)   ((cap) & 0x1fu)          /* ports - 1 */
#define AHCI_CAP_NCS(cap)  (((cap) >> 8) & 0x1fu)   /* slots - 1 */
#define AHCI_CAP_S64A      (1u << 31)
#define AHCI_CAP2_BOH      (1u << 0)
#define AHCI_BOHC_BOS      (1u << 0)   /* BIOS owned */
#define AHCI_BOHC_OOS      (1u << 1)   /* OS owned */
#define AHCI_BOHC_BB       (1u << 4)   /* BIOS busy */

#define AHCI_PIS_TFES      (1u << 30)  /* task file error status */

/* Port CMD bits */
#define AHCI_PCMD_ST   (1u << 0)
#define AHCI_PCMD_SUD  (1u << 1)
#define AHCI_PCMD_POD  (1u << 2)
#define AHCI_PCMD_FRE  (1u << 4)
#define AHCI_PCMD_FR   (1u << 14)
#define AHCI_PCMD_CR   (1u << 15)

/* Port TFD bits */
#define AHCI_TFD_ERR   (1u << 0)
#define AHCI_TFD_DRQ   (1u << 3)
#define AHCI_TFD_BSY   (1u << 7)

/* Port SSTS fields */
#define AHCI_SSTS_DET(s)  ((s) & 0xfu)
#define AHCI_SSTS_IPM(s)  (((s) >> 8) & 0xfu)
#define AHCI_DET_PRESENT  3u   /* device present, PHY established */
#define AHCI_IPM_ACTIVE   1u

/* Port signatures */
#define AHCI_SIG_ATA    0x00000101u
#define AHCI_SIG_ATAPI  0xeb140101u
#define AHCI_SIG_SEMB   0xc33c0101u
#define AHCI_SIG_PM     0x96690101u

struct ahci_port_regs {
    uint32_t clb;        /* 0x00 command list base (1 KiB aligned) */
    uint32_t clbu;       /* 0x04 upper 32 bits */
    uint32_t fb;         /* 0x08 FIS base (256 B aligned) */
    uint32_t fbu;        /* 0x0c */
    uint32_t is;         /* 0x10 interrupt status (RWC) */
    uint32_t ie;         /* 0x14 interrupt enable */
    uint32_t cmd;        /* 0x18 command and status */
    uint32_t reserved0;  /* 0x1c */
    uint32_t tfd;        /* 0x20 task file data */
    uint32_t sig;        /* 0x24 signature */
    uint32_t ssts;       /* 0x28 SATA status (SCR0) */
    uint32_t sctl;       /* 0x2c SATA control (SCR2) */
    uint32_t serr;       /* 0x30 SATA error (SCR1, RWC) */
    uint32_t sact;       /* 0x34 SATA active (SCR3) */
    uint32_t ci;         /* 0x38 command issue */
    uint32_t sntf;       /* 0x3c SATA notification (SCR4) */
    uint32_t fbs;        /* 0x40 FIS-based switching control */
    uint32_t devslp;     /* 0x44 device sleep */
    uint32_t reserved1[10]; /* 0x48..0x6f */
    uint32_t vendor[4];  /* 0x70..0x7f */
};

struct ahci_hba_regs {
    uint32_t cap;        /* 0x00 host capabilities */
    uint32_t ghc;        /* 0x04 global host control */
    uint32_t is;         /* 0x08 interrupt status (RWC) */
    uint32_t pi;         /* 0x0c ports implemented */
    uint32_t vs;         /* 0x10 version */
    uint32_t ccc_ctl;    /* 0x14 command completion coalescing control */
    uint32_t ccc_ports;  /* 0x18 CCC ports */
    uint32_t em_loc;     /* 0x1c enclosure management location */
    uint32_t em_ctl;     /* 0x20 enclosure management control */
    uint32_t cap2;       /* 0x24 extended capabilities */
    uint32_t bohc;       /* 0x28 BIOS/OS handoff control */
    uint8_t  reserved[0x60 - 0x2c];
    uint8_t  vendor[0x100 - 0x60];
    struct ahci_port_regs ports[32]; /* 0x100 + 0x80 * n */
};

_Static_assert(offsetof(struct ahci_hba_regs, bohc) == 0x28, "bohc offset");
_Static_assert(offsetof(struct ahci_hba_regs, ports) == 0x100, "ports offset");
_Static_assert(sizeof(struct ahci_port_regs) == 0x80, "port regs size");
_Static_assert(offsetof(struct ahci_port_regs, fbs) == 0x40, "fbs offset");
_Static_assert(sizeof(struct ahci_hba_regs) == 0x1100, "hba regs size");

/* In-memory structures handed to the HBA. */

struct ahci_cmd_header {          /* 32 bytes */
    uint16_t flags;               /* CFL[4:0], A, W, P, R, B, C, PMP[15:12] */
    uint16_t prdtl;               /* PRD entry count */
    uint32_t prdbc;               /* bytes transferred (written by HBA) */
    uint32_t ctba;                /* command table base (128 B aligned) */
    uint32_t ctbau;
    uint32_t reserved[4];
};

struct ahci_prd {                 /* 16 bytes */
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;                 /* byte count - 1 in [21:0], I in bit 31 */
};

#define AHCI_PRDT_ENTRIES 8u

struct ahci_cmd_table {           /* 128 + 8 * 16 = 256 bytes */
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    struct ahci_prd prdt[AHCI_PRDT_ENTRIES];
};

struct ahci_rfis {                /* 256 bytes, received FIS area */
    uint8_t dsfis[0x1c];  uint8_t pad0[4];
    uint8_t psfis[0x14];  uint8_t pad1[12];
    uint8_t rfis[0x14];   uint8_t pad2[4];
    uint8_t sdbfis[8];
    uint8_t ufis[64];
    uint8_t reserved[0x60];
};

_Static_assert(sizeof(struct ahci_cmd_header) == 32, "cmd header size");
_Static_assert(sizeof(struct ahci_prd) == 16, "prd size");
_Static_assert(sizeof(struct ahci_cmd_table) == 256, "cmd table size");
_Static_assert(sizeof(struct ahci_rfis) == 256, "rfis size");

#define AHCI_CMD_SLOTS 32u

/* Per-port DMA memory. Alignment: CL 1 KiB, RFIS 256 B, CT 128 B. */
struct ahci_port_mem {
    struct ahci_cmd_header cl[AHCI_CMD_SLOTS];
    struct ahci_rfis rfis;
    struct ahci_cmd_table ct[AHCI_CMD_SLOTS];
} __attribute__((aligned(1024)));

#define AHCI_MAX_PORTS 32u

/* Register Host-to-Device FIS (20 bytes) */
struct ahci_fis_h2d {
    uint8_t type;        /* 0x27 */
    uint8_t flags;       /* bit 7 = C (command register update) */
    uint8_t command;
    uint8_t feature_low;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t feature_high;
    uint8_t count_low, count_high;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
};

_Static_assert(sizeof(struct ahci_fis_h2d) == 20, "h2d fis size");

#define AHCI_FIS_TYPE_H2D  0x27u
#define AHCI_CMD_FLAG_WRITE (1u << 6)
#define AHCI_CMD_FIS_DWORDS 5u

struct ahci_port {
    volatile struct ahci_port_regs *regs;
    struct ahci_port_mem *mem;
    uint32_t signature;
    uint8_t index;
    bool present;                 /* link up with a device attached */
    bool s64a;                    /* HBA accepts 64-bit DMA addresses */
};

struct ahci_hba {
    volatile struct ahci_hba_regs *regs;
    uint32_t cap;
    uint32_t ports_implemented;
    uint32_t version;
    uint8_t slots;                /* command slots per port */
    uint8_t port_count;           /* ports initialised below */
    struct ahci_port ports[AHCI_MAX_PORTS];
};

enum ahci_status {
    AHCI_OK = 0,
    AHCI_ERR_ARG,
    AHCI_ERR_RESET,       /* GHC.HR never cleared */
    AHCI_ERR_NO_PORTS,    /* PI is zero */
    AHCI_ERR_STOP,        /* command engine did not stop */
    AHCI_ERR_ADDRESS      /* DMA memory above 4 GiB on a 32-bit-only HBA */
};

/* Resets the HBA at `abar` (BAR5 MMIO base, already mapped), enables AHCI
 * mode, reads PI and sets up every implemented port. Ports are left stopped
 * (ST/FRE clear) with CLB/FB programmed. Returns AHCI_OK when at least one
 * port was initialised. */
enum ahci_status ahci_init(struct ahci_hba *hba, void *abar);

/* Stops a port's command engine (clears ST then FRE, polls CR/FR). */
enum ahci_status ahci_port_stop(volatile struct ahci_port_regs *port);

/* Programs CLB/FB and the command table pointers for one stopped port. */
enum ahci_status ahci_port_setup(struct ahci_port *port);

/* Starts the command engine (FRE then ST) once setup is complete. */
void ahci_port_start(volatile struct ahci_port_regs *port);

/* Starts the port, waits for link (DET=3) and a idle task file, then
 * refreshes port->present and port->signature. The D2H signature FIS is only
 * received while FRE is set, so this must run before reading SIG. */
bool ahci_port_probe(struct ahci_port *port);

/* Issues one polled command on slot 0. `bytes` == 0 sends no data; otherwise
 * `data` (identity-mapped) is moved with a single PRD (1..4 MiB). Restarts a
 * stopped engine first. Returns true on clean completion (no TFES, no ERR). */
bool ahci_port_issue(struct ahci_port *port, uint8_t command, uint64_t lba,
    uint16_t count, void *data, uint32_t bytes, bool write);

#endif
