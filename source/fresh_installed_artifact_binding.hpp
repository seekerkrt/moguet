#pragma once

#include "installed_artifact_binding.hpp"
#include "installed_artifact_binding_observer_authority.hpp"
#include "installed_package_record_observation.hpp"

// Fresh local DB observation correlated with a successful exact transaction's
// PostTransaction anchor. It says nothing about all installed payload bytes,
// later filesystem changes, or scriptlet/NoExtract/NoUpgrade effects.
// This is a component capability, not InstalledDevelSourceBuildProof.
class FreshInstalledArtifactBinding final {
public:
    FreshInstalledArtifactBinding() = delete;
    FreshInstalledArtifactBinding(const FreshInstalledArtifactBinding&) = delete;
    FreshInstalledArtifactBinding& operator=(const FreshInstalledArtifactBinding&) = delete;
    FreshInstalledArtifactBinding(FreshInstalledArtifactBinding&&) noexcept = default;
    FreshInstalledArtifactBinding& operator=(FreshInstalledArtifactBinding&&) = delete;
    ~FreshInstalledArtifactBinding() = default;

    [[nodiscard]] const InstalledArtifactBinding& binding() const noexcept;
    [[nodiscard]] const std::string& transaction_token() const noexcept;
    [[nodiscard]] std::size_t artifact_index() const noexcept;

private:
    friend class InstalledArtifactBindingObserver;
    FreshInstalledArtifactBinding(InstalledArtifactBinding binding, std::string token, std::size_t index) noexcept;
    InstalledArtifactBinding binding_;
    std::string transaction_token_;
    std::size_t artifact_index_;
};

struct FreshInstalledArtifactBindingFailure {
    InstalledRecordObservationIssue reason;
};
