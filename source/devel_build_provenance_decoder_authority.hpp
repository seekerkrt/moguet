#pragma once

#include <string_view>
#include <variant>

struct DevelBuildProvenanceDecoded;
struct DevelBuildProvenanceInvalidDocument;
struct DevelBuildProvenanceCorruptDocument;
struct DevelBuildProvenanceFutureSchema;

using DevelBuildProvenanceDocument = std::variant<
    DevelBuildProvenanceDecoded,
    DevelBuildProvenanceInvalidDocument,
    DevelBuildProvenanceCorruptDocument,
    DevelBuildProvenanceFutureSchema>;

// Every granting header includes this complete declaration, including narrow
// value headers. Only the persistent codec entry can invoke the private mint;
// decoding historical values never grants fresh installed observation authority.
class DevelBuildProvenancePersistentDecoderAccess final {
    DevelBuildProvenancePersistentDecoderAccess() = delete;

    friend DevelBuildProvenanceDocument decode_devel_build_provenance(
        std::string_view document);

    [[nodiscard]] static DevelBuildProvenanceDocument decode_document(
        std::string_view document);
};
