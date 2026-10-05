/* AHCI host controllers (R3 of docs/plan/release-0.5.0.md, D4 of
 * docs/plan/drivers.md, docs/design/ahci.md).
 *
 * Adapted from MdeModulePkg/Bus/Ata/AtaAtapiPassThru/AhciMode.c and
 * AhciMode.h of edk2, revision 999fd0f12a27709eee04b93e46bd867e6b0163a5
 * (third_party/edk2/README):
 *
 *   Copyright (c) 2010 - 2020, Intel Corporation. All rights reserved.
 *   (C) Copyright 2015 Hewlett Packard Enterprise Development LP
 *   SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * The licence text is third_party/edk2/License.txt.
 *
 * The register definitions, the command list and FIS construction, the
 * port reset and error recovery, the retry rule, the command start and
 * the port initialization follow edk2. The adaptation gives each port its
 * own command list, received FIS area, command table and bounce pages,
 * and the ports work in parallel. The edk2 driver stops a port after every
 * command. This driver leaves it running between commands and stops it
 * only for error recovery. Completion is the clearing of the command
 * issue bit, which an MSI interrupt reports, and the driver polls without
 * one. SMART, device sleep, PUIS, staggered spin up and the IDE controller
 * init protocol of edk2 are left out. ATA disks become sdX and use DMA.
 * ATAPI drives use the PACKET command with PIO data, as in edk2, and
 * register through block/scsi.c as srN. */
#define KLOG_SUBSYS "ahci"
#include <drivers/ahci.h>
#include <drivers/pci.h>
#include <drivers/timer.h>
#include <drivers/devinfo.h>
#include <block/blockdev.h>
#include <block/scsi.h>
#include <arch/barrier.h>
#include <arch/irq.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <sched/wait.h>
#include <sync/spinlock.h>
#include <sync/mutex.h>
#include <lib/cmdline.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

/* HBA registers. */
#define AHCI_BAR                5
#define HBA_CAP                 0x00
#define   CAP_NP_MASK           0x1fu
#define   CAP_SCLO              (1u << 24)
#define   CAP_SSS               (1u << 27)
#define   CAP_S64A              (1u << 31)
#define HBA_GHC                 0x04
#define   GHC_RESET             (1u << 0)
#define   GHC_IE                (1u << 1)
#define   GHC_ENABLE            (1u << 31)
#define HBA_IS                  0x08
#define HBA_PI                  0x0c
#define HBA_VS                  0x10
#define HBA_CAP2                0x24
#define   CAP2_BOH              (1u << 0)
#define HBA_BOHC                0x28
#define   BOHC_BOS              (1u << 0)
#define   BOHC_OOS              (1u << 1)
#define   BOHC_BB               (1u << 4)

#define MAX_PORTS               32

/* Port registers, at 0x100 + 0x80 * port. */
#define PORT_START              0x100
#define PORT_REG_WIDTH          0x80
#define PORT_CLB                0x00
#define PORT_CLBU               0x04
#define PORT_FB                 0x08
#define PORT_FBU                0x0c
#define PORT_IS                 0x10
#define   IS_DHRS               (1u << 0)
#define   IS_PSS                (1u << 1)
#define   IS_DSS                (1u << 2)
#define   IS_SDBS               (1u << 3)
#define   IS_UFS                (1u << 4)
#define   IS_DPS                (1u << 5)
#define   IS_OFS                (1u << 24)
#define   IS_INFS               (1u << 26)
#define   IS_IFS                (1u << 27)
#define   IS_HBDS               (1u << 28)
#define   IS_HBFS               (1u << 29)
#define   IS_TFES               (1u << 30)
#define   IS_ERROR_MASK         (IS_INFS | IS_IFS | IS_HBDS | IS_HBFS | IS_TFES)
#define   IS_FATAL_ERROR_MASK   (IS_IFS | IS_HBDS | IS_HBFS | IS_TFES)
#define PORT_IE                 0x14
#define   IE_ENABLED            (IS_DHRS | IS_PSS | IS_DSS | IS_SDBS | IS_UFS | IS_DPS | IS_OFS | IS_ERROR_MASK)
#define PORT_CMD                0x18
#define   CMD_ST                (1u << 0)
#define   CMD_SUD               (1u << 1)
#define   CMD_POD               (1u << 2)
#define   CMD_CLO               (1u << 3)
#define   CMD_FRE               (1u << 4)
#define   CMD_FR                (1u << 14)
#define   CMD_CR                (1u << 15)
#define   CMD_CPD               (1u << 20)
#define   CMD_ATAPI             (1u << 24)
#define   CMD_DLAE              (1u << 25)
#define   CMD_ALPE              (1u << 26)
#define   CMD_ICC_MASK          (0xfu << 28)
#define   CMD_ICC_ACTIVE        (1u << 28)
#define PORT_TFD                0x20
#define   TFD_ERR               (1u << 0)
#define   TFD_DRQ               (1u << 3)
#define   TFD_BSY               (1u << 7)
#define   TFD_MASK              (TFD_BSY | TFD_DRQ | TFD_ERR)
#define   TFD_ERR_INT_CRC       (1u << 15)
#define PORT_SIG                0x24
#define   SIG_ATAPI             0xeb140000u
#define   SIG_ATA               0x00000000u
#define   SIG_MASK              0xffff0000u
#define PORT_SSTS               0x28
#define   SSTS_DET_MASK         0xfu
#define   SSTS_DET              0x1u
#define   SSTS_DET_PCE          0x3u
#define PORT_SCTL               0x2c
#define   SCTL_DET_MASK         0xfu
#define   SCTL_DET_INIT         0x1u
#define   SCTL_IPM_INIT         0x300u
#define PORT_SERR               0x30
#define   SERR_CRCE             (1u << 21)
#define PORT_CI                 0x38

