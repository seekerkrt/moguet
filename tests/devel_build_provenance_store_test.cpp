#include "devel_build_provenance_store.hpp"

#include "xdg_paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace allocation_fault {
bool blocked = false;
std::size_t failures = 0;
} // namespace allocation_fault

// Preserve the replacement boundary under optimized fixture compilation.
[[gnu::noinline]] void* operator new(std::size_t size) {
    if(allocation_fault::blocked) {
        ++allocation_fault::failures;
        throw std::bad_alloc();
    }
    if(void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* memory) noexcept {
    std::free(memory);
}
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace fs = std::filesystem;

namespace {

constexpr std::string_view SHA1_RECIPE =
    "1111111111111111111111111111111111111111";
constexpr std::string_view SHA1_BUILT =
    "2222222222222222222222222222222222222222";
constexpr std::string_view SHA1_OTHER =
    "3333333333333333333333333333333333333333";
constexpr std::string_view SHA256_RECIPE =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view SHA256_BUILT =
    "2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view REVIEWED_DOCUMENT_DIGEST =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view ARCHIVE_DIGEST =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view MTREE_DIGEST =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr std::string_view DATABASE_DIGEST =
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
constexpr std::string_view AUR_REMOTE =
    "https://aur.archlinux.org/moguet-provenance-fixture.git";
constexpr std::string_view UPSTREAM_REMOTE =
    "https://example.invalid/moguet-provenance-upstream.git";

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string pattern =
            (fs::temp_directory_path() / "moguet-provenance-store-XXXXXX")
                .string();
        if(::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = std::move(pattern);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(std::string name, const std::string& value)
        : name_(std::move(name)) {
        if(const char* previous = std::getenv(name_.c_str());
           previous != nullptr) {
            previous_ = previous;
        }
        if(::setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("setenv failed");
        }
    }

    ~ScopedEnvironmentVariable() {
        if(previous_.has_value()) {
            static_cast<void>(
                ::setenv(name_.c_str(), previous_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name_.c_str()));
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

struct StoreHome {
    TemporaryDirectory temporary;
    ScopedEnvironmentVariable state_home;
    ScopedEnvironmentVariable home;

    StoreHome()
        : state_home(
              "XDG_STATE_HOME",
              (temporary.path() / "state").string()),
          home("HOME", (temporary.path() / "home").string()) {
        fs::create_directory(temporary.path() / "state");
        fs::create_directory(temporary.path() / "home");
        reset_xdg_generation_store_test_hooks();
    }
};

void require(bool condition, std::string_view message) {
    if(!condition) throw std::runtime_error(std::string(message));
}

template <typename Arm, typename Variant>
const Arm& require_arm(const Variant& value, std::string_view message) {
    const Arm* arm = std::get_if<Arm>(&value);
    if(arm == nullptr) throw std::runtime_error(std::string(message));
    return *arm;
}

PackageBaseIdentity aur_package_base(
    std::string package_base = "moguet-provenance-fixture",
    std::string remote = std::string(AUR_REMOTE)) {
    return PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(std::move(remote))),
        std::move(package_base));
}

VcsSourceIdentity upstream_source() {
    return VcsSourceIdentity::make(
        VcsKind::Git, std::string(UPSTREAM_REMOTE),
        VcsSelector::default_head());
}

ActualBuiltGitRevision actual_revision(
    const VcsSourceIdentity& source,
    std::string oid) {
    ActualBuiltGitRevisionProofResult result =
        prove_actual_built_git_revision(
            make_makepkg_git_workspace_revision_observation_fixture_for_test(
                UpstreamGitRevision::git_commit(source, oid),
                UpstreamGitRevision::git_commit(source, oid)));
    const auto* revision = std::get_if<ActualBuiltGitRevision>(&result);
    if(revision == nullptr) throw std::runtime_error("actual proof failed");
    return *revision;
}

DevelBuildProvenance provenance(
    PackageBaseIdentity package_base = aur_package_base(),
    std::string built_oid = std::string(SHA1_BUILT),
    std::string recipe_oid = std::string(SHA1_RECIPE),
    std::uint64_t reviewed_generation = 9) {
    const VcsSourceIdentity source = upstream_source();
    const std::string package_base_name = package_base.package_base();
    ReviewedSourceStateRecordBinding reviewed =
        make_reviewed_source_state_record_binding_fixture_for_test(
            package_base, AurRecipeRevision::git_commit(std::move(recipe_oid)),
            reviewed_generation,
            ReviewedSourceStateDocumentSha256Digest::make(
                std::string(REVIEWED_DOCUMENT_DIGEST)));
    InstalledArtifactBinding installed =
        make_installed_artifact_binding_fixture_for_test(
            PackageChildIdentity::make(
                package_base, "moguet-provenance-fixture"),
            PackageVersionIdentity::composite("1.r2.g222222222222-1"),
            InstalledPackageArchitectureIdentity::known("any"),
            AlpmMtreeSha256Digest::make(std::string(MTREE_DIGEST)),
            InstalledDatabaseRecordSha256Digest::make(
                std::string(DATABASE_DIGEST)),
            make_installed_package_record_generation_fixture_for_test(
                "ext4:opaque-generation"));
    BuiltPackageArtifactEvidence artifact{
        ArtifactPackageIdentity{
            "moguet-provenance-fixture", "1.r2.g222222222222-1",
            ArtifactPackageBaseIdentity::known(
                package_base_name),
            ArtifactPackageArchitectureIdentity::known("any")},
        PackageArchiveSha256Digest::make(std::string(ARCHIVE_DIGEST)),
        AlpmMtreeSha256Digest::make(std::string(MTREE_DIGEST))};
    DevelBuildProvenanceResult result = make_devel_build_provenance(
        std::move(package_base), std::move(reviewed), source,
        actual_revision(source, std::move(built_oid)), std::move(artifact),
        std::move(installed));
    auto* loaded = std::get_if<DevelBuildProvenance>(&result);
    if(loaded == nullptr) throw std::runtime_error("provenance mint failed");
    return std::move(*loaded);
}

void replace_once(
    std::string& document,
    std::string_view before,
    std::string_view after) {
    const std::size_t position = document.find(before);
    if(position == std::string::npos ||
       document.find(before, position + before.size()) != std::string::npos) {
        throw std::runtime_error("replacement fixture is not unique");
    }
    document.replace(position, before.size(), after);
}

void write_bytes(const fs::path& path, std::string_view bytes, mode_t mode) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) throw std::runtime_error("fixture open failed");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if(!output || ::chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error("fixture write failed");
    }
}

void test_codec_roundtrip_and_strict_failures() {
    const DevelBuildProvenance sha1 = provenance();
    const std::string encoded = encode_devel_build_provenance(sha1);
    require(encoded == encode_devel_build_provenance(sha1),
            "codec is not deterministic");
    const DevelBuildProvenanceDocument decoded_document =
        decode_devel_build_provenance(encoded);
    const auto& decoded = require_arm<DevelBuildProvenanceDecoded>(
        decoded_document,
        "valid SHA-1 provenance did not decode");
    require(decoded.provenance == sha1 &&
                decoded.provenance.reviewed_source_binding()
                        .generation()
                        .value() == 9 &&
                decoded.provenance.reviewed_source_binding()
                        .document_digest()
                        .value() == REVIEWED_DOCUMENT_DIGEST,
            "semantic roundtrip or exact #411 binding drifted");

    const DevelBuildProvenance sha256 = provenance(
        aur_package_base(), std::string(SHA256_BUILT),
        std::string(SHA256_RECIPE),
        std::numeric_limits<std::uint64_t>::max());
    require(require_arm<DevelBuildProvenanceDecoded>(
                decode_devel_build_provenance(
                    encode_devel_build_provenance(sha256)),
                "valid SHA-256 provenance did not decode")
                    .provenance == sha256,
            "SHA-256 roundtrip drifted");

    std::string invalid_oid = encoded;
    replace_once(invalid_oid, SHA1_BUILT, "short");
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(invalid_oid),
        "invalid actual OID was accepted");

    std::string invalid_digest = encoded;
    replace_once(invalid_digest, ARCHIVE_DIGEST, "short");
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(invalid_digest),
        "invalid SHA-256 digest was accepted");

    std::string installed_base_mismatch = encoded;
    replace_once(
        installed_base_mismatch,
        "installed_package_base = \"moguet-provenance-fixture\"",
        "installed_package_base = \"different-base\"");
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(installed_base_mismatch),
        "installed PackageBase mismatch was inferred from upper context");

    std::string invalid_source = encoded;
    replace_once(invalid_source, "source_kind = \"aur\"",
                 "source_kind = \"repository\"");
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(invalid_source),
        "invalid source kind was accepted");

    std::string invalid_location = encoded;
    replace_once(
        invalid_location,
        "https://example.invalid/moguet-provenance-upstream.git",
        "http://example.invalid/moguet-provenance-upstream.git");
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(invalid_location),
        "non-HTTPS evaluated source was accepted");

    std::string duplicate = encoded;
    duplicate += "artifact_child = \"duplicate\"\n";
    require_arm<DevelBuildProvenanceCorruptDocument>(
        decode_devel_build_provenance(duplicate),
        "duplicate field was accepted");

    std::string unknown = encoded;
    unknown += "unknown_field = \"x\"\n";
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(unknown),
        "unknown field was accepted");

    std::string missing = encoded;
    replace_once(
        missing,
        "evaluated_source_location = \"https://example.invalid/moguet-provenance-upstream.git\"\n",
        "");
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(missing),
        "missing field was inferred");

    std::string invalid_generation = encoded;
    replace_once(
        invalid_generation, "reviewed_state_generation = \"9\"",
        "reviewed_state_generation = \"0\"");
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(invalid_generation),
        "invalid #411 generation was accepted");

    std::string untyped_generation = encoded;
    replace_once(
        untyped_generation, "reviewed_state_generation = \"9\"",
        "reviewed_state_generation = 9");
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(untyped_generation),
        "non-string #411 generation representation was accepted");

    std::string invalid_selector = encoded;
    replace_once(
        invalid_selector, "evaluated_selector_kind = \"default-head\"",
        "evaluated_selector_kind = \"tag\"");
    require_arm<DevelBuildProvenanceInvalidDocument>(
        decode_devel_build_provenance(invalid_selector),
        "unsupported selector enum was accepted");

    require_arm<DevelBuildProvenanceCorruptDocument>(
        decode_devel_build_provenance(
            encoded.substr(0, encoded.size() - 2)),
        "truncated document was accepted");

    std::string future = encoded;
    replace_once(future, "schema_version = 1", "schema_version = 2");
    const DevelBuildProvenanceDocument future_document =
        decode_devel_build_provenance(future);
    const auto& future_result = require_arm<DevelBuildProvenanceFutureSchema>(
        future_document,
        "future schema was parsed as current");
    require(future_result.schema_version == 2,
            "future schema version was lost");
}

