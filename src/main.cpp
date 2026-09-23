// vxe-monitor — Windows 托盘鼠标(VXE/ATK COMPX 方案)电量显示
//
// HID 协议层见 src/vxe_hid.h(逆向自 ATK HUB Web 驱动,协议详情见 README.md)。
// 托盘数字图标 / OSD 悬浮 / 开机自启动实现改编自 rapoo-tray
// (https://github.com/Iris-0109/rapoo-tray, MIT License, Copyright (c) Iris-0109)。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include "vxe_hid.h"
#include <shellapi.h>
#include <strsafe.h>
#include <stdint.h>
#include <stdio.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define WM_TRAY_ICON      (WM_USER + 101)
#define WM_APP_BAT_UPDATE (WM_USER + 103)

#define IDM_HEADER    2001
#define IDM_BATTERY   2002
#define IDM_STATUS    2003
#define IDM_VOLTAGE   2004
#define IDM_CONNECT   2005
#define IDM_AUTORUN   2006
#define IDM_RECONNECT 2007
#define IDM_EXIT      2008
#define IDM_VERSION   2009

#define TIMER_OSD_HIDE 3001
#define TIMER_OSD_FADE 3002

#define OSD_KEY_COLOR RGB(255, 0, 255)
#define POLL_INTERVAL_MS 30000

static HINSTANCE g_hInstance = NULL;
static HWND g_hMainWnd = NULL;
static HWND g_hOsdWnd = NULL;
static NOTIFYICONDATAW g_nid = {0};
static HANDLE g_hHidThread = NULL;
static HANDLE g_hStopEvent = NULL;

// 共享状态(HID 线程写,UI 线程读;字符串由 g_cs 保护)
static volatile LONG g_battery = -1;   // -1 = 未知
static volatile LONG g_charging = 0;
static volatile LONG g_voltage = 0;    // mV
static volatile LONG g_connectType = -1;
static volatile LONG g_online = 0;
static volatile bool g_deviceConnected = false;
static WCHAR g_model[64] = L"VXE 无线鼠标";
static char g_version[16] = "";
static CRITICAL_SECTION g_cs;

static UINT g_uTaskbarRestartMsg = 0;
static const WCHAR* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR* APP_NAME = L"vxe-monitor";
static const WCHAR* APP_VERSION = L"V1.0.0";

static void UpdateTrayTooltip();
static void UpdateTrayIcon();
static void ShowOsdNotification();
static bool IsAutoRunEnabled();
static void SetAutoRun(bool enable);

// ---- 点阵字模 (改编自 rapoo-tray) ----------------------------------------

// 3x5 数字,每行 3 bit
static const uint8_t FONT_3X5[10][5] = {
    { 0x07, 0x05, 0x05, 0x05, 0x07 }, { 0x02, 0x06, 0x02, 0x02, 0x07 },
    { 0x07, 0x01, 0x07, 0x04, 0x07 }, { 0x07, 0x01, 0x07, 0x01, 0x07 },
    { 0x05, 0x05, 0x07, 0x01, 0x01 }, { 0x07, 0x04, 0x07, 0x01, 0x07 },
    { 0x07, 0x04, 0x07, 0x05, 0x07 }, { 0x07, 0x01, 0x02, 0x02, 0x02 },
    { 0x07, 0x05, 0x07, 0x05, 0x07 }, { 0x07, 0x05, 0x07, 0x01, 0x07 }
};
// 5x9 数字,每行 5 bit
static const uint16_t FONT_5X9[10][9] = {
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10, 0x1F },
    { 0x1E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x01, 0x01, 0x1E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02, 0x02, 0x02 },
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x01, 0x01, 0x11, 0x0E },
    { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x1F, 0x01, 0x02, 0x02, 0x04, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x01, 0x01, 0x0E }
};
// "100" 的窄体 0 (4x9)
static const uint8_t FONT_4X9_0[9] = {
    0x06, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x06
};
// 闪电 (充电标志):大号 5x9 / 小号 3x5
static const uint16_t BOLT_5X9[9] = {
    0x01, 0x03, 0x07, 0x0E, 0x06, 0x04, 0x02, 0x02, 0x01
};
static const uint8_t BOLT_3X5[5] = { 0x1, 0x3, 0x2, 0x6, 0x4 };
// 破折号(未知电量 "--" 用单个宽横杠):5x9 / 3x5
static const uint16_t DASH_5X9[9] = { 0, 0, 0, 0, 0x1F, 0, 0, 0, 0 };
static const uint8_t DASH_3X5[5] = { 0, 0, 0x07, 0, 0 };

