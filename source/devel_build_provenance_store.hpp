#pragma once

#include "devel_build_provenance_codec.hpp"
#include "xdg_generation_store.hpp"
#include "xdg_paths.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <variant>

inline constexpr std::size_t devel_build_provenance_store_max_record_bytes =
    xdg_generation_store_default_max_record_bytes;

using DevelBuildProvenanceStoreObservedRecord =
    XdgGenerationObservedRecord;

struct DevelBuildProvenanceStoreMissing {
    bool operator==(const DevelBuildProvenanceStoreMissing&) const = default;
};

struct DevelBuildProvenanceStoreLoaded {
    DevelBuildProvenance provenance;
    DevelBuildProvenanceStoreObservedRecord observed;

    bool operator==(const DevelBuildProvenanceStoreLoaded&) const = default;
};

struct DevelBuildProvenanceStoreInvalidDocument {
    DevelBuildProvenanceInvalidDocument invalid;
    DevelBuildProvenanceStoreObservedRecord observed;

    bool operator==(
        const DevelBuildProvenanceStoreInvalidDocument&) const = default;
};

struct DevelBuildProvenanceStoreCorruptRecord {
    DevelBuildProvenanceCorruptDocument corrupt;
    DevelBuildProvenanceStoreObservedRecord observed;

    bool operator==(
        const DevelBuildProvenanceStoreCorruptRecord&) const = default;
};

struct DevelBuildProvenanceStoreSourceMismatch {
    DevelBuildProvenanceSourceMismatch mismatch;
    DevelBuildProvenanceStoreObservedRecord observed;

    bool operator==(
        const DevelBuildProvenanceStoreSourceMismatch&) const = default;
};

struct DevelBuildProvenanceStorePackageBaseMismatch {
    DevelBuildProvenanceSourceMismatch mismatch;
    DevelBuildProvenanceStoreObservedRecord observed;

    bool operator==(
        const DevelBuildProvenanceStorePackageBaseMismatch&) const = default;
};

struct DevelBuildProvenanceStoreFutureSchema {
    DevelBuildProvenanceFutureSchema future;
    DevelBuildProvenanceStoreObservedRecord observed;

    bool operator==(
        const DevelBuildProvenanceStoreFutureSchema&) const = default;
};

struct DevelBuildProvenanceStoreUnsafeHistory {
    XdgGenerationStoreUnsafeHistory store_result;

    bool operator==(
        const DevelBuildProvenanceStoreUnsafeHistory&) const = default;
};

struct DevelBuildProvenanceStoreAuthorityUnavailable {
    XdgGenerationStoreFailure store_failure;

    bool operator==(
        const DevelBuildProvenanceStoreAuthorityUnavailable&) const = default;
};

struct DevelBuildProvenanceStoreFailure {
    XdgGenerationStoreFailure store_failure;

    bool operator==(const DevelBuildProvenanceStoreFailure&) const = default;
};

using DevelBuildProvenanceStoreReadResult = std::variant<
    DevelBuildProvenanceStoreMissing,
    DevelBuildProvenanceStoreLoaded,
    DevelBuildProvenanceStoreInvalidDocument,
    DevelBuildProvenanceStoreCorruptRecord,
    DevelBuildProvenanceStoreSourceMismatch,
    DevelBuildProvenanceStorePackageBaseMismatch,
    DevelBuildProvenanceStoreFutureSchema,
    DevelBuildProvenanceStoreUnsafeHistory,
    DevelBuildProvenanceStoreAuthorityUnavailable,
    DevelBuildProvenanceStoreFailure>;

struct DevelBuildProvenanceStorePublished {
    DevelBuildProvenance provenance;
    DevelBuildProvenanceStoreObservedRecord observed;

    bool operator==(const DevelBuildProvenanceStorePublished&) const = default;
};

struct DevelBuildProvenanceStorePublishedUncertain {
    DevelBuildProvenance provenance;
    XdgGenerationStorePublishedUncertain store_result;

    bool operator==(
        const DevelBuildProvenanceStorePublishedUncertain&) const = default;
};

struct DevelBuildProvenanceStoreCasConflict {
    XdgGenerationStoreFailure store_failure;

    bool operator==(
        const DevelBuildProvenanceStoreCasConflict&) const = default;
};

enum class DevelBuildProvenanceStoreOverwriteRefusalReason {
    InvalidDocument,
    CorruptRecord,
    SourceMismatch,
    PackageBaseMismatch,
};

struct DevelBuildProvenanceStoreOverwriteRefused {
    DevelBuildProvenanceStoreOverwriteRefusalReason reason;
    DevelBuildProvenanceStoreObservedRecord observed;

    bool operator==(
        const DevelBuildProvenanceStoreOverwriteRefused&) const = default;
};

struct DevelBuildProvenanceStoreFutureSchemaOverwriteRefused {
    DevelBuildProvenanceFutureSchema future;
    DevelBuildProvenanceStoreObservedRecord observed;

    bool operator==(
        const DevelBuildProvenanceStoreFutureSchemaOverwriteRefused&) const =
        default;
};

using DevelBuildProvenanceStorePublishResult = std::variant<
    DevelBuildProvenanceStorePublished,
    DevelBuildProvenanceStorePublishedUncertain,
    DevelBuildProvenanceStoreCasConflict,
    DevelBuildProvenanceStoreOverwriteRefused,
    DevelBuildProvenanceStoreFutureSchemaOverwriteRefused,
    DevelBuildProvenanceStoreUnsafeHistory,
    DevelBuildProvenanceStoreAuthorityUnavailable,
    DevelBuildProvenanceStoreFailure>;

[[nodiscard]] xdg_paths::DevelBuildProvenancePaths
devel_build_provenance_store_paths();
[[nodiscard]] std::filesystem::path
devel_build_provenance_store_directory();
[[nodiscard]] std::filesystem::path devel_build_provenance_store_entry_path(
    const PackageBaseIdentity& package_base);

[[nodiscard]] DevelBuildProvenanceStoreReadResult
read_devel_build_provenance(
    const PackageBaseIdentity& expected_package_base);

// Exact predecessor CAS. An exact same payload with the current predecessor
// deliberately publishes a new generation; Slice 2 does not infer
// idempotence or retry/rebind policy for a future production publisher.
[[nodiscard]] DevelBuildProvenanceStorePublishResult
publish_devel_build_provenance(
    const DevelBuildProvenance& provenance,
    const std::optional<DevelBuildProvenanceStoreObservedRecord>&
        expected_observed);
