/*
 * watermark.c - Windows 图片水印工具 (纯 C, Win32 + WIC + GDI)
 *
 * 功能:
 *   - 打开常见图片 (PNG / JPEG / BMP / TIFF / GIF, 由 WIC 解码)
 *   - 预览界面, 可设置: 水印文字、字号、颜色、倾斜角度、
 *     填充密度(数量)、透明度
 *   - 保存时按原格式重新编码, JPEG 质量 100%, PNG 无损, 保证画质
 *
 * 编译 (TDM-GCC):
 *   gcc -O2 -mwindows -o watermark.exe watermark.c \
 *       -lcomctl32 -lgdi32 -lole32 -loleaut32 -luuid -lcomdlg32 -lm
 */
#define INITGUID
#define _USE_MATH_DEFINES
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <oleauto.h>
#include <wincodec.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* 控件 ID                                                             */
/* ------------------------------------------------------------------ */
#define IDC_OPEN        1001
#define IDC_SAVE        1002
#define IDC_TEXT        1003
#define IDC_COLOR       1004
#define IDC_TB_FONT     1005
#define IDC_TB_ANGLE    1006
#define IDC_TB_DENSITY  1007
#define IDC_TB_OPACITY  1008
#define IDC_ST_FONT     1009
#define IDC_ST_ANGLE    1010
#define IDC_ST_DENSITY  1011
#define IDC_ST_OPACITY  1012
#define IDC_ST_INFO     1013

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */
static HINSTANCE g_hInst;
static HWND      g_hWnd;

static uint8_t  *g_orig = NULL;   /* 原始像素 BGRA */
static uint8_t  *g_res  = NULL;   /* 加水印后像素 BGRA */
static int  g_w = 0, g_h = 0;
static wchar_t g_srcPath[MAX_PATH] = L"";
static wchar_t g_ext[16] = L".png";   /* 保存时默认扩展名 */

/* 水印参数 (默认: 红色, 字号50, 密度3, 透明度20%%, 仰角30度) */
static wchar_t g_text[256]  = L"水印文字";
static int  g_fontSize      = 50;
static COLORREF g_color     = RGB(255, 0, 0);
static int  g_angle         = 30;    /* -90..90 度 */
static int  g_density       = 3;     /* 水平方向平铺数量 1..20 */
static int  g_opacity       = 20;    /* 0..100 % */
static ULONG_PTR g_gdiToken   = 0;   /* GDI+ 会话令牌 */
static int  g_dpi             = 96;  /* 系统 DPI, 用于布局缩放 */
#define S(v) MulDiv((v), g_dpi, 96)  /* 逻辑像素 -> 物理像素 */

/* ------------------------------------------------------------------ */
/* WIC: 载入图片 -> BGRA32                                             */
/* ------------------------------------------------------------------ */
static int loadImage(const wchar_t *path)
{
    HRESULT hr;
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&fac);
    if (FAILED(hr)) return -1;

    hr = fac->lpVtbl->CreateDecoderFromFilename(
             fac, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
    if (FAILED(hr)) { fac->lpVtbl->Release(fac); return -2; }

    hr = dec->lpVtbl->GetFrame(dec, 0, &frame);
    if (FAILED(hr)) goto fail;

    hr = fac->lpVtbl->CreateFormatConverter(fac, &conv);
    if (FAILED(hr)) goto fail;

    hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame,
              &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
              NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) goto fail;

    {
        UINT w = 0, h = 0, stride;
        conv->lpVtbl->GetSize(conv, &w, &h);
        if (w == 0 || h == 0 || (double)w * h > 400e6) goto fail;
        stride = w * 4;
        uint8_t *buf = (uint8_t *)malloc((size_t)stride * h);
        if (!buf) goto fail;
        hr = conv->lpVtbl->CopyPixels(conv, NULL, stride, stride * h, buf);
        if (FAILED(hr)) { free(buf); goto fail; }

        free(g_orig); free(g_res);
        g_orig = buf;
        g_res  = (uint8_t *)malloc((size_t)stride * h);
        g_w = (int)w; g_h = (int)h;
    }

fail:
    if (conv)  conv->lpVtbl->Release(conv);
    if (frame) frame->lpVtbl->Release(frame);
    if (dec)   dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
    return FAILED(hr) ? -3 : 0;
}

