#include "devel_build_provenance_codec.hpp"

#include "package_identifier.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include <toml++/toml.hpp>

namespace {

constexpr std::string_view SCHEMA_VERSION_KEY = "schema_version";
constexpr std::string_view SOURCE_KIND_KEY = "source_kind";
constexpr std::string_view PACKAGE_BASE_KEY = "package_base";
constexpr std::string_view AUR_GIT_REMOTE_KEY = "aur_git_remote";
constexpr std::string_view REVIEWED_RECIPE_OID_KEY = "reviewed_recipe_oid";
constexpr std::string_view REVIEWED_STATE_GENERATION_KEY =
    "reviewed_state_generation";
constexpr std::string_view REVIEWED_STATE_DOCUMENT_SHA256_KEY =
    "reviewed_state_document_sha256";
constexpr std::string_view EVALUATED_VCS_KIND_KEY = "evaluated_vcs_kind";
constexpr std::string_view EVALUATED_SOURCE_LOCATION_KEY =
    "evaluated_source_location";
constexpr std::string_view EVALUATED_SELECTOR_KIND_KEY =
    "evaluated_selector_kind";
constexpr std::string_view EVALUATED_SELECTOR_VALUE_KEY =
    "evaluated_selector_value";
constexpr std::string_view EVALUATED_ARCHITECTURE_SCOPE_KEY =
    "evaluated_architecture_scope";
constexpr std::string_view ACTUAL_BUILT_GIT_OID_KEY =
    "actual_built_git_oid";
constexpr std::string_view ARTIFACT_CHILD_KEY = "artifact_child";
constexpr std::string_view ARTIFACT_PACKAGE_BASE_KEY =
    "artifact_package_base";
constexpr std::string_view ARTIFACT_FULL_VERSION_KEY =
    "artifact_full_version";
constexpr std::string_view ARTIFACT_ARCHITECTURE_KEY =
    "artifact_architecture";
constexpr std::string_view ARTIFACT_ARCHIVE_SHA256_KEY =
    "artifact_archive_sha256";
constexpr std::string_view ARTIFACT_MTREE_SHA256_KEY =
    "artifact_mtree_sha256";
constexpr std::string_view INSTALLED_CHILD_KEY = "installed_child";
constexpr std::string_view INSTALLED_PACKAGE_BASE_KEY =
    "installed_package_base";
constexpr std::string_view INSTALLED_FULL_VERSION_KEY =
    "installed_full_version";
constexpr std::string_view INSTALLED_ARCHITECTURE_KEY =
    "installed_architecture";
constexpr std::string_view INSTALLED_MTREE_SHA256_KEY =
    "installed_mtree_sha256";
constexpr std::string_view INSTALLED_DATABASE_RECORD_SHA256_KEY =
    "installed_database_record_sha256";
constexpr std::string_view INSTALLED_RECORD_GENERATION_SCHEME_KEY =
    "installed_record_generation_scheme";
constexpr std::string_view INSTALLED_RECORD_GENERATION_IDENTITY_KEY =
    "installed_record_generation_identity";

constexpr std::string_view AUR_SOURCE_KIND = "aur";
constexpr std::string_view GIT_VCS_KIND = "git";
constexpr std::string_view DEFAULT_HEAD_SELECTOR = "default-head";
constexpr std::string_view BRANCH_SELECTOR = "branch";
constexpr std::string_view INDEPENDENT_ARCHITECTURE_SCOPE = "independent";
constexpr std::string_view LINUX_NAME_TO_HANDLE_AT_SCHEME =
    "linux-name-to-handle-at";

constexpr std::array<std::string_view, 27> CURRENT_KEYS = {
    SCHEMA_VERSION_KEY,
    SOURCE_KIND_KEY,
    PACKAGE_BASE_KEY,
    AUR_GIT_REMOTE_KEY,
    REVIEWED_RECIPE_OID_KEY,
    REVIEWED_STATE_GENERATION_KEY,
    REVIEWED_STATE_DOCUMENT_SHA256_KEY,
    EVALUATED_VCS_KIND_KEY,
    EVALUATED_SOURCE_LOCATION_KEY,
    EVALUATED_SELECTOR_KIND_KEY,
    EVALUATED_SELECTOR_VALUE_KEY,
    EVALUATED_ARCHITECTURE_SCOPE_KEY,
    ACTUAL_BUILT_GIT_OID_KEY,
    ARTIFACT_CHILD_KEY,
    ARTIFACT_PACKAGE_BASE_KEY,
    ARTIFACT_FULL_VERSION_KEY,
    ARTIFACT_ARCHITECTURE_KEY,
    ARTIFACT_ARCHIVE_SHA256_KEY,
    ARTIFACT_MTREE_SHA256_KEY,
    INSTALLED_CHILD_KEY,
    INSTALLED_PACKAGE_BASE_KEY,
    INSTALLED_FULL_VERSION_KEY,
    INSTALLED_ARCHITECTURE_KEY,
    INSTALLED_MTREE_SHA256_KEY,
    INSTALLED_DATABASE_RECORD_SHA256_KEY,
    INSTALLED_RECORD_GENERATION_SCHEME_KEY,
    INSTALLED_RECORD_GENERATION_IDENTITY_KEY};

bool is_ascii_whitespace(char character) noexcept {
    switch(character) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '\f':
        case '\v':
            return true;
        default:
            return false;
    }
}

