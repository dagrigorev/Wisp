/* font.cpp — DirectWrite font management, compiled as C++ */
#include <windows.h>
#include <dwrite.h>
#include <math.h>
#include <string.h>
#include "font.h"
#include "../core/str_util.h"
#include "../core/log.h"

/* Families to try when the configured one is not installed, in order. The
 * first two carry the Nerd Font private-use glyphs that prompts like
 * oh-my-posh draw their separators with; Consolas is the last resort that
 * ships with Windows. */
static const wchar_t *const fallback_families[] = {
    L"GeistMono NF",
    L"Cascadia Code",
    L"Consolas",
};

/* True if the family is actually installed.
 *
 * This matters because CreateTextFormat does NOT fail for a family that does
 * not exist: it returns S_OK and silently substitutes a default font. Asking
 * the system collection is the only way to know what we really got. */
static bool family_installed(IDWriteFactory *fac, const wchar_t *family) {
    if (!fac || !family || !family[0]) return false;
    IDWriteFontCollection *coll = NULL;
    if (FAILED(fac->GetSystemFontCollection(&coll, FALSE)) || !coll) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    const HRESULT hr = coll->FindFamilyName(family, &index, &exists);
    coll->Release();
    return SUCCEEDED(hr) && exists;
}

/* Resolve the family to use, writing it into `out`. Falls back in order and
 * logs when the configured family is missing, so a typo in the config is
 * visible rather than silently becoming some other font. */
static void resolve_family(IDWriteFactory *fac, const wchar_t *wanted, wchar_t *out, size_t out_len) {
    if (family_installed(fac, wanted)) {
        wcsncpy(out, wanted, out_len - 1);
        out[out_len - 1] = L'\0';
        return;
    }
    WISP_LOG_WARN("font family not installed, falling back");
    for (size_t i = 0; i < sizeof(fallback_families) / sizeof(fallback_families[0]); i++) {
        if (family_installed(fac, fallback_families[i])) {
            wcsncpy(out, fallback_families[i], out_len - 1);
            out[out_len - 1] = L'\0';
            return;
        }
    }
    wcsncpy(out, L"Consolas", out_len - 1);
    out[out_len - 1] = L'\0';
}

static IDWriteTextFormat *make_fmt(IDWriteFactory *fac, const wchar_t *family,
                                   float px, DWRITE_FONT_WEIGHT w, DWRITE_FONT_STYLE s) {
    IDWriteTextFormat *fmt=NULL;
    HRESULT hr=fac->CreateTextFormat(family,NULL,w,s,DWRITE_FONT_STRETCH_NORMAL,px,L"en-US",&fmt);
    if (FAILED(hr)) fac->CreateTextFormat(L"Consolas",NULL,w,s,DWRITE_FONT_STRETCH_NORMAL,px,L"en-US",&fmt);
    if (fmt) {
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    return fmt;
}

/* The regular font face of `family`, for design metrics. Caller releases. */
static IDWriteFontFace *make_face(IDWriteFactory *fac, const wchar_t *family) {
    if (!fac) return NULL;
    IDWriteFontCollection *coll = NULL;
    if (FAILED(fac->GetSystemFontCollection(&coll, FALSE)) || !coll) return NULL;

    UINT32 index = 0;
    BOOL exists = FALSE;
    IDWriteFontFace *face = NULL;
    if (SUCCEEDED(coll->FindFamilyName(family, &index, &exists)) && exists) {
        IDWriteFontFamily *fam = NULL;
        if (SUCCEEDED(coll->GetFontFamily(index, &fam)) && fam) {
            IDWriteFont *font = NULL;
            if (SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                                                    DWRITE_FONT_STRETCH_NORMAL,
                                                    DWRITE_FONT_STYLE_NORMAL, &font)) &&
                font) {
                font->CreateFontFace(&face);
                font->Release();
            }
            fam->Release();
        }
    }
    coll->Release();
    return face;
}

/* Cell metrics from the font's design units, rounded to whole pixels.
 *
 * Whole pixels are the point. The cell size is multiplied by the column and
 * row to place every glyph, so a fractional width puts each column on a
 * different subpixel phase: DirectWrite then antialiases the same letter
 * differently depending on where it lands, the gaps between characters vary
 * by a fraction of a pixel, cell background fills get soft edges, and box
 * drawing characters do not meet across cell boundaries.
 *
 * The advance also comes from the font face rather than from laying out a
 * "W" and taking DWRITE_TEXT_METRICS.width, which is the measured ink width
 * of that one string and not the font's advance. */
