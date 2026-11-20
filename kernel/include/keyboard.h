/* Keyboard translation shared by the input drivers (virtio input, USB HID).
 * Keys are Linux input codes (include/uapi/linux/input-event-codes.h); a US
 * layout turns them into the bytes a serial terminal would send, so the
 * shell treats every console alike. Function keys go to the desktop. */
#pragma once
#include "types.h"

struct kbd_state {
    bool shift_l, shift_r, ctrl_l, ctrl_r, caps;
};

/* value: 0 released, 1 pressed, 2 auto-repeat */
void kbd_key(struct kbd_state *k, u16 code, int value);
/* True for modifier keys (they never auto-repeat). */
bool kbd_is_modifier(u16 code);
/* HID keyboard usage (page 7) to Linux key code, 0 if none. */
u16 kbd_hid_to_code(u8 usage);
