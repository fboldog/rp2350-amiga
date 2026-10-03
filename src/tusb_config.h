// TinyUSB configuration: host mode on the RP2350's native USB port (the
// board's USB-C), with a hub, for HID mice and keyboards (src/usb_input.c).
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif
#define CFG_TUSB_DEBUG 0

#define CFG_TUH_ENABLED 1
#define BOARD_TUH_RHPORT 0
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB 1
#define CFG_TUH_DEVICE_MAX (3 * CFG_TUH_HUB + 1)
#define CFG_TUH_HID (3 * CFG_TUH_DEVICE_MAX)  // a receiver often has 2-3
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64
#define CFG_TUH_CDC 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0

#endif
