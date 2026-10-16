// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <diagnostics/content.h>
#include "results.h"
#include "IStorage.h"

namespace skyline::service::fssrv {
    IStorage::IStorage(std::shared_ptr<vfs::Backing> backing, const DeviceState &state, ServiceManager &manager, u64 diagnosticDataId) : backing(std::move(backing)), diagnosticDataId(diagnosticDataId), BaseService(state, manager) {}

    Result IStorage::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto offset{request.Pop<u64>()};
        auto size{request.Pop<u64>()};

        if (!diagnosticDataId || !diagnostics::Enabled()) {
            backing->Read(request.outputBuf.at(0), static_cast<size_t>(offset));
            return {};
        }
        try {
            size_t actualRead{};
            auto output{request.outputBuf.at(0)};
            backing->Read(output, static_cast<size_t>(offset), &actualRead);
            auto ordinal{diagnosticReads.fetch_add(1, std::memory_order_relaxed)};
            if (actualRead != output.size() || ordinal % 128 == 0)
                PINE_DIAG_CONTENT_LEVEL(actualRead == output.size() ? "INFO" : "ERROR", actualRead == output.size() ? "DLC_DATA_READ" : "DLC_RUNTIME_ERROR", {"title_id", fmt::format("{:016X}", diagnosticDataId)}, {"offset", fmt::format("{}", offset)}, {"request_size", fmt::format("{}", size)}, {"buffer_size", fmt::format("{}", output.size())}, {"actual_bytes", fmt::format("{}", actualRead)}, {"status", actualRead == output.size() ? "ok" : "short_read"}, {"success_sampling", "1_in_128"});
        } catch (const std::exception &e) {
            PINE_DIAG_CONTENT_ERROR("DLC_RUNTIME_ERROR", {"title_id", fmt::format("{:016X}", diagnosticDataId)}, {"reason", e.what()}, {"status", "exception"});
            throw;
        }
        return {};
    }

    Result IStorage::GetSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(backing->size);
        return {};
    }
}
