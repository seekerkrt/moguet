#ifndef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
#error "publication fixtures must not enter the normal production graph"
#endif

#include "devel_build_provenance_publication_fixture.hpp"
#include "devel_build_provenance_publication.hpp"
#include "evaluated_devel_source_build.hpp"
#include "xdg_directory_safety.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace publication_allocation {
bool blocked = false;
unsigned failures = 0;
} // namespace publication_allocation
[[gnu::noinline]] void* operator new(std::size_t size) {
    if(publication_allocation::blocked) {
        ++publication_allocation::failures;
        throw std::bad_alloc();
    }
    if(void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
[[gnu::noinline]] void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {
namespace fs = std::filesystem;
using State = DevelBuildProvenancePublicationState;
using Stage = DevelBuildProvenancePublicationStage;
using Issue = DevelBuildProvenancePublicationIssue;
std::string_view g_scenario;
std::array<unsigned, 5> g_stages{};
const DevelBuildProvenance* g_seed = nullptr;

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
template <class T, class V>
const T& arm(const V& value, const char* message) {
    const auto* result = std::get_if<T>(&value);
    require(result != nullptr, message);
    return *result;
}
void write_document(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    require(static_cast<bool>(out), "fixture document write failed");
}
void replace_field(std::string& bytes, std::string_view key, std::string_view value) {
    const auto prefix = "\n" + std::string(key) + " = ";
    const auto start = bytes.find(prefix);
    require(start != std::string::npos, "fixture key missing");
    const auto end = bytes.find('\n', start + 1);
    bytes.replace(start + 1, end - start - 1, std::string(key) + " = \"" + std::string(value) + "\"");
}
void seed_store(std::string_view scenario, const DevelBuildProvenance& expected) {
    std::optional<DevelBuildProvenance> seed;
    seed.emplace(expected);
    if(scenario == "loaded") {
        auto bytes = encode_devel_build_provenance(*seed);
        replace_field(bytes, "actual_built_git_oid", std::string(40, 'f'));
        auto decoded = decode_devel_build_provenance(bytes);
        seed.emplace(arm<DevelBuildProvenanceDecoded>(decoded, "historical seed did not decode").provenance);
    }
    const auto published = publish_devel_build_provenance(*seed, std::nullopt);
    const auto& origin = arm<DevelBuildProvenanceStorePublished>(published, "historical seed failed");
    const auto unit = devel_build_provenance_store_entry_path(expected.package_base());
    auto bytes = origin.observed.raw_contents;
    if(scenario == "future" || scenario == "late-future") bytes.replace(bytes.find("schema_version = 1"), 18, "schema_version = 2");
    if(scenario == "corrupt" || scenario == "late-corrupt") bytes = "not valid TOML = [\n";
    if(scenario == "wrong-source") replace_field(bytes, "aur_git_remote", "https://aur.archlinux.org/other-source.git");
    if(scenario == "wrong-base") {
        for(auto key : {"package_base", "artifact_package_base", "installed_package_base"})
            replace_field(bytes, key, "other-base");
    }
    if(bytes != origin.observed.raw_contents) write_document(unit / origin.observed.leaf_name, bytes);
    if(scenario == "unsafe" || scenario == "late-unsafe") write_document(unit / "unrecognized", "foreign entry");
}
void publisher_stage(Stage stage) {
    ++g_stages[static_cast<std::size_t>(stage)];
    if((g_scenario == "projection-resource" && stage == Stage::Projection) ||
       (g_scenario == "serialization-resource" && stage == Stage::Serialization)) {
        publication_allocation::blocked = true;
        // Let the production projection/codec encounter the denied allocation.
    }
    if(g_scenario == "serialization-failure" && stage == Stage::Serialization) throw std::logic_error("fixture serialization fault");
    if(stage != Stage::StorePublication) return;
    if(g_scenario == "cas-conflict" || g_scenario == "late-corrupt" || g_scenario == "late-future" || g_scenario == "late-unsafe") seed_store(g_scenario, *g_seed);
    if(g_scenario == "late-authority") require(::setenv("XDG_STATE_HOME", "relative", 1) == 0, "fixture environment failure");
}
void low_resource(const XdgGenerationStoreTestRaceContext&) {
    publication_allocation::blocked = true;
    static_cast<void>(::operator new(1));
}
void low_internal(const XdgGenerationStoreTestRaceContext&) {
    throw std::logic_error("fixture store internal failure");
}
void deny_after_verified(const XdgGenerationStoreTestRaceContext&) {
    publication_allocation::blocked = true;
}
std::optional<std::error_code> parent_sync_failure(int) {
    return std::make_error_code(std::errc::io_error);
}
struct Cleanup {
    std::optional<std::string> state_home;
    Cleanup() {
        if(const char* value = std::getenv("XDG_STATE_HOME")) state_home = value;
    }
    ~Cleanup() {
        publication_allocation::blocked = false;
        set_devel_build_provenance_publication_test_hook(nullptr);
        reset_xdg_generation_store_test_hooks();
        xdg_directory_safety::set_managed_parent_sync_hook_for_test(nullptr);
        if(state_home)
            ::setenv("XDG_STATE_HOME", state_home->c_str(), 1);
        else
            ::unsetenv("XDG_STATE_HOME");
    }
};
} // namespace

std::vector<std::string> devel_publication_fixture_cases(bool projection) {
    if(projection) return {"install", "upgrade", "reinstall", "downgrade", "historical-drift", "loaded", "same-payload", "invalid-input", "not-requested"};
    return {"not-attempted", "unknown-wait", "nonzero", "no-post", "malformed-receipt", "receipt-resource", "outer-mtree", "outer-empty-files",
            "generation-unsupported", "outer-world", "final-mismatch", "unsupported-cardinality", "projection-resource", "serialization-failure", "serialization-resource",
            "store-authority", "namespace-sync", "write-failure", "file-sync", "cas-conflict", "future", "corrupt", "unsafe", "wrong-source", "wrong-base",
            "late-corrupt", "late-future", "late-unsafe", "late-authority", "post-link-resource", "post-link-sync", "cleanup-failure", "retirement-failure",
            "verified-no-allocation", "store-resource", "store-internal"};
}
std::string devel_publication_fixture_install_mode(std::string_view scenario) {
    for(auto mode : {"not-attempted", "unknown-wait", "nonzero", "no-post", "malformed-receipt", "receipt-resource", "outer-mtree", "outer-empty-files",
                     "generation-unsupported", "outer-world", "upgrade", "reinstall", "downgrade", "cleanup-failure", "retirement-failure"})
        if(scenario == mode) return std::string(mode);
    return "install";
}

void check_devel_publication_fixture(std::string_view scenario, DevelSourceArtifactInstallResult installation,
                                     const std::function<void()>& replace_installed_record) {
    Cleanup cleanup;
    g_scenario = scenario;
    g_stages.fill(0);
    publication_allocation::failures = 0;
    const auto namespace_path = devel_build_provenance_store_directory();
    require(!fs::exists(namespace_path), "S5 fixture already published provenance");
    const auto before = std::tuple{installation.operation(), installation.pacman_exit_status(), installation.receipt_state(), installation.receipt_issue(),
                                   installation.proof_state(), installation.proof_issue(), installation.binding_issue(), installation.privileged_cleanup().state,
                                   installation.privileged_cleanup().issue, installation.privileged_cleanup().helper_exit_status};
    const auto* transport = installation.transport_result();
    const auto transport_status = transport ? std::optional(transport->status()) : std::nullopt;
    std::optional<DevelBuildProvenance> expected;
    if(const auto* proof = installation.proof()) {
        const auto& built = proof->built_proof();
        auto semantic = make_devel_build_provenance(built.package_base(), built.reviewed_binding(), built.evaluated_source().git_source(),
                                                    built.actual_built_revision(), built.artifact().evidence(), proof->installed_binding());
        expected.emplace(std::move(std::get<DevelBuildProvenance>(semantic)));
    }
    g_seed = expected ? &*expected : nullptr;
    if(scenario == "loaded" || scenario == "same-payload" || scenario == "future" || scenario == "corrupt" || scenario == "unsafe" || scenario == "wrong-source" || scenario == "wrong-base") seed_store(scenario, *expected);
    if(scenario == "historical-drift") {
        require(static_cast<bool>(replace_installed_record), "missing current DB replacement control");
        replace_installed_record();
    }
    if(scenario == "store-authority") require(::setenv("XDG_STATE_HOME", "relative", 1) == 0, "environment fixture failure");
    if(scenario == "namespace-sync") xdg_directory_safety::set_managed_parent_sync_hook_for_test(&parent_sync_failure);
    if(scenario == "write-failure") fail_next_xdg_generation_store_operation_for_test(XdgGenerationStoreTestFailurePoint::Write);
    if(scenario == "file-sync") fail_next_xdg_generation_store_operation_for_test(XdgGenerationStoreTestFailurePoint::Sync);
    if(scenario == "post-link-sync") fail_next_xdg_generation_store_operation_for_test(XdgGenerationStoreTestFailurePoint::DirectorySync);
    if(scenario == "post-link-resource" || scenario == "store-resource") run_xdg_generation_store_race_once_for_test(
        scenario == "post-link-resource" ? XdgGenerationStoreTestRacePoint::AfterPublication : XdgGenerationStoreTestRacePoint::BeforePublication, &low_resource);
    if(scenario == "store-internal") run_xdg_generation_store_race_once_for_test(XdgGenerationStoreTestRacePoint::BeforePublication, &low_internal);
    if(scenario == "verified-no-allocation") run_xdg_generation_store_race_once_for_test(XdgGenerationStoreTestRacePoint::AfterVerifiedPublication, &deny_after_verified);
    set_devel_build_provenance_publication_test_hook(&publisher_stage);
    if(scenario == "invalid-input") {
        auto held = std::move(installation);
        require(!publish_installed_devel_source_build(std::move(installation)) && held.valid(), "invalid S5 input accepted");
        require(g_stages == std::array<unsigned, 5>{} && !fs::exists(namespace_path), "invalid input touched store");
        std::cout << "S6-B publication invalid-input PASS\n";
        return;
    }
    auto published = publish_installed_devel_source_build(std::move(installation), scenario == "not-requested"
                                                                                       ? DevelBuildProvenancePublicationRequest::NotRequested
                                                                                       : DevelBuildProvenancePublicationRequest::Publish);
    const bool blocked_at_return = publication_allocation::blocked;
    publication_allocation::blocked = false;
    require(published && published->valid() && !installation.valid(), "publisher lost ownership/result");
    const auto stage_counts = g_stages;
    require(!publish_installed_devel_source_build(std::move(installation)) && stage_counts == g_stages, "publication input replayed");
    const auto& retained = published->installation();
    require(before == std::tuple{retained.operation(), retained.pacman_exit_status(), retained.receipt_state(), retained.receipt_issue(), retained.proof_state(),
                                 retained.proof_issue(), retained.binding_issue(), retained.privileged_cleanup().state, retained.privileged_cleanup().issue,
                                 retained.privileged_cleanup().helper_exit_status},
            "S6 flattened S5 dimensions");
    require((retained.transport_result() ? std::optional(retained.transport_result()->status()) : std::nullopt) == transport_status,
            "S6 lost original transport status");
    require(retained.source_context_cleanup() == DevelSourceArtifactInstallCleanupState::Retained, "source context lifetime was released early");
    const unsigned reads = g_stages[static_cast<std::size_t>(Stage::PredecessorRead)];
    const unsigned writes = g_stages[static_cast<std::size_t>(Stage::StorePublication)];
    if(!expected || scenario == "not-requested") {
        Issue why = scenario == "not-requested" ? Issue::NotRequested : retained.operation() == DevelSourceArtifactInstallOperation::NotAttempted ? Issue::NotExecuted
                                                                    : retained.operation() == DevelSourceArtifactInstallOperation::OutcomeUnknown ? Issue::OperationUnknown
                                                                    : retained.operation() == DevelSourceArtifactInstallOperation::Failed         ? Issue::OperationFailed
                                                                    : retained.receipt_state() != DevelSourceArtifactInstallReceipt::Complete     ? Issue::ReceiptUnavailable
                                                                                                                                                  : Issue::ProofIncomplete;
        require(published->state() == State::NotAttempted && published->issue() == why && !published->identity() && !published->projected_provenance(), "ineligible publication state/cause wrong");
        require(reads == 0 && writes == 0 && !published->store_read_result() && !published->store_publish_result() && !fs::exists(namespace_path), "no-publication gate touched store");
    } else {
        require(retained.proof() && retained.proof()->valid(), "publication failure invalidated final proof");
        const bool complete = scenario == "install" || scenario == "upgrade" || scenario == "reinstall" || scenario == "downgrade" || scenario == "loaded" ||
                              scenario == "same-payload" || scenario == "historical-drift" || scenario == "cleanup-failure" || scenario == "retirement-failure" || scenario == "verified-no-allocation";
        const bool uncertain = scenario == "post-link-resource" || scenario == "post-link-sync";
        require(published->state() == (complete ? State::Complete : uncertain ? State::OutcomeUnknown
                                                                              : State::Failed),
                "publication state mapping wrong");
        if(complete) {
            require(reads == 1 && writes == 1 && !published->issue() && published->identity(), "complete result missing sealed identity");
            const auto readback = read_devel_build_provenance(expected->package_base());
            const auto& loaded = arm<DevelBuildProvenanceStoreLoaded>(readback, "publication readback missing");
            const auto encoded = encode_devel_build_provenance(*expected);
            require(loaded.provenance == *expected && loaded.observed.raw_contents == encoded, "schema projection differed from pre-consumption S5 snapshot");
            require(published->identity()->package_base == expected->package_base() && published->identity()->generation == loaded.observed.generation &&
                        published->identity()->document_sha256 == xdg_generation_store_raw_contents_sha256(encoded),
                    "publication identity not exact document identity");
            require(loaded.observed.generation == (scenario == "loaded" || scenario == "same-payload" ? 2U : 1U), "CAS generation/idempotence changed");
            require(std::count(encoded.begin(), encoded.end(), '\n') == 27 && encoded.starts_with("schema_version = 1\n") &&
                        encoded.find("transaction_token") == std::string::npos && encoded.find("/tmp/") == std::string::npos &&
                        encoded.find("lineage") == std::string::npos && encoded.find("source-artifact-installs") == std::string::npos,
                    "live evidence leaked into schema");
            arm<DevelBuildProvenanceStorePublished>(*published->store_publish_result(), "Complete did not come from verified store Published");
        } else if(uncertain) {
            require(reads == 1 && writes == 1 && !published->identity() && published->issue() == Issue::StorePublicationUncertain, "uncertainty was flattened");
            const auto& low = arm<DevelBuildProvenanceStorePublishedUncertain>(*published->store_publish_result(), "lost store uncertainty");
            require(low.provenance == *expected, "uncertain semantic evidence lost");
        } else if(scenario == "projection-resource" || scenario == "serialization-resource" || scenario == "serialization-failure") {
            require(reads == 0 && writes == 0 && !fs::exists(namespace_path) && published->issue() == (scenario == "serialization-failure" ? Issue::SerializationFailure : Issue::ResourceFailure), "local failure lost ordering/cause");
        } else if(scenario == "future" || scenario == "corrupt" || scenario == "unsafe" || scenario == "wrong-base" || scenario == "wrong-source" || scenario == "store-authority") {
            require(reads == 1 && writes == 0 && published->issue() == Issue::StoreReadRejected && published->store_read_result(), "read refusal was lost/retried");
            const auto& read = *published->store_read_result();
            if(scenario == "future") arm<DevelBuildProvenanceStoreFutureSchema>(read, "future cause lost");
            if(scenario == "corrupt") arm<DevelBuildProvenanceStoreCorruptRecord>(read, "corruption cause lost");
            if(scenario == "unsafe") arm<DevelBuildProvenanceStoreUnsafeHistory>(read, "unsafe history lost");
            if(scenario == "wrong-base") arm<DevelBuildProvenanceStorePackageBaseMismatch>(read, "base mismatch lost");
            if(scenario == "wrong-source") arm<DevelBuildProvenanceStoreSourceMismatch>(read, "source mismatch lost");
            if(scenario == "store-authority") arm<DevelBuildProvenanceStoreAuthorityUnavailable>(read, "authority failure lost");
        } else {
            require(reads == 1 && writes == 1 && published->issue() == Issue::StorePublicationFailed && published->store_publish_result(), "store failure was lost/retried");
            const auto& low = *published->store_publish_result();
            if(scenario == "cas-conflict")
                arm<DevelBuildProvenanceStoreCasConflict>(low, "CAS conflict adopted/rebased");
            else if(scenario == "late-corrupt")
                arm<DevelBuildProvenanceStoreOverwriteRefused>(low, "overwrite refusal lost");
            else if(scenario == "late-future")
                arm<DevelBuildProvenanceStoreFutureSchemaOverwriteRefused>(low, "future refusal lost");
            else if(scenario == "late-unsafe")
                arm<DevelBuildProvenanceStoreUnsafeHistory>(low, "unsafe history lost");
            else if(scenario == "late-authority")
                arm<DevelBuildProvenanceStoreAuthorityUnavailable>(low, "authority failure lost");
            else {
                const auto kind = arm<DevelBuildProvenanceStoreFailure>(low, "store fault not retained").store_failure.kind;
                const auto wanted = scenario == "write-failure" ? XdgGenerationStoreFailureKind::WriteFailed : scenario == "store-resource" ? XdgGenerationStoreFailureKind::ResourceFailure
                                                                                                           : scenario == "store-internal"   ? XdgGenerationStoreFailureKind::InternalFailure
                                                                                                                                            : XdgGenerationStoreFailureKind::SyncFailed;
                require(kind == wanted, "store fault kind flattened");
            }
        }
        if(scenario == "verified-no-allocation") require(blocked_at_return && publication_allocation::failures == 0, "publisher allocated after verified store success");
        if(scenario == "projection-resource" || scenario == "serialization-resource" || scenario == "post-link-resource" || scenario == "store-resource")
            require(blocked_at_return && publication_allocation::failures == 1, "resource result allocated again/escaped");
    }
    const auto state = published->state();
    auto moved = std::move(*published);
    require(!published->valid() && !published->identity() && moved.valid() && moved.state() == state, "result move retained source authority");
    bool rejected = false;
    try {
        static_cast<void>(published->installation());
    } catch(const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "moved-from aggregate remained usable");
    std::cout << "S6-B publication " << scenario << " read=" << reads << " write=" << writes << " PASS\n";
}
