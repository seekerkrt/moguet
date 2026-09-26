#include "trusted_git.hpp"

#include "process.hpp"
#include "shell_words.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace {

std::string trim(const std::string& value) {
    const std::string::size_type first =
        value.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) return "";
    const std::string::size_type last =
        value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}

std::string remote_ref(const std::string& branch) {
    return "origin/" + branch;
}

} // namespace

struct TrustedGitPinnedCheckout::State {
    const ValidatedCachePath checkout;
    const AurReviewedSourceReviewIdentity identity;

    State(
        ValidatedCachePath value_checkout,
        AurReviewedSourceReviewIdentity value_identity)
        : checkout(std::move(value_checkout)),
          identity(std::move(value_identity)) {
    }
};

TrustedGitPinnedCheckout::TrustedGitPinnedCheckout(
    ValidatedCachePath checkout,
    AurReviewedSourceReviewIdentity identity)
    : state_(std::make_unique<State>(
          std::move(checkout), std::move(identity))) {
}

TrustedGitPinnedCheckout::TrustedGitPinnedCheckout(
    TrustedGitPinnedCheckout&& other) noexcept = default;

TrustedGitPinnedCheckout& TrustedGitPinnedCheckout::operator=(
    TrustedGitPinnedCheckout&& other) noexcept = default;

TrustedGitPinnedCheckout::~TrustedGitPinnedCheckout() = default;

bool TrustedGitPinnedCheckout::valid() const noexcept {
    return state_ != nullptr;
}

const TrustedGitPinnedCheckout::State&
TrustedGitPinnedCheckout::require_state() const {
    if(!state_) {
        throw std::logic_error(
            "A moved-from pinned Git checkout has no authority.");
    }
    return *state_;
}

const AurReviewedSourceReviewIdentity&
TrustedGitPinnedCheckout::identity() const {
    return require_state().identity;
}

const std::filesystem::path&
TrustedGitPinnedCheckout::checkout_path() const {
    return require_state().checkout.canonical_path();
}

std::uintmax_t TrustedGitPinnedCheckout::checkout_device() const {
    return require_state().checkout.device();
}

std::uintmax_t TrustedGitPinnedCheckout::checkout_inode() const {
    return require_state().checkout.inode();
}

std::string trusted_git_remote_origin_url(
    const ValidatedCachePath&) {
    return trim(exec_command("git config --get remote.origin.url"));
}

int trusted_git_fetch_origin(
    const ValidatedCachePath&,
    const std::string&) {
    return run_command("git fetch origin");
}

int trusted_git_fetch_origin(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const ReviewedSourcePackageBaseLease&) {
    return trusted_git_fetch_origin(checkout, expected_remote_url);
}

std::string trusted_git_detect_remote_branch(
    const ValidatedCachePath&,
    const std::string&) {
    const std::string remote_head = trim(exec_command(
        "git symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>/dev/null"));
    constexpr const char* prefix = "origin/";
    if(remote_head.starts_with(prefix) &&
       remote_head.size() > std::string(prefix).size()) {
        return remote_head.substr(std::string(prefix).size());
    }
    if(command_status(
           "git show-ref --verify --quiet refs/remotes/origin/main") ==
       0) {
        return "main";
    }
    if(command_status(
           "git show-ref --verify --quiet refs/remotes/origin/master") ==
       0) {
        return "master";
    }
    return "master";
}

int trusted_git_diff_quiet(
    const ValidatedCachePath&,
    const std::string&,
    const std::string& branch) {
    return run_command(
        "git diff --quiet " +
        shell_words::quote("HEAD.." + remote_ref(branch)));
}

std::string trusted_git_diff_name_only(
    const ValidatedCachePath&,
    const std::string&,
    const std::string& branch) {
    const std::string command =
        "git diff --name-only " +
        shell_words::quote("HEAD.." + remote_ref(branch)) +
        " 2>/dev/null";
    return exec_command(command.c_str());
}

int trusted_git_show_diff(
    const ValidatedCachePath&,
    const std::string&,
    const std::string& branch) {
    return run_command(
        "git diff " +
        shell_words::quote("HEAD.." + remote_ref(branch)) +
        " --color=always");
}

int trusted_git_reset_hard(
    const ValidatedCachePath&,
    const std::string&,
    const std::string& branch) {
    return run_command(
        "git reset --hard " + shell_words::quote(remote_ref(branch)));
}

int trusted_git_reset_hard(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch,
    const ReviewedSourcePackageBaseLease&) {
    return trusted_git_reset_hard(
        checkout, expected_remote_url, branch);
}

int trusted_git_clone_persistent_checkout(
    const ValidatedCachePath& destination,
    const std::string& remote_url) {
    return run_command(
        "git clone " + shell_words::quote(remote_url) + " " +
        shell_words::quote(destination.path().filename().string()));
}

