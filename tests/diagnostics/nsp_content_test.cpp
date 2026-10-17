// SPDX-License-Identifier: MPL-2.0
// Exercises the exact candidate-selection/catch helper used by the production NSP loop.
// Controlled candidates do not validate PFS0 parsing, NCA cryptography, tickets or keys.
#include "loader/nsp_content.h"
#include <array>
#include <cassert>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace skyline::diagnostics {
bool enabled{};
struct Observation {
    std::string event, level;
    std::map<std::string, std::string> fields;
};
std::vector<Observation> observations;
bool Enabled() noexcept { return enabled; }
void Emit(std::string_view category, std::string_view event,
          std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
          std::string_view level) noexcept {
    assert(category == "content");
    Observation observation{std::string(event), std::string(level), {}};
    for (const auto &[key, value] : fields) observation.fields.emplace(key, value);
    observations.push_back(std::move(observation));
}
}
enum class Kind { Program = 0, Meta = 1, Control = 2, Manual = 3, Data = 4, PublicData = 5 };
struct Candidate {
    Kind contentType;
    struct Header { std::uint64_t titleId; } header;
    void *romFs, *exeFs;
};
enum class Failure { MissingKey = 3 };
struct LoaderFailure : std::runtime_error {
    Failure error;
    explicit LoaderFailure(Failure error, const char *message = "No message") : std::runtime_error(message), error(error) {}
};
struct Slots {
    std::optional<Candidate> program, control, meta, publicData;
    skyline::loader::NspSelectionNames names;
    template<class MakeCandidate> void load(std::string_view name, MakeCandidate &&make) {
        skyline::loader::LoadNspContent<LoaderFailure>(name, std::forward<MakeCandidate>(make), program, control, meta, publicData, &names);
    }
};
int main() {
    using namespace skyline::diagnostics;
    int backing;
    const std::array candidates{
        Candidate{Kind::Program, {0x101}, &backing, &backing},
        Candidate{Kind::Program, {0xBAD1}, &backing, nullptr},
        Candidate{Kind::Program, {0xBAD2}, nullptr, &backing},
        Candidate{Kind::Control, {0x201}, &backing, nullptr},
        Candidate{Kind::Control, {0xBAD3}, nullptr, &backing},
        Candidate{Kind::Meta, {0x301}, nullptr, nullptr},
        Candidate{Kind::PublicData, {0x401}, nullptr, nullptr},
        Candidate{Kind::Data, {0xBAD4}, &backing, &backing},
        Candidate{Kind::Manual, {0xBAD5}, &backing, &backing},
        Candidate{Kind::Program, {0x102}, &backing, &backing},
        Candidate{Kind::Control, {0x202}, &backing, nullptr},
        Candidate{Kind::Meta, {0x302}, nullptr, nullptr},
        Candidate{Kind::PublicData, {0x402}, &backing, nullptr},
    };
    auto scan = [&](Slots &slots) {
        for (const auto &candidate : candidates)
            slots.load("/private/archive/entry.nca", [&] { return candidate; });
        assert(slots.program->header.titleId == 0x102 && slots.control->header.titleId == 0x202);
        assert(slots.meta->header.titleId == 0x302 && slots.publicData->header.titleId == 0x402);
    };
    Slots disabled;
    scan(disabled);
    assert(observations.empty());

    enabled = true;
    ContentScope scope("dlc", 2);
    Slots traced;
    scan(traced);
    assert(observations.size() == candidates.size() * 2);
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto &candidate = observations.at(i * 2);
        assert(candidate.event == "NCA_CANDIDATE");
        assert(candidate.fields.at("title_id") == ContentTitleId(candidates[i].header.titleId));
        assert(candidate.fields.at("content_name") == "entry.nca");
        assert(candidate.fields.at("loader_source") == "dlc" && candidate.fields.at("loader_index") == "2");
    }
    assert(observations.at(1).event == "NCA_SELECTED");
    for (const auto index : {1, 2, 4, 7, 8})
        assert(observations.at(index * 2 + 1).event == "NCA_SKIPPED");
    for (size_t index = 9; index < candidates.size(); ++index) {
        const auto &replacement = observations.at(index * 2 + 1);
        assert(replacement.event == "NCA_OVERWRITTEN");
        assert(replacement.fields.at("policy") == "last_eligible_of_type");
        assert(replacement.fields.at("replaced_title_id") == ContentTitleId((index - 8) * 0x100 + 1));
        assert(replacement.fields.at("title_id") == ContentTitleId(candidates[index].header.titleId));
    }

    traced.load("/private/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.nca", [&] { return candidates[0]; });
    assert(traced.names.program == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    traced.load("/private/BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB.nca", [&] { return candidates[1]; });
    assert(traced.names.program == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); // ineligible is not selected
    traced.load("/private/CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC.nca", [&] { return candidates[9]; });
    assert(traced.names.program == "cccccccccccccccccccccccccccccccc");
    assert(observations.back().fields.at("nca_content_id") == traced.names.program);
    assert(observations.back().fields.at("full_content_hash_verified") == "false");

    // A standard failure is observed, then the following candidate can replace the slot.
    traced.load("broken.nca", []() -> Candidate { throw std::runtime_error("controlled malformed NCA"); });
    assert(observations.back().event == "NCA_LOAD_FAILED" && observations.back().level == "ERROR");
    assert(observations.back().fields.at("action") == "continue" && traced.program->header.titleId == 0x102);
    const Candidate afterFailure{Kind::Program, {0x103}, &backing, &backing};
    std::string openedName;
    const std::string fullName{"/private/source/after-failure.nca"};
    traced.load(fullName, [&] { openedName = fullName; return afterFailure; });
    assert(traced.program->header.titleId == 0x103 && openedName == fullName);
    assert(observations.back().fields.at("content_name") == "after-failure.nca");

    // The original loader policy rethrows a fresh loader exception with the same code.
    try {
        traced.load("missing-key.nca", []() -> Candidate { throw LoaderFailure(Failure::MissingKey, "specific original text"); });
        assert(false);
    } catch (const LoaderFailure &e) {
        assert(e.error == Failure::MissingKey && std::string_view(e.what()) == "No message");
    }
    assert(observations.back().fields.at("action") == "propagate");
    assert(observations.back().fields.at("error_code") == "3" && observations.back().level == "ERROR");
    assert(traced.program->header.titleId == 0x103);
    const auto observed = observations.size();
    try { traced.load("non-standard.nca", []() -> Candidate { throw 42; }); assert(false); }
    catch (int value) { assert(value == 42); }
    assert(observations.size() == observed); // No new catch-all swallows a non-standard exception.

    enabled = false;
    try { traced.load("disabled-key.nca", []() -> Candidate { throw LoaderFailure(Failure::MissingKey); }); assert(false); }
    catch (const LoaderFailure &e) { assert(e.error == Failure::MissingKey); }
    traced.load("disabled-malformed.nca", []() -> Candidate { throw std::runtime_error("controlled"); });
    assert(observations.size() == observed);
}