bool is_empty_or_whitespace(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), is_ascii_whitespace);
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t offset = 0;
    while(offset < text.size()) {
        const auto first = static_cast<unsigned char>(text[offset]);
        std::size_t length = 0;
        if(first <= 0x7f) {
            length = 1;
        } else if(first >= 0xc2 && first <= 0xdf) {
            length = 2;
            if(offset + 1 >= text.size()) return false;
            const auto second = static_cast<unsigned char>(text[offset + 1]);
            if(second < 0x80 || second > 0xbf) return false;
        } else if(first >= 0xe0 && first <= 0xef) {
            length = 3;
            if(offset + 2 >= text.size()) return false;
            const auto second = static_cast<unsigned char>(text[offset + 1]);
            const auto third = static_cast<unsigned char>(text[offset + 2]);
            const bool valid_second =
                first == 0xe0 ? second >= 0xa0 && second <= 0xbf
                : first == 0xed
                    ? second >= 0x80 && second <= 0x9f
                    : second >= 0x80 && second <= 0xbf;
            if(!valid_second || third < 0x80 || third > 0xbf) return false;
        } else if(first >= 0xf0 && first <= 0xf4) {
            length = 4;
            if(offset + 3 >= text.size()) return false;
            const auto second = static_cast<unsigned char>(text[offset + 1]);
            const auto third = static_cast<unsigned char>(text[offset + 2]);
            const auto fourth = static_cast<unsigned char>(text[offset + 3]);
            const bool valid_second =
                first == 0xf0 ? second >= 0x90 && second <= 0xbf
                : first == 0xf4
                    ? second >= 0x80 && second <= 0x8f
                    : second >= 0x80 && second <= 0xbf;
            if(!valid_second || third < 0x80 || third > 0xbf ||
               fourth < 0x80 || fourth > 0xbf) {
                return false;
            }
        } else {
            return false;
        }
        offset += length;
    }
    return true;
}

bool is_nonempty_token(std::string_view value) noexcept {
    return !value.empty() &&
           std::none_of(value.begin(), value.end(), [](char character) {
               const unsigned char byte =
                   static_cast<unsigned char>(character);
               return byte <= 0x20 || byte == 0x7f;
           });
}

bool is_current_key(std::string_view key) noexcept {
    return std::find(CURRENT_KEYS.begin(), CURRENT_KEYS.end(), key) !=
           CURRENT_KEYS.end();
}

DevelBuildProvenanceDocument invalid_document(
    DevelBuildProvenanceInvalidReason reason,
    std::string_view field = {}) {
    return DevelBuildProvenanceInvalidDocument{
        reason,
        field.empty() ? std::nullopt
                      : std::optional<std::string>(field)};
}

