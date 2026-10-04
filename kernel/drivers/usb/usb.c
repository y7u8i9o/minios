/* The USB core (D2, docs/design/usb.md): the descriptors of a device that
 * has an address, its configuration and the binding of the class drivers.
 * Every function here runs in the thread of the device's controller. */
#define KLOG_SUBSYS "usb"
#include "usb.h"
#include <drivers/usb.h>
#include <drivers/devinfo.h>
#include <lib/endian.h>
#include <mm/slab.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

static unsigned ndevices;               /* devices with an address, changed atomically */

void usb_count_device(int delta)
{
    __atomic_add_fetch(&ndevices, (unsigned)delta, __ATOMIC_RELAXED);
}

unsigned usb_device_count(void)
{
    return __atomic_load_n(&ndevices, __ATOMIC_RELAXED);
}

const char *usb_speed_name(enum usb_speed s)
{
    switch (s) {
    case USB_SPEED_LOW:        return "low";
    case USB_SPEED_FULL:       return "full";
    case USB_SPEED_HIGH:       return "high";
    case USB_SPEED_SUPER:      return "super";
    case USB_SPEED_SUPER_PLUS: return "super plus";
    default:                   return "unknown";
    }
}

/* A string descriptor in the first language of the device, reduced to
 * ASCII. Characters outside ASCII become '?'. Returns false when the
 * device has no such string. */
static bool read_string(struct usb_device *dev, uint8_t index, char *out, size_t size)
{
    uint8_t buf[256];
    if (!index || size == 0)
        return false;
    memset(buf, 0, sizeof buf);
    if (xhci_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0, buf, 255) < 0 ||
        buf[0] < 4 || buf[1] != USB_DT_STRING)
        return false;
    uint16_t lang = get_le16(buf + 2);
    memset(buf, 0, sizeof buf);
    if (xhci_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DT_STRING << 8 | index), lang,
                     buf, 255) < 0 || buf[0] < 2 || buf[1] != USB_DT_STRING)
        return false;
    size_t n = 0;
    for (unsigned i = 2; i + 1 < buf[0] && n + 1 < size; i += 2) {
        uint16_t c = (uint16_t)(buf[i] | buf[i + 1] << 8);
        out[n++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = '\0';
    return n > 0;
}

int usb_enumerate(struct usb_device *dev)
{
    int r = xhci_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &dev->desc,
                         sizeof dev->desc);
    if (r < 0 || dev->desc.bDescriptorType != USB_DT_DEVICE) {
        klog_warn("port %u: no device descriptor", dev->port);
        return r < 0 ? r : -EIO;
    }
    if (!read_string(dev, dev->desc.iProduct, dev->product, sizeof dev->product))
        ksnprintf(dev->product, sizeof dev->product, "USB device %04x:%04x", dev->desc.idVendor,
                  dev->desc.idProduct);
    read_string(dev, dev->desc.iManufacturer, dev->manufacturer, sizeof dev->manufacturer);
    read_string(dev, dev->desc.iSerialNumber, dev->serial, sizeof dev->serial);

    struct usb_config_descriptor head;
    r = xhci_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &head, sizeof head);
    if (r < 0 || head.bDescriptorType != USB_DT_CONFIG || head.wTotalLength < sizeof head) {
        klog_warn("port %u: %s: no configuration descriptor", dev->port, dev->product);
        return r < 0 ? r : -EIO;
    }
    unsigned total = MIN((unsigned)head.wTotalLength, 4096u);
    uint8_t *cfg = kmalloc(total);
    if (!cfg)
        return -ENOMEM;
    r = xhci_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, cfg, (uint16_t)total);
    if (r >= 0)
        r = xhci_control(dev, USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION,
                         head.bConfigurationValue, 0, NULL, 0);
    if (r < 0) {
        klog_warn("port %u: %s: configuration %u not set: %d", dev->port, dev->product,
                  head.bConfigurationValue, r);
        kfree(cfg);
        return r;
    }
    klog_info("port %u: %s, %04x:%04x, %s speed, slot %u, %u interface%s", dev->port, dev->product,
              dev->desc.idVendor, dev->desc.idProduct, usb_speed_name(dev->speed), dev->slot,
              head.bNumInterfaces, head.bNumInterfaces == 1 ? "" : "s");

    /* The first alternate setting of every interface. */
    unsigned bound = 0;
    for (unsigned off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        if (cfg[off + 1] != USB_DT_INTERFACE || off + sizeof(struct usb_interface_descriptor) > total)
            continue;
        const struct usb_interface_descriptor *intf = (const void *)(cfg + off);
        if (intf->bAlternateSetting != 0)
            continue;
        if (intf->bInterfaceClass == USB_CLASS_HID && hid_probe(dev, cfg, total, intf) == 0)
            bound++;
    }
    if (!bound)
        klog_info("port %u: %s: no driver", dev->port, dev->product);
    /* The descriptor is retained for /dev/devices and freed with the
     * device. */
    dev->config = cfg;
    dev->config_len = total;
    return 0;
}

void usb_disconnect(struct usb_device *dev)
{
    for (unsigned i = 0; i < USB_MAX_HID; i++) {
        if (dev->hid[i])
            hid_disconnect(dev->hid[i]);
        dev->hid[i] = NULL;
    }
}

/* ---- /dev/devices (docs/design/sysinfo.md) ---- */