struct Glyph {
    uint16_t rows[9];
    int fw, fh;
};

static Glyph MakeDigit(int d, bool small) {
    Glyph g;
    memset(&g, 0, sizeof(g));
    if (small) {
        g.fw = 3; g.fh = 5;
        for (int r = 0; r < 5; ++r) g.rows[r] = FONT_3X5[d][r];
    } else {
        g.fw = 5; g.fh = 9;
        for (int r = 0; r < 9; ++r) g.rows[r] = FONT_5X9[d][r];
    }
    return g;
}

static Glyph MakeZeroNarrow() { // "100" 里的 0
    Glyph g;
    memset(&g, 0, sizeof(g));
    g.fw = 4; g.fh = 9;
    for (int r = 0; r < 9; ++r) g.rows[r] = FONT_4X9_0[r];
    return g;
}

static Glyph MakeOneBar(bool small) { // "100" 里的 1(实心窄条)
    Glyph g;
    memset(&g, 0, sizeof(g));
    if (small) { g.fw = 2; g.fh = 5; for (int r = 0; r < 5; ++r) g.rows[r] = 0x3; }
    else       { g.fw = 2; g.fh = 9; for (int r = 0; r < 9; ++r) g.rows[r] = 0x3; }
    return g;
}

static Glyph MakeBolt(bool small) {
    Glyph g;
    memset(&g, 0, sizeof(g));
    if (small) {
        g.fw = 3; g.fh = 5;
        for (int r = 0; r < 5; ++r) g.rows[r] = BOLT_3X5[r];
    } else {
        g.fw = 5; g.fh = 9;
        for (int r = 0; r < 9; ++r) g.rows[r] = BOLT_5X9[r];
    }
    return g;
}

static Glyph MakeDash(bool small) {
    Glyph g;
    memset(&g, 0, sizeof(g));
    if (small) {
        g.fw = 3; g.fh = 5;
        for (int r = 0; r < 5; ++r) g.rows[r] = DASH_3X5[r];
    } else {
        g.fw = 5; g.fh = 9;
        for (int r = 0; r < 9; ++r) g.rows[r] = DASH_5X9[r];
    }
    return g;
}

// ---- 图标渲染 (改编自 rapoo-tray:4x 超采样 + box 降采样抗锯齿) -----------

static void PutPixelARGB(uint32_t* px, int size, int x, int y, uint32_t color) {
    if (x >= 0 && x < size && y >= 0 && y < size) px[y * size + x] = color;
}

static int GetTrayIconSize(HWND hWnd) {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    typedef UINT(WINAPI* pfnGetDpiForWindow)(HWND);
    typedef int(WINAPI* pfnGetSystemMetricsForDpi)(int, UINT);
    pfnGetDpiForWindow fnGetDpiForWindow =
        (pfnGetDpiForWindow)GetProcAddress(hUser, "GetDpiForWindow");
    pfnGetSystemMetricsForDpi fnGetSystemMetricsForDpi =
        (pfnGetSystemMetricsForDpi)GetProcAddress(hUser, "GetSystemMetricsForDpi");
    int sz = 0;
    if (fnGetDpiForWindow && fnGetSystemMetricsForDpi && hWnd) {
        UINT dpi = fnGetDpiForWindow(hWnd);
        if (dpi > 0) sz = fnGetSystemMetricsForDpi(SM_CXSMICON, dpi);
    }
    if (sz <= 0) sz = GetSystemMetrics(SM_CXSMICON);
    if (sz <= 0) sz = 16;
    return sz;
}

static bool IsSystemDarkTheme() {
    DWORD val = 0, size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &val,
                     &size) == ERROR_SUCCESS) {
        return (val == 0);
    }
    return true;
}

