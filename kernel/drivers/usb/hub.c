/* USB hubs (R4 of docs/plan/release-0.5.0.md, D5 of docs/plan/drivers.md,
 * docs/design/usb.md).
 *
 * Adapted from MdeModulePkg/Bus/Usb/UsbBusDxe/UsbHub.c, UsbHub.h and the
 * port enumeration of UsbEnumer.c (UsbEnumeratePort, UsbEnumerateNewDev)
 * of edk2, revision 999fd0f12a27709eee04b93e46bd867e6b0163a5
 * (third_party/edk2/README):
 *
 *   Copyright (c) 2007 - 2018, Intel Corporation. All rights reserved.
 *   SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * The licence text is third_party/edk2/License.txt.
 *
 * The hub class requests, the hub initialization with its port power and
 * power-on delay, the hub depth of a SuperSpeed hub, the port reset, the
 * acknowledgement of the change bits and the handling of a port change
 * follow edk2. The root hub parts of edk2 are left out, since the xHCI
 * driver handles its root ports itself. The adaptation runs the port
 * changes in the controller thread instead of a UEFI event, gives the
 * xHCI slot of the hub its hub fields, and enumerates the ports present
 * when the hub binds. Ports above 15, which a route string cannot name,
 * are not used. */
#define KLOG_SUBSYS "usb-hub"
#include "usb.h"
#include <drivers/devinfo.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define USB_DESC_TYPE_HUB               0x29
#define USB_DESC_TYPE_HUB_SUPER_SPEED   0x2a

/* Class requests (USB 2.0 11.24.2, USB 3.2 10.16.2). */
#define HUB_REQ_GET_STATUS      0
#define HUB_REQ_CLEAR_FEATURE   1
#define HUB_REQ_SET_FEATURE     3
#define HUB_REQ_GET_DESC        6
#define HUB_REQ_SET_DEPTH       12
#define HUB_TARGET_HUB          (USB_TYPE_CLASS | 0x00)
#define HUB_TARGET_PORT         (USB_TYPE_CLASS | 0x03)

/* Feature selectors. */
#define C_HUB_LOCAL_POWER       0
#define C_HUB_OVER_CURRENT      1
#define PORT_RESET              4
#define PORT_POWER              8
#define C_PORT_CONNECTION       16
#define C_PORT_ENABLE           17
#define C_PORT_SUSPEND          18
#define C_PORT_OVER_CURRENT     19
#define C_PORT_RESET            20
#define C_PORT_LINK_STATE       25
#define C_PORT_CONFIG_ERROR     26
#define C_BH_PORT_RESET         29

/* Port status and change bits. */
#define PS_CONNECTION           0x0001
#define PS_ENABLE               0x0002
#define PS_OVER_CURRENT         0x0008
#define PS_LOW_SPEED            0x0200
#define PS_HIGH_SPEED           0x0400
#define PC_CONNECTION           0x0001
#define PC_ENABLE               0x0002
#define PC_SUSPEND              0x0004
#define PC_OVER_CURRENT         0x0008
#define PC_RESET                0x0010
#define PC_BH_RESET             0x0020          /* SuperSpeed hubs */
#define PC_LINK_STATE           0x0040
#define PC_CONFIG_ERROR         0x0080
#define HS_C_LOCAL_POWER        0x0001
#define HS_C_OVER_CURRENT       0x0002

#define HUB_MAX_PORTS           15
#define WAIT_PORT_STABLE_MS     100             /* USB_WAIT_PORT_STABLE_STALL */
#define PORT_RESET_MS           20              /* USB_SET_PORT_RESET_STALL */
#define PORT_RECOVERY_MS        10              /* USB_SET_PORT_RECOVERY_STALL */
#define PORT_CHANGE_WAIT_MS     500             /* USB_WAIT_PORT_STS_CHANGE_LOOP of 100 us */
#define ENUM_RETRIES            3

/* A hub. change is written by the report callback of the status change
 * endpoint under xhci.lock and taken by the controller thread with an
 * atomic exchange. child[] is written by the controller thread under
 * usb_topology_lock and read by /dev/devices under it. The other fields
 * are set by hub_probe. */
struct usb_hub {
    struct usb_device *dev;
    unsigned nports;
    bool superspeed;
    unsigned power_good_ms;
    uint8_t ep;
    uint32_t change;                    /* bit 0: the hub, bit n: port n */
    struct usb_device *child[HUB_MAX_PORTS + 1];
};

