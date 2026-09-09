#include "devel_git_revision_comparison.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void require(bool condition) {
    if(!condition) throw std::runtime_error("Git comparison mismatch");
}
} // namespace
int main() {
    try {
        using Result = DevelGitRevisionComparison;
        const auto source = VcsSourceIdentity::make(VcsKind::Git, "https://example.test/upstream.git", VcsSelector::default_head());
        for(const std::size_t length : {40U, 64U}) {
            const auto built = UpstreamGitRevision::git_commit(source, std::string(length, 'b'));
            require(compare_devel_git_revision(built, built) == Result::SameRevision);
            const auto different = UpstreamGitRevision::git_commit(source, std::string(length, 'a'));
            require(compare_devel_git_revision(built, different) == Result::DifferentRevision);
            require(compare_devel_git_revision(different, built) == Result::DifferentRevision);
            std::cout << "S7A Git " << length << " same/different/reversed PASS\n";
        }
        const auto built = UpstreamGitRevision::git_commit(source, std::string(40, 'a'));
        const auto sha256 = UpstreamGitRevision::git_commit(source, std::string(64, 'a'));
        require(compare_devel_git_revision(built, sha256) == Result::ObjectFormatMismatch);
        require(compare_devel_git_revision(sha256, built) == Result::ObjectFormatMismatch);
        for(const auto& other : {
                VcsSourceIdentity::make(VcsKind::Git, "https://example.test/other.git", VcsSelector::default_head()),
                VcsSourceIdentity::make(VcsKind::Git, "https://example.test/upstream.git", VcsSelector::branch("main")),
                VcsSourceIdentity::make(VcsKind::Git, "https://example.test/upstream.git", VcsSelector::default_head(), "x86_64")}) {
            require(compare_devel_git_revision(built, UpstreamGitRevision::git_commit(other, std::string(40, 'a'))) == Result::SourceMismatch);
            require(compare_devel_git_revision(built, UpstreamGitRevision::git_commit(other, std::string(64, 'b'))) == Result::SourceMismatch);
        }
        auto moved = built;
        const auto held = std::move(moved);
        require(compare_devel_git_revision(held, built) == Result::SameRevision);
        if(moved.value().git_commit()->empty()) require(compare_devel_git_revision(moved, moved) == Result::InvalidRevision);
        std::cout << "S7A Git algorithm/source precedence/moved-value PASS\n";
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
