// Host stand-ins only for IPC transport and service registration.
#pragma once
#include <common.h>
namespace skyline::kernel {
namespace type { struct KSession {}; }
namespace ipc {
struct IpcRequest {
    std::array<u64, 2> arguments;
    std::vector<span<u8>> outputBuf;
    size_t cursor{};
    template<class T> T Pop() { return static_cast<T>(arguments.at(cursor++)); }
};
struct IpcResponse {
    u64 value{};
    template<class T> void Push(T next) { value = next; }
};
}
}
namespace skyline::service {
using namespace kernel;
struct ServiceManager {};
struct BaseService {
    BaseService(const DeviceState &, ServiceManager &) {}
    virtual ~BaseService() = default;
};
}
#define SERVICE_DECL(...)
