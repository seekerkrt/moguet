#include "process.hpp"
#include "devel_build_provenance_reviewed_binding.hpp"
#include "reviewed_source_pinned_build.hpp"
#include "reviewed_source_review.hpp"
#include "trusted_cache.hpp"
#include "trusted_cache_test_support.hpp"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace {

static_assert(!std::is_default_constructible_v<
              ReviewedSourcePackageBaseLease>);
static_assert(!std::is_copy_constructible_v<
              ReviewedSourcePackageBaseLease>);
static_assert(!std::is_copy_assignable_v<
              ReviewedSourcePackageBaseLease>);
static_assert(std::is_move_constructible_v<
              ReviewedSourcePackageBaseLease>);
static_assert(!std::is_move_assignable_v<
              ReviewedSourcePackageBaseLease>);
static_assert(!std::is_constructible_v<
              ReviewedSourcePackageBaseLease,
              RetainedTrustedCacheDirectory>);
static_assert(!std::is_constructible_v<
              ReviewedSourceExpectedStateObservation,
              ReviewedSourceStateStoreRead>);
static_assert(!std::is_constructible_v<
              ReviewedSourceAlreadyReviewedContinue,
              AurReviewedSourceReviewIdentity,
              ReviewedSourceExpectedStateObservation>);
static_assert(!std::is_default_constructible_v<
              AcceptedReviewedSourceCheckout>);
static_assert(!std::is_copy_constructible_v<
              AcceptedReviewedSourceCheckout>);
static_assert(std::is_move_constructible_v<
              AcceptedReviewedSourceCheckout>);
static_assert(!std::is_constructible_v<
              AcceptedReviewedSourceCheckout,
              AcceptedReviewedSourceTarget,
              RetainedTrustedCacheDirectory,
              TrustedGitPinnedCheckout>);
static_assert(!std::is_constructible_v<
              AcceptedReviewedSourceCheckout,
              AcceptedReviewedSourceTarget,
              ReviewedSourcePackageBaseLease,
              TrustedGitPinnedCheckout>);
static_assert(!std::is_default_constructible_v<
              AlreadyReviewedSourceCheckout>);
static_assert(!std::is_copy_constructible_v<
              AlreadyReviewedSourceCheckout>);
static_assert(std::is_move_constructible_v<
              AlreadyReviewedSourceCheckout>);
static_assert(!std::is_constructible_v<
              AlreadyReviewedSourceCheckout,
              ReviewedSourceAlreadyReviewedContinue,
              RetainedTrustedCacheDirectory,
              TrustedGitPinnedCheckout>);
static_assert(!std::is_constructible_v<
              AlreadyReviewedSourceCheckout,
              ReviewedSourceAlreadyReviewedContinue,
              ReviewedSourcePackageBaseLease,
              TrustedGitPinnedCheckout>);
static_assert(!std::is_move_assignable_v<
              ReviewedSourceAlreadyReviewedContinue>);
static_assert(!std::is_default_constructible_v<
              PinnedReviewedSourceBuild>);
static_assert(!std::is_copy_constructible_v<
              PinnedReviewedSourceBuild>);
static_assert(std::is_move_constructible_v<
              PinnedReviewedSourceBuild>);
static_assert(!std::is_constructible_v<
              PinnedReviewedSourceBuild,
              AurReviewedSourceReviewIdentity,
              SourceRevisionIdentity,
              ValidatedCachePath,
              ReviewedSourceState>);
static_assert(!std::is_constructible_v<
              PinnedReviewedSourceBuild,
              AcceptedReviewedSourceCheckout,
              ReviewedSourcePublicationStatus,
              ReviewedSourceState,
              ReviewedSourceStateObservedRecord>);
static_assert(!std::is_constructible_v<
              PinnedReviewedSourceBuild,
              AlreadyReviewedSourceCheckout,
              ReviewedSourceState,
              ReviewedSourceStateObservedRecord>);
static_assert(!std::is_default_constructible_v<TrustedGitPinnedCheckout>);
static_assert(!std::is_copy_constructible_v<TrustedGitPinnedCheckout>);
static_assert(std::is_move_constructible_v<TrustedGitPinnedCheckout>);
static_assert(!std::is_constructible_v<
              TrustedGitPinnedCheckout,
              ValidatedCachePath,
              AurReviewedSourceReviewIdentity>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceEditorBoundary>);
static_assert(!std::is_copy_constructible_v<
              ReviewedSourceEditorBoundary>);
static_assert(std::is_move_constructible_v<
              ReviewedSourceEditorBoundary>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceEditorOverlayProof>);
static_assert(!std::is_copy_constructible_v<
              ReviewedSourceEditorOverlayProof>);
static_assert(std::is_move_constructible_v<
              ReviewedSourceEditorOverlayProof>);
static_assert(!std::is_constructible_v<
              ReviewedSourceEditorOverlayProof,
              ReviewedSourceEditorOverlayStatus>);
static_assert(!std::is_constructible_v<
              ReviewedSourceEditorOverlayProof,
              bool>);
static_assert(!std::is_constructible_v<
              ReviewedSourceEditorOverlayProof,
              ValidatedCachePath>);
static_assert(!std::is_invocable_v<
              decltype(trusted_git_materialize_pinned_checkout),
              const ValidatedCachePath&,
              AurReviewedSourceReviewIdentity>);
static_assert(std::is_invocable_v<
              decltype(trusted_git_materialize_pinned_checkout),
              const ValidatedCachePath&,
              AurReviewedSourceReviewIdentity,
              const ReviewedSourcePackageBaseLease&>);
static_assert(std::is_invocable_v<
              decltype(materialize_accepted_reviewed_source_checkout),
              AcceptedReviewedSourceTarget,
              const ValidatedCachePath&>);
static_assert(!std::is_invocable_v<
              decltype(materialize_accepted_reviewed_source_checkout),
              PresentedReviewedSourceTarget,
              const ValidatedCachePath&>);
static_assert(!std::is_invocable_v<
              decltype(materialize_accepted_reviewed_source_checkout),
              ReviewedSourceCompatibilityBuildWithoutReview,
              const ValidatedCachePath&>);
static_assert(!std::is_invocable_v<
              decltype(materialize_accepted_reviewed_source_checkout),
              AurReviewedSourceReviewIdentity,
              SourceRevisionIdentity,
              const ValidatedCachePath&>);
static_assert(!std::is_invocable_v<
              decltype(materialize_accepted_reviewed_source_checkout),
              AcceptedReviewedSourceTarget,
              const fs::path&>);
static_assert(std::is_invocable_v<
              decltype(materialize_already_reviewed_source_checkout),
              ReviewedSourceAlreadyReviewedContinue,
              const ValidatedCachePath&>);
static_assert(!std::is_invocable_v<
              decltype(materialize_already_reviewed_source_checkout),
              AcceptedReviewedSourceTarget,
              const ValidatedCachePath&>);
static_assert(std::is_invocable_v<
              decltype(publish_accepted_reviewed_source_checkout),
              AcceptedReviewedSourceCheckout>);
static_assert(!std::is_invocable_v<
              decltype(publish_accepted_reviewed_source_checkout),
              AcceptedReviewedSourceTarget>);
static_assert(!std::is_invocable_v<
              decltype(publish_accepted_reviewed_source_checkout),
              ReviewedSourceState>);
static_assert(!std::is_invocable_v<
              decltype(publish_accepted_reviewed_source_checkout),
              TrustedGitPinnedCheckout>);
static_assert(!std::is_invocable_v<
              decltype(publish_accepted_reviewed_source_checkout),
              PresentedReviewedSourceTarget>);
static_assert(!std::is_invocable_v<
              decltype(publish_accepted_reviewed_source_checkout),
              ReviewedSourceCompatibilityBuildWithoutReview>);
static_assert(std::is_invocable_v<
              decltype(confirm_already_reviewed_source_checkout),
              AlreadyReviewedSourceCheckout>);
static_assert(!std::is_invocable_v<
              decltype(confirm_already_reviewed_source_checkout),
              ReviewedSourceAlreadyReviewedContinue>);

void require(bool condition, std::string_view message) {
    if(!condition) throw std::runtime_error(std::string(message));
}

template <typename Expected, typename Variant>
const Expected& require_arm(
    const Variant& value, std::string_view message) {
    const Expected* arm = std::get_if<Expected>(&value);
    if(arm == nullptr) throw std::runtime_error(std::string(message));
    return *arm;
}

template <typename Expected, typename Variant>
Expected take_arm(Variant& value, std::string_view message) {
    Expected* arm = std::get_if<Expected>(&value);
    if(arm == nullptr) throw std::runtime_error(std::string(message));
    return std::move(*arm);
}

