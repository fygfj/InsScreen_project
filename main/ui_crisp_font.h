#pragma once

#include <stdbool.h>

#include "fb_render.h"

int ui_crisp_font_width(const char *text, int scale);
bool ui_crisp_font_can_draw_all(const char *text);
int ui_crisp_font_draw(fb_t *fb, int x, int y, const char *text,
                       fb_color_t color, int scale);
int ui_crisp_font_draw_maxw(fb_t *fb, int x, int y, const char *text,
                            fb_color_t color, int scale, int max_w);
