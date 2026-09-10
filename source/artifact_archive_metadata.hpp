#pragma once

#include "evaluated_devel_source_build_authority.hpp"
#include "artifact_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace artifact_archive_metadata {

class QueryAuthority final {
public:
    QueryAuthority() = delete;
    explicit QueryAuthority(
        const ValidatedPackageArtifactPath& artifact);
    QueryAuthority(
        const ValidatedPackageArtifactSet& artifacts,
        std::size_t artifact_index);

    void require_validity() const;
    [[nodiscard]] const std::filesystem::path& path() const;

private:
    const ValidatedPackageArtifactPath* artifact_ = nullptr;
    const ValidatedPackageArtifactSet* artifacts_ = nullptr;
    std::size_t artifact_index_ = 0;
};

// Closed borrowed-FD authority for callers that must bind archive metadata to
// an already opened artifact rather than ask libalpm to resolve a mutable
// pathname. The production mint is limited to the Slice 4 build proof owner.
class RetainedDescriptorQueryAuthority final {
public:
    RetainedDescriptorQueryAuthority() = delete;
    RetainedDescriptorQueryAuthority(
        const RetainedDescriptorQueryAuthority&) = delete;
    RetainedDescriptorQueryAuthority(
        RetainedDescriptorQueryAuthority&&) noexcept = default;
    RetainedDescriptorQueryAuthority& operator=(
        const RetainedDescriptorQueryAuthority&) = delete;
    RetainedDescriptorQueryAuthority& operator=(
        RetainedDescriptorQueryAuthority&&) = delete;
    ~RetainedDescriptorQueryAuthority() = default;

    void require_validity() const;
    [[nodiscard]] std::filesystem::path proc_descriptor_path() const;

private:
    friend class ::EvaluatedDevelSourceBuildAuthority;

    explicit RetainedDescriptorQueryAuthority(int descriptor);

    int descriptor_ = -1;
    std::uintmax_t device_ = 0;
    std::uintmax_t inode_ = 0;
    std::uintmax_t owner_ = 0;
    std::uintmax_t size_ = 0;
};

// Reads one package archive through libalpm without opening a transaction or
// consulting package databases. The caller owns filesystem provenance and
// must hold the validated artifact query authority and revalidate it around
// this read.
ArtifactPackageIdentity query_with_libalpm(
    const QueryAuthority& authority);

ArtifactPackageIdentity query_with_libalpm(
    const RetainedDescriptorQueryAuthority& authority);

#ifdef MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
// Existing filesystem/aggregate tests replace only the archive reader. Their
// process stub remains test authority and never enters a production target.
ArtifactPackageIdentity query_with_test_stub(
    const std::filesystem::path& artifact_path);

// Deterministic actual-archive fixture only. Production has no raw-path
// entrypoint without QueryAuthority.
ArtifactPackageIdentity query_with_libalpm_for_test(
    const std::filesystem::path& artifact_path);
#endif

} // namespace artifact_archive_metadata