class TemporaryTree final {
public:
    TemporaryTree() {
        std::string path_template =
            "/tmp/moguet-reviewed-source-pinned-build-XXXXXX";
        std::vector<char> writable(
            path_template.begin(), path_template.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if(created == nullptr) {
            throw std::runtime_error("Failed to create pinned-build fixture");
        }
        path_ = created;
    }

    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;

    ~TemporaryTree() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

std::vector<std::string> git_environment(const fs::path& home) {
    return {
        "PATH=/usr/bin:/bin",
        "LC_ALL=C",
        "LANG=C",
        "HOME=" + home.string(),
        "GIT_CONFIG_NOSYSTEM=1",
        "GIT_CONFIG_GLOBAL=/dev/null",
        "GIT_AUTHOR_NAME=Slice 4B Fixture",
        "GIT_AUTHOR_EMAIL=slice4b@example.invalid",
        "GIT_COMMITTER_NAME=Slice 4B Fixture",
        "GIT_COMMITTER_EMAIL=slice4b@example.invalid",
        "GIT_TERMINAL_PROMPT=0",
    };
}

std::string shell_quote_path(const fs::path& path) {
    std::string quoted = "'";
    for(char character : path.string()) {
        if(character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
}

class PinnedBuildFixture final {
public:
    explicit PinnedBuildFixture(
        GitObjectFormat object_format = GitObjectFormat::Sha1) {
        cache_home_ = tree_.path() / "cache";
        state_home_ = tree_.path() / "state";
        home_ = tree_.path() / "home";
        fs::create_directory(cache_home_);
        fs::create_directory(state_home_);
        fs::create_directory(home_);
        require(::chmod(cache_home_.c_str(), 0700) == 0,
                "Failed to secure fixture cache home");
        require(::chmod(state_home_.c_str(), 0700) == 0,
                "Failed to secure fixture state home");
        require(::chmod(home_.c_str(), 0700) == 0,
                "Failed to secure fixture home");
        require(::setenv("XDG_CACHE_HOME", cache_home_.c_str(), 1) == 0,
                "Failed to set fixture XDG_CACHE_HOME");
        require(::setenv("XDG_STATE_HOME", state_home_.c_str(), 1) == 0,
                "Failed to set fixture XDG_STATE_HOME");
        require(::setenv("HOME", home_.c_str(), 1) == 0,
                "Failed to set fixture HOME");

        cache_root_.emplace(prepare_test_trusted_cache_root());
        checkout_.emplace(create_trusted_cache_directory(
            *cache_root_, package_base_));
        repository_ = checkout_->path();

        std::vector<std::string> init{"init", "-q", "-b", "main"};
        if(object_format == GitObjectFormat::Sha256) {
            init.push_back("--object-format=sha256");
        }
        run_git(init);
        run_git({"config", "--local", "remote.origin.url", remote_url_});
        run_git({"config", "--local", "remote.origin.fetch",
                 "+refs/heads/*:refs/remotes/origin/*"});
        run_git({"config", "--local", "branch.main.remote", "origin"});
        run_git({"config", "--local", "branch.main.merge",
                 "refs/heads/main"});

        write_file(".gitignore", "ignored-*.tmp\n");
        write_file("PKGBUILD", "pkgname=example\npkgver=1\npkgrel=1\n");
        first_oid_ = commit("first");
        write_file("PKGBUILD", "pkgname=example\npkgver=2\npkgrel=1\n");
        second_oid_ = commit("second");
        run_git({"update-ref", "refs/remotes/origin/main", second_oid_});
    }

    const std::string& package_base() const noexcept {
        return package_base_;
    }
    const std::string& remote_url() const noexcept {
        return remote_url_;
    }
    const std::string& first_oid() const noexcept {
        return first_oid_;
    }
    const std::string& second_oid() const noexcept {
        return second_oid_;
    }
    const fs::path& repository() const noexcept {
        return repository_;
    }
    const fs::path& state_home() const noexcept {
        return state_home_;
    }
    const fs::path& fixture_root() const noexcept {
        return tree_.path();
    }

    ValidatedCachePath checkout() const {
        return revalidate_trusted_cache_path(
            *checkout_, CachePathRequirement::ExistingDirectory);
    }

    AurReviewedSourceReviewIdentity identity(
        const std::string& oid) const {
        return AurReviewedSourceReviewIdentity::make(
            PackageBaseIdentity::make(
                PackageSourceIdentity::aur(
                    SourceLocationIdentity::known_git_remote(
                        remote_url_)),
                package_base_),
            SourceRevisionIdentity::git_commit(oid));
    }

    void write_file(
        const std::string& relative_path,
        std::string_view contents) const {
        const fs::path path = repository_ / relative_path;
        if(!path.parent_path().empty()) {
            fs::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if(!output) throw std::runtime_error("Failed to open fixture file");
        output.write(
            contents.data(),
            static_cast<std::streamsize>(contents.size()));
        output.close();
        require(static_cast<bool>(output), "Failed to write fixture file");
        require(::chmod(path.c_str(), 0644) == 0,
                "Failed to secure fixture file");
    }

    std::string read_file(const std::string& relative_path) const {
        std::ifstream input(repository_ / relative_path, std::ios::binary);
        if(!input) throw std::runtime_error("Failed to read fixture file");
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    std::string output_git(std::vector<std::string> arguments) const {
        std::vector<std::string> complete{"-C", repository_.string()};
        complete.insert(complete.end(), arguments.begin(), arguments.end());
        CapturedCommandResult result = capture_explicit_process_output_raw(
            ExplicitProcessInvocation{
                "/usr/bin/git", std::move(complete),
                git_environment(home_)},
            true);
        if(result.exit_code != 0 || result.stdout_capture_limit_exceeded ||
           result.output.empty() || result.output.back() != '\n') {
            throw std::runtime_error("Fixture Git capture failed");
        }
        result.output.pop_back();
        return result.output;
    }

    int git_status(std::vector<std::string> arguments) const {
        std::vector<std::string> complete{"-C", repository_.string()};
        complete.insert(complete.end(), arguments.begin(), arguments.end());
        return run_explicit_process(
            ExplicitProcessInvocation{
                "/usr/bin/git", std::move(complete),
                git_environment(home_)},
            true, true);
    }

    void run_git(std::vector<std::string> arguments) const {
        const int exit_code = git_status(std::move(arguments));
        if(exit_code != 0) {
            throw std::runtime_error(
                "Fixture Git command failed with exit code " +
                std::to_string(exit_code));
        }
    }

private:
    std::string commit(std::string_view message) const {
        run_git({"add", "-A"});
        run_git({"commit", "-q", "-m", std::string(message)});
        return output_git({"rev-parse", "HEAD"});
    }

    TemporaryTree tree_;
    fs::path cache_home_;
    fs::path state_home_;
    fs::path home_;
    fs::path repository_;
    const std::string package_base_ = "example-base";
    const std::string remote_url_ =
        "https://aur.archlinux.org/example-base.git";
    std::string first_oid_;
    std::string second_oid_;
    std::optional<ValidatedCacheRoot> cache_root_;
    std::optional<ValidatedCachePath> checkout_;
};

ExplicitConfirmationResult explicit_yes() {
    ExplicitConfirmationInputParseResult parsed =
        parse_explicit_confirmation_input("yes");
    auto* accepted =
        std::get_if<ExplicitConfirmationAcceptance>(&parsed);
    if(accepted == nullptr) {
        throw std::runtime_error("Explicit yes fixture was not accepted");
    }
    return std::move(*accepted);
}

AcceptedReviewedSourceTarget make_initial_accepted(
    const AurReviewedSourceReviewIdentity& identity) {
    ReviewedSourceLifecyclePlanResult planned =
        plan_reviewed_source_lifecycle(identity);
    ReviewedSourceReviewRequirement requirement =
        take_arm<ReviewedSourceReviewRequirement>(
            planned, "Initial lifecycle did not require review");

    ReviewedSourceVerifiedMaterializedReview verified =
        seal_reviewed_source_materialized_review_for_test(
            ReviewedSourceMaterializedInitialFullReview{
                identity.target_revision(),
                ReviewedSourceReviewBody{
                    ReviewedSourceReviewReadiness::Complete,
                    {}}});
    TrustedAurReviewedSourceReview trusted =
        make_trusted_aur_reviewed_source_review_fixture_for_test(
            std::move(verified));
    ReviewedSourceVerifiedLifecycleResult bound =
        bind_reviewed_source_verified_review(
            std::move(requirement), std::move(trusted));
    ReviewedSourceVerifiedLifecycleTarget verified_target =
        take_arm<ReviewedSourceVerifiedLifecycleTarget>(
            bound, "Verified review did not bind");
    std::ostringstream output;
    PresentedReviewedSourceTargetResult presented =
        present_reviewed_source_target(
            std::move(verified_target), output);
    ReviewedSourceAcceptanceDisposition disposition =
        decide_reviewed_source_acceptance(
            take_arm<PresentedReviewedSourceTarget>(
                presented, "Review presentation failed"),
            explicit_yes());
    return take_arm<AcceptedReviewedSourceTarget>(
        disposition, "Explicit review did not produce Accepted");
}

AcceptedReviewedSourceCheckout materialize(
    AcceptedReviewedSourceTarget accepted,
    const ValidatedCachePath& checkout) {
    AcceptedReviewedSourceCheckoutResult result =
        materialize_accepted_reviewed_source_checkout(
            std::move(accepted), checkout);
    return take_arm<AcceptedReviewedSourceCheckout>(
        result, "Accepted target did not materialize exact checkout");
}

template <typename Checkout>
ReviewedSourceEditorBoundary begin_editor_boundary(
    const Checkout& checkout,
    std::string_view message = "Editor boundary was not proven") {
    ReviewedSourceEditorBoundaryResult result =
        begin_reviewed_source_editor_boundary(checkout);
    return take_arm<ReviewedSourceEditorBoundary>(result, message);
}

template <typename Checkout>
ReviewedSourceEditorOverlayProof seal_editor_overlay(
    const Checkout& checkout,
    ReviewedSourceEditorBoundary boundary,
    std::string_view message = "Editor overlay was not sealed") {
    ReviewedSourceEditorOverlayProofResult result =
        seal_reviewed_source_editor_overlay(
            checkout, std::move(boundary));
    return take_arm<ReviewedSourceEditorOverlayProof>(result, message);
}

template <typename Checkout>
ReviewedSourceEditorOverlayProof seal_no_editor_overlay(
    const Checkout& checkout,
    ReviewedSourceEditorBoundary boundary,
    std::string_view message = "No-editor overlay was not sealed") {
    ReviewedSourceEditorOverlayProofResult result =
        seal_reviewed_source_no_editor_overlay(
            checkout, std::move(boundary));
    return take_arm<ReviewedSourceEditorOverlayProof>(result, message);
}

const ReviewedSourceStateStoreRead& read_loaded(
    ReviewedSourceStateStoreReadResult& result,
    const SourceRevisionIdentity& expected) {
    const auto& read = require_arm<ReviewedSourceStateStoreRead>(
        result, "Reviewed state lookup did not return Read");
    const auto& loaded = require_arm<ReviewedSourceStateLoaded>(
        read.observation, "Reviewed state lookup was not Loaded");
    require(loaded.state.reviewed_revision() == expected,
            "Reviewed state target differs from expected");
    return read;
}

void test_exact_oid_checkout_and_definite_publication() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceTarget accepted = make_initial_accepted(identity);

    fixture.write_file(
        "PKGBUILD",
        "pkgname=example\npkgver=999\npkgrel=1\n# dirty\n");
    fixture.write_file("ignored-residue.tmp", "ignored residue\n");
    fixture.write_file("ordinary-residue.tmp", "untracked residue\n");
    fixture.run_git({"update-ref", "refs/remotes/origin/main",
                     fixture.second_oid()});

    AcceptedReviewedSourceCheckout exact = materialize(
        std::move(accepted), fixture.checkout());
    require(fixture.output_git({"rev-parse", "HEAD"}) == fixture.first_oid(),
            "Exact checkout followed a mutable ref");
    require(fixture.git_status({"symbolic-ref", "--quiet", "HEAD"}) == 1,
            "Exact checkout did not detach HEAD");
    require(!fs::exists(fixture.repository() / "ignored-residue.tmp"),
            "Exact checkout retained Git-ignored residue");
    require(!fs::exists(fixture.repository() / "ordinary-residue.tmp"),
            "Exact checkout retained ordinary untracked residue");
    require(fixture.read_file("PKGBUILD").find("pkgver=1") !=
                std::string::npos,
            "Exact checkout did not materialize reviewed bytes");

    ReviewedSourcePublicationResult publication =
        publish_accepted_reviewed_source_checkout(std::move(exact));
    PinnedReviewedSourceBuild pinned = take_arm<PinnedReviewedSourceBuild>(
        publication, "Definite publication did not produce pinned build");
    require(pinned.publication_status() ==
                    ReviewedSourcePublicationStatus::Published &&
                pinned.identity() == identity &&
                pinned.reviewed_upstream_base_revision() ==
                    identity.target_revision() &&
                pinned.published_record().generation == 1 &&
                pinned.checkout_path() == fixture.repository(),
            "Pinned build lost checkout/publication identity");

    ReviewedSourceStateRecordBindingResult binding_result =
        derive_reviewed_source_state_record_binding(pinned);
    const auto* binding =
        std::get_if<ReviewedSourceStateRecordBinding>(&binding_result);
    require(binding != nullptr &&
                binding->package_base() == identity.package_base() &&
                binding->reviewed_recipe_revision().value() ==
                    identity.target_revision() &&
                binding->generation().value() == 1 &&
                binding->document_digest().value() ==
                    xdg_generation_store_raw_contents_sha256(
                        pinned.published_record().raw_contents),
            "#411 pinned capability did not derive an exact record binding");
    const ReviewedSourceStateRecordBinding retained_binding = *binding;

    PinnedReviewedSourceBuild moved(std::move(pinned));
    require(!pinned.valid() && moved.valid(),
            "Pinned build move did not transfer one-shot authority");
    ReviewedSourceStateStoreReadResult after =
        read_reviewed_source_state(identity.package_base());
    const ReviewedSourceStateStoreRead& current =
        read_loaded(after, identity.target_revision());
    require(
        std::holds_alternative<ReviewedSourceStateRecordBindingMatch>(
            compare_reviewed_source_state_record_binding(
                retained_binding, current)),
        "derived #411 binding could not be reproven against current state");
    ReviewedSourceStateStoreRead wrong_generation = current;
    ++wrong_generation.observed->generation;
    require(
        require_arm<ReviewedSourceStateRecordBindingMismatch>(
            compare_reviewed_source_state_record_binding(
                retained_binding, wrong_generation),
            "#411 generation drift was accepted")
                .reason ==
            ReviewedSourceStateRecordBindingMismatchReason::
                ReviewedStateGenerationMismatch,
        "#411 generation drift returned the wrong mismatch");
    ReviewedSourceStateStoreRead wrong_document = current;
    wrong_document.observed->raw_contents += "# changed\n";
    require(
        require_arm<ReviewedSourceStateRecordBindingMismatch>(
            compare_reviewed_source_state_record_binding(
                retained_binding, wrong_document),
            "#411 raw document drift was accepted")
                .reason ==
            ReviewedSourceStateRecordBindingMismatchReason::
                ReviewedStateDocumentDigestMismatch,
        "#411 raw document drift returned the wrong mismatch");
    // Simulated later build failure: destroying build authority must not roll
    // back a successfully published reviewed revision.
}

void test_lease_contention_and_same_target_idempotence() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceTarget first = make_initial_accepted(identity);
    AcceptedReviewedSourceTarget contended_before =
        make_initial_accepted(identity);
    AcceptedReviewedSourceTarget contended_after =
        make_initial_accepted(identity);
    AcceptedReviewedSourceTarget stale_same =
        make_initial_accepted(identity);

    AcceptedReviewedSourceCheckout exact = materialize(
        std::move(first), fixture.checkout());
    AcceptedReviewedSourceCheckoutResult blocked_before =
        materialize_accepted_reviewed_source_checkout(
            std::move(contended_before), fixture.checkout());
    require(require_arm<ReviewedSourcePinnedCheckoutFailure>(
                blocked_before, "Concurrent checkout acquired held lease")
                    .reason ==
                ReviewedSourcePinnedCheckoutFailureReason::LeaseContended,
            "Lease contention returned the wrong typed failure");

    ReviewedSourcePublicationResult first_publication =
        publish_accepted_reviewed_source_checkout(std::move(exact));
    {
        PinnedReviewedSourceBuild pinned =
            take_arm<PinnedReviewedSourceBuild>(
                first_publication,
                "First same-target writer did not publish");
        AcceptedReviewedSourceCheckoutResult blocked_after =
            materialize_accepted_reviewed_source_checkout(
                std::move(contended_after), fixture.checkout());
        require(require_arm<ReviewedSourcePinnedCheckoutFailure>(
                    blocked_after,
                    "Pinned build did not retain PackageBase lease")
                        .reason ==
                    ReviewedSourcePinnedCheckoutFailureReason::
                        LeaseContended,
                "Pinned build lease contention returned wrong failure");
    }

    AcceptedReviewedSourceCheckout stale_exact = materialize(
        std::move(stale_same), fixture.checkout());
    ReviewedSourcePublicationResult idempotent =
        publish_accepted_reviewed_source_checkout(
            std::move(stale_exact));
    const auto& pinned = require_arm<PinnedReviewedSourceBuild>(
        idempotent, "Same-target stale CAS was not idempotent success");
    require(pinned.publication_status() ==
                    ReviewedSourcePublicationStatus::
                        AlreadyPublishedSameTarget &&
                pinned.published_record().generation == 1,
            "Same-target idempotence appended or misclassified state");
}

void test_different_target_stale_writer_conflicts() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity first_identity =
        fixture.identity(fixture.first_oid());
    const AurReviewedSourceReviewIdentity second_identity =
        fixture.identity(fixture.second_oid());
    AcceptedReviewedSourceTarget first =
        make_initial_accepted(first_identity);
    AcceptedReviewedSourceTarget stale_different =
        make_initial_accepted(second_identity);

    ReviewedSourcePublicationResult first_result =
        publish_accepted_reviewed_source_checkout(materialize(
            std::move(first), fixture.checkout()));
    {
        PinnedReviewedSourceBuild pinned =
            take_arm<PinnedReviewedSourceBuild>(
                first_result, "First writer did not publish");
    }

    AcceptedReviewedSourceCheckout second_checkout = materialize(
        std::move(stale_different), fixture.checkout());
    ReviewedSourcePublicationResult second_result =
        publish_accepted_reviewed_source_checkout(
            std::move(second_checkout));
    const auto& conflict = require_arm<ReviewedSourcePublicationConflict>(
        second_result, "Different-target stale writer did not conflict");
    const auto& loaded = require_arm<ReviewedSourceStateLoaded>(
        conflict.current.observation,
        "Different-target conflict lost current Loaded state");
    require(loaded.state.reviewed_revision() ==
                first_identity.target_revision(),
            "Different-target conflict overwrote newer reviewed state");
}

void test_already_reviewed_reconfirmation_rejects_newer_target() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity first_identity =
        fixture.identity(fixture.first_oid());
    const AurReviewedSourceReviewIdentity second_identity =
        fixture.identity(fixture.second_oid());
    {
        ReviewedSourcePublicationResult initial =
            publish_accepted_reviewed_source_checkout(materialize(
                make_initial_accepted(first_identity),
                fixture.checkout()));
        PinnedReviewedSourceBuild pinned =
            take_arm<PinnedReviewedSourceBuild>(
                initial, "Initial reviewed target did not publish");
        require(pinned.valid(), "Initial pinned build lost authority");
    }

    ReviewedSourceStateStoreReadResult first_read =
        read_reviewed_source_state(first_identity.package_base());
    const std::optional<ReviewedSourceStateObservedRecord> first_observed =
        require_arm<ReviewedSourceStateStoreRead>(
            first_read, "AlreadyReviewed setup lookup failed")
            .observed;
    ReviewedSourceLifecyclePlanResult planned =
        plan_reviewed_source_lifecycle(first_identity);
    ReviewedSourceAlreadyReviewedContinue already = take_arm<
        ReviewedSourceAlreadyReviewedContinue>(
        planned, "Loaded exact target was not AlreadyReviewed");
    AlreadyReviewedSourceCheckoutResult materialized =
        materialize_already_reviewed_source_checkout(
            std::move(already), fixture.checkout());
    AlreadyReviewedSourceCheckout exact =
        take_arm<AlreadyReviewedSourceCheckout>(
            materialized,
            "AlreadyReviewed target did not materialize");

    const ReviewedSourceState second_state = ReviewedSourceState::make(
        second_identity.package_base(), second_identity.target_revision());
    ReviewedSourceStateStorePublishResult concurrent =
        publish_reviewed_source_state(second_state, first_observed);
    require(std::holds_alternative<ReviewedSourceStateStorePublished>(
                concurrent),
            "Concurrent newer target fixture did not publish");

    ReviewedSourcePublicationResult confirmed =
        confirm_already_reviewed_source_checkout(std::move(exact));
    const auto& conflict = require_arm<ReviewedSourcePublicationConflict>(
        confirmed,
        "Stale AlreadyReviewed target produced pinned build authority");
    const auto& loaded = require_arm<ReviewedSourceStateLoaded>(
        conflict.current.observation,
        "Stale AlreadyReviewed conflict lost current target");
    require(loaded.state.reviewed_revision() ==
                second_identity.target_revision(),
            "AlreadyReviewed confirmation flattened the newer target");
}

void test_precommit_failure_and_published_uncertain() {
    {
        PinnedBuildFixture fixture;
        const AurReviewedSourceReviewIdentity identity =
            fixture.identity(fixture.first_oid());
        AcceptedReviewedSourceCheckout exact = materialize(
            make_initial_accepted(identity), fixture.checkout());
        fail_next_reviewed_source_state_store_operation_for_test(
            ReviewedSourceStateStoreTestFailurePoint::Sync);
        ReviewedSourcePublicationResult failed =
            publish_accepted_reviewed_source_checkout(std::move(exact));
        const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
            failed, "Precommit sync failure was not definite failure");
        require(failure.reason ==
                        ReviewedSourcePublicationFailureReason::StoreFailure &&
                    failure.store_failure.has_value() &&
                    failure.store_failure->kind ==
                        ReviewedSourceStateStoreFailureKind::SyncFailed,
                "Precommit failure classification drifted");
        ReviewedSourceStateStoreReadResult after =
            read_reviewed_source_state(identity.package_base());
        require(std::holds_alternative<ReviewedSourceStateMissing>(
                    require_arm<ReviewedSourceStateStoreRead>(
                        after, "Precommit lookup failed")
                        .observation),
                "Precommit failure advanced reviewed state");
        reset_reviewed_source_state_store_test_hooks();
    }

    {
        PinnedBuildFixture fixture;
        const AurReviewedSourceReviewIdentity identity =
            fixture.identity(fixture.first_oid());
        AcceptedReviewedSourceCheckout exact = materialize(
            make_initial_accepted(identity), fixture.checkout());
        fail_next_reviewed_source_state_store_operation_for_test(
            ReviewedSourceStateStoreTestFailurePoint::PostCommitVerify);
        ReviewedSourcePublicationResult uncertain =
            publish_accepted_reviewed_source_checkout(std::move(exact));
        require(std::holds_alternative<
                    ReviewedSourcePublicationUncertain>(uncertain),
                "Post-commit uncertainty was flattened or retried");
        ReviewedSourceStateStoreReadResult after =
            read_reviewed_source_state(identity.package_base());
        static_cast<void>(read_loaded(after, identity.target_revision()));
        ReviewedSourceLifecyclePlanResult recovered =
            plan_reviewed_source_lifecycle(identity);
        reset_reviewed_source_state_store_test_hooks();
        ReviewedSourceAlreadyReviewedContinue already = take_arm<
            ReviewedSourceAlreadyReviewedContinue>(
            recovered,
            "Uncertain publication recovery did not re-observe state");
        ReviewedSourceAlreadyReviewedContinue moved_already(
            std::move(already));
        AlreadyReviewedSourceCheckoutResult rejected =
            materialize_already_reviewed_source_checkout(
                std::move(already), fixture.checkout());
        require(require_arm<ReviewedSourcePinnedCheckoutFailure>(
                    rejected,
                    "Moved-from AlreadyReviewed produced checkout")
                        .reason ==
                    ReviewedSourcePinnedCheckoutFailureReason::
                        InvalidAlreadyReviewedCapability,
                "Moved-from AlreadyReviewed did not fail closed");
        AlreadyReviewedSourceCheckoutResult materialized =
            materialize_already_reviewed_source_checkout(
                std::move(moved_already), fixture.checkout());
        AlreadyReviewedSourceCheckout recovered_checkout =
            take_arm<AlreadyReviewedSourceCheckout>(
                materialized,
                "AlreadyReviewed recovery did not materialize");
        ReviewedSourcePublicationResult confirmed =
            confirm_already_reviewed_source_checkout(
                std::move(recovered_checkout));
        const auto& recovered_pinned = require_arm<
            PinnedReviewedSourceBuild>(
            confirmed,
            "AlreadyReviewed recovery did not produce pinned build");
        require(recovered_pinned.publication_status() ==
                        ReviewedSourcePublicationStatus::
                            AlreadyPublishedSameTarget &&
                    recovered_pinned.published_record().generation == 1,
                "AlreadyReviewed recovery rewrote state or lost authority");
    }
}

fs::path g_checkout_for_ignored_residue;

void add_ignored_residue_after_publication(
    const ReviewedSourceStateStoreTestRaceContext&) {
    std::ofstream output(
        g_checkout_for_ignored_residue /
            "ignored-after-publication.tmp",
        std::ios::binary | std::ios::trunc);
    output << "ignored residue\n";
    output.close();
}

void test_post_publication_checkout_failure_keeps_state() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceTarget accepted = make_initial_accepted(identity);
    AcceptedReviewedSourceTarget moved_accepted(std::move(accepted));
    AcceptedReviewedSourceCheckoutResult rejected_accepted =
        materialize_accepted_reviewed_source_checkout(
            std::move(accepted), fixture.checkout());
    require(require_arm<ReviewedSourcePinnedCheckoutFailure>(
                rejected_accepted,
                "Moved-from Accepted produced exact checkout")
                    .reason ==
                ReviewedSourcePinnedCheckoutFailureReason::
                    InvalidAcceptedCapability,
            "Moved-from Accepted did not fail closed");

