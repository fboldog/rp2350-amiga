// USB HID input: TinyUSB host on the native USB port (the board's USB-C,
// through a powered hub). Mice move the port-1 mouse counters (JOY0DAT) and
// drive its buttons (left: CIA-A PRA bit 6, /FIR0; right/middle: POTINP
// DATLY/DATLX); keyboards are sent to the Amiga keyboard through
// pressKey()/releaseKey(), whose table is indexed by USB HID usage codes.
//
// Everything runs on core 0: TinyUSB is polled from the emulation loop and
// its callbacks only collect input; usb_input_frame() applies it once per
// frame, between emulation slices. (Core 1 must not run flash code: a
// stalled XIP fetch can make it miss the HDMI line interrupt.)
#include "usb_input.h"

#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "Host.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"

// ── Mouse ─────────────────────────────────────────────────────────────────
static int mouse_dx, mouse_dy;   // movement since the last frame
static uint8_t mouse_buttons;    // HID buttons: bit 0 left, 1 right, 2 middle

// ── Keyboard ──────────────────────────────────────────────────────────────
// Key events wait here and go to the Amiga one per frame, so the keyboard
// handshake (CIA-A serial register + interrupt) never overwrites a code the
// Amiga has not read. Bit 15 marks a release.
#define KEY_QUEUE 64
static uint16_t key_queue[KEY_QUEUE];
static unsigned key_head, key_tail;
static uint8_t prev_modifiers;
static uint8_t prev_keys[6];

static void queue_key(uint8_t usage, int release) {
    const unsigned next = (key_head + 1) % KEY_QUEUE;
    if (next == key_tail)
        return;  // full: drop rather than block
    key_queue[key_head] = usage | (release ? 0x8000u : 0);
    key_head = next;
}

static int key_in(uint8_t key, const uint8_t *keys) {
    for (int i = 0; i < 6; ++i)
        if (keys[i] == key)
            return 1;
    return 0;
}

static void keyboard_report(const hid_keyboard_report_t *r) {
    // Modifiers are usages 224..231 (left Ctrl .. right GUI).
    const uint8_t changed = r->modifier ^ prev_modifiers;
    for (int bit = 0; bit < 8; ++bit)
        if (changed & (1u << bit))
            queue_key((uint8_t)(224 + bit), !(r->modifier & (1u << bit)));
    prev_modifiers = r->modifier;
    for (int i = 0; i < 6; ++i)
        if (prev_keys[i] > 1 && !key_in(prev_keys[i], r->keycode))
            queue_key(prev_keys[i], 1);
    for (int i = 0; i < 6; ++i)
        if (r->keycode[i] > 1 && !key_in(r->keycode[i], prev_keys))
            queue_key(r->keycode[i], 0);
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
void usb_input_init(void) {
    // Released buttons read high: POTINP DATLY (bit 10, right) and DATLX
    // (bit 8, middle) of both ports.
    chipset.potinp |= 0x5500;
    // Boot protocol: fixed report layouts for mice and keyboards.
    tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
    tuh_init(BOARD_TUH_RHPORT);
    printf("USB: host on the native port (hub, HID mouse/keyboard)\n");
}

void usb_input_task(void) {
    if (tuh_task_event_ready())
        tuh_task();
}

static int clamp_delta(int d) {
    return d > 127 ? 127 : d < -127 ? -127 : d;
}

void usb_input_frame(void) {
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
        const uint16_t e = key_queue[key_tail];
        key_tail = (key_tail + 1) % KEY_QUEUE;
        if (e & 0x8000u)
            releaseKey(e & 0xFF);
        else
            pressKey(e & 0xFF);
    }
}
