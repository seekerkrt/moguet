#include "devel_build_provenance_store.hpp"

#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

constexpr std::string_view PROVENANCE_TEMPORARY_PREFIX =
    "-.moguet-devel-build-provenance-";

void require_aur_known_package_base(const PackageBaseIdentity& package_base) {
    const PackageSourceIdentity& source = package_base.source();
    if(source.kind() != PackageSourceKind::Aur ||
       source.location().kind() != SourceLocationKind::GitRemote ||
       source.location().state() != SourceLocationState::Known ||
       source.location().value() == nullptr) {
        throw std::invalid_argument(
            "Devel build provenance store requires a known AUR PackageBase.");
    }
}

bool is_future_provenance_document(std::string_view document) {
    return std::holds_alternative<DevelBuildProvenanceFutureSchema>(
        decode_devel_build_provenance(document));
}

XdgGenerationStoreConfiguration configuration_for(
    const PackageBaseIdentity& package_base) {
    require_aur_known_package_base(package_base);
    return XdgGenerationStoreConfiguration{
        devel_build_provenance_store_paths(), package_base.package_base(),
        std::string(PROVENANCE_TEMPORARY_PREFIX),
        devel_build_provenance_store_max_record_bytes,
        &is_future_provenance_document};
}

DevelBuildProvenanceStoreReadResult map_failure(
    XdgGenerationStoreFailure failure) {
    if(failure.kind ==
       XdgGenerationStoreFailureKind::AuthorityUnavailable) {
        return DevelBuildProvenanceStoreAuthorityUnavailable{
            std::move(failure)};
    }
    return DevelBuildProvenanceStoreFailure{std::move(failure)};
}

DevelBuildProvenanceStorePublishResult map_publish_failure(
    XdgGenerationStoreFailure failure) {
    if(failure.kind ==
       XdgGenerationStoreFailureKind::ConcurrentReplacement) {
        return DevelBuildProvenanceStoreCasConflict{std::move(failure)};
    }
    if(failure.kind ==
       XdgGenerationStoreFailureKind::AuthorityUnavailable) {
        return DevelBuildProvenanceStoreAuthorityUnavailable{
            std::move(failure)};
    }
    return DevelBuildProvenanceStoreFailure{std::move(failure)};
}

DevelBuildProvenanceStoreCasConflict expectation_conflict(
    const std::filesystem::path& entry_path) {
    return DevelBuildProvenanceStoreCasConflict{
        XdgGenerationStoreFailure{
            XdgGenerationStoreFailureKind::ConcurrentReplacement,
            entry_path, std::nullopt, std::nullopt, std::nullopt}};
}

DevelBuildProvenanceStoreReadResult interpret_store_read(
    const PackageBaseIdentity& expected_package_base,
    XdgGenerationStoreReadResult result) {
    if(std::holds_alternative<XdgGenerationStoreMissing>(result)) {
        return DevelBuildProvenanceStoreMissing{};
    }
    if(auto* loaded = std::get_if<XdgGenerationStoreLoaded>(&result)) {
        DevelBuildProvenanceInterpretation interpreted =
            interpret_devel_build_provenance(
                loaded->observed.raw_contents, expected_package_base);
        if(auto* provenance =
               std::get_if<DevelBuildProvenanceDecoded>(&interpreted)) {
            return DevelBuildProvenanceStoreLoaded{
                std::move(provenance->provenance),
                std::move(loaded->observed)};
        }
        if(auto* invalid = std::get_if<
               DevelBuildProvenanceInvalidDocument>(&interpreted)) {
            return DevelBuildProvenanceStoreInvalidDocument{
                std::move(*invalid), std::move(loaded->observed)};
        }
        if(auto* corrupt = std::get_if<
               DevelBuildProvenanceCorruptDocument>(&interpreted)) {
            return DevelBuildProvenanceStoreCorruptRecord{
                std::move(*corrupt), std::move(loaded->observed)};
        }
        if(auto* future = std::get_if<
               DevelBuildProvenanceFutureSchema>(&interpreted)) {
            return DevelBuildProvenanceStoreFutureSchema{
                std::move(*future), std::move(loaded->observed)};
        }
        DevelBuildProvenanceSourceMismatch mismatch = std::move(
            std::get<DevelBuildProvenanceSourceMismatch>(interpreted));
        if(mismatch.reason ==
           DevelBuildProvenanceSourceMismatchReason::PackageBaseMismatch) {
            return DevelBuildProvenanceStorePackageBaseMismatch{
                std::move(mismatch), std::move(loaded->observed)};
        }
        return DevelBuildProvenanceStoreSourceMismatch{
            std::move(mismatch), std::move(loaded->observed)};
    }
    if(auto* unsafe =
           std::get_if<XdgGenerationStoreUnsafeHistory>(&result)) {
        return DevelBuildProvenanceStoreUnsafeHistory{
            std::move(*unsafe)};
    }
    return map_failure(
        std::move(std::get<XdgGenerationStoreFailure>(result)));
}

} // namespace