/* ------------------------------------------------------------------ */
/* GDI+: 保存图片 (按扩展名选择编码器, JPEG 质量 100%)              */
/* 说明: 本机 WIC 编码器 Initialize 始终返回 UNSUPPORTEDOPERATION,   */
/*       故保存改用 GDI+ 平面 API; 读取仍用 WIC。                    */
/*       TDM-GCC 自带的 gdiplus 头文件无法在 C 下编译, 这里手工声明。 */
/* ------------------------------------------------------------------ */
#ifndef STGM_TRUNCATE_EXISTING
#define STGM_TRUNCATE_EXISTING 0x00000200
#endif
typedef void *GpImage;
typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;
typedef struct {
    GUID Guid;
    ULONG NumberOfValues;
    ULONG Type;
    void *Value;
} GpEncoderParameter;
typedef struct {
    UINT Count;
    GpEncoderParameter Parameter[1];
} GpEncoderParameters;

extern int __stdcall GdiplusStartup(ULONG_PTR *, const GdiplusStartupInput *, void *);
extern void __stdcall GdiplusShutdown(ULONG_PTR);
extern int __stdcall GdipCreateBitmapFromScan0(int, int, int, int, BYTE *, void **);
extern int __stdcall GdipDisposeImage(GpImage *);
extern int __stdcall GdipSaveImageToStream(GpImage *, IStream *, const CLSID *, const GpEncoderParameters *);

#define GP_PIXELFORMAT_32BPP_ARGB 0x0026200A
#define GP_ENCODER_VALUE_TYPE_LONG 4
static const CLSID CLSID_GpEncoderPng  = {0x557CF406,0x1A04,0x11D3,{0x9A,0x73,0x00,0x00,0xF8,0x1E,0xF3,0x2E}};
static const CLSID CLSID_GpEncoderJpeg = {0x557CF401,0x1A04,0x11D3,{0x9A,0x73,0x00,0x00,0xF8,0x1E,0xF3,0x2E}};
static const CLSID CLSID_GpEncoderBmp  = {0x557CF400,0x1A04,0x11D3,{0x9A,0x73,0x00,0x00,0xF8,0x1E,0xF3,0x2E}};
static const CLSID CLSID_GpEncoderGif  = {0x557CF402,0x1A04,0x11D3,{0x9A,0x73,0x00,0x00,0xF8,0x1E,0xF3,0x2E}};
static const CLSID CLSID_GpEncoderTiff = {0x557CF405,0x1A04,0x11D3,{0x9A,0x73,0x00,0x00,0xF8,0x1E,0xF3,0x2E}};
static const GUID  GUID_GpEncoderQuality = {0x1d5be4b5,0xfa4a,0x452d,{0x9c,0xdd,0x5d,0xb3,0x51,0x05,0xe7,0xeb}};

/* 调试: 记录保存失败步骤 */
static const char *g_saveStep = "";
static HRESULT g_saveHr = 0;

static int saveImage(const wchar_t *path, const uint8_t *px, int w, int h)
{
    const CLSID *clsid = NULL;
    const wchar_t *ext = wcsrchr(path, L'.');
    IStream *stream = NULL;
    void *img = NULL;
    HRESULT hr = E_FAIL;
    int st;

    if (!ext) return -1;
    if      (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0)
        clsid = &CLSID_GpEncoderJpeg;
    else if (_wcsicmp(ext, L".png") == 0)
        clsid = &CLSID_GpEncoderPng;
    else if (_wcsicmp(ext, L".bmp") == 0)
        clsid = &CLSID_GpEncoderBmp;
    else if (_wcsicmp(ext, L".gif") == 0)
        clsid = &CLSID_GpEncoderGif;
    else if (_wcsicmp(ext, L".tif") == 0 || _wcsicmp(ext, L".tiff") == 0)
        clsid = &CLSID_GpEncoderTiff;
    else return -1;

    if (g_gdiToken == 0) {
        GdiplusStartupInput in;
        memset(&in, 0, sizeof(in));
        in.GdiplusVersion = 1;
        if (GdiplusStartup(&g_gdiToken, &in, NULL) != 0) return -2;
    }

    if (GdipCreateBitmapFromScan0(w, h, w * 4, GP_PIXELFORMAT_32BPP_ARGB,
                                  (BYTE *)px, &img) != 0) {
        g_saveStep = "GdipCreateBitmapFromScan0";
        return -2;
    }

    hr = SHCreateStreamOnFileW(path, STGM_CREATE | STGM_WRITE, &stream);
    g_saveHr = hr; g_saveStep = "SHCreateStreamOnFileW";
    if (FAILED(hr)) goto done;

    {
        GpEncoderParameters ep;
        ULONG q = 100;
        const GpEncoderParameters *p = NULL;
        if (clsid == &CLSID_GpEncoderJpeg) {
            ep.Count = 1;
            ep.Parameter[0].Guid = GUID_GpEncoderQuality;
            ep.Parameter[0].Type = GP_ENCODER_VALUE_TYPE_LONG;
            ep.Parameter[0].NumberOfValues = 1;
            ep.Parameter[0].Value = &q;
            p = &ep;
        }
        st = GdipSaveImageToStream((GpImage *)img, stream, clsid, p);
        g_saveHr = (HRESULT)st;
        g_saveStep = "GdipSaveImageToStream";
        if (st != 0) { hr = E_FAIL; goto done; }
    }
    hr = S_OK;

done:
    if (img) GdipDisposeImage((GpImage *)img);
    if (stream) stream->lpVtbl->Release(stream);
    return FAILED(hr) ? -3 : 0;
}

