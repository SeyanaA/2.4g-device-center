// svg_icon 解析器 / 矢量绘制层测试。
//
// 覆盖：绝对与相对指令、M 之后隐式 L、C/S/Q/T 平滑曲线、圆弧
// （小弧/半圆/大弧/零半径）、非法输入容错、图标表逐项解析，
// 以及在真实内存 DC 上绘制不崩溃、不破坏 DC 状态。
//
// 运行入口由维护者在链接 -lgdiplus 后生成；本文件不包含构建脚本。

#include "svg_icon.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

namespace svgicon {
// 内部诊断入口，定义在 src/svg_icon.cpp；不在 svg_icon.h 暴露，
// 仅用于让测试逐个 Id 校验图标表。
const wchar_t* IconPathDataForTest(Id id);
}

namespace {

int AbsDiff(int a, int b) { return a > b ? a - b : b - a; }

bool Near(int actual, int expected, int eps = 2) { return AbsDiff(actual, expected) <= eps; }

bool BoundsNear(const RECT& r, int left, int top, int right, int bottom, int eps = 2) {
    return Near(r.left, left, eps) && Near(r.top, top, eps) && Near(r.right, right, eps) &&
           Near(r.bottom, bottom, eps);
}

bool Inspect(const wchar_t* d, RECT& bounds, int& points) {
    return svgicon::InspectPath(d, &bounds, &points);
}

void TestAbsoluteCommands() {
    RECT b{};
    int n = 0;

    assert(Inspect(L"M 2 3 L 20 3 L 20 18 L 2 18 Z", b, n));
    assert(BoundsNear(b, 2, 3, 20, 18));
    assert(n == 4);  // M + 3 个 L；Z 不产生新锚点

    // H / V 混合
    assert(Inspect(L"M 4 6 H 20 V 18 H 4 Z", b, n));
    assert(BoundsNear(b, 4, 6, 20, 18));
    assert(n == 4);

    // 单个 H 只有一条水平线（高度为 0）
    assert(Inspect(L"M 4 12 H 20", b, n));
    assert(BoundsNear(b, 4, 12, 20, 12));
    assert(n == 2);
}

void TestRelativeCommands() {
    RECT b{};
    int n = 0;

    assert(Inspect(L"M 4 4 l 16 0 v 14 h -16 z", b, n));
    assert(BoundsNear(b, 4, 4, 20, 18));
    assert(n == 4);

    // 相对路径从原点累加
    assert(Inspect(L"m 10 12 l 10 0", b, n));
    assert(BoundsNear(b, 10, 12, 20, 12));
    assert(n == 2);

    // Z 之后当前点回到子路径起点：l 2 2 必须从 (20,20) 出发而不是 (20,12)
    assert(Inspect(L"M 20 20 h -8 v -8 h 8 z l 2 2", b, n));
    assert(BoundsNear(b, 12, 12, 22, 22));
    assert(n == 5);  // M + h + v + h + l
}

void TestImplicitRepeats() {
    RECT b{};
    int n = 0;

    // M 之后多出来的坐标对按 L 处理
    assert(Inspect(L"M 0 0 10 10 20 20", b, n));
    assert(BoundsNear(b, 0, 0, 20, 20));
    assert(n == 3);  // M + 两个隐式 L

    // L 之后的隐式坐标对
    assert(Inspect(L"M 2 2 L 6 6 10 2 14 6", b, n));
    assert(BoundsNear(b, 2, 2, 14, 6));
    assert(n == 4);

    // H / V 的隐式重复
    assert(Inspect(L"M 2 2 H 8 14", b, n));
    assert(BoundsNear(b, 2, 2, 14, 2));
    assert(n == 3);

    assert(Inspect(L"M 2 2 V 8 14", b, n));
    assert(BoundsNear(b, 2, 2, 2, 14));
    assert(n == 3);
}

void TestCubicAndSmoothCubic() {
    RECT b{};
    int n = 0;

    assert(Inspect(L"M 2 12 C 2 4 10 2 12 12", b, n));
    assert(n == 4);  // M + 两个控制点 + 端点
    assert(Near(b.left, 2) && Near(b.right, 12));
    assert(b.top >= 1 && b.top <= 5);
    assert(BoundsNear(b, 2, 3, 12, 12, 3));

    // S 反射上一个三次控制点：应产生向下再向上的 S 形，不能崩溃且范围合理
    assert(Inspect(L"M 2 12 C 4 4 8 4 10 12 S 16 20 22 12", b, n));
    assert(n == 7);  // M + C(3) + S(3)
    assert(Near(b.left, 2) && Near(b.right, 22));
    assert(b.top >= 4 && b.top <= 8);
    assert(b.bottom >= 16 && b.bottom <= 20);

    // 前面没有 C 时，S 的第一个控制点退化为当前点
    assert(Inspect(L"M 4 20 S 10 4 20 20", b, n));
    assert(n == 4);
    assert(Near(b.left, 4) && Near(b.right, 20) && Near(b.bottom, 20));
    assert(b.top >= 3 && b.top <= 17);  // 曲线向上鼓起而不是直线
}

void TestQuadraticAndSmoothQuadratic() {
    RECT b{};
    int n = 0;

    assert(Inspect(L"M 2 20 Q 12 2 22 20", b, n));
    assert(n == 4);
    assert(Near(b.left, 2) && Near(b.right, 22) && Near(b.bottom, 20));
    // 数学最低点 y=11；这里放宽上界以兼容 GDI+ 是否把控制点计入包围盒
    assert(b.top >= 1 && b.top <= 14);

    // T 反射上一个二次控制点，终点继续向右
    assert(Inspect(L"M 2 20 Q 12 2 22 20 T 42 20", b, n));
    assert(n == 7);
    assert(Near(b.left, 2) && Near(b.right, 42));
    assert(b.bottom >= 26 && b.bottom <= 40);  // 第二段下探到 y≈29

    // 前面没有 Q 时，T 退化为直线
    assert(Inspect(L"M 4 4 T 20 4", b, n));
    assert(n == 4);
    assert(BoundsNear(b, 4, 4, 20, 4));
}

void TestArcSemicircleAndSmallArc() {
    RECT b{};
    int n = 0;

    // 上半圆：圆心 (12,12)，半径 8
    assert(Inspect(L"M 4 12 A 8 8 0 0 1 20 12", b, n));
    assert(BoundsNear(b, 4, 4, 20, 12));
    assert(n >= 4 && (n - 1) % 3 == 0);

    // 下半圆（sweep=0）
    assert(Inspect(L"M 4 12 A 8 8 0 0 0 20 12", b, n));
    assert(BoundsNear(b, 4, 12, 20, 20));

    // 90 度小弧：圆心 (12,12)
    assert(Inspect(L"M 12 4 A 8 8 0 0 1 20 12", b, n));
    assert(BoundsNear(b, 12, 4, 20, 12));

    // 相对圆弧与绝对等价
    assert(Inspect(L"M 12 4 a 8 8 0 0 1 8 8", b, n));
    assert(BoundsNear(b, 12, 4, 20, 12));

    // 半径不足时按规范放大：半径 1 不足以连接 (0,0)->(20,0)，会放大成半圆
    assert(Inspect(L"M 0 0 A 1 1 0 0 1 20 0", b, n));
    assert(Near(b.left, 0) && Near(b.right, 20));
    assert(b.top >= -12 && b.top <= -8);  // 放大后圆心在 (10,0)，向上一侧为 y=-10
}

void TestArcLargeAndZeroRadius() {
    RECT b{};
    int n = 0;

    // 270 度大弧：圆心 (20,4)，半径 8，扫过 180->450 度
    assert(Inspect(L"M 12 4 A 8 8 0 1 1 20 12", b, n));
    assert(BoundsNear(b, 12, -4, 28, 12));
    assert(n >= 4 && (n - 1) % 3 == 0);

    // 近乎整圆（起终点几乎重合，大弧 + 顺时针）
    assert(Inspect(L"M 12 4 A 8 8 0 1 1 12.02 4", b, n));
    assert(Near(b.left, 4, 3) && Near(b.right, 20, 3));
    assert(Near(b.top, -12, 3) && Near(b.bottom, 4, 3));

    // 半径为 0：退化为直线
    assert(Inspect(L"M 4 20 A 0 6 0 0 1 20 20", b, n));
    assert(BoundsNear(b, 4, 20, 20, 20));
    assert(n == 2);  // M + 直线端点

    // 起终点重合：按规范不绘制
    assert(Inspect(L"M 4 20 A 8 8 0 1 1 4 20", b, n) == false);

    // 紧凑旗标写法：large=0、sweep=1 后直接跟 x=20，省略分隔符
    assert(Inspect(L"M 12 4 A 8 8 0 0120 12", b, n));
    assert(BoundsNear(b, 12, 4, 20, 12));
}

void TestMalformedInputs() {
    RECT b{};
    int n = 0;

    // 提示词要求的六个用例：不崩溃即可；当前实现全部返回 false。
    assert(Inspect(L"M", b, n) == false);
    assert(Inspect(L"M 0", b, n) == false);
    assert(Inspect(L"Z", b, n) == false);
    assert(Inspect(L"", b, n) == false);
    assert(Inspect(L"M0 0 X 1 2", b, n) == false);        // 只有 move，没有几何
    assert(Inspect(L"M0 0 L 1e999 2", b, n) == false);    // 指数溢出

    // 未知指令不能吞掉后续合法指令
    assert(Inspect(L"M0 0 X 1 2 L 5 5", b, n));
    assert(BoundsNear(b, 0, 0, 5, 5));
    assert(n == 2);

    // 其余损坏数据：逐个跑一遍，只要不崩溃/不死循环即可。
    const wchar_t* cases[] = {
        L"....",
        L"M 1,,, 2 L",
        L"M0 0 C 1 2",
        L"M0 0 C 1 2 3 4",
        L"M0 0 S 1 2",
        L"M0 0 Q 1 1 2",
        L"M0 0 T",
        L"M0 0 H",
        L"M0 0 V",
        L"M0 0 A 1 1 0 1 1",
        L"M0 0 A nan inf 0 0 0 2 2",
        L"M0 0 L nan 2",
        L"M0 0 L inf 2",
        L"M0 0 L 1e-999 2",
        L"M0 0 L 1e308 2",
        L"M0 0 L 0.0.0 2",
        L"M0 0 Z Z Z",
        L"M0 0 L",
        L"M0 0 0",
        L"   ,,,   ",
        L"M0 0 A 5 5 0 9 9 2 2",  // 非常规旗标按非零处理
    };
    for (const wchar_t* d : cases) {
        RECT tmp{};
        int p = 0;
        svgicon::InspectPath(d, &tmp, &p);  // 只要求不崩溃
        assert(p >= 0);
    }

    // 超长重复坐标：命令预算应截断，不能卡死。
    std::wstring longPath = L"M0 0";
    for (int i = 0; i < 10000; ++i) longPath += L" L1 1";
    RECT lb{};
    int ln = 0;
    assert(svgicon::InspectPath(longPath.c_str(), &lb, &ln));
    assert(ln > 0);
}

void TestIconTable() {
    RECT b{};
    int n = 0;

    assert(svgicon::IconPathDataForTest(svgicon::Id::None) == nullptr);

    for (int i = 1; i <= static_cast<int>(svgicon::Id::Link); ++i) {
        const svgicon::Id id = static_cast<svgicon::Id>(i);
        const wchar_t* d = svgicon::IconPathDataForTest(id);
        assert(d != nullptr);
        assert(*d != L'\0');

        assert(svgicon::InspectPath(d, &b, &n));
        assert(n > 0);
        assert(b.left >= 0 && b.top >= 0 && b.right <= 24 && b.bottom <= 24);
        assert(b.right > b.left && b.bottom > b.top);
        assert(svgicon::DefaultStrokeUnits(id) > 0.0f);
    }
}

void TestDefaultStrokeUnits() {
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::Bolt) - 1.2f) < 0.001f);
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::Mouse) - 1.8f) < 0.001f);
    // 导航图标单独加粗到 2.0：20px 下 1.8 比原 GDI 2px 明显偏细，
    // 与「设备/日志/测试」三字字重不匹配。
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::NavDevice) - 2.0f) < 0.001f);
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::NavLog) - 2.0f) < 0.001f);
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::NavTest) - 2.0f) < 0.001f);
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::Keyboard) - 1.8f) < 0.001f);
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::Headset) - 1.8f) < 0.001f);
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::Other) - 1.8f) < 0.001f);
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::Bluetooth) - 1.8f) < 0.001f);
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::Usb) - 1.8f) < 0.001f);
    assert(std::fabs(svgicon::DefaultStrokeUnits(svgicon::Id::Link) - 1.8f) < 0.001f);
}