std::optional<DevelBuildProvenanceDocument> require_string(
    const toml::table& root, std::string_view key) {
    const toml::node* node = root.get(key);
    if(node == nullptr) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MissingField, key);
    }
    if(!node->value_exact<std::string>().has_value()) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::UnexpectedRepresentation,
            key);
    }
    return std::nullopt;
}

std::string string_value(const toml::table& root, std::string_view key) {
    return root.get(key)->value_exact<std::string>().value();
}

std::optional<std::uint64_t> parse_reviewed_state_generation(
    std::string_view value) {
    if(value.empty() || value.front() == '0') return std::nullopt;
    std::uint64_t generation = 0;
    const char* first = value.data();
    const char* last = first + value.size();
    const auto [position, error] =
        std::from_chars(first, last, generation, 10);
    if(error != std::errc{} || position != last || generation == 0) {
        return std::nullopt;
    }
    return generation;
}

char hex_digit(unsigned value) noexcept {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
}

std::string encode_toml_basic_string(std::string_view value) {
    std::string encoded = "\"";
    for(unsigned char character : value) {
        switch(character) {
            case '"':
                encoded += "\\\"";
                break;
            case '\\':
                encoded += "\\\\";
                break;
            case '\b':
                encoded += "\\b";
                break;
            case '\t':
                encoded += "\\t";
                break;
            case '\n':
                encoded += "\\n";
                break;
            case '\f':
                encoded += "\\f";
                break;
            case '\r':
                encoded += "\\r";
                break;
            default:
                if(character < 0x20) {
                    encoded += "\\u00";
                    encoded += hex_digit(character >> 4);
                    encoded += hex_digit(character & 0x0f);
                } else {
                    encoded += static_cast<char>(character);
                }
                break;
        }
    }
    encoded += '"';
    return encoded;
}

void append_string(
    std::string& document,
    std::string_view key,
    std::string_view value) {
    document += key;
    document += " = ";
    document += encode_toml_basic_string(value);
    document += '\n';
}

} // namespace