/* Frame information structures. */
#define FIS_REGISTER_H2D        0x27
#define FIS_REGISTER_H2D_LENGTH 20
#define D2H_FIS_OFFSET          0x40

/* ATA commands. */
#define ATA_READ_DMA            0xc8
#define ATA_WRITE_DMA           0xca
#define ATA_READ_DMA_EXT        0x25
#define ATA_WRITE_DMA_EXT       0x35
#define ATA_FLUSH_CACHE         0xe7
#define ATA_FLUSH_CACHE_EXT     0xea
#define ATA_IDENTIFY            0xec
#define ATA_IDENTIFY_PACKET     0xa1
#define ATA_PACKET              0xa0
#define ATAPI_MAX_BYTE_COUNT    0xfffe

#define BOUNCE_PAGES            32      /* 128 KiB per command */
#define COMMAND_RETRIES         3
#define ATA_TIMEOUT_MS          5000
#define ATAPI_TIMEOUT_MS        20000
#define RESET_TIMEOUT_MS        1000
#define PHY_DETECT_MS           15      /* SATA 1.0a, section 5.2, with a margin */
#define READY_TIMEOUT_MS        5000
#define MAX_CONTROLLERS         4

/* A command header of the command list. */
struct cmd_header {
    uint16_t flags;                 /* CFL in bits 0 to 4, A, W, P, R, B, C, PMP in bits 12 to 15 */
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} __packed;
#define HDR_ATAPI               (1u << 5)
#define HDR_WRITE               (1u << 6)
#define HDR_PREFETCH            (1u << 7)

/* The host to device register FIS of a command. */
struct cmd_fis {
    uint8_t type;
    uint8_t flags;                  /* bit 7: command, bits 0 to 3: port multiplier port */
    uint8_t command;
    uint8_t features;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t features_exp;
    uint8_t count, count_exp;
    uint8_t reserved0;
    uint8_t control;
    uint8_t reserved1[4];
} __packed;

struct prd {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;                   /* byte count minus 1 in bits 0 to 21, interrupt in bit 31 */
} __packed;

/* The command table: the command FIS, the ATAPI command and the PRD table. */
#define TABLE_ACMD              0x40
#define TABLE_PRDT              0x80

/* An ATA command block, as EFI_ATA_COMMAND_BLOCK of edk2. */
struct ata_cmd {
    uint8_t command;
    uint8_t features, features_exp;
    uint8_t count, count_exp;
    uint8_t lba[6];
    uint8_t device;
};

struct ahci;

/* One port with a device.
 *
 * lock serializes the commands of the port and protects the command list,
 * the received FIS area, the command table and the bounce pages.
 * irq_status is protected by ahci.lock. The other fields are set during
 * the probe. */
struct ahci_port {
    struct ahci *hba;
    unsigned index;
    struct mutex lock;
    uint8_t *mem;                   /* command list at 0, received FIS area at 0x400 */
    uintptr_t mem_phys;
    uint8_t *table;
    uintptr_t table_phys;
    uint8_t *bounce[BOUNCE_PAGES];
    uintptr_t bounce_phys[BOUNCE_PAGES];
    uint32_t irq_status;            /* PxIS bits that the interrupt handler cleared */
    uint32_t last_is;               /* PxIS of the last command, for the error recovery */
    bool atapi;
    bool lba48;
    char model[41], serial[21], firmware[9];
    struct blockdev disk;           /* an ATA disk */
    struct scsi_device *scsi;       /* an ATAPI drive */
};

/* One controller. lock protects irq_status of every port and is the
 * condition lock of waitq. The other fields are set during the probe. */
struct ahci {
    struct pci_dev *pci;
    unsigned index;
    volatile uint8_t *regs;
    uint32_t cap, version;
    int irq;
    const char *irq_kind;
    struct spinlock lock;
    struct waitq waitq;
    struct ahci_port *ports[MAX_PORTS];
};

static struct ahci *controllers[MAX_CONTROLLERS];
static unsigned ncontrollers;           /* written by ahci_init before any device is registered */

static inline uint32_t rd(struct ahci *x, unsigned off)
{
    return *(volatile uint32_t *)(x->regs + off);
}

