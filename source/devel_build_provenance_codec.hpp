#pragma once

#include "devel_build_provenance.hpp"
#include "devel_build_provenance_decoder_authority.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

inline constexpr std::int64_t devel_build_provenance_schema_version = 1;

enum class DevelBuildProvenanceInvalidReason {
    MissingSchemaVersion,
    MalformedSchemaVersion,
    MissingField,
    UnexpectedRepresentation,
    UnknownField,
    UnsupportedSourceKind,
    MalformedPackageBase,
    MalformedAurGitRemote,
    MalformedReviewedRecipeOid,
    MalformedReviewedStateGeneration,
    MalformedDigest,
    UnsupportedVcsKind,
    MalformedEvaluatedSourceLocation,
    UnsupportedSelectorKind,
    MalformedSelectorValue,
    UnsupportedArchitectureScope,
    MalformedActualBuiltOid,
    MalformedArtifactIdentity,
    UnsupportedInstalledGenerationScheme,
    MalformedInstalledGenerationIdentity,
    InconsistentProvenance,
};

struct DevelBuildProvenanceInvalidDocument {
    DevelBuildProvenanceInvalidReason reason;
    std::optional<std::string> field;

    bool operator==(
        const DevelBuildProvenanceInvalidDocument&) const = default;
};

enum class DevelBuildProvenanceCorruptReason {
    EmptyDocument,
    InvalidUtf8,
    UnparseableDocument,
};

struct DevelBuildProvenanceCorruptDocument {
    DevelBuildProvenanceCorruptReason reason;

    bool operator==(
        const DevelBuildProvenanceCorruptDocument&) const = default;
};

struct DevelBuildProvenanceFutureSchema {
    std::int64_t schema_version;

    bool operator==(const DevelBuildProvenanceFutureSchema&) const = default;
};

struct DevelBuildProvenanceDecoded {
    DevelBuildProvenance provenance;

    bool operator==(const DevelBuildProvenanceDecoded&) const = default;
};

[[nodiscard]] DevelBuildProvenanceDocument decode_devel_build_provenance(
    std::string_view document);

enum class DevelBuildProvenanceSourceMismatchReason {
    SourceIdentityMismatch,
    PackageBaseMismatch,
};

struct DevelBuildProvenanceSourceMismatch {
    DevelBuildProvenance provenance;
    PackageBaseIdentity expected;
    DevelBuildProvenanceSourceMismatchReason reason;

    bool operator==(const DevelBuildProvenanceSourceMismatch&) const = default;
};

using DevelBuildProvenanceInterpretation = std::variant<
    DevelBuildProvenanceDecoded,
    DevelBuildProvenanceInvalidDocument,
    DevelBuildProvenanceCorruptDocument,
    DevelBuildProvenanceFutureSchema,
    DevelBuildProvenanceSourceMismatch>;

[[nodiscard]] std::string encode_devel_build_provenance(
    const DevelBuildProvenance& provenance);

[[nodiscard]] DevelBuildProvenanceInterpretation
interpret_devel_build_provenance(
    std::string_view document,
    const PackageBaseIdentity& expected_package_base);