static int hub_request(struct usb_hub *h, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                       void *data, uint16_t len)
{
    int r = xhci_control(h->dev, type, request, value, index, data, len);
    return r < 0 ? r : 0;
}

static int set_port_feature(struct usb_hub *h, unsigned port, uint16_t feature)
{
    return hub_request(h, HUB_TARGET_PORT, HUB_REQ_SET_FEATURE, feature, (uint16_t)port, NULL, 0);
}

static int clear_port_feature(struct usb_hub *h, unsigned port, uint16_t feature)
{
    return hub_request(h, HUB_TARGET_PORT, HUB_REQ_CLEAR_FEATURE, feature, (uint16_t)port, NULL, 0);
}

/* GET_STATUS of a port: the status in the low half, the changes in the
 * high half (UsbHubCtrlGetPortStatus). */
static int get_port_status(struct usb_hub *h, unsigned port, uint16_t *status, uint16_t *change)
{
    uint8_t buf[4];
    int r = hub_request(h, USB_DIR_IN | HUB_TARGET_PORT, HUB_REQ_GET_STATUS, 0, (uint16_t)port, buf, 4);
    if (r < 0)
        return r;
    *status = (uint16_t)(buf[0] | buf[1] << 8);
    *change = (uint16_t)(buf[2] | buf[3] << 8);
    return 0;
}

/* Acknowledge the change bits of the hub itself (UsbHubAckHubStatus). */
static void ack_hub_status(struct usb_hub *h)
{
    uint8_t buf[4];
    if (hub_request(h, USB_DIR_IN | HUB_TARGET_HUB, HUB_REQ_GET_STATUS, 0, 0, buf, 4) < 0)
        return;
    uint16_t change = (uint16_t)(buf[2] | buf[3] << 8);
    if (change & HS_C_LOCAL_POWER)
        hub_request(h, HUB_TARGET_HUB, HUB_REQ_CLEAR_FEATURE, C_HUB_LOCAL_POWER, 0, NULL, 0);
    if (change & HS_C_OVER_CURRENT)
        hub_request(h, HUB_TARGET_HUB, HUB_REQ_CLEAR_FEATURE, C_HUB_OVER_CURRENT, 0, NULL, 0);
}

/* Clear every change bit that a port reports (UsbHubClearPortChange). A
 * USB 2 hub reports enable and suspend changes, a SuperSpeed hub warm
 * reset, link state and configuration error changes instead. */
struct change_feature {
    uint16_t bit, feature;
};

static const struct change_feature usb2_changes[] = {
    { PC_CONNECTION, C_PORT_CONNECTION }, { PC_ENABLE, C_PORT_ENABLE },  { PC_SUSPEND, C_PORT_SUSPEND },
    { PC_OVER_CURRENT, C_PORT_OVER_CURRENT }, { PC_RESET, C_PORT_RESET },
};

static const struct change_feature ss_changes[] = {
    { PC_CONNECTION, C_PORT_CONNECTION },   { PC_OVER_CURRENT, C_PORT_OVER_CURRENT },
    { PC_RESET, C_PORT_RESET },             { PC_BH_RESET, C_BH_PORT_RESET },
    { PC_LINK_STATE, C_PORT_LINK_STATE },   { PC_CONFIG_ERROR, C_PORT_CONFIG_ERROR },
};

static void clear_port_change(struct usb_hub *h, unsigned port)
{
    uint16_t status, change;
    if (get_port_status(h, port, &status, &change) < 0)
        return;
    const struct change_feature *map = h->superspeed ? ss_changes : usb2_changes;
    size_t n = h->superspeed ? ARRAY_SIZE(ss_changes) : ARRAY_SIZE(usb2_changes);
    for (size_t i = 0; i < n; i++)
        if (change & map[i].bit)
            clear_port_feature(h, port, map[i].feature);
}

/* Reset a port and wait for the reset change (UsbHubResetPort). */
static int reset_port(struct usb_hub *h, unsigned port)
{
    int r = set_port_feature(h, port, PORT_RESET);
    if (r < 0)
        return r;
    sleep_ms(PORT_RESET_MS);
    for (unsigned t = 0; t < PORT_CHANGE_WAIT_MS; t++) {
        uint16_t status, change;
        if ((r = get_port_status(h, port, &status, &change)) < 0)
            return r;
        if (change & PC_RESET) {
            clear_port_feature(h, port, C_PORT_RESET);
            sleep_ms(PORT_RECOVERY_MS);
            return 0;
        }
        sleep_ms(1);
    }
    return -ETIMEDOUT;
}

