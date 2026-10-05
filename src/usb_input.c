// USB HID input: TinyUSB host on the native USB port (the board's USB-C,
// through a powered hub). Mice move the port-1 mouse counters (JOY0DAT) and
// drive its buttons (left: CIA-A PRA bit 6, /FIR0; right/middle: POTINP
// DATLY/DATLX); keyboard usages are mapped to Amiga raw keycodes by key
// position and sent through hostAmigaKey(). Ctrl + Alt + Delete reboots the
// board (next ADF in the rotation); Ctrl + both Amiga keys only the Amiga.
//
// Everything runs on core 0: TinyUSB is polled from the emulation loop and
// its callbacks only collect input; usb_input_frame() applies it once per
// frame, between emulation slices. (Core 1 must not run flash code: a
// stalled XIP fetch can make it miss the HDMI line interrupt.)
#include "usb_input.h"

#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "hardware/watchdog.h"
#include "hardware/structs/usb.h"
#include "Host.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"

// ── Mouse ─────────────────────────────────────────────────────────────────
static int mouse_dx, mouse_dy;   // movement since the last frame
static uint8_t mouse_buttons;    // HID buttons: bit 0 left, 1 right, 2 middle

// ── Keyboard ──────────────────────────────────────────────────────────────
// USB HID usage (page 7) -> Amiga raw keycode, by key position (the Amiga
// keymap then picks the characters, as on a real keyboard); 0xFF: no key.
// Keys a PC keyboard lacks: Help on F11 and Insert, the Amiga keys on the
// GUI keys (Menu as right Amiga), keypad ( ) on Num Lock / Scroll Lock.
#define NO_KEY 0xFF
static const uint8_t hid_to_amiga[0x68] = {
    [0x04] = 0x20, [0x05] = 0x35, [0x06] = 0x33, [0x07] = 0x22, // a b c d
    [0x08] = 0x12, [0x09] = 0x23, [0x0A] = 0x24, [0x0B] = 0x25, // e f g h
    [0x0C] = 0x17, [0x0D] = 0x26, [0x0E] = 0x27, [0x0F] = 0x28, // i j k l
    [0x10] = 0x37, [0x11] = 0x36, [0x12] = 0x18, [0x13] = 0x19, // m n o p
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x21, [0x17] = 0x14, // q r s t
    [0x18] = 0x16, [0x19] = 0x34, [0x1A] = 0x11, [0x1B] = 0x32, // u v w x
    [0x1C] = 0x15, [0x1D] = 0x31,                               // y z
    [0x1E] = 0x01, [0x1F] = 0x02, [0x20] = 0x03, [0x21] = 0x04, // 1 2 3 4
    [0x22] = 0x05, [0x23] = 0x06, [0x24] = 0x07, [0x25] = 0x08, // 5 6 7 8
    [0x26] = 0x09, [0x27] = 0x0A,                               // 9 0
    [0x28] = 0x44, [0x29] = 0x45, [0x2A] = 0x41, [0x2B] = 0x42, // Return Esc BS Tab
    [0x2C] = 0x40, [0x2D] = 0x0B, [0x2E] = 0x0C, [0x2F] = 0x1A, // Space - = [
    [0x30] = 0x1B, [0x31] = 0x0D, [0x32] = 0x2B, [0x33] = 0x29, // ] \ #(intl) ;
    [0x34] = 0x2A, [0x35] = 0x00, [0x36] = 0x38, [0x37] = 0x39, // ' ` , .
    [0x38] = 0x3A, [0x39] = 0x62,                               // / Caps Lock
    [0x3A] = 0x50, [0x3B] = 0x51, [0x3C] = 0x52, [0x3D] = 0x53, // F1-F4
    [0x3E] = 0x54, [0x3F] = 0x55, [0x40] = 0x56, [0x41] = 0x57, // F5-F8
    [0x42] = 0x58, [0x43] = 0x59, [0x44] = 0x5F,                // F9 F10 F11=Help
    [0x47] = 0x5B, [0x49] = 0x5F,                               // ScrLk=KP) Ins=Help
    [0x4C] = 0x46,                                              // Delete
    [0x4F] = 0x4E, [0x50] = 0x4F, [0x51] = 0x4D, [0x52] = 0x4C, // right left down up
    [0x53] = 0x5A, [0x54] = 0x5C, [0x55] = 0x5D, [0x56] = 0x4A, // NumLk=KP( / * -
    [0x57] = 0x5E, [0x58] = 0x43,                               // KP+ KP Enter
    [0x59] = 0x1D, [0x5A] = 0x1E, [0x5B] = 0x1F, [0x5C] = 0x2D, // KP1-4
    [0x5D] = 0x2E, [0x5E] = 0x2F, [0x5F] = 0x3D, [0x60] = 0x3E, // KP5-8
    [0x61] = 0x3F, [0x62] = 0x0F, [0x63] = 0x3C,                // KP9 KP0 KP.
    [0x64] = 0x30, [0x65] = 0x67,                               // <>(intl) Menu=RAmiga
};
// Modifiers (usages 0xE0-0xE7): LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI.
static const uint8_t modifier_to_amiga[8] = {
    0x63, 0x60, 0x64, 0x66, 0x63, 0x61, 0x65, 0x67,
};