    AcceptedReviewedSourceCheckout exact = materialize(
        std::move(moved_accepted), fixture.checkout());
    g_checkout_for_ignored_residue = fixture.repository();
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterPublication,
        add_ignored_residue_after_publication);
    ReviewedSourcePublicationResult result =
        publish_accepted_reviewed_source_checkout(std::move(exact));
    const auto& failure = require_arm<
        ReviewedSourcePostPublicationCheckoutFailure>(
        result,
        "Ignored post-publication residue returned build authority");
    require(failure.publication_status ==
                    ReviewedSourcePublicationStatus::Published &&
                failure.checkout_failure.reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        OverlayMismatch,
            "Post-publication checkout failure lost definite state outcome");
    ReviewedSourceStateStoreReadResult after =
        read_reviewed_source_state(identity.package_base());
    static_cast<void>(read_loaded(after, identity.target_revision()));
    reset_reviewed_source_state_store_test_hooks();
    g_checkout_for_ignored_residue.clear();
}

void test_editor_overlay_is_explicit_build_provenance() {
    {
        PinnedBuildFixture fixture;
        const AurReviewedSourceReviewIdentity identity =
            fixture.identity(fixture.first_oid());
        AcceptedReviewedSourceCheckout exact = materialize(
            make_initial_accepted(identity), fixture.checkout());
        fixture.write_file(
            "PKGBUILD",
            "pkgname=fixture\npkgver=edited\npkgrel=1\n");
        ReviewedSourcePublicationResult rejected =
            publish_accepted_reviewed_source_checkout(
                std::move(exact));
        const auto& failure = require_arm<
            ReviewedSourcePublicationFailure>(
            rejected,
            "Dirty editor tree was flattened into exact build authority");
        require(
            failure.reason == ReviewedSourcePublicationFailureReason::
                                  CheckoutRevalidationFailed &&
                failure.checkout_failure.has_value() &&
                failure.checkout_failure->reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        DirtyWorktree,
            "Exact publication did not reject an untyped editor overlay");
    }

    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary =
        begin_editor_boundary(exact);
    fixture.write_file(
        "PKGBUILD",
        "pkgname=fixture\npkgver=edited\npkgrel=1\n");
    ReviewedSourceEditorOverlayProof overlay = seal_editor_overlay(
        exact, std::move(boundary));
    ReviewedSourcePublicationResult published =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(exact),
            std::move(overlay));
    const auto& pinned = require_arm<PinnedReviewedSourceBuild>(
        published, "Typed editor overlay did not produce build authority");
    require(
        pinned.editor_overlay_status() ==
                ReviewedSourceEditorOverlayStatus::
                    InvocationLocal &&
            pinned.reviewed_upstream_base_revision() ==
                identity.target_revision() &&
            fixture.read_file("PKGBUILD").find("pkgver=edited") != std::string::npos,
        "Editor overlay was reported as an exact commit tree");
    const ReviewedSourceStateRecordBindingResult overlay_binding =
        derive_reviewed_source_state_record_binding(pinned);
    const auto& binding_failure =
        require_arm<ReviewedSourceStateRecordBindingFailure>(
            overlay_binding,
            "editor overlay minted provenance reviewed-state authority");
    require(binding_failure.reason ==
                ReviewedSourceStateRecordBindingFailureReason::
                    EditorOverlayPresent,
            "editor overlay binding rejection kind drifted");
}