static bool measure_cell(FontState *fs, float dpi) {
    if (!fs->factory||!fs->fmt_normal) return false;

    const float px = fs->pt_size * dpi / 72.0f;
    IDWriteFontFace *face = make_face(fs->factory, fs->family);

    if (face) {
        DWRITE_FONT_METRICS fm = {0};
        face->GetMetrics(&fm);

        /* The advance of a glyph every monospace font has. */
        const UINT32 cp = (UINT32)L'W';
        UINT16 glyph = 0;
        DWRITE_GLYPH_METRICS gm = {0};
        float advance_em = 0.0f;
        if (SUCCEEDED(face->GetGlyphIndices(&cp, 1, &glyph)) && glyph != 0 &&
            SUCCEEDED(face->GetDesignGlyphMetrics(&glyph, 1, &gm, FALSE)) &&
            fm.designUnitsPerEm != 0) {
            advance_em = (float)gm.advanceWidth / (float)fm.designUnitsPerEm;
        }

        if (advance_em > 0.0f) {
            fs->cell_width = floorf(advance_em * px + 0.5f);
        }
        if (fm.designUnitsPerEm != 0) {
            const float upem = (float)fm.designUnitsPerEm;
            const float ascent  = (float)fm.ascent  / upem * px;
            const float descent = (float)fm.descent / upem * px;
            const float gap     = (float)fm.lineGap / upem * px;
            fs->cell_height = floorf(ascent + descent + gap + 0.5f);
            fs->baseline    = floorf(ascent + (gap * 0.5f) + 0.5f);
        }
        face->Release();
    }

    /* Fall back to measuring if the face or its metrics were unavailable. */
    if (fs->cell_width < 1.0f || fs->cell_height < 1.0f) {
        IDWriteTextLayout *layout=NULL;
        HRESULT hr=fs->factory->CreateTextLayout(L"W",1,fs->fmt_normal,10000.0f,10000.0f,&layout);
        if (FAILED(hr)||!layout) return false;
        DWRITE_TEXT_METRICS m={0}; layout->GetMetrics(&m);
        if (fs->cell_width  < 1.0f) fs->cell_width  = floorf(m.width  + 0.5f);
        if (fs->cell_height < 1.0f) fs->cell_height = floorf(m.height + 0.5f);
        DWRITE_LINE_SPACING_METHOD lsm; float ls=0,bl=0;
        layout->GetLineSpacing(&lsm,&ls,&bl);
        if (fs->baseline < 1.0f) fs->baseline = floorf(bl + 0.5f);
        layout->Release();
    }

    if (fs->cell_width<1.0f)  fs->cell_width=8.0f;
    if (fs->cell_height<1.0f) fs->cell_height=16.0f;
    return true;
}

bool font_init(FontState *fs, const wchar_t *family, float pt_size, float dpi) {
    memset(fs,0,sizeof(*fs));
    fs->pt_size=pt_size>0?pt_size:13.0f;
    HRESULT hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown**)&fs->factory);
    if (FAILED(hr)||!fs->factory) { WISP_LOG_ERROR("DWriteCreateFactory: 0x%08X",hr); return false; }
    /* Resolve before creating any format: CreateTextFormat would otherwise
     * substitute silently and we would measure a font we are not drawing. */
    resolve_family(fs->factory, family?family:L"GeistMono NF", fs->family,
                   sizeof(fs->family)/sizeof(fs->family[0]));
    float px=fs->pt_size*dpi/72.0f;
    fs->fmt_normal     =make_fmt(fs->factory,fs->family,px,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL);
    fs->fmt_bold       =make_fmt(fs->factory,fs->family,px,DWRITE_FONT_WEIGHT_BOLD,  DWRITE_FONT_STYLE_NORMAL);
    fs->fmt_italic     =make_fmt(fs->factory,fs->family,px,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_ITALIC);
    fs->fmt_bold_italic=make_fmt(fs->factory,fs->family,px,DWRITE_FONT_WEIGHT_BOLD,  DWRITE_FONT_STYLE_ITALIC);
    if (!fs->fmt_normal) { font_free(fs); return false; }
    return measure_cell(fs,dpi);
}

bool font_resize(FontState *fs, float pt_size, float dpi) {
    if (!fs->factory) return false;
    if (fs->fmt_normal)     { fs->fmt_normal->Release();      fs->fmt_normal=NULL; }
    if (fs->fmt_bold)       { fs->fmt_bold->Release();        fs->fmt_bold=NULL; }
    if (fs->fmt_italic)     { fs->fmt_italic->Release();      fs->fmt_italic=NULL; }
    if (fs->fmt_bold_italic){ fs->fmt_bold_italic->Release(); fs->fmt_bold_italic=NULL; }
    fs->pt_size=pt_size; float px=pt_size*dpi/72.0f;
    fs->fmt_normal     =make_fmt(fs->factory,fs->family,px,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL);
    fs->fmt_bold       =make_fmt(fs->factory,fs->family,px,DWRITE_FONT_WEIGHT_BOLD,  DWRITE_FONT_STYLE_NORMAL);
    fs->fmt_italic     =make_fmt(fs->factory,fs->family,px,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_ITALIC);
    fs->fmt_bold_italic=make_fmt(fs->factory,fs->family,px,DWRITE_FONT_WEIGHT_BOLD,  DWRITE_FONT_STYLE_ITALIC);
    return measure_cell(fs,dpi);
}

void font_free(FontState *fs) {
    if (fs->fmt_bold_italic){ fs->fmt_bold_italic->Release(); fs->fmt_bold_italic=NULL; }
    if (fs->fmt_italic)     { fs->fmt_italic->Release();      fs->fmt_italic=NULL; }
    if (fs->fmt_bold)       { fs->fmt_bold->Release();        fs->fmt_bold=NULL; }
    if (fs->fmt_normal)     { fs->fmt_normal->Release();      fs->fmt_normal=NULL; }
    if (fs->factory)        { fs->factory->Release();         fs->factory=NULL; }
}