static inline void wr(struct ahci *x, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(x->regs + off) = v;
}

static inline unsigned port_off(unsigned port, unsigned reg)
{
    return PORT_START + port * PORT_REG_WIDTH + reg;
}

static inline uint32_t port_rd(struct ahci_port *p, unsigned reg)
{
    return rd(p->hba, port_off(p->index, reg));
}

static inline void port_wr(struct ahci_port *p, unsigned reg, uint32_t v)
{
    wr(p->hba, port_off(p->index, reg), v);
}

static void port_or(struct ahci_port *p, unsigned reg, uint32_t bits)
{
    port_wr(p, reg, port_rd(p, reg) | bits);
}

static void port_and(struct ahci_port *p, unsigned reg, uint32_t bits)
{
    port_wr(p, reg, port_rd(p, reg) & bits);
}

/* Wait until (register & mask) == value, for at most timeout_ms
 * (AhciWaitMmioSet). */
static int wait_mmio(struct ahci *x, unsigned off, uint32_t mask, uint32_t value, unsigned timeout_ms)
{
    for (unsigned t = 0;; t++) {
        if ((rd(x, off) & mask) == value)
            return 0;
        if (t >= timeout_ms)
            return -ETIMEDOUT;
        sleep_ms(1);
    }
}

/* Clear the error and interrupt status of the port and its bit in the
 * HBA interrupt status (AhciClearPortStatus). */
static void clear_port_status(struct ahci_port *p)
{
    port_wr(p, PORT_SERR, port_rd(p, PORT_SERR));
    spin_lock(&p->hba->lock);
    port_wr(p, PORT_IS, port_rd(p, PORT_IS));
    p->irq_status = 0;
    wr(p->hba, HBA_IS, 1u << p->index);
    spin_unlock(&p->hba->lock);
}

/* Stop the command engine of the port (AhciStopCommand). */
static int stop_command(struct ahci_port *p)
{
    uint32_t v = port_rd(p, PORT_CMD);
    if (!(v & (CMD_ST | CMD_CR)))
        return 0;
    if (v & CMD_ST)
        port_and(p, PORT_CMD, ~CMD_ST);
    return wait_mmio(p->hba, port_off(p->index, PORT_CMD), CMD_CR, 0, 500);
}

/* Wait until the device clears BSY, DRQ and ERR (AhciWaitDeviceReady). */
static int wait_device_ready(struct ahci_port *p, unsigned timeout_ms)
{
    for (unsigned t = 0;; t++) {
        uint32_t serr = port_rd(p, PORT_SERR);
        if (serr)
            port_wr(p, PORT_SERR, serr);
        if (!(port_rd(p, PORT_TFD) & TFD_MASK))
            return 0;
        if (t >= timeout_ms) {
            klog_warn("port %u: device not ready (TFD 0x%x)", p->index, port_rd(p, PORT_TFD));
            return -ETIMEDOUT;
        }
        sleep_ms(1);
    }
}

/* COMRESET of the port (AhciResetPort). DET remains 1 for at least 1 ms,
 * which sends at least one COMRESET. */
static int reset_port(struct ahci_port *p)
{
    port_or(p, PORT_SCTL, SCTL_DET_INIT);
    sleep_ms(2);
    port_and(p, PORT_SCTL, ~SCTL_DET_MASK);
    int r = wait_mmio(p->hba, port_off(p->index, PORT_SSTS), SSTS_DET_MASK, SSTS_DET_PCE, ATA_TIMEOUT_MS);
    if (r < 0)
        return r;
    return wait_device_ready(p, READY_TIMEOUT_MS);
}

/* After a fatal error the command engine is stopped, and a device that
 * remains busy is reset (AhciRecoverPortError). The engine starts again
 * with the next command. */
static int recover_port_error(struct ahci_port *p)
{
    if (!(p->last_is & IS_FATAL_ERROR_MASK))
        return 0;
    if (stop_command(p) < 0) {
        klog_error("port %u: the command engine does not stop, recovery abandoned", p->index);
        return -EIO;
    }
    if (port_rd(p, PORT_TFD) & (TFD_BSY | TFD_DRQ)) {
        if (reset_port(p) < 0) {
            klog_error("port %u: reset failed", p->index);
            return -EIO;
        }
    }
    clear_port_status(p);
    return 0;
}

/* A transport error that a repeated command may cure: a CRC error
 * between memory and the controller, or between the controller and the
 * device (AhciShouldCmdBeRetried). */
static bool should_retry(struct ahci_port *p)
{
    uint32_t is = p->last_is, serr = port_rd(p, PORT_SERR), tfd = port_rd(p, PORT_TFD);
    if (is & IS_HBDS)
        return true;
    if ((is & (IS_IFS | IS_INFS)) && (serr & SERR_CRCE))
        return true;
    return (is & IS_TFES) && (tfd & TFD_ERR_INT_CRC);
}

