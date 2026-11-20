/*
 * Keyboard translation shared by the input drivers: Linux key codes to the
 * bytes a serial terminal sends (US layout; arrows and editing keys as ANSI
 * escape sequences, Ctrl-C as 0x03). Function keys go to the desktop.
 */
#include "console.h"
#include "keyboard.h"
#include "string.h"
#include "ui.h"

enum {
    KEY_ESC = 1, KEY_BACKSPACE = 14, KEY_TAB = 15, KEY_ENTER = 28, KEY_LEFTCTRL = 29,
    KEY_LEFTSHIFT = 42, KEY_RIGHTSHIFT = 54, KEY_KPASTERISK = 55, KEY_LEFTALT = 56,
    KEY_CAPSLOCK = 58, KEY_F1 = 59, KEY_F10 = 68, KEY_NUMLOCK = 69, KEY_KP7 = 71,
    KEY_KPDOT = 83, KEY_F11 = 87, KEY_F12 = 88, KEY_KPENTER = 96, KEY_RIGHTCTRL = 97,
    KEY_KPSLASH = 98, KEY_RIGHTALT = 100, KEY_HOME = 102, KEY_UP = 103, KEY_PAGEUP = 104,
    KEY_LEFT = 105, KEY_RIGHT = 106, KEY_END = 107, KEY_DOWN = 108, KEY_PAGEDOWN = 109,
    KEY_INSERT = 110, KEY_DELETE = 111, KEY_KPEQUAL = 117, KEY_LEFTMETA = 125,
    KEY_RIGHTMETA = 126,
};

/* US layout for keys 0..58: unshifted and shifted. */
static const char plain[59] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x7f, '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
};
static const char shifted[59] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0x7f, '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
};
/* Keypad 7 8 9 - 4 5 6 + 1 2 3 0 . (codes 71..83), as digits. */
static const char keypad[14] = "789-456+1230.";

/* HID usage page 7 (USB HID Usage Tables 1.4, section 10) to Linux codes,
 * as in Linux drivers/hid/hid-input.c. */
static const u8 hid_keys[0x68] = {
    0, 0, 0, 0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
    50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11, 28, 1, 14, 15, 57, 12, 13, 26,
    27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106,
    105, 108, 103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
    72, 73, 82, 83, 86, 127, 116, 117,
};
static const u8 hid_mods[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

u16 kbd_hid_to_code(u8 usage)
{
    if (usage < sizeof(hid_keys))
        return hid_keys[usage];
    if (usage >= 0xe0 && usage <= 0xe7)
        return hid_mods[usage - 0xe0];
    return 0;
}

bool kbd_is_modifier(u16 code)
{
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
    case KEY_LEFTALT: case KEY_RIGHTALT: case KEY_LEFTMETA: case KEY_RIGHTMETA:
    case KEY_CAPSLOCK: case KEY_NUMLOCK:
        return true;
    }
    return false;
}

void kbd_key(struct kbd_state *in, u16 code, int value)
{
    bool down = value != 0;
    if ((code >= KEY_F1 && code <= KEY_F10) || code == KEY_F11 || code == KEY_F12) {
        if (value == 1)
            ui_function_key(code <= KEY_F10 ? code - KEY_F1 + 1 : code - KEY_F11 + 11);
        return;
    }
    switch (code) {
    case KEY_LEFTSHIFT: in->shift_l = down; return;
    case KEY_RIGHTSHIFT: in->shift_r = down; return;
    case KEY_LEFTCTRL: in->ctrl_l = down; return;
    case KEY_RIGHTCTRL: in->ctrl_r = down; return;
    case KEY_CAPSLOCK:
        if (value == 1)
            in->caps = !in->caps;
        return;
    }
    if (!down)
        return;
    const char *seq = NULL;
    switch (code) {
    case KEY_UP: seq = "\x1b[A"; break;
    case KEY_DOWN: seq = "\x1b[B"; break;
    case KEY_RIGHT: seq = "\x1b[C"; break;
    case KEY_LEFT: seq = "\x1b[D"; break;
    case KEY_HOME: seq = "\x1b[H"; break;
    case KEY_END: seq = "\x1b[F"; break;
    case KEY_DELETE: seq = "\x1b[3~"; break;
    case KEY_PAGEUP: seq = "\x1b[5~"; break;
    case KEY_PAGEDOWN: seq = "\x1b[6~"; break;
    case KEY_INSERT: seq = "\x1b[2~"; break;
    case KEY_KPENTER: seq = "\r"; break;
    case KEY_KPSLASH: seq = "/"; break;
    case KEY_KPEQUAL: seq = "="; break;
    }
    if (code >= KEY_KP7 && code <= KEY_KPDOT) {
        console_input(&keypad[code - KEY_KP7], 1);
        return;
    }
    if (seq) {
        console_input(seq, strlen(seq));
        return;
    }
    if (code >= sizeof(plain))
        return;
    bool shift = in->shift_l || in->shift_r;
    char c = shift ? shifted[code] : plain[code];
    if (!c)
        return;
    if (in->caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (in->ctrl_l || in->ctrl_r) {
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
            c = (char)((c | 0x20) - 'a' + 1); /* Ctrl-A = 1 ... Ctrl-C = 3 */
        else if (c == '[')
            c = 27;
        else
            return;
    }
    console_input(&c, 1);
}