/* ------------------------------------------------------------------ */
/* GDI: 生成水印文字的灰度遮罩 (白色文字/黑底 -> alpha)             */
/* ------------------------------------------------------------------ */
static void makeTextMask(const wchar_t *text, int fontSize,
                         uint8_t **outMask, int *outW, int *outH)
{
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HFONT oldFont = (HFONT)SelectObject(mem, font);

    SIZE sz;
    GetTextExtentPoint32W(mem, text, (int)wcslen(text), &sz);
    int mw = sz.cx + 8, mh = sz.cy + 8;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = mw;
    bi.bmiHeader.biHeight = -mh;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    uint8_t *bits = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

    SetBkColor(mem, RGB(0, 0, 0));
    SetTextColor(mem, RGB(255, 255, 255));
    TextOutW(mem, 4, 4, text, (int)wcslen(text));

    uint8_t *mask = (uint8_t *)malloc((size_t)mw * mh);
    for (int i = 0; i < mw * mh; i++)
        mask[i] = bits[i * 4];            /* 白字 -> R 通道即亮度/alpha */

    SelectObject(mem, oldBmp);
    SelectObject(mem, oldFont);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);

    *outMask = mask; *outW = mw; *outH = mh;
}

/* ------------------------------------------------------------------ */
/* 在 (cx,cy) 处绘制一个旋转后的水印 tile (双线性采样遮罩)       */
/* ------------------------------------------------------------------ */
static void blendTile(uint8_t *img, const uint8_t *mask, int mw, int mh,
                      int cx, int cy, double cs, double sn,
                      COLORREF color, int opacity)
{
    double bw = mw * fabs(cs) + mh * fabs(sn);
    double bh = mw * fabs(sn) + mh * fabs(cs);
    int halfW = (int)(bw / 2) + 1, halfH = (int)(bh / 2) + 1;

    int x0 = cx - halfW, x1 = cx + halfW;
    int y0 = cy - halfH, y1 = cy + halfH;
    if (x0 < 0) x0 = 0;
    if (x1 > g_w - 1) x1 = g_w - 1;
    if (y0 < 0) y0 = 0;
    if (y1 > g_h - 1) y1 = g_h - 1;

    int R = GetRValue(color), G = GetGValue(color), B = GetBValue(color);
    double mhalf = mw * 0.5, nhalf = mh * 0.5;

    for (int y = y0; y <= y1; y++) {
        double dy = y - cy;
        uint8_t *p = img + ((size_t)y * g_w + x0) * 4;
        for (int x = x0; x <= x1; x++, p += 4) {
            double dx = x - cx;
            double sx = dx * cs + dy * sn + mhalf;
            double sy = -dx * sn + dy * cs + nhalf;
            if (sx < 0 || sy < 0 || sx > mw - 1 || sy > mh - 1) continue;

            int fx = (int)sx, fy = (int)sy;
            double u = sx - fx, v = sy - fy;
            int fx1 = fx + 1 < mw ? fx + 1 : fx;
            int fy1 = fy + 1 < mh ? fy + 1 : fy;
            double a00 = mask[fy * mw + fx],  a10 = mask[fy * mw + fx1];
            double a01 = mask[fy1 * mw + fx], a11 = mask[fy1 * mw + fx1];
            double alpha = (a00 * (1 - u) + a10 * u) * (1 - v)
                         + (a01 * (1 - u) + a11 * u) * v;
            alpha *= (opacity / 100.0);
            if (alpha <= 0.5) continue;
            double k = alpha / 255.0;
            p[0] = (uint8_t)(p[0] + (B - p[0]) * k);
            p[1] = (uint8_t)(p[1] + (G - p[1]) * k);
            p[2] = (uint8_t)(p[2] + (R - p[2]) * k);
            /* 不改变目标 alpha, 保持原图画质 */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 应用水印: 原图 -> 结果图                                            */
/* ------------------------------------------------------------------ */
static void applyWatermark(void)
{
    if (!g_orig || !g_res) return;
    memcpy(g_res, g_orig, (size_t)g_w * g_h * 4);

    /* 去除首尾空格后为空则不加水印 */
    wchar_t *t = g_text;
    while (*t == L' ') t++;
    if (!*t) return;

    uint8_t *mask; int mw, mh;
    makeTextMask(t, g_fontSize, &mask, &mw, &mh);

    double ang = g_angle * (M_PI / 180.0);
    double cs = cos(ang), sn = sin(ang);

    double bw = mw * fabs(cs) + mh * fabs(sn);
    double bh = mw * fabs(sn) + mh * fabs(cs);

    int cols = g_density;
    double cellW = (double)g_w / cols;
    if (cellW < bw) cellW = bw;                 /* 太密时按 tile 宽度 */
    double cellH = cellW * bh / bw;             /* 保持 tile 纵横比例 */

    for (int row = 0; ; row++) {
        double cy = (row + 0.5) * cellH;
        if (cy - bh / 2 > g_h) break;
        for (int col = 0; col <= cols; col++) {
            double cx = (col + ((row & 1) ? 0.75 : 0.25)) * cellW;
            if (cx - bw / 2 > g_w) break;
            blendTile(g_res, mask, mw, mh, (int)cx, (int)cy, cs, sn,
                      g_color, g_opacity);
        }
    }
    free(mask);
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */
static void setTrack(HWND hWnd, int id, int lo, int hi, int val)
{
    HWND tb = GetDlgItem(hWnd, id);
    /* 注意: WM_CREATE 阶段全局 g_hWnd 还未赋值, 必须用传入的 hWnd,
       否则 GetDlgItem 返回 NULL, 滑条的范围和位置根子上没被设置。
       另外 TBM_SETRANGE 的 lParam 用 16 位打包, 负数下限会被当成无符号数,
       所以角度滑条统一用 0..180, 由调用方加 90 偏移。 */
    if (!tb) return;
    SendMessage(tb, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessage(tb, TBM_SETPOS, TRUE, val);
}

static void updateLabels(HWND hWnd)
{
    wchar_t buf[64];
    wsprintfW(buf, L"%d px", g_fontSize);
    SetDlgItemTextW(hWnd, IDC_ST_FONT, buf);
    wsprintfW(buf, L"%d \x5EA6", g_angle);            /* 度 */
    SetDlgItemTextW(hWnd, IDC_ST_ANGLE, buf);
    wsprintfW(buf, L"%d", g_density);
    SetDlgItemTextW(hWnd, IDC_ST_DENSITY, buf);
    wsprintfW(buf, L"%d %%", g_opacity);
    SetDlgItemTextW(hWnd, IDC_ST_OPACITY, buf);
}

/* 计算预览图像在窗口中的目标矩形 (客户区坐标) */
static void calcPreview(int *dx, int *dy, int *dw, int *dh)
{
    RECT rc; GetClientRect(g_hWnd, &rc);
    int areaX = S(12), areaY = S(124);
    int areaW = rc.right - areaX - S(12);
    int areaH = rc.bottom - areaY - S(12);
    *dw = 0; *dh = 0;
    if (g_w > 0 && areaW > 0 && areaH > 0) {
        double scale = (double)areaW / g_w;
        if ((double)areaH / g_h < scale) scale = (double)areaH / g_h;
        if (scale > 1.0) scale = 1.0;
        *dw = (int)(g_w * scale); *dh = (int)(g_h * scale);
        *dx = areaX + (areaW - *dw) / 2;
        *dy = areaY + (areaH - *dh) / 2;
    } else {
        *dx = areaX; *dy = areaY; *dw = areaW; *dh = areaH;
    }
}

/* 只重绘预览区域(不擦背景), 避免控件区闪烁 */
static void invalidatePreview(void)
{
    int dx, dy, dw, dh;
    calcPreview(&dx, &dy, &dw, &dh);
    RECT r = { dx - 4, dy - 4, dx + dw + 4, dy + dh + 4 };
    InvalidateRect(g_hWnd, &r, FALSE);
}

static void refreshPreview(void)
{
    updateLabels(g_hWnd);
    invalidatePreview();
}

static void doOpen(void)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_hWnd;
    of.lpstrFilter =
        L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif)\0"
        L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif\0"
        L"所有文件 (*.*)\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;

    int r = loadImage(file);
    if (r != 0) {
        MessageBoxW(g_hWnd,
            L"无法打开该图片文件。",   /* 无法打开该图片文件。 */
            L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    wcscpy(g_srcPath, file);
    wchar_t *e = wcsrchr(file, L'.');
    if (e) { wcsncpy(g_ext, e, 15); g_ext[15] = 0; }
    applyWatermark();
    refreshPreview();
}

static void doSave(void)
{
    if (!g_res) {
        MessageBoxW(g_hWnd, L"请先打开图片。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t file[MAX_PATH];
    /* 默认文件名: 原名 + _水印 */
    wchar_t base[MAX_PATH];
    wcscpy(base, g_srcPath);
    wchar_t *e = wcsrchr(base, L'.');
    if (e) *e = 0;
    wsprintfW(file, L"%s%s", base, L"_水印.png");

    OPENFILENAMEW sf;
    memset(&sf, 0, sizeof(sf));
    sf.lStructSize = sizeof(sf);
    sf.hwndOwner = g_hWnd;
    sf.lpstrFilter =
        L"PNG (*.png)\0*.png\0JPEG (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
        L"BMP (*.bmp)\0*.bmp\0TIFF (*.tif;*.tiff)\0*.tif;*.tiff\0"
        L"GIF (*.gif)\0*.gif\0所有文件 (*.*)\0*.*\0";
    sf.lpstrFile = file;
    sf.nMaxFile = MAX_PATH;
    sf.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    sf.nFilterIndex = 1;
    if (!GetSaveFileNameW(&sf)) return;

    int r = saveImage(file, g_res, g_w, g_h);
    if (r != 0) {
        wchar_t msg[256];
        wsprintfW(msg, L"保存失败 (代码 %d)。", r);
        MessageBoxW(g_hWnd, msg, L"错误", MB_OK | MB_ICONERROR);
    } else {
        MessageBoxW(g_hWnd, L"保存成功！", L"提示", MB_OK | MB_ICONINFORMATION);
    }
}

static void doChooseColor(void)
{
    CHOOSECOLORW cc;
    static COLORREF acrCmt[16] = {0};
    memset(&cc, 0, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_hWnd;
    cc.lpCustColors = acrCmt;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    cc.rgbResult = g_color;
    if (ChooseColorW(&cc)) {
        g_color = cc.rgbResult;
        applyWatermark();
        invalidatePreview();
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        static const wchar_t *CLS = L"STATIC";
        static const wchar_t *BTN = L"BUTTON";
        static const wchar_t *EDT = L"EDIT";
        static const wchar_t *TRK = TRACKBAR_CLASSW;
        HFONT hFont = CreateFontW(S(-14), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                  L"Microsoft YaHei");
        #define MK(cls, txt, id, x, y, w, h, style) \
            do { HWND _c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, \
                            x, y, w, h, hWnd, (HMENU)(INT_PTR)id, g_hInst, NULL); \
                 /* 立即设字体: 部分 STATIC 的 id 为 0, 无法用 ID 循环覆盖 */ \
                 if (_c) SendMessageW(_c, WM_SETFONT, (WPARAM)hFont, TRUE); } while (0)
        #define FONTALL(id) SendDlgItemMessageW(hWnd, id, WM_SETFONT, (WPARAM)hFont, TRUE)

        MK(BTN, L"打开图片", IDC_OPEN, S(12), S(12), S(100), S(32), BS_PUSHBUTTON);
        MK(BTN, L"保存图片", IDC_SAVE, S(122), S(12), S(100), S(32), BS_PUSHBUTTON);

        MK(CLS, L"水印文字:", 0, S(240), S(18), S(70), S(20), 0);
        MK(EDT, g_text, IDC_TEXT, S(312), S(14), S(240), S(28),
           WS_BORDER | ES_AUTOHSCROLL);

        MK(BTN, L"文字颜色...", IDC_COLOR, S(566), S(12), S(100), S(32), BS_PUSHBUTTON);

        MK(CLS, L"字号:", 0, S(684), S(18), S(40), S(20), 0);
        MK(TRK, NULL, IDC_TB_FONT, S(726), S(16), S(160), S(22), 0);
        MK(CLS, L"36 px", IDC_ST_FONT, S(892), S(18), S(60), S(20), 0);

        MK(CLS, L"倾斜角度:", 0, S(12), S(58), S(70), S(20), 0);
        MK(TRK, NULL, IDC_TB_ANGLE, S(84), S(56), S(180), S(22), 0);
        MK(CLS, L"30 度", IDC_ST_ANGLE, S(270), S(58), S(60), S(20), 0);

        MK(CLS, L"填充密度:", 0, S(344), S(58), S(70), S(20), 0);
        MK(TRK, NULL, IDC_TB_DENSITY, S(416), S(56), S(180), S(22), 0);
        MK(CLS, L"5", IDC_ST_DENSITY, S(602), S(58), S(40), S(20), 0);

        MK(CLS, L"透明度:", 0, S(656), S(58), S(60), S(20), 0);
        MK(TRK, NULL, IDC_TB_OPACITY, S(718), S(56), S(168), S(22), 0);
        MK(CLS, L"35 %", IDC_ST_OPACITY, S(892), S(58), S(60), S(20), 0);

        MK(CLS,
           L"打开图片后可实时预览水印效果, 调节滑块即时生效; 保存时按原格式高质量编码。",
           IDC_ST_INFO, S(12), S(92), S(900), S(20), 0);

        setTrack(hWnd, IDC_TB_FONT,    10, 200, g_fontSize);
        setTrack(hWnd, IDC_TB_ANGLE,    0, 180, g_angle + 90);  /* 滑条用 0..180, 避免负数下限 */
        setTrack(hWnd, IDC_TB_DENSITY,  1,  20, g_density);
        setTrack(hWnd, IDC_TB_OPACITY,  1, 100, g_opacity);
        updateLabels(hWnd);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OPEN: doOpen(); return 0;
        case IDC_SAVE: doSave(); return 0;
        case IDC_COLOR: doChooseColor(); return 0;
        case IDC_TEXT:
            if (HIWORD(wp) == EN_CHANGE) {
                /* 防抖: 150ms 内无新输入才重新生成水印 */
                SetTimer(hWnd, 1, 150, NULL);
            }
            return 0;
        case IDCANCEL: DestroyWindow(hWnd); return 0;
        }
        break;

    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hWnd, 1);
            GetDlgItemTextW(hWnd, IDC_TEXT, g_text, 255);
            applyWatermark();
            invalidatePreview();
        }
        return 0;

    case WM_HSCROLL: {
        HWND tb = (HWND)lp;
        int id = GetDlgCtrlID(tb);
        int pos = (int)SendMessage(tb, TBM_GETPOS, 0, 0);
        int event = LOWORD(wp);
        switch (id) {
        case IDC_TB_FONT:    g_fontSize = pos; break;
        case IDC_TB_ANGLE:   g_angle    = pos - 90; break;
        case IDC_TB_DENSITY: g_density  = pos; break;
        case IDC_TB_OPACITY: g_opacity  = pos; break;
        default: return 0;
        }
        updateLabels(g_hWnd);
        /* 拖动过程中只更新数字, 松手/点击时才重算水印, 避免卡顿抖动 */
        if (event == TB_THUMBTRACK) return 0;
        applyWatermark();
        invalidatePreview();
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT rc; GetClientRect(hWnd, &rc);
        HBRUSH br = CreateSolidBrush(RGB(240, 240, 240));
        FillRect(dc, &rc, br);
        DeleteObject(br);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hWnd, &ps);

        /* 双缓冲: 先在离屏位图绘制预览区, 再一次性 Blt, 消除闪烁 */
        RECT rc; GetClientRect(hWnd, &rc);
        RECT area = { 8, 118, rc.right - 8, rc.bottom - 8 };
        int aw = area.right - area.left, ah = area.bottom - area.top;
        if (aw > 0 && ah > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bm = CreateCompatibleBitmap(dc, aw, ah);
            HBITMAP oldBm = (HBITMAP)SelectObject(mem, bm);

            HBRUSH bg = CreateSolidBrush(RGB(240, 240, 240));
            RECT marea = { 0, 0, aw, ah };
            FillRect(mem, &marea, bg);
            DeleteObject(bg);

            if (g_res && g_w > 0) {
                int dx, dy, dw, dh;
                calcPreview(&dx, &dy, &dw, &dh);

                RECT pr = { dx - area.left - 1, dy - area.top - 1,
                            dx - area.left + dw + 1, dy - area.top + dh + 1 };
                HBRUSH br = CreateSolidBrush(RGB(200, 200, 200));
                FillRect(mem, &pr, br);
                DeleteObject(br);

                BITMAPINFO bi;
                memset(&bi, 0, sizeof(bi));
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = g_w;
                bi.bmiHeader.biHeight = -g_h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;

                SetStretchBltMode(mem, HALFTONE);
                SetBrushOrgEx(mem, 0, 0, NULL);
                StretchDIBits(mem, dx - area.left, dy - area.top, dw, dh,
                              0, 0, g_w, g_h, g_res, &bi, DIB_RGB_COLORS, SRCCOPY);
            } else {
                SetTextColor(mem, RGB(120, 120, 120));
                SetBkMode(mem, TRANSPARENT);
                RECT tr = { 0, ah / 2 - 20, aw, ah / 2 + 20 };
                DrawTextW(mem, L"点击 [打开图片] 选择一张图片", -1, &tr,
                          DT_CENTER | DT_SINGLELINE);
            }

            BitBlt(dc, area.left, area.top, aw, ah, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBm);
            DeleteObject(bm);
            DeleteDC(mem);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev,
                    LPWSTR cmdLine, int nShow)
{
    g_hInst = hInst;
    CoInitialize(NULL);
    {
        /* 开启 DPI 感知, 否则高缩放屏幕上整个窗口会被拉伸模糊 */
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        SetCtxFn f = u32 ? (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
        if (f)
            f((HANDLE)-4);                     /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        else {
            typedef BOOL (WINAPI *SetAwareFn)(void);
            SetAwareFn g = u32 ? (SetAwareFn)GetProcAddress(u32, "SetProcessDPIAware") : NULL;
            if (g) g();
        }
        HDC sdc = GetDC(NULL);
        g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
        ReleaseDC(NULL, sdc);
        if (g_dpi < 96) g_dpi = 96;
    }
    {
        GdiplusStartupInput gsi;
        memset(&gsi, 0, sizeof(gsi));
        gsi.GdiplusVersion = 1;
        GdiplusStartup(&g_gdiToken, &gsi, NULL);
    }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"WatermarkApp";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));   /* 程序图标 watermark.ico */
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowW(L"WatermarkApp",
        L"图片水印工具",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(760),
        NULL, NULL, hInst, NULL);
    ShowWindow(g_hWnd, nShow);

    /* 命令行直接打开图片 */
    if (cmdLine && *cmdLine) {
        wchar_t path[MAX_PATH];
        if (*cmdLine == L'"') {
            wchar_t *end = wcschr(cmdLine + 1, L'"');
            if (end) { wcsncpy(path, cmdLine + 1, end - cmdLine - 1);
                       path[end - cmdLine - 1] = 0; }
            else wcscpy(path, cmdLine + 1);
        } else wcscpy(path, cmdLine);
        if (loadImage(path) == 0) {
            wcscpy(g_srcPath, path);
            wchar_t *e = wcsrchr(path, L'.');
            if (e) { wcsncpy(g_ext, e, 15); g_ext[15] = 0; }
            applyWatermark();
            InvalidateRect(g_hWnd, NULL, TRUE);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    free(g_orig); free(g_res);
    if (g_gdiToken) { GdiplusShutdown(g_gdiToken); g_gdiToken = 0; }
    CoUninitialize();
    return 0;
}
