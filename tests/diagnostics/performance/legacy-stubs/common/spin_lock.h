// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <condition_variable>
#include <mutex>
// Host platform stand-in only. CircularQueue's actual Push/Pop/wait code is unchanged.
namespace skyline { using SpinLock = std::mutex; }