static uint8_t amiga_key(uint8_t usage) {
    if (usage >= sizeof(hid_to_amiga))
        return NO_KEY;
    const uint8_t code = hid_to_amiga[usage];
    // Unlisted entries are 0, which is also the ` key (usage 0x35).
    return code || usage == 0x35 ? code : NO_KEY;
}

// Amiga key events wait here and go to the Amiga one per frame, so the
// keyboard handshake (CIA-A serial register + interrupt) never overwrites a
// code the Amiga has not read. Bit 7 marks a release.
#define KEY_QUEUE 64
static uint8_t key_queue[KEY_QUEUE];
static unsigned key_head, key_tail;
static uint8_t prev_modifiers;
static uint8_t prev_keys[6];
static int caps_locked;

static void queue_key(uint8_t code, int release) {
    if (code == NO_KEY)
        return;
    const unsigned next = (key_head + 1) % KEY_QUEUE;
    if (next == key_tail)
        return;  // full: drop rather than block
    key_queue[key_head] = (uint8_t)(code | (release ? 0x80u : 0));
    key_head = next;
}

static int key_in(uint8_t key, const uint8_t *keys) {
    for (int i = 0; i < 6; ++i)
        if (keys[i] == key)
            return 1;
    return 0;
}

static void key_change(uint8_t usage, int release) {
    if (usage == 0x39) {
        // Caps Lock latches on the Amiga: "down" while locked, "up" when
        // unlocked. USB sends press and release on every stroke.
        if (!release) {
            caps_locked = !caps_locked;
            queue_key(0x62, !caps_locked);
        }
        return;
    }
    queue_key(amiga_key(usage), release);
}

