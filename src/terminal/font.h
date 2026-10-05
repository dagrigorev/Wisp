#pragma once
#ifndef WISP_FONT_H
#define WISP_FONT_H

/* font.h — DirectWrite font enumeration and glyph metrics */

#include <windows.h>
#include <dwrite.h>
#include "wisp_bool.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    IDWriteFactory    *factory;
    IDWriteTextFormat *fmt_normal;
    IDWriteTextFormat *fmt_bold;
    IDWriteTextFormat *fmt_italic;
    IDWriteTextFormat *fmt_bold_italic;
    float              cell_width;
    float              cell_height;
    float              baseline;
    float              pt_size;
    wchar_t            family[128];
} FontState;

/* Initialize DirectWrite factory and load font family at pt_size */
bool font_init(FontState *fs, const wchar_t *family, float pt_size, float dpi);

/* Recompute cell metrics for a new DPI / pt_size */
bool font_resize(FontState *fs, float pt_size, float dpi);

/* Release all DirectWrite resources */
void font_free(FontState *fs);


#ifdef __cplusplus
}
#endif

#endif /* WISP_FONT_H */
