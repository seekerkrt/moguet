#pragma once

#include "evaluated_devel_source_artifact_transport.hpp"

#include <variant>

class EvaluatedDevelSourceBuildProof;
class ExactArtifactTransactionReceipt;
class FreshInstalledArtifactBinding;
struct FreshInstalledArtifactBindingFailure;
using FreshInstalledArtifactBindingObservation = std::variant<FreshInstalledArtifactBinding, FreshInstalledArtifactBindingFailure>;

// The complete private declaration is shared by every granting narrow header.
// Historical decode and live observation cannot impersonate each other's mint.
class InstalledArtifactBindingObserver final {
    InstalledArtifactBindingObserver() = delete;
    friend class EvaluatedDevelSourceArtifactTransport;

    [[nodiscard]] static FreshInstalledArtifactBindingObservation observe(
        const ExactArtifactTransactionReceipt& receipt, const EvaluatedDevelSourceBuildProof& built) noexcept;
};
