#pragma once
/* Declarations shared by the xHCI driver (xhci.c), the USB core (usb.c)
 * and the HID class driver (hid.c). docs/design/usb.md describes the
 * design. */
#include <kernel.h>

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

#define USB_MAX_HID 4                   /* HID interfaces bound per device */

/* A device with an address. It is created by the controller thread when a
 * port connects and destroyed when the port disconnects. The fields,
 * hid[] included, are read and written by that thread only. The report
 * callbacks receive their struct hid_dev as their argument. */
struct usb_device {
    struct xhci *hc;
    unsigned slot;                      /* the xHCI slot ID */
    unsigned port;                      /* the root hub port, from 1 */
    enum usb_speed speed;
    struct usb_device_descriptor desc;
    char product[64];                   /* the product string, or a name made from the IDs */
    char manufacturer[64];              /* the manufacturer string, or "" */
    char serial[64];                    /* the serial number string, or "" */
    uint8_t *config;                    /* the first configuration descriptor, kmalloc'd, or NULL */
    unsigned config_len;
    struct hid_dev *hid[USB_MAX_HID];
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

/* USB core (usb.c). */

/* Read the descriptors of a device that has an address, select its first
 * configuration and bind the class drivers. Called by the controller
 * thread. Returns 0 or a negative errno; the device then remains without
 * drivers. */
int usb_enumerate(struct usb_device *dev);
/* The USB part of a device node of /dev/devices: the descriptors, the
 * interfaces, their endpoints and their drivers (usb.c). */
struct devinfo;
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
