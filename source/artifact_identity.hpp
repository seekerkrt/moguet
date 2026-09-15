#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class ValidatedPackageArtifactPath;
class ValidatedPackageArtifactSet;

enum class ArtifactMetadataValueState {
    Known,
    Missing,
    Malformed,
    Unavailable,
};

// Archive内のPackageBaseそのものの観測値。Missing / Malformed / Unavailableを
// upper source contextで補完してKnownへ昇格させない。
class ArtifactPackageBaseIdentity final {
public:
    ArtifactPackageBaseIdentity() = delete;
    ArtifactPackageBaseIdentity(const ArtifactPackageBaseIdentity&) = default;
    ArtifactPackageBaseIdentity(ArtifactPackageBaseIdentity&&) noexcept =
        default;
    ArtifactPackageBaseIdentity& operator=(
        const ArtifactPackageBaseIdentity&) = default;
    ArtifactPackageBaseIdentity& operator=(
        ArtifactPackageBaseIdentity&&) noexcept = default;
    ~ArtifactPackageBaseIdentity() = default;

    [[nodiscard]] static ArtifactPackageBaseIdentity known(
        std::string package_base) {
        return ArtifactPackageBaseIdentity(
            ArtifactMetadataValueState::Known,
            std::move(package_base));
    }

    [[nodiscard]] static ArtifactPackageBaseIdentity missing() noexcept {
        return ArtifactPackageBaseIdentity(
            ArtifactMetadataValueState::Missing, std::nullopt);
    }

    [[nodiscard]] static ArtifactPackageBaseIdentity malformed() noexcept {
        return ArtifactPackageBaseIdentity(
            ArtifactMetadataValueState::Malformed, std::nullopt);
    }

    [[nodiscard]] static ArtifactPackageBaseIdentity unavailable() noexcept {
        return ArtifactPackageBaseIdentity(
            ArtifactMetadataValueState::Unavailable, std::nullopt);
    }

    [[nodiscard]] ArtifactMetadataValueState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::string* value() const noexcept {
        return value_.has_value() ? &value_.value() : nullptr;
    }

    bool operator==(const ArtifactPackageBaseIdentity&) const = default;

private:
    ArtifactPackageBaseIdentity(
        ArtifactMetadataValueState state,
        std::optional<std::string> value) noexcept
        : state_(state), value_(std::move(value)) {
    }

    ArtifactMetadataValueState state_;
    std::optional<std::string> value_;
};

// Architectureはcompatibility判定ではなくarchiveのexact metadata valueを保持する。
// "any"やknown other archもKnownの単一値であり、solver semanticsへ展開しない。
class ArtifactPackageArchitectureIdentity final {
public:
    ArtifactPackageArchitectureIdentity() = delete;
    ArtifactPackageArchitectureIdentity(
        const ArtifactPackageArchitectureIdentity&) = default;
    ArtifactPackageArchitectureIdentity(
        ArtifactPackageArchitectureIdentity&&) noexcept = default;
    ArtifactPackageArchitectureIdentity& operator=(
        const ArtifactPackageArchitectureIdentity&) = default;
    ArtifactPackageArchitectureIdentity& operator=(
        ArtifactPackageArchitectureIdentity&&) noexcept = default;
    ~ArtifactPackageArchitectureIdentity() = default;

    [[nodiscard]] static ArtifactPackageArchitectureIdentity known(
        std::string architecture) {
        return ArtifactPackageArchitectureIdentity(
            ArtifactMetadataValueState::Known,
            std::move(architecture));
    }

    [[nodiscard]] static ArtifactPackageArchitectureIdentity missing() noexcept {
        return ArtifactPackageArchitectureIdentity(
            ArtifactMetadataValueState::Missing, std::nullopt);
    }

    [[nodiscard]] static ArtifactPackageArchitectureIdentity malformed() noexcept {
        return ArtifactPackageArchitectureIdentity(
            ArtifactMetadataValueState::Malformed, std::nullopt);
    }

    [[nodiscard]] static ArtifactPackageArchitectureIdentity unavailable() noexcept {
        return ArtifactPackageArchitectureIdentity(
            ArtifactMetadataValueState::Unavailable, std::nullopt);
    }

    [[nodiscard]] ArtifactMetadataValueState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::string* value() const noexcept {
        return value_.has_value() ? &value_.value() : nullptr;
    }