void test_store_read_publish_cas_and_namespaces() {
    StoreHome home;
    const PackageBaseIdentity package_base = aur_package_base();
    require_arm<DevelBuildProvenanceStoreMissing>(
        read_devel_build_provenance(package_base),
        "missing provenance was not Missing");
    require(!fs::exists(devel_build_provenance_store_directory()),
            "read-only provenance lookup created its namespace");

    const DevelBuildProvenance first_value = provenance(package_base);
    const auto first = require_arm<DevelBuildProvenanceStorePublished>(
        publish_devel_build_provenance(first_value, std::nullopt),
        "first provenance publication failed");
    require(first.observed.generation == 1,
            "first provenance generation was not store-owned generation 1");
    const auto loaded = require_arm<DevelBuildProvenanceStoreLoaded>(
        read_devel_build_provenance(package_base),
        "published provenance was not Loaded");
    require(loaded.provenance == first_value &&
                loaded.observed == first.observed,
            "loaded provenance/token drifted");

    // Explicit predecessor CAS owns this transition. Equal semantic payload is
    // not implicitly treated as idempotent publication in Slice 2.
    const auto same = require_arm<DevelBuildProvenanceStorePublished>(
        publish_devel_build_provenance(first_value, first.observed),
        "exact same payload CAS failed");
    require(same.observed.generation == 2,
            "same payload was silently made idempotent");

    const DevelBuildProvenance replacement = provenance(
        package_base, std::string(SHA1_OTHER));
    const auto replaced = require_arm<DevelBuildProvenanceStorePublished>(
        publish_devel_build_provenance(replacement, same.observed),
        "replacement provenance publication failed");
    require(replaced.observed.generation == 3,
            "replacement generation did not advance");
    require_arm<DevelBuildProvenanceStoreCasConflict>(
        publish_devel_build_provenance(first_value, first.observed),
        "stale provenance writer did not conflict");
    require(require_arm<DevelBuildProvenanceStoreLoaded>(
                read_devel_build_provenance(package_base),
                "post-conflict provenance was not loaded")
                    .provenance == replacement,
            "stale writer rolled back current provenance");

    const fs::path reviewed_source_directory =
        xdg_paths::resolve_reviewed_source_state_process_environment()
            .directory;
    require(devel_build_provenance_store_directory() !=
                reviewed_source_directory,
            "provenance and #411 share an XDG namespace");
    require(!fs::exists(reviewed_source_directory),
            "provenance publication created the #411 namespace");
}