int trusted_git_clone_persistent_checkout(
    const ValidatedCachePath& destination,
    const std::string& remote_url,
    const ReviewedSourcePackageBaseLease&) {
    return trusted_git_clone_persistent_checkout(
        destination, remote_url);
}

TrustedGitCommitResolutionResult trusted_git_resolve_remote_commit(
    const ValidatedCachePath&,
    const std::string&,
    const std::string&) {
    return TrustedGitReviewFailure{
        TrustedGitReviewFailureReason::CommandFailed,
        TrustedGitReviewStage::TargetResolution,
        1};
}

TrustedGitAurReviewedSourceProjectionResult
trusted_git_project_aur_reviewed_source(
    const ValidatedCachePath&,
    AurReviewedSourceReviewIdentity,
    std::optional<SourceRevisionIdentity>) {
    return TrustedGitReviewFailure{
        TrustedGitReviewFailureReason::CommandFailed,
        TrustedGitReviewStage::TargetValidation,
        1};
}

TrustedGitAurReviewedSourceMaterializationResult
trusted_git_materialize_aur_reviewed_source_review(
    const ValidatedCachePath&,
    TrustedAurReviewedSourceProjection) {
    return TrustedGitReviewFailure{
        TrustedGitReviewFailureReason::CommandFailed,
        TrustedGitReviewStage::BlobRead,
        1};
}

TrustedGitPinnedCheckoutResult trusted_git_materialize_pinned_checkout(
    const ValidatedCachePath&,
    AurReviewedSourceReviewIdentity,
    const ReviewedSourcePackageBaseLease&) {
    return TrustedGitPinnedCheckoutFailure{
        TrustedGitPinnedCheckoutFailureReason::CommandFailed,
        TrustedGitPinnedCheckoutStage::CheckoutMaterialization,
        1, 0, 0, std::nullopt};
}

TrustedGitPinnedCheckoutRevalidationResult
revalidate_trusted_git_pinned_checkout(
    const TrustedGitPinnedCheckout&) {
    return TrustedGitPinnedCheckoutFailure{
        TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
        TrustedGitPinnedCheckoutStage::BoundaryRevalidation,
        std::nullopt, 0, 0, std::nullopt};
}

TrustedGitPinnedCheckoutOverlayObservationResult
observe_clean_trusted_git_pinned_checkout_overlay(
    const TrustedGitPinnedCheckout&,
    const ReviewedSourcePackageBaseLease&) {
    return TrustedGitPinnedCheckoutFailure{
        TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
        TrustedGitPinnedCheckoutStage::OverlayObservation,
        std::nullopt, 0, 0, std::nullopt};
}

std::variant<std::string, TrustedGitPinnedCheckoutFailure>
trusted_git_read_review_pkgbuild(const ValidatedCachePath&) {
    // This profile cannot mint reviewed checkout authority. Never synthesize
    // recipe bytes if the review-local capture path is reached unexpectedly.
    return TrustedGitPinnedCheckoutFailure{
        TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
        TrustedGitPinnedCheckoutStage::OverlayObservation,
        std::nullopt, 0, 0, std::nullopt};
}

TrustedGitPinnedCheckoutOverlayObservationResult
observe_trusted_git_pinned_checkout_overlay(
    const TrustedGitPinnedCheckout&,
    const ReviewedSourcePackageBaseLease&) {
    return TrustedGitPinnedCheckoutFailure{
        TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
        TrustedGitPinnedCheckoutStage::OverlayObservation,
        std::nullopt, 0, 0, std::nullopt};
}

TrustedGitPinnedCheckoutRevalidationResult
revalidate_trusted_git_pinned_checkout_overlay(
    const TrustedGitPinnedCheckout&,
    const ReviewedSourcePackageBaseLease&,
    const TrustedGitPinnedCheckoutOverlayObservation&) {
    return TrustedGitPinnedCheckoutFailure{
        TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
        TrustedGitPinnedCheckoutStage::OverlayRevalidation,
        std::nullopt, 0, 0, std::nullopt};
}

int trusted_git_clone_aur_export(
    const std::string& remote_url,
    const std::filesystem::path& anchored_destination) {
    return run_command(
        "git clone --quiet -- " + shell_words::quote(remote_url) + " " +
        shell_words::quote(anchored_destination.string()) +
        " > /dev/null");
}

std::string trusted_git_aur_export_remote_origin_url(
    const std::filesystem::path& anchored_checkout) {
    const std::string command =
        "git -C " + shell_words::quote(anchored_checkout.string()) +
        " config --local --get remote.origin.url 2>/dev/null";
    return trim(exec_command(command.c_str()));
}
