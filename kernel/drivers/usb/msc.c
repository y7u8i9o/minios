/* USB mass storage with the bulk-only transport (R4 of
 * docs/plan/release-0.5.0.md, D5 of docs/plan/drivers.md,
 * docs/design/usb.md).
 *
 * Adapted from MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassBot.c and
 * UsbMassBot.h of edk2, revision 999fd0f12a27709eee04b93e46bd867e6b0163a5
 * (third_party/edk2/README):
 *
 *   Copyright (c) 2007 - 2018, Intel Corporation. All rights reserved.
 *   SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * The licence text is third_party/edk2/License.txt.
 *
 * The command block wrapper, the data stage, the status stage with its
 * retries, the checks of the command status wrapper, the reset recovery
 * and GET MAX LUN follow edk2. The adaptation adds the check of the tag
 * and of the length of the status wrapper, which the Bulk-Only Transport
 * specification (6.3) requires, splits a data stage into transfers of at
 * most XHCI_BULK_MAX bytes, and registers each logical unit through
 * block/scsi.c, which issues the SCSI commands. */
#define KLOG_SUBSYS "usb-storage"
#include "usb.h"
#include <block/scsi.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define MSC_SUBCLASS_SCSI       0x06
#define MSC_PROTOCOL_BOT        0x50

#define BOT_CBW_SIGNATURE       0x43425355u     /* "USBC" */
#define BOT_CSW_SIGNATURE       0x53425355u     /* "USBS" */
#define BOT_GETLUN_REQUEST      0xfe
#define BOT_RESET_REQUEST       0xff
#define BOT_MAX_LUN             0x0f
#define BOT_MAX_CMDLEN          16
#define BOT_COMMAND_OK          0
#define BOT_COMMAND_FAILED      1
#define BOT_COMMAND_ERROR       2               /* phase error */
#define BOT_RECV_CSW_RETRY      3

#define BOT_SEND_CBW_TIMEOUT_MS 3000
#define BOT_RECV_CSW_TIMEOUT_MS 3000
#define BOT_DATA_TIMEOUT_MS     30000
#define BOT_RESET_STALL_MS      100

struct cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t flags;                  /* bit 7: data from the device */
    uint8_t lun;
    uint8_t cmd_len;
    uint8_t cmd[BOT_MAX_CMDLEN];
} __packed;

struct csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __packed;

struct msc_dev;

struct msc_lun {
    struct msc_dev *msc;
    uint8_t lun;
    struct scsi_device *scsi;
};

/* One bulk-only interface. lock serializes the commands of all its
 * logical units and protects tag. The other fields are set by msc_probe. */
struct msc_dev {
    struct usb_device *usb;
    uint8_t interface;
    uint8_t ep_in, ep_out;
    uint32_t tag;
    unsigned nluns;
    struct mutex lock;
    struct msc_lun luns[BOT_MAX_LUN + 1];
};

/* The class specific reset and the clearing of both bulk endpoints
 * (UsbBotResetDevice, section 5.3.4 of the specification). */
static int bot_reset(struct msc_dev *m)
{
    int r = xhci_control(m->usb, USB_TYPE_CLASS | USB_RECIP_INTERFACE, BOT_RESET_REQUEST, 0, m->interface, NULL,
                         0);
    if (r < 0)
        return r;
    /* The device NAKs until the reset completes. edk2 waits 100 ms. */
    sleep_ms(BOT_RESET_STALL_MS);
    xhci_clear_halt(m->usb, m->ep_in);
    xhci_clear_halt(m->usb, m->ep_out);
    return 0;
}

/* Send the command block wrapper (UsbBotSendCommand). A stall of the
 * bulk-out endpoint is answered by a reset recovery. */
static int bot_send_command(struct msc_dev *m, const uint8_t *cmd, unsigned cmd_len, bool in, uint32_t data_len,
                            uint8_t lun)
{
    struct cbw cbw = {
        .signature = BOT_CBW_SIGNATURE,
        .tag = m->tag,
        .data_len = data_len,
        .flags = in ? 0x80 : 0,
        .lun = lun,
        .cmd_len = (uint8_t)cmd_len,
    };
    memcpy(cbw.cmd, cmd, cmd_len);
    uint32_t actual;
    int r = xhci_bulk(m->usb, m->ep_out, &cbw, sizeof cbw, BOT_SEND_CBW_TIMEOUT_MS, &actual);
    if (r == -EPIPE)
        bot_reset(m);
    if (r == 0 && actual != sizeof cbw)
        r = -EIO;
    return r;
}