/* Remove the device of a port, with the devices behind it. */
static void remove_child(struct usb_hub *h, unsigned port)
{
    struct usb_device *child = h->child[port];
    if (!child)
        return;
    mutex_lock(&usb_topology_lock);
    h->child[port] = NULL;
    mutex_unlock(&usb_topology_lock);
    xhci_detach(child);
}

/* Enumerate the device that connected to a port (UsbEnumerateNewDev). The
 * port is reset unless it reports a completed reset. The speed comes from
 * the port status, and an enumeration that fails is repeated after a
 * further reset while the device remains connected. */
static void new_device(struct usb_hub *h, unsigned port, bool reset)
{
    sleep_ms(WAIT_PORT_STABLE_MS);
    for (unsigned attempt = 0; attempt < ENUM_RETRIES; attempt++) {
        if (reset && reset_port(h, port) < 0) {
            klog_warn("port %s.%u: reset failed", h->dev->path, port);
            return;
        }
        reset = true;
        uint16_t status, change;
        if (get_port_status(h, port, &status, &change) < 0 || !(status & PS_CONNECTION))
            return;
        if (!h->superspeed && !(status & PS_ENABLE))
            continue;
        enum usb_speed speed = h->superspeed ? USB_SPEED_SUPER
                               : (status & PS_LOW_SPEED) ? USB_SPEED_LOW
                               : (status & PS_HIGH_SPEED) ? USB_SPEED_HIGH : USB_SPEED_FULL;
        struct usb_device *child = xhci_attach(h->dev, port, speed);
        if (child) {
            mutex_lock(&usb_topology_lock);
            h->child[port] = child;
            mutex_unlock(&usb_topology_lock);
            return;
        }
        sleep_ms(WAIT_PORT_STABLE_MS);
    }
    klog_warn("port %s.%u: the device does not enumerate", h->dev->path, port);
}

/* Bring a port in line with its status (UsbEnumeratePort): a change of the
 * connection, of the enable state, of the over-current state or a
 * completed reset removes the device of the port and enumerates a
 * connected one again. force does so without a change, for the ports
 * present when the hub binds. The change bits are cleared afterwards. */
static void enumerate_port(struct usb_hub *h, unsigned port, bool force)
{
    uint16_t status, change;
    if (get_port_status(h, port, &status, &change) < 0)
        return;
    if (!force && !(change & (PC_CONNECTION | PC_ENABLE | PC_OVER_CURRENT | PC_RESET))) {
        clear_port_change(h, port);
        return;
    }
    if ((change & PC_OVER_CURRENT) && (status & PS_OVER_CURRENT)) {
        /* Over-current, probably a short circuit, is left to the hardware. */
        klog_warn("port %s.%u: over-current", h->dev->path, port);
        clear_port_change(h, port);
        return;
    }
    if (force && !(status & PS_CONNECTION)) {
        clear_port_change(h, port);
        return;
    }
    remove_child(h, port);
    if (status & PS_CONNECTION)
        new_device(h, port, !(change & PC_RESET));
    /* The changes are acknowledged after the enumeration, as in edk2,
     * which also clears the changes that the port reset caused. */
    clear_port_change(h, port);
}

/* The report of the status change endpoint, under xhci.lock: one bit for
 * the hub and one for each port. */
static void hub_report(void *arg, const uint8_t *data, unsigned len)
{
    struct usb_hub *h = arg;
    uint32_t bits = 0;
    for (unsigned i = 0; i < len && i < 4; i++)
        bits |= (uint32_t)data[i] << (8 * i);
    __atomic_fetch_or(&h->change, bits, __ATOMIC_RELAXED);
}

void hub_service(struct usb_device *dev)
{
    struct usb_hub *h = dev->hub;
    uint32_t bits = __atomic_exchange_n(&h->change, 0, __ATOMIC_ACQ_REL);
    if (bits & 1)
        ack_hub_status(h);
    for (unsigned port = 1; port <= h->nports; port++)
        if (bits & (1u << port))
            enumerate_port(h, port, false);
}

