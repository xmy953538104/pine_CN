// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <diagnostics/content.h>
#include "results.h"
#include "IStorage.h"

namespace skyline::service::fssrv {
    IStorage::IStorage(std::shared_ptr<vfs::Backing> backing, const DeviceState &state, ServiceManager &manager, u64 diagnosticDataId) : BaseService(state, manager), backing(std::move(backing)), diagnosticDataId(diagnosticDataId) {}

    Result IStorage::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto offset{request.Pop<u64>()};
        auto size{request.Pop<u64>()};

        if (!diagnosticDataId || !diagnostics::Enabled()) {
            backing->Read(request.outputBuf.at(0), static_cast<size_t>(offset));
            return {};
        }
        const auto ordinal{diagnosticReads.fetch_add(1, std::memory_order_relaxed) + 1};
        const auto samplingInterval{diagnostics::Verbose() ? 16U : 128U};
        size_t bufferSize{};
        bool bufferKnown{};
        try {
            vfs::ReadObservation observation;
            auto output{request.outputBuf.at(0)};
            bufferSize = output.size();
            bufferKnown = true;
            backing->Read(output, static_cast<size_t>(offset), &observation);
            const auto &actualRead{observation.bytes};
            const bool shortRead{actualRead && *actualRead != bufferSize};
            if (shortRead || (ordinal - 1) % samplingInterval == 0)
                PINE_DIAG_STORAGE_LEVEL(shortRead ? "ERROR" : "INFO", shortRead ? "DLC_RUNTIME_ERROR" : "DLC_DATA_READ",
                    {"title_id", diagnostics::ContentTitleId(diagnosticDataId)}, {"source", "dlc"}, {"operation", "read"},
                    {"offset", std::to_string(offset)}, {"request_size", std::to_string(size)},
                    {"buffer_size", std::to_string(bufferSize)}, {"actual_bytes", actualRead ? std::to_string(*actualRead) : "unknown"},
                    {"status", actualRead ? (shortRead ? "short_read" : "ok") : "unknown"},
                    {"byte_count_evidence", actualRead ? "reliable_backing_result" : "unavailable_for_backing"}, {"read_ordinal", std::to_string(ordinal)},
                    {"sampling_period", std::to_string(shortRead ? 1U : samplingInterval)},
                    {"success_sampling", samplingInterval == 16 ? "first_then_1_in_16" : "first_then_1_in_128"});
        } catch (const std::exception &e) {
            PINE_DIAG_STORAGE_ERROR("DLC_RUNTIME_ERROR", {"title_id", diagnostics::ContentTitleId(diagnosticDataId)}, {"source", "dlc"},
                {"operation", "read"}, {"offset", std::to_string(offset)}, {"request_size", std::to_string(size)},
                {"buffer_size", bufferKnown ? std::to_string(bufferSize) : "unknown"}, {"actual_bytes", "unknown"},
                {"read_ordinal", std::to_string(ordinal)}, {"reason", e.what()}, {"status", "exception"}, {"sampling_period", "1"});
            throw;
        }
        return {};
    }

    Result IStorage::GetSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(backing->size);
        return {};
    }
}
