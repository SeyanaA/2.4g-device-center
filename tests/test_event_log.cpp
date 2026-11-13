#include "common.h"

#include <cassert>
#include <iostream>

int main() {
    // 保留当天 + 前 6 天：今天 2026-11-13，窗口 11-07 .. 11-13。
    assert(ShouldPurgeEventLog(2026, 11, 6, 2026, 11, 13, 7));   // 1. 7 天前删除
    assert(!ShouldPurgeEventLog(2026, 11, 7, 2026, 11, 13, 7));  // 2. 第 7 天保留
    assert(!ShouldPurgeEventLog(2026, 11, 13, 2026, 11, 13, 7));  // 3. 当天保留

    // 4. 跨月：10-31 在窗口外，11-01 在窗口内。
    assert(ShouldPurgeEventLog(2026, 10, 31, 2026, 11, 7, 7));
    assert(!ShouldPurgeEventLog(2026, 11, 1, 2026, 11, 7, 7));

    // 5. 跨年：2025-12-31 距 2026-01-03 只有 3 天，保留。
    assert(!ShouldPurgeEventLog(2025, 12, 31, 2026, 1, 3, 7));
    // 6. 跨年删除：2025-12-27 距 2026-01-03 为 7 天，删除。
    assert(ShouldPurgeEventLog(2025, 12, 27, 2026, 1, 3, 7));

    // 7. keepDays=1 只保留当天。
    assert(ShouldPurgeEventLog(2026, 11, 12, 2026, 11, 13, 1));
    // 8. keepDays<=0 按 1 处理，不会误删当天。
    assert(!ShouldPurgeEventLog(2026, 11, 13, 2026, 11, 13, 0));

    // 9. 未来日期不删除（时钟回拨/文件来自未来）。
    assert(!ShouldPurgeEventLog(2026, 12, 1, 2026, 11, 13, 7));

    // 10. 闰年 2-29 参与计算。
    assert(!ShouldPurgeEventLog(2028, 2, 29, 2028, 3, 1, 7));
    assert(ShouldPurgeEventLog(2028, 2, 20, 2028, 3, 1, 7));

    std::cout << "event_log: 12 tests passed\n";
    return 0;
}
