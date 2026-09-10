#include "installed_artifact_binding.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

constexpr const char* DIGEST_A =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* DIGEST_B =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* DIGEST_C =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr const char* AUR_REMOTE =
    "https://aur.archlinux.org/moguet-binding-fixture.git";

static_assert(!std::is_default_constructible_v<
              InstalledPackageRecordGeneration>);
static_assert(!std::is_constructible_v<
              InstalledPackageRecordGeneration,
              std::string>);
static_assert(!std::is_constructible_v<
              InstalledPackageRecordGeneration,
              std::uint64_t>);
static_assert(!std::is_default_constructible_v<InstalledArtifactBinding>);
static_assert(!std::is_constructible_v<
              InstalledArtifactBinding,
              PackageChildIdentity,
              PackageVersionIdentity,
              InstalledPackageArchitectureIdentity,
              AlpmMtreeSha256Digest,
              InstalledDatabaseRecordSha256Digest,
              InstalledPackageRecordGeneration>);
static_assert(std::variant_size_v<
                  InstalledArtifactBindingObservationResult> == 2);

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void require_invalid_argument(
    const std::function<void()>& operation,
    const std::string& message) {
    try {
        operation();
    } catch(const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}

PackageChildIdentity package(
    std::string package_name = "moguet-binding-fixture",
    std::string package_base = "moguet-binding-fixture",
    std::string remote = AUR_REMOTE) {
    return PackageChildIdentity::make(
        PackageBaseIdentity::make(
            PackageSourceIdentity::aur(
                SourceLocationIdentity::known_git_remote(
                    std::move(remote))),
            std::move(package_base)),
        std::move(package_name));
}

InstalledArtifactBinding binding(
    std::string generation = "ext4:fixture-generation-a",
    std::string package_name = "moguet-binding-fixture",
    std::string package_base = "moguet-binding-fixture",
    std::string version = "1.r2.g0123456789ab-1",
    std::string architecture = "any",
    std::string mtree_digest = DIGEST_A,
    std::string database_digest = DIGEST_B,
    std::string remote = AUR_REMOTE) {
    return make_installed_artifact_binding_fixture_for_test(
        package(
            std::move(package_name), std::move(package_base),
            std::move(remote)),
        PackageVersionIdentity::composite(std::move(version)),
        InstalledPackageArchitectureIdentity::known(
            std::move(architecture)),
        AlpmMtreeSha256Digest::make(std::move(mtree_digest)),
        InstalledDatabaseRecordSha256Digest::make(
            std::move(database_digest)),
        make_installed_package_record_generation_fixture_for_test(
            std::move(generation)));
}

void expect_mismatch(
    const InstalledArtifactBinding& expected,
    const InstalledArtifactBinding& current,
    InstalledArtifactBindingMismatchReason expected_reason,
    const std::string& message) {
    const InstalledArtifactBindingComparisonResult result =
        compare_installed_artifact_binding(expected, current);
    const auto* mismatch =
        std::get_if<InstalledArtifactBindingMismatch>(&result);
    require(
        mismatch != nullptr && mismatch->reason == expected_reason,
        message);
}

void test_binding_retains_distinct_evidence() {
    const InstalledArtifactBinding observed = binding();
    require(
        observed.package().package_name() == "moguet-binding-fixture" &&
            observed.package().package_base().package_base() ==
                "moguet-binding-fixture",
        "Installed child/PackageBase identity was flattened.");
    require(
        observed.version().full_version() != nullptr &&
            *observed.version().full_version() ==
                "1.r2.g0123456789ab-1",
        "Installed full version was not retained.");
    require(
        observed.architecture().value() != nullptr &&
            *observed.architecture().value() == "any",
        "Installed architecture was not retained.");
    require(
        observed.mtree_digest().value() == DIGEST_A &&
            observed.database_record_digest().value() == DIGEST_B,
        "MTREE and local database digests were flattened.");
    require(
        observed.record_generation().scheme() ==
                InstalledPackageRecordGenerationScheme::
                    LinuxNameToHandleAt &&
            observed.record_generation().opaque_identity() ==
                "ext4:fixture-generation-a",
        "Opaque installed record generation was not retained.");
}

void test_comparison_taxonomy() {
    const InstalledArtifactBinding expected = binding();
    require(
        std::holds_alternative<InstalledArtifactBindingMatch>(
            compare_installed_artifact_binding(expected, expected)),
        "Equal installed bindings did not match.");

    expect_mismatch(
        expected, binding("ext4:fixture-generation-a", "different-child"),
        InstalledArtifactBindingMismatchReason::PackageIdentityMismatch,
        "Package child mismatch was not typed.");
    expect_mismatch(
        expected, binding("ext4:fixture-generation-a", "moguet-binding-fixture", "different-base"),
        InstalledArtifactBindingMismatchReason::PackageBaseMismatch,
        "PackageBase mismatch was not typed.");
    expect_mismatch(
        expected,
        binding(
            "ext4:fixture-generation-a", "moguet-binding-fixture",
            "moguet-binding-fixture", "1.r3.gabcdef012345-1"),
        InstalledArtifactBindingMismatchReason::VersionMismatch,
        "Version mismatch was not typed.");
    expect_mismatch(
        expected,
        binding(
            "ext4:fixture-generation-a", "moguet-binding-fixture",
            "moguet-binding-fixture", "1.r2.g0123456789ab-1",
            "x86_64"),
        InstalledArtifactBindingMismatchReason::ArchitectureMismatch,
        "Architecture mismatch was not typed.");
    expect_mismatch(
        expected,
        binding(
            "ext4:fixture-generation-a", "moguet-binding-fixture",
            "moguet-binding-fixture", "1.r2.g0123456789ab-1", "any",
            DIGEST_C),
        InstalledArtifactBindingMismatchReason::MtreeMismatch,
        "MTREE mismatch was not typed.");
    expect_mismatch(
        expected,
        binding(
            "ext4:fixture-generation-a", "moguet-binding-fixture",
            "moguet-binding-fixture", "1.r2.g0123456789ab-1", "any",
            DIGEST_A, DIGEST_C),
        InstalledArtifactBindingMismatchReason::DatabaseRecordMismatch,
        "Local database digest mismatch was not typed.");

    // The exact same semantic metadata and digests still fail closed after a
    // same-artifact record replacement.
    expect_mismatch(
        expected, binding("ext4:fixture-generation-b"),
        InstalledArtifactBindingMismatchReason::RecordGenerationMismatch,
        "Opaque record generation mismatch was flattened.");
    expect_mismatch(
        expected,
        binding(
            "ext4:fixture-generation-a", "moguet-binding-fixture",
            "moguet-binding-fixture", "1.r2.g0123456789ab-1", "any",
            DIGEST_A, DIGEST_B,
            "https://aur.archlinux.org/different.git"),
        InstalledArtifactBindingMismatchReason::PackageIdentityMismatch,
        "Package source mismatch was not typed.");
}

void test_invalid_values_and_observation_failures() {
    require_invalid_argument(
        [] { static_cast<void>(AlpmMtreeSha256Digest::make("short")); },
        "Short MTREE digest was accepted.");
    require_invalid_argument(
        [] {
            static_cast<void>(InstalledDatabaseRecordSha256Digest::make(
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
        },
        "Uppercase database digest was accepted.");
    require_invalid_argument(
        [] {
            static_cast<void>(
                make_installed_package_record_generation_fixture_for_test(
                    ""));
        },
        "Empty opaque generation was accepted.");
    require_invalid_argument(
        [] {
            static_cast<void>(make_installed_artifact_binding_fixture_for_test(
                package(), PackageVersionIdentity::unknown(),
                InstalledPackageArchitectureIdentity::known("any"),
                AlpmMtreeSha256Digest::make(DIGEST_A),
                InstalledDatabaseRecordSha256Digest::make(DIGEST_B),
                make_installed_package_record_generation_fixture_for_test(
                    "ext4:generation")));
        },
        "Unknown installed version was accepted.");

    const InstalledArtifactBindingObservationResult unsupported =
        InstalledArtifactBindingObservationFailure{
            InstalledArtifactBindingObservationFailureReason::
                UnsupportedGeneration};
    const InstalledArtifactBindingObservationResult malformed =
        InstalledArtifactBindingObservationFailure{
            InstalledArtifactBindingObservationFailureReason::
                MalformedBinding};
    require(
        std::get<InstalledArtifactBindingObservationFailure>(unsupported)
                .reason ==
            InstalledArtifactBindingObservationFailureReason::
                UnsupportedGeneration,
        "Unsupported generation failure was not retained.");
    require(
        std::get<InstalledArtifactBindingObservationFailure>(malformed)
                .reason ==
            InstalledArtifactBindingObservationFailureReason::
                MalformedBinding,
        "Malformed binding failure was not retained.");
}

} // namespace

int main() {
    try {
        test_binding_retains_distinct_evidence();
        test_comparison_taxonomy();
        test_invalid_values_and_observation_failures();
        std::cout << "installed artifact binding tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