/* The data stage (UsbBotDataTransfer). A stall is cleared, and a timeout is
 * answered by a reset recovery. The stage ends early at a short transfer.
 * *done receives the bytes transferred. */
static int bot_data(struct msc_dev *m, bool in, uint8_t *data, uint32_t len, uint32_t *done)
{
    uint8_t ep = in ? m->ep_in : m->ep_out;
    *done = 0;
    while (*done < len) {
        uint32_t chunk = MIN(len - *done, (uint32_t)XHCI_BULK_MAX), actual;
        int r = xhci_bulk(m->usb, ep, data + *done, chunk, BOT_DATA_TIMEOUT_MS, &actual);
        if (r == -EPIPE) {
            xhci_clear_halt(m->usb, ep);
            return r;
        }
        if (r == -ETIMEDOUT)
            bot_reset(m);
        if (r < 0)
            return r;
        *done += actual;
        if (actual < chunk)
            break;
    }
    return 0;
}

/* The status stage (UsbBotGetStatus): up to three attempts to read the
 * command status wrapper. A wrapper with a wrong signature, tag or
 * length, and a phase error, are answered by a reset recovery. The tag
 * advances also after an error. */
static int bot_get_status(struct msc_dev *m, uint8_t *status)
{
    int r = -EIO;
    for (unsigned i = 0; i < BOT_RECV_CSW_RETRY; i++) {
        struct csw csw = { 0 };
        uint32_t actual;
        r = xhci_bulk(m->usb, m->ep_in, &csw, sizeof csw, BOT_RECV_CSW_TIMEOUT_MS, &actual);
        if (r == -ENODEV)
            break;
        if (r < 0) {
            if (r == -EPIPE)
                xhci_clear_halt(m->usb, m->ep_in);
            continue;
        }
        if (actual != sizeof csw || csw.signature != BOT_CSW_SIGNATURE || csw.tag != m->tag) {
            klog_warn("port %s: invalid command status wrapper", m->usb->path);
            bot_reset(m);
            r = -EIO;
        } else if (csw.status == BOT_COMMAND_ERROR) {
            klog_warn("port %s: phase error", m->usb->path);
            bot_reset(m);
            r = -EIO;
        } else {
            *status = csw.status;
            r = 0;
        }
        break;
    }
    m->tag++;
    return r;
}

/* The SCSI transport of a logical unit (UsbBotExecCommand). The status
 * stage follows the data stage also when the data stage failed, as the
 * specification asks. */
static int bot_command(void *ctx, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len, bool write)
{
    struct msc_lun *l = ctx;
    struct msc_dev *m = l->msc;
    if (cdb_len == 0 || cdb_len > BOT_MAX_CMDLEN)
        return -EINVAL;
    mutex_lock(&m->lock);
    bool in = !write && len;
    int r = bot_send_command(m, cdb, cdb_len, in, len, l->lun);
    if (r == 0) {
        uint32_t done = 0;
        if (len)
            bot_data(m, in, buf, len, &done);
        uint8_t status = BOT_COMMAND_ERROR;
        r = bot_get_status(m, &status);
        if (r == 0 && status == BOT_COMMAND_FAILED)
            r = SCSI_CHECK_CONDITION;
        else if (r == 0 && done < len && write)
            r = -EIO;
    }
    if (r < 0 && m->usb->gone)
        r = -ENODEV;
    mutex_unlock(&m->lock);
    return r;
}

/* GET MAX LUN (UsbBotGetMaxLun). A device that stalls the request, or
 * reports more than 15, has one logical unit. */
static unsigned bot_get_max_lun(struct msc_dev *m)
{
    uint8_t max = 0;
    int r = xhci_control(m->usb, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE, BOT_GETLUN_REQUEST, 0,
                         m->interface, &max, 1);
    if (r != 1 || max > BOT_MAX_LUN)
        max = 0;
    return max;
}