/* Fill the command FIS from an ATA command block (AhciBuildCommandFis). */
static void build_fis(struct cmd_fis *fis, const struct ata_cmd *ac)
{
    memset(fis, 0, sizeof *fis);
    fis->type = FIS_REGISTER_H2D;
    fis->flags = 0x80;
    fis->command = ac->command;
    fis->features = ac->features;
    fis->features_exp = ac->features_exp;
    fis->lba0 = ac->lba[0];
    fis->lba1 = ac->lba[1];
    fis->lba2 = ac->lba[2];
    fis->lba3 = ac->lba[3];
    fis->lba4 = ac->lba[4];
    fis->lba5 = ac->lba[5];
    fis->count = ac->count;
    fis->count_exp = ac->count_exp;
    fis->device = (uint8_t)(ac->device | 0xe0);
}

/* Fill command slot 0 and the command table for a transfer of len bytes
 * of the bounce pages (AhciBuildCommand). */
static void build_command(struct ahci_port *p, const struct ata_cmd *ac, const uint8_t *cdb, unsigned cdb_len,
                          size_t len, bool write)
{
    memset(p->mem + 0x400, 0, 256);
    memset(p->table, 0, TABLE_PRDT + BOUNCE_PAGES * sizeof(struct prd));
    build_fis((struct cmd_fis *)p->table, ac);
    uint16_t flags = FIS_REGISTER_H2D_LENGTH / 4 | (write ? HDR_WRITE : 0);
    if (cdb) {
        memcpy(p->table + TABLE_ACMD, cdb, cdb_len);
        flags |= HDR_ATAPI | HDR_PREFETCH;
        port_or(p, PORT_CMD, CMD_DLAE | CMD_ATAPI);
    } else {
        port_and(p, PORT_CMD, ~(CMD_DLAE | CMD_ATAPI));
    }
    struct prd *prdt = (struct prd *)(p->table + TABLE_PRDT);
    unsigned n = 0;
    for (size_t done = 0; done < len; n++) {
        size_t chunk = MIN(len - done, (size_t)PAGE_SIZE);
        prdt[n].dba = (uint32_t)p->bounce_phys[n];
        prdt[n].dbau = (uint32_t)((uint64_t)p->bounce_phys[n] >> 32);
        prdt[n].dbc = (uint32_t)(chunk - 1);
        done += chunk;
    }
    if (n)
        prdt[n - 1].dbc |= 1u << 31;
    struct cmd_header *h = (struct cmd_header *)p->mem;
    memset(h, 0, sizeof *h);
    h->flags = flags;
    h->prdtl = (uint16_t)n;
    h->ctba = (uint32_t)p->table_phys;
    h->ctbau = (uint32_t)((uint64_t)p->table_phys >> 32);
}

/* Start command slot 0 (AhciStartCommand): clear the status, enable FIS
 * reception, wake the link, clear a busy task file through CLO where the
 * controller offers it, start the engine and issue the slot. */
static void start_command(struct ahci_port *p)
{
    clear_port_status(p);
    port_or(p, PORT_CMD, CMD_FRE);
    uint32_t cmd = port_rd(p, PORT_CMD), start = 0;
    if (cmd & CMD_ALPE)
        start = (cmd & ~CMD_ICC_MASK) | CMD_ICC_ACTIVE;
    if ((port_rd(p, PORT_TFD) & (TFD_BSY | TFD_DRQ)) && (p->hba->cap & CAP_SCLO)) {
        port_or(p, PORT_CMD, CMD_CLO);
        wait_mmio(p->hba, port_off(p->index, PORT_CMD), CMD_CLO, 0, 500);
    }
    wmb();
    port_or(p, PORT_CMD, CMD_ST | start);
    port_wr(p, PORT_CI, 1);
}

/* Wait until slot 0 completes or reports an error. The waiter reads the
 * port itself on every pass: after each interrupt, every second with an
 * interrupt as a safeguard, and every millisecond without one. */
static int wait_command(struct ahci_port *p, unsigned timeout_ms)
{
    struct ahci *x = p->hba;
    uint64_t end = timer_ms() + timeout_ms;
    int r;
    spin_lock(&x->lock);
    for (;;) {
        uint32_t is = port_rd(p, PORT_IS) | p->irq_status;
        if (is & IS_ERROR_MASK) {
            r = -EIO;
            break;
        }
        if (!(port_rd(p, PORT_CI) & 1)) {
            r = (port_rd(p, PORT_TFD) & TFD_ERR) ? -EIO : 0;
            break;
        }
        uint64_t now = timer_ms();
        if (now >= end) {
            r = -ETIMEDOUT;
            break;
        }
        waitq_wait_timeout(&x->waitq, &x->lock, MIN(end, now + (x->irq >= 0 ? 1000 : 1)));
    }
    p->last_is = port_rd(p, PORT_IS) | p->irq_status;
    spin_unlock(&x->lock);
    rmb();
    return r;
}

/* Run one command on slot 0 with len bytes of the bounce pages, repeating
 * it after a transport error that a repetition may cure (the transfer
 * functions of edk2). Returns -EIO with the task file in *tfd when the
 * device reports an error. The caller has locked p->lock. */
