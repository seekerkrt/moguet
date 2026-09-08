#pragma once

#include <optional>

class DevelSourceArtifactInstallResult;
class DevelBuildProvenancePublicationResult;
enum class DevelBuildProvenancePublicationRequest;

// Complete in every granting narrow header. The only public entrance still
// requires ownership of the sealed S5 result; no component tuple enters here.
class DevelBuildProvenancePublicationAuthority final {
    DevelBuildProvenancePublicationAuthority() = delete;
    friend std::optional<DevelBuildProvenancePublicationResult>
    publish_installed_devel_source_build(DevelSourceArtifactInstallResult result,
                                         DevelBuildProvenancePublicationRequest request) noexcept;

    [[nodiscard]] static std::optional<DevelBuildProvenancePublicationResult>
    publish(DevelSourceArtifactInstallResult result,
            DevelBuildProvenancePublicationRequest request) noexcept;
};
