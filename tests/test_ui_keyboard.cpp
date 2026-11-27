#include "ui_keyboard.h"

#include <cassert>
#include <iostream>

namespace {

int g_checks = 0;

#define CHECK(expr)     \
    do {                \
        ++g_checks;     \
        assert(expr);   \
    } while (0)

void TestNextIndex() {
    CHECK(uikey::NextIndex(0, 3, 1) == 1);
    CHECK(uikey::NextIndex(1, 3, 1) == 2);
    CHECK(uikey::NextIndex(2, 3, 1) == 0);   // 正向回绕
    CHECK(uikey::NextIndex(0, 3, -1) == 2);  // 反向回绕
    CHECK(uikey::NextIndex(1, 3, -2) == 2);
    CHECK(uikey::NextIndex(0, 3, 4) == 1);
    CHECK(uikey::NextIndex(5, 3, 1) == 1);   // 非法输入按 0 起算
    CHECK(uikey::NextIndex(0, 0, 1) == uikey::kFocusNav);
}

void TestNextFocusSlot() {
    // 导航 → 首个/末个控件
    CHECK(uikey::NextFocusSlot(uikey::kFocusNav, 3, true) == 0);
    CHECK(uikey::NextFocusSlot(uikey::kFocusNav, 3, false) == 2);
    // 正向推进
    CHECK(uikey::NextFocusSlot(0, 3, true) == 1);
    CHECK(uikey::NextFocusSlot(1, 3, true) == 2);
    // 末尾回到导航
    CHECK(uikey::NextFocusSlot(2, 3, true) == uikey::kFocusNav);
    // 反向推进
    CHECK(uikey::NextFocusSlot(2, 3, false) == 1);
    CHECK(uikey::NextFocusSlot(1, 3, false) == 0);
    CHECK(uikey::NextFocusSlot(0, 3, false) == uikey::kFocusNav);
    // 只有一个控件时，Tab 双向都在导航与控件之间
    CHECK(uikey::NextFocusSlot(uikey::kFocusNav, 1, true) == 0);
    CHECK(uikey::NextFocusSlot(0, 1, true) == uikey::kFocusNav);
    CHECK(uikey::NextFocusSlot(0, 1, false) == uikey::kFocusNav);
    // 没有原生控件时停留在导航栏
    CHECK(uikey::NextFocusSlot(uikey::kFocusNav, 0, true) == uikey::kFocusNav);
    CHECK(uikey::NextFocusSlot(2, 0, false) == uikey::kFocusNav);
    // 非法当前槽位按导航栏处理
    CHECK(uikey::NextFocusSlot(9, 3, true) == 0);
    CHECK(uikey::NextFocusSlot(9, 3, false) == 2);
}

}  // namespace

int main() {
    TestNextIndex();
    TestNextFocusSlot();
    std::cout << "ui_keyboard: 2 tests, " << g_checks << " checks passed\n";
    return 0;
}