static int exec(struct ahci_port *p, const struct ata_cmd *ac, const uint8_t *cdb, unsigned cdb_len, size_t len,
                bool write, unsigned timeout_ms, uint32_t *tfd)
{
    int r = -EIO;
    for (unsigned retry = 0; retry < COMMAND_RETRIES; retry++) {
        build_command(p, ac, cdb, cdb_len, len, write);
        start_command(p);
        r = wait_command(p, timeout_ms);
        if (r == 0)
            break;
        if (tfd)
            *tfd = port_rd(p, PORT_TFD);
        if (r == -ETIMEDOUT) {
            klog_error("port %u: command %02x without completion within %u ms", p->index, ac->command, timeout_ms);
            p->last_is |= IS_TFES;
            recover_port_error(p);
            break;
        }
        bool again = should_retry(p);
        if (recover_port_error(p) < 0 || !again)
            break;
    }
    /* The error interrupt and an ATAPI CHECK CONDITION leave the engine
     * running only when the error was not fatal. A stopped engine starts
     * again with the next command. */
    return r;
}

/* ---- ATA disks ---- */

/* Copy an identify string: the bytes of each word are swapped, and the
 * trailing blanks are removed. */
static void identify_string(char *dst, const uint16_t *words, unsigned nwords)
{
    for (unsigned i = 0; i < nwords; i++) {
        dst[2 * i] = (char)(words[i] >> 8);
        dst[2 * i + 1] = (char)words[i];
    }
    unsigned len = nwords * 2;
    dst[len] = '\0';
    while (len && (dst[len - 1] == ' ' || dst[len - 1] == '\0'))
        dst[--len] = '\0';
}

static int identify(struct ahci_port *p, uint8_t command)
{
    struct ata_cmd ac = { .command = command, .count = 1 };
    return exec(p, &ac, NULL, 0, 512, false, ATA_TIMEOUT_MS, NULL);
}

static int ata_rw(struct blockdev *dev, uint64_t sector, uint32_t count, void *buf, bool write)
{
    struct ahci_port *p = dev->priv;
    if (sector + count > dev->nsectors || sector + count < sector)
        return -EINVAL;
    uint32_t per = (uint32_t)(BOUNCE_PAGES * PAGE_SIZE / dev->sector_size);
    if (!p->lba48)
        per = MIN(per, 256u);
    int r = 0;
    mutex_lock(&p->lock);
    while (count && r == 0) {
        uint32_t n = MIN(count, per);
        size_t bytes = (size_t)n * dev->sector_size;
        if (write)
            for (size_t off = 0; off < bytes; off += PAGE_SIZE)
                memcpy(p->bounce[off / PAGE_SIZE], (uint8_t *)buf + off, MIN(bytes - off, (size_t)PAGE_SIZE));
        struct ata_cmd ac = { .device = 0x40 };
        for (int i = 0; i < 6; i++)
            ac.lba[i] = (uint8_t)(sector >> (8 * i));
        ac.count = (uint8_t)n;
        if (p->lba48) {
            ac.command = write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT;
            ac.count_exp = (uint8_t)(n >> 8);
        } else {
            ac.command = write ? ATA_WRITE_DMA : ATA_READ_DMA;
            ac.device = (uint8_t)(0x40 | ((sector >> 24) & 0xf));
            ac.lba[3] = ac.lba[4] = ac.lba[5] = 0;
        }
        uint32_t tfd = 0;
        r = exec(p, &ac, NULL, 0, bytes, write, ATA_TIMEOUT_MS, &tfd);
        if (r < 0)
            klog_warn("%s: %s of %u sectors at %lu failed (TFD 0x%x)", dev->name, write ? "write" : "read", n,
                      sector, tfd);
        else if (!write)
            for (size_t off = 0; off < bytes; off += PAGE_SIZE)
                memcpy((uint8_t *)buf + off, p->bounce[off / PAGE_SIZE], MIN(bytes - off, (size_t)PAGE_SIZE));
        buf = (uint8_t *)buf + bytes;
        sector += n;
        count -= n;
    }
    mutex_unlock(&p->lock);
    return r < 0 ? -EIO : 0;
}

static int ata_flush(struct blockdev *dev)
{
    struct ahci_port *p = dev->priv;
    struct ata_cmd ac = { .command = p->lba48 ? ATA_FLUSH_CACHE_EXT : ATA_FLUSH_CACHE };
    mutex_lock(&p->lock);
    int r = exec(p, &ac, NULL, 0, 0, false, ATA_TIMEOUT_MS, NULL);
    mutex_unlock(&p->lock);
    return r < 0 ? -EIO : 0;
}

