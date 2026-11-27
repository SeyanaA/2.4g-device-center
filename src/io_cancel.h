#pragma once

#include <windows.h>

// 协作式取消（B01）：DeviceManager::Stop() 必须能打断正在阻塞的协议读取，
// 而不是干等整轮扫描（枚举 + 11 个协议 × N 个接口 × 每次最长数百 ms 的读）。
//
// 进程内只有一个 DeviceManager 工作线程，所以取消状态是单例：
//  - Request()   由 Stop()（UI 线程）调用：置位 + 打断该线程当前的同步 I/O；
//  - Reset()     由工作线程在入口调用，清掉上一次退出留下的状态；
//  - Requested() I/O 层在每次读写 / 每个 80 ms 读取切片之前检查。
namespace iocancel {

void Request();
void Reset();
bool Requested();

// 工作线程入口绑定真实线程句柄（内部复制），Request() 用它取消同步 I/O；
// 未绑定时 Request() 只置标志，不影响其它线程。Bind/Unbind 幂等。
void BindCurrentThread();
void UnbindCurrentThread();

// overlapped I/O 进行中登记：CancelSynchronousIo 只能打断“同步”I/O，
// 驱动返回 ERROR_IO_PENDING 的等待必须靠 CancelIoEx。I/O 层在开始等待前
// BeginIo(handle)、等待结束后 EndIo(handle)；Request() 会对登记的句柄
// CancelIoEx(h, nullptr)，让 WaitForSingleObject 立刻返回。
// 句柄只在 Begin/End 之间保证有效，调用方不需要自己加锁。
void BeginIo(HANDLE handle);
void EndIo(HANDLE handle);

// 可取消的等待：把 ms 拆成 ≤20 ms 的切片，期间被取消就立刻返回 false。
// 协议重试循环里的 Sleep(160/180/…) 在 Stop() 时会变成几个毫秒。
bool Sleep(unsigned ms);

}  // namespace iocancel
