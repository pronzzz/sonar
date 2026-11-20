/* DeepSeek integration: model discovery, setup, and the system assistant. */
#pragma once

#include "types.h"

void ai_boot(void);                    /* load config, run first-time setup, load model */
void ai_ask_command(const char *text); /* "? question" from the shell */
bool ai_ready(void);
const char *ai_model_name(void);       /* loaded model, or NULL */
bool ai_busy(void);                   /* DeepSeek is computing right now */
u32 ai_last_speed_x10(void);           /* last answer's tokens/s x 10 (0: none yet) */
