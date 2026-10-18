// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "loader.h"
#include "selected_validation.h"
#include <catalog/catalog.h>

namespace skyline::loader {
    enum class SelectedMode { Application, StandaloneRebuiltPatch };
    struct ProgramChoice {
        std::string groupId, contentId;
        std::optional<uint8_t> programIndex;
        std::optional<uint64_t> programId;
    };
    struct SelectionRequest {
        SelectedMode mode{SelectedMode::Application};
        catalog::ContentInfoProfile profile{catalog::ContentInfoProfile::Unknown};
        uint64_t applicationId{};
        ProgramChoice primary;
        std::optional<ProgramChoice> update;
        std::vector<std::string> addOnGroups;
        selected::Limits limits;
    };
    class SelectedContent;
    class SelectedContentLoader final : public Loader {
        friend class SelectedContent;
        friend std::shared_ptr<SelectedContent> PrepareSelectedContent(const catalog::Catalog &, const SelectionRequest &,
                                                                      std::shared_ptr<crypto::KeyStore>);
        catalog::Source owner;
        std::shared_ptr<vfs::FileSystem> controlFiles;
        bool primary{};
        explicit SelectedContentLoader(catalog::Source source) : owner(std::move(source)) {}
      public:
        std::string selectedGroupId;
        std::optional<uint8_t> selectedProgramIndex;
        std::string validationProfile{"bounded_structural_v1_no_signature_or_hardware_execution_claim"};
        void *LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state) override;
    };
    class SelectedContent {
        friend std::shared_ptr<SelectedContent> PrepareSelectedContent(const catalog::Catalog &, const SelectionRequest &,
                                                                      std::shared_ptr<crypto::KeyStore>);
        struct Binding { catalog::Source source; std::string sha256; };
        catalog::Source owner;
        std::vector<Binding> bindings;
        std::shared_ptr<crypto::KeyStore> keys;
        std::shared_ptr<SelectedContentLoader> primary, update;
        std::vector<std::shared_ptr<loader::Loader>> addOns;
        void Recheck() const;
      public:
        // Read-only inspection views. This remains a trusted internal C++ API;
        // legacy Loader subobjects include shared backing pointers, not a
        // capability boundary against arbitrary in-process mutation.
        std::shared_ptr<const SelectedContentLoader> Primary() const { return primary; }
        std::shared_ptr<const SelectedContentLoader> Update() const { return update; }
        std::vector<std::shared_ptr<const loader::Loader>> AddOns() const { return {addOns.begin(), addOns.end()}; }
        // All checks run before the first state mutation. A prepared bundle does
        // not authenticate mutable bytes merely because it has a stat token.
        void Install(DeviceState &state) const;
    };
    std::shared_ptr<SelectedContent> PrepareSelectedContent(const catalog::Catalog &catalog, const SelectionRequest &request,
                                                          std::shared_ptr<crypto::KeyStore> providedKeys);
}