static const char *class_name(uint8_t c)
{
    switch (c) {
    case 0x00: return "defined by the interfaces";
    case 0x01: return "audio";
    case 0x02: return "communications";
    case 0x03: return "HID";
    case 0x05: return "physical";
    case 0x06: return "image";
    case 0x07: return "printer";
    case 0x08: return "mass storage";
    case 0x09: return "hub";
    case 0x0a: return "CDC data";
    case 0x0b: return "smart card";
    case 0x0d: return "content security";
    case 0x0e: return "video";
    case 0x0f: return "personal healthcare";
    case 0x10: return "audio/video";
    case 0x11: return "billboard";
    case 0x12: return "USB Type-C bridge";
    case 0xdc: return "diagnostic";
    case 0xe0: return "wireless controller";
    case 0xef: return "miscellaneous";
    case 0xfe: return "application specific";
    case 0xff: return "vendor specific";
    default:   return "unknown";
    }
}

static const char *hid_kind(const struct usb_interface_descriptor *i)
{
    if (i->bInterfaceClass != USB_CLASS_HID || i->bInterfaceSubClass != 1)
        return "";
    return i->bInterfaceProtocol == 1 ? ", boot keyboard" : i->bInterfaceProtocol == 2 ? ", boot mouse" : ", boot";
}

static const char *transfer_type(uint8_t attributes)
{
    static const char *const types[] = { "control", "isochronous", "bulk", "interrupt" };
    return types[attributes & 3];
}

void usb_describe_device(struct devinfo *d, const char *path, const struct usb_device *dev)
{
    devinfo_node(d, path, "%s", dev->product);
    devinfo_prop(d, "product", "%s", dev->product);
    if (dev->manufacturer[0])
        devinfo_prop(d, "manufacturer", "%s", dev->manufacturer);
    if (dev->serial[0])
        devinfo_prop(d, "serial", "%s", dev->serial);
    devinfo_prop(d, "vendor_id", "%04x", dev->desc.idVendor);
    devinfo_prop(d, "product_id", "%04x", dev->desc.idProduct);
    devinfo_prop(d, "device_release", "%x.%02x", dev->desc.bcdDevice >> 8, dev->desc.bcdDevice & 0xff);
    devinfo_prop(d, "usb_version", "%x.%02x", dev->desc.bcdUSB >> 8, dev->desc.bcdUSB & 0xff);
    devinfo_prop(d, "speed", "%s", usb_speed_name(dev->speed));
    devinfo_prop(d, "slot", "%u", dev->slot);
    devinfo_prop(d, "device_class", "%02x:%02x:%02x (%s)", dev->desc.bDeviceClass, dev->desc.bDeviceSubClass,
                 dev->desc.bDeviceProtocol, class_name(dev->desc.bDeviceClass));
    devinfo_prop(d, "max_packet_size_0", "%u bytes",
                 dev->speed == USB_SPEED_SUPER ? 1u << dev->desc.bMaxPacketSize0 : dev->desc.bMaxPacketSize0);
    devinfo_prop(d, "configurations", "%u", dev->desc.bNumConfigurations);
    if (!dev->config || dev->config_len < sizeof(struct usb_config_descriptor))
        return;
    const struct usb_config_descriptor *c = (const void *)dev->config;
    devinfo_prop(d, "configuration", "%u", c->bConfigurationValue);
    devinfo_prop(d, "interfaces", "%u", c->bNumInterfaces);
    devinfo_prop(d, "power", "%s%s, %u mA maximum", (c->bmAttributes & 0x40) ? "self powered" : "bus powered",
                 (c->bmAttributes & 0x20) ? ", remote wakeup" : "",
                 c->bMaxPower * (dev->speed == USB_SPEED_SUPER ? 8u : 2u));
    const uint8_t *cfg = dev->config;
    char ipath[96];
    for (unsigned off = 0; off + 2 <= dev->config_len && cfg[off] >= 2; off += cfg[off]) {
        if (cfg[off + 1] == USB_DT_INTERFACE && off + sizeof(struct usb_interface_descriptor) <= dev->config_len) {
            const struct usb_interface_descriptor *i = (const void *)(cfg + off);
            ksnprintf(ipath, sizeof ipath, "%s/if%u.%u", path, i->bInterfaceNumber, i->bAlternateSetting);
            devinfo_node(d, ipath, "Interface %u, %s%s", i->bInterfaceNumber, class_name(i->bInterfaceClass),
                         hid_kind(i));
            devinfo_prop(d, "interface", "%u", i->bInterfaceNumber);
            devinfo_prop(d, "alternate_setting", "%u", i->bAlternateSetting);
            devinfo_prop(d, "interface_class", "%02x:%02x:%02x (%s%s)", i->bInterfaceClass, i->bInterfaceSubClass,
                         i->bInterfaceProtocol, class_name(i->bInterfaceClass), hid_kind(i));
            devinfo_prop(d, "endpoints", "%u", i->bNumEndpoints);
            const struct hid_dev *h = NULL;
            for (unsigned k = 0; k < USB_MAX_HID; k++)
                if (dev->hid[k] && hid_interface(dev->hid[k]) == i->bInterfaceNumber && i->bAlternateSetting == 0)
                    h = dev->hid[k];
            devinfo_prop(d, "driver", "%s", h ? "usb-hid" : "none");
            if (h)
                hid_describe(d, h);
        } else if (cfg[off + 1] == USB_DT_ENDPOINT && off + sizeof(struct usb_endpoint_descriptor) <= dev->config_len) {
            const struct usb_endpoint_descriptor *e = (const void *)(cfg + off);
            char key[24];
            ksnprintf(key, sizeof key, "endpoint_%02x", e->bEndpointAddress);
            devinfo_prop(d, key, "%s %s, %u bytes, interval %u", transfer_type(e->bmAttributes),
                         (e->bEndpointAddress & USB_DIR_IN) ? "IN" : "OUT", e->wMaxPacketSize & 0x7ff, e->bInterval);
        }
    }
}