static HICON CreateBatteryIcon(int battery, bool charging) {
    int size = GetTrayIconSize(g_hMainWnd);
    if (size <= 0) size = 16;
    const int SS = 4;
    const int W = size * SS;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -W;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = NULL;
    HBITMAP hbmColor = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    if (!hbmColor || !pvBits) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }
    uint32_t* hi = (uint32_t*)pvBits;
    memset(hi, 0, W * W * sizeof(uint32_t));

    bool isDark = IsSystemDarkTheme();
    bool unknown = (battery < 0);

    // 填充色分档:>20 绿 / 11-20 黄 / <=10 红;未知为灰
    const uint32_t c_fill = unknown      ? 0xFF8A8A8A
                          : (battery > 20) ? 0xFF2DD773
                          : (battery > 10) ? 0xFFFFB020
                                           : 0xFFFF4D4F;
    const uint32_t c_frame = isDark ? 0xFFFFFFFF : 0xFF1E1E1E;
    const uint32_t c_digit = 0xFF000000;

    auto HiPixel = [&](int x, int y, uint32_t col) {
        if (x >= 0 && x < W && y >= 0 && y < W) hi[y * W + x] = col;
    };
    auto FillRoundRect = [&](int x0, int y0, int x1, int y1, uint32_t col, int rad) {
        if (rad > (x1 - x0) / 2) rad = (x1 - x0) / 2;
        if (rad > (y1 - y0) / 2) rad = (y1 - y0) / 2;
        if (rad < 0) rad = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                int dx = (x < x0 + rad) ? (x0 + rad - x) : (x > x1 - rad) ? (x - (x1 - rad)) : 0;
                int dy = (y < y0 + rad) ? (y0 + rad - y) : (y > y1 - rad) ? (y - (y1 - rad)) : 0;
                if (dx * dx + dy * dy <= rad * rad) HiPixel(x, y, col);
            }
    };

    int pad_y = (size < 20) ? 1 : ((size <= 24) ? 2 : 3);
    int tip_w = (size < 20) ? 2 : ((size <= 24) ? 3 : 4);
    int tip_h = (size < 20) ? 6 : (size / 3);
    int tip_y = (size - tip_h) / 2;

    int body_x0 = 0;
    int body_x1 = (size - 1 - tip_w) * SS + (SS - 1);
    int body_y0 = pad_y * SS;
    int body_y1 = (size - 1 - pad_y) * SS + (SS - 1);
    int frame_t = 1 * SS;
    int radius = 2 * SS;

    FillRoundRect(body_x0, body_y0, body_x1, body_y1, c_frame, radius);
    FillRoundRect(body_x0 + frame_t, body_y0 + frame_t, body_x1 - frame_t, body_y1 - frame_t,
                  c_fill, (radius > frame_t ? radius - frame_t : 0));
    int tip_x0 = (size - tip_w) * SS;
    int tip_x1 = size * SS - 1;
    int tip_y0 = tip_y * SS;
    int tip_y1 = (tip_y + tip_h) * SS - 1;
    FillRoundRect(tip_x0, tip_y0, tip_x1, tip_y1, c_frame, SS);

    int in_x0 = body_x0 + frame_t;
    int in_x1 = body_x1 - frame_t;
    int in_y0 = body_y0 + frame_t;
    int in_w = in_x1 - in_x0 + 1;
    int in_h = (body_y1 - frame_t) - in_y0 + 1;

    // ---- 构造字形序列:[数字][窄0 x2](或 [--])[闪电?] ----
    Glyph glyphs[5];
    int ng = 0;
    bool small = (size < 20);
    if (unknown) {
        glyphs[ng++] = MakeDash(small);
    } else if (battery == 100) {
        glyphs[ng++] = MakeOneBar(small);
        glyphs[ng++] = MakeZeroNarrow();
        glyphs[ng++] = MakeZeroNarrow();
    } else {
        char s[8];
        snprintf(s, sizeof(s), "%d", battery);
        for (int i = 0; s[i]; ++i) glyphs[ng++] = MakeDigit(s[i] - '0', small);
    }
    if (charging) glyphs[ng++] = MakeBolt(small);

    int gap_fp = (ng > 1) ? 1 : 0;
    int scale = 1;
    int bold_w = 1;
    {
        int fh = small ? 5 : 9;
        int target_h = in_h * 70 / 100;
        scale = target_h / fh;
        if (scale < 1) scale = 1;
        auto TextW = [&](int sc) {
            int w = 0;
            for (int i = 0; i < ng; ++i) w += glyphs[i].fw * sc + bold_w;
            return w + (ng - 1) * gap_fp * sc;
        };
        while (scale > 1 && TextW(scale) > in_w) --scale;

        int text_h = fh * scale;
        int text_w = TextW(scale);
        int sx = in_x0 + (in_w - text_w) / 2;
        int sy = in_y0 + (in_h - text_h) / 2;

        int cur = sx;
        for (int i = 0; i < ng; ++i) {
            const Glyph& g = glyphs[i];
            for (int r = 0; r < g.fh; ++r) {
                for (int c = 0; c < g.fw; ++c) {
                    if (!((g.rows[r] >> (g.fw - 1 - c)) & 1)) continue;
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale + bold_w; ++dx)
                            HiPixel(cur + c * scale + dx, sy + r * scale + dy, c_digit);
                }
            }
            cur += g.fw * scale + bold_w + gap_fp * scale;
        }
    }

    // ---- box 降采样 (alpha 加权平均 = 抗锯齿) ----
    BITMAPINFO bomi = {0};
    bomi.bmiHeader = bmi.bmiHeader;
    bomi.bmiHeader.biWidth = size;
    bomi.bmiHeader.biHeight = -size;
    void* pvOut = NULL;
    HBITMAP hbmOut = CreateDIBSection(hdcMem, &bomi, DIB_RGB_COLORS, &pvOut, NULL, 0);
    if (!hbmOut || !pvOut) {
        DeleteObject(hbmColor);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }
    uint32_t* out = (uint32_t*)pvOut;
    const int SS2 = SS * SS;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t sumA = 0, sumR = 0, sumG = 0, sumB = 0;
            for (int sy = 0; sy < SS; ++sy) {
                const uint32_t* row = hi + (y * SS + sy) * W + x * SS;
                for (int sx2 = 0; sx2 < SS; ++sx2) {
                    uint32_t c = row[sx2];
                    uint32_t a = (c >> 24) & 0xFF;
                    if (a > 0) {
                        sumA += a;
                        sumR += (c >> 16) & 0xFF;
                        sumG += (c >> 8) & 0xFF;
                        sumB += (c & 0xFF);
                    }
                }
            }
            uint32_t a = sumA / SS2;
            if (a < 8) {
                out[y * size + x] = 0;
            } else {
                out[y * size + x] = (a << 24) | ((sumR / SS2) << 16) | ((sumG / SS2) << 8) | (sumB / SS2);
            }
        }
    }

    int maskPitch = ((size + 15) / 16) * 2;
    BYTE maskBits[256] = {0};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (((out[y * size + x] >> 24) & 0xFF) < 32)
                maskBits[y * maskPitch + (x / 8)] |= (1 << (7 - (x % 8)));
        }
    }
    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, maskBits);

    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmOut;
    ii.hbmMask = hbmMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmOut);
    DeleteObject(hbmMask);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return hIcon;
}

