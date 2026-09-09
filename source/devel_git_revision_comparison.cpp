#include "devel_git_revision_comparison.hpp"

namespace {
bool has_complete_revision(const UpstreamGitRevision& revision) noexcept {
    // Typed construction validates canonical OIDs. Moved-from semantic values
    // can lose their strings; two such empty values must not compare as Same.
    const auto& value = revision.value();
    return value.state() == SourceRevisionState::Known && value.git_object_format() &&
           value.git_commit() && !value.git_commit()->empty() &&
           !revision.source().source_location().empty();
}
} // namespace

DevelGitRevisionComparison compare_devel_git_revision(
    const UpstreamGitRevision& built_revision,
    const UpstreamGitRevision& remote_revision) noexcept {
    if(!has_complete_revision(built_revision) || !has_complete_revision(remote_revision))
        return DevelGitRevisionComparison::InvalidRevision;
    if(built_revision.source() != remote_revision.source()) return DevelGitRevisionComparison::SourceMismatch;
    if(*built_revision.value().git_object_format() != *remote_revision.value().git_object_format())
        return DevelGitRevisionComparison::ObjectFormatMismatch;
    return built_revision.value() == remote_revision.value() ? DevelGitRevisionComparison::SameRevision
                                                             : DevelGitRevisionComparison::DifferentRevision;
}