xdg_paths::DevelBuildProvenancePaths
devel_build_provenance_store_paths() {
    return xdg_paths::resolve_devel_build_provenance_process_environment();
}

std::filesystem::path devel_build_provenance_store_directory() {
    return devel_build_provenance_store_paths().directory;
}

std::filesystem::path devel_build_provenance_store_entry_path(
    const PackageBaseIdentity& package_base) {
    return xdg_generation_store_entry_path(configuration_for(package_base));
}

DevelBuildProvenanceStoreReadResult read_devel_build_provenance(
    const PackageBaseIdentity& expected_package_base) {
    XdgGenerationStoreConfiguration configuration{
        {}, "unresolved", std::string(PROVENANCE_TEMPORARY_PREFIX), devel_build_provenance_store_max_record_bytes, &is_future_provenance_document};
    try {
        configuration = configuration_for(expected_package_base);
    } catch(const xdg_paths::ResolutionError&) {
        return DevelBuildProvenanceStoreAuthorityUnavailable{
            XdgGenerationStoreFailure{
                XdgGenerationStoreFailureKind::AuthorityUnavailable,
                expected_package_base.package_base(), std::nullopt,
                std::nullopt, std::nullopt}};
    }

    return interpret_store_read(
        expected_package_base, read_xdg_generation_store(configuration));
}