// ---------------------------------------------------------------------------
// 真实内存 DC：验证绘制入口不崩溃、确实有像素输出、且 DC 仍然可用。
// ---------------------------------------------------------------------------

struct Canvas {
    int width = 0;
    int height = 0;
    HDC screen = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;

    Canvas(int w, int h) : width(w), height(h) {
        screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        bitmap = CreateCompatibleBitmap(screen, w, h);
        oldBitmap = SelectObject(dc, bitmap);
    }

    ~Canvas() {
        if (dc != nullptr) {
            SelectObject(dc, oldBitmap);
            DeleteDC(dc);
        }
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (screen != nullptr) ReleaseDC(nullptr, screen);
    }

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    void Clear(COLORREF color) const {
        RECT r{0, 0, width, height};
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }
};

int NonWhitePixels(HDC dc, int w, int h) {
    int count = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const COLORREF pixel = GetPixel(dc, x, y);
            if (pixel != CLR_INVALID && pixel != RGB(255, 255, 255)) ++count;
        }
    }
    return count;
}

void TestDrawIcons(HDC dc, int w, int h) {
    RECT all{0, 0, w, h};
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &all, white);
    DeleteObject(white);

    for (int i = 1; i <= static_cast<int>(svgicon::Id::Link); ++i) {
        const svgicon::Id id = static_cast<svgicon::Id>(i);
        RECT normal{4, 4, 28, 28};
        svgicon::Draw(dc, id, normal, RGB(24, 26, 30), 0.0f, false);
        RECT selected{34, 34, 64, 64};
        svgicon::Draw(dc, id, selected, RGB(24, 26, 30), 0.0f, true);
        RECT thick{70, 4, 94, 28};
        svgicon::Draw(dc, id, thick, RGB(200, 80, 60), 3.0f, false);
    }

    assert(NonWhitePixels(dc, w, h) > 500);
    // 图标没有画到画布最左下角，说明缩放/居中（不是铺满）
    assert(GetPixel(dc, 0, h - 1) == RGB(255, 255, 255));
    // DC 仍然可用：GDI 状态没有被 GDI+ 打乱
    assert(GetDeviceCaps(dc, BITSPIXEL) > 0);
    assert(MoveToEx(dc, 0, 0, nullptr) != 0);
    assert(LineTo(dc, 5, 5) != 0);
}

