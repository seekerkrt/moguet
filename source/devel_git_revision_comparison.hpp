#pragma once

#include "vcs_source_identity.hpp"

enum class DevelGitRevisionComparison {
    SameRevision,
    DifferentRevision,
    ObjectFormatMismatch,
    SourceMismatch,
    InvalidRevision,
};

// Pure semantic comparison, not an observation or an authoritative assessment.
// Caller owns the historical-built/current-remote roles. No raw OID overload,
// network approval, version ordering, or ancestor inference is provided.
[[nodiscard]] DevelGitRevisionComparison compare_devel_git_revision(
    const UpstreamGitRevision& built_revision,
    const UpstreamGitRevision& remote_revision) noexcept;
