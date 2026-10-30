#include "unlocker/errors.h"

namespace unlocker {
namespace {

class Category final : public std::error_category {
public:
    const char* name() const noexcept override { return "unlocker"; }
    std::string message(int code) const override {
        switch (static_cast<Error>(code)) {
            case Error::critical_process: return "critical system process";
            case Error::protected_handle: return "handle is protected from close";
            case Error::protected_process: return "protected process";
            case Error::not_supported: return "lock type cannot be released";
            case Error::system_held: return "held by the system (kernel/driver)";
            case Error::still_locked: return "share access closed on the server, but the file is still locked";
            case Error::no_process: return "locked, but no process found";
        }
        return "unknown";
    }
};

}  // namespace

const std::error_category& error_category() noexcept {
    static const Category instance;
    return instance;
}

std::error_code make_error_code(Error e) noexcept { return {static_cast<int>(e), error_category()}; }

}  // namespace unlocker