void TestDrawPathRaw(HDC dc, int w, int h) {
    RECT all{0, 0, w, h};
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &all, white);
    DeleteObject(white);

    RECT box{2, 2, 40, 40};
    svgicon::DrawPath(dc, L"M2 2 L22 22", box, RGB(10, 20, 200), 1.8f, false);
    svgicon::DrawPath(dc, L"M2 12 C6 2 18 2 22 12", box, RGB(10, 20, 200), 0.0f, true);

    // 非法数据 / 空数据：静默跳过
    svgicon::DrawPath(dc, L"", box, RGB(0, 0, 0), 1.8f, false);
    svgicon::DrawPath(dc, L"M0 0 X 1 2", box, RGB(0, 0, 0), 1.8f, false);
    svgicon::DrawPath(dc, L"M0 0 L 1e999 2", box, RGB(0, 0, 0), 1.8f, false);
    svgicon::DrawPath(dc, nullptr, box, RGB(0, 0, 0), 1.8f, false);

    // 退化矩形：不绘制
    RECT degenerate{5, 5, 5, 30};
    svgicon::DrawPath(dc, L"M0 0 L24 24", degenerate, RGB(0, 0, 0), 1.8f, false);
    RECT inverted{40, 40, 10, 10};
    svgicon::DrawPath(dc, L"M0 0 L24 24", inverted, RGB(0, 0, 0), 1.8f, false);

    assert(NonWhitePixels(dc, w, h) > 20);
}