void test_noop_editor_seals_no_overlay() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary =
        begin_editor_boundary(exact);
    ReviewedSourceEditorOverlayProof overlay = seal_editor_overlay(
        exact, std::move(boundary));
    require(overlay.status() == ReviewedSourceEditorOverlayStatus::None,
            "No-op editor was classified as a dirty overlay");
    ReviewedSourcePublicationResult published =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(exact), std::move(overlay));
    const auto& pinned = require_arm<PinnedReviewedSourceBuild>(
        published, "No-op editor did not retain build authority");
    require(pinned.editor_overlay_status() ==
                ReviewedSourceEditorOverlayStatus::None,
            "No-op editor provenance changed after publication");
}

void test_overlay_mutation_after_seal_is_rejected_before_publication() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary =
        begin_editor_boundary(exact);
    fixture.write_file(
        "PKGBUILD",
        "pkgname=fixture\npkgver=edited\npkgrel=1\n");
    fixture.write_file("ignored-editor.tmp", "sealed ignored bytes\n");
    ReviewedSourceEditorOverlayProof overlay = seal_editor_overlay(
        exact, std::move(boundary));
    fixture.write_file("ignored-editor.tmp", "external mutation\n");

    ReviewedSourcePublicationResult rejected =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(exact), std::move(overlay));
    const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
        rejected,
        "Post-editor ignored-file mutation produced build authority");
    require(
        failure.reason == ReviewedSourcePublicationFailureReason::
                              CheckoutRevalidationFailed &&
            failure.checkout_failure.has_value() &&
            failure.checkout_failure->reason ==
                TrustedGitPinnedCheckoutFailureReason::
                    OverlayMismatch,
        "Post-editor exact overlay drift was not rejected");
    ReviewedSourceStateStoreReadResult after =
        read_reviewed_source_state(identity.package_base());
    require(std::holds_alternative<ReviewedSourceStateMissing>(
                require_arm<ReviewedSourceStateStoreRead>(
                    after, "Overlay drift lookup failed")
                    .observation),
            "Pre-CAS overlay drift advanced reviewed state");
}

void test_empty_directory_after_seal_is_rejected_before_cas() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary = begin_editor_boundary(exact);
    ReviewedSourceEditorOverlayProof overlay = seal_no_editor_overlay(
        exact, std::move(boundary));
    const std::string sealed_git_tree = fixture.output_git({"write-tree"});
    const fs::path empty_directory = fixture.repository() / "empty-dir";
    fs::create_directory(empty_directory);
    require(::chmod(empty_directory.c_str(), 0700) == 0,
            "Failed to secure empty-directory overlay fixture");
    require(fixture.output_git({"write-tree"}) == sealed_git_tree,
            "Empty directory unexpectedly changed the Git tree oracle");

    ReviewedSourcePublicationResult rejected =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(exact), std::move(overlay));
    const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
        rejected, "Empty directory produced reviewed build authority");
    require(failure.checkout_failure.has_value() &&
                failure.checkout_failure->reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        OverlayMismatch,
            "Empty-directory manifest drift lost overlay mismatch");
    ReviewedSourceStateStoreReadResult after =
        read_reviewed_source_state(identity.package_base());
    require(std::holds_alternative<ReviewedSourceStateMissing>(
                require_arm<ReviewedSourceStateStoreRead>(
                    after, "Empty-directory drift lookup failed")
                    .observation),
            "Empty-directory pre-CAS drift advanced reviewed state");
}