DevelBuildProvenanceDocument
DevelBuildProvenancePersistentDecoderAccess::decode_document(
    std::string_view document) {
    if(!is_valid_utf8(document)) {
        return DevelBuildProvenanceCorruptDocument{
            DevelBuildProvenanceCorruptReason::InvalidUtf8};
    }
    if(is_empty_or_whitespace(document)) {
        return DevelBuildProvenanceCorruptDocument{
            DevelBuildProvenanceCorruptReason::EmptyDocument};
    }

    toml::table root;
    try {
        root = toml::parse(document);
    } catch(const toml::parse_error&) {
        return DevelBuildProvenanceCorruptDocument{
            DevelBuildProvenanceCorruptReason::UnparseableDocument};
    }

    const toml::node* version_node = root.get(SCHEMA_VERSION_KEY);
    if(version_node == nullptr) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MissingSchemaVersion,
            SCHEMA_VERSION_KEY);
    }
    const auto version = version_node->value_exact<std::int64_t>();
    if(!version.has_value()) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MalformedSchemaVersion,
            SCHEMA_VERSION_KEY);
    }
    // Future ownership is determined from the version integer alone. Current
    // key rules must never turn a future document into overwriteable invalid
    // current state.
    if(*version > devel_build_provenance_schema_version) {
        return DevelBuildProvenanceFutureSchema{*version};
    }
    if(*version != devel_build_provenance_schema_version) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MalformedSchemaVersion,
            SCHEMA_VERSION_KEY);
    }

    for(const auto& [key, node] : root) {
        static_cast<void>(node);
        if(!is_current_key(key.str())) {
            return invalid_document(
                DevelBuildProvenanceInvalidReason::UnknownField,
                key.str());
        }
    }
    for(std::string_view key : CURRENT_KEYS) {
        if(key == SCHEMA_VERSION_KEY) continue;
        if(const auto failure = require_string(root, key)) return *failure;
    }

    const std::optional<std::uint64_t> reviewed_generation =
        parse_reviewed_state_generation(
            string_value(root, REVIEWED_STATE_GENERATION_KEY));
    if(!reviewed_generation.has_value()) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::
                MalformedReviewedStateGeneration,
            REVIEWED_STATE_GENERATION_KEY);
    }

    if(string_value(root, SOURCE_KIND_KEY) != AUR_SOURCE_KIND) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::UnsupportedSourceKind,
            SOURCE_KIND_KEY);
    }

    const std::string package_base_text =
        string_value(root, PACKAGE_BASE_KEY);
    const std::string aur_remote = string_value(root, AUR_GIT_REMOTE_KEY);
    std::optional<PackageBaseIdentity> package_base;
    try {
        package_base = PackageBaseIdentity::make(
            PackageSourceIdentity::aur(
                SourceLocationIdentity::known_git_remote(aur_remote)),
            package_base_text);
    } catch(const std::invalid_argument&) {
        const DevelBuildProvenanceInvalidReason reason =
            !is_valid_package_name(package_base_text)
                ? DevelBuildProvenanceInvalidReason::MalformedPackageBase
                : DevelBuildProvenanceInvalidReason::MalformedAurGitRemote;
        return invalid_document(
            reason,
            reason == DevelBuildProvenanceInvalidReason::MalformedPackageBase
                ? PACKAGE_BASE_KEY
                : AUR_GIT_REMOTE_KEY);
    }

    std::optional<AurRecipeRevision> recipe_revision;
    try {
        recipe_revision = AurRecipeRevision::git_commit(
            string_value(root, REVIEWED_RECIPE_OID_KEY));
    } catch(const std::invalid_argument&) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MalformedReviewedRecipeOid,
            REVIEWED_RECIPE_OID_KEY);
    }

    std::optional<ReviewedSourceStateDocumentSha256Digest>
        reviewed_document_digest;
    try {
        reviewed_document_digest =
            ReviewedSourceStateDocumentSha256Digest::make(
                string_value(root, REVIEWED_STATE_DOCUMENT_SHA256_KEY));
    } catch(const std::invalid_argument&) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MalformedDigest,
            REVIEWED_STATE_DOCUMENT_SHA256_KEY);
    }

    if(string_value(root, EVALUATED_VCS_KIND_KEY) != GIT_VCS_KIND) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::UnsupportedVcsKind,
            EVALUATED_VCS_KIND_KEY);
    }
    if(string_value(root, EVALUATED_ARCHITECTURE_SCOPE_KEY) !=
       INDEPENDENT_ARCHITECTURE_SCOPE) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::
                UnsupportedArchitectureScope,
            EVALUATED_ARCHITECTURE_SCOPE_KEY);
    }

    std::optional<VcsSelector> selector;
    const std::string selector_kind =
        string_value(root, EVALUATED_SELECTOR_KIND_KEY);
    const std::string selector_value =
        string_value(root, EVALUATED_SELECTOR_VALUE_KEY);
    try {
        if(selector_kind == DEFAULT_HEAD_SELECTOR) {
            if(!selector_value.empty()) {
                return invalid_document(
                    DevelBuildProvenanceInvalidReason::
                        MalformedSelectorValue,
                    EVALUATED_SELECTOR_VALUE_KEY);
            }
            selector = VcsSelector::default_head();
        } else if(selector_kind == BRANCH_SELECTOR) {
            selector = VcsSelector::branch(selector_value);
        } else {
            return invalid_document(
                DevelBuildProvenanceInvalidReason::
                    UnsupportedSelectorKind,
                EVALUATED_SELECTOR_KIND_KEY);
        }
    } catch(const std::invalid_argument&) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MalformedSelectorValue,
            EVALUATED_SELECTOR_VALUE_KEY);
    }

    const std::string evaluated_source_location =
        string_value(root, EVALUATED_SOURCE_LOCATION_KEY);
    if(!std::string_view(evaluated_source_location)
            .starts_with("https://") ||
       evaluated_source_location.size() <=
           std::string_view("https://").size()) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::
                MalformedEvaluatedSourceLocation,
            EVALUATED_SOURCE_LOCATION_KEY);
    }

    std::optional<VcsSourceIdentity> evaluated_source;
    try {
        evaluated_source = VcsSourceIdentity::make(
            VcsKind::Git, evaluated_source_location,
            std::move(*selector));
    } catch(const std::invalid_argument&) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::
                MalformedEvaluatedSourceLocation,
            EVALUATED_SOURCE_LOCATION_KEY);
    }

    std::optional<ActualBuiltGitRevision> actual_revision;
    try {
        actual_revision = ActualBuiltGitRevision(
            UpstreamGitRevision::git_commit(
                *evaluated_source,
                string_value(root, ACTUAL_BUILT_GIT_OID_KEY)));
    } catch(const std::invalid_argument&) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MalformedActualBuiltOid,
            ACTUAL_BUILT_GIT_OID_KEY);
    }

    std::optional<PackageArchiveSha256Digest> archive_digest;
    std::optional<AlpmMtreeSha256Digest> artifact_mtree_digest;
    std::optional<AlpmMtreeSha256Digest> installed_mtree_digest;
    std::optional<InstalledDatabaseRecordSha256Digest> database_digest;
    try {
        archive_digest = PackageArchiveSha256Digest::make(
            string_value(root, ARTIFACT_ARCHIVE_SHA256_KEY));
        artifact_mtree_digest = AlpmMtreeSha256Digest::make(
            string_value(root, ARTIFACT_MTREE_SHA256_KEY));
        installed_mtree_digest = AlpmMtreeSha256Digest::make(
            string_value(root, INSTALLED_MTREE_SHA256_KEY));
        database_digest = InstalledDatabaseRecordSha256Digest::make(
            string_value(root, INSTALLED_DATABASE_RECORD_SHA256_KEY));
    } catch(const std::invalid_argument&) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MalformedDigest);
    }

    if(string_value(root, INSTALLED_RECORD_GENERATION_SCHEME_KEY) !=
       LINUX_NAME_TO_HANDLE_AT_SCHEME) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::
                UnsupportedInstalledGenerationScheme,
            INSTALLED_RECORD_GENERATION_SCHEME_KEY);
    }
    const std::string installed_generation_identity =
        string_value(root, INSTALLED_RECORD_GENERATION_IDENTITY_KEY);
    if(!is_nonempty_token(installed_generation_identity)) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::
                MalformedInstalledGenerationIdentity,
            INSTALLED_RECORD_GENERATION_IDENTITY_KEY);
    }

    const std::string artifact_child = string_value(root, ARTIFACT_CHILD_KEY);
    const std::string artifact_package_base =
        string_value(root, ARTIFACT_PACKAGE_BASE_KEY);
    const std::string artifact_version =
        string_value(root, ARTIFACT_FULL_VERSION_KEY);
    const std::string artifact_architecture =
        string_value(root, ARTIFACT_ARCHITECTURE_KEY);
    const std::string installed_child =
        string_value(root, INSTALLED_CHILD_KEY);
    const std::string installed_package_base =
        string_value(root, INSTALLED_PACKAGE_BASE_KEY);
    const std::string installed_version =
        string_value(root, INSTALLED_FULL_VERSION_KEY);
    const std::string installed_architecture =
        string_value(root, INSTALLED_ARCHITECTURE_KEY);
    if(!is_valid_package_name(artifact_child) ||
       !is_valid_package_name(artifact_package_base) ||
       !is_valid_package_name(installed_child) ||
       !is_valid_package_name(installed_package_base) ||
       !is_nonempty_token(artifact_version) ||
       !is_nonempty_token(artifact_architecture) ||
       !is_nonempty_token(installed_version) ||
       !is_nonempty_token(installed_architecture)) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MalformedArtifactIdentity);
    }

    ReviewedSourceStateRecordBinding reviewed_binding(
        *package_base, std::move(*recipe_revision),
        ReviewedSourceStateRecordGeneration(*reviewed_generation),
        std::move(*reviewed_document_digest));

    const BuiltPackageArtifactEvidence artifact{
        ArtifactPackageIdentity{
            artifact_child, artifact_version,
            ArtifactPackageBaseIdentity::known(artifact_package_base),
            ArtifactPackageArchitectureIdentity::known(
                artifact_architecture)},
        std::move(*archive_digest), std::move(*artifact_mtree_digest)};

    std::optional<InstalledArtifactBinding> installed_binding;
    try {
        InstalledPackageRecordGeneration installed_generation(
            InstalledPackageRecordGenerationScheme::LinuxNameToHandleAt,
            installed_generation_identity);
        installed_binding = InstalledArtifactBinding::make(
            PackageChildIdentity::make(
                PackageBaseIdentity::make(
                    package_base->source(), installed_package_base),
                installed_child),
            PackageVersionIdentity::composite(installed_version),
            InstalledPackageArchitectureIdentity::known(
                installed_architecture),
            std::move(*installed_mtree_digest),
            std::move(*database_digest), std::move(installed_generation));
    } catch(const std::invalid_argument&) {
        return invalid_document(
            DevelBuildProvenanceInvalidReason::MalformedArtifactIdentity);
    }

    DevelBuildProvenanceResult provenance = make_devel_build_provenance(
        *package_base, std::move(reviewed_binding),
        std::move(*evaluated_source), std::move(*actual_revision), artifact,
        std::move(*installed_binding));
    if(auto* loaded = std::get_if<DevelBuildProvenance>(&provenance)) {
        return DevelBuildProvenanceDecoded{std::move(*loaded)};
    }
    return invalid_document(
        DevelBuildProvenanceInvalidReason::InconsistentProvenance);
}