void TestFillRoundRect(HDC dc, int w, int h) {
    RECT all{0, 0, w, h};
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &all, white);
    DeleteObject(white);

    RECT card{8, 8, 72, 72};
    svgicon::FillRoundRect(dc, card, 14, RGB(235, 238, 242), RGB(208, 214, 222));

    assert(GetPixel(dc, 40, 40) == RGB(235, 238, 242));
    // 圆角处被切掉：直角位置仍为白底
    assert(GetPixel(dc, 8, 8) == RGB(255, 255, 255));

    // 退化矩形：直接返回，不崩溃
    RECT zeroWidth{10, 10, 10, 50};
    svgicon::FillRoundRect(dc, zeroWidth, 4, RGB(0, 0, 0), RGB(0, 0, 0));
    RECT zeroHeight{10, 10, 50, 10};
    svgicon::FillRoundRect(dc, zeroHeight, 4, RGB(0, 0, 0), RGB(0, 0, 0));
    RECT inverted{60, 60, 50, 50};
    svgicon::FillRoundRect(dc, inverted, 4, RGB(0, 0, 0), RGB(0, 0, 0));

    // 半径超过半边长：钳制为胶囊，不崩溃
    RECT round{74, 74, 96, 96};
    svgicon::FillRoundRect(dc, round, 100, RGB(250, 200, 0), RGB(250, 200, 0));
    assert(GetPixel(dc, 85, 85) == RGB(250, 200, 0));

    // radius <= 0：普通矩形（只检查内部像素，避免依赖边缘抗锯齿细节）
    RECT plain{2, 2, 20, 20};
    svgicon::FillRoundRect(dc, plain, 0, RGB(200, 0, 200), RGB(200, 0, 200));
    assert(GetPixel(dc, 10, 10) == RGB(200, 0, 200));
}