namespace {

DevelBuildProvenanceStorePublishResult publish_provenance(
    const DevelBuildProvenance& provenance,
    const std::optional<DevelBuildProvenanceStoreObservedRecord>&
        expected_observed) {
    const PackageBaseIdentity& package_base = provenance.package_base();
    XdgGenerationStoreConfiguration configuration{
        {}, "unresolved", std::string(PROVENANCE_TEMPORARY_PREFIX), devel_build_provenance_store_max_record_bytes, &is_future_provenance_document};
    try {
        configuration = configuration_for(package_base);
    } catch(const xdg_paths::ResolutionError&) {
        return DevelBuildProvenanceStoreAuthorityUnavailable{
            XdgGenerationStoreFailure{
                XdgGenerationStoreFailureKind::AuthorityUnavailable,
                package_base.package_base(), std::nullopt, std::nullopt,
                std::nullopt}};
    }
    const std::filesystem::path entry_path =
        xdg_generation_store_entry_path(configuration);

    // Semantic preflight refuses abnormal current documents even when a caller
    // supplies their raw token. The low-level CAS repeats the exact filesystem
    // proof under its exclusive lock, so a change after this read conflicts.
    DevelBuildProvenanceStoreReadResult current = interpret_store_read(
        package_base, read_xdg_generation_store(configuration));
    if(std::holds_alternative<DevelBuildProvenanceStoreMissing>(current)) {
        if(expected_observed.has_value()) {
            return expectation_conflict(entry_path);
        }
    } else if(auto* loaded =
                  std::get_if<DevelBuildProvenanceStoreLoaded>(&current)) {
        if(!expected_observed.has_value() ||
           *expected_observed != loaded->observed) {
            return expectation_conflict(entry_path);
        }
    } else if(auto* invalid = std::get_if<
                  DevelBuildProvenanceStoreInvalidDocument>(&current)) {
        return DevelBuildProvenanceStoreOverwriteRefused{
            DevelBuildProvenanceStoreOverwriteRefusalReason::InvalidDocument,
            std::move(invalid->observed)};
    } else if(auto* corrupt = std::get_if<
                  DevelBuildProvenanceStoreCorruptRecord>(&current)) {
        return DevelBuildProvenanceStoreOverwriteRefused{
            DevelBuildProvenanceStoreOverwriteRefusalReason::CorruptRecord,
            std::move(corrupt->observed)};
    } else if(auto* mismatch = std::get_if<
                  DevelBuildProvenanceStoreSourceMismatch>(&current)) {
        return DevelBuildProvenanceStoreOverwriteRefused{
            DevelBuildProvenanceStoreOverwriteRefusalReason::SourceMismatch,
            std::move(mismatch->observed)};
    } else if(auto* mismatch = std::get_if<
                  DevelBuildProvenanceStorePackageBaseMismatch>(&current)) {
        return DevelBuildProvenanceStoreOverwriteRefused{
            DevelBuildProvenanceStoreOverwriteRefusalReason::
                PackageBaseMismatch,
            std::move(mismatch->observed)};
    } else if(auto* future = std::get_if<
                  DevelBuildProvenanceStoreFutureSchema>(&current)) {
        return DevelBuildProvenanceStoreFutureSchemaOverwriteRefused{
            future->future, std::move(future->observed)};
    } else if(auto* unsafe = std::get_if<
                  DevelBuildProvenanceStoreUnsafeHistory>(&current)) {
        return std::move(*unsafe);
    } else if(auto* unavailable = std::get_if<
                  DevelBuildProvenanceStoreAuthorityUnavailable>(&current)) {
        return std::move(*unavailable);
    } else {
        return std::move(
            std::get<DevelBuildProvenanceStoreFailure>(current));
    }

    // Both successful arms own this value. Copy it before any record write;
    // after the low-level terminal result, projection must be move-only.
    DevelBuildProvenance success_value = provenance;
    XdgGenerationStorePublishResult result = publish_xdg_generation_store(
        configuration, encode_devel_build_provenance(provenance),
        expected_observed);
    if(auto* published =
           std::get_if<XdgGenerationStorePublished>(&result)) {
        return DevelBuildProvenanceStorePublished{
            std::move(success_value), std::move(published->observed)};
    }
    if(auto* uncertain =
           std::get_if<XdgGenerationStorePublishedUncertain>(&result)) {
        return DevelBuildProvenanceStorePublishedUncertain{
            std::move(success_value), std::move(*uncertain)};
    }
    if(auto* unsafe =
           std::get_if<XdgGenerationStoreUnsafeHistory>(&result)) {
        return DevelBuildProvenanceStoreUnsafeHistory{
            std::move(*unsafe)};
    }
    XdgGenerationStoreFailure failure =
        std::move(std::get<XdgGenerationStoreFailure>(result));
    if(failure.kind ==
       XdgGenerationStoreFailureKind::FutureSchemaOverwriteRefused) {
        // A future document can only appear here by racing the semantic
        // preflight. Preserve refusal even though no stable future payload is
        // available from this operation.
        return DevelBuildProvenanceStoreFailure{std::move(failure)};
    }
    return map_publish_failure(std::move(failure));
}

} // namespace

DevelBuildProvenanceStorePublishResult publish_devel_build_provenance(
    const DevelBuildProvenance& provenance,
    const std::optional<DevelBuildProvenanceStoreObservedRecord>& expected_observed) {
    static_assert(std::is_nothrow_move_constructible_v<DevelBuildProvenanceStorePublishResult>);
    try {
        return publish_provenance(provenance, expected_observed);
    } catch(const std::bad_alloc&) {
        // The low-level call handles its own post-commit exceptions. All
        // allocation in this wrapper precedes that call; returned evidence
        // is projected using only nothrow moves.
        return DevelBuildProvenanceStoreFailure{XdgGenerationStoreFailure{
            XdgGenerationStoreFailureKind::ResourceFailure, {}, std::nullopt, std::nullopt, std::nullopt}};
    } catch(const std::length_error&) {
        return DevelBuildProvenanceStoreFailure{XdgGenerationStoreFailure{
            XdgGenerationStoreFailureKind::ResourceFailure, {}, std::nullopt, std::nullopt, std::nullopt}};
    }
}
