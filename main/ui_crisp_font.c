#include "ui_crisp_font.h"

#include <stdint.h>
#include <stdbool.h>

#include "calendar_font_data.h"

static bool crisp_font_empty16(const uint8_t *glyph)
{
    if (!glyph)
        return true;
    for (int i = 0; i < 32; i++) {
        if (glyph[i])
            return false;
    }
    return true;
}

static const uint8_t *crisp_font_find_zh(uint32_t cp)
{
    int lo = 0;
    int hi = CAL_FONT_ZH_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cal_font_zh[mid].cp == cp)
            return cal_font_zh[mid].bmp;
        if (cal_font_zh[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static bool crisp_utf8_cont(uint8_t c)
{
    return (c & 0xC0) == 0x80;
}

static int crisp_decode_utf8(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    int cp;
    if (p[0] < 0x80) {
        cp = p[0];
        *pp += 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        if (!p[1] || !crisp_utf8_cont(p[1])) {
            *pp += 1;
            return '?';
        }
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *pp += 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        if (!p[1] || !crisp_utf8_cont(p[1]) ||
            !p[2] || !crisp_utf8_cont(p[2])) {
            *pp += 1;
            return '?';
        }
        cp = ((p[0] & 0x0F) << 12) |
             ((p[1] & 0x3F) << 6) |
             (p[2] & 0x3F);
        *pp += 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        if (!p[1] || !crisp_utf8_cont(p[1]) ||
            !p[2] || !crisp_utf8_cont(p[2]) ||
            !p[3] || !crisp_utf8_cont(p[3])) {
            *pp += 1;
            return '?';
        }
        cp = ((p[0] & 0x07) << 18) |
             ((p[1] & 0x3F) << 12) |
             ((p[2] & 0x3F) << 6) |
             (p[3] & 0x3F);
        *pp += 4;
    } else {
        *pp += 1;
        return '?';
    }
    return cp;
}

static int crisp_norm_ascii(int cp)
{
    if (cp == 0x3000)
        return ' ';
    if (cp >= 0xFF01 && cp <= 0xFF5E)
        return cp - 0xFEE0;
    return cp;
}

static void crisp_encode_utf8(int cp, char out[5])
{
    int len = 0;
    if (cp <= 0x7F) {
        out[len++] = (char)cp;
    } else if (cp <= 0x7FF) {
        out[len++] = (char)(0xC0 | (cp >> 6));
        out[len++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out[len++] = (char)(0xE0 | (cp >> 12));
        out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[len++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[len++] = (char)(0xF0 | (cp >> 18));
        out[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[len++] = (char)(0x80 | (cp & 0x3F));
    }
    out[len] = '\0';
}

static void crisp_font_bitmap_sc(fb_t *fb, int x, int y, int w, int h,
                                 const uint8_t *data, fb_color_t color,
                                 int scale)
{
    if (scale < 1)
        scale = 1;
    int stride = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            if (!(data[row * stride + col / 8] & (0x80 >> (col % 8))))
                continue;
            int px = x + col * scale;
            int py = y + row * scale;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    fb_pixel(fb, px + dx, py + dy, color);
        }
    }
}

int ui_crisp_font_width(const char *text, int scale)
{
    if (!text)
        return 0;
    if (scale < 1)
        scale = 1;
    int width = 0;
    while (*text) {
        int cp = crisp_norm_ascii(crisp_decode_utf8(&text));
        width += (cp < 0x80 ? 8 : 16) * scale;
    }
    return width;
}

bool ui_crisp_font_can_draw_all(const char *text)
{
    if (!text)
        return true;
    while (*text) {
        int cp = crisp_norm_ascii(crisp_decode_utf8(&text));
        if (cp < 0x80)
            continue;
        if (crisp_font_empty16(crisp_font_find_zh((uint32_t)cp)))
            return false;
    }
    return true;
}

int ui_crisp_font_draw(fb_t *fb, int x, int y, const char *text,
                       fb_color_t color, int scale)
{
    if (!text)
        return 0;
    if (scale < 1)
        scale = 1;
    int cx = x;
    while (*text) {
        int cp = crisp_norm_ascii(crisp_decode_utf8(&text));
        if (cp < 0x80) {
            int idx = cp - 32;
            if (idx < 0 || idx >= 95)
                idx = 0;
            crisp_font_bitmap_sc(fb, cx, y, 8, 16, cal_font_ascii[idx],
                                 color, scale);
            cx += 8 * scale;
        } else {
            char raw[5] = {0};
            const uint8_t *glyph = crisp_font_find_zh((uint32_t)cp);
            if (!crisp_font_empty16(glyph))
                crisp_font_bitmap_sc(fb, cx, y, 16, 16, glyph, color, scale);
            else {
                crisp_encode_utf8(cp, raw);
                fb_utf8_scaled_builtin(fb, cx, y, raw, color, scale);
            }
            cx += 16 * scale;
        }
    }
    return cx - x;
}

int ui_crisp_font_draw_maxw(fb_t *fb, int x, int y, const char *text,
                            fb_color_t color, int scale, int max_w)
{
    if (!text || max_w <= 0)
        return 0;
    if (scale < 1)
        scale = 1;
    int cx = x;
    int lim = x + max_w;
    while (*text && cx < lim) {
        int cp = crisp_norm_ascii(crisp_decode_utf8(&text));
        int adv = (cp < 0x80 ? 8 : 16) * scale;
        if (cx + adv > lim)
            break;
        if (cp < 0x80) {
            int idx = cp - 32;
            if (idx < 0 || idx >= 95)
                idx = 0;
            crisp_font_bitmap_sc(fb, cx, y, 8, 16, cal_font_ascii[idx],
                                 color, scale);
        } else {
            char raw[5] = {0};
            const uint8_t *glyph = crisp_font_find_zh((uint32_t)cp);
            if (!crisp_font_empty16(glyph))
                crisp_font_bitmap_sc(fb, cx, y, 16, 16, glyph, color, scale);
            else {
                crisp_encode_utf8(cp, raw);
                fb_utf8_scaled_builtin(fb, cx, y, raw, color, scale);
            }
        }
        cx += adv;
    }
    return cx - x;
}