void TestDrawRing(HDC dc, int w, int h) {
    RECT all{0, 0, w, h};
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &all, white);
    DeleteObject(white);

    const COLORREF track = RGB(210, 214, 220);
    const COLORREF fill = RGB(30, 110, 200);

    // 0% 只有轨道；中心线半径 = 20-3 = 17，12 点位置 (30,13)
    svgicon::DrawRing(dc, 30, 30, 20, 6, track, fill, 0.0f);
    assert(GetPixel(dc, 30, 13) == track);

    // 100% 闭合，12 点位置变为进度色
    svgicon::DrawRing(dc, 70, 30, 20, 6, track, fill, 100.0f);
    assert(GetPixel(dc, 70, 13) == fill);

    // 常规进度 + 圆头端点
    svgicon::DrawRing(dc, 30, 70, 20, 6, track, fill, 37.5f);
    assert(GetPixel(dc, 30, 53) == fill);

    // 厚度 <= 0 时钳制为 1；percent 越界钳制
    svgicon::DrawRing(dc, 64, 70, 12, 0, track, fill, -5.0f);
    // 中心线半径 <= 0：直接返回
    svgicon::DrawRing(dc, 30, 70, 2, 8, track, fill, 50.0f);
    // 半径 <= 0：直接返回
    svgicon::DrawRing(dc, 30, 70, 0, 4, track, fill, 50.0f);
    // 部分在画布外：不崩溃
    svgicon::DrawRing(dc, -100, -100, 200, 4, track, fill, 50.0f);

    assert(NonWhitePixels(dc, w, h) > 100);
}