    bool operator==(const ArtifactPackageArchitectureIdentity&) const =
        default;

private:
    ArtifactPackageArchitectureIdentity(
        ArtifactMetadataValueState state,
        std::optional<std::string> value) noexcept
        : state_(state), value_(std::move(value)) {
    }

    ArtifactMetadataValueState state_;
    std::optional<std::string> value_;
};

// package archiveからread-only libalpm metadata queryが読み出したidentity。
// filesystem capabilityとは分離し、requested packageとの一致判定はpure modelへ委ねる。
struct ArtifactPackageIdentity {
    std::string package_name;
    std::string full_version;
    ArtifactPackageBaseIdentity package_base =
        ArtifactPackageBaseIdentity::unavailable();
    ArtifactPackageArchitectureIdentity architecture =
        ArtifactPackageArchitectureIdentity::unavailable();

    bool operator==(const ArtifactPackageIdentity&) const = default;
};

class ArtifactPackageIdentitySet;
class EvaluatedDevelSourceBuildProof;

// ValidatedPackageArtifactSet内のstable positionと、archiveから取得したidentity。
// pathやfilesystem cleanup capabilityは保持しない。
class IndexedArtifactPackageIdentity final {
    std::size_t artifact_index_ = 0;
    ArtifactPackageIdentity identity_;

    IndexedArtifactPackageIdentity(
        std::size_t artifact_index,
        ArtifactPackageIdentity identity) noexcept;

    friend class ArtifactPackageIdentitySet;

public:
    IndexedArtifactPackageIdentity(
        const IndexedArtifactPackageIdentity&) = default;
    IndexedArtifactPackageIdentity(
        IndexedArtifactPackageIdentity&&) noexcept = default;
    IndexedArtifactPackageIdentity& operator=(
        const IndexedArtifactPackageIdentity&) = delete;
    IndexedArtifactPackageIdentity& operator=(
        IndexedArtifactPackageIdentity&&) noexcept = delete;
    ~IndexedArtifactPackageIdentity() = default;

    std::size_t artifact_index() const noexcept {
        return artifact_index_;
    }

    const ArtifactPackageIdentity& identity() const noexcept {
        return identity_;
    }
};

// Aggregate順のidentityを、欠落・重複しないstable artifact indexと一体で保持する。
// raw identity vectorからのconstructionはquery/test factoryだけへ閉じる。
class ArtifactPackageIdentitySet final {
    std::vector<IndexedArtifactPackageIdentity> entries_;
    bool is_active_ = true;

    explicit ArtifactPackageIdentitySet(
        std::vector<ArtifactPackageIdentity> identities);

    void require_active() const;

    friend ArtifactPackageIdentitySet query_artifact_package_identities(const EvaluatedDevelSourceBuildProof& proof);
    friend ArtifactPackageIdentitySet query_artifact_package_identities(
        const ValidatedPackageArtifactSet& artifacts);
#ifdef MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
    friend ArtifactPackageIdentitySet
    make_artifact_package_identity_set_for_test(
        std::vector<ArtifactPackageIdentity> identities);
#endif

public:
    ArtifactPackageIdentitySet(const ArtifactPackageIdentitySet&) = delete;
    ArtifactPackageIdentitySet& operator=(
        const ArtifactPackageIdentitySet&) = delete;
    ArtifactPackageIdentitySet(
        ArtifactPackageIdentitySet&& other) noexcept;
    ArtifactPackageIdentitySet& operator=(
        ArtifactPackageIdentitySet&&) = delete;
    ~ArtifactPackageIdentitySet() = default;

    std::size_t size() const;

    const IndexedArtifactPackageIdentity& entry_at(
        std::size_t position) const;
};

// Arbitraryなraw pathを受けず、post-build validation済みartifactだけを照会する。
ArtifactPackageIdentity query_artifact_package_identity(
    const ValidatedPackageArtifactPath& artifact);

// Aggregateをborrowし、各artifactをaggregate順に一回ずつpacmanへ照会する。
// 全件成功とaggregate-wide revalidationを通過した場合だけclosed resultを返す。
ArtifactPackageIdentitySet query_artifact_package_identities(
    const ValidatedPackageArtifactSet& artifacts);

#ifdef MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
// Pure adapter test用。filesystem capabilityやpathを生成せず、indexはvector順に固定する。
inline ArtifactPackageIdentitySet make_artifact_package_identity_set_for_test(
    std::vector<ArtifactPackageIdentity> identities) {
    return ArtifactPackageIdentitySet(std::move(identities));
}
#endif
