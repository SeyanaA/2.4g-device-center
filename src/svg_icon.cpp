// SVG 图标 / 矢量绘制层实现。
//
// 设计要点（见 docs/SPEC-vector-icons.md）：
// - 图标唯一数据源是 24x24 viewBox 的 SVG path `d` 字符串，运行时翻译成
//   Gdiplus::GraphicsPath，再等比缩放居中到目标矩形。
// - 不引入第三方 SVG 库，不新增位图资源；只实现图标需要的 path 子集。
// - 每次公开绘制调用都在函数作用域内创建 Gdiplus::Graphics，函数返回前析构，
//   保证与同一 DC 上的 GDI 调用保持先后顺序（tray.cpp 也在复用本模块）。
// - GDI+ 启动在本文件内懒加载，调用方（包括托盘）不需要自己 Startup。

#include "svg_icon.h"

#include <objidl.h>
#include <gdiplus.h>

#include <cmath>
#include <cstddef>
#include <new>
#include <optional>

namespace svgicon {
namespace {

// 整帧共享的 GDI+ 上下文（仅 UI 线程使用；BeginFrame/EndFrame 成对调用）。
thread_local HDC g_frameDc = nullptr;
thread_local Gdiplus::Graphics* g_frameG = nullptr;

// 取当前 DC 可用的 Graphics：帧内复用同一个对象，帧外临时创建一个。
// 析构时把抗锯齿状态恢复，避免污染后续 GDI 绘制。
class GraphicsScope {
public:
    explicit GraphicsScope(HDC dc) {
        if (g_frameDc == dc && g_frameG != nullptr) {
            g_ = g_frameG;
        } else {
            owned_.emplace(dc);
            g_ = &*owned_;
        }
        g_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    }
    ~GraphicsScope() {
        if (g_ != nullptr) {
            g_->SetSmoothingMode(Gdiplus::SmoothingModeNone);
            g_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);
        }
    }
    GraphicsScope(const GraphicsScope&) = delete;
    GraphicsScope& operator=(const GraphicsScope&) = delete;
    Gdiplus::Graphics& get() { return *g_; }

private:
    std::optional<Gdiplus::Graphics> owned_;
    Gdiplus::Graphics* g_ = nullptr;
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kViewBox = 24.0;

// 解析预算：防止恶意/损坏数据让解析器在超长字符串上反复空转。
constexpr int kMaxCommands = 4096;
constexpr int kMaxArcSegments = 64;
constexpr int kMaxSteps = 200000;

// 坐标上限：正常图标都在 24 单位内，超过这个量级视为数据损坏，
// 拒绝该数字，避免 float 溢出或 GDI+ 内部做无意义的巨大几何运算。
constexpr double kMaxCoordinate = 1.0e7;

struct Vec {
    double x = 0.0;
    double y = 0.0;
};

bool Finite(double v) { return std::isfinite(v) && std::fabs(v) <= kMaxCoordinate; }

bool FiniteVec(const Vec& v) { return Finite(v.x) && Finite(v.y); }

Gdiplus::REAL R(double v) { return static_cast<Gdiplus::REAL>(v); }

// GDI+ 只在本进程启动一次。故意不调用 GdiplusShutdown：
// 模块与 ui.cpp / tray.cpp 共享进程级 GDI+ 生命周期，提前关闭会破坏其他调用方。
bool EnsureGdiplus() {
    static const bool ok = [] {
        static ULONG_PTR token = 0;
        Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ok;
}

Gdiplus::Color ToColor(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

// ---------------------------------------------------------------------------
// SVG path 解析器
// ---------------------------------------------------------------------------

struct ParseState {
    Gdiplus::GraphicsPath* path = nullptr;
    Vec cur;          // 当前点
    Vec subpathStart; // 当前子路径起点（Z 之后回到这里）
    bool haveSubpath = false;
    bool figureHasGeometry = false;
    // GDI+ 没有 MoveTo：只有真正产生线段/曲线时才 StartFigure，
    // 否则每个 M 都会生成一个零长度线段（圆头画笔会在该点画出一个圆点）。
    bool needStartFigure = true;
    bool haveCubicCtrl = false;
    Vec cubicCtrl;
    bool haveQuadCtrl = false;
    Vec quadCtrl;
    int anchors = 0;  // 逻辑锚点数量：move 点 + 线段端点 + 曲线控制点/端点
};

void EnsureFigure(ParseState& st) {
    if (st.needStartFigure) {
        st.path->StartFigure();
        st.needStartFigure = false;
    }
}

void MoveTo(ParseState& st, double x, double y) {
    st.cur = Vec{x, y};
    st.subpathStart = st.cur;
    st.haveSubpath = true;
    st.figureHasGeometry = false;
    st.needStartFigure = true;
    st.haveCubicCtrl = false;
    st.haveQuadCtrl = false;
    ++st.anchors;
}

bool AddLineTo(ParseState& st, double x, double y) {
    if (!Finite(x) || !Finite(y)) return false;
    EnsureFigure(st);
    st.path->AddLine(R(st.cur.x), R(st.cur.y), R(x), R(y));
    st.cur = Vec{x, y};
    st.figureHasGeometry = true;
    st.haveCubicCtrl = false;
    st.haveQuadCtrl = false;
    ++st.anchors;
    return true;
}

bool AddCubicTo(ParseState& st, const Vec& c1, const Vec& c2, const Vec& end) {
    if (!FiniteVec(c1) || !FiniteVec(c2) || !FiniteVec(end)) return false;
    EnsureFigure(st);
    st.path->AddBezier(R(st.cur.x), R(st.cur.y), R(c1.x), R(c1.y), R(c2.x), R(c2.y),
                       R(end.x), R(end.y));
    st.cur = end;
    st.figureHasGeometry = true;
    st.anchors += 3;  // 两个控制点 + 一个端点
    return true;
}

void CloseSubpath(ParseState& st) {
    if (!st.haveSubpath) return;
    if (st.figureHasGeometry) {
        st.path->CloseFigure();
    }
    // SVG：Z 之后当前点回到子路径起点；之后的绘制从此处继续。
    st.cur = st.subpathStart;
    st.figureHasGeometry = false;
    st.needStartFigure = true;
    st.haveCubicCtrl = false;
    st.haveQuadCtrl = false;
}

bool IsAsciiLetter(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

bool IsSupportedCommand(wchar_t c) {
    switch (c) {
        case L'M':
        case L'm':
        case L'L':
        case L'l':
        case L'H':
        case L'h':
        case L'V':
        case L'v':
        case L'C':
        case L'c':
        case L'S':
        case L's':
        case L'Q':
        case L'q':
        case L'T':
        case L't':
        case L'A':
        case L'a':
        case L'Z':
        case L'z':
            return true;
        default:
            return false;
    }
}

class Cursor {
public:
    explicit Cursor(const wchar_t* text) : p_(text) {}

    bool End() const { return p_ == nullptr || *p_ == L'\0'; }

    wchar_t Peek() const { return End() ? L'\0' : *p_; }

    void Advance() {
        if (!End()) ++p_;
    }

    // SVG 分隔符：空白与逗号。宽松接受连续逗号，便于容错。
    void SkipSeparators() {
        while (!End()) {
            const wchar_t c = *p_;
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' ||
                c == L'\v' || c == L',') {
                ++p_;
            } else {
                break;
            }
        }
    }

    // 手工解析十进制数字，避免 wcstod 受 locale 影响，也避免其接受 "nan"/"inf"。
    // 失败时游标不前进，由调用方统一 resync。
    bool ReadNumber(double& out) {
        SkipSeparators();
        if (End()) return false;

        const wchar_t* s = p_;
        bool negative = false;
        if (*s == L'+' || *s == L'-') {
            negative = (*s == L'-');
            ++s;
        }

        bool hasDigits = false;
        double value = 0.0;
        while (*s >= L'0' && *s <= L'9') {
            // 长数字串先饱和，避免整数部分无限增长；指数与最终范围再做检查。
            if (value < 1.0e18) {
                value = value * 10.0 + static_cast<double>(*s - L'0');
            }
            hasDigits = true;
            ++s;
        }
        if (*s == L'.') {
            ++s;
            double place = 0.1;
            while (*s >= L'0' && *s <= L'9') {
                value += static_cast<double>(*s - L'0') * place;
                place *= 0.1;
                hasDigits = true;
                ++s;
            }
        }
        if (!hasDigits) return false;

        if (*s == L'e' || *s == L'E') {
            const wchar_t* e = s + 1;
            bool expNegative = false;
            if (*e == L'+' || *e == L'-') {
                expNegative = (*e == L'-');
                ++e;
            }
            bool hasExpDigits = false;
            int exp = 0;
            while (*e >= L'0' && *e <= L'9') {
                if (exp < 10000) exp = exp * 10 + static_cast<int>(*e - L'0');
                hasExpDigits = true;
                ++e;
            }
            if (hasExpDigits) {
                // 1e999 之类指数超出 double 范围：直接判为非法数字，不产生 Inf。
                if (exp > 400) return false;
                s = e;
                value *= std::pow(10.0, expNegative ? -exp : exp);
            }
        }

        if (negative) value = -value;
        if (!Finite(value)) return false;

        p_ = s;
        out = value;
        return true;
    }

    // SVG 的 large-arc/sweep 旗标是单字符 0/1，允许紧凑写法 "011"。
    bool ReadFlag(bool& out) {
        SkipSeparators();
        if (End()) return false;
        if (*p_ == L'0' || *p_ == L'1') {
            out = (*p_ == L'1');
            ++p_;
            return true;
        }
        // 兼容 "0.0" 之类的非紧凑写法。
        double number = 0.0;
        if (!ReadNumber(number)) return false;
        out = number != 0.0;
        return true;
    }

    // 出错后跳到下一个指令字母，保证主循环继续前进，不会死循环。
    void SkipToNextCommand() {
        while (!End() && !IsAsciiLetter(*p_)) ++p_;
    }

private:
    const wchar_t* p_ = nullptr;
};

bool ReadPoint(Cursor& c, Vec& p) {
    double x = 0.0;
    double y = 0.0;
    if (!c.ReadNumber(x) || !c.ReadNumber(y)) return false;
    p = Vec{x, y};
    return true;
}

Vec Reflect(const Vec& control, const Vec& cur) {
    return Vec{2.0 * cur.x - control.x, 2.0 * cur.y - control.y};
}

// 二次贝塞尔转三次（GDI+ GraphicsPath 没有 AddBezier 的二次形式）。
void QuadraticToCubic(const Vec& p0, const Vec& q, const Vec& p1, Vec& c1, Vec& c2) {
    constexpr double k = 2.0 / 3.0;
    c1 = Vec{p0.x + (q.x - p0.x) * k, p0.y + (q.y - p0.y) * k};
    c2 = Vec{p1.x + (q.x - p1.x) * k, p1.y + (q.y - p1.y) * k};
}

// SVG 椭圆弧：端点参数化 -> 圆心参数化（SVG 规范附录 F.6.5），
// 再切成不超过 90 度的分段，每段用三次贝塞尔逼近（支持 x-axis-rotation）。
bool AppendArc(ParseState& st, double rxIn, double ryIn, double rotationDeg, bool largeArc,
               bool sweep, double endX, double endY) {
    const Vec p0 = st.cur;
    const Vec p1{endX, endY};
    if (!FiniteVec(p1)) return false;

    // A 也是“普通指令”：之后的 S/T 不再反射更早的 C/Q 控制点。
    st.haveCubicCtrl = false;
    st.haveQuadCtrl = false;

    // 起终点重合：按规范不绘制任何内容。
    const double dxRaw = p0.x - p1.x;
    const double dyRaw = p0.y - p1.y;
    if (std::fabs(dxRaw) < 1e-12 && std::fabs(dyRaw) < 1e-12) return true;

    double rx = std::fabs(rxIn);
    double ry = std::fabs(ryIn);
    if (!std::isfinite(rx) || !std::isfinite(ry) || rx < 1e-9 || ry < 1e-9) {
        // 半径为 0（或非法）：退化为直线。
        return AddLineTo(st, p1.x, p1.y);
    }

    const double phi = rotationDeg * kPi / 180.0;
    const double cosPhi = std::cos(phi);
    const double sinPhi = std::sin(phi);

    const double dx = dxRaw * 0.5;
    const double dy = dyRaw * 0.5;
    const double x1p = cosPhi * dx + sinPhi * dy;
    const double y1p = -sinPhi * dx + cosPhi * dy;

    // 半径不足时按规范等比放大。
    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const double grow = std::sqrt(lambda);
        rx *= grow;
        ry *= grow;
    }

    const double rx2 = rx * rx;
    const double ry2 = ry * ry;
    const double num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    const double den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    double coef = 0.0;
    if (den > 0.0) {
        coef = std::sqrt(num > 0.0 ? num / den : 0.0);
        // 大弧/扫描方向决定圆心在弦的哪一侧。
        if (largeArc == sweep) coef = -coef;
    }

    const double cxp = coef * rx * y1p / ry;
    const double cyp = -coef * ry * x1p / rx;
    const Vec center{cosPhi * cxp - sinPhi * cyp + (p0.x + p1.x) * 0.5,
                     sinPhi * cxp + cosPhi * cyp + (p0.y + p1.y) * 0.5};

    const double ux = (x1p - cxp) / rx;
    const double uy = (y1p - cyp) / ry;
    const double vx = (-x1p - cxp) / rx;
    const double vy = (-y1p - cyp) / ry;

    const double theta1 = std::atan2(uy, ux);
    double dtheta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && dtheta > 0.0) dtheta -= 2.0 * kPi;
    if (sweep && dtheta < 0.0) dtheta += 2.0 * kPi;

    int segments = static_cast<int>(std::ceil(std::fabs(dtheta) / (kPi * 0.5)));
    if (segments < 1) segments = 1;
    if (segments > kMaxArcSegments) segments = kMaxArcSegments;
    const double delta = dtheta / static_cast<double>(segments);
    // 单段三次贝塞尔的标准圆心角逼近系数。
    const double handle = (4.0 / 3.0) * std::tan(delta / 4.0);

    auto toWorld = [&](const Vec& local) {
        return Vec{cosPhi * local.x - sinPhi * local.y + center.x,
                   sinPhi * local.x + cosPhi * local.y + center.y};
    };

    for (int i = 0; i < segments; ++i) {
        const double a1 = theta1 + delta * static_cast<double>(i);
        const double a2 = a1 + delta;
        const Vec q0{rx * std::cos(a1), ry * std::sin(a1)};
        const Vec q3{rx * std::cos(a2), ry * std::sin(a2)};
        const Vec d0{-rx * std::sin(a1), ry * std::cos(a1)};
        const Vec d1{-rx * std::sin(a2), ry * std::cos(a2)};
        const Vec lc1{q0.x + handle * d0.x, q0.y + handle * d0.y};
        const Vec lc2{q3.x - handle * d1.x, q3.y - handle * d1.y};

        const Vec c1 = toWorld(lc1);
        const Vec c2 = toWorld(lc2);
        const Vec end = toWorld(q3);
        if (!FiniteVec(c1) || !FiniteVec(c2) || !FiniteVec(end)) break;  // 丢弃异常弧段
        if (!AddCubicTo(st, c1, c2, end)) break;
    }
    // 贝塞尔逼近的终点与理论端点只差浮点误差；当前点直接对齐到规范端点，
    // 保证后续相对指令的基准准确。
    st.cur = p1;
    return true;
}

// 执行一条指令（只在成功时改变 path/current point）。
bool AppendCommand(wchar_t command, Cursor& c, ParseState& st) {
    const bool relative = (command >= L'a' && command <= L'z');
    const wchar_t upper = relative ? static_cast<wchar_t>(command - L'a' + L'A') : command;

    switch (upper) {
        case L'M': {
            Vec p;
            if (!ReadPoint(c, p)) return false;
            const Vec abs = relative ? Vec{st.cur.x + p.x, st.cur.y + p.y} : p;
            if (!FiniteVec(abs)) return false;
            MoveTo(st, abs.x, abs.y);
            return true;
        }
        case L'L': {
            Vec p;
            if (!ReadPoint(c, p)) return false;
            const Vec abs = relative ? Vec{st.cur.x + p.x, st.cur.y + p.y} : p;
            return AddLineTo(st, abs.x, abs.y);
        }
        case L'H': {
            double x = 0.0;
            if (!c.ReadNumber(x)) return false;
            const double absX = relative ? st.cur.x + x : x;
            return AddLineTo(st, absX, st.cur.y);
        }
        case L'V': {
            double y = 0.0;
            if (!c.ReadNumber(y)) return false;
            const double absY = relative ? st.cur.y + y : y;
            return AddLineTo(st, st.cur.x, absY);
        }
        case L'C': {
            Vec c1, c2, end;
            if (!ReadPoint(c, c1) || !ReadPoint(c, c2) || !ReadPoint(c, end)) return false;
            if (relative) {
                c1 = Vec{st.cur.x + c1.x, st.cur.y + c1.y};
                c2 = Vec{st.cur.x + c2.x, st.cur.y + c2.y};
                end = Vec{st.cur.x + end.x, st.cur.y + end.y};
            }
            if (!AddCubicTo(st, c1, c2, end)) return false;
            st.cubicCtrl = c2;
            st.haveCubicCtrl = true;
            st.haveQuadCtrl = false;
            return true;
        }
        case L'S': {
            Vec c2, end;
            if (!ReadPoint(c, c2) || !ReadPoint(c, end)) return false;
            if (relative) {
                c2 = Vec{st.cur.x + c2.x, st.cur.y + c2.y};
                end = Vec{st.cur.x + end.x, st.cur.y + end.y};
            }
            const Vec c1 = st.haveCubicCtrl ? Reflect(st.cubicCtrl, st.cur) : st.cur;
            if (!AddCubicTo(st, c1, c2, end)) return false;
            st.cubicCtrl = c2;
            st.haveCubicCtrl = true;
            st.haveQuadCtrl = false;
            return true;
        }
        case L'Q': {
            Vec q, end;
            if (!ReadPoint(c, q) || !ReadPoint(c, end)) return false;
            if (relative) {
                q = Vec{st.cur.x + q.x, st.cur.y + q.y};
                end = Vec{st.cur.x + end.x, st.cur.y + end.y};
            }
            Vec c1, c2;
            QuadraticToCubic(st.cur, q, end, c1, c2);
            if (!AddCubicTo(st, c1, c2, end)) return false;
            st.quadCtrl = q;
            st.haveQuadCtrl = true;
            st.haveCubicCtrl = false;
            return true;
        }
        case L'T': {
            Vec end;
            if (!ReadPoint(c, end)) return false;
            if (relative) end = Vec{st.cur.x + end.x, st.cur.y + end.y};
            const Vec q = st.haveQuadCtrl ? Reflect(st.quadCtrl, st.cur) : st.cur;
            Vec c1, c2;
            QuadraticToCubic(st.cur, q, end, c1, c2);
            if (!AddCubicTo(st, c1, c2, end)) return false;
            st.quadCtrl = q;
            st.haveQuadCtrl = true;
            st.haveCubicCtrl = false;
            return true;
        }
        case L'A': {
            double rx = 0.0;
            double ry = 0.0;
            double rotation = 0.0;
            bool largeArc = false;
            bool sweep = false;
            Vec end;
            if (!c.ReadNumber(rx) || !c.ReadNumber(ry) || !c.ReadNumber(rotation) ||
                !c.ReadFlag(largeArc) || !c.ReadFlag(sweep)) {
                return false;
            }
            if (!ReadPoint(c, end)) return false;
            if (relative) end = Vec{st.cur.x + end.x, st.cur.y + end.y};
            return AppendArc(st, rx, ry, rotation, largeArc, sweep, end.x, end.y);
        }
        default:
            return false;
    }
}

// 解析整段 d 数据，返回逻辑锚点数（0 表示没有可用几何）。
int ParsePath(const wchar_t* d, Gdiplus::GraphicsPath& path) {
    if (d == nullptr || *d == L'\0') return 0;

    ParseState st;
    st.path = &path;
    Cursor c(d);

    wchar_t command = 0;  // 0 = 当前没有待重复的指令
    int commands = 0;
    int steps = 0;

    while (!c.End()) {
        if (++steps > kMaxSteps) break;
        c.SkipSeparators();
        if (c.End()) break;

        const wchar_t ch = c.Peek();
        if (IsAsciiLetter(ch)) {
            c.Advance();
            if (ch == L'Z' || ch == L'z') {
                CloseSubpath(st);
                command = 0;
            } else if (IsSupportedCommand(ch)) {
                command = ch;  // 随后的数字按该指令（以及隐式重复）解析
            } else {
                // 未知指令：跳过字母，参数按“散落数字”逐个跳过；
                // 不中断后续合法指令。
                command = 0;
            }
            continue;
        }

        if (command == 0) {
            // 没有指令可重复时的散落数字/垃圾字符：逐个跳过。
            c.Advance();
            continue;
        }
        if (commands >= kMaxCommands) break;
        ++commands;

        const bool ok = AppendCommand(command, c, st);
        // SVG 规则：M 之后多出来的坐标对隐含为 L（m 则为 l）。
        if (command == L'M') {
            command = L'L';
        } else if (command == L'm') {
            command = L'l';
        }
        if (!ok) {
            c.SkipToNextCommand();
            command = 0;
        }
    }

    return st.anchors;
}

// ---------------------------------------------------------------------------
// 图标表（24x24 viewBox）。全部为描边线图标；Bolt 自带闭合轮廓，
// 依赖调用方传 filled=true 填充（默认线宽 1.2，保证只描边时仍然成立）。
// ---------------------------------------------------------------------------

struct IconEntry {
    Id id;
    const wchar_t* d;
};

constexpr IconEntry kIconTable[] = {
    // 导航：设备 / 列表：同一个鼠标轮廓，20px 下只保留外轮廓 + 按键分界。
    {Id::NavDevice,
     L"M12 3.2 C8.6 3.2 6.8 5.9 6.8 9.4 V14.8 C6.8 18.8 9 21 12 21 "
     L"C15 21 17.2 18.8 17.2 14.8 V9.4 C17.2 5.9 15.4 3.2 12 3.2 Z "
     L"M7 9.6 H17"},
    {Id::Mouse,
     L"M12 3.2 C8.6 3.2 6.8 5.9 6.8 9.4 V14.8 C6.8 18.8 9 21 12 21 "
     L"C15 21 17.2 18.8 17.2 14.8 V9.4 C17.2 5.9 15.4 3.2 12 3.2 Z "
     L"M7 9.6 H17"},
    // 导航：日志 —— 三行文字 + 行首圆点（圆点用四段三次曲线，避免小尺寸下变形）。
    {Id::NavLog,
     L"M4.6 5.45 C5.235 5.45 5.75 5.965 5.75 6.6 C5.75 7.235 5.235 7.75 4.6 7.75 "
     L"C3.965 7.75 3.45 7.235 3.45 6.6 C3.45 5.965 3.965 5.45 4.6 5.45 Z "
     L"M4.6 10.85 C5.235 10.85 5.75 11.365 5.75 12 C5.75 12.635 5.235 13.15 4.6 13.15 "
     L"C3.965 13.15 3.45 12.635 3.45 12 C3.45 11.365 3.965 10.85 4.6 10.85 Z "
     L"M4.6 16.25 C5.235 16.25 5.75 16.765 5.75 17.4 C5.75 18.035 5.235 18.55 4.6 18.55 "
     L"C3.965 18.55 3.45 18.035 3.45 17.4 C3.45 16.765 3.965 16.25 4.6 16.25 Z "
     L"M8.2 6.6 H19.5 M8.2 12 H19.5 M8.2 17.4 H19.5"},
    // 导航：测试 —— 靶心（圆）+ 四向刻度。
    {Id::NavTest,
     L"M12 6 C15.314 6 18 8.686 18 12 C18 15.314 15.314 18 12 18 "
     L"C8.686 18 6 15.314 6 12 C6 8.686 8.686 6 12 6 Z "
     L"M12 2.5 V4.6 M12 19.4 V21.5 M2.5 12 H4.6 M19.4 12 H21.5"},
    // 键盘：圆角机身 + 三个键位 + 空格条。
    {Id::Keyboard,
     L"M4.7 6.5 H19.3 C20.515 6.5 21.5 7.485 21.5 8.7 V15.3 "
     L"C21.5 16.515 20.515 17.5 19.3 17.5 H4.7 C3.485 17.5 2.5 16.515 2.5 15.3 V8.7 "
     L"C2.5 7.485 3.485 6.5 4.7 6.5 Z "
     L"M6.8 9.9 H9 M10.9 9.9 H13.1 M15 9.9 H17.2 M8.2 13.9 H15.8"},
    // 耳机：头梁弧 + 两个耳罩。
    {Id::Headset,
     L"M6.2 12.8 C6.2 7.4 8.7 4.2 12 4.2 C15.3 4.2 17.8 7.4 17.8 12.8 "
     L"M5.95 12.8 H6.45 C7.22 12.8 7.85 13.43 7.85 14.2 V18.4 "
     L"C7.85 19.17 7.22 19.8 6.45 19.8 H5.95 C5.18 19.8 4.55 19.17 4.55 18.4 V14.2 "
     L"C4.55 13.43 5.18 12.8 5.95 12.8 Z "
     L"M17.55 12.8 H18.05 C18.82 12.8 19.45 13.43 19.45 14.2 V18.4 "
     L"C19.45 19.17 18.82 19.8 18.05 19.8 H17.55 C16.78 19.8 16.15 19.17 16.15 18.4 V14.2 "
     L"C16.15 13.43 16.78 12.8 17.55 12.8 Z"},
    // 通用 HID：芯片外形 + 四根引脚。
    {Id::Other,
     L"M8.4 6 H15.6 C16.925 6 18 7.075 18 8.4 V15.6 C18 16.925 16.925 18 15.6 18 H8.4 "
     L"C7.075 18 6 16.925 6 15.6 V8.4 C6 7.075 7.075 6 8.4 6 Z "
     L"M12 3 V6 M12 18 V21 M3 12 H6 M18 12 H21"},
    // 充电闪电：闭合多边形。调用方通常传 filled=true；若只描边，
    // 默认 1.2 单位线宽仍然能看出闪电轮廓。
    {Id::Bolt, L"M13.2 2.5 L5.2 14.2 H11.3 L10.5 21.5 L18.8 9.8 H12.4 Z"},
    // 蓝牙 B 字纹（一条竖线 + 两组斜线）。
    {Id::Bluetooth, L"M6.5 6.5 L17.5 17.5 L12 21.5 V2.5 L17.5 6.5 L6.5 17.5"},
    // USB 三叉戟：箭头、圆点、方块、底座圆环。
    {Id::Usb,
     L"M12 18.9 V4.8 M9.7 7.1 L12 4.8 L14.3 7.1 M12 14 L6.8 11.05 "
     L"M6.8 8.55 C7.49 8.55 8.05 9.11 8.05 9.8 C8.05 10.49 7.49 11.05 6.8 11.05 "
     L"C6.11 11.05 5.55 10.49 5.55 9.8 C5.55 9.11 6.11 8.55 6.8 8.55 Z "
     L"M12 10.6 L16.7 6.8 M15.6 4.6 H17.8 V6.8 H15.6 Z "
     L"M12 18.9 C12.828 18.9 13.5 19.572 13.5 20.4 C13.5 21.228 12.828 21.9 12 21.9 "
     L"C11.172 21.9 10.5 21.228 10.5 20.4 C10.5 19.572 11.172 18.9 12 18.9 Z"},
    // 有线连接：两个 C 形环 + 中间连接条。
    {Id::Link,
     L"M9.5 7.5 H8.2 C5.715 7.5 4 9.515 4 12 C4 14.485 5.715 16.5 8.2 16.5 H9.5 "
     L"M14.5 7.5 H15.8 C18.285 7.5 20 9.515 20 12 C20 14.485 18.285 16.5 15.8 16.5 H14.5 "
     L"M9.5 12 H14.5"},
};

const wchar_t* IconPathForId(Id id) {
    for (const IconEntry& entry : kIconTable) {
        if (entry.id == id) return entry.d;
    }
    return nullptr;
}

// 统一的绘制入口：already-resolved strokeUnits（viewBox 单位）。
// solidFill=true 时按不透明实心填充（Bolt 这类闭合填充图标），否则 filled 只加
// 12% 底色（选中态）。
void DrawPathOnDc(HDC dc, const wchar_t* d, const RECT& box, COLORREF color, float strokeUnits,
                  bool filled, bool solidFill) {
    if (dc == nullptr || d == nullptr || *d == L'\0') return;
    if (!EnsureGdiplus()) return;

    const double boxW = static_cast<double>(box.right) - static_cast<double>(box.left);
    const double boxH = static_cast<double>(box.bottom) - static_cast<double>(box.top);
    if (!(boxW > 0.0) || !(boxH > 0.0)) return;

    const double scale = (boxW < boxH ? boxW : boxH) / kViewBox;
    // 上限保护：正常 UI 不会把 24 单位的图标放大到几万像素；
    // 超限时直接放弃，避免坐标乘以超大 scale 后溢出 float。
    if (!(scale > 0.0) || !std::isfinite(scale) || scale > 1000.0) return;

    Gdiplus::GraphicsPath path;
    if (ParsePath(d, path) <= 0 || path.GetPointCount() <= 0) return;

    const float offsetX =
        static_cast<float>(box.left + (boxW - kViewBox * scale) * 0.5);
    const float offsetY =
        static_cast<float>(box.top + (boxH - kViewBox * scale) * 0.5);

    float units = strokeUnits;
    if (!(units > 0.0f) || !std::isfinite(units)) units = 1.8f;

    // 先把路径变换到设备坐标，再显式用 strokeUnits * scale 作为画笔宽度，
    // 不依赖 GDI+ 是否按 world transform 缩放画笔。
    Gdiplus::Matrix transform(static_cast<Gdiplus::REAL>(scale), 0.0f, 0.0f,
                              static_cast<Gdiplus::REAL>(scale), offsetX, offsetY);
    path.Transform(&transform);

    GraphicsScope scope(dc);
    Gdiplus::Graphics& g = scope.get();

    if (filled) {
        // 选中态：12% 透明度的柔和底色（30/255）。
        // 实心图标（Bolt）改成不透明填充，否则闪电会变成一层淡影。
        Gdiplus::SolidBrush soft(ToColor(color, solidFill ? 255 : 30));
        g.FillPath(&soft, &path);
    }

    // 实心图标只填充：再描一圈同色边会让轮廓变粗、失去原本的闪电比例。
    if (solidFill && filled) return;

    Gdiplus::Pen pen(ToColor(color), units * static_cast<float>(scale));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&pen, &path);
}

// 圆角矩形路径：四段 90 度圆弧 + 闭合，和 ui.cpp 既有实现保持一致的几何。
void AddRoundRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h,
                      float radius) {
    const float d = radius * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

float DefaultStrokeUnits(Id id) {
    switch (id) {
        case Id::Bolt:
            return 1.2f;  // 填充图标；只描边时的后备线宽
        case Id::NavDevice:
        case Id::NavLog:
        case Id::NavTest:
            // 导航图标在 20px 下需要比列表图形更实一点，1.8 会显得比原 GDI 2px
            // 明显偏细，与「设备/日志/测试」三个字的字重不匹配。
            return 2.0f;
        default:
            return 1.8f;
    }
}

void Draw(HDC dc, Id id, const RECT& box, COLORREF color, float strokeUnits, bool filled) {
    if (!EnsureGdiplus()) return;
    const wchar_t* d = IconPathForId(id);
    if (d == nullptr) return;
    const float units = strokeUnits > 0.0f ? strokeUnits : DefaultStrokeUnits(id);
    DrawPathOnDc(dc, d, box, color, units, filled, id == Id::Bolt);
}

void DrawPath(HDC dc, const wchar_t* d, const RECT& box, COLORREF color, float strokeUnits,
              bool filled) {
    if (!EnsureGdiplus()) return;
    const float units = strokeUnits > 0.0f ? strokeUnits : 1.8f;
    DrawPathOnDc(dc, d, box, color, units, filled, false);
}

namespace {

// ARGB(0xAARRGGBB) → Gdiplus::Color。alpha=0 时视为“不描边”。
Gdiplus::Color ColorFromArgb(DWORD argb) {
    return Gdiplus::Color(static_cast<BYTE>(argb >> 24), static_cast<BYTE>(argb >> 16),
                          static_cast<BYTE>(argb >> 8), static_cast<BYTE>(argb));
}

// 圆角矩形填充的共用实现：毛玻璃、投影层都走这里。
void FillRoundRectArgbImpl(HDC dc, const RECT& rc, int radius, DWORD argbFill,
                           DWORD argbBorder) {
    if (dc == nullptr) return;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    if (!EnsureGdiplus()) return;

    const float x = static_cast<float>(rc.left);
    const float y = static_cast<float>(rc.top);
    const float w = static_cast<float>(rc.right - rc.left);
    const float h = static_cast<float>(rc.bottom - rc.top);

    Gdiplus::GraphicsPath path;
    float r = static_cast<float>(radius);
    const float maxR = (w < h ? w : h) * 0.5f;
    if (r > maxR) r = maxR;
    if (r <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
    } else {
        AddRoundRectPath(path, x, y, w, h, r);
    }

    GraphicsScope scope(dc);
    Gdiplus::Graphics& g = scope.get();

    Gdiplus::SolidBrush brush(ColorFromArgb(argbFill));
    g.FillPath(&brush, &path);

    if ((argbBorder >> 24) != 0) {
        Gdiplus::Pen pen(ColorFromArgb(argbBorder), 1.0f);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&pen, &path);
    }
}

}  // namespace