/* Bind a hub interface (UsbHubInit). */
int hub_probe(struct usb_device *dev, const uint8_t *cfg, unsigned cfg_len,
              const struct usb_interface_descriptor *intf)
{
    if (dev->hub)
        return -EBUSY;
    if (dev->depth >= 5) {
        klog_warn("port %s: hub beyond the fifth tier not supported", dev->path);
        return -ENODEV;
    }
    /* The interrupt IN endpoint of the port change map. */
    const struct usb_endpoint_descriptor *ep = NULL;
    for (unsigned off = (unsigned)((const uint8_t *)intf - cfg) + intf->bLength; off + 2 <= cfg_len && cfg[off] >= 2;
         off += cfg[off]) {
        if (cfg[off + 1] == USB_DT_INTERFACE)
            break;
        if (cfg[off + 1] == USB_DT_ENDPOINT && off + sizeof *ep <= cfg_len) {
            const struct usb_endpoint_descriptor *e = (const void *)(cfg + off);
            if ((e->bEndpointAddress & USB_DIR_IN) && (e->bmAttributes & USB_EP_XFER_MASK) == USB_EP_XFER_INT) {
                ep = e;
                break;
            }
        }
    }
    if (!ep)
        return -ENODEV;
    struct usb_hub *h = kzalloc(sizeof *h);
    if (!h)
        return -ENOMEM;
    h->dev = dev;
    h->superspeed = dev->speed >= USB_SPEED_SUPER;
    h->ep = ep->bEndpointAddress;

    /* The hub descriptor: its length first, then the whole descriptor
     * (UsbHubReadDesc). */
    uint8_t desc[72] = { 0 };
    uint16_t type = (uint16_t)((h->superspeed ? USB_DESC_TYPE_HUB_SUPER_SPEED : USB_DESC_TYPE_HUB) << 8);
    int r = hub_request(h, USB_DIR_IN | HUB_TARGET_HUB, HUB_REQ_GET_DESC, type, 0, desc, 2);
    if (r == 0)
        r = hub_request(h, USB_DIR_IN | HUB_TARGET_HUB, HUB_REQ_GET_DESC, type, 0, desc,
                        MIN((uint16_t)desc[0], (uint16_t)sizeof desc));
    if (r < 0 || desc[0] < 7) {
        klog_warn("port %s: no hub descriptor", dev->path);
        kfree(h);
        return r < 0 ? r : -EIO;
    }
    h->nports = desc[2];
    if (h->nports > HUB_MAX_PORTS) {
        klog_warn("port %s: hub with %u ports, the ports above %u are not used", dev->path, h->nports,
                  HUB_MAX_PORTS);
        h->nports = HUB_MAX_PORTS;
    }
    uint16_t characteristics = (uint16_t)(desc[3] | desc[4] << 8);
    h->power_good_ms = desc[5] * 2u;
    unsigned ttt = dev->speed == USB_SPEED_HIGH ? (characteristics >> 5) & 3 : 0;
    if ((r = xhci_hub_configure(dev, desc[2], ttt, false)) < 0) {
        kfree(h);
        return r;
    }
    if (h->superspeed)
        hub_request(h, HUB_TARGET_HUB, HUB_REQ_SET_DEPTH, (uint16_t)dev->depth, 0, NULL, 0);
    /* Power every port, which serves ganged and individually switched
     * hubs, and wait the power-on time of the descriptor. */
    for (unsigned port = 1; port <= h->nports; port++)
        set_port_feature(h, port, PORT_POWER);
    sleep_ms(MAX(h->power_good_ms, 20u));
    ack_hub_status(h);
    dev->hub = h;
    klog_info("port %s: hub with %u ports, %s", dev->path, h->nports,
              h->superspeed ? "SuperSpeed" : (characteristics & 3) == 0 ? "ganged power" : "port power");

    for (unsigned port = 1; port <= h->nports; port++)
        enumerate_port(h, port, true);
    r = xhci_interrupt_in(dev, ep, h->nports / 8 + 1, hub_report, h);
    if (r < 0)
        klog_warn("port %s: status change endpoint not started (%d), later changes are not seen", dev->path, r);
    return 0;
}

void hub_disconnect(struct usb_device *dev)
{
    struct usb_hub *h = dev->hub;
    for (unsigned port = 1; port <= h->nports; port++)
        remove_child(h, port);
    dev->hub = NULL;
    kfree(h);
}

void hub_describe(struct devinfo *d, const char *path, const struct usb_device *dev)
{
    const struct usb_hub *h = dev->hub;
    for (unsigned port = 1; port <= h->nports; port++) {
        if (!h->child[port])
            continue;
        char cpath[128];
        ksnprintf(cpath, sizeof cpath, "%s/port%u", path, port);
        usb_describe_device(d, cpath, h->child[port]);
    }
}