/* The bulk endpoints of the interface at intf and their SuperSpeed
 * companion descriptors (UsbBotInit). */
static int find_endpoints(struct msc_dev *m, const uint8_t *cfg, unsigned cfg_len,
                          const struct usb_interface_descriptor *intf)
{
    const struct usb_endpoint_descriptor *in = NULL, *out = NULL;
    unsigned burst_in = 0, burst_out = 0, *last = NULL;
    for (unsigned off = (unsigned)((const uint8_t *)intf - cfg) + intf->bLength; off + 2 <= cfg_len && cfg[off] >= 2;
         off += cfg[off]) {
        if (cfg[off + 1] == USB_DT_INTERFACE)
            break;
        if (cfg[off + 1] == USB_DT_SS_EP_COMPANION && last && off + 3 <= cfg_len) {
            *last = cfg[off + 2];
            last = NULL;
            continue;
        }
        if (cfg[off + 1] != USB_DT_ENDPOINT || off + sizeof(struct usb_endpoint_descriptor) > cfg_len)
            continue;
        const struct usb_endpoint_descriptor *e = (const void *)(cfg + off);
        last = NULL;
        if ((e->bmAttributes & USB_EP_XFER_MASK) != USB_EP_XFER_BULK)
            continue;
        if ((e->bEndpointAddress & USB_DIR_IN) && !in) {
            in = e;
            last = &burst_in;
        } else if (!(e->bEndpointAddress & USB_DIR_IN) && !out) {
            out = e;
            last = &burst_out;
        }
    }
    if (!in || !out)
        return -ENODEV;
    int r = xhci_bulk_open(m->usb, in, burst_in);
    if (r == 0)
        r = xhci_bulk_open(m->usb, out, burst_out);
    m->ep_in = in->bEndpointAddress;
    m->ep_out = out->bEndpointAddress;
    return r;
}

int msc_probe(struct usb_device *dev, const uint8_t *cfg, unsigned cfg_len,
              const struct usb_interface_descriptor *intf)
{
    if (intf->bInterfaceSubClass != MSC_SUBCLASS_SCSI || intf->bInterfaceProtocol != MSC_PROTOCOL_BOT) {
        klog_info("port %s: mass storage subclass %02x protocol %02x not supported", dev->path,
                  intf->bInterfaceSubClass, intf->bInterfaceProtocol);
        return -ENODEV;
    }
    if (dev->msc)
        return -EBUSY;
    struct msc_dev *m = kzalloc(sizeof *m);
    if (!m)
        return -ENOMEM;
    m->usb = dev;
    m->interface = intf->bInterfaceNumber;
    m->tag = 1;
    mutex_init(&m->lock, "msc");
    int r = find_endpoints(m, cfg, cfg_len, intf);
    if (r < 0) {
        klog_warn("port %s: no bulk endpoints", dev->path);
        kfree(m);
        return r;
    }
    unsigned max_lun = bot_get_max_lun(m);
    for (unsigned lun = 0; lun <= max_lun; lun++) {
        struct msc_lun *l = &m->luns[m->nluns];
        l->msc = m;
        l->lun = (uint8_t)lun;
        l->scsi = scsi_attach(bot_command, l, XHCI_BULK_MAX, "usb");
        if (l->scsi)
            m->nluns++;
    }
    if (!m->nluns) {
        klog_warn("port %s: no usable logical unit", dev->path);
        kfree(m);
        return -ENODEV;
    }
    klog_info("port %s: %s: bulk-only mass storage, %u logical unit%s", dev->path, dev->product, m->nluns,
              m->nluns == 1 ? "" : "s");
    dev->msc = m;
    return 0;
}

void msc_disconnect(struct usb_device *dev)
{
    struct msc_dev *m = dev->msc;
    /* The device is marked gone, and a command in progress ends with
     * ENODEV. scsi_detach waits for it, and no command starts afterwards. */
    for (unsigned i = 0; i < m->nluns; i++)
        scsi_detach(m->luns[i].scsi);
    dev->msc = NULL;
    kfree(m);
}