void test_manifest_change_during_git_projection_is_not_sealed() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary = begin_editor_boundary(exact);
    const fs::path wrapper = fixture.fixture_root() /
                             "git-overlay-stability-wrapper";
    const fs::path marker = fixture.fixture_root() /
                            "overlay-stability-race-fired";
    const fs::path raced_directory = fixture.repository() /
                                     "raced-empty-dir";
    {
        std::ofstream script(wrapper, std::ios::binary | std::ios::trunc);
        script << R"(#!/bin/sh
set -eu
if [ "${MOGUET_TEST_TRUSTED_GIT_DISPLAY_COMMAND:-}" = "git hash-object -w -t tree /dev/null" ] && [ ! -e "$MOGUET_TEST_OVERLAY_RACE_MARKER" ]; then
    /usr/bin/touch "$MOGUET_TEST_OVERLAY_RACE_MARKER"
    /usr/bin/mkdir --mode=0700 -- "$MOGUET_TEST_OVERLAY_RACE_DIRECTORY"
fi
exec /usr/bin/git "$@"
)";
        script.close();
        require(static_cast<bool>(script) &&
                    ::chmod(wrapper.c_str(), 0700) == 0,
                "Failed to create overlay stability Git wrapper");
    }
    require(::setenv(
                "MOGUET_TEST_GIT_EXECUTABLE", wrapper.c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_OVERLAY_RACE_MARKER",
                    marker.c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_OVERLAY_RACE_DIRECTORY",
                    raced_directory.c_str(), 1) == 0,
            "Failed to configure overlay stability Git wrapper");
    ReviewedSourceEditorOverlayProofResult sealed =
        seal_reviewed_source_no_editor_overlay(
            exact, std::move(boundary));
    static_cast<void>(::unsetenv("MOGUET_TEST_GIT_EXECUTABLE"));
    static_cast<void>(::unsetenv("MOGUET_TEST_OVERLAY_RACE_MARKER"));
    static_cast<void>(::unsetenv("MOGUET_TEST_OVERLAY_RACE_DIRECTORY"));

    const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
        sealed,
        "Git-invisible mutation during projection was sealed");
    require(fs::exists(marker) && fs::is_directory(raced_directory) &&
                failure.checkout_failure.has_value() &&
                failure.checkout_failure->reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        OverlayMismatch,
            "Manifest-before/after stability mismatch was not retained");
}

fs::path g_checkout_for_mode_mutation;

void chmod_regular_file_after_publication(
    const ReviewedSourceStateStoreTestRaceContext&) {
    if(::chmod(
           (g_checkout_for_mode_mutation / "PKGBUILD").c_str(),
           0600) != 0) {
        throw std::runtime_error(
            "Failed to mutate reviewed overlay file mode");
    }
}

void test_mode_mutation_after_cas_keeps_publication_and_stops_pinned_build() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary = begin_editor_boundary(exact);
    ReviewedSourceEditorOverlayProof overlay = seal_no_editor_overlay(
        exact, std::move(boundary));
    const std::string sealed_git_tree = fixture.output_git({"write-tree"});
    g_checkout_for_mode_mutation = fixture.repository();
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterPublication,
        chmod_regular_file_after_publication);

    ReviewedSourcePublicationResult rejected =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(exact), std::move(overlay));
    const auto& failure = require_arm<
        ReviewedSourcePostPublicationCheckoutFailure>(
        rejected,
        "Post-CAS chmod produced pinned reviewed build authority");
    require(failure.publication_status ==
                    ReviewedSourcePublicationStatus::Published &&
                failure.checkout_failure.reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        OverlayMismatch &&
                fixture.output_git({"write-tree"}) == sealed_git_tree,
            "Post-CAS chmod was not isolated from Git tree authority");
    ReviewedSourceStateStoreReadResult after =
        read_reviewed_source_state(identity.package_base());
    static_cast<void>(read_loaded(after, identity.target_revision()));
    reset_reviewed_source_state_store_test_hooks();
    g_checkout_for_mode_mutation.clear();
}

void test_index_and_head_mutation_after_overlay_seal_are_rejected() {
    {
        PinnedBuildFixture fixture;
        const AurReviewedSourceReviewIdentity identity =
            fixture.identity(fixture.first_oid());
        AcceptedReviewedSourceCheckout exact = materialize(
            make_initial_accepted(identity), fixture.checkout());
        ReviewedSourceEditorBoundary boundary =
            begin_editor_boundary(exact);
        fixture.write_file(
            "PKGBUILD",
            "pkgname=fixture\npkgver=edited\npkgrel=1\n");
        ReviewedSourceEditorOverlayProof overlay = seal_editor_overlay(
            exact, std::move(boundary));
        fixture.run_git({"add", "PKGBUILD"});
        ReviewedSourcePublicationResult rejected =
            publish_accepted_reviewed_source_checkout_with_editor_overlay(
                std::move(exact), std::move(overlay));
        const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
            rejected, "Index mutation produced build authority");
        require(failure.checkout_failure.has_value() &&
                    failure.checkout_failure->reason ==
                        TrustedGitPinnedCheckoutFailureReason::
                            DirtyIndex,
                "Index mutation lost its typed rejection");
    }

    {
        PinnedBuildFixture fixture;
        const AurReviewedSourceReviewIdentity identity =
            fixture.identity(fixture.first_oid());
        AcceptedReviewedSourceCheckout exact = materialize(
            make_initial_accepted(identity), fixture.checkout());
        ReviewedSourceEditorBoundary boundary =
            begin_editor_boundary(exact);
        ReviewedSourceEditorOverlayProof overlay = seal_no_editor_overlay(
            exact, std::move(boundary));
        fixture.run_git({"checkout", "--detach", "--force",
                         fixture.second_oid()});
        ReviewedSourcePublicationResult rejected =
            publish_accepted_reviewed_source_checkout_with_editor_overlay(
                std::move(exact), std::move(overlay));
        const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
            rejected, "HEAD mutation produced build authority");
        require(failure.checkout_failure.has_value() &&
                    failure.checkout_failure->reason ==
                        TrustedGitPinnedCheckoutFailureReason::
                            TargetRevisionMismatch,
                "HEAD mutation lost its typed rejection");
    }
}

void test_checkout_replacement_after_overlay_seal_is_rejected() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary =
        begin_editor_boundary(exact);
    ReviewedSourceEditorOverlayProof overlay = seal_no_editor_overlay(
        exact, std::move(boundary));

    fs::path moved = fixture.repository();
    moved += ".replaced-original";
    fs::rename(fixture.repository(), moved);
    fs::create_directory(fixture.repository());
    require(::chmod(fixture.repository().c_str(), 0700) == 0,
            "Failed to secure replacement checkout");

    ReviewedSourcePublicationResult rejected =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(exact), std::move(overlay));
    const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
        rejected, "Checkout replacement produced build authority");
    require(failure.checkout_failure.has_value() &&
                failure.checkout_failure->reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        CheckoutBoundaryInvalid,
            "Checkout replacement lost its typed rejection");
}

void test_embedded_repository_cannot_hide_overlay_files() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary =
        begin_editor_boundary(exact);
    fixture.run_git({"init", "-q", "embedded-helper"});
    fixture.write_file(
        "embedded-helper/helper.sh", "#!/bin/sh\nexit 0\n");
    fixture.run_git({"-C", "embedded-helper", "add", "helper.sh"});
    fixture.run_git({"-C", "embedded-helper", "commit", "-q", "-m",
                     "embedded helper"});
    ReviewedSourceEditorOverlayProofResult sealed =
        seal_reviewed_source_editor_overlay(
            exact, std::move(boundary));
    const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
        sealed, "Embedded repository was sealed as exact overlay input");
    require(failure.checkout_failure.has_value() &&
                failure.checkout_failure->reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        UnsupportedOverlayEntry,
            "Embedded repository did not fail closed at overlay observation");
}

void test_special_and_oversized_overlay_entries_fail_closed() {
    {
        PinnedBuildFixture fixture;
        const AurReviewedSourceReviewIdentity identity =
            fixture.identity(fixture.first_oid());
        AcceptedReviewedSourceCheckout exact = materialize(
            make_initial_accepted(identity), fixture.checkout());
        ReviewedSourceEditorBoundary boundary = begin_editor_boundary(exact);
        const fs::path fifo = fixture.repository() / "build-input.fifo";
        require(::mkfifo(fifo.c_str(), 0600) == 0,
                "Failed to create special overlay fixture");
        ReviewedSourceEditorOverlayProofResult sealed =
            seal_reviewed_source_editor_overlay(
                exact, std::move(boundary));
        const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
            sealed, "FIFO was read or sealed as overlay input");
        require(failure.checkout_failure.has_value() &&
                    failure.checkout_failure->reason ==
                        TrustedGitPinnedCheckoutFailureReason::
                            UnsupportedOverlayEntry,
                "FIFO did not fail closed before Git projection");
    }

    {
        PinnedBuildFixture fixture;
        const AurReviewedSourceReviewIdentity identity =
            fixture.identity(fixture.first_oid());
        AcceptedReviewedSourceCheckout exact = materialize(
            make_initial_accepted(identity), fixture.checkout());
        ReviewedSourceEditorBoundary boundary = begin_editor_boundary(exact);
        const fs::path oversized = fixture.repository() / "oversized.input";
        const int descriptor = ::open(
            oversized.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            0600);
        require(descriptor >= 0,
                "Failed to create oversized overlay fixture");
        require(::ftruncate(
                    descriptor,
                    static_cast<off_t>(
                        REVIEWED_SOURCE_SINGLE_BLOB_LIMIT + 1)) == 0,
                "Failed to size oversized overlay fixture");
        static_cast<void>(::close(descriptor));
        ReviewedSourceEditorOverlayProofResult sealed =
            seal_reviewed_source_editor_overlay(
                exact, std::move(boundary));
        const auto& failure = require_arm<ReviewedSourcePublicationFailure>(
            sealed, "Oversized file was sealed as overlay input");
        require(failure.checkout_failure.has_value() &&
                    failure.checkout_failure->reason ==
                        TrustedGitPinnedCheckoutFailureReason::
                            CaptureLimitExceeded,
                "Oversized regular file did not enforce manifest bounds");
    }
}

void test_makepkg_boundary_reproof_rejects_post_cas_mutation() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary =
        begin_editor_boundary(exact);
    fixture.write_file(
        "PKGBUILD",
        "pkgname=fixture\npkgver=edited\npkgrel=1\n");
    ReviewedSourceEditorOverlayProof overlay = seal_editor_overlay(
        exact, std::move(boundary));
    ReviewedSourcePublicationResult published =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(exact), std::move(overlay));
    PinnedReviewedSourceBuild pinned = take_arm<PinnedReviewedSourceBuild>(
        published, "Overlay publication did not produce pinned build");

    fixture.write_file(
        "PKGBUILD",
        "pkgname=fixture\npkgver=external\npkgrel=1\n");
    const fs::path marker = fixture.fixture_root() / "makepkg-invoked";
    try {
        static_cast<void>(pinned.run_guarded_command(
            "touch " + marker.string()));
        throw std::runtime_error(
            "Post-CAS mutation reached the makepkg command boundary");
    } catch(const ReviewedSourceBuildCheckoutReproofError& error) {
        require(error.failure().reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        OverlayMismatch,
                "Post-CAS mutation lost overlay mismatch detail");
    }
    require(!fs::exists(marker),
            "Post-CAS overlay drift invoked the guarded command");
    ReviewedSourceStateStoreReadResult after =
        read_reviewed_source_state(identity.package_base());
    static_cast<void>(read_loaded(after, identity.target_revision()));
}