DevelBuildProvenanceDocument decode_devel_build_provenance(
    std::string_view document) {
    return DevelBuildProvenancePersistentDecoderAccess::decode_document(
        document);
}

std::string encode_devel_build_provenance(
    const DevelBuildProvenance& provenance) {
    const PackageBaseIdentity& package_base = provenance.package_base();
    const std::string* aur_remote = package_base.source().location().value();
    const ReviewedSourceStateRecordBinding& reviewed =
        provenance.reviewed_source_binding();
    const std::string* reviewed_oid =
        reviewed.reviewed_recipe_revision().value().git_commit();
    const VcsSourceIdentity& evaluated = provenance.evaluated_source();
    const VcsSelector& selector = evaluated.selector();
    const std::string* selector_value = selector.value();
    const std::string* actual_oid =
        provenance.actual_built_revision().revision().value().git_commit();
    const BuiltPackageArtifactEvidence& artifact = provenance.artifact();
    const std::string* artifact_package_base =
        artifact.identity.package_base.value();
    const std::string* artifact_architecture =
        artifact.identity.architecture.value();
    const InstalledArtifactBinding& installed =
        provenance.installed_binding();
    const std::string* installed_version = installed.version().full_version();
    const std::string* installed_architecture =
        installed.architecture().value();
    if(aur_remote == nullptr || reviewed_oid == nullptr ||
       actual_oid == nullptr || artifact_package_base == nullptr ||
       artifact_architecture == nullptr || installed_version == nullptr ||
       installed_architecture == nullptr ||
       reviewed.generation().value() == 0 ||
       evaluated.kind() != VcsKind::Git ||
       evaluated.architecture() != nullptr ||
       installed.record_generation().scheme() !=
           InstalledPackageRecordGenerationScheme::LinuxNameToHandleAt) {
        throw std::logic_error(
            "Devel build provenance lost a persistent identity field.");
    }

    std::string selector_kind;
    std::string selector_text;
    if(selector.kind() == VcsSelectorKind::DefaultHead &&
       selector_value == nullptr) {
        selector_kind = DEFAULT_HEAD_SELECTOR;
    } else if(selector.kind() == VcsSelectorKind::Branch &&
              selector_value != nullptr) {
        selector_kind = BRANCH_SELECTOR;
        selector_text = *selector_value;
    } else {
        throw std::logic_error(
            "Devel build provenance has an unsupported selector.");
    }

    std::string document;
    document += "schema_version = ";
    document += std::to_string(devel_build_provenance_schema_version);
    document += '\n';
    append_string(document, SOURCE_KIND_KEY, AUR_SOURCE_KIND);
    append_string(document, PACKAGE_BASE_KEY, package_base.package_base());
    append_string(document, AUR_GIT_REMOTE_KEY, *aur_remote);
    append_string(document, REVIEWED_RECIPE_OID_KEY, *reviewed_oid);
    append_string(
        document, REVIEWED_STATE_GENERATION_KEY,
        std::to_string(reviewed.generation().value()));
    append_string(
        document, REVIEWED_STATE_DOCUMENT_SHA256_KEY,
        reviewed.document_digest().value());
    append_string(document, EVALUATED_VCS_KIND_KEY, GIT_VCS_KIND);
    append_string(
        document, EVALUATED_SOURCE_LOCATION_KEY,
        evaluated.source_location());
    append_string(document, EVALUATED_SELECTOR_KIND_KEY, selector_kind);
    append_string(document, EVALUATED_SELECTOR_VALUE_KEY, selector_text);
    append_string(
        document, EVALUATED_ARCHITECTURE_SCOPE_KEY,
        INDEPENDENT_ARCHITECTURE_SCOPE);
    append_string(document, ACTUAL_BUILT_GIT_OID_KEY, *actual_oid);
    append_string(document, ARTIFACT_CHILD_KEY, artifact.identity.package_name);
    append_string(
        document, ARTIFACT_PACKAGE_BASE_KEY, *artifact_package_base);
    append_string(
        document, ARTIFACT_FULL_VERSION_KEY,
        artifact.identity.full_version);
    append_string(
        document, ARTIFACT_ARCHITECTURE_KEY, *artifact_architecture);
    append_string(
        document, ARTIFACT_ARCHIVE_SHA256_KEY,
        artifact.archive_digest.value());
    append_string(
        document, ARTIFACT_MTREE_SHA256_KEY,
        artifact.mtree_digest.value());
    append_string(
        document, INSTALLED_CHILD_KEY, installed.package().package_name());
    append_string(
        document, INSTALLED_PACKAGE_BASE_KEY,
        installed.package().package_base().package_base());
    append_string(
        document, INSTALLED_FULL_VERSION_KEY, *installed_version);
    append_string(
        document, INSTALLED_ARCHITECTURE_KEY, *installed_architecture);
    append_string(
        document, INSTALLED_MTREE_SHA256_KEY,
        installed.mtree_digest().value());
    append_string(
        document, INSTALLED_DATABASE_RECORD_SHA256_KEY,
        installed.database_record_digest().value());
    append_string(
        document, INSTALLED_RECORD_GENERATION_SCHEME_KEY,
        LINUX_NAME_TO_HANDLE_AT_SCHEME);
    append_string(
        document, INSTALLED_RECORD_GENERATION_IDENTITY_KEY,
        installed.record_generation().opaque_identity());
    return document;
}