// ---- 托盘图标 / 提示文本 --------------------------------------------------

static void UpdateTrayTooltip() {
    EnterCriticalSection(&g_cs);
    if (g_deviceConnected) {
        StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip),
                         L"%s\n电量: %s%d%%\n%s · %dmV",
                         g_model,
                         g_charging ? L"⚡" : L"",
                         (int)g_battery,
                         vxe::ConnectTypeName((int)g_connectType),
                         (int)g_voltage);
    } else {
        StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip),
                         L"vxe-monitor\n(等待设备连接...)");
    }
    LeaveCriticalSection(&g_cs);
}

static void UpdateTrayIcon() {
    HICON hNewIcon = CreateBatteryIcon((int)g_battery, g_charging != 0);
    if (hNewIcon) {
        if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = hNewIcon;
    }
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    UpdateTrayTooltip();
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---- OSD 悬浮窗 (改编自 rapoo-tray) ---------------------------------------

static WCHAR g_osdTextLine1[80] = L"";
static WCHAR g_osdTextLine2[80] = L"";
static BYTE g_osdAlpha = 0;

static LRESULT CALLBACK OsdWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            HBRUSH hBrKey = CreateSolidBrush(OSD_KEY_COLOR);
            FillRect(memDC, &rc, hBrKey);
            DeleteObject(hBrKey);

            RECT rcBox = rc;
            InflateRect(&rcBox, -2, -2);
            HBRUSH hBg = CreateSolidBrush(RGB(24, 26, 32));
            HPEN hBorder = CreatePen(PS_SOLID, 1, RGB(70, 75, 90));
            HGDIOBJ oldBr = SelectObject(memDC, hBg);
            HGDIOBJ oldPen = SelectObject(memDC, hBorder);
            RoundRect(memDC, rcBox.left, rcBox.top, rcBox.right, rcBox.bottom, 22, 22);
            SetBkMode(memDC, TRANSPARENT);

            HFONT hFontBig = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         VARIABLE_PITCH, L"Segoe UI");
            HGDIOBJ oldFont = SelectObject(memDC, hFontBig);
            SetTextColor(memDC, RGB(255, 255, 255));
            RECT rcTop = rcBox;
            rcTop.bottom = rcBox.top + 42;
            DrawTextW(memDC, g_osdTextLine1, -1, &rcTop, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            HFONT hFontSub = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         VARIABLE_PITCH, L"Segoe UI");
            SelectObject(memDC, hFontSub);
            SetTextColor(memDC, RGB(130, 215, 255));
            RECT rcBot = rcBox;
            rcBot.top = rcBox.top + 40;
            DrawTextW(memDC, g_osdTextLine2, -1, &rcBot, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldFont);
            SelectObject(memDC, oldPen);
            SelectObject(memDC, oldBr);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            DeleteObject(hFontBig);
            DeleteObject(hFontSub);
            DeleteObject(hBorder);
            DeleteObject(hBg);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_TIMER: {
            if (wParam == TIMER_OSD_HIDE) {
                KillTimer(hWnd, TIMER_OSD_HIDE);
                SetTimer(hWnd, TIMER_OSD_FADE, 16, NULL);
            } else if (wParam == TIMER_OSD_FADE) {
                if (g_osdAlpha > 15) {
                    g_osdAlpha -= 15;
                    SetLayeredWindowAttributes(hWnd, OSD_KEY_COLOR, g_osdAlpha,
                                               LWA_COLORKEY | LWA_ALPHA);
                } else {
                    KillTimer(hWnd, TIMER_OSD_FADE);
                    ShowWindow(hWnd, SW_HIDE);
                    g_osdAlpha = 0;
                }
            }
            return 0;
        }
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

