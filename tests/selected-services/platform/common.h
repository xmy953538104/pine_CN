#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <common/base.h>
#include <common/exception.h>
using nullptr_t = std::nullptr_t;
#include <common/span.h>
#include <common/result.h>
namespace skyline {
namespace loader {class Loader;}
namespace kernel {class OS; namespace type {class KProcess;class KThread;}}
struct DeviceState {
 kernel::OS *os{};
 std::shared_ptr<loader::Loader> loader,updateLoader;
 std::vector<std::shared_ptr<loader::Loader>> dlcLoaders;
 std::shared_ptr<kernel::type::KProcess> process;
 static inline thread_local std::shared_ptr<kernel::type::KThread> thread;
};
}
#define LOGW(...) ((void)0)
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
#define LOGI(...) ((void)0)
#define LOGV(...) ((void)0)
#define LOGDNF(...) ((void)0)
#define LOGINF(...) ((void)0)
