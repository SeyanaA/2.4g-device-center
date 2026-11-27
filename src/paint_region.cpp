#include "paint_region.h"

namespace paintregion {

bool IsEmpty(const RECT& rc) {
    return rc.right <= rc.left || rc.bottom <= rc.top;
}

bool ClampToClient(RECT& dirty, const RECT& client) {
    if (IsEmpty(dirty) || IsEmpty(client)) return false;
    if (dirty.left < client.left) dirty.left = client.left;
    if (dirty.top < client.top) dirty.top = client.top;
    if (dirty.right > client.right) dirty.right = client.right;
    if (dirty.bottom > client.bottom) dirty.bottom = client.bottom;
    return !IsEmpty(dirty);
}

bool Covers(const RECT& dirty, const RECT& client) {
    if (IsEmpty(dirty) || IsEmpty(client)) return false;
    return dirty.left <= client.left && dirty.top <= client.top &&
           dirty.right >= client.right && dirty.bottom >= client.bottom;
}

Mode Decide(const RECT& dirty, const RECT& client) {
    if (IsEmpty(dirty)) return Mode::Full;
    if (Covers(dirty, client)) return Mode::Full;
    return Mode::Dirty;
}

bool Touches(const RECT& dirty, const RECT& item, int pad) {
    if (IsEmpty(dirty) || IsEmpty(item)) return false;
    if (pad < 0) pad = 0;
    // 右/下边界是开区间：脏区刚好贴在绘制块边上时该块不需要重画。
    return dirty.left < item.right + pad && dirty.right > item.left - pad &&
           dirty.top < item.bottom + pad && dirty.bottom > item.top - pad;
}

}  // namespace paintregion
