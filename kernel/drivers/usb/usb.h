#pragma once
/* Declarations shared by the xHCI driver (xhci.c), the USB core (usb.c)
 * and the HID class driver (hid.c). docs/design/usb.md describes the
 * design. */
#include <kernel.h>
#include <sync/mutex.h>

/* Standard requests and descriptor types (USB 2.0 chapter 9). */
#define USB_DIR_IN              0x80
#define USB_TYPE_STANDARD       0x00
#define USB_TYPE_CLASS          0x20
#define USB_RECIP_DEVICE        0x00
#define USB_RECIP_INTERFACE     0x01

#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIGURATION 0x09

#define USB_DT_DEVICE           0x01
#define USB_DT_CONFIG           0x02
#define USB_DT_STRING           0x03
#define USB_DT_INTERFACE        0x04
#define USB_DT_ENDPOINT         0x05
#define USB_DT_HID              0x21
#define USB_DT_REPORT           0x22

#define USB_CLASS_HID           0x03
#define USB_CLASS_MASS_STORAGE  0x08
#define USB_CLASS_HUB           0x09
#define USB_RECIP_ENDPOINT      0x02
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_FEATURE_ENDPOINT_HALT 0
#define USB_DT_SS_EP_COMPANION  0x30
#define USB_EP_XFER_BULK        0x02

#define USB_EP_XFER_MASK        0x03
#define USB_EP_XFER_INT         0x03

struct usb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __packed;

struct usb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __packed;

struct usb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __packed;

struct usb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __packed;

struct usb_hid_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __packed;

enum usb_speed {
    USB_SPEED_FULL = 1,
    USB_SPEED_LOW = 2,
    USB_SPEED_HIGH = 3,
    USB_SPEED_SUPER = 4,
    USB_SPEED_SUPER_PLUS = 5,
};

/* The name of a speed: "low", "full", "high", "super" or "super plus". */
const char *usb_speed_name(enum usb_speed s);

struct xhci;
struct hid_dev;
struct usb_hub;
struct msc_dev;
struct devinfo;

#define USB_MAX_HID 4                   /* HID interfaces bound per device */

/* A device with an address. It is created by the controller thread when a
 * port of the root hub or of a hub connects and destroyed when the port
 * disconnects. The fields, hid[] included, are read and written by that
 * thread only, except gone, which xhci.lock protects. The report callbacks
 * receive their struct hid_dev as their argument. */
struct usb_device {
    struct xhci *hc;
    unsigned slot;                      /* the xHCI slot ID */
    unsigned port;                      /* the root hub port, from 1 */
    struct usb_device *parent;          /* the hub the device is connected to, NULL on a root port */
    unsigned parent_port;               /* the port of the parent hub, from 1 */
    unsigned depth;                     /* 0 on a root port, 1 behind one hub, ... */
    uint32_t route;                     /* the route string of the slot context */
    unsigned tt_slot, tt_port;          /* the transaction translator of a low or full speed device */
    char path[24];                      /* "2" on root port 2, "2.3" on port 3 of a hub on root port 2 */
    bool gone;                          /* the device was disconnected; transfers end with ENODEV */
    enum usb_speed speed;
    struct usb_device_descriptor desc;
    char product[64];                   /* the product string, or a name made from the IDs */
    char manufacturer[64];              /* the manufacturer string, or "" */
    char serial[64];                    /* the serial number string, or "" */
    uint8_t *config;                    /* the first configuration descriptor, kmalloc'd, or NULL */
    unsigned config_len;
    struct hid_dev *hid[USB_MAX_HID];
    struct usb_hub *hub;                /* the hub driver of a hub */
    struct msc_dev *msc;                /* the mass storage driver */
};

/* Called by the controller with each completed report of an interrupt IN
 * endpoint, under xhci.lock, from the interrupt handler or from the
 * controller thread. len is the number of bytes the device sent. */
typedef void (*usb_report_fn)(void *arg, const uint8_t *data, unsigned len);

/* Controller functions for the USB core and the class drivers. They are
 * called from the controller thread only, which issues every command and
 * every control transfer of its controller. */

/* A control transfer on endpoint 0. data is NULL when len is 0, and len is
 * at most 4096. Returns the number of bytes transferred, -EPIPE on a stall,
 * -ETIMEDOUT or -EIO. */
int xhci_control(struct usb_device *dev, uint8_t request_type, uint8_t request, uint16_t value,
                 uint16_t index, void *data, uint16_t len);
/* Configure the interrupt IN endpoint ep and start polling it with
 * transfers of len bytes (at most 1024). Every completed transfer is passed
 * to fn. */
int xhci_interrupt_in(struct usb_device *dev, const struct usb_endpoint_descriptor *ep, unsigned len,
                      usb_report_fn fn, void *arg);