// COLORREF 是 0x00BBGGRR，必须显式转成 0xAARRGGBB；
// 直接 OR 上 alpha 会把 R/B 互换（蓝色会变成橙色）。
DWORD ArgbFromColorref(int alpha, COLORREF c) {
    return ((DWORD)(alpha & 0xFF) << 24) | ((DWORD)GetRValue(c) << 16) |
           ((DWORD)GetGValue(c) << 8) | (DWORD)GetBValue(c);
}

void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    FillRoundRectArgbImpl(dc, rc, radius, ArgbFromColorref(255, fill),
                          ArgbFromColorref(255, border));
}

void FillRoundRectArgb(HDC dc, const RECT& rc, int radius, DWORD argbFill,
                       DWORD argbBorder) {
    FillRoundRectArgbImpl(dc, rc, radius, argbFill, argbBorder);
}

void FillRectArgb(HDC dc, const RECT& rc, DWORD argb) {
    if (dc == nullptr) return;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    if (!EnsureGdiplus()) return;
    GraphicsScope scope(dc);
    Gdiplus::Graphics& g = scope.get();
    Gdiplus::SolidBrush brush(ColorFromArgb(argb));
    g.FillRectangle(&brush, Gdiplus::RectF((float)rc.left, (float)rc.top,
                                           (float)(rc.right - rc.left),
                                           (float)(rc.bottom - rc.top)));
}

