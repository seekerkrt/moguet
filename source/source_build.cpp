#include "source_build.hpp"

#include "app_config.hpp"
#include "aur_devel_update.hpp"
#include "reviewed_devel_source_route.hpp"
#include "srcinfo_source_metadata.hpp"
#include <fstream>
#include "diagnostic_projection.hpp"
#include "interactive_confirmation.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"
#include "persistent_checkout.hpp"
#include "process.hpp"
#include "reviewed_source_acceptance.hpp"
#include "reviewed_source_lifecycle.hpp"
#include "reviewed_source_pinned_build.hpp"
#include "reviewed_source_trusted_review.hpp"
#include "invocation_owned_cleanup_adapter.hpp"
#include "runtime_diagnostic.hpp"
#include "separated_package_base_source_build.hpp"
#include "separated_source_build.hpp"
#include "shell_words.hpp"
#include "trusted_git.hpp"
#include "trusted_cache.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <linux/openat2.h>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

struct PreparedSourceBuildExecutionCapabilities {
    ProductionArtifactSourceTree source_tree;
    ValidatedPrivateCacheRoot artifact_root;
    bool rebuild = false;
    bool clean_build = false;
};

struct SourceBuildPreparationAccess {
    static PreparedSourceBuildNeedsBuild make(PreparedReviewedDevelSourceBuildExecution devel) noexcept {
        return PreparedSourceBuildNeedsBuild(std::move(devel));
    }
    static PreparedSourceBuildNeedsBuild make(
        ProductionArtifactSourceTree source_tree,
        ValidatedPrivateCacheRoot artifact_root,
        bool rebuild,
        bool clean_build) noexcept {
        return PreparedSourceBuildNeedsBuild(
            std::move(source_tree), std::move(artifact_root), rebuild,
            clean_build);
    }
};

struct SourceBuildPreparedExecutionAccess {
    static std::optional<PreparedReviewedDevelSourceBuildExecution> take_devel(PreparedSourceBuildNeedsBuild& prepared) {
        if(!prepared.devel_) return std::nullopt;
        return std::move(prepared.devel_);
    }
    static PreparedSourceBuildExecutionCapabilities consume(
        PreparedSourceBuildNeedsBuild prepared) {
        if(!prepared.source_tree_.has_value() ||
           !prepared.artifact_root_.has_value()) {
            throw std::logic_error(localization::translate_message(
                "Prepared source-build execution capability is invalid or test-only."));
        }
        return PreparedSourceBuildExecutionCapabilities{
            std::move(prepared.source_tree_.value()),
            std::move(prepared.artifact_root_.value()),
            prepared.rebuild_,
            prepared.clean_build_};
    }
};

class ReviewedSourceFatalStatePreflightSlot final {
    std::optional<ReviewedSourceFatalStatePreflight> observation_;

    explicit ReviewedSourceFatalStatePreflightSlot(
        ReviewedSourceFatalStatePreflight observation) noexcept
        : observation_(std::move(observation)) {
    }

    friend std::shared_ptr<ReviewedSourceFatalStatePreflightSlot>
    preflight_reviewed_source_fatal_state_for_production(
        const SourceBuildRequest& request);
    friend SourceBuildPreparationOutcome prepare_source_build_for_execution(
        const SourceBuildRequest& request,
        const std::string& display_name,
        SourceBuildUpdatePolicy update_policy,
        const ValidatedCacheRoot& cache_root,
        const AppConfig& config,
        const ReviewedDevelSourceBuildIntent* execution_intent);
};

ProductionArtifactSourceTree
make_unreviewed_production_artifact_source_tree(
    ValidatedCachePath checkout,
    ReviewedSourcePackageBaseLease lease,
    ProductionSourceReviewStatus review_status,
    ReviewedSourceEditorOverlayStatus editor_overlay,
    std::optional<ReviewedSourceCompatibilityBuildReason>
        compatibility_reason) {
    if(!lease.valid() || lease.device() != checkout.device() ||
       lease.inode() != checkout.inode() ||
       review_status == ProductionSourceReviewStatus::Reviewed ||
       (review_status ==
        ProductionSourceReviewStatus::CompatibilityWithoutReview) !=
           compatibility_reason.has_value()) {
        throw std::logic_error(
            "Unreviewed production source authority is inconsistent.");
    }
    auto authority =
        std::make_shared<ReviewedSourcePackageBaseLease>(
            std::move(lease));
    ProductionSourceBuildProvenance provenance;
    provenance.review_status = review_status;
    provenance.editor_overlay = editor_overlay;
    provenance.compatibility_reason = std::move(compatibility_reason);
    return ProductionArtifactSourceTree(
        std::move(checkout), authority,
        [authority](
            const std::string& command,
            const std::string& display_command) {
            try {
                return authority->run_guarded_command(
                    command, display_command);
            } catch(const ReviewedSourceBuildCheckoutReproofError&
                        error) {
                throw ReviewedSourceProductionError(
                    ReviewedSourceProductionFailure{
                        ReviewedSourceProductionFailureStage::
                            BuildBoundaryRevalidation,
                        ReviewedSourceProductionFailureReason::
                            BuildBoundaryRevalidationFailure,
                        error.failure()});
            }
        },
        std::move(provenance));
}

ProductionArtifactSourceTree
make_reviewed_production_artifact_source_tree(
    ValidatedCachePath checkout,
    PinnedReviewedSourceBuild reviewed,
    ProductionReviewedSourceOutcome reviewed_outcome,
    std::optional<ReviewedSourceAbnormalStateReason>
        abnormal_state_reason) {
    if(!reviewed.valid() || reviewed.checkout_device() != checkout.device() ||
       reviewed.checkout_inode() != checkout.inode() ||
       (reviewed_outcome ==
        ProductionReviewedSourceOutcome::
            AbnormalStateRebindFullReview) !=
           abnormal_state_reason.has_value()) {
        throw std::logic_error(
            "Reviewed production source authority is inconsistent.");
    }
    ProductionSourceBuildProvenance provenance;
    provenance.review_status = ProductionSourceReviewStatus::Reviewed;
    provenance.editor_overlay = reviewed.editor_overlay_status();
    provenance.reviewed_upstream_base_revision =
        reviewed.reviewed_upstream_base_revision();
    provenance.publication_status = reviewed.publication_status();
    provenance.reviewed_outcome = reviewed_outcome;
    provenance.abnormal_state_reason = abnormal_state_reason;
    provenance.reviewed_state_generation =
        reviewed.published_record().generation;
    auto authority =
        std::make_shared<PinnedReviewedSourceBuild>(
            std::move(reviewed));
    return ProductionArtifactSourceTree(
        std::move(checkout), authority,
        [authority](
            const std::string& command,
            const std::string& display_command) {
            return authority->run_guarded_command(
                command, display_command);
        },
        std::move(provenance));
}

namespace {

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS
ReviewedSourceBeforePublicationHookForTest
    g_reviewed_source_before_publication_hook;

void notify_reviewed_source_before_publication_for_test() {
    ReviewedSourceBeforePublicationHookForTest hook =
        std::exchange(
            g_reviewed_source_before_publication_hook, nullptr);
    if(hook != nullptr) hook();
}
#else
void notify_reviewed_source_before_publication_for_test() {
}
#endif

struct MakepkgBuildOptions {
    bool rebuild = false;
    bool clean_build = false;
};

enum class SourceBuildPreparationFailureStage {
    ArtifactRoot,
    CheckoutOrReview,
};

class SourceBuildPreparationError final : public std::runtime_error {
    SourceBuildPreparationFailureStage stage_;
    bool unknown_exception_ = false;

public:
    SourceBuildPreparationError(
        SourceBuildPreparationFailureStage stage,
        const std::string& diagnostic,
        bool unknown_exception = false)
        : std::runtime_error(diagnostic), stage_(stage),
          unknown_exception_(unknown_exception) {
    }

    SourceBuildPreparationFailureStage stage() const noexcept {
        return stage_;
    }

    bool unknown_exception() const noexcept {
        return unknown_exception_;
    }
};

class ScopedPrivateUmask final {
    mode_t previous_;

public:
    ScopedPrivateUmask() noexcept : previous_(umask(0077)) {
    }

    ScopedPrivateUmask(const ScopedPrivateUmask&) = delete;
    ScopedPrivateUmask& operator=(const ScopedPrivateUmask&) = delete;