void test_status_zero_post_command_revalidation_keeps_publication() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary = begin_editor_boundary(exact);
    ReviewedSourceEditorOverlayProof overlay = seal_no_editor_overlay(
        exact, std::move(boundary));
    ReviewedSourcePublicationResult published =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(exact), std::move(overlay));
    PinnedReviewedSourceBuild pinned = take_arm<PinnedReviewedSourceBuild>(
        published, "Clean publication did not produce pinned build");

    const fs::path displaced =
        fixture.fixture_root() / "status-zero-reviewed-checkout";
    const std::string command =
        "/usr/bin/mv -- " + shell_quote_path(fixture.repository()) +
        " " + shell_quote_path(displaced) +
        " && /usr/bin/mkdir --mode=0700 -- " +
        shell_quote_path(fixture.repository());
    bool failure_reported = false;
    try {
        static_cast<void>(pinned.run_guarded_command(command));
    } catch(const ProductionSourceBuildPostCommandRevalidationError& error) {
        failure_reported = true;
        require(
            error.command_exit_status() == 0,
            "Reviewed post-command revalidation changed status zero");
        bool nested_failure_retained = false;
        try {
            error.rethrow_failure();
        } catch(...) {
            nested_failure_retained = true;
        }
        require(
            nested_failure_retained,
            "Reviewed post-command revalidation lost identity failure");
    }
    require(
        failure_reported,
        "Reviewed status-zero command returned through failed revalidation");
    require(
        pinned.valid() &&
            pinned.publication_status() ==
                ReviewedSourcePublicationStatus::Published &&
            pinned.identity() == identity &&
            fs::is_directory(fixture.repository()) &&
            fs::is_regular_file(displaced / "PKGBUILD"),
        "Reviewed status-zero failure lost publication or checkout evidence");
    ReviewedSourceStateStoreReadResult after =
        read_reviewed_source_state(identity.package_base());
    static_cast<void>(read_loaded(after, identity.target_revision()));
}

void test_nested_dot_git_input_is_rejected_at_makepkg_boundary() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    ReviewedSourceEditorBoundary boundary = begin_editor_boundary(exact);
    ReviewedSourceEditorOverlayProof overlay = seal_no_editor_overlay(
        exact, std::move(boundary));
    ReviewedSourcePublicationResult published =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(exact), std::move(overlay));
    PinnedReviewedSourceBuild pinned = take_arm<PinnedReviewedSourceBuild>(
        published, "Clean overlay did not produce pinned build");

    const std::string sealed_git_tree = fixture.output_git({"write-tree"});
    fixture.write_file(
        "helper/.git/build-input", "nested metadata build input\n");
    require(fixture.output_git({"write-tree"}) == sealed_git_tree,
            "Nested .git input unexpectedly changed the Git tree oracle");
    const fs::path marker = fixture.fixture_root() /
                            "nested-dot-git-makepkg-invoked";
    try {
        static_cast<void>(pinned.run_guarded_command(
            "touch " + marker.string()));
        throw std::runtime_error(
            "Nested .git input reached the makepkg command boundary");
    } catch(const ReviewedSourceBuildCheckoutReproofError& error) {
        require(error.failure().reason ==
                        TrustedGitPinnedCheckoutFailureReason::
                            OverlayMismatch ||
                    error.failure().reason ==
                        TrustedGitPinnedCheckoutFailureReason::
                            UnsupportedOverlayEntry,
                "Nested .git input lost typed fail-closed detail");
    }
    require(!fs::exists(marker),
            "Nested .git input invoked the guarded build command");
    ReviewedSourceStateStoreReadResult after =
        read_reviewed_source_state(identity.package_base());
    static_cast<void>(read_loaded(after, identity.target_revision()));
}

fs::path prepare_raw_state_directory(
    const PinnedBuildFixture& fixture,
    const PackageBaseIdentity& package_base) {
    const fs::path entry =
        reviewed_source_state_store_entry_path(package_base);
    fs::create_directories(entry);
    // create_directories used the process umask; explicitly secure every
    // managed descendant used by the store fixture.
    for(fs::path current = entry;
        current != fixture.state_home();
        current = current.parent_path()) {
        require(::chmod(current.c_str(), 0700) == 0,
                "Failed to secure future-state fixture path");
    }
    return entry;
}

void create_raw_origin(
    const PinnedBuildFixture& fixture,
    const PackageBaseIdentity& package_base,
    std::string_view contents) {
    const fs::path entry =
        prepare_raw_state_directory(fixture, package_base);
    const fs::path origin = entry / reviewed_source_state_store_origin_leaf();
    std::ofstream output(origin, std::ios::binary | std::ios::trunc);
    output.write(
        contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    require(static_cast<bool>(output), "Failed to write raw state fixture");
    require(::chmod(origin.c_str(), 0600) == 0,
            "Failed to secure raw state fixture");
}

void test_future_state_is_not_overwritten() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    create_raw_origin(
        fixture, identity.package_base(),
        "schema_version = 2\nnext_field = true\n");
    ReviewedSourcePublicationResult result =
        publish_accepted_reviewed_source_checkout(std::move(exact));
    const auto& conflict = require_arm<ReviewedSourcePublicationConflict>(
        result, "Future state was not a fail-closed CAS conflict");
    require(std::holds_alternative<ReviewedSourceStateUnsupportedFuture>(
                conflict.current.observation),
            "Future state conflict was flattened");
    require(conflict.current.observed.has_value() &&
                conflict.current.observed->generation == 1,
            "Future state was overwritten or advanced");
}

void test_abnormal_history_uses_exact_cas_rebind() {
    const auto exercise = [](
                              const std::string& raw_document,
                              auto observation_predicate,
                              std::string_view label) {
        PinnedBuildFixture fixture;
        const AurReviewedSourceReviewIdentity identity =
            fixture.identity(fixture.first_oid());
        create_raw_origin(
            fixture, identity.package_base(), raw_document);
        ReviewedSourceStateStoreReadResult before =
            read_reviewed_source_state(identity.package_base());
        const auto& before_read = require_arm<ReviewedSourceStateStoreRead>(
            before, "Abnormal state lookup failed");
        require(observation_predicate(before_read.observation), label);

        AcceptedReviewedSourceCheckout exact = materialize(
            make_initial_accepted(identity), fixture.checkout());
        ReviewedSourcePublicationResult result =
            publish_accepted_reviewed_source_checkout(std::move(exact));
        const auto& pinned = require_arm<PinnedReviewedSourceBuild>(
            result, "Abnormal-state rebind did not publish");
        require(pinned.publication_status() ==
                        ReviewedSourcePublicationStatus::Published &&
                    pinned.published_record().generation == 2,
                "Abnormal-state rebind lost exact predecessor CAS");
        ReviewedSourceStateStoreReadResult after =
            read_reviewed_source_state(identity.package_base());
        static_cast<void>(read_loaded(after, identity.target_revision()));
    };

    exercise(
        "schema_version = 1\n",
        [](const ReviewedSourceStateObservation& observation) {
            return std::holds_alternative<ReviewedSourceStateInvalid>(
                observation);
        },
        "Invalid history was flattened before rebind");
    exercise(
        "",
        [](const ReviewedSourceStateObservation& observation) {
            return std::holds_alternative<ReviewedSourceStateCorrupted>(
                observation);
        },
        "Corrupted history was flattened before rebind");

    PinnedBuildFixture mismatch_fixture;
    const AurReviewedSourceReviewIdentity mismatch_identity =
        mismatch_fixture.identity(mismatch_fixture.first_oid());
    const PackageBaseIdentity other_package_base = PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(
                "https://aur.archlinux.org/other-base.git")),
        "other-base");
    create_raw_origin(
        mismatch_fixture, mismatch_identity.package_base(),
        encode_reviewed_source_state(ReviewedSourceState::make(
            other_package_base,
            mismatch_identity.target_revision())));
    ReviewedSourceStateStoreReadResult mismatch_before =
        read_reviewed_source_state(mismatch_identity.package_base());
    require(std::holds_alternative<ReviewedSourceStateSourceMismatch>(
                require_arm<ReviewedSourceStateStoreRead>(
                    mismatch_before,
                    "Source-mismatch state lookup failed")
                    .observation),
            "Source-mismatch history was flattened before rebind");
    ReviewedSourcePublicationResult mismatch_result =
        publish_accepted_reviewed_source_checkout(materialize(
            make_initial_accepted(mismatch_identity),
            mismatch_fixture.checkout()));
    const auto& mismatch_pinned = require_arm<PinnedReviewedSourceBuild>(
        mismatch_result, "Source-mismatch rebind did not publish");
    require(mismatch_pinned.published_record().generation == 2,
            "Source-mismatch rebind did not preserve exact CAS chain");
}

void test_unsafe_history_is_not_flattened() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    const fs::path entry =
        prepare_raw_state_directory(fixture, identity.package_base());
    const fs::path unexpected = entry / "unexpected-managed-entry";
    std::ofstream output(unexpected, std::ios::binary | std::ios::trunc);
    output << "unexpected\n";
    output.close();
    require(static_cast<bool>(output) && ::chmod(unexpected.c_str(), 0600) == 0,
            "Failed to create unsafe-history fixture");
    ReviewedSourcePublicationResult result =
        publish_accepted_reviewed_source_checkout(std::move(exact));
    require(std::holds_alternative<
                ReviewedSourcePublicationUnsafeHistory>(result),
            "Unsafe history was flattened into publication or failure");
}

void test_sha256_and_object_format_binding() {
    PinnedBuildFixture fixture(GitObjectFormat::Sha256);
    const AurReviewedSourceReviewIdentity valid =
        fixture.identity(fixture.first_oid());
    const AurReviewedSourceReviewIdentity wrong_format =
        fixture.identity(std::string(40, 'a'));
    AcceptedReviewedSourceTarget invalid_accepted =
        make_initial_accepted(wrong_format);
    AcceptedReviewedSourceTarget valid_accepted =
        make_initial_accepted(valid);

    AcceptedReviewedSourceCheckoutResult rejected =
        materialize_accepted_reviewed_source_checkout(
            std::move(invalid_accepted), fixture.checkout());
    const auto& failure = require_arm<ReviewedSourcePinnedCheckoutFailure>(
        rejected, "SHA-1 identity was accepted by SHA-256 checkout");
    require(failure.reason ==
                    ReviewedSourcePinnedCheckoutFailureReason::
                        GitMaterializationFailed &&
                failure.git_failure.has_value() &&
                failure.git_failure->reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        ObjectFormatMismatch,
            "Object-format mismatch lost typed proof");

    ReviewedSourcePublicationResult published =
        publish_accepted_reviewed_source_checkout(materialize(
            std::move(valid_accepted), fixture.checkout()));
    const auto& pinned = require_arm<PinnedReviewedSourceBuild>(
        published, "SHA-256 exact target did not publish");
    require(pinned.identity().git_object_format() == GitObjectFormat::Sha256,
            "Pinned build lost SHA-256 object-format identity");
}