bool EnsureStarted() {
    EnsureGdiplus();
    return true;
}

void EndFrame() {
    if (g_frameG != nullptr) {
        // 析构会把 GDI+ 的绘制落到 HDC 上。
        delete g_frameG;
        g_frameG = nullptr;
    }
    g_frameDc = nullptr;
}

void BeginFrame(HDC dc) {
    EndFrame();
    if (dc == nullptr) return;
    if (!EnsureGdiplus()) return;
    g_frameDc = dc;
    g_frameG = new Gdiplus::Graphics(dc);
    if (g_frameG == nullptr) {
        g_frameDc = nullptr;
        return;
    }
    g_frameG->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g_frameG->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

void DrawRing(HDC dc, int cx, int cy, int radius, int thickness, COLORREF track, COLORREF fill,
              float percent, float startDeg) {
    if (dc == nullptr || radius <= 0) return;
    if (!EnsureGdiplus()) return;

    // 厚度至少 1，且中心线半径必须为正，否则环不可见。
    int t = thickness < 1 ? 1 : thickness;
    const float centerRadius = static_cast<float>(radius) - static_cast<float>(t) * 0.5f;
    if (!(centerRadius > 0.0f)) return;

    float pct = percent;
    if (!std::isfinite(pct)) pct = 0.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    float start = startDeg;
    if (!std::isfinite(start)) start = -90.0f;

    const Gdiplus::RectF rect(static_cast<float>(cx) - centerRadius,
                              static_cast<float>(cy) - centerRadius, centerRadius * 2.0f,
                              centerRadius * 2.0f);

    GraphicsScope scope(dc);
    Gdiplus::Graphics& g = scope.get();

    Gdiplus::Pen trackPen(ToColor(track), static_cast<float>(t));
    g.DrawEllipse(&trackPen, rect);

    if (pct <= 0.0f) return;  // 0%：只保留轨道

    Gdiplus::Pen progressPen(ToColor(fill), static_cast<float>(t));
    progressPen.SetStartCap(Gdiplus::LineCapRound);
    progressPen.SetEndCap(Gdiplus::LineCapRound);
    if (pct >= 100.0f) {
        // 100%：画完整椭圆，闭合且无缺口。
        g.DrawEllipse(&progressPen, rect);
    } else {
        // GDI+ 正角度为顺时针（屏幕坐标 y 向下），与进度环方向一致。
        g.DrawArc(&progressPen, rect, start, pct / 100.0f * 360.0f);
    }
}

void FillPolygon(HDC dc, const POINT* pts, int count, COLORREF color) {
    if (dc == nullptr || pts == nullptr || count < 3) return;
    if (!EnsureGdiplus()) return;

    // 逐边 AddLine，避免为 Gdiplus::Point 数组做动态分配；CloseFigure 负责首尾闭合。
    Gdiplus::GraphicsPath path;
    path.StartFigure();
    for (int i = 0; i + 1 < count; ++i) {
        path.AddLine(static_cast<Gdiplus::REAL>(pts[i].x), static_cast<Gdiplus::REAL>(pts[i].y),
                     static_cast<Gdiplus::REAL>(pts[i + 1].x),
                     static_cast<Gdiplus::REAL>(pts[i + 1].y));
    }
    path.CloseFigure();

    GraphicsScope scope(dc);
    Gdiplus::Graphics& g = scope.get();

    Gdiplus::SolidBrush brush(ToColor(color));
    g.FillPath(&brush, &path);
}

bool InspectPath(const wchar_t* d, RECT* bounds, int* points) {
    if (bounds != nullptr) SetRect(bounds, 0, 0, 0, 0);
    if (points != nullptr) *points = 0;
    if (d == nullptr || *d == L'\0') return false;
    if (!EnsureGdiplus()) return false;

    Gdiplus::GraphicsPath path;
    const int anchors = ParsePath(d, path);
    if (points != nullptr) *points = anchors;

    if (anchors <= 0) return false;
    if (path.GetPointCount() <= 0) return false;  // 只有 M/Z、没有实际几何

    Gdiplus::RectF rc(0.0f, 0.0f, 0.0f, 0.0f);
    if (path.GetBounds(&rc) != Gdiplus::Ok) return false;
    if (!std::isfinite(rc.X) || !std::isfinite(rc.Y) || !std::isfinite(rc.Width) ||
        !std::isfinite(rc.Height)) {
        return false;
    }

    if (bounds != nullptr) {
        // 向外取整，包围盒覆盖到像素边界，便于调用方直接使用。
        bounds->left = static_cast<LONG>(std::floor(rc.X));
        bounds->top = static_cast<LONG>(std::floor(rc.Y));
        bounds->right = static_cast<LONG>(std::ceil(rc.X + rc.Width));
        bounds->bottom = static_cast<LONG>(std::ceil(rc.Y + rc.Height));
    }
    return true;
}

// 仅供 tests/test_svg_icon.cpp 遍历图标表使用。
// 不在 svg_icon.h 暴露，避免扩大公开契约。
const wchar_t* IconPathDataForTest(Id id) { return IconPathForId(id); }

}  // namespace svgicon
