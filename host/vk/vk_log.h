// Logging for the Vulkan back end (hos_log, host/hos_log.c), and a limit per kind of message.
#pragma once
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C" void hos_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" [[noreturn]] void hos_fatal(void* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

namespace ovk {
/// at most `limit` messages of a kind
template <typename... A>
void vk_log_limited(const char* kind, int limit, const char* fmt, A... a) {
    static std::mutex mu;
    static std::unordered_map<std::string, int> counts;
    {
        std::lock_guard lk(mu);
        if (counts[kind]++ >= limit) return;
    }
    hos_log(fmt, a...);
}
}  // namespace ovk