void test_invalid_mismatch_and_future_are_not_missing_or_rebound() {
    const auto prepare_origin = [](StoreHome& home) {
        static_cast<void>(home);
        const PackageBaseIdentity package_base = aur_package_base();
        const DevelBuildProvenance value = provenance(package_base);
        require_arm<DevelBuildProvenanceStorePublished>(
            publish_devel_build_provenance(value, std::nullopt),
            "fixture seed failed");
        return std::pair{package_base, value};
    };

    {
        StoreHome home;
        auto [package_base, value] = prepare_origin(home);
        const fs::path origin =
            devel_build_provenance_store_entry_path(package_base) / "1.toml";
        write_bytes(origin, "not toml = \"", 0600);
        const auto corrupt =
            require_arm<DevelBuildProvenanceStoreCorruptRecord>(
                read_devel_build_provenance(package_base),
                "corrupt record was flattened");
        require_arm<DevelBuildProvenanceStoreOverwriteRefused>(
            publish_devel_build_provenance(value, corrupt.observed),
            "corrupt record was automatically rebound");
    }

    {
        StoreHome home;
        auto [package_base, value] = prepare_origin(home);
        const fs::path origin =
            devel_build_provenance_store_entry_path(package_base) / "1.toml";
        std::string invalid = encode_devel_build_provenance(value);
        invalid += "unknown_field = \"x\"\n";
        write_bytes(origin, invalid, 0600);
        const auto invalid_read =
            require_arm<DevelBuildProvenanceStoreInvalidDocument>(
                read_devel_build_provenance(package_base),
                "invalid record was flattened");
        require_arm<DevelBuildProvenanceStoreOverwriteRefused>(
            publish_devel_build_provenance(value, invalid_read.observed),
            "invalid record was automatically rebound");
    }

    {
        StoreHome home;
        auto [package_base, value] = prepare_origin(home);
        const fs::path origin =
            devel_build_provenance_store_entry_path(package_base) / "1.toml";
        write_bytes(
            origin,
            encode_devel_build_provenance(
                provenance(aur_package_base("different-base"))),
            0600);
        require_arm<DevelBuildProvenanceStorePackageBaseMismatch>(
            read_devel_build_provenance(package_base),
            "PackageBase mismatch was flattened");
        static_cast<void>(value);
    }

    {
        StoreHome home;
        auto [package_base, value] = prepare_origin(home);
        const fs::path origin =
            devel_build_provenance_store_entry_path(package_base) / "1.toml";
        write_bytes(
            origin,
            encode_devel_build_provenance(provenance(aur_package_base(
                "moguet-provenance-fixture",
                "https://aur.archlinux.org/different.git"))),
            0600);
        require_arm<DevelBuildProvenanceStoreSourceMismatch>(
            read_devel_build_provenance(package_base),
            "source mismatch was flattened");
        static_cast<void>(value);
    }

    {
        StoreHome home;
        auto [package_base, value] = prepare_origin(home);
        const fs::path origin =
            devel_build_provenance_store_entry_path(package_base) / "1.toml";
        std::string future = encode_devel_build_provenance(value);
        replace_once(future, "schema_version = 1", "schema_version = 2");
        write_bytes(origin, future, 0600);
        const auto future_read =
            require_arm<DevelBuildProvenanceStoreFutureSchema>(
                read_devel_build_provenance(package_base),
                "future record was flattened");
        require_arm<DevelBuildProvenanceStoreFutureSchemaOverwriteRefused>(
            publish_devel_build_provenance(value, future_read.observed),
            "future record was downgraded");
        std::ifstream input(origin, std::ios::binary);
        const std::string retained{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        require(retained == future,
                "future-schema refusal changed the record");
    }
}

void test_unsafe_files_and_published_uncertainty() {
    {
        StoreHome home;
        const PackageBaseIdentity package_base = aur_package_base();
        const DevelBuildProvenance value = provenance(package_base);
        require_arm<DevelBuildProvenanceStorePublished>(
            publish_devel_build_provenance(value, std::nullopt),
            "mode seed failed");
        const fs::path origin =
            devel_build_provenance_store_entry_path(package_base) / "1.toml";
        require(::chmod(origin.c_str(), 0644) == 0, "chmod failed");
        const auto mode_failure =
            require_arm<DevelBuildProvenanceStoreFailure>(
                read_devel_build_provenance(package_base),
                "unsafe file mode was flattened");
        require(mode_failure.store_failure.kind ==
                    XdgGenerationStoreFailureKind::UnsafePermissions,
                "unsafe provenance mode failure kind drifted");
    }

    {
        StoreHome home;
        const PackageBaseIdentity package_base = aur_package_base();
        const DevelBuildProvenance value = provenance(package_base);
        require_arm<DevelBuildProvenanceStorePublished>(
            publish_devel_build_provenance(value, std::nullopt),
            "hardlink seed failed");
        const fs::path origin =
            devel_build_provenance_store_entry_path(package_base) / "1.toml";
        require(::link(
                    origin.c_str(),
                    (home.temporary.path() / "alias").c_str()) == 0,
                "hardlink fixture failed");
        require_arm<DevelBuildProvenanceStoreFailure>(
            read_devel_build_provenance(package_base),
            "hardlink was flattened");
    }

    {
        StoreHome home;
        const PackageBaseIdentity package_base = aur_package_base();
        const DevelBuildProvenance value = provenance(package_base);
        fail_next_xdg_generation_store_operation_for_test(
            XdgGenerationStoreTestFailurePoint::DirectorySync);
        const auto uncertain =
            require_arm<DevelBuildProvenanceStorePublishedUncertain>(
                publish_devel_build_provenance(value, std::nullopt),
                "post-commit uncertainty was flattened");
        require(uncertain.store_result.observed.has_value() &&
                    uncertain.store_result.issue ==
                        XdgGenerationPostPublicationIssue::
                            DirectorySyncUncertain,
                "published uncertainty lost its committed record");
    }
}

void test_authority_unavailable_is_not_missing() {
    TemporaryDirectory temporary;
    ScopedEnvironmentVariable state_home(
        "XDG_STATE_HOME", "relative/state");
    ScopedEnvironmentVariable home(
        "HOME", (temporary.path() / "home").string());
    require_arm<DevelBuildProvenanceStoreAuthorityUnavailable>(
        read_devel_build_provenance(aur_package_base()),
        "unavailable XDG authority was flattened to Missing");
}


void block_after_verified_publication(const XdgGenerationStoreTestRaceContext&) {
    allocation_fault::blocked = true;
}

void test_publication_resource_boundary() {
    StoreHome home;
    const auto value = provenance();
    allocation_fault::blocked = true;
    std::optional<DevelBuildProvenanceStorePublishResult> result;
    try {
        result.emplace(publish_devel_build_provenance(value, std::nullopt));
    } catch(...) {
        allocation_fault::blocked = false;
        throw;
    }
    allocation_fault::blocked = false;
    require(require_arm<DevelBuildProvenanceStoreFailure>(*result, "wrapper allocation escaped").store_failure.kind ==
                XdgGenerationStoreFailureKind::ResourceFailure,
            "wrapper resource cause lost");
    require(!fs::exists(devel_build_provenance_store_directory()), "pre-write wrapper failure created namespace");

    run_xdg_generation_store_race_once_for_test(XdgGenerationStoreTestRacePoint::AfterVerifiedPublication,
                                                &block_after_verified_publication);
    allocation_fault::failures = 0;
    try {
        result.emplace(publish_devel_build_provenance(value, std::nullopt));
    } catch(...) {
        allocation_fault::blocked = false;
        throw;
    }
    const bool remained_blocked = allocation_fault::blocked;
    allocation_fault::blocked = false;
    const auto& published = require_arm<DevelBuildProvenanceStorePublished>(*result, "verified low-level success disappeared in wrapper");
    require(remained_blocked && allocation_fault::failures == 0 && published.provenance == value,
            "wrapper allocated after verified low-level success");
    require(require_arm<DevelBuildProvenanceStoreLoaded>(read_devel_build_provenance(value.package_base()),
                                                         "verified wrapper result did not persist")
                    .observed == published.observed,
            "wrapper observed record changed");
    std::cout << "semantic publication terminal allocation firewall PASS\n";
}

} // namespace

int main() {
    try {
        test_publication_resource_boundary();
        test_codec_roundtrip_and_strict_failures();
        test_store_read_publish_cas_and_namespaces();
        test_invalid_mismatch_and_future_are_not_missing_or_rebound();
        test_unsafe_files_and_published_uncertainty();
        test_authority_unavailable_is_not_missing();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "devel build provenance store tests passed\n";
    return 0;
}