/* Configure a bulk endpoint for xhci_bulk. max_burst comes from the
 * SuperSpeed endpoint companion descriptor, 0 without one. Called by the
 * controller thread while a class driver binds. */
int xhci_bulk_open(struct usb_device *dev, const struct usb_endpoint_descriptor *ep, unsigned max_burst);
/* One bulk transfer of at most XHCI_BULK_MAX bytes on an endpoint of
 * xhci_bulk_open. *actual receives the bytes transferred. Returns 0,
 * -EPIPE when the endpoint stalled (xhci_clear_halt clears it),
 * -ETIMEDOUT, -ENODEV for a disconnected device or -EIO. Called from any
 * thread; the class driver serializes the transfers of one endpoint. */
#define XHCI_BULK_MAX 65536
int xhci_bulk(struct usb_device *dev, uint8_t ep_addr, void *buf, uint32_t len, unsigned timeout_ms,
              uint32_t *actual);
/* Clear a halted bulk endpoint in the controller and in the device
 * (CLEAR_FEATURE ENDPOINT_HALT). */
int xhci_clear_halt(struct usb_device *dev, uint8_t ep_addr);
/* Mark the slot of dev as a hub with nports ports, the think time ttt and
 * multiple transaction translators when mtt is set (xHCI 4.6.7). */
int xhci_hub_configure(struct usb_device *dev, unsigned nports, unsigned ttt, bool mtt);
/* Give the device on port of the hub parent an address and enumerate it.
 * Returns the device, or NULL. Called by the hub driver in the controller
 * thread. */
struct usb_device *xhci_attach(struct usb_device *parent, unsigned port, enum usb_speed speed);
/* Remove a device whose hub port disconnected, with the devices behind
 * it. Called by the hub driver in the controller thread. */
void xhci_detach(struct usb_device *dev);

/* The devices of the root ports and of the hubs. A controller thread takes
 * it to publish a device after its enumeration and to remove it before its
 * slot is freed, and /dev/devices takes it while it reads the topology. */
extern struct mutex usb_topology_lock;

/* USB core (usb.c). */

/* Read the descriptors of a device that has an address, select its first
 * configuration and bind the class drivers. Called by the controller
 * thread. Returns 0 or a negative errno; the device then remains without
 * drivers. */
int usb_enumerate(struct usb_device *dev);
/* The USB part of a device node of /dev/devices: the descriptors, the
 * interfaces, their endpoints and their drivers (usb.c). */
void usb_describe_device(struct devinfo *d, const char *path, const struct usb_device *dev);
/* The properties of a bound HID interface (hid.c). */
void hid_describe(struct devinfo *d, const struct hid_dev *h);
/* The interface number of a bound HID interface. */
unsigned hid_interface(const struct hid_dev *h);

/* Unbind the class drivers of a device whose port disconnected. Called by
 * the controller thread after the controller stopped the transfers of the
 * device. */
void usb_disconnect(struct usb_device *dev);
/* Count a device with an address, or one that lost it (delta -1). */
void usb_count_device(int delta);

/* Hub class driver (hub.c). */

/* Bind a hub interface: read the hub descriptor, configure the slot,
 * power the ports, enumerate the devices on them and start the status
 * change endpoint. */
int hub_probe(struct usb_device *dev, const uint8_t *cfg, unsigned cfg_len,
              const struct usb_interface_descriptor *intf);
/* Handle the port changes that the status change endpoint reported.
 * Called by the controller thread. */
void hub_service(struct usb_device *dev);
/* Remove the devices behind a disconnected hub and free the hub. */
void hub_disconnect(struct usb_device *dev);
/* The ports of a hub and the devices on them for /dev/devices. The
 * caller has locked usb_topology_lock. */
void hub_describe(struct devinfo *d, const char *path, const struct usb_device *dev);

/* Mass storage class driver (msc.c). */

/* Bind a bulk-only SCSI interface and register its logical units. */
int msc_probe(struct usb_device *dev, const uint8_t *cfg, unsigned cfg_len,
              const struct usb_interface_descriptor *intf);
/* Detach the logical units of a disconnected device. */
void msc_disconnect(struct usb_device *dev);

/* HID class driver (hid.c). */

/* Bind one HID interface. intf points into the configuration descriptor of
 * cfg_len bytes at cfg. Returns 0, or a negative errno when the interface
 * reports nothing that the input core knows. */
int hid_probe(struct usb_device *dev, const uint8_t *cfg, unsigned cfg_len,
              const struct usb_interface_descriptor *intf);
/* Stop reporting for a HID interface of a disconnected device. The input
 * device remains registered and is reused by the next interface with the
 * same name and capabilities. Called without xhci.lock. */
void hid_disconnect(struct hid_dev *h);
