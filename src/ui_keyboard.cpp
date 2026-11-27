#include "ui_keyboard.h"

namespace uikey {

int NextIndex(int current, int count, int delta) {
    if (count <= 0) return kFocusNav;
    int base = current;
    if (base < 0 || base >= count) base = 0;
    int next = (base + delta) % count;
    if (next < 0) next += count;
    return next;
}

int NextFocusSlot(int current, int nativeCount, bool forward) {
    if (nativeCount <= 0) return kFocusNav;
    if (current < 0 || current >= nativeCount) {
        // 从导航栏进入：正向到首个控件，反向到最后一个控件。
        return forward ? 0 : nativeCount - 1;
    }
    if (forward) return current + 1 < nativeCount ? current + 1 : kFocusNav;
    return current - 1 >= 0 ? current - 1 : kFocusNav;
}

}  // namespace uikey