/* Register an ATA disk from its IDENTIFY DEVICE data. */
static void add_disk(struct ahci_port *p, const uint16_t *id)
{
    identify_string(p->serial, id + 10, 10);
    identify_string(p->firmware, id + 23, 4);
    identify_string(p->model, id + 27, 20);
    uint64_t sectors;
    p->lba48 = id[83] & (1u << 10);
    if (p->lba48)
        sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
    else
        sectors = (uint64_t)id[60] | (uint64_t)id[61] << 16;
    uint32_t sector_size = 512;
    if ((id[106] & 0xc000) == 0x4000 && (id[106] & (1u << 12)))
        sector_size = 2 * ((uint32_t)id[117] | (uint32_t)id[118] << 16);
    if (!(id[49] & (1u << 9)) || !sectors || sector_size < 512 || sector_size > PAGE_SIZE) {
        klog_warn("port %u: %s without LBA, or with %u byte sectors, not supported", p->index, p->model,
                  sector_size);
        return;
    }
    blockdev_next_name(p->disk.name, sizeof p->disk.name, "sd", true);
    p->disk.sector_size = sector_size;
    p->disk.nsectors = sectors;
    p->disk.rw = ata_rw;
    p->disk.flush = ata_flush;
    p->disk.priv = p;
    if (blockdev_register(&p->disk) < 0)
        return;
    klog_info("%s: %s (serial %s, firmware %s), port %u, %lu sectors of %u bytes (%lu MiB), %s", p->disk.name,
              p->model, p->serial, p->firmware, p->index, sectors, sector_size, (sectors * sector_size) >> 20,
              p->lba48 ? "LBA48" : "LBA28");
}

/* ---- ATAPI drives ---- */

/* The SCSI transport of an ATAPI drive (AhciPacketCommandExecute): the
 * PACKET command with PIO data and the device's choice of byte count. An
 * error with ERR in the task file is a CHECK CONDITION. */
static int atapi_command(void *ctx, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len, bool write)
{
    struct ahci_port *p = ctx;
    if (len > BOUNCE_PAGES * PAGE_SIZE || cdb_len > 16)
        return -EINVAL;
    uint8_t packet[16] = { 0 };
    memcpy(packet, cdb, cdb_len);
    struct ata_cmd ac = {
        .command = ATA_PACKET,
        .lba = { 0, ATAPI_MAX_BYTE_COUNT & 0xff, ATAPI_MAX_BYTE_COUNT >> 8 },
    };
    mutex_lock(&p->lock);
    if (write)
        for (uint32_t off = 0; off < len; off += PAGE_SIZE)
            memcpy(p->bounce[off / PAGE_SIZE], (uint8_t *)buf + off, MIN(len - off, (uint32_t)PAGE_SIZE));
    uint32_t tfd = 0;
    int r = exec(p, &ac, packet, 16, len, write, ATAPI_TIMEOUT_MS, &tfd);
    if (r == 0 && !write)
        for (uint32_t off = 0; off < len; off += PAGE_SIZE)
            memcpy((uint8_t *)buf + off, p->bounce[off / PAGE_SIZE], MIN(len - off, (uint32_t)PAGE_SIZE));
    mutex_unlock(&p->lock);
    if (r == -EIO && (tfd & TFD_ERR))
        return SCSI_CHECK_CONDITION;
    return r;
}

/* ---- Controller ---- */

static void ahci_irq(struct trapframe *tf, void *arg)
{
    struct ahci *x = arg;
    spin_lock(&x->lock);
    uint32_t is = rd(x, HBA_IS);
    for (unsigned i = 0; i < MAX_PORTS; i++) {
        if (!(is & (1u << i)))
            continue;
        uint32_t v = rd(x, port_off(i, PORT_IS));
        wr(x, port_off(i, PORT_IS), v);
        if (x->ports[i])
            x->ports[i]->irq_status |= v;
    }
    wr(x, HBA_IS, is);
    if (is)
        waitq_wake_all(&x->waitq);
    spin_unlock(&x->lock);
}

/* Take the controller from the firmware (BIOS/OS handoff of AHCI 1.3,
 * section 10.6.3). */
static void bios_handoff(struct ahci *x)
{
    if (!(rd(x, HBA_CAP2) & CAP2_BOH))
        return;
    wr(x, HBA_BOHC, rd(x, HBA_BOHC) | BOHC_OOS);
    if (wait_mmio(x, HBA_BOHC, BOHC_BOS, 0, 25) < 0) {
        /* The firmware may need up to 2 seconds while it is busy. */
        if ((rd(x, HBA_BOHC) & BOHC_BB) && wait_mmio(x, HBA_BOHC, BOHC_BOS, 0, 2000) == 0)
            return;
        klog_warn("the firmware did not release the controller");
    }
}

/* Reset the HBA (AhciReset). GHC.AE is set before any other register is
 * accessed, and again after the reset. */
static int hba_reset(struct ahci *x)
{
    if (!(rd(x, HBA_GHC) & GHC_ENABLE))
        wr(x, HBA_GHC, rd(x, HBA_GHC) | GHC_ENABLE);
    wr(x, HBA_GHC, rd(x, HBA_GHC) | GHC_RESET);
    if (wait_mmio(x, HBA_GHC, GHC_RESET, 0, RESET_TIMEOUT_MS) < 0)
        return -ETIMEDOUT;
    wr(x, HBA_GHC, rd(x, HBA_GHC) | GHC_ENABLE);
    return 0;
}