std::string read_path(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if(!input) throw std::runtime_error("Failed to read replacement fixture");
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void test_named_checkout_replacement_cannot_receive_git_mutation() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceTarget accepted = make_initial_accepted(identity);
    const fs::path wrapper = fixture.fixture_root() / "git-race-wrapper";
    const fs::path moved = fixture.fixture_root() / "moved-checkout";
    const fs::path marker = fixture.fixture_root() / "race-fired";
    {
        std::ofstream script(wrapper, std::ios::binary | std::ios::trunc);
        script << R"(#!/bin/sh
set -eu
if [ "${MOGUET_TEST_TRUSTED_GIT_DISPLAY_COMMAND:-}" = "git checkout --detach --force <pinned-commit>" ] && [ ! -e "$MOGUET_TEST_PINNED_RACE_MARKER" ]; then
    /usr/bin/touch "$MOGUET_TEST_PINNED_RACE_MARKER"
    /usr/bin/mv -- "$MOGUET_TEST_PINNED_RACE_ORIGINAL" "$MOGUET_TEST_PINNED_RACE_MOVED"
    /usr/bin/mkdir --mode=0700 -- "$MOGUET_TEST_PINNED_RACE_ORIGINAL"
    /usr/bin/printf '%s\n' replacement >"$MOGUET_TEST_PINNED_RACE_ORIGINAL/sentinel"
fi
exec /usr/bin/git "$@"
)";
        script.close();
        require(static_cast<bool>(script) && ::chmod(wrapper.c_str(), 0700) == 0,
                "Failed to create Git replacement-race wrapper");
    }
    require(::setenv(
                "MOGUET_TEST_GIT_EXECUTABLE",
                wrapper.c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PINNED_RACE_ORIGINAL",
                    fixture.repository().c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PINNED_RACE_MOVED",
                    moved.c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PINNED_RACE_MARKER",
                    marker.c_str(), 1) == 0,
            "Failed to configure Git replacement-race wrapper");

    AcceptedReviewedSourceCheckoutResult result =
        materialize_accepted_reviewed_source_checkout(
            std::move(accepted), fixture.checkout());
    static_cast<void>(::unsetenv("MOGUET_TEST_GIT_EXECUTABLE"));
    static_cast<void>(::unsetenv("MOGUET_TEST_PINNED_RACE_ORIGINAL"));
    static_cast<void>(::unsetenv("MOGUET_TEST_PINNED_RACE_MOVED"));
    static_cast<void>(::unsetenv("MOGUET_TEST_PINNED_RACE_MARKER"));

    const auto& failure = require_arm<ReviewedSourcePinnedCheckoutFailure>(
        result, "Named replacement produced exact checkout capability");
    require(failure.reason ==
                    ReviewedSourcePinnedCheckoutFailureReason::
                        GitMaterializationFailed &&
                failure.git_failure.has_value() &&
                failure.git_failure->reason ==
                    TrustedGitPinnedCheckoutFailureReason::
                        CheckoutBoundaryInvalid,
            "Named replacement returned the wrong checkout failure");
    require(fs::exists(marker) && fs::exists(moved / ".git") &&
                read_path(fixture.repository() / "sentinel") ==
                    "replacement\n",
            "Git mutation followed or damaged the replacement checkout");
    require(read_path(moved / "PKGBUILD").find("pkgver=1") !=
                std::string::npos,
            "Descriptor-bound Git did not mutate the retained original");
    ReviewedSourceStateStoreReadResult state =
        read_reviewed_source_state(identity.package_base());
    require(std::holds_alternative<ReviewedSourceStateMissing>(
                require_arm<ReviewedSourceStateStoreRead>(
                    state, "Replacement-race state lookup failed")
                    .observation),
            "Checkout replacement failure advanced reviewed state");
}

void write_pipe_byte(int descriptor, char value) {
    require(::write(descriptor, &value, 1) == 1,
            "Failed to write lease synchronization byte");
}

void read_pipe_byte(int descriptor) {
    char value = 0;
    require(::read(descriptor, &value, 1) == 1,
            "Failed to read lease synchronization byte");
}

enum class GuardedMutatorDeathTarget {
    OuterParent,
    Supervisor,
};