// Ctrl + Alt + Delete reboots the board (not just the Amiga): a watchdog
// reset, as tooling/disk.sh does, so the next ADF in the flash rotation is
// mounted. The Amiga has no meaning for this combination.
static void board_reset_check(const hid_keyboard_report_t *r) {
    const int ctrl = (r->modifier & (KEYBOARD_MODIFIER_LEFTCTRL |
                                     KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;
    const int alt = (r->modifier & (KEYBOARD_MODIFIER_LEFTALT |
                                    KEYBOARD_MODIFIER_RIGHTALT)) != 0;
    if (ctrl && alt && key_in(0x4C, r->keycode)) {  // 0x4C: Delete
        printf("Ctrl+Alt+Del: board reset\n");
        watchdog_reboot(0, 0, 10);  // 10 ms: let the UART drain
        for (;;)
            tight_loop_contents();
    }
}

static void keyboard_report(const hid_keyboard_report_t *r) {
    board_reset_check(r);
    const uint8_t changed = r->modifier ^ prev_modifiers;
    for (int bit = 0; bit < 8; ++bit)
        if (changed & (1u << bit))
            queue_key(modifier_to_amiga[bit], !(r->modifier & (1u << bit)));
    prev_modifiers = r->modifier;
    for (int i = 0; i < 6; ++i)
        if (prev_keys[i] > 3 && !key_in(prev_keys[i], r->keycode))
            key_change(prev_keys[i], 1);
    for (int i = 0; i < 6; ++i)
        if (r->keycode[i] > 3 && !key_in(r->keycode[i], prev_keys))
            key_change(r->keycode[i], 0);
    memcpy(prev_keys, r->keycode, sizeof(prev_keys));
}

static void mouse_report(const uint8_t *report, uint16_t len) {
    if (len < 3)
        return;
    mouse_buttons = report[0];
    mouse_dx += (int8_t)report[1];
    mouse_dy += (int8_t)report[2];
}

// ── TinyUSB callbacks (from tuh_task(), core 0) ───────────────────────────
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;
    const uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    printf("USB: device %u (%04x:%04x) HID %u: %s\n", dev_addr, vid, pid,
           instance,
           protocol == HID_ITF_PROTOCOL_KEYBOARD ? "keyboard" :
           protocol == HID_ITF_PROTOCOL_MOUSE ? "mouse" : "other (ignored)");
    if (protocol == HID_ITF_PROTOCOL_KEYBOARD ||
        protocol == HID_ITF_PROTOCOL_MOUSE)
        tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("USB: device %u HID %u removed\n", dev_addr, instance);
    if (tuh_hid_interface_protocol(dev_addr, instance) ==
        HID_ITF_PROTOCOL_MOUSE)
        mouse_buttons = 0;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    switch (tuh_hid_interface_protocol(dev_addr, instance)) {
    case HID_ITF_PROTOCOL_KEYBOARD:
        if (len >= sizeof(hid_keyboard_report_t))
            keyboard_report((const hid_keyboard_report_t *)report);
        break;
    case HID_ITF_PROTOCOL_MOUSE:
        mouse_report(report, len);
        break;
    default:
        break;
    }
    tuh_hid_receive_report(dev_addr, instance);
}

// ── Emulator side ─────────────────────────────────────────────────────────
// TinyUSB 0.18's RP2040/RP2350 host driver panics on a data sequence error
// (a packet with the wrong DATA0/1 toggle), which stopped the emulator now
// and then with the wireless receivers on the hub. Masking the interrupt
// kept it running, but the endpoint that saw the error never completed its
// transfer again (the mouse froze, the keyboard kept working), so
// usb_input_frame() restarts the host instead: the hub and the receivers
// enumerate again within about a second.
static void usb_host_start(void) {
    tuh_init(BOARD_TUH_RHPORT);
    hw_clear_bits(&usb_hw->inte, USB_INTE_ERROR_DATA_SEQ_BITS);
}

static void usb_host_restart(void) {
    // Release whatever was held: the devices report afresh.
    static const hid_keyboard_report_t none;
    keyboard_report(&none);
    mouse_buttons = 0;
    mouse_dx = mouse_dy = 0;
    tuh_deinit(BOARD_TUH_RHPORT);
    usb_host_start();
    printf("USB: data sequence error, host restarted\n");
}

void usb_input_init(void) {
    // Released buttons read high: POTINP DATLY (bit 10, right) and DATLX
    // (bit 8, middle) of both ports.
    chipset.potinp |= 0x5500;
    // Boot protocol: fixed report layouts for mice and keyboards.
    tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
    usb_host_start();
    printf("USB: host on the native port (hub, HID mouse/keyboard)\n");
}

// Data sequence errors so far (readable over SWD); setting
// usb_restart_request over SWD restarts the host the same way (tests).
volatile uint32_t usb_seq_errors;
volatile uint32_t usb_restart_request;

void usb_input_task(void) {
    if (tuh_task_event_ready())
        tuh_task();
}

static int clamp_delta(int d) {
    return d > 127 ? 127 : d < -127 ? -127 : d;
}

void usb_input_frame(void) {
    if ((usb_hw->sie_status & USB_SIE_STATUS_DATA_SEQ_ERROR_BITS) ||
        usb_restart_request) {
        usb_hw->sie_status = USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;  // write-1-to-clear
        ++usb_seq_errors;
        usb_restart_request = 0;
        usb_host_restart();
    }
    const int dx = clamp_delta(mouse_dx);
    const int dy = clamp_delta(mouse_dy);
    mouse_dx -= dx;
    mouse_dy -= dy;
    // JOY0DAT: vertical counter in the high byte, horizontal in the low.
    const uint8_t h = (uint8_t)((chipset.joy0dat & 0xFF) + dx);
    const uint8_t v = (uint8_t)((chipset.joy0dat >> 8) + dy);
    chipset.joy0dat = (uint16_t)(v << 8 | h);
    if (mouse_buttons & 1)
        CIAA.pra &= (uint8_t)~0x40;
    else
        CIAA.pra |= 0x40;
    if (mouse_buttons & 2)
        chipset.potinp &= (uint16_t)~0x0400;
    else
        chipset.potinp |= 0x0400;
    if (mouse_buttons & 4)
        chipset.potinp &= (uint16_t)~0x0100;
    else
        chipset.potinp |= 0x0100;

    if (key_tail != key_head) {
        const uint8_t e = key_queue[key_tail];
        key_tail = (key_tail + 1) % KEY_QUEUE;
        hostAmigaKey(e & 0x7F, e & 0x80);
    }
}