    ~ScopedPrivateUmask() noexcept {
        static_cast<void>(umask(previous_));
    }
};

enum class UpdateCheckResult {
    NeedsBuild,
    UpToDate,
    Unknown,
};

std::string up_to_date_diagnostic(
    const std::string& package_name,
    const std::string& installed_version) {
    // TRANSLATORS: The placeholders are a package name and version.
    return localization::format_translated_message(
        "{} is up to date ({}). Skipping.",
        package_name,
        installed_version);
}

std::string unknown_update_skip_diagnostic(
    const std::string& package_name,
    SourceBuildUpdateStatusUnknownSkipReason reason) {
    switch(reason) {
        case SourceBuildUpdateStatusUnknownSkipReason::NoConfirm:
            // TRANSLATORS: The placeholders are a package name and the literal --noconfirm option.
            return localization::format_translated_message(
                "Skipping {}: update status is unknown and {} is set.",
                package_name,
                "--noconfirm");
        case SourceBuildUpdateStatusUnknownSkipReason::NonInteractiveStdin:
            // TRANSLATORS: The placeholders are a package name and the literal stdin identity.
            return localization::format_translated_message(
                "Skipping {}: update status is unknown and {} is non-interactive.",
                package_name, "stdin");
        case SourceBuildUpdateStatusUnknownSkipReason::UserDeclined:
            // TRANSLATORS: The placeholder is a package name.
            return localization::format_translated_message(
                "Skipping {}: update status is unknown and the user declined to continue.",
                package_name);
    }
    throw std::logic_error(localization::translate_message(
        "Unknown source-build update-status skip reason."));
}

SourceBuildExecutionResult source_build_result_from_artifact_outcome(
    ArtifactInstallExecutionOutcome outcome) {
    switch(outcome) {
        case ArtifactInstallExecutionOutcome::Installed:
            return SourceBuildExecutionResult{
                SourceBuildExecutionStatus::Installed, std::nullopt, {}, std::nullopt};
        case ArtifactInstallExecutionOutcome::SkippedAsNeeded:
            return SourceBuildExecutionResult{
                SourceBuildExecutionStatus::SkippedAsNeeded,
                std::nullopt,
                {},
                std::nullopt};
    }
    throw std::logic_error(localization::translate_message(
        "Unknown artifact install execution outcome."));
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

bool is_safe_command_token(const std::string& token) {
    if(token.empty()) return false;
    return std::all_of(token.begin(), token.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '/' || ch == '.' || ch == '_' || ch == '+' || ch == '-' || ch == '=' ||
               ch == ':' || ch == '@' || ch == '%';
    });
}

std::vector<std::string> split_command_words(const std::string& command) {
    std::stringstream ss(command);
    std::string word;
    std::vector<std::string> words;
    while(ss >> word) {
        if(!is_safe_command_token(word)) {
            // TRANSLATORS: The placeholder is an editor command token.
            throw std::runtime_error(localization::format_translated_message(
                "Unsafe command token: {}", word));
        }
        words.push_back(word);
    }
    if(words.empty()) {
        throw std::runtime_error(localization::translate_message(
            "Editor command is empty."));
    }
    return words;
}

std::string join_comma_display_values(const std::vector<std::string>& values) {
    std::stringstream ss;
    for(size_t i = 0; i < values.size(); ++i) {
        if(i > 0) ss << ", ";
        ss << values[i];
    }
    return ss.str();
}

void report_confirmation_stop(
    const ConfirmationResult& result,
    DiagnosticPhase phase,
    DiagnosticIdentity identity = {}) {
    report_runtime_diagnostic(
        project_confirmation_diagnostic(
            result, DiagnosticOperation::Build, phase,
            std::move(identity)),
        confirmation_stop_diagnostic(result));
}

[[noreturn]] void stop_after_confirmation(
    ConfirmationResult result,
    DiagnosticPhase phase,
    DiagnosticIdentity identity = {}) {
    report_confirmation_stop(result, phase, std::move(identity));
    throw ConfirmationOperationStopped(std::move(result));
}

std::string build_editor_command(const std::string& editor, const fs::path& target) {
    std::vector<std::string> args = split_command_words(editor);
    fs::path editor_target = target;
    if(editor_target.is_relative()) {
        // POLICY(#219): package-controlled basenameをeditor optionにせず、checkout内の明示pathとして渡す。
        editor_target = fs::path(".") / editor_target;
    }
    args.push_back(editor_target.string());
    return shell_words::join(args);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while(std::getline(stream, line)) {
        line = trim(line);
        if(!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> git_changed_files(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch) {
    return split_lines(trusted_git_diff_name_only(
        checkout, expected_remote_url, branch));
}

bool is_review_sensitive_file(const std::string& path) {
    fs::path file_path(path);
    return file_path.filename() == "PKGBUILD" || file_path.extension() == ".install";
}

void log_update_diff_guidance(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch) {
    const std::string range = "HEAD..origin/" + branch;
    std::vector<std::string> changed_files = git_changed_files(
        checkout, expected_remote_url, branch);
    if(changed_files.empty()) return;

    // TRANSLATORS: The placeholder is a literal Git revision range.
    Logger::info(localization::format_translated_message(
        "Update diff range: {} (existing cache repository).",
        range));

    std::vector<std::string> review_sensitive_files;
    for(const auto& file : changed_files) {
        if(is_review_sensitive_file(file)) review_sensitive_files.push_back(file);
    }
    if(!review_sensitive_files.empty()) {
        // TRANSLATORS: The placeholder is a comma-separated list of file paths.
        Logger::warn(localization::format_translated_message(
            "Review-sensitive file changes: {}",
            join_comma_display_values(review_sensitive_files)));
    }
}

void log_review_targets(const fs::path& pkg_dir, const std::vector<fs::path>& install_scripts) {
    // TRANSLATORS: The placeholder is the literal PKGBUILD artifact name.
    Logger::info(localization::format_translated_message(
        "Review target: {}", "PKGBUILD"));
    if(install_scripts.empty()) return;

    std::vector<std::string> names;
    for(const auto& script : install_scripts) {
        names.push_back(script.string());
    }
    // POLICY: PKGBUILD はここで評価しない。作業ツリーにある *.install だけを、見落とし防止として案内する。
    // TRANSLATORS: The placeholder is a comma-separated list of install script paths.
    Logger::warn(localization::format_translated_message(
        "Install script(s) present; review before build: {}",
        join_comma_display_values(names)));
    // TRANSLATORS: The placeholder is a package checkout directory path.
    Logger::info(localization::format_translated_message(
        "Review directory: {}", pkg_dir.string()));
}

bool review_build_files(
    const ValidatedCachePath& checkout,
    const AppConfig& config,
    const std::function<int(const std::string&)>& run_checkout_command) {
    const fs::path& pkg_dir = checkout.canonical_path();
    std::vector<fs::path> install_scripts =
        require_safe_persistent_checkout_descendants(checkout);

    if(config.user_config.review.pkgbuild == ReviewPolicy::Skip) {
        // TRANSLATORS: The placeholders are literal artifact names and the --noedit option.
        Logger::info(localization::format_translated_message(
            "Skipping {}/{} review ({}).",
            "PKGBUILD", ".install", "--noedit"));
        return false;
    }

    log_review_targets(pkg_dir, install_scripts);

    bool editor_invoked = false;

    // TRANSLATORS: The placeholder is the literal PKGBUILD artifact name.
    const std::string edit_pkgbuild_question =
        localization::format_translated_message(
            "Edit {}?", "PKGBUILD");
    ConfirmationResult edit_pkgbuild_confirmation = request_confirmation(
        edit_pkgbuild_question, ConfirmationDefault::No,
        config.no_confirm);
    if(std::holds_alternative<ConfirmationAccepted>(
           edit_pkgbuild_confirmation)) {
        require_safe_persistent_checkout_review_targets(checkout, install_scripts);
        if(run_checkout_command(
               build_editor_command(config.editor, "PKGBUILD")) != 0) {
            throw std::runtime_error(localization::translate_message(
                "Editor failed."));
        }
        require_safe_persistent_checkout_review_targets(checkout, install_scripts);
        editor_invoked = true;
    } else if(!std::holds_alternative<ConfirmationDeclined>(
                  edit_pkgbuild_confirmation)) {
        DiagnosticIdentity identity;
        identity.canonical_source_identity = pkg_dir.string();
        stop_after_confirmation(
            std::move(edit_pkgbuild_confirmation),
            DiagnosticPhase::Preflight, std::move(identity));
    }

    for(const auto& install_script : install_scripts) {
        // TRANSLATORS: The placeholder is an install script path.
        const std::string edit_question = localization::format_translated_message(
            "Edit install script {}?", install_script.string());
        ConfirmationResult edit_install_confirmation = request_confirmation(
            edit_question, ConfirmationDefault::No,
            config.no_confirm);
        if(std::holds_alternative<ConfirmationAccepted>(
               edit_install_confirmation)) {
            require_safe_persistent_checkout_review_targets(checkout, install_scripts);
            if(run_checkout_command(
                   build_editor_command(
                       config.editor, install_script)) != 0) {
                throw std::runtime_error(localization::translate_message(
                    "Editor failed."));
            }
            require_safe_persistent_checkout_review_targets(checkout, install_scripts);
            editor_invoked = true;
        } else if(!std::holds_alternative<ConfirmationDeclined>(
                      edit_install_confirmation)) {
            DiagnosticIdentity identity;
            identity.canonical_source_identity = pkg_dir.string();
            stop_after_confirmation(
                std::move(edit_install_confirmation),
                DiagnosticPhase::Preflight, std::move(identity));
        }
    }

    // LANDMINE(#197): editor はreview対象を置換できるため、review開始時の検証結果を持ち越さない。
    require_safe_persistent_checkout_review_targets(checkout, install_scripts);
    return editor_invoked;
}

void confirm_after_build_file_edit(
    const fs::path& checkout_path,
    const AppConfig& config,
    bool editor_invoked) {
    if(editor_invoked) {
        ConfirmationResult proceed_confirmation = request_confirmation(
            localization::translate_message("Proceed with build?"),
            ConfirmationDefault::Yes, config.no_confirm);
        if(!std::holds_alternative<ConfirmationAccepted>(
               proceed_confirmation)) {
            DiagnosticIdentity identity;
            identity.canonical_source_identity = checkout_path.string();
            stop_after_confirmation(
                std::move(proceed_confirmation),
                DiagnosticPhase::Preflight, std::move(identity));
        }
    }
}

ReviewedSourcePackageBaseLease acquire_package_base_lease(
    const ValidatedCachePath& checkout,
    bool reviewed_source) {
    try {
        return acquire_reviewed_source_package_base_lease(
            retain_trusted_cache_directory(checkout));
    } catch(const std::system_error& error) {
        if(!reviewed_source) throw;
        const int lock_error = error.code().value();
        ReviewedSourcePinnedCheckoutFailure failure{
            lock_error == EWOULDBLOCK || lock_error == EAGAIN
                ? ReviewedSourcePinnedCheckoutFailureReason::
                      LeaseContended
                : ReviewedSourcePinnedCheckoutFailureReason::
                      LeaseUnavailable,
            error.code(), std::nullopt, std::nullopt};
        throw ReviewedSourceProductionError(
            ReviewedSourceProductionFailure{
                ReviewedSourceProductionFailureStage::
                    LeaseAcquisition,
                lock_error == EWOULDBLOCK || lock_error == EAGAIN
                    ? ReviewedSourceProductionFailureReason::
                          LeaseContended
                    : ReviewedSourceProductionFailureReason::
                          LeaseUnavailable,
                std::move(failure)});
    }
}

ReviewedSourceProductionFailureReason fatal_production_reason(
    ReviewedSourceFatalStateReason reason) noexcept {
    switch(reason) {
        case ReviewedSourceFatalStateReason::UnsupportedFuture:
            return ReviewedSourceProductionFailureReason::UnsupportedFuture;
        case ReviewedSourceFatalStateReason::UnsafeHistory:
            return ReviewedSourceProductionFailureReason::UnsafeHistory;
        case ReviewedSourceFatalStateReason::StoreFailure:
            return ReviewedSourceProductionFailureReason::StateStoreFailure;
        case ReviewedSourceFatalStateReason::InconsistentStoreObservation:
            return ReviewedSourceProductionFailureReason::
                InconsistentStateObservation;
    }
    return ReviewedSourceProductionFailureReason::
        InconsistentStateObservation;
}

struct ProductionReviewedSourceOutcomeSnapshot {
    ProductionReviewedSourceOutcome outcome =
        ProductionReviewedSourceOutcome::InitialFullReview;
    std::optional<ReviewedSourceAbnormalStateReason> abnormal_state_reason;
};

ProductionReviewedSourceOutcomeSnapshot production_reviewed_source_outcome(
    const ReviewedSourceIntegrationLifecycle& lifecycle) {
    if(std::holds_alternative<ReviewedSourceLifecycleInitialFullReview>(
           lifecycle)) {
        return {ProductionReviewedSourceOutcome::InitialFullReview,
                std::nullopt};
    }
    if(std::holds_alternative<ReviewedSourceLifecycleUpdateReview>(
           lifecycle)) {
        return {ProductionReviewedSourceOutcome::UpdateReview,
                std::nullopt};
    }
    if(std::holds_alternative<
           ReviewedSourceLifecycleRebaselineFullReview>(lifecycle)) {
        return {ProductionReviewedSourceOutcome::RebaselineFullReview,
                std::nullopt};
    }
    if(const auto* abnormal = std::get_if<
           ReviewedSourceLifecycleAbnormalStateRebindFullReview>(
           &lifecycle)) {
        return {
            ProductionReviewedSourceOutcome::
                AbnormalStateRebindFullReview,
            abnormal->reason};
    }
    if(std::holds_alternative<ReviewedSourceLifecycleAlreadyReviewed>(
           lifecycle)) {
        return {ProductionReviewedSourceOutcome::AlreadyReviewed,
                std::nullopt};
    }
    throw std::logic_error(
        "Reviewed production success contains a fatal lifecycle.");
}

template <typename Detail>
[[noreturn]] void stop_reviewed_source_route(
    ReviewedSourceProductionFailureStage stage,
    ReviewedSourceProductionFailureReason reason,
    Detail detail) {
    throw ReviewedSourceProductionError(
        ReviewedSourceProductionFailure{
            stage, reason, std::move(detail)});
}

[[noreturn]] void stop_reviewed_source_operation(
    const ReviewedSourceOperationStop& stop,
    ReviewedSourceProductionFailureStage stage) {
    switch(stop.reason()) {
        case ReviewedSourceOperationStopReason::ExplicitCancellation:
            throw ConfirmationOperationStopped(
                ConfirmationCancelled{
                    ConfirmationCancellationReason::ExplicitToken});
        case ReviewedSourceOperationStopReason::EndOfInput:
            throw ConfirmationOperationStopped(
                ConfirmationCancelled{
                    ConfirmationCancellationReason::EndOfInput});
        case ReviewedSourceOperationStopReason::InputFailure:
            throw ConfirmationOperationStopped(ConfirmationInputFailure{});
        default:
            if(stop.fatal_state_failure().has_value()) {
                stop_reviewed_source_route(
                    stage,
                    fatal_production_reason(
                        stop.fatal_state_failure()->reason),
                    *stop.fatal_state_failure());
            }
            stop_reviewed_source_route(
                stage,
                ReviewedSourceProductionFailureReason::
                    ReviewOperationStopped,
                stop);
    }
}

struct AurCompatibilityCheckout {
    ReviewedSourcePackageBaseLease lease;
    ReviewedSourceCompatibilityBuildReason reason;
};

using AurCheckoutAuthority = std::variant<
    AurCompatibilityCheckout,
    AcceptedReviewedSourceCheckout,
    AlreadyReviewedSourceCheckout>;

ReviewedSourceCompatibilityBuildReason review_bypass_reason(
    const AppConfig& config) {
    if(config.user_config.review.diff == ReviewPolicy::Skip) {
        return ReviewedSourceCompatibilityBuildReason::NoDiff;
    }
    if(config.no_confirm) {
        return ReviewedSourceCompatibilityBuildReason::NoConfirm;
    }
    return ReviewedSourceCompatibilityBuildReason::NonInteractiveInput;
}

bool should_run_reviewed_source_route(const AppConfig& config) noexcept {
    return config.user_config.review.diff == ReviewPolicy::Prompt &&
           !config.no_confirm && isatty(STDIN_FILENO) == 1;
}

std::string reviewed_source_acceptance_question(
    const ReviewedSourceIntegrationLifecycle& lifecycle) {
    if(std::holds_alternative<
           ReviewedSourceLifecycleRebaselineFullReview>(lifecycle)) {
        return localization::translate_message(
            "Accept this explicit reviewed-source full rebaseline?");
    }
    if(const auto* abnormal = std::get_if<
           ReviewedSourceLifecycleAbnormalStateRebindFullReview>(
           &lifecycle)) {
        switch(abnormal->reason) {
            case ReviewedSourceAbnormalStateReason::Invalid:
                return localization::translate_message(
                    "Accept this explicit rebind/rebaseline of invalid reviewed state?");
            case ReviewedSourceAbnormalStateReason::Corrupted:
                return localization::translate_message(
                    "Accept this explicit rebind/rebaseline of corrupted reviewed state?");
            case ReviewedSourceAbnormalStateReason::SourceMismatch:
                return localization::translate_message(
                    "Accept this explicit reviewed-source identity rebind/rebaseline?");
        }
        throw std::logic_error(
            "Reviewed source abnormal state reason is unknown.");
    }
    return localization::translate_message(
        "Accept this reviewed upstream revision?");
}

AurCheckoutAuthority prepare_aur_checkout_authority(
    const SourceBuildRequest& request,
    const ValidatedCachePath& checkout,
    const std::string& branch,
    ReviewedSourcePackageBaseLease lease,
    ReviewedSourceFatalStatePreflight fatal_preflight,
    const AppConfig& config) {
    if(!request.aur_review_identity.has_value()) {
        throw std::logic_error(
            "AUR source-build request has no reviewed PackageBase identity.");
    }
    if(!should_run_reviewed_source_route(config)) {
        return AurCompatibilityCheckout{
            std::move(lease), review_bypass_reason(config)};
    }

    TrustedGitCommitResolutionResult target_result =
        trusted_git_resolve_remote_commit(
            checkout, request.git_url, branch);
    if(auto* failure = std::get_if<TrustedGitReviewFailure>(
           &target_result)) {
        stop_reviewed_source_route(
            ReviewedSourceProductionFailureStage::TargetResolution,
            ReviewedSourceProductionFailureReason::
                TargetResolutionFailure,
            std::move(*failure));
    }
    AurReviewedSourceReviewIdentity identity =
        AurReviewedSourceReviewIdentity::make(
            request.aur_review_identity.value(),
            std::get<SourceRevisionIdentity>(
                std::move(target_result)));

    ReviewedSourceLifecyclePlanResult lifecycle =
        plan_reviewed_source_lifecycle_from_preflight(
            identity, std::move(fatal_preflight));
    if(auto* stop = std::get_if<ReviewedSourceOperationStop>(&lifecycle)) {
        stop_reviewed_source_operation(
            *stop,
            ReviewedSourceProductionFailureStage::LifecyclePlanning);
    }
    if(auto* already = std::get_if<
           ReviewedSourceAlreadyReviewedContinue>(&lifecycle)) {
        AlreadyReviewedSourceCheckoutResult materialized =
            materialize_already_reviewed_source_checkout_with_lease(
                std::move(*already), std::move(lease));
        if(auto* failure = std::get_if<
               ReviewedSourcePinnedCheckoutFailure>(&materialized)) {
            ReviewedSourceProductionFailureReason reason =
                ReviewedSourceProductionFailureReason::
                    ExactCheckoutFailure;
            if(failure->reason ==
               ReviewedSourcePinnedCheckoutFailureReason::LeaseContended) {
                reason = ReviewedSourceProductionFailureReason::
                    LeaseContended;
            } else if(
                failure->reason ==
                ReviewedSourcePinnedCheckoutFailureReason::
                    LeaseUnavailable) {
                reason = ReviewedSourceProductionFailureReason::
                    LeaseUnavailable;
            }
            stop_reviewed_source_route(
                ReviewedSourceProductionFailureStage::
                    ExactCheckoutMaterialization,
                reason, std::move(*failure));
        }
        return std::get<AlreadyReviewedSourceCheckout>(
            std::move(materialized));
    }

    ReviewedSourceReviewRequirement requirement = std::move(
        std::get<ReviewedSourceReviewRequirement>(lifecycle));
    std::optional<SourceRevisionIdentity> baseline;
    if(requirement.baseline() != nullptr) {
        baseline = *requirement.baseline();
    }
    TrustedGitAurReviewedSourceProjectionResult projection =
        trusted_git_project_aur_reviewed_source(
            checkout, requirement.identity(), std::move(baseline));
    if(auto* failure = std::get_if<TrustedGitReviewFailure>(&projection)) {
        stop_reviewed_source_route(
            ReviewedSourceProductionFailureStage::ReviewProjection,
            ReviewedSourceProductionFailureReason::
                ReviewProjectionFailure,
            std::move(*failure));
    }
    TrustedGitAurReviewedSourceMaterializationResult materialized_review =
        trusted_git_materialize_aur_reviewed_source_review(
            checkout,
            std::get<TrustedAurReviewedSourceProjection>(
                std::move(projection)));
    if(auto* failure = std::get_if<TrustedGitReviewFailure>(
           &materialized_review)) {
        stop_reviewed_source_route(
            ReviewedSourceProductionFailureStage::
                ReviewMaterialization,
            ReviewedSourceProductionFailureReason::
                ReviewMaterializationFailure,
            std::move(*failure));
    }
    if(auto* failure = std::get_if<ReviewedSourceReviewFailure>(
           &materialized_review)) {
        stop_reviewed_source_route(
            ReviewedSourceProductionFailureStage::
                ReviewMaterialization,
            ReviewedSourceProductionFailureReason::
                ReviewMaterializationFailure,
            std::move(*failure));
    }
    ReviewedSourceVerifiedLifecycleResult verified =
        bind_reviewed_source_verified_review(
            std::move(requirement),
            std::get<TrustedAurReviewedSourceReview>(
                std::move(materialized_review)));
    if(auto* stop = std::get_if<ReviewedSourceOperationStop>(&verified)) {
        stop_reviewed_source_operation(
            *stop,
            ReviewedSourceProductionFailureStage::ReviewBinding);
    }
    PresentedReviewedSourceTargetResult presented =
        present_reviewed_source_target(
            std::get<ReviewedSourceVerifiedLifecycleTarget>(
                std::move(verified)),
            std::cout);
    if(auto* stop = std::get_if<ReviewedSourceOperationStop>(&presented)) {
        stop_reviewed_source_operation(
            *stop,
            ReviewedSourceProductionFailureStage::Presentation);
    }

    const PresentedReviewedSourceTarget& presented_target =
        std::get<PresentedReviewedSourceTarget>(presented);
    const std::string acceptance_question =
        reviewed_source_acceptance_question(
            presented_target.lifecycle());
    ExplicitConfirmationResult confirmation = request_explicit_confirmation(
        acceptance_question, false);
    ReviewedSourceAcceptanceDisposition disposition =
        decide_reviewed_source_acceptance(
            std::get<PresentedReviewedSourceTarget>(
                std::move(presented)),
            std::move(confirmation));
    if(auto* accepted =
           std::get_if<AcceptedReviewedSourceTarget>(&disposition)) {
        AcceptedReviewedSourceCheckoutResult exact =
            materialize_accepted_reviewed_source_checkout_with_lease(
                std::move(*accepted), std::move(lease));
        if(!std::holds_alternative<AcceptedReviewedSourceCheckout>(exact)) {
            auto failure = std::get<ReviewedSourcePinnedCheckoutFailure>(
                std::move(exact));
            ReviewedSourceProductionFailureReason reason =
                ReviewedSourceProductionFailureReason::
                    ExactCheckoutFailure;
            if(failure.reason ==
               ReviewedSourcePinnedCheckoutFailureReason::LeaseContended) {
                reason = ReviewedSourceProductionFailureReason::
                    LeaseContended;
            } else if(
                failure.reason ==
                ReviewedSourcePinnedCheckoutFailureReason::
                    LeaseUnavailable) {
                reason = ReviewedSourceProductionFailureReason::
                    LeaseUnavailable;
            }
            stop_reviewed_source_route(
                ReviewedSourceProductionFailureStage::
                    ExactCheckoutMaterialization,
                reason, std::move(failure));
        }
        return std::get<AcceptedReviewedSourceCheckout>(std::move(exact));
    }
    if(auto* compatibility = std::get_if<
           ReviewedSourceCompatibilityBuildWithoutReview>(
           &disposition)) {
        return AurCompatibilityCheckout{
            std::move(lease), compatibility->reason()};
    }
    stop_reviewed_source_operation(
        std::get<ReviewedSourceOperationStop>(disposition),
        ReviewedSourceProductionFailureStage::Acceptance);
}

ReviewedSourceEditorBoundaryResult begin_aur_editor_boundary(
    const AurCheckoutAuthority& authority) {
    return std::visit(
        [](const auto& checkout) -> ReviewedSourceEditorBoundaryResult {
            using Checkout = std::decay_t<decltype(checkout)>;
            if constexpr(std::is_same_v<
                             Checkout,
                             AurCompatibilityCheckout>) {
                throw std::logic_error(
                    "Compatibility checkout has no reviewed editor boundary.");
            } else {
                return begin_reviewed_source_editor_boundary(checkout);
            }
        },
        authority);
}

ReviewedSourceEditorOverlayProofResult seal_aur_editor_boundary(
    const AurCheckoutAuthority& authority,
    ReviewedSourceEditorBoundary boundary,
    bool editor_invoked) {
    return std::visit(
        [&boundary, editor_invoked](const auto& checkout) mutable
            -> ReviewedSourceEditorOverlayProofResult {
            using Checkout = std::decay_t<decltype(checkout)>;
            if constexpr(std::is_same_v<
                             Checkout,
                             AurCompatibilityCheckout>) {
                throw std::logic_error(
                    "Compatibility checkout cannot seal reviewed editor overlay authority.");
            } else if(editor_invoked) {
                return seal_reviewed_source_editor_overlay(
                    checkout, std::move(boundary));
            } else {
                return seal_reviewed_source_no_editor_overlay(
                    checkout, std::move(boundary));
            }
        },
        authority);
}

ReviewedProductionSourceExecution finalize_aur_checkout_authority(
    AurCheckoutAuthority authority,
    const ValidatedCachePath& checkout,
    std::optional<ReviewedSourceEditorOverlayProof> editor_overlay,
    bool editor_invoked,
    const ReviewedDevelSourceBuildIntent* intent = nullptr) {
    if(auto* compatibility =
           std::get_if<AurCompatibilityCheckout>(&authority)) {
        if(intent && intent->request.authoritative_devel_update)
            return ReviewedDevelSourceBuildRejected{ReviewedDevelSourceBuildIssue::InvalidPin};
        if(editor_overlay.has_value()) {
            throw std::logic_error(
                "Compatibility checkout received reviewed overlay authority.");
        }
        return make_unreviewed_production_artifact_source_tree(
            checkout, std::move(compatibility->lease),
            ProductionSourceReviewStatus::CompatibilityWithoutReview,
            editor_invoked
                ? ReviewedSourceEditorOverlayStatus::InvocationLocal
                : ReviewedSourceEditorOverlayStatus::None,
            compatibility->reason);
    }

    if(!editor_overlay.has_value()) {
        ReviewedSourceEditorBoundaryResult boundary =
            begin_aur_editor_boundary(authority);
        if(auto* failure = std::get_if<ReviewedSourcePublicationFailure>(
               &boundary)) {
            stop_reviewed_source_route(
                ReviewedSourceProductionFailureStage::
                    EditorOverlayObservation,
                ReviewedSourceProductionFailureReason::
                    OverlayObservationFailure,
                std::move(*failure));
        }
        ReviewedSourceEditorOverlayProofResult sealed =
            seal_aur_editor_boundary(
                authority,
                std::get<ReviewedSourceEditorBoundary>(
                    std::move(boundary)),
                false);
        if(auto* failure = std::get_if<ReviewedSourcePublicationFailure>(
               &sealed)) {
            stop_reviewed_source_route(
                ReviewedSourceProductionFailureStage::
                    EditorOverlayObservation,
                ReviewedSourceProductionFailureReason::
                    OverlayObservationFailure,
                std::move(*failure));
        }
        editor_overlay.emplace(
            std::get<ReviewedSourceEditorOverlayProof>(
                std::move(sealed)));
    }

    const ProductionReviewedSourceOutcomeSnapshot reviewed_outcome =
        std::visit(
            [](const auto& checkout)
                -> ProductionReviewedSourceOutcomeSnapshot {
                using Checkout = std::decay_t<decltype(checkout)>;
                if constexpr(std::is_same_v<
                                 Checkout,
                                 AurCompatibilityCheckout>) {
                    throw std::logic_error(
                        "Compatibility checkout has no reviewed outcome.");
                } else {
                    return production_reviewed_source_outcome(
                        checkout.lifecycle());
                }
            },
            authority);

    notify_reviewed_source_before_publication_for_test();

    ReviewedSourcePublicationResult publication =
        std::holds_alternative<AcceptedReviewedSourceCheckout>(authority)
            ? publish_accepted_reviewed_source_checkout_with_editor_overlay(
                  std::get<AcceptedReviewedSourceCheckout>(
                      std::move(authority)),
                  std::move(editor_overlay.value()))
            : confirm_already_reviewed_source_checkout_with_editor_overlay(
                  std::get<AlreadyReviewedSourceCheckout>(
                      std::move(authority)),
                  std::move(editor_overlay.value()));
    if(auto* pinned =
           std::get_if<PinnedReviewedSourceBuild>(&publication)) {
        return select_normal_reviewed_source_execution(checkout, std::move(*pinned), reviewed_outcome.outcome, reviewed_outcome.abnormal_state_reason, intent);
    }
    if(std::holds_alternative<ReviewedSourcePublicationUncertain>(
           publication)) {
        stop_reviewed_source_route(
            ReviewedSourceProductionFailureStage::StatePublication,
            ReviewedSourceProductionFailureReason::PublishedUncertain,
            std::get<ReviewedSourcePublicationUncertain>(
                std::move(publication)));
    }
    if(std::holds_alternative<
           ReviewedSourcePostPublicationCheckoutFailure>(publication)) {
        stop_reviewed_source_route(
            ReviewedSourceProductionFailureStage::
                PostPublicationCheckoutRevalidation,
            ReviewedSourceProductionFailureReason::
                PostPublicationCheckoutFailure,
            std::get<ReviewedSourcePostPublicationCheckoutFailure>(
                std::move(publication)));
    }
    if(std::holds_alternative<ReviewedSourcePublicationConflict>(
           publication)) {
        stop_reviewed_source_route(
            ReviewedSourceProductionFailureStage::StatePublication,
            ReviewedSourceProductionFailureReason::
                PublicationConflict,
            std::get<ReviewedSourcePublicationConflict>(
                std::move(publication)));
    }
    if(std::holds_alternative<ReviewedSourcePublicationUnsafeHistory>(
           publication)) {
        stop_reviewed_source_route(
            ReviewedSourceProductionFailureStage::StatePublication,
            ReviewedSourceProductionFailureReason::
                PublicationUnsafeHistory,
            std::get<ReviewedSourcePublicationUnsafeHistory>(
                std::move(publication)));
    }
    ReviewedSourcePublicationFailure failure =
        std::get<ReviewedSourcePublicationFailure>(
            std::move(publication));
    stop_reviewed_source_route(
        ReviewedSourceProductionFailureStage::StatePublication,
        failure.reason ==
                ReviewedSourcePublicationFailureReason::
                    CheckoutRevalidationFailed
            ? ReviewedSourceProductionFailureReason::
                  CheckoutRevalidationFailure
            : ReviewedSourceProductionFailureReason::
                  PublicationFailure,
        std::move(failure));
}

std::optional<std::string> read_nofollow_regular_file(
    const fs::path& path) {
    struct open_how how{};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    const int descriptor = static_cast<int>(syscall(
        SYS_openat2, AT_FDCWD, path.c_str(), &how, sizeof(how)));
    if(descriptor < 0) return std::nullopt;

    struct stat status{};
    if(fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
       status.st_uid != geteuid() ||
       (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        static_cast<void>(close(descriptor));
        return std::nullopt;
    }

    std::string contents;
    char buffer[4096];
    while(true) {
        const ssize_t read_size = read(descriptor, buffer, sizeof(buffer));
        if(read_size > 0) {
            contents.append(buffer, static_cast<std::size_t>(read_size));
            continue;
        }
        if(read_size == 0) break;
        if(errno == EINTR) continue;
        static_cast<void>(close(descriptor));
        return std::nullopt;
    }
    if(close(descriptor) != 0) return std::nullopt;
    return contents;
}

std::optional<std::string> read_srcinfo_version(const fs::path& pkg_dir) {
    std::optional<std::string> contents =
        read_nofollow_regular_file(pkg_dir / ".SRCINFO");
    if(!contents.has_value()) return std::nullopt;

    std::istringstream file(contents.value());

    std::string pkgver;
    std::string pkgrel;
    std::string epoch;
    std::string line;
    while(std::getline(file, line)) {
        std::string trimmed = trim(line);
        if(trimmed.starts_with("pkgver =")) {
            pkgver = trim(trimmed.substr(trimmed.find('=') + 1));
        } else if(trimmed.starts_with("pkgrel =")) {
            pkgrel = trim(trimmed.substr(trimmed.find('=') + 1));
        } else if(trimmed.starts_with("epoch =")) {
            if(!epoch.empty()) return std::nullopt;
            epoch = trim(trimmed.substr(trimmed.find('=') + 1));
            if(epoch.empty() ||
               epoch.find_first_not_of("0123456789") != std::string::npos) {
                return std::nullopt;
            }
        }
    }

    if(pkgver.empty() || pkgrel.empty()) return std::nullopt;
    // Match makepkg's full version: only a positive epoch adds a prefix.
    if(epoch.find_first_not_of('0') != std::string::npos) {
        return epoch + ":" + pkgver + "-" + pkgrel;
    }
    return pkgver + "-" + pkgrel;
}

UpdateCheckResult check_update_status(
    const std::string& pkg_name, const fs::path& pkg_dir,
    const SourceInstalledSnapshot& installed_snapshot,
    const std::optional<SourceUpdateBaseline>& update_baseline) {
    const std::optional<std::string>& installed_version =
        installed_snapshot.installed_version;
    if(!installed_version.has_value()) {
        return UpdateCheckResult::NeedsBuild; // インストールされていないのでビルド必要
    }

    // POLICY: upgrade の pre-review 更新判定では PKGBUILD を評価しない。
    // 既存 .SRCINFO が読めない場合は呼び出し元で対話確認または skip へ進める。
    std::optional<std::string> new_ver = read_srcinfo_version(pkg_dir);
    if(!new_ver.has_value()) return UpdateCheckResult::Unknown;

    std::string cmp_cmd = "vercmp " + shell_words::quote(new_ver.value()) + " " + shell_words::quote(installed_version.value()) + " 2>/dev/null";
    const CapturedCommandResult comparison_result = capture_command_output(cmp_cmd.c_str());
    if(comparison_result.exit_code != 0) return UpdateCheckResult::Unknown;

    try {
        std::size_t consumed_characters = 0;
        int version_comparison = std::stoi(comparison_result.output, &consumed_characters);
        if(consumed_characters != comparison_result.output.size()) {
            return UpdateCheckResult::Unknown;
        }
        if(version_comparison > 0) return UpdateCheckResult::NeedsBuild;
        if(version_comparison == 0 && update_baseline.has_value()) {
            const std::optional<std::string>& pre_upgrade_version =
                update_baseline->installed_version;
            // POLICY(#152,#215): pre/postは同じmetadata mappingのowned full versionなので、
            // system transaction中のversion変化は文字列の不一致として判定する。
            if(!pre_upgrade_version.has_value() ||
               pre_upgrade_version.value() != installed_version.value()) {
                if(pre_upgrade_version.has_value()) {
                    // TRANSLATORS: The placeholders are a package name, its previous version, and its installed version.
                    Logger::info(localization::format_translated_message(
                        "{} was updated by the system transaction ({} -> {}); rebuilding the preferred source package.",
                        pkg_name,
                        pre_upgrade_version.value(),
                        installed_version.value()));
                } else {
                    // TRANSLATORS: The placeholders are a package name and its installed version.
                    Logger::info(localization::format_translated_message(
                        "{} was installed by the system transaction at version {}; rebuilding the preferred source package.",
                        pkg_name,
                        installed_version.value()));
                }
                return UpdateCheckResult::NeedsBuild;
            }
        }
    } catch(...) {
        return UpdateCheckResult::Unknown;
    }

    Logger::info(up_to_date_diagnostic(pkg_name, installed_version.value()));
    return UpdateCheckResult::UpToDate;
}

bool has_local_package_artifact(const fs::path& pkg_dir) {
    std::error_code directory_error;
    fs::file_status directory_status =
        fs::symlink_status(pkg_dir, directory_error);
    if(directory_error || !fs::is_directory(directory_status)) return false;

    fs::directory_iterator entry(pkg_dir, directory_error);
    const fs::directory_iterator end;
    while(!directory_error && entry != end) {
        std::error_code status_error;
        const fs::file_status status = entry->symlink_status(status_error);
        if(!status_error && fs::is_regular_file(status)) {
            std::string filename = entry->path().filename().string();
            if(filename.size() < 4 ||
               filename.substr(filename.size() - 4) != ".sig") {
                if(filename.find(".pkg.tar") != std::string::npos) {
                    return true;
                }
            }
        }

        entry.increment(directory_error);
    }
    return false;
}

bool has_local_srcdir(const fs::path& pkg_dir) {
    std::error_code ec;
    const fs::file_status status =
        fs::symlink_status(pkg_dir / "src", ec);
    return !ec && fs::is_directory(status);
}

MakepkgBuildOptions resolve_makepkg_build_options(
    const fs::path& pkg_dir, const AppConfig& config) {
    MakepkgBuildOptions options;
    bool has_artifact = has_local_package_artifact(pkg_dir);

    if(config.user_config.build.mode == BuildMode::Clean) {
        options.clean_build = true;
    } else if(has_local_srcdir(pkg_dir)) {
        ConfirmationResult clean_confirmation = request_confirmation(
            localization::translate_message(
                "Clean build existing build directory?"),
            ConfirmationDefault::No, config.no_confirm);
        if(std::holds_alternative<ConfirmationAccepted>(
               clean_confirmation)) {
            options.clean_build = true;
        } else if(std::holds_alternative<ConfirmationDeclined>(
                      clean_confirmation)) {
            options.clean_build = false;
        } else {
            stop_after_confirmation(
                std::move(clean_confirmation), DiagnosticPhase::Build);
        }
    }

    if(config.user_config.build.mode == BuildMode::Rebuild) {
        options.rebuild = true;
    } else if(options.clean_build && has_artifact) {
        options.rebuild = true;
    } else if(has_artifact) {
        ConfirmationResult rebuild_confirmation = request_confirmation(
            localization::translate_message("Rebuild package?"),
            ConfirmationDefault::No, config.no_confirm);
        if(std::holds_alternative<ConfirmationAccepted>(
               rebuild_confirmation)) {
            options.rebuild = true;
        } else if(std::holds_alternative<ConfirmationDeclined>(
                      rebuild_confirmation)) {
            options.rebuild = false;
        } else {
            stop_after_confirmation(
                std::move(rebuild_confirmation), DiagnosticPhase::Build);
        }
    }

    return options;
}

struct PreparedSourceBuildCheckout {
    ReviewedProductionSourceExecution source_tree;
    MakepkgBuildOptions makepkg_options;
};

using SourceBuildCheckoutPreparation =
    std::variant<
        SourceBuildUpToDate,
        SourceBuildUpdateStatusUnknownSkipped,
        PreparedSourceBuildCheckout>;

SourceBuildCheckoutPreparation prepare_source_build_checkout(
    const SourceBuildRequest& request,
    const std::string& display_name,
    SourceBuildUpdatePolicy update_policy,
    const ValidatedCacheRoot& build_root,
    std::optional<ReviewedSourceFatalStatePreflight>
        reviewed_state_preflight,
    const AppConfig& config,
    const ReviewedDevelSourceBuildIntent* execution_intent) {
    require_valid_package_name(request.checkout_name);
    if(update_policy == SourceBuildUpdatePolicy::OnlyIfUpdated &&
       !request.installed_snapshot.has_value()) {
        // TRANSLATORS: The placeholder is a package name.
        throw std::runtime_error(localization::format_translated_message(
            "No authoritative installed package snapshot was supplied for {}.",
            request.package_name));
    }
    // TRANSLATORS: The placeholder is a package or PackageBase name.
    Logger::info(localization::format_translated_message(
        "Processing {}...", display_name));
    ValidatedCachePath pkg_path = require_trusted_cache_path(
        build_root, request.checkout_name,
        CachePathRequirement::ExistingOrMissing);

    bool existed_before_update = false;
    std::optional<ReviewedSourcePackageBaseLease> package_base_lease;
    std::string branch;

    {
        WorkDirGuard wd(build_root);
        bool needs_clone = true;

        if(pkg_path.exists() && pkg_path.is_directory() &&
           has_safe_persistent_checkout_git_directory(pkg_path)) {
            require_safe_persistent_checkout_descendants(pkg_path);
            package_base_lease.emplace(acquire_package_base_lease(
                pkg_path, request.aur_review_identity.has_value()));
            {
                WorkDirGuard wd_repo(pkg_path);
                std::string current_url =
                    trusted_git_remote_origin_url(pkg_path);
                if(!remote_url_matches_expected(current_url, request.git_url)) {
                    Logger::warn(localization::translate_message(
                        "Remote URL mismatch. Re-cloning..."));
                } else {
                    needs_clone = false;
                    existed_before_update = true;
                }
            }

            if(!needs_clone) {
                Logger::info(localization::translate_message(
                    "Updating repository..."));
                WorkDirGuard wd_repo(pkg_path);
                pkg_path = revalidate_trusted_cache_path(
                    pkg_path, CachePathRequirement::ExistingDirectory);
                require_safe_persistent_checkout_descendants(pkg_path);
                {
                    ScopedPrivateUmask private_umask;
                    if(trusted_git_fetch_origin(
                           pkg_path, request.git_url,
                           package_base_lease.value()) != 0) {
                        throw std::runtime_error(localization::translate_message(
                            "Failed to fetch updates."));
                    }
                }

                // fetch中にcheckoutまたはreview対象が差し替えられた場合、
                // branch検出を含む後続git commandへ進む前にauthorityを失効させる。
                pkg_path = revalidate_trusted_cache_path(
                    pkg_path, CachePathRequirement::ExistingDirectory);
                package_base_lease->require_unchanged_identity();
                require_safe_persistent_checkout_descendants(pkg_path);
            }
        }

        if(needs_clone) {
            if(pkg_path.exists()) {
                if(pkg_path.is_directory() &&
                   !package_base_lease.has_value()) {
                    package_base_lease.emplace(
                        acquire_package_base_lease(
                            pkg_path,
                            request.aur_review_identity.has_value()));
                }
                // POLICY(#175): remote mismatch/non-repository cleanup is limited to the validated cache entry.
                remove_trusted_cache_path(pkg_path);
                package_base_lease.reset();
            }
            pkg_path = create_trusted_cache_directory(
                build_root, request.checkout_name);
            package_base_lease.emplace(acquire_package_base_lease(
                pkg_path, request.aur_review_identity.has_value()));
            Logger::info(localization::translate_message(
                "Cloning repository..."));
            DirCleanupGuard cleanup_guard(pkg_path);
            {
                ScopedPrivateUmask private_umask;
                if(trusted_git_clone_persistent_checkout(
                       pkg_path, request.git_url,
                       package_base_lease.value()) != 0) {
                    // TRANSLATORS: The placeholder is a package checkout name.
                    throw std::runtime_error(localization::format_translated_message(
                        "Failed to clone {}",
                        request.checkout_name));
                }
            }

            pkg_path = revalidate_trusted_cache_path(
                pkg_path, CachePathRequirement::ExistingDirectory);
            require_safe_persistent_checkout_descendants(pkg_path);
            {
                WorkDirGuard wd_repo(pkg_path);
                std::string current_url = trim(
                    trusted_git_remote_origin_url(pkg_path));
                if(current_url.empty()) {
                    // TRANSLATORS: The placeholders are a literal Git configuration key and a package checkout name.
                    throw std::runtime_error(localization::format_translated_message(
                        "Missing {} for {}.",
                        "remote.origin.url",
                        request.checkout_name));
                }
                if(!remote_url_matches_expected(current_url, request.git_url)) {
                    // TRANSLATORS: The placeholder is a package checkout name.
                    throw std::runtime_error(localization::format_translated_message(
                        "Remote URL mismatch for {}.",
                        request.checkout_name));
                }
            }
            pkg_path = revalidate_trusted_cache_path(
                pkg_path, CachePathRequirement::ExistingDirectory);
            package_base_lease->require_unchanged_identity();
            require_safe_persistent_checkout_descendants(pkg_path);
            cleanup_guard.commit();
        }

        if(!package_base_lease.has_value()) {
            throw std::logic_error(
                "Source checkout has no PackageBase lease.");
        }
        pkg_path = revalidate_trusted_cache_path(
            pkg_path, CachePathRequirement::ExistingDirectory);
        package_base_lease->require_unchanged_identity();
        require_safe_persistent_checkout_descendants(pkg_path);
        const bool needs_branch = existed_before_update ||
                                  (request.aur_review_identity.has_value() &&
                                   should_run_reviewed_source_route(config));
        if(needs_branch) {
            WorkDirGuard wd_repo(pkg_path);
            branch = trusted_git_detect_remote_branch(
                pkg_path, request.git_url);
            // TRANSLATORS: The placeholder is a literal Git branch name.
            Logger::info(localization::format_translated_message(
                "Detected branch: {}", branch));
        }
    }

    std::optional<AurCheckoutAuthority> aur_authority;
    if(request.aur_review_identity.has_value()) {
        if(!reviewed_state_preflight.has_value()) {
            throw std::logic_error(
                "AUR source-build preparation lost reviewed-state preflight authority.");
        }
        aur_authority.emplace(prepare_aur_checkout_authority(
            request, pkg_path, branch,
            std::move(package_base_lease.value()),
            std::move(reviewed_state_preflight.value()),
            config));
        package_base_lease.reset();
    } else if(reviewed_state_preflight.has_value()) {
        throw std::logic_error(
            "Non-AUR source-build preparation received reviewed-state preflight authority.");
    }

    const bool uses_legacy_checkout = !aur_authority.has_value() ||
                                      std::holds_alternative<AurCompatibilityCheckout>(
                                          aur_authority.value());
    std::optional<WorkDirGuard> legacy_workdir;
    if(uses_legacy_checkout) legacy_workdir.emplace(pkg_path);
    if(uses_legacy_checkout && existed_before_update &&
       config.user_config.review.diff == ReviewPolicy::Prompt &&
       !aur_authority.has_value()) {
        int diff_ret = trusted_git_diff_quiet(
            pkg_path, request.git_url, branch);
        if(diff_ret > 1) {
            throw std::runtime_error(localization::translate_message(
                "Failed to compare repository changes."));
        }
        if(diff_ret == 1) {
            log_update_diff_guidance(
                pkg_path, request.git_url, branch);
            ConfirmationResult diff_confirmation = request_confirmation(
                localization::format_translated_message(
                    "Updates were detected in the existing cache repository. View the {} diff?",
                    "Git"),
                ConfirmationDefault::No, config.no_confirm);
            if(std::holds_alternative<ConfirmationAccepted>(
                   diff_confirmation)) {
                static_cast<void>(trusted_git_show_diff(
                    pkg_path, request.git_url, branch));
            } else if(!std::holds_alternative<ConfirmationDeclined>(
                          diff_confirmation)) {
                DiagnosticIdentity identity;
                identity.requested_package = request.package_name;
                identity.package_base = request.checkout_name;
                stop_after_confirmation(
                    std::move(diff_confirmation), DiagnosticPhase::Fetch,
                    std::move(identity));
            }
        }
    }

    if(uses_legacy_checkout && existed_before_update) {
        // LANDMINE: reset is build/install-only. Compatibility continuation
        // never receives reviewed/pinned authority.
        pkg_path = revalidate_trusted_cache_path(
            pkg_path, CachePathRequirement::ExistingDirectory);
        const ReviewedSourcePackageBaseLease& checkout_lease =
            aur_authority.has_value()
                ? std::get<AurCompatibilityCheckout>(
                      aur_authority.value())
                      .lease
                : package_base_lease.value();
        checkout_lease.require_unchanged_identity();
        require_safe_persistent_checkout_descendants(pkg_path);
        {
            ScopedPrivateUmask private_umask;
            if(trusted_git_reset_hard(
                   pkg_path, request.git_url, branch,
                   checkout_lease) != 0) {
                throw std::runtime_error(localization::translate_message(
                    "Failed to reset repository."));
            }
        }
        pkg_path = revalidate_trusted_cache_path(
            pkg_path, CachePathRequirement::ExistingDirectory);
        if(aur_authority.has_value()) {
            std::get<AurCompatibilityCheckout>(aur_authority.value())
                .lease.require_unchanged_identity();
        } else {
            package_base_lease->require_unchanged_identity();
        }
        require_safe_persistent_checkout_descendants(pkg_path);
    }

    MakepkgBuildOptions makepkg_options;
    bool editor_invoked = false;
    std::optional<ReviewedSourceEditorOverlayProof> editor_overlay;
    const auto run_checkout_command =
        [&aur_authority, &package_base_lease](
            const std::string& command) {
            if(aur_authority.has_value()) {
                return std::visit(
                    [&command](const auto& authority) {
                        if constexpr(std::is_same_v<
                                         std::decay_t<
                                             decltype(authority)>,
                                         AurCompatibilityCheckout>) {
                            return authority.lease.run_guarded_command(
                                command);
                        } else {
                            return authority.run_guarded_command(command);
                        }
                    },
                    aur_authority.value());
            }
            return package_base_lease->run_guarded_command(command);
        };
    {
        pkg_path = revalidate_trusted_cache_path(
            pkg_path, CachePathRequirement::ExistingDirectory);
        require_safe_persistent_checkout_descendants(pkg_path);
        WorkDirGuard wd(pkg_path);

        if(update_policy == SourceBuildUpdatePolicy::OnlyIfUpdated) {
            UpdateCheckResult update_check = check_update_status(
                request.package_name, ".",
                request.installed_snapshot.value(), request.update_baseline);
            if(update_check == UpdateCheckResult::UpToDate) {
                std::optional<ProductionSourceBuildProvenance> provenance;
                if(aur_authority.has_value()) {
                    auto selected = finalize_aur_checkout_authority(
                        std::move(aur_authority.value()),
                        pkg_path, std::nullopt, false);
                    provenance = std::get<ProductionArtifactSourceTree>(selected).provenance();
                } else {
                    ProductionArtifactSourceTree source_tree =
                        make_unreviewed_production_artifact_source_tree(
                            pkg_path,
                            std::move(package_base_lease.value()),
                            ProductionSourceReviewStatus::
                                NotApplicable,
                            ReviewedSourceEditorOverlayStatus::None);
                    provenance = source_tree.provenance();
                }
                static_cast<void>(revalidate_trusted_cache_path(
                    pkg_path, CachePathRequirement::ExistingDirectory));
                return SourceBuildUpToDate{
                    up_to_date_diagnostic(
                        request.package_name,
                        request.installed_snapshot->installed_version.value()),
                    std::move(provenance)};
            }
            if(update_check == UpdateCheckResult::Unknown) {
                // TRANSLATORS: The placeholders are the literal .SRCINFO file name and a package name.
                Logger::warn(localization::format_translated_message(
                    "Unable to determine update status from {} for {}.",
                    ".SRCINFO",
                    request.package_name));
                // TRANSLATORS: The placeholder is the literal PKGBUILD artifact name.
                Logger::warn(localization::format_translated_message(
                    "Skipping pre-review {} evaluation.", "PKGBUILD"));
                ConfirmationResult continue_confirmation =
                    request_confirmation(
                        localization::format_translated_message(
                            "Update status is unknown because {} is missing or incomplete. Continue to review/build?",
                            ".SRCINFO"),
                        ConfirmationDefault::No,
                        config.no_confirm);
                if(const auto* declined =
                       std::get_if<ConfirmationDeclined>(
                           &continue_confirmation)) {
                    SourceBuildUpdateStatusUnknownSkipReason reason =
                        SourceBuildUpdateStatusUnknownSkipReason::
                            UserDeclined;
                    if(declined->origin ==
                       ConfirmationDecisionOrigin::NoConfirm) {
                        reason = SourceBuildUpdateStatusUnknownSkipReason::
                            NoConfirm;
                    } else if(declined->origin ==
                              ConfirmationDecisionOrigin::
                                  NonInteractiveDefault) {
                        reason = SourceBuildUpdateStatusUnknownSkipReason::
                            NonInteractiveStdin;
                    }
                    std::string diagnostic = unknown_update_skip_diagnostic(
                        request.package_name, reason);
                    Logger::warn(diagnostic);
                    static_cast<void>(revalidate_trusted_cache_path(
                        pkg_path,
                        CachePathRequirement::ExistingDirectory));
                    return SourceBuildUpdateStatusUnknownSkipped{
                        reason,
                        std::move(diagnostic), std::nullopt};
                }
                if(!std::holds_alternative<ConfirmationAccepted>(
                       continue_confirmation)) {
                    DiagnosticIdentity identity;
                    identity.requested_package = request.package_name;
                    identity.package_base = request.checkout_name;
                    stop_after_confirmation(
                        std::move(continue_confirmation),
                        DiagnosticPhase::Preflight,
                        std::move(identity));
                }
            }
        }

        std::optional<ReviewedSourceEditorBoundary> editor_boundary;
        if(aur_authority.has_value() &&
           !std::holds_alternative<AurCompatibilityCheckout>(
               aur_authority.value())) {
            ReviewedSourceEditorBoundaryResult boundary =
                begin_aur_editor_boundary(aur_authority.value());
            if(auto* failure =
                   std::get_if<ReviewedSourcePublicationFailure>(
                       &boundary)) {
                stop_reviewed_source_route(
                    ReviewedSourceProductionFailureStage::
                        EditorOverlayObservation,
                    ReviewedSourceProductionFailureReason::
                        OverlayObservationFailure,
                    std::move(*failure));
            }
            editor_boundary.emplace(
                std::get<ReviewedSourceEditorBoundary>(
                    std::move(boundary)));
        }

        editor_invoked = review_build_files(
            pkg_path, config, run_checkout_command);
        if(editor_boundary.has_value()) {
            ReviewedSourceEditorOverlayProofResult sealed =
                seal_aur_editor_boundary(
                    aur_authority.value(),
                    std::move(editor_boundary.value()),
                    editor_invoked);
            if(auto* failure =
                   std::get_if<ReviewedSourcePublicationFailure>(
                       &sealed)) {
                stop_reviewed_source_route(
                    ReviewedSourceProductionFailureStage::
                        EditorOverlayObservation,
                    ReviewedSourceProductionFailureReason::
                        OverlayObservationFailure,
                    std::move(*failure));
            }
            editor_overlay.emplace(
                std::get<ReviewedSourceEditorOverlayProof>(
                    std::move(sealed)));
        }
        confirm_after_build_file_edit(
            pkg_path.canonical_path(), config, editor_invoked);
        makepkg_options = resolve_makepkg_build_options(".", config);
    }

    const std::string custom_environment = serialize_source_build_environment(
        request.custom_environment, request.empty_value_policy);
    if(!trim(custom_environment).empty()) {
        // TRANSLATORS: The placeholder is a literal serialized environment assignment list.
        Logger::info(localization::format_translated_message(
            "Applying custom build flags: {}", custom_environment));
    } else {
        // TRANSLATORS: The placeholder is the literal makepkg.conf file name.
        Logger::info(localization::format_translated_message(
            "Using default {} settings.", "makepkg.conf"));
    }

    // LANDMINE(#175,#197,#242,#268): review/editor後のcheckoutだけを
    // singular/set lifecycleへ渡す。private artifact rootはowner側で作る。
    pkg_path = revalidate_trusted_cache_path(
        pkg_path, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_descendants(pkg_path);
    ReviewedProductionSourceExecution source_tree = aur_authority.has_value()
                                                        ? finalize_aur_checkout_authority(
                                                              std::move(aur_authority.value()), pkg_path,
                                                              std::move(editor_overlay), editor_invoked, execution_intent)
                                                        : make_unreviewed_production_artifact_source_tree(
                                                              pkg_path, std::move(package_base_lease.value()),
                                                              ProductionSourceReviewStatus::NotApplicable,
                                                              editor_invoked
                                                                  ? ReviewedSourceEditorOverlayStatus::
                                                                        InvocationLocal
                                                                  : ReviewedSourceEditorOverlayStatus::None);
    return PreparedSourceBuildCheckout{
        std::move(source_tree), makepkg_options};
}

void require_package_base_source_build_request(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets) {
    require_valid_package_name(request.checkout_name);
    if(request.git_url.empty()) {
        throw std::logic_error(
            localization::format_translated_message(
                "{} set source-build request has an empty {} URL.",
                "PackageBase", "Git"));
    }
    if(request.only_if_updated) {
        throw std::logic_error(
            localization::format_translated_message(
                "{} set source-build does not support only-if-updated execution.",
                "PackageBase"));
    }
    if(required_targets.empty()) {
        throw std::logic_error(
            localization::format_translated_message(
                "{} set source-build requires at least one required package target.",
                "PackageBase"));
    }
    for(std::size_t index = 0; index < required_targets.size(); ++index) {
        const RequiredPackageArtifactTarget& target = required_targets[index];
        require_valid_package_name(target.package_base);
        require_valid_package_name(target.package_name);
        if(target.package_base != request.checkout_name) {
            throw std::logic_error(
                localization::format_translated_message(
                    "{} set source-build required target attribution is inconsistent.",
                    "PackageBase"));
        }
        if(std::any_of(
               required_targets.begin(),
               required_targets.begin() + index,
               [&target](const RequiredPackageArtifactTarget& existing) {
                   return existing.package_name == target.package_name;
               })) {
            throw std::logic_error(
                localization::format_translated_message(
                    "{} set source-build contains a duplicate required package target.",
                    "PackageBase"));
        }
        switch(target.desired_reason) {
            case DesiredInstallReason::Explicit:
            case DesiredInstallReason::Dependency:
                break;
            default:
                throw std::logic_error(
                    localization::format_translated_message(
                        "{} set source-build has an unknown install reason.",
                        "PackageBase"));
        }
    }
    if(required_targets.size() == 1) {
        require_valid_package_name(request.package_name);
        if(request.package_name != required_targets.front().package_name) {
            throw std::logic_error(
                localization::format_translated_message(
                    "{} set source-build singular request identity is inconsistent.",
                    "PackageBase"));
        }
    } else if(!request.package_name.empty()) {
        throw std::logic_error(
            localization::format_translated_message(
                "{} set source-build multiple request must not expose a singular package name.",
                "PackageBase"));
    }
}

} // namespace

std::shared_ptr<ReviewedSourceFatalStatePreflightSlot>
preflight_reviewed_source_fatal_state_for_production(
    const SourceBuildRequest& request) {
    if(!request.aur_review_identity.has_value()) return nullptr;

    ReviewedSourceFatalStatePreflightResult fatal_preflight =
        preflight_reviewed_source_fatal_state(
            request.aur_review_identity.value());
    if(auto* stop = std::get_if<ReviewedSourceOperationStop>(
           &fatal_preflight)) {
        stop_reviewed_source_operation(
            *stop,
            ReviewedSourceProductionFailureStage::FatalStatePreflight);
    }
    return std::shared_ptr<ReviewedSourceFatalStatePreflightSlot>(
        new ReviewedSourceFatalStatePreflightSlot(
            std::get<ReviewedSourceFatalStatePreflight>(
                std::move(fatal_preflight))));
}

SourceBuildPreparationOutcome prepare_source_build_for_execution(
    const SourceBuildRequest& request,
    const std::string& display_name,
    SourceBuildUpdatePolicy update_policy,
    const ValidatedCacheRoot& cache_root,
    const AppConfig& config,
    const ReviewedDevelSourceBuildIntent* execution_intent) {
    std::optional<ReviewedSourceFatalStatePreflight> reviewed_state_preflight;
    if(request.aur_review_identity.has_value()) {
        std::shared_ptr<ReviewedSourceFatalStatePreflightSlot> slot =
            request.reviewed_state_preflight;
        if(!slot) {
            slot = preflight_reviewed_source_fatal_state_for_production(
                request);
        }
        if(!slot || !slot->observation_.has_value()) {
            throw std::logic_error(
                "Reviewed source fatal-state preflight observation is missing or already consumed.");
        }
        reviewed_state_preflight.emplace(
            std::move(slot->observation_.value()));
        slot->observation_.reset();
    } else if(request.reviewed_state_preflight) {
        throw std::logic_error(
            "Repository source-build request contains an AUR reviewed-state preflight observation.");
    }

    // POLICY: checkoutのclone/fetch/resetより先に、artifact ownerと同じ
    // private cache contractを証明し、NeedsBuild capabilityへ一緒に保持する。
    ValidatedPrivateCacheRoot artifact_root = [&]() {
        try {
            return prepare_private_trusted_cache_root(cache_root);
        } catch(const TrustedCacheError&) {
            throw;
        } catch(const ConfirmationOperationStopped&) {
            throw;
        } catch(const std::exception& error) {
            throw SourceBuildPreparationError(
                SourceBuildPreparationFailureStage::ArtifactRoot,
                error.what());
        } catch(...) {
            throw SourceBuildPreparationError(
                SourceBuildPreparationFailureStage::ArtifactRoot,
                localization::translate_message(
                    "Private artifact workspace preparation failed."),
                true);
        }
    }();

    SourceBuildCheckoutPreparation checkout_preparation = [&]() {
        try {
            return prepare_source_build_checkout(
                request, display_name, update_policy, cache_root,
                std::move(reviewed_state_preflight), config, execution_intent);
        } catch(const ReviewedSourceProductionError&) {
            throw;
        } catch(const TrustedCacheError&) {
            throw;
        } catch(const ConfirmationOperationStopped&) {
            throw;
        } catch(const std::exception& error) {
            throw SourceBuildPreparationError(
                SourceBuildPreparationFailureStage::CheckoutOrReview,
                error.what());
        } catch(...) {
            throw SourceBuildPreparationError(
                SourceBuildPreparationFailureStage::CheckoutOrReview,
                localization::translate_message(
                    "Source checkout or build preparation failed."),
                true);
        }
    }();

    if(auto* up_to_date =
           std::get_if<SourceBuildUpToDate>(&checkout_preparation)) {
        return std::move(*up_to_date);
    }
    if(auto* skipped = std::get_if<
           SourceBuildUpdateStatusUnknownSkipped>(
           &checkout_preparation)) {
        return std::move(*skipped);
    }
    PreparedSourceBuildCheckout checkout = std::move(
        std::get<PreparedSourceBuildCheckout>(checkout_preparation));
    if(auto* devel = std::get_if<PreparedReviewedDevelSourceBuildExecution>(&checkout.source_tree))
        return SourceBuildPreparationAccess::make(std::move(*devel));
    if(std::holds_alternative<ReviewedDevelSourceBuildRejected>(checkout.source_tree))
        throw std::runtime_error("Authoritative reviewed devel execution intent was rejected; no legacy fallback.");
    return SourceBuildPreparationAccess::make(
        std::get<ProductionArtifactSourceTree>(std::move(checkout.source_tree)), std::move(artifact_root),
        checkout.makepkg_options.rebuild,
        checkout.makepkg_options.clean_build);
}

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS
void set_reviewed_source_before_publication_hook_for_test(
    ReviewedSourceBeforePublicationHookForTest hook) {
    g_reviewed_source_before_publication_hook = hook;
}

const ProductionSourceBuildProvenance&
prepared_source_build_provenance_for_test(
    const PreparedSourceBuildNeedsBuild& prepared) {
    if(!prepared.source_tree_.has_value()) {
        throw std::logic_error(
            "Prepared source-build test capability has no source tree.");
    }
    return prepared.source_tree_->provenance();
}
#endif

SourceBuildExecutionResult execute_source_build_typed(
    const SourceBuildRequest& input_request,
    const ValidatedCacheRoot& cache_root,
    DesiredInstallReason desired_reason,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    require_valid_package_name(input_request.package_name);
    SourceBuildRequest request = input_request;
    std::shared_ptr<const AurUpdateQueryResult> devel_query;
    std::optional<ConfirmationDecisionOrigin> devel_confirmation;
    if(request.only_if_updated && request.aur_review_identity && request.installed_snapshot && request.installed_snapshot->installed_version) {
        devel_query = std::make_shared<AurUpdateQueryResult>(query_registered_aur_devel_update(*request.aur_review_identity, request.package_name));
        const auto& entry = devel_query->plan.entries.front();
        const auto effective = project_aur_update_effective_state(entry);
        if(effective == AurUpdateEffectiveState::UpdateAvailable) {
            request.only_if_updated = false;
            request.authoritative_devel_update = aur_update_basis(entry) == AurUpdateBasis::GitRevision;
        } else if(effective == AurUpdateEffectiveState::RequiresCheck) {
            auto answer = request_confirmation(localization::translate_message("Devel update requires check. Continue with an explicit reviewed rebuild?"), ConfirmationDefault::No, config.no_confirm);
            if(const auto* accepted = std::get_if<ConfirmationAccepted>(&answer)) {
                devel_confirmation = accepted->origin;
                request.only_if_updated = false; // Explicit rebuild intent; still requires the normal reviewed pin.
                request.authoritative_devel_update = false;
            } else if(const auto* declined = std::get_if<ConfirmationDeclined>(&answer)) {
                SourceBuildExecutionResult out;
                out.status = SourceBuildExecutionStatus::DevelRequiresCheckSkipped;
                out.devel_update_query = devel_query;
                out.devel_rebuild_confirmation = declined->origin;
                out.diagnostic = localization::translate_message("Devel package requires check; explicit rebuild was not accepted.");
                return out;
            } else {
                DiagnosticIdentity identity;
                identity.requested_package = request.package_name;
                identity.package_base = request.checkout_name;
                stop_after_confirmation(std::move(answer), DiagnosticPhase::Preflight, std::move(identity));
            }
        } else if(effective == AurUpdateEffectiveState::UpToDate && entry.devel_assessment.state() == DevelUpdateAssessmentState::NotApplicable) {
            // Ordinary registered preference/baseline semantics stay on the legacy path.
        } else {
            SourceBuildExecutionResult out;
            out.devel_update_query = devel_query;
            out.status = effective == AurUpdateEffectiveState::UpToDate ? SourceBuildExecutionStatus::UpToDate : SourceBuildExecutionStatus::AuthoritativeIncomplete;
            out.diagnostic = effective == AurUpdateEffectiveState::UpToDate ? "Git revision matches the validated installed baseline." : effective == AurUpdateEffectiveState::Unknown     ? "Devel Git observation failed; no automatic build."
                                                                                                                                     : effective == AurUpdateEffectiveState::RequiresCheck ? "Devel package requires check; no automatic build."
                                                                                                                                                                                           : "Devel automatic update is unsupported.";
            return out;
        }
    }
    // generic/direct/system compatibility pathはsingular identityを引き続き要求する。
    require_valid_package_name(request.package_name);
    const ReviewedDevelSourceBuildIntent intent{request, {{request.checkout_name, request.package_name, desired_reason}}, database_paths, config.rm_deps, {config.no_confirm}};
    SourceBuildPreparationOutcome preparation = [&]() {
        try {
            return prepare_source_build_for_execution(
                request, request.package_name,
                request.only_if_updated
                    ? SourceBuildUpdatePolicy::OnlyIfUpdated
                    : SourceBuildUpdatePolicy::AlwaysBuild,
                cache_root, config, &intent);
        } catch(const SourceBuildPreparationError& error) {
            throw std::runtime_error(error.what());
        }
    }();
    if(const auto* up_to_date =
           std::get_if<SourceBuildUpToDate>(&preparation)) {
        SourceBuildExecutionResult result{
            SourceBuildExecutionStatus::UpToDate,
            std::nullopt,
            up_to_date->diagnostic,
            std::nullopt};
        if(up_to_date->source_provenance.has_value()) {
            result.production_outcome =
                ProductionSourceBuildStagedOutcome{
                    .source_provenance =
                        *up_to_date->source_provenance};
        }
        return result;
    }
    if(const auto* skipped = std::get_if<
           SourceBuildUpdateStatusUnknownSkipped>(&preparation)) {
        SourceBuildExecutionResult result{
            SourceBuildExecutionStatus::UpdateStatusUnknownSkipped,
            skipped->reason,
            skipped->diagnostic,
            std::nullopt};
        if(skipped->source_provenance.has_value()) {
            result.production_outcome =
                ProductionSourceBuildStagedOutcome{
                    .source_provenance =
                        *skipped->source_provenance};
        }
        return result;
    }
    auto& pending = std::get<PreparedSourceBuildNeedsBuild>(preparation);
    if(auto devel = SourceBuildPreparedExecutionAccess::take_devel(pending)) {
        SourceBuildExecutionResult out;
        out.devel_execution.emplace(execute_normal_reviewed_devel(std::move(*devel)));
        out.devel_update_query = devel_query;
        out.devel_rebuild_confirmation = devel_confirmation;
        out.status = out.devel_execution->complete ? SourceBuildExecutionStatus::Installed : SourceBuildExecutionStatus::AuthoritativeIncomplete;
        try {
            out.production_outcome = out.devel_execution->production_outcome;
        } catch(...) {
            out.devel_execution->projection_failed = true;
            out.devel_execution->complete = false;
            out.status = SourceBuildExecutionStatus::AuthoritativeIncomplete;
        }
        return out;
    }
    PreparedSourceBuildExecutionCapabilities prepared = SourceBuildPreparedExecutionAccess::consume(std::move(pending));

    const SeparatedSourceBuildUnitOptions options{
        .no_confirm = config.no_confirm,
        .needed = request.needed,
        .rm_deps = config.rm_deps,
        .rebuild = prepared.rebuild,
        .clean_build = prepared.clean_build};
    SeparatedSourceBuildExecutionResult separated =
        execute_separated_source_build_unit(
            SeparatedSourceBuildUnitRequest{
                std::move(prepared.source_tree),
                std::move(prepared.artifact_root),
                request.package_name,
                request.checkout_name,
                desired_reason,
                request.custom_environment,
                request.empty_value_policy,
                database_paths},
            options);
    SourceBuildExecutionResult result =
        source_build_result_from_artifact_outcome(
            separated.install_outcome);
    result.production_outcome = std::move(separated.production_outcome);
    result.devel_update_query = devel_query;
    result.devel_rebuild_confirmation = devel_confirmation;
    return result;
}

SourceBuildPackageBaseExecutionResult
execute_prepared_source_build_package_base_typed(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets,
    PreparedSourceBuildNeedsBuild prepared,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    require_package_base_source_build_request(request, required_targets);
    require_supported_separated_install_options(config.rm_deps);
    require_unclaimed_artifact_pkgdest(request.custom_environment);
    if(auto devel = SourceBuildPreparedExecutionAccess::take_devel(prepared)) {
        return execute_normal_reviewed_devel(std::move(*devel));
    }
    PreparedSourceBuildExecutionCapabilities capabilities =
        SourceBuildPreparedExecutionAccess::consume(
            std::move(prepared));

    return execute_separated_package_base_source_build(
        SeparatedPackageBaseSourceBuildRequest{
            std::move(capabilities.source_tree),
            std::move(capabilities.artifact_root),
            request.checkout_name,
            required_targets,
            request.custom_environment,
            request.empty_value_policy,
            database_paths},
        SeparatedSourceBuildUnitOptions{
            .no_confirm = config.no_confirm,
            .needed = request.needed,
            .rm_deps = config.rm_deps,
            .rebuild = capabilities.rebuild,
            .clean_build = capabilities.clean_build});
}

PackageBaseSourceBuildExecutionResult
execute_prepared_source_build_package_base_with_cleanup_authority(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets,
    PreparedSourceBuildNeedsBuild prepared,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config,
    RemoteAurCleanupCandidateCollector& collector,
    std::size_t work_item_index) {
    require_package_base_source_build_request(request, required_targets);
    require_supported_separated_install_options(config.rm_deps);
    require_unclaimed_artifact_pkgdest(request.custom_environment);
    PreparedSourceBuildExecutionCapabilities capabilities =
        SourceBuildPreparedExecutionAccess::consume(std::move(prepared));

    return execute_separated_package_base_source_build_with_cleanup_authority(
        SeparatedPackageBaseSourceBuildRequest{
            std::move(capabilities.source_tree),
            std::move(capabilities.artifact_root),
            request.checkout_name,
            required_targets,
            request.custom_environment,
            request.empty_value_policy,
            database_paths},
        SeparatedSourceBuildUnitOptions{
            .no_confirm = config.no_confirm,
            .needed = request.needed,
            .rm_deps = config.rm_deps,
            .rebuild = capabilities.rebuild,
            .clean_build = capabilities.clean_build},
        collector, work_item_index);
}

namespace {

SourceBuildPackageBaseExecutionResult
execute_source_build_package_base_typed_impl(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets,
    const ValidatedCacheRoot& cache_root,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config,
    RemoteAurCleanupCandidateCollector* collector,
    std::optional<std::size_t> work_item_index) {
    // request/target correlationはcheckout/cache mutationより前に再証明する。
    require_package_base_source_build_request(request, required_targets);
    require_supported_separated_install_options(config.rm_deps);
    require_unclaimed_artifact_pkgdest(request.custom_environment);
    const ReviewedDevelSourceBuildIntent intent{request, required_targets, database_paths, config.rm_deps, {config.no_confirm}};
    SourceBuildPreparationOutcome preparation = [&]() {
        try {
            return prepare_source_build_for_execution(
                request, request.checkout_name,
                SourceBuildUpdatePolicy::AlwaysBuild,
                cache_root, config, collector ? nullptr : &intent);
        } catch(const ReviewedSourceProductionError& error) {
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::Build,
                error.what(), error.failure());
        } catch(const TrustedCacheError&) {
            throw;
        } catch(const SourceBuildPreparationError& error) {
            const bool artifact_root_failure =
                error.stage() ==
                SourceBuildPreparationFailureStage::ArtifactRoot;
            std::string diagnostic;
            if(error.unknown_exception()) {
                diagnostic = artifact_root_failure
                                 ? localization::format_translated_message(
                                       "{} private artifact workspace preparation failed.",
                                       "PackageBase")
                                 : localization::format_translated_message(
                                       "{} source checkout or build preparation failed.",
                                       "PackageBase");
            } else {
                diagnostic = artifact_root_failure
                                 ? localization::format_translated_message(
                                       "{} private artifact workspace preparation failed: {}",
                                       "PackageBase", error.what())
                                 : localization::format_translated_message(
                                       "{} source checkout or build preparation failed: {}",
                                       "PackageBase", error.what());
            }
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::Build,
                std::move(diagnostic));
        }
    }();
    if(!std::holds_alternative<PreparedSourceBuildNeedsBuild>(preparation)) {
        throw std::logic_error(
            localization::format_translated_message(
                "{} set source-build unexpectedly produced an update-status result.",
                "PackageBase"));
    }
    PreparedSourceBuildNeedsBuild prepared = std::move(
        std::get<PreparedSourceBuildNeedsBuild>(preparation));
    if(collector != nullptr) {
        if(!work_item_index.has_value()) {
            throw std::logic_error(
                "Remote AUR cleanup source-build has no work-item attribution.");
        }
        return execute_prepared_source_build_package_base_with_cleanup_authority(
            request, required_targets, std::move(prepared),
            database_paths, config, *collector,
            work_item_index.value());
    }
    return execute_prepared_source_build_package_base_typed(
        request, required_targets, std::move(prepared),
        database_paths, config);
}

} // namespace

SourceBuildPackageBaseExecutionResult
execute_source_build_package_base_typed(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets,
    const ValidatedCacheRoot& cache_root,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    return execute_source_build_package_base_typed_impl(
        request, required_targets, cache_root, database_paths,
        config, nullptr, std::nullopt);
}

PackageBaseSourceBuildExecutionResult
execute_source_build_package_base_with_cleanup_authority(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets,
    const ValidatedCacheRoot& cache_root,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config,
    RemoteAurCleanupCandidateCollector& collector,
    std::size_t work_item_index) {
    return std::get<PackageBaseSourceBuildExecutionResult>(execute_source_build_package_base_typed_impl(
        request, required_targets, cache_root, database_paths,
        config, &collector, work_item_index));
}
