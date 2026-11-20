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
