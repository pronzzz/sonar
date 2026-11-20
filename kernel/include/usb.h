/* USB: devices found by the host controller driver (kernel/drivers/xhci.c)
 * and the interface a class driver (kernel/drivers/usb_hid.c) uses. */
#pragma once
#include "types.h"

struct usb_dev;

/* Standard request types (USB 2.0, section 9.3) */
#define USB_DIR_IN 0x80
#define USB_TYPE_CLASS 0x20
#define USB_RECIP_INTERFACE 0x01
#define USB_REQ_GET_DESCRIPTOR 6
#define USB_REQ_SET_CONFIGURATION 9

/* A control transfer on endpoint 0; data (up to 4096 bytes) is copied. */
bool usb_control(struct usb_dev *d, u8 type, u8 req, u16 value, u16 index, void *data,
                 u16 len);

/* Poll an interrupt IN endpoint: fn runs with each report the device sends
 * (from interrupt or idle context). */
typedef void (*usb_report_fn)(void *ctx, const u8 *data, u32 len);
bool usb_interrupt_in(struct usb_dev *d, u8 ep_addr, u16 mps, u8 interval, usb_report_fn fn,
                      void *ctx);

const char *usb_name(struct usb_dev *d); /* "usb1.2" (controller.port) */

/* Class drivers: offered every configured device with its configuration
 * descriptor; true if a driver took one of its interfaces. */
bool usb_hid_probe(struct usb_dev *d, const u8 *cfg, u32 len);
