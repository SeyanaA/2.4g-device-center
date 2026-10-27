#include "tray.h"

#include <shellapi.h>

#include <cmath>
#include <cstring>

namespace {

struct Canvas {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;

    Canvas(int width, int height) : w(width), h(height), px((size_t)width * height, 0) {}

    void Put(int x, int y, uint32_t argb) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        px[(size_t)y * w + x] = argb;
    }

    void FillRect(int x, int y, int ww, int hh, uint32_t argb) {
        for (int yy = y; yy < y + hh; ++yy) {
            for (int xx = x; xx < x + ww; ++xx) Put(xx, yy, argb);
        }
    }

    void FillCircle(int cx, int cy, int r, uint32_t argb) {
        for (int y = -r; y <= r; ++y) {
            for (int x = -r; x <= r; ++x) {
                if (x * x + y * y <= r * r) Put(cx + x, cy + y, argb);
            }
        }
    }

    void DrawArc(double startDeg, double endDeg, double radius, int thickness, uint32_t argb) {
        double cx = w / 2.0;
        double cy = h / 2.0;
        double step = std::max(0.5, 90.0 / std::max(w, h));
        for (double a = startDeg; a <= endDeg; a += step) {
            double rad = a * 3.14159265358979323846 / 180.0;
            int px = (int)std::lround(cx + radius * std::cos(rad));
            int py = (int)std::lround(cy + radius * std::sin(rad));
            int t = std::max(1, thickness);
            for (int dy = -t / 2; dy <= t / 2; ++dy) {
                for (int dx = -t / 2; dx <= t / 2; ++dx) Put(px + dx, py + dy, argb);
            }
        }
    }
};

uint32_t Argb(int a, int r, int g, int b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint32_t LevelColor(int percent, bool stale, bool charging) {
    if (stale || percent < 0) return Argb(255, 156, 163, 175);
    if (charging) return Argb(255, 34, 197, 94);
    if (percent <= 20) return Argb(255, 239, 68, 68);
    if (percent <= 49) return Argb(255, 245, 158, 11);
    return Argb(255, 34, 197, 94);
}

const uint8_t kGlyphs[10][7] = {
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},  // 0
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},  // 1
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},  // 2
    {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110},  // 3
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},  // 4
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110},  // 5
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},  // 6
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},  // 7
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},  // 8
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100},  // 9
};

uint8_t GlyphRow(wchar_t ch, int row) {
    if (ch >= L'0' && ch <= L'9') return kGlyphs[ch - L'0'][row];
    if (ch == L'?') {
        static const uint8_t q[7] = {0b01110, 0b10001, 0b00001, 0b00010,
                                     0b00100, 0b00000, 0b00100};
        return q[row];
    }
    if (ch == L'F') {
        static const uint8_t f[7] = {0b11111, 0b10000, 0b10000, 0b11110,
                                     0b10000, 0b10000, 0b10000};
        return f[row];
    }
    if (ch == L'-') {
        static const uint8_t d[7] = {0, 0, 0, 0b11111, 0, 0, 0};
        return d[row];
    }
    return 0;
}

int TextWidth(const std::wstring& text, int scale) {
    if (text.empty()) return 0;
    return (int)text.size() * 5 * scale + ((int)text.size() - 1) * scale;
}

void DrawTextPixels(Canvas& c, const std::wstring& text, int x, int y, int scale, uint32_t color,
                    uint32_t outline) {
    auto draw = [&](int ox, int oy, uint32_t col) {
        int cx = x + ox;
        for (wchar_t ch : text) {
            for (int row = 0; row < 7; ++row) {
                uint8_t bits = GlyphRow(ch, row);
                for (int colIdx = 0; colIdx < 5; ++colIdx) {
                    if (bits & (1 << (4 - colIdx))) {
                        c.FillRect(cx + colIdx * scale, y + oy + row * scale, scale, scale, col);
                    }
                }
            }
            cx += 6 * scale;
        }
    };
    if (outline) {
        int off = std::max(1, scale / 2);
        draw(-off, 0, outline);
        draw(off, 0, outline);
        draw(0, -off, outline);
        draw(0, off, outline);
    }
    draw(0, 0, color);
}

}  // namespace

