#include "paint_region.h"

#include <cassert>
#include <iostream>

// B05：ps.rcPaint 增量重绘的几何判定。这里只测纯函数，不启动窗口。
namespace {

int g_checks = 0;

#define CHECK(expr)     \
    do {                \
        ++g_checks;     \
        assert(expr);   \
    } while (0)

const RECT kClient{0, 0, 1000, 740};

void TestDecide() {
    // 整窗失效（窗口首次显示、尺寸变化、改 DPI）→ 整帧
    CHECK(paintregion::Decide(kClient, kClient) == paintregion::Mode::Full);
    // 悬停/胶囊只失效导航栏 → 增量
    RECT rail{0, 0, 76, 740};
    CHECK(paintregion::Decide(rail, kClient) == paintregion::Mode::Dirty);
    // 页脚 toast 只失效页脚 → 增量
    RECT footer{0, 714, 1000, 740};
    CHECK(paintregion::Decide(footer, kClient) == paintregion::Mode::Dirty);
    // 单张卡片/信息卡 → 增量
    RECT card{100, 100, 400, 300};
    CHECK(paintregion::Decide(card, kClient) == paintregion::Mode::Dirty);
    RECT single{5, 5, 6, 6};
    CHECK(paintregion::Decide(single, kClient) == paintregion::Mode::Dirty);
    // 空脏区说明没有可信刷新范围 → 退回整帧，避免留下半块旧画面
    RECT empty{10, 10, 10, 40};
    CHECK(paintregion::Decide(empty, kClient) == paintregion::Mode::Full);
    RECT zeroHeight{0, 0, 1000, 0};
    CHECK(paintregion::Decide(zeroHeight, kClient) == paintregion::Mode::Full);
}

void TestCovers() {
    CHECK(paintregion::Covers(kClient, kClient));
    RECT bigger{-5, -5, 1005, 745};
    CHECK(paintregion::Covers(bigger, kClient));
    // 缺 1 px 就不是整帧：否则右下角会漏画一条
    RECT oneShort{0, 0, 999, 740};
    CHECK(!paintregion::Covers(oneShort, kClient));
    RECT shortBottom{0, 0, 1000, 739};
    CHECK(!paintregion::Covers(shortBottom, kClient));
    RECT empty{0, 0, 0, 0};
    CHECK(!paintregion::Covers(empty, kClient));
    RECT emptyClient{0, 0, 0, 0};
    CHECK(!paintregion::Covers(kClient, emptyClient));
}

void TestClamp() {
    // 负坐标夹到 0（WM_PAINT 一般不会给，但布局/合成路径可能算出越界矩形）
    RECT v{-20, -10, 400, 300};
    CHECK(paintregion::ClampToClient(v, kClient));
    CHECK(v.left == 0 && v.top == 0 && v.right == 400 && v.bottom == 300);
    RECT v2{900, 700, 1200, 900};
    CHECK(paintregion::ClampToClient(v2, kClient));
    CHECK(v2.right == 1000 && v2.bottom == 740);
    // 完全落在客户区外 → 空，调用方按整帧处理
    RECT outside{1200, 800, 1400, 900};
    CHECK(!paintregion::ClampToClient(outside, kClient));
    RECT empty{};
    CHECK(!paintregion::ClampToClient(empty, kClient));
    // 客户区为空（最小化/销毁中）不当作有效范围
    RECT noClient{};
    RECT any{0, 0, 10, 10};
    CHECK(!paintregion::ClampToClient(any, noClient));
}

void TestTouches() {
    RECT card{100, 100, 400, 300};
    RECT inside{150, 150, 260, 260};
    CHECK(paintregion::Touches(inside, card, 0));
    RECT faraway{500, 500, 600, 600};
    CHECK(!paintregion::Touches(faraway, card, 6));
    // 右/下边界是开区间：脏区正好贴在卡片右侧时这张卡不用重画
    RECT adjacent{400, 100, 500, 300};
    CHECK(!paintregion::Touches(adjacent, card, 0));
    RECT onePx{399, 100, 500, 300};
    CHECK(paintregion::Touches(onePx, card, 0));
    // 卡片阴影外扩 3 px：脏区离卡片 4 px 时，只有外扩 6 px 才会重画这张卡，
    // 否则阴影与脏区交界处会残留旧像素。
    RECT gap4{100, 304, 400, 320};
    CHECK(!paintregion::Touches(gap4, card, 0));
    CHECK(paintregion::Touches(gap4, card, 6));
    RECT gap7{100, 307, 400, 320};
    CHECK(!paintregion::Touches(gap7, card, 6));
    // 零尺寸绘制块（当前视图不显示的内容区）不参与
    RECT zeroItem{100, 100, 100, 300};
    CHECK(!paintregion::Touches(inside, zeroItem, 6));
    RECT emptyDirty{};
    CHECK(!paintregion::Touches(emptyDirty, card, 6));
    // 负 pad 按 0 处理
    CHECK(paintregion::Touches(onePx, card, -6));
    CHECK(!paintregion::Touches(adjacent, card, -6));
}

}  // namespace

int main() {
    TestDecide();
    TestCovers();
    TestClamp();
    TestTouches();
    std::cout << "paint_region: 4 tests, " << g_checks << " checks passed\n";
    return 0;
}
