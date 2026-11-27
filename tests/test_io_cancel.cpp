#include "io_cancel.h"

#include <atomic>
#include <cassert>
#include <iostream>

// B01：Stop() 的协作式取消。用 Win32 线程复现“工作线程阻塞在等待中”的循环，
// 主线程 Request() 后必须在一个很短的窗口内看到退出，而不是等整轮扫描。
namespace {

int g_checks = 0;

#define CHECK(expr)     \
    do {                \
        ++g_checks;     \
        assert(expr);   \
    } while (0)

struct LoopCtx {
    std::atomic<bool> exited{false};
    volatile LONG iterations = 0;
};

DWORD WINAPI LoopThread(LPVOID param) {
    auto* ctx = (LoopCtx*)param;
    iocancel::BindCurrentThread();
    const ULONGLONG end = GetTickCount64() + 2000;
    while (GetTickCount64() < end) {
        if (iocancel::Requested()) break;
        InterlockedIncrement(&ctx->iterations);
        Sleep(5);
    }
    iocancel::UnbindCurrentThread();
    ctx->exited = true;
    return 0;
}

void TestTokenBasics() {
    iocancel::Reset();
    CHECK(!iocancel::Requested());
    iocancel::Request();
    CHECK(iocancel::Requested());
    // 重复 Request 幂等
    iocancel::Request();
    CHECK(iocancel::Requested());
    iocancel::Reset();
    CHECK(!iocancel::Requested());
    // Reset 后可以重新开始一轮（Stop → Start 的语义）
    iocancel::Request();
    CHECK(iocancel::Requested());
    iocancel::Reset();
    CHECK(!iocancel::Requested());
}

void TestInterruptsBlockedLoop() {
    iocancel::Reset();
    LoopCtx ctx;
    HANDLE thread = CreateThread(nullptr, 0, LoopThread, &ctx, 0, nullptr);
    CHECK(thread != nullptr);
    // 让线程进入循环（正常等待 2 s）
    Sleep(40);
    const bool stillRunning = ctx.exited.load() == false;
    CHECK(stillRunning);

    const ULONGLONG begin = GetTickCount64();
    iocancel::Request();
    while (!ctx.exited.load() && GetTickCount64() - begin < 800) Sleep(2);
    const ULONGLONG elapsed = GetTickCount64() - begin;
    CHECK(ctx.exited.load());
    // 一个 5 ms 睡眠周期内必须退出；给 200 ms 上限避免 CI 抖动
    CHECK(elapsed < 200);
    WaitForSingleObject(thread, 1000);
    CloseHandle(thread);
    CHECK(ctx.iterations > 0);
    iocancel::Reset();
}

void TestRequestNeedsNoBindingOnCaller() {
    // Request() 由 UI 线程调用，它自己不需要 Bind：打断的是工作线程绑定的那个句柄。
    iocancel::UnbindCurrentThread();
    iocancel::Reset();
    CHECK(!iocancel::Requested());
    LoopCtx ctx;
    HANDLE thread = CreateThread(nullptr, 0, LoopThread, &ctx, 0, nullptr);
    CHECK(thread != nullptr);
    Sleep(40);
    CHECK(!ctx.exited.load());

    const ULONGLONG begin = GetTickCount64();
    iocancel::Request();
    while (!ctx.exited.load() && GetTickCount64() - begin < 800) Sleep(2);
    CHECK(ctx.exited.load());
    CHECK(GetTickCount64() - begin < 200);
    WaitForSingleObject(thread, 1000);
    CloseHandle(thread);
    iocancel::Reset();
}

void TestBindUnbindIdempotent() {
    // 重复 Bind/Unbind 不得崩（内部句柄被替换/关闭后仍是有效状态）。
    iocancel::BindCurrentThread();
    iocancel::BindCurrentThread();
    iocancel::Request();  // 当前线程没有阻塞 I/O：CancelSynchronousIo 失败无副作用
    CHECK(iocancel::Requested());
    iocancel::UnbindCurrentThread();
    iocancel::UnbindCurrentThread();
    iocancel::Reset();
    CHECK(!iocancel::Requested());
}

}  // namespace

int main() {
    TestTokenBasics();
    TestInterruptsBlockedLoop();
    TestRequestNeedsNoBindingOnCaller();
    TestBindUnbindIdempotent();
    std::cout << "io_cancel: 4 tests, " << g_checks << " checks passed\n";
    return 0;
}