HICON CreateBatteryIcon(int percent, bool charging, bool stale, int sizePx) {
    int size = ClampInt(sizePx, 16, 64);
    const int SS = 4;  // 4x supersampling for anti-aliased tray icons
    int S = size * SS;
    Canvas c(S, S);
    int cx = S / 2;
    int cy = S / 2;
    double outer = S / 2.0 - SS;
    double thicknessF = std::max(1.2, size / 14.0) * SS;
    int thickness = std::max(1, (int)std::lround(thicknessF));
    double pathRadius = outer - thickness / 2.0;
    uint32_t track = Argb(235, 120, 132, 148);
    uint32_t fill = LevelColor(percent, stale, charging);

    c.DrawArc(-90.0, 270.0, pathRadius, thickness, track);
    int pct = stale ? -1 : ClampInt(percent, 0, 100);
    if (pct > 0) {
        c.DrawArc(-90.0, -90.0 + 3.6 * pct, pathRadius, thickness, fill);
    } else if (charging) {
        c.DrawArc(-90.0, -86.0, pathRadius, thickness, fill);
    }

    int inner = (int)std::floor(pathRadius - thickness / 2.0);
    if (inner >= 2 * SS) {
        // White face with dark digits reads better than white-on-dark at 16 px,
        // on both light and dark taskbars.
        c.FillCircle(cx, cy, std::max(1, inner), Argb(255, 255, 255, 255));
    }

    std::wstring text;
    if (stale) {
        text = L"?";
    } else if (percent < 0) {
        text = L"--";
    } else if (pct >= 100 && size < 48) {
        text = L"F";
    } else {
        text = std::to_wstring(pct);
    }

    // Downsample the ring/disc first, then draw the pixel digits at native
    // resolution: smooth circle, crisp readable numbers.
    std::vector<uint32_t> out((size_t)size * size, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint64_t sa = 0, sr = 0, sg = 0, sb = 0;
            for (int dy = 0; dy < SS; ++dy) {
                for (int dx = 0; dx < SS; ++dx) {
                    uint32_t p = c.px[(size_t)(y * SS + dy) * S + (x * SS + dx)];
                    uint32_t a = p >> 24;
                    if (!a) continue;
                    sa += a;
                    sr += ((p >> 16) & 0xFF) * a;
                    sg += ((p >> 8) & 0xFF) * a;
                    sb += (p & 0xFF) * a;
                }
            }
            if (sa) {
                uint32_t a = (uint32_t)(sa / (SS * SS));
                uint32_t r = (uint32_t)(sr / sa);
                uint32_t g = (uint32_t)(sg / sa);
                uint32_t b = (uint32_t)(sb / sa);
                out[(size_t)y * size + x] = Argb((int)a, (int)r, (int)g, (int)b);
            }
        }
    }

    Canvas face(size, size);
    face.px = out;
    int textScale = size >= 28 ? 2 : 1;
    int tw = TextWidth(text, textScale);
    int faceDiameter = (int)(2.0 * inner / SS);
    if (textScale > 1 && tw + 2 > faceDiameter) textScale = 1;
    tw = TextWidth(text, textScale);
    int th = 7 * textScale;
    uint32_t textColor = stale ? Argb(255, 107, 114, 128) : Argb(255, 17, 24, 39);
    DrawTextPixels(face, text, (size - tw) / 2, (size - th) / 2 + 1, textScale,
                   textColor, 0);

    // Convert to a 32bpp DIB and then an icon.
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return nullptr;
    memcpy(bits, face.px.data(), face.px.size() * sizeof(uint32_t));
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

bool TrayIcon::Add(HWND hwnd, UINT callbackMessage, HICON icon, const std::wstring& tip) {
    hwnd_ = hwnd;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = id_;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = callbackMessage;
    nid.hIcon = icon;
    wcsncpy(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip) - 1);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (added_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    return added_;
}

bool TrayIcon::Update(HICON icon, const std::wstring& tip) {
    if (!added_) return false;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = id_;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = icon;
    wcsncpy(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip) - 1);
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

bool TrayIcon::Balloon(const std::wstring& title, const std::wstring& text, DWORD flags) {
    if (!added_) return false;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = id_;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = flags;
    wcsncpy(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle) - 1);
    wcsncpy(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo) - 1);
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

void TrayIcon::Remove() {
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = id_;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}
