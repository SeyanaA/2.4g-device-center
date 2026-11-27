#include "io_cancel.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace iocancel {

namespace {

std::atomic<bool> g_requested{false};
std::mutex g_threadMutex;
// 工作线程句柄的副本；Bind/Unbind/Request 都在 g_threadMutex 下读写。
HANDLE g_workerThread = nullptr;

// 正在 overlapped 等待的句柄（通常 0–1 个）。
std::mutex g_ioMutex;
std::vector<HANDLE> g_ioHandles;

}  // namespace

void Request() {
    g_requested.store(true, std::memory_order_release);
    // 复制一份句柄再调，避免与工作线程的 Unbind/CloseHandle 竞争（句柄被关掉后
    // 再 CancelSynchronousIo 是未定义句柄）。
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        if (g_workerThread) {
            DuplicateHandle(GetCurrentProcess(), g_workerThread, GetCurrentProcess(), &thread,
                            0, FALSE, DUPLICATE_SAME_ACCESS);
        }
    }
    if (thread) {
        // 只打断该线程“当前正在等待”的同步 I/O（ReadFile/WriteFile/HidD_*Feature）；
        // 没有在等就返回 FALSE，后续 I/O 由 Requested() 在发起前拦下。
        CancelSynchronousIo(thread);
        CloseHandle(thread);
    }
    // overlapped 等待不在 CancelSynchronousIo 的范围内，必须按句柄取消。
    std::lock_guard<std::mutex> lock(g_ioMutex);
    for (HANDLE handle : g_ioHandles) CancelIoEx(handle, nullptr);
}

void Reset() { g_requested.store(false, std::memory_order_release); }

bool Requested() { return g_requested.load(std::memory_order_acquire); }

void BeginIo(HANDLE handle) {
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    std::lock_guard<std::mutex> lock(g_ioMutex);
    g_ioHandles.push_back(handle);
}

void EndIo(HANDLE handle) {
    std::lock_guard<std::mutex> lock(g_ioMutex);
    for (auto it = g_ioHandles.begin(); it != g_ioHandles.end(); ++it) {
        if (*it == handle) {
            g_ioHandles.erase(it);
            break;
        }
    }
}

bool Sleep(unsigned ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        if (Requested()) return false;
        const ULONGLONG left = end - GetTickCount64();
        ::Sleep((DWORD)(left > 20 ? 20 : left));
    }
    return !Requested();
}

void BindCurrentThread() {
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup, 0,
                         FALSE, DUPLICATE_SAME_ACCESS)) {
        return;
    }
    HANDLE old = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        old = g_workerThread;
        g_workerThread = dup;
    }
    if (old) CloseHandle(old);
}

void UnbindCurrentThread() {
    HANDLE old = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        old = g_workerThread;
        g_workerThread = nullptr;
    }
    if (old) CloseHandle(old);
}

}  // namespace iocancel
