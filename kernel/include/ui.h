/* The Sonar desktop (kernel/ui/desktop.c): a windowed UI on the display. */
#pragma once
#include "types.h"

/* Start the desktop if there is a display and ui.desktop is not "off". */
void ui_boot(void);
bool ui_desktop_active(void);

/* Input, called from interrupt handlers. */
void ui_pointer_present(bool absolute);
void ui_pointer_move_abs(int fx, int fy); /* 0..65535 across the screen */
void ui_pointer_move_rel(int dx, int dy);
void ui_pointer_button(int button, bool down); /* 0 left, 1 right, 2 middle */
void ui_pointer_wheel(int delta);             /* positive: away from the user */
void ui_function_key(int n);                  /* F1..F12 pressed */
#define UI_FKEY_SUPER 20                       /* the Super (logo) key */
/* While a desktop text field has focus, keys go to it instead of the shell. */
bool ui_keyboard_grabbed(void);
enum { UI_KEY_UP = 0x101, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_HOME, UI_KEY_END, UI_KEY_DELETE };
void ui_key(int key); /* a character (Enter '\r', Backspace 0x7f, Esc 27) or UI_KEY_* */