/* The interrupt of the controller: MSI-X, else MSI, else none, and the
 * waiters poll. The kernel option ahci=poll selects polling, which the
 * QEMU controller cannot otherwise show. */
static void setup_interrupt(struct ahci *x)
{
    x->irq = -1;
    x->irq_kind = "no";
    char opt[8];
    if (cmdline_lookup("ahci", opt, sizeof opt) && strcmp(opt, "poll") == 0)
        return;
    int irq = irq_alloc();
    if (irq < 0)
        return;
    irq_register((unsigned)irq, ahci_irq, x);
    if (pci_msix_enable(x->pci) == 0 && pci_msix_set_vector(x->pci, 0, (unsigned)irq) == 0) {
        x->irq = irq;
        x->irq_kind = "msi-x";
    } else if (pci_msi_enable(x->pci, (unsigned)irq) == 0) {
        x->irq = irq;
        x->irq_kind = "msi";
    }
}

/* Bring up one implemented port and attach its device
 * (AhciModeInitialization). */
static void probe_port(struct ahci *x, unsigned index)
{
    struct ahci_port *p = kzalloc(sizeof *p);
    if (!p)
        return;
    p->hba = x;
    p->index = index;
    mutex_init(&p->lock, "ahci_port");
    if (!(p->mem = pmm_alloc_dma_page(&p->mem_phys)) || !(p->table = pmm_alloc_dma_page(&p->table_phys)))
        goto fail;
    if (!(x->cap & CAP_S64A) && (p->mem_phys >> 32 || p->table_phys >> 32))
        goto fail;

    /* The engine and FIS reception stop before the bases change. */
    stop_command(p);
    port_and(p, PORT_CMD, ~CMD_FRE);
    wait_mmio(x, port_off(index, PORT_CMD), CMD_FR, 0, 500);
    port_wr(p, PORT_CLB, (uint32_t)p->mem_phys);
    port_wr(p, PORT_CLBU, (uint32_t)((uint64_t)p->mem_phys >> 32));
    port_wr(p, PORT_FB, (uint32_t)(p->mem_phys + 0x400));
    port_wr(p, PORT_FBU, (uint32_t)((uint64_t)(p->mem_phys + 0x400) >> 32));
    if (port_rd(p, PORT_CMD) & CMD_CPD)
        port_or(p, PORT_CMD, CMD_POD);
    if (x->cap & CAP_SSS)
        port_or(p, PORT_CMD, CMD_SUD);
    /* Aggressive power management off. */
    port_or(p, PORT_SCTL, SCTL_IPM_INIT);
    port_wr(p, PORT_IE, 0);
    port_or(p, PORT_CMD, CMD_FRE);

    /* The phy reports a device within PHY_DETECT_MS. */
    unsigned det = 0;
    for (unsigned t = 0; t <= PHY_DETECT_MS; t++) {
        det = port_rd(p, PORT_SSTS) & SSTS_DET_MASK;
        if (det == SSTS_DET_PCE || det == SSTS_DET)
            break;
        sleep_ms(1);
    }
    if (det != SSTS_DET_PCE && det != SSTS_DET) {
        port_and(p, PORT_CMD, ~CMD_SUD);
        goto fail;
    }
    if (wait_device_ready(p, READY_TIMEOUT_MS) < 0)
        goto fail;
    /* The first D2H register FIS sets the signature. */
    if (wait_mmio(x, port_off(index, PORT_SIG), 0xffff, 0x0101, 16000) < 0)
        goto fail;
    uint32_t sig = port_rd(p, PORT_SIG) & SIG_MASK;
    if (sig != SIG_ATA && sig != SIG_ATAPI) {
        klog_info("port %u: signature 0x%08x not supported", index, port_rd(p, PORT_SIG));
        goto fail;
    }
    p->atapi = sig == SIG_ATAPI;
    /* The bounce pages are allocated for a port with a device only. */
    for (unsigned i = 0; i < BOUNCE_PAGES; i++) {
        if (!(p->bounce[i] = pmm_alloc_dma_page(&p->bounce_phys[i])) ||
            (!(x->cap & CAP_S64A) && p->bounce_phys[i] >> 32)) {
            klog_error("port %u: no memory below 4 GiB for the transfers", index);
            goto fail;
        }
    }
    spin_lock(&x->lock);
    x->ports[index] = p;
    spin_unlock(&x->lock);
    clear_port_status(p);
    if (x->irq >= 0)
        port_wr(p, PORT_IE, IE_ENABLED);

    mutex_lock(&p->lock);
    int r = identify(p, p->atapi ? ATA_IDENTIFY_PACKET : ATA_IDENTIFY);
    uint16_t id[256];
    if (r == 0)
        memcpy(id, p->bounce[0], sizeof id);
    mutex_unlock(&p->lock);
    if (r < 0) {
        klog_warn("port %u: IDENTIFY failed", index);
        return;
    }
    if (p->atapi) {
        identify_string(p->serial, id + 10, 10);
        identify_string(p->firmware, id + 23, 4);
        identify_string(p->model, id + 27, 20);
        p->scsi = scsi_attach(atapi_command, p, BOUNCE_PAGES * PAGE_SIZE, "atapi");
    } else {
        add_disk(p, id);
    }
    return;
fail:
    /* The pages of a port that failed are not returned: the port may have
     * received their addresses. */
    return;
}

