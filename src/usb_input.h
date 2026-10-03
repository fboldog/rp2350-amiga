// USB HID input (RP2350 builds with OMEGA_ENABLE_USB_HOST): TinyUSB host on
// the native USB port, through a hub; mice drive JOY0DAT and the buttons,
// keyboards go through pressKey()/releaseKey(). Everything runs on core 0,
// between emulation slices.
#ifndef USB_INPUT_H
#define USB_INPUT_H

void usb_input_init(void);
// Runs TinyUSB's host task when its interrupt has queued work.
void usb_input_task(void);
// Called every pass of the emulation loop (~10 k/s): only every 32nd pass
// looks for USB work (a few ms apart, ample for enumeration and 8-10 ms HID
// reports), so idle polling costs next to nothing.
static inline void usb_input_poll(void) {
    static unsigned passes;
    if ((++passes & 31u) == 0)
        usb_input_task();
}
// Apply the input gathered since the last frame (once per VBL).
void usb_input_frame(void);

#endif
