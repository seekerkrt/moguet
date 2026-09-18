#pragma once

#include <optional>
#include <string>
#include <utility>

// installed packageのidentityを、外部sessionに依存しないowned valueで表す。
enum class InstalledPackageReason {
    Explicit,
    Dependency,
    Unknown,
};

// Installed local database metadata is factual input. Missing, malformed, or
// unavailable values must not be reconstructed from the package name, source
// context, or an archive observation.
enum class InstalledPackageMetadataValueState {
    Known,
    Missing,
    Malformed,
    Unavailable,
    Unknown,
};

class InstalledPackageBaseIdentity final {
public:
    InstalledPackageBaseIdentity() = delete;
    InstalledPackageBaseIdentity(const InstalledPackageBaseIdentity&) = default;
    InstalledPackageBaseIdentity(InstalledPackageBaseIdentity&&) noexcept =
        default;
    InstalledPackageBaseIdentity& operator=(
        const InstalledPackageBaseIdentity&) = default;
    InstalledPackageBaseIdentity& operator=(
        InstalledPackageBaseIdentity&&) noexcept = default;
    ~InstalledPackageBaseIdentity() = default;

    [[nodiscard]] static InstalledPackageBaseIdentity known(
        std::string package_base) {
        return InstalledPackageBaseIdentity(
            InstalledPackageMetadataValueState::Known,
            std::move(package_base));
    }

    [[nodiscard]] static InstalledPackageBaseIdentity missing() noexcept {
        return InstalledPackageBaseIdentity(
            InstalledPackageMetadataValueState::Missing, std::nullopt);
    }

    [[nodiscard]] static InstalledPackageBaseIdentity malformed() noexcept {
        return InstalledPackageBaseIdentity(
            InstalledPackageMetadataValueState::Malformed, std::nullopt);
    }

    [[nodiscard]] static InstalledPackageBaseIdentity unavailable() noexcept {
        return InstalledPackageBaseIdentity(
            InstalledPackageMetadataValueState::Unavailable, std::nullopt);
    }

    [[nodiscard]] static InstalledPackageBaseIdentity unknown() noexcept {
        return InstalledPackageBaseIdentity(
            InstalledPackageMetadataValueState::Unknown, std::nullopt);
    }

    [[nodiscard]] InstalledPackageMetadataValueState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::string* value() const noexcept {
        return value_.has_value() ? &value_.value() : nullptr;
    }

    bool operator==(const InstalledPackageBaseIdentity&) const = default;

private:
    InstalledPackageBaseIdentity(
        InstalledPackageMetadataValueState state,
        std::optional<std::string> value) noexcept
        : state_(state), value_(std::move(value)) {
    }

    InstalledPackageMetadataValueState state_;
    std::optional<std::string> value_;
};

class InstalledPackageArchitectureIdentity final {
public:
    InstalledPackageArchitectureIdentity() = delete;
    InstalledPackageArchitectureIdentity(
        const InstalledPackageArchitectureIdentity&) = default;
    InstalledPackageArchitectureIdentity(
        InstalledPackageArchitectureIdentity&&) noexcept = default;
    InstalledPackageArchitectureIdentity& operator=(
        const InstalledPackageArchitectureIdentity&) = default;
    InstalledPackageArchitectureIdentity& operator=(
        InstalledPackageArchitectureIdentity&&) noexcept = default;
    ~InstalledPackageArchitectureIdentity() = default;

    [[nodiscard]] static InstalledPackageArchitectureIdentity known(
        std::string architecture) {
        return InstalledPackageArchitectureIdentity(
            InstalledPackageMetadataValueState::Known,
            std::move(architecture));
    }

    [[nodiscard]] static InstalledPackageArchitectureIdentity missing() noexcept {
        return InstalledPackageArchitectureIdentity(
            InstalledPackageMetadataValueState::Missing, std::nullopt);
    }

    [[nodiscard]] static InstalledPackageArchitectureIdentity malformed() noexcept {
        return InstalledPackageArchitectureIdentity(
            InstalledPackageMetadataValueState::Malformed, std::nullopt);
    }

    [[nodiscard]] static InstalledPackageArchitectureIdentity unavailable() noexcept {
        return InstalledPackageArchitectureIdentity(
            InstalledPackageMetadataValueState::Unavailable, std::nullopt);
    }

    [[nodiscard]] static InstalledPackageArchitectureIdentity unknown() noexcept {
        return InstalledPackageArchitectureIdentity(
            InstalledPackageMetadataValueState::Unknown, std::nullopt);
    }

    [[nodiscard]] InstalledPackageMetadataValueState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::string* value() const noexcept {
        return value_.has_value() ? &value_.value() : nullptr;
    }

    bool operator==(const InstalledPackageArchitectureIdentity&) const =
        default;

private:
    InstalledPackageArchitectureIdentity(
        InstalledPackageMetadataValueState state,
        std::optional<std::string> value) noexcept
        : state_(state), value_(std::move(value)) {
    }

    InstalledPackageMetadataValueState state_;
    std::optional<std::string> value_;
};

struct InstalledPackageMetadata {
    std::string name;
    std::string version;
    InstalledPackageReason reason = InstalledPackageReason::Unknown;
    // Trailing defaults preserve legacy factual callers without granting them
    // current PackageBase or architecture authority.
    InstalledPackageBaseIdentity package_base =
        InstalledPackageBaseIdentity::unknown();
    InstalledPackageArchitectureIdentity architecture =
        InstalledPackageArchitectureIdentity::unknown();
    bool operator==(const InstalledPackageMetadata&) const = default;
};
