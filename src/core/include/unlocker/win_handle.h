#pragma once

#include <windows.h>

#include <system_error>
#include <utility>

namespace unlocker {

// RAII-Wrapper für Kernel-Handles (CloseHandle). Behandelt nullptr und INVALID_HANDLE_VALUE als leer.
class WinHandle {
public:
    WinHandle() = default;
    explicit WinHandle(HANDLE h) noexcept : h_(normalize(h)) {}
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            reset();
            h_ = std::exchange(other.h_, nullptr);
        }
        return *this;
    }
    ~WinHandle() { reset(); }

    HANDLE get() const noexcept { return h_; }
    HANDLE release() noexcept { return std::exchange(h_, nullptr); }
    void reset(HANDLE h = nullptr) noexcept {
        if (h_) CloseHandle(h_);
        h_ = normalize(h);
    }
    explicit operator bool() const noexcept { return h_ != nullptr; }

private:
    static HANDLE normalize(HANDLE h) noexcept { return h == INVALID_HANDLE_VALUE ? nullptr : h; }
    HANDLE h_ = nullptr;
};

inline std::error_code last_error() {
    return std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

inline std::error_code win_error(DWORD code) {
    return std::error_code(static_cast<int>(code), std::system_category());
}

}  // namespace unlocker