static void ShowOsdNotification() {
    if (!g_hOsdWnd) return;
    EnterCriticalSection(&g_cs);
    if (g_deviceConnected) {
        StringCchPrintfW(g_osdTextLine1, ARRAYSIZE(g_osdTextLine1),
                         L"电量 %d%%%s", (int)g_battery, g_charging ? L"  ⚡充电中" : L"");
        StringCchPrintfW(g_osdTextLine2, ARRAYSIZE(g_osdTextLine2),
                         L"%s  |  %s  |  %dmV", g_model,
                         vxe::ConnectTypeName((int)g_connectType), (int)g_voltage);
    } else {
        StringCchPrintfW(g_osdTextLine1, ARRAYSIZE(g_osdTextLine1), L"未连接");
        StringCchPrintfW(g_osdTextLine2, ARRAYSIZE(g_osdTextLine2), L"等待鼠标 / 接收器...");
    }
    LeaveCriticalSection(&g_cs);

    RECT rcWork;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0)) {
        rcWork.left = 0;
        rcWork.top = 0;
        rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int w = 300, h = 76;
    int x = rcWork.left + ((rcWork.right - rcWork.left) - w) / 2;
    int y = rcWork.bottom - h - 80;
    SetWindowPos(g_hOsdWnd, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_osdAlpha = 240;
    SetLayeredWindowAttributes(g_hOsdWnd, OSD_KEY_COLOR, g_osdAlpha,
                               LWA_COLORKEY | LWA_ALPHA);
    InvalidateRect(g_hOsdWnd, NULL, FALSE);
    UpdateWindow(g_hOsdWnd);
    KillTimer(g_hOsdWnd, TIMER_OSD_FADE);
    SetTimer(g_hOsdWnd, TIMER_OSD_HIDE, 1600, NULL);
}

// ---- HID 后台线程 (VXE COMPX 协议轮询) ------------------------------------

static void SetModelName(BYTE cid, BYTE mid, const WCHAR* product) {
    WCHAR name[64];
    if (cid == 2 && (mid == 31 || mid == 116)) {
        StringCchCopyW(name, 64, L"VXE R1SE+");
    } else if (cid == 2 && (mid == 29 || mid == 30)) {
        StringCchCopyW(name, 64, L"VXE R1SE");
    } else if (cid == 2 && (mid == 27 || mid == 28)) {
        StringCchCopyW(name, 64, L"VXE R1 PRO MAX");
    } else if (cid == 2 && mid == 63) {
        StringCchCopyW(name, 64, L"VXE R1S+");
    } else if (cid == 2 && mid == 121) {
        StringCchCopyW(name, 64, L"VXE X3");
    } else if (product && product[0]) {
        StringCchCopyW(name, 64, product);
    } else {
        StringCchCopyW(name, 64, L"VXE 无线鼠标");
    }
    EnterCriticalSection(&g_cs);
    StringCchCopyW(g_model, 64, name);
    LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI HidWorkerThread(LPVOID) {
    static vxe::HidEntry list[128];

    while (WaitForSingleObject(g_hStopEvent, 200) == WAIT_TIMEOUT) {
        int n = vxe::EnumDevices(list, 128);
        int idx = vxe::FindCompxDevice(list, n);
        if (idx < 0) {
            if (g_deviceConnected) {
                g_deviceConnected = false;
                InterlockedExchange(&g_battery, -1);
                PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, 0, 0);
            }
            Sleep(1500);
            continue;
        }

        vxe::Transport t;
        if (!vxe::OpenTransport(list[idx], &t)) {
            Sleep(1500);
            continue;
        }

        // 连接建立后查询一次型号 / 连接方式 / 固件版本
        BYTE cid = 0, mid = 0;
        int ct = vxe::QueryConnectType(t, &cid, &mid);
        BYTE cid2 = 0, mid2 = 0;
        if (vxe::QueryCidMid(t, &cid2, &mid2, nullptr)) {
            cid = cid2;
            mid = mid2;
        }
        SetModelName(cid, mid, list[idx].product);
        vxe::QueryVersion(t, g_version, sizeof(g_version));
        InterlockedExchange(&g_connectType, ct >= 0 ? ct : -1);
        g_deviceConnected = true;
        PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, 0, 0);

        int failCount = 0;
        while (WaitForSingleObject(g_hStopEvent, 0) == WAIT_TIMEOUT) {
            bool online = vxe::QueryOnline(t);
            vxe::BatteryInfo b;
            bool hasBatt = vxe::QueryBattery(t, &b);

            if (hasBatt) {
                failCount = 0;
                InterlockedExchange(&g_battery, b.level);
                InterlockedExchange(&g_charging, b.charging ? 1 : 0);
                InterlockedExchange(&g_voltage, b.voltageMv);
                InterlockedExchange(&g_online, online ? 1 : 0);
                if (!g_deviceConnected) g_deviceConnected = true;
                PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, 0, 0);
            } else {
                ++failCount;
                if (failCount >= 3) break; // 设备可能已拔出,回到扫描
            }

            if (WaitForSingleObject(g_hStopEvent, POLL_INTERVAL_MS) != WAIT_TIMEOUT) break;
        }

        t.Close();
        g_deviceConnected = false;
        InterlockedExchange(&g_battery, -1);
        PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, 0, 0);
    }
    return 0;
}