void TestFillPolygon(HDC dc, int w, int h) {
    RECT all{0, 0, w, h};
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &all, white);
    DeleteObject(white);

    POINT triangle[3] = {{8, 8}, {88, 8}, {48, 88}};
    svgicon::FillPolygon(dc, triangle, 3, RGB(250, 180, 20));
    assert(GetPixel(dc, 48, 20) == RGB(250, 180, 20));

    // 空参数 / 退化顶点数：静默返回
    svgicon::FillPolygon(dc, nullptr, 3, RGB(0, 0, 0));
    svgicon::FillPolygon(dc, triangle, 0, RGB(0, 0, 0));
    svgicon::FillPolygon(dc, triangle, 1, RGB(0, 0, 0));
    svgicon::FillPolygon(dc, triangle, 2, RGB(0, 0, 0));
    svgicon::FillPolygon(dc, triangle, -5, RGB(0, 0, 0));

    assert(NonWhitePixels(dc, w, h) > 1000);
}

void TestNullSafety() {
    RECT b{};
    int n = -1;

    assert(svgicon::InspectPath(nullptr, &b, &n) == false);
    assert(n == 0);
    assert(svgicon::InspectPath(L"", &b, &n) == false);
    assert(svgicon::InspectPath(L"M2 2 L20 20", nullptr, nullptr));

    svgicon::Draw(nullptr, svgicon::Id::Mouse, RECT{0, 0, 20, 20}, RGB(0, 0, 0));
    svgicon::Draw(nullptr, svgicon::Id::None, RECT{0, 0, 20, 20}, RGB(0, 0, 0));
    svgicon::DrawPath(nullptr, L"M0 0 L1 1", RECT{0, 0, 20, 20}, RGB(0, 0, 0), 1.8f, false);
    svgicon::FillRoundRect(nullptr, RECT{0, 0, 10, 10}, 4, RGB(0, 0, 0), RGB(0, 0, 0));
    svgicon::DrawRing(nullptr, 0, 0, 10, 2, RGB(0, 0, 0), RGB(0, 0, 0), 50.0f);
    svgicon::FillPolygon(nullptr, nullptr, 0, RGB(0, 0, 0));
}

}  // namespace

int main() {
    TestAbsoluteCommands();
    TestRelativeCommands();
    TestImplicitRepeats();
    TestCubicAndSmoothCubic();
    TestQuadraticAndSmoothQuadratic();
    TestArcSemicircleAndSmallArc();
    TestArcLargeAndZeroRadius();
    TestMalformedInputs();
    TestIconTable();
    TestDefaultStrokeUnits();

    Canvas canvas(96, 96);
    assert(canvas.dc != nullptr);
    TestDrawIcons(canvas.dc, canvas.width, canvas.height);
    TestDrawPathRaw(canvas.dc, canvas.width, canvas.height);
    TestFillRoundRect(canvas.dc, canvas.width, canvas.height);
    TestDrawRing(canvas.dc, canvas.width, canvas.height);
    TestFillPolygon(canvas.dc, canvas.width, canvas.height);

    TestNullSafety();

    std::cout << "svg_icon: 16 tests passed\n";
    return 0;
}