static struct ahci *ahci_start(struct pci_dev *pd, unsigned index)
{
    uint64_t size = pci_bar_size(pd, AHCI_BAR);
    if (size < 0x180) {
        klog_error("%02x:%02x.%u: no register BAR", pd->bus, pd->slot, pd->func);
        return NULL;
    }
    struct ahci *x = kzalloc(sizeof *x);
    if (!x)
        return NULL;
    x->pci = pd;
    x->index = index;
    spinlock_init(&x->lock, "ahci");
    waitq_init(&x->waitq, "ahci");
    pci_enable_bus_master(pd);
    x->regs = vmm_map_mmio(pd->bar[AHCI_BAR], ALIGN_UP(size, PAGE_SIZE), VM_KERNEL_RW | VM_NOCACHE);
    if (!x->regs)
        return NULL;
    bios_handoff(x);
    if (hba_reset(x) < 0) {
        klog_error("%02x:%02x.%u: the reset does not complete", pd->bus, pd->slot, pd->func);
        return NULL;
    }
    x->cap = rd(x, HBA_CAP);
    x->version = rd(x, HBA_VS);
    uint32_t pi = rd(x, HBA_PI);
    setup_interrupt(x);
    klog_info("%02x:%02x.%u: ahci %x.%x, %s interrupt, %u ports, implemented 0x%x%s", pd->bus, pd->slot,
              pd->func, x->version >> 16, (x->version >> 8) & 0xff, x->irq_kind, (x->cap & CAP_NP_MASK) + 1, pi,
              x->cap & CAP_S64A ? ", 64 bit addresses" : "");
    if (x->irq >= 0) {
        wr(x, HBA_IS, rd(x, HBA_IS));
        wr(x, HBA_GHC, rd(x, HBA_GHC) | GHC_IE);
    }
    for (unsigned port = 0; port < MAX_PORTS; port++)
        if (pi & (1u << port))
            probe_port(x, port);
    return x;
}

void ahci_init(void)
{
    for (size_t i = 0; i < pci_count() && ncontrollers < MAX_CONTROLLERS; i++) {
        struct pci_dev *p = pci_device(i);
        if (p->class != 0x01 || p->subclass != 0x06 || p->prog_if != 0x01)
            continue;
        struct ahci *x = ahci_start(p, ncontrollers);
        if (x) {
            controllers[ncontrollers++] = x;
            p->driver = "ahci";
        }
    }
}

/* ---- /dev/devices (docs/design/sysinfo.md) ---- */

void ahci_describe(struct devinfo *d)
{
    if (!ncontrollers)
        return;
    devinfo_node(d, "ahci", "AHCI");
    devinfo_prop(d, "controllers", "%u", ncontrollers);
    for (unsigned i = 0; i < ncontrollers; i++) {
        struct ahci *x = controllers[i];
        const struct pci_dev *pd = x->pci;
        char path[32];
        ksnprintf(path, sizeof path, "ahci/ahci%u", i);
        devinfo_node(d, path, "AHCI controller %02x:%02x.%u", pd->bus, pd->slot, pd->func);
        devinfo_prop(d, "pci_address", "%02x:%02x.%u", pd->bus, pd->slot, pd->func);
        devinfo_prop(d, "version", "%x.%x", x->version >> 16, (x->version >> 8) & 0xff);
        devinfo_prop(d, "ports", "%u", (x->cap & CAP_NP_MASK) + 1);
        devinfo_prop(d, "interrupt", "%s", x->irq_kind);
        devinfo_prop(d, "64_bit_addresses", "%s", x->cap & CAP_S64A ? "yes" : "no");
        for (unsigned port = 0; port < MAX_PORTS; port++) {
            struct ahci_port *p = x->ports[port];
            if (!p)
                continue;
            char ppath[48];
            ksnprintf(ppath, sizeof ppath, "%s/port%u", path, port);
            const char *name = p->atapi ? (p->scsi ? p->scsi->bdev.name : "-") : p->disk.name;
            devinfo_node(d, ppath, "Port %u: %s", port, p->model);
            devinfo_prop(d, "device", "%s", name[0] ? name : "-");
            devinfo_prop(d, "kind", "%s", p->atapi ? "ATAPI" : "ATA");
            devinfo_prop(d, "model", "%s", p->model);
            devinfo_prop(d, "serial", "%s", p->serial);
            devinfo_prop(d, "firmware", "%s", p->firmware);
        }
    }
}
