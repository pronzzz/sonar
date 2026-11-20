/* Desktop icons (kernel/ui/icons.c): drawn from shapes with antialiased
 * edges at any size, and cached per size. */
#pragma once
#include "gfx.h"

enum {
    IC_TERM, IC_FILES, IC_MONITOR, IC_ABOUT, IC_DEEPSEEK, IC_CONSOLE, IC_POWER, IC_RESTART,
    IC_CMD, IC_FILE, IC_FOLDER, IC_CALC, IC_HOME, IC_SETUP, IC_SONAR,
    IC_COUNT
};

/* Draw icon `kind` as the largest square that fits in r, centered. */
void icon_draw(struct surface *s, int kind, struct rect r);