// ---- 菜单 / 自启动 --------------------------------------------------------

static bool IsAutoRunEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR path[MAX_PATH];
        DWORD len = sizeof(path), type = 0;
        LSTATUS st = RegQueryValueExW(hKey, APP_NAME, NULL, &type, (LPBYTE)path, &len);
        RegCloseKey(hKey);
        return (st == ERROR_SUCCESS);
    }
    return false;
}

static void SetAutoRun(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR selfPath[MAX_PATH];
            GetModuleFileNameW(NULL, selfPath, MAX_PATH);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (const BYTE*)selfPath,
                           (DWORD)((wcslen(selfPath) + 1) * sizeof(WCHAR)));
        } else {
            RegDeleteValueW(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

static void ShowContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    WCHAR bufHeader[80], bufBat[64], bufSt[64], bufV[64], bufC[64], bufVer[64];

    EnterCriticalSection(&g_cs);
    if (g_deviceConnected) {
        StringCchPrintfW(bufHeader, 80, L"%s", g_model);
        if (g_battery >= 0)
            StringCchPrintfW(bufBat, 64, L"电池电量: %d%%", (int)g_battery);
        else
            StringCchPrintfW(bufBat, 64, L"电池电量: --");
        StringCchPrintfW(bufSt, 64, L"状态: %s", g_charging ? L"充电中" : L"放电中");
        StringCchPrintfW(bufV, 64, L"电压: %d mV", (int)g_voltage);
        StringCchPrintfW(bufC, 64, L"连接: %s", vxe::ConnectTypeName((int)g_connectType));
        StringCchPrintfW(bufVer, 64, L"固件: %S", g_version[0] ? g_version : "--");
    } else {
        StringCchPrintfW(bufHeader, 80, L"vxe-monitor (未连接)");
        StringCchPrintfW(bufBat, 64, L"电池电量: --");
        StringCchPrintfW(bufSt, 64, L"状态: --");
        StringCchPrintfW(bufV, 64, L"电压: --");
        StringCchPrintfW(bufC, 64, L"连接: --");
        StringCchPrintfW(bufVer, 64, L"固件: --");
    }
    LeaveCriticalSection(&g_cs);

    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_HEADER, bufHeader);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_BATTERY, bufBat);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_STATUS, bufSt);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_VOLTAGE, bufV);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_CONNECT, bufC);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_HEADER, bufVer);
    WCHAR bufAppVer[64];
    StringCchPrintfW(bufAppVer, 64, L"程序版本: %s", APP_VERSION);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_VERSION, bufAppVer);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    UINT autoRunFlags = MF_STRING | (IsAutoRunEnabled() ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, autoRunFlags, IDM_AUTORUN, L"开机自启动");
    AppendMenuW(hMenu, MF_STRING, IDM_RECONNECT, L"立即刷新 / 重新连接");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"退出");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uTaskbarRestartMsg && g_uTaskbarRestartMsg != 0) {
        UpdateTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_TRAY_ICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowContextMenu(hWnd);
            } else if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                ShowOsdNotification();
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_AUTORUN:
                    SetAutoRun(!IsAutoRunEnabled());
                    break;
                case IDM_RECONNECT: {
                    SetEvent(g_hStopEvent);
                    WaitForSingleObject(g_hHidThread, 3000);
                    CloseHandle(g_hHidThread);
                    ResetEvent(g_hStopEvent);
                    g_hHidThread = CreateThread(NULL, 0, HidWorkerThread, NULL, 0, NULL);
                    break;
                }
                case IDM_EXIT:
                    DestroyWindow(hWnd);
                    break;
            }
            return 0;
        case WM_APP_BAT_UPDATE:
            UpdateTrayIcon();
            return 0;
        case WM_SETTINGCHANGE:
            UpdateTrayIcon();
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\vxe-monitorSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }
    InitializeCriticalSection(&g_cs);

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI* pfnSetDpiAware)(DPI_AWARENESS_CONTEXT);
    pfnSetDpiAware fnSetDpiAware =
        (pfnSetDpiAware)GetProcAddress(hUser, "SetProcessDpiAwarenessContext");
    if (fnSetDpiAware) fnSetDpiAware(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_hInstance = hInstance;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"VxeTrayMessageWnd";
    RegisterClassExW(&wc);

    g_hMainWnd = CreateWindowExW(0, wc.lpszClassName, L"VxeMonitor", WS_POPUP, 0, 0, 0, 0,
                                 NULL, NULL, hInstance, NULL);
    g_uTaskbarRestartMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wcOsd = {0};
    wcOsd.cbSize = sizeof(WNDCLASSEXW);
    wcOsd.lpfnWndProc = OsdWndProc;
    wcOsd.hInstance = hInstance;
    wcOsd.lpszClassName = L"VxeOsdPopupWnd";
    wcOsd.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wcOsd);
    g_hOsdWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wcOsd.lpszClassName, L"VxeOSD", WS_POPUP, 0, 0, 300, 76,
                                NULL, NULL, hInstance, NULL);

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hMainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_ICON;
    g_nid.hIcon = CreateBatteryIcon(-1, false);
    UpdateTrayTooltip();
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hHidThread = CreateThread(NULL, 0, HidWorkerThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_hStopEvent);
    WaitForSingleObject(g_hHidThread, 2000);
    CloseHandle(g_hHidThread);
    CloseHandle(g_hStopEvent);
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    if (g_hOsdWnd) DestroyWindow(g_hOsdWnd);
    DeleteCriticalSection(&g_cs);
    CloseHandle(hMutex);
    return (int)msg.wParam;
}