void test_parent_and_supervisor_death_during_external_mutator_keeps_lease() {
    const auto exercise = [](
                              std::string_view blocked_display_command,
                              GuardedMutatorDeathTarget death_target) {
        PinnedBuildFixture fixture;
        const AurReviewedSourceReviewIdentity identity =
            fixture.identity(fixture.first_oid());
        AcceptedReviewedSourceTarget worker_target =
            make_initial_accepted(identity);
        const fs::path wrapper =
            fixture.fixture_root() / "git-parent-death-wrapper";
        const fs::path ready_fifo = fixture.fixture_root() / "mutator-ready";
        const fs::path release_fifo =
            fixture.fixture_root() / "mutator-release";
        const fs::path blocked_once =
            fixture.fixture_root() / "mutator-blocked-once";
        const fs::path completed =
            fixture.fixture_root() / "mutator-completed";
        const fs::path descriptor_missing =
            fixture.fixture_root() / "mutator-descriptor-missing";
        const fs::path supervisor_pid_file =
            fixture.fixture_root() / "mutator-supervisor-pid";
        {
            std::ofstream script(wrapper, std::ios::binary | std::ios::trunc);
            script << R"(#!/bin/sh
set -eu
blocked=0
if [ "${MOGUET_TEST_TRUSTED_GIT_DISPLAY_COMMAND:-}" = "$MOGUET_TEST_PARENT_DEATH_STAGE" ] && /usr/bin/mkdir --mode=0700 -- "$MOGUET_TEST_PARENT_DEATH_ONCE" 2>/dev/null; then
    blocked=1
    lease_found=0
    for descriptor in /proc/self/fd/*; do
        target=$(/usr/bin/readlink -- "$descriptor" 2>/dev/null || true)
        if [ "$target" = "$MOGUET_TEST_PARENT_DEATH_CHECKOUT" ]; then
            lease_found=1
        fi
    done
    if [ "$lease_found" -ne 1 ]; then
        /usr/bin/touch -- "$MOGUET_TEST_PARENT_DEATH_DESCRIPTOR_MISSING"
    fi
    /bin/sh -c '
        lease_found=0
        for descriptor in /proc/self/fd/*; do
            target=$(/usr/bin/readlink -- "$descriptor" 2>/dev/null || true)
            if [ "$target" = "$MOGUET_TEST_PARENT_DEATH_CHECKOUT" ]; then
                lease_found=1
            fi
        done
        if [ "$lease_found" -ne 1 ]; then
            /usr/bin/touch -- "$MOGUET_TEST_PARENT_DEATH_DESCRIPTOR_MISSING"
        fi
    '
    /usr/bin/printf '%s' "$PPID" >"$MOGUET_TEST_PARENT_DEATH_SUPERVISOR_PID"
    /usr/bin/printf x >"$MOGUET_TEST_PARENT_DEATH_READY"
    IFS= read -r _ <"$MOGUET_TEST_PARENT_DEATH_RELEASE" || true
fi
set +e
/usr/bin/git "$@"
status=$?
set -e
if [ "$blocked" -eq 1 ]; then
    /usr/bin/touch -- "$MOGUET_TEST_PARENT_DEATH_COMPLETED"
fi
exit "$status"
)";
            script.close();
            require(
                static_cast<bool>(script) &&
                    ::chmod(wrapper.c_str(), 0700) == 0,
                "Failed to create parent-death Git wrapper");
        }
        require(
            ::mkfifo(ready_fifo.c_str(), 0600) == 0 &&
                ::mkfifo(release_fifo.c_str(), 0600) == 0,
            "Failed to create parent-death synchronization FIFOs");
        require(
            ::setenv("MOGUET_TEST_GIT_EXECUTABLE", wrapper.c_str(), 1) ==
                    0 &&
                ::setenv(
                    "MOGUET_TEST_PARENT_DEATH_STAGE",
                    std::string(blocked_display_command).c_str(),
                    1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PARENT_DEATH_ONCE",
                    blocked_once.c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PARENT_DEATH_READY",
                    ready_fifo.c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PARENT_DEATH_RELEASE",
                    release_fifo.c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PARENT_DEATH_COMPLETED",
                    completed.c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PARENT_DEATH_CHECKOUT",
                    fixture.repository().c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PARENT_DEATH_DESCRIPTOR_MISSING",
                    descriptor_missing.c_str(), 1) == 0 &&
                ::setenv(
                    "MOGUET_TEST_PARENT_DEATH_SUPERVISOR_PID",
                    supervisor_pid_file.c_str(), 1) == 0,
            "Failed to configure parent-death Git wrapper");

        fixture.write_file("ignored-parent-death.tmp", "ignored\n");
        fixture.write_file("ordinary-parent-death.tmp", "untracked\n");
        const pid_t worker = ::fork();
        require(worker >= 0, "Failed to fork parent-death worker");
        if(worker == 0) {
            AcceptedReviewedSourceCheckoutResult result =
                materialize_accepted_reviewed_source_checkout(
                    std::move(worker_target), fixture.checkout());
            ::_exit(std::holds_alternative<
                        AcceptedReviewedSourceCheckout>(result)
                        ? 0
                        : 2);
        }

        const int ready_descriptor =
            ::open(ready_fifo.c_str(), O_RDONLY | O_CLOEXEC);
        require(ready_descriptor >= 0,
                "Failed to open parent-death ready FIFO");
        read_pipe_byte(ready_descriptor);
        require(::close(ready_descriptor) == 0,
                "Failed to close parent-death ready FIFO");
        require(!fs::exists(descriptor_missing),
                "PackageBase lease descriptor did not reach mutator tree");
        const std::string supervisor_pid_text =
            read_path(supervisor_pid_file);
        std::size_t parsed_size = 0;
        const long parsed_supervisor_pid = std::stol(
            supervisor_pid_text, &parsed_size);
        require(
            parsed_size == supervisor_pid_text.size() &&
                parsed_supervisor_pid > 0,
            "Git wrapper reported an invalid supervisor PID");
        const pid_t supervisor_pid =
            static_cast<pid_t>(parsed_supervisor_pid);
        const pid_t killed_pid =
            death_target == GuardedMutatorDeathTarget::OuterParent
                ? worker
                : supervisor_pid;
        require(::kill(killed_pid, SIGKILL) == 0,
                "Failed to kill guarded mutator lease owner");
        int worker_status = 0;
        require(::waitpid(worker, &worker_status, 0) == worker,
                "Failed to reap guarded mutator worker");
        if(death_target == GuardedMutatorDeathTarget::OuterParent) {
            require(
                WIFSIGNALED(worker_status) &&
                    WTERMSIG(worker_status) == SIGKILL,
                "Parent-death worker did not terminate by SIGKILL");
        } else {
            require(
                WIFEXITED(worker_status) &&
                    WEXITSTATUS(worker_status) == 2,
                "Supervisor death did not fail the outer worker");
        }

        AcceptedReviewedSourceCheckoutResult blocked =
            materialize_accepted_reviewed_source_checkout(
                make_initial_accepted(identity), fixture.checkout());
        require(
            require_arm<ReviewedSourcePinnedCheckoutFailure>(
                blocked,
                "Guard owner death released lease while Git was alive")
                    .reason ==
                ReviewedSourcePinnedCheckoutFailureReason::
                    LeaseContended,
            "Guard owner death did not retain the PackageBase lease");

        const int release_descriptor =
            ::open(release_fifo.c_str(), O_WRONLY | O_CLOEXEC);
        require(release_descriptor >= 0,
                "Failed to open parent-death release FIFO");
        write_pipe_byte(release_descriptor, 'x');
        require(::close(release_descriptor) == 0,
                "Failed to close parent-death release FIFO");

        for(int attempt = 0; attempt < 200 && !fs::exists(completed);
            ++attempt) {
            ::usleep(5000);
        }
        require(fs::exists(completed),
                "Parent-death Git mutator did not complete");
        static_cast<void>(::unsetenv("MOGUET_TEST_GIT_EXECUTABLE"));
        static_cast<void>(::unsetenv("MOGUET_TEST_PARENT_DEATH_STAGE"));
        static_cast<void>(::unsetenv("MOGUET_TEST_PARENT_DEATH_ONCE"));
        static_cast<void>(::unsetenv("MOGUET_TEST_PARENT_DEATH_READY"));
        static_cast<void>(::unsetenv("MOGUET_TEST_PARENT_DEATH_RELEASE"));
        static_cast<void>(::unsetenv("MOGUET_TEST_PARENT_DEATH_COMPLETED"));
        static_cast<void>(::unsetenv("MOGUET_TEST_PARENT_DEATH_CHECKOUT"));
        static_cast<void>(::unsetenv(
            "MOGUET_TEST_PARENT_DEATH_DESCRIPTOR_MISSING"));
        static_cast<void>(::unsetenv(
            "MOGUET_TEST_PARENT_DEATH_SUPERVISOR_PID"));

        std::optional<AcceptedReviewedSourceCheckout> reacquired;
        for(int attempt = 0; attempt < 200 && !reacquired.has_value();
            ++attempt) {
            AcceptedReviewedSourceCheckoutResult retry =
                materialize_accepted_reviewed_source_checkout(
                    make_initial_accepted(identity),
                    fixture.checkout());
            if(auto* checkout =
                   std::get_if<AcceptedReviewedSourceCheckout>(&retry)) {
                reacquired.emplace(std::move(*checkout));
                break;
            }
            require(
                require_arm<ReviewedSourcePinnedCheckoutFailure>(
                    retry,
                    "Parent-death retry returned an unknown result")
                        .reason ==
                    ReviewedSourcePinnedCheckoutFailureReason::
                        LeaseContended,
                "Parent-death retry failed for a non-contention reason");
            ::usleep(5000);
        }
        require(reacquired.has_value() && reacquired->valid(),
                "Lease did not release after the complete mutator tree exited");
        require(
            !fs::exists(
                fixture.repository() / "ignored-parent-death.tmp") &&
                !fs::exists(
                    fixture.repository() /
                    "ordinary-parent-death.tmp"),
            "Retry reused stale checkout proof instead of rematerializing");
        ReviewedSourceStateStoreReadResult state =
            read_reviewed_source_state(identity.package_base());
        require(
            std::holds_alternative<ReviewedSourceStateMissing>(
                require_arm<ReviewedSourceStateStoreRead>(
                    state,
                    "Parent-death state lookup failed")
                    .observation),
            "Guard owner death during materialization advanced reviewed state");
    };

    for(const GuardedMutatorDeathTarget death_target : {
            GuardedMutatorDeathTarget::OuterParent,
            GuardedMutatorDeathTarget::Supervisor}) {
        exercise(
            "git checkout --detach --force <pinned-commit>",
            death_target);
        exercise("git clean -ffdx", death_target);
    }
}

void test_process_crash_releases_lease_without_publication() {
    PinnedBuildFixture fixture;
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceTarget child_accepted =
        make_initial_accepted(identity);
    AcceptedReviewedSourceTarget parent_contended =
        make_initial_accepted(identity);
    AcceptedReviewedSourceTarget parent_retry =
        make_initial_accepted(identity);

    int ready_pipe[2];
    int release_pipe[2];
    require(::pipe(ready_pipe) == 0 && ::pipe(release_pipe) == 0,
            "Failed to create lease synchronization pipes");
    const pid_t child = ::fork();
    require(child >= 0, "Failed to fork lease crash fixture");
    if(child == 0) {
        static_cast<void>(::close(ready_pipe[0]));
        static_cast<void>(::close(release_pipe[1]));
        AcceptedReviewedSourceCheckoutResult held =
            materialize_accepted_reviewed_source_checkout(
                std::move(child_accepted), fixture.checkout());
        if(!std::holds_alternative<AcceptedReviewedSourceCheckout>(held)) {
            ::_exit(2);
        }
        const char ready = 'r';
        if(::write(ready_pipe[1], &ready, 1) != 1) ::_exit(3);
        char release = 0;
        if(::read(release_pipe[0], &release, 1) != 1) ::_exit(4);
        // _exit deliberately skips C++ destructors. Kernel FD teardown must
        // release the PackageBase flock and no state publication occurred.
        ::_exit(0);
    }

    static_cast<void>(::close(ready_pipe[1]));
    static_cast<void>(::close(release_pipe[0]));
    read_pipe_byte(ready_pipe[0]);
    AcceptedReviewedSourceCheckoutResult blocked =
        materialize_accepted_reviewed_source_checkout(
            std::move(parent_contended), fixture.checkout());
    require(require_arm<ReviewedSourcePinnedCheckoutFailure>(
                blocked, "Parent acquired child-held lease")
                    .reason ==
                ReviewedSourcePinnedCheckoutFailureReason::LeaseContended,
            "Cross-process lease contention was not typed");
    write_pipe_byte(release_pipe[1], 'x');
    int child_status = 0;
    require(::waitpid(child, &child_status, 0) == child &&
                WIFEXITED(child_status) &&
                WEXITSTATUS(child_status) == 0,
            "Lease crash fixture child failed");
    static_cast<void>(::close(ready_pipe[0]));
    static_cast<void>(::close(release_pipe[1]));

    AcceptedReviewedSourceCheckout retry = materialize(
        std::move(parent_retry), fixture.checkout());
    require(retry.valid(), "Process crash did not release PackageBase lease");
    ReviewedSourceStateStoreReadResult state =
        read_reviewed_source_state(identity.package_base());
    require(std::holds_alternative<ReviewedSourceStateMissing>(
                require_arm<ReviewedSourceStateStoreRead>(
                    state, "Crash recovery state lookup failed")
                    .observation),
            "Crash before publication advanced reviewed state");
}

void test_missing_target_and_second_consume_fail_closed() {
    PinnedBuildFixture fixture;
    const std::string missing_oid(fixture.first_oid().size(), 'a');
    const AurReviewedSourceReviewIdentity missing =
        fixture.identity(missing_oid);
    AcceptedReviewedSourceCheckoutResult failed =
        materialize_accepted_reviewed_source_checkout(
            make_initial_accepted(missing), fixture.checkout());
    const auto& checkout_failure =
        require_arm<ReviewedSourcePinnedCheckoutFailure>(
            failed, "Missing target produced exact checkout");
    require(checkout_failure.reason ==
                ReviewedSourcePinnedCheckoutFailureReason::
                    GitMaterializationFailed,
            "Missing exact target returned wrong checkout failure");

    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckout exact = materialize(
        make_initial_accepted(identity), fixture.checkout());
    AcceptedReviewedSourceCheckout moved(std::move(exact));
    ReviewedSourcePublicationResult rejected =
        publish_accepted_reviewed_source_checkout(std::move(exact));
    require(require_arm<ReviewedSourcePublicationFailure>(
                rejected, "Second checkout consume produced publication")
                    .reason ==
                ReviewedSourcePublicationFailureReason::
                    InvalidCheckoutCapability,
            "Moved-from checkout did not fail closed");
    require(moved.valid(), "Moved-to checkout lost authority");
}

} // namespace

int main() {
    try {
        test_exact_oid_checkout_and_definite_publication();
        test_lease_contention_and_same_target_idempotence();
        test_different_target_stale_writer_conflicts();
        test_already_reviewed_reconfirmation_rejects_newer_target();
        test_precommit_failure_and_published_uncertain();
        test_post_publication_checkout_failure_keeps_state();
        test_editor_overlay_is_explicit_build_provenance();
        test_noop_editor_seals_no_overlay();
        test_overlay_mutation_after_seal_is_rejected_before_publication();
        test_empty_directory_after_seal_is_rejected_before_cas();
        test_manifest_change_during_git_projection_is_not_sealed();
        test_mode_mutation_after_cas_keeps_publication_and_stops_pinned_build();
        test_index_and_head_mutation_after_overlay_seal_are_rejected();
        test_checkout_replacement_after_overlay_seal_is_rejected();
        test_embedded_repository_cannot_hide_overlay_files();
        test_special_and_oversized_overlay_entries_fail_closed();
        test_makepkg_boundary_reproof_rejects_post_cas_mutation();
        test_status_zero_post_command_revalidation_keeps_publication();
        test_nested_dot_git_input_is_rejected_at_makepkg_boundary();
        test_future_state_is_not_overwritten();
        test_abnormal_history_uses_exact_cas_rebind();
        test_unsafe_history_is_not_flattened();
        test_sha256_and_object_format_binding();
        test_named_checkout_replacement_cannot_receive_git_mutation();
        test_parent_and_supervisor_death_during_external_mutator_keeps_lease();
        test_process_crash_releases_lease_without_publication();
        test_missing_target_and_second_consume_fail_closed();
        reset_reviewed_source_state_store_test_hooks();
        std::cout << "reviewed source pinned build tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        reset_reviewed_source_state_store_test_hooks();
        std::cerr << "reviewed source pinned build test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
