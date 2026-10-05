/* font.cpp — DirectWrite font management, compiled as C++ */
#include <windows.h>
#include <dwrite.h>
#include <string.h>
#include "font.h"
#include "../core/str_util.h"
#include "../core/log.h"

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

static bool measure_cell(FontState *fs, float /*dpi*/) {
    if (!fs->factory||!fs->fmt_normal) return false;
    IDWriteTextLayout *layout=NULL;
    HRESULT hr=fs->factory->CreateTextLayout(L"W",1,fs->fmt_normal,10000.0f,10000.0f,&layout);
    if (FAILED(hr)||!layout) return false;
    DWRITE_TEXT_METRICS m={0}; layout->GetMetrics(&m);
    fs->cell_width=m.width; fs->cell_height=m.height;
    DWRITE_LINE_SPACING_METHOD lsm; float ls=0,bl=0;
    layout->GetLineSpacing(&lsm,&ls,&bl); fs->baseline=bl;
    layout->Release();
    if (fs->cell_width<1.0f)  fs->cell_width=8.0f;
    if (fs->cell_height<1.0f) fs->cell_height=16.0f;
    return true;
}

bool font_init(FontState *fs, const wchar_t *family, float pt_size, float dpi) {
    memset(fs,0,sizeof(*fs));
    wcsncpy(fs->family, family?family:L"Cascadia Code", 127);
    fs->pt_size=pt_size>0?pt_size:13.0f;
    HRESULT hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown**)&fs->factory);
    if (FAILED(hr)||!fs->factory) { WISP_LOG_ERROR("DWriteCreateFactory: 0x%08X",hr); return false; }
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