DevelBuildProvenanceInterpretation interpret_devel_build_provenance(
    std::string_view document,
    const PackageBaseIdentity& expected_package_base) {
    DevelBuildProvenanceDocument decoded =
        decode_devel_build_provenance(document);
    if(auto* loaded = std::get_if<DevelBuildProvenanceDecoded>(&decoded)) {
        if(loaded->provenance.package_base().source() !=
           expected_package_base.source()) {
            return DevelBuildProvenanceSourceMismatch{
                std::move(loaded->provenance), expected_package_base,
                DevelBuildProvenanceSourceMismatchReason::
                    SourceIdentityMismatch};
        }
        if(loaded->provenance.package_base().package_base() !=
           expected_package_base.package_base()) {
            return DevelBuildProvenanceSourceMismatch{
                std::move(loaded->provenance), expected_package_base,
                DevelBuildProvenanceSourceMismatchReason::
                    PackageBaseMismatch};
        }
        return std::move(*loaded);
    }
    if(auto* invalid =
           std::get_if<DevelBuildProvenanceInvalidDocument>(&decoded)) {
        return std::move(*invalid);
    }
    if(auto* corrupt =
           std::get_if<DevelBuildProvenanceCorruptDocument>(&decoded)) {
        return std::move(*corrupt);
    }
    return std::move(std::get<DevelBuildProvenanceFutureSchema>(decoded));
}
