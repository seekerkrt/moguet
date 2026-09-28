#include "app_config.hpp"
#include "cache_authority.hpp"
#include "generated_recipe_patch.hpp"
#include "review_recipe_patch_save.hpp"
#include "source_install.hpp"
#include "xdg_generation_store.hpp"
#include "reviewed_source_pinned_build.hpp"
#include "reviewed_source_production_outcome.hpp"
#include "reviewed_source_state_store.hpp"
#include "source_build.hpp"
#include "source_package_identity.hpp"
#include "trusted_cache.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace reviewed_source_production_execution_stub {

void fail_next_install_transaction();
void fail_next_package_metadata();

} // namespace reviewed_source_production_execution_stub

namespace fs = std::filesystem;

namespace {

constexpr std::string_view PACKAGE_BASE = "reviewed-production-fixture";
constexpr std::string_view CANONICAL_REMOTE =
    "https://aur.archlinux.org/reviewed-production-fixture.git";

void require(bool condition, std::string_view diagnostic) {
    if(!condition) throw std::runtime_error(std::string(diagnostic));
}

void require_contains(
    const std::string& text,
    std::string_view expected,
    std::string_view diagnostic) {
    require(text.find(expected) != std::string::npos, diagnostic);
}

ProductionSourceBuildProvenance reviewed_provenance(
    ProductionReviewedSourceOutcome outcome,
    ReviewedSourcePublicationStatus publication_status =
        ReviewedSourcePublicationStatus::Published,
    ReviewedSourceEditorOverlayStatus editor_overlay =
        ReviewedSourceEditorOverlayStatus::None,
    std::optional<ReviewedSourceAbnormalStateReason> abnormal_reason =
        std::nullopt) {
    ProductionSourceBuildProvenance provenance;
    provenance.review_status = ProductionSourceReviewStatus::Reviewed;
    provenance.editor_overlay = editor_overlay;
    provenance.reviewed_upstream_base_revision =
        SourceRevisionIdentity::git_commit(
            "1111111111111111111111111111111111111111");
    provenance.publication_status = publication_status;
    provenance.reviewed_outcome = outcome;
    provenance.abnormal_state_reason = abnormal_reason;
    provenance.reviewed_state_generation = 17;
    return provenance;
}

void require_staged_grouping(const ProductionSourceBuildProvenance& provenance) {
    const auto standalone = format_reviewed_source_production_outcome("matrix-base", provenance);
    for(const auto build : {ProductionSourceBuildCommandOutcome::NotAttempted,
                            ProductionSourceBuildCommandOutcome::Started,
                            ProductionSourceBuildCommandOutcome::Failed,
                            ProductionSourceBuildCommandOutcome::Succeeded}) {
        for(const auto install : {ProductionSourceInstallOutcome::NotAttempted,
                                  ProductionSourceInstallOutcome::Started,
                                  ProductionSourceInstallOutcome::Failed,
                                  ProductionSourceInstallOutcome::Succeeded}) {
            const auto staged = format_production_source_build_staged_outcome(
                "matrix-base", ProductionSourceBuildStagedOutcome{provenance, build, install});
            require(staged.info_lines.size() == standalone.info_lines.size() + 2,
                    "Grouping changed the number of info events");
            for(std::size_t index = 0; index < standalone.info_lines.size(); ++index) {
                require(staged.info_lines[index] == standalone.info_lines[index] +
                                                        (index + 1 == standalone.info_lines.size() ? "\n" : ""),
                        "Only the final provenance line may carry the group separator");
                require(standalone.info_lines[index].find('\n') == std::string::npos,
                        "Standalone provenance gained a separator");
            }
            const auto first_outcome = standalone.info_lines.size();
            require(staged.info_lines[first_outcome].starts_with("Build outcome for PackageBase matrix-base: ") &&
                        staged.info_lines[first_outcome + 1].starts_with("Install outcome for PackageBase matrix-base: "),
                    "Terminal outcome order or PackageBase attribution changed");
            require(staged.info_lines[first_outcome].find('\n') == std::string::npos &&
                        staged.info_lines[first_outcome + 1].find('\n') == std::string::npos,
                    "Terminal outcomes gained leading, intervening, or trailing blank lines");
        }
    }
}

void test_outcome_projection_matrix() {
    require_staged_grouping(ProductionSourceBuildProvenance{});
    struct ReviewedCase {
        ProductionReviewedSourceOutcome outcome;
        std::optional<ReviewedSourceAbnormalStateReason> abnormal_reason;
        std::string_view expected;
    };
    for(const ReviewedCase& test_case : {
            ReviewedCase{
                ProductionReviewedSourceOutcome::InitialFullReview,
                std::nullopt, "initial full review accepted"},
            ReviewedCase{
                ProductionReviewedSourceOutcome::UpdateReview,
                std::nullopt, "update review accepted"},
            ReviewedCase{
                ProductionReviewedSourceOutcome::
                    RebaselineFullReview,
                std::nullopt, "explicit full rebaseline accepted"},
            ReviewedCase{
                ProductionReviewedSourceOutcome::
                    AbnormalStateRebindFullReview,
                ReviewedSourceAbnormalStateReason::Invalid,
                "rebind/rebaseline of invalid state"},
            ReviewedCase{
                ProductionReviewedSourceOutcome::
                    AbnormalStateRebindFullReview,
                ReviewedSourceAbnormalStateReason::Corrupted,
                "rebind/rebaseline of corrupted state"},
            ReviewedCase{
                ProductionReviewedSourceOutcome::
                    AbnormalStateRebindFullReview,
                ReviewedSourceAbnormalStateReason::SourceMismatch,
                "source-identity rebind/rebaseline"},
        }) {
        ReviewedSourceProductionOutcomePresentation presentation =
            format_reviewed_source_production_outcome(
                "matrix-base",
                reviewed_provenance(
                    test_case.outcome,
                    ReviewedSourcePublicationStatus::Published,
                    ReviewedSourceEditorOverlayStatus::None,
                    test_case.abnormal_reason));
        require(presentation.info_lines.size() == 3,
                "Reviewed outcome projection line count differs");
        require_contains(
            presentation.info_lines.front(), test_case.expected,
            "Reviewed outcome kind was flattened");
        require_contains(
            presentation.info_lines[1], "generation 17",
            "Reviewed outcome lost state generation");
        require_staged_grouping(reviewed_provenance(
            test_case.outcome, ReviewedSourcePublicationStatus::Published,
            ReviewedSourceEditorOverlayStatus::None, test_case.abnormal_reason));
    }

    ReviewedSourceProductionOutcomePresentation already =
        format_reviewed_source_production_outcome(
            "matrix-base",
            reviewed_provenance(
                ProductionReviewedSourceOutcome::AlreadyReviewed,
                ReviewedSourcePublicationStatus::
                    AlreadyPublishedSameTarget));
    require_contains(
        already.info_lines.front(), "was already reviewed",
        "AlreadyReviewed was flattened into new acceptance");
    require_contains(
        already.info_lines[1], "remains at generation 17",
        "AlreadyReviewed lost unchanged generation");
    require_staged_grouping(reviewed_provenance(
        ProductionReviewedSourceOutcome::AlreadyReviewed,
        ReviewedSourcePublicationStatus::AlreadyPublishedSameTarget));

    ReviewedSourceProductionOutcomePresentation raced =
        format_reviewed_source_production_outcome(
            "matrix-base",
            reviewed_provenance(
                ProductionReviewedSourceOutcome::UpdateReview,
                ReviewedSourcePublicationStatus::
                    AlreadyPublishedSameTarget));
    require_contains(
        raced.info_lines[1],
        "already matched the accepted target at publication",
        "Same-target publication was flattened into a fresh write");

    ReviewedSourceProductionOutcomePresentation overlay =
        format_reviewed_source_production_outcome(
            "matrix-base",
            reviewed_provenance(
                ProductionReviewedSourceOutcome::UpdateReview,
                ReviewedSourcePublicationStatus::Published,
                ReviewedSourceEditorOverlayStatus::
                    InvocationLocal));
    require_contains(
        overlay.info_lines.back(),
        "it is not the exact reviewed commit tree",
        "Editor overlay was flattened into the reviewed commit");
    require_staged_grouping(reviewed_provenance(
        ProductionReviewedSourceOutcome::UpdateReview,
        ReviewedSourcePublicationStatus::Published,
        ReviewedSourceEditorOverlayStatus::InvocationLocal));

    struct CompatibilityCase {
        ReviewedSourceCompatibilityBuildReason reason;
        std::string_view expected;
    };
    for(const CompatibilityCase& test_case : {
            CompatibilityCase{
                ReviewedSourceCompatibilityBuildReason::NoDiff,
                "--nodiff skipped review acceptance"},
            CompatibilityCase{
                ReviewedSourceCompatibilityBuildReason::NoConfirm,
                "--noconfirm did not create review acceptance"},
            CompatibilityCase{
                ReviewedSourceCompatibilityBuildReason::
                    NonInteractiveInput,
                "stdin is non-interactive"},
            CompatibilityCase{
                ReviewedSourceCompatibilityBuildReason::
                    ExplicitReviewDecline,
                "review acceptance was explicitly declined"},
            CompatibilityCase{
                ReviewedSourceCompatibilityBuildReason::
                    DefaultReviewDecline,
                "declined by the safe default"},
        }) {
        ProductionSourceBuildProvenance provenance;
        provenance.review_status =
            ProductionSourceReviewStatus::CompatibilityWithoutReview;
        provenance.compatibility_reason = test_case.reason;
        ReviewedSourceProductionOutcomePresentation presentation =
            format_reviewed_source_production_outcome(
                "matrix-base", provenance);
        require(presentation.info_lines.size() == 1,
                "Compatibility outcome projection line count differs");
        require_contains(
            presentation.info_lines.front(), test_case.expected,
            "Compatibility reason was flattened");
        require_contains(
            presentation.info_lines.front(),
            "reviewed state was not advanced",
            "Compatibility outcome claimed reviewed state advance");
        require_staged_grouping(provenance);
        provenance.editor_overlay = ReviewedSourceEditorOverlayStatus::InvocationLocal;
        require_staged_grouping(provenance);
    }
}

void test_failure_projection_matrix() {
    struct FailureCase {
        ReviewedSourceProductionFailureReason reason;
        std::string_view expected;
    };
    for(const FailureCase& test_case : {
            FailureCase{
                ReviewedSourceProductionFailureReason::
                    UnsupportedFuture,
                "unsupported future schema"},
            FailureCase{
                ReviewedSourceProductionFailureReason::UnsafeHistory,
                "history is unsafe"},
            FailureCase{
                ReviewedSourceProductionFailureReason::
                    StateStoreFailure,
                "could not be read safely"},
            FailureCase{
                ReviewedSourceProductionFailureReason::
                    InconsistentStateObservation,
                "observation was inconsistent"},
            FailureCase{
                ReviewedSourceProductionFailureReason::LeaseContended,
                "lease is already held"},
            FailureCase{
                ReviewedSourceProductionFailureReason::
                    ExactCheckoutFailure,
                "exact checkout materialization failed"},
            FailureCase{
                ReviewedSourceProductionFailureReason::
                    PublicationConflict,
                "publication conflicted"},
            FailureCase{
                ReviewedSourceProductionFailureReason::
                    PublishedUncertain,
                "post-commit ambiguity"},
            FailureCase{
                ReviewedSourceProductionFailureReason::
                    PostPublicationCheckoutFailure,
                "changed after state publication"},
        }) {
        const std::string diagnostic =
            reviewed_source_production_failure_diagnostic(
                ReviewedSourceProductionFailure{
                    ReviewedSourceProductionFailureStage::
                        StatePublication,
                    test_case.reason, std::monostate{}});
        require_contains(
            diagnostic, test_case.expected,
            "Typed reviewed-source failure was flattened");
        require_contains(
            diagnostic, "build was not started",
            "Reviewed-source STOP diagnostic lost build status");
    }

    for(const auto& [reason, expected] : {
            std::pair{
                ReviewedSourceOperationStopReason::
                    ManualInspectionRequired,
                std::string_view{"requires manual inspection"}},
            std::pair{
                ReviewedSourceOperationStopReason::
                    SensitiveSourceUnrenderable,
                std::string_view{"could not be rendered safely"}},
        }) {
        const std::string diagnostic =
            reviewed_source_production_failure_diagnostic(
                ReviewedSourceProductionFailure{
                    ReviewedSourceProductionFailureStage::
                        Acceptance,
                    ReviewedSourceProductionFailureReason::
                        ReviewOperationStopped,
                    ReviewedSourceOperationStop::make(reason)});
        require_contains(
            diagnostic, expected,
            "Readiness STOP reason was flattened");
        require_contains(
            diagnostic,
            "without state publication or compatibility fallback",
            "Readiness STOP diagnostic allowed fallback");
    }
}

std::string shell_quote(const fs::path& path) {
    std::string result = "'";
    for(char character : path.string()) {
        if(character == '\'')
            result += "'\\''";
        else
            result += character;
    }
    result += "'";
    return result;
}

void run(const std::string& command) {
    require(std::system(command.c_str()) == 0, "Fixture command failed");
}

std::string capture(const std::string& command) {
    FILE* stream = popen(command.c_str(), "r");
    if(stream == nullptr) throw std::runtime_error("Fixture capture failed");
    std::string result;
    char buffer[256];
    while(fgets(buffer, sizeof(buffer), stream) != nullptr) {
        result += buffer;
    }
    require(pclose(stream) == 0, "Fixture capture command failed");
    while(!result.empty() &&
          (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

void write_file(
    const fs::path& path, std::string_view contents,
    fs::perms permissions = fs::perms::owner_read |
                            fs::perms::owner_write) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "Failed to open fixture file");
    output.write(
        contents.data(),
        static_cast<std::streamsize>(contents.size()));
    output.close();
    require(static_cast<bool>(output), "Failed to write fixture file");
    fs::permissions(path, permissions, fs::perm_options::replace);
}

class TemporaryTree final {
    fs::path path_;

public:
    TemporaryTree() {
        std::string pattern =
            (fs::temp_directory_path() / "moguet-reviewed-source-production-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = mkdtemp(writable.data());
        if(created == nullptr) {
            throw std::system_error(
                errno, std::generic_category(),
                "Failed to create production review fixture");
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
};

PackageBaseIdentity aur_identity() {
    return PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(
                std::string(CANONICAL_REMOTE))),
        std::string(PACKAGE_BASE));
}

SourceBuildRequest request() {
    SourceBuildRequest result;
    result.package_name = PACKAGE_BASE;
    result.checkout_name = PACKAGE_BASE;
    result.git_url = CANONICAL_REMOTE;
    result.aur_review_identity = aur_identity();
    return result;
}

AppConfig reviewed_config(const fs::path& editor) {
    AppConfig config;
    config.user_config.review.diff = ReviewPolicy::Prompt;
    config.user_config.review.pkgbuild = ReviewPolicy::Prompt;
    config.editor = editor.string();
    return config;
}

void require_loaded_state(
    const SourceRevisionIdentity& target,
    std::uint64_t generation) {
    ReviewedSourceStateStoreReadResult result =
        read_reviewed_source_state(aur_identity());
    const auto* read = std::get_if<ReviewedSourceStateStoreRead>(&result);
    require(read != nullptr, "Reviewed state lookup failed");
    const auto* loaded =
        std::get_if<ReviewedSourceStateLoaded>(&read->observation);
    require(loaded != nullptr && read->observed.has_value(),
            "Reviewed state was not loaded");
    require(loaded->state.reviewed_revision() == target &&
                read->observed->generation == generation,
            "Reviewed state target or generation differs");
}

std::optional<ReviewedSourceState> g_concurrent_reviewed_state;
std::optional<ReviewedSourceStateObservedRecord>
    g_concurrent_expected_record;
bool g_concurrent_publication_succeeded = false;
fs::path g_post_publication_mutation_checkout;

void publish_concurrent_reviewed_state() {
    if(!g_concurrent_reviewed_state.has_value()) return;
    ReviewedSourceStateStorePublishResult result =
        publish_reviewed_source_state(
            *g_concurrent_reviewed_state,
            g_concurrent_expected_record);
    g_concurrent_publication_succeeded =
        std::holds_alternative<ReviewedSourceStateStorePublished>(
            result);
}

void mutate_checkout_after_publication(
    const ReviewedSourceStateStoreTestRaceContext&) {
    std::ofstream output(
        g_post_publication_mutation_checkout /
            "ignored-production-race.tmp",
        std::ios::binary | std::ios::trunc);
    output << "post-publication mutation\n";
}

class ReviewAnswers final {
    std::istringstream answers_;
    std::streambuf* original_;

public:
    explicit ReviewAnswers(std::string answers)
        : answers_(std::move(answers)), original_(std::cin.rdbuf(answers_.rdbuf())) {
        std::cin.clear();
    }
    ~ReviewAnswers() {
        std::cin.rdbuf(original_);
        std::cin.clear();
    }
};

class ReviewOutput final {
    std::ostringstream output_;
    std::streambuf* original_ = std::cout.rdbuf(output_.rdbuf());

public:
    ~ReviewOutput() {
        std::cout.rdbuf(original_);
    }
    std::string text() const {
        return output_.str();
    }
};

std::string read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot read fixture bytes");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string g_recipe_fault_command;
SourceBuildRequest* g_recipe_fault_request = nullptr;
std::optional<PackageBaseIdentity> g_recipe_fault_identity;
bool g_recipe_fault_invoked = false;
void mutate_after_recipe_snapshot() {
    g_recipe_fault_invoked = true;
    if(!g_recipe_fault_command.empty()) run(g_recipe_fault_command);
    if(g_recipe_fault_request) g_recipe_fault_request->aur_review_identity = g_recipe_fault_identity;
}

} // namespace

int main() {
    try {
        test_outcome_projection_matrix();
        test_failure_projection_matrix();
        TemporaryTree tree;
        const fs::path home = tree.path() / "home";
        const fs::path cache_home = tree.path() / "cache";
        const fs::path state_home = tree.path() / "state";
        const fs::path work = tree.path() / "upstream-work";
        const fs::path remote = tree.path() / "upstream.git";
        const fs::path wrapper = tree.path() / "git-wrapper";
        const fs::path editor = tree.path() / "editor-wrapper";
        fs::create_directories(home);
        fs::create_directories(cache_home);
        fs::create_directories(state_home);
        fs::permissions(home, fs::perms::owner_all, fs::perm_options::replace);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        fs::permissions(
            state_home, fs::perms::owner_all,
            fs::perm_options::replace);
        require(setenv("HOME", home.c_str(), 1) == 0 &&
                    setenv("XDG_CACHE_HOME", cache_home.c_str(), 1) == 0 &&
                    setenv("XDG_STATE_HOME", state_home.c_str(), 1) == 0,
                "Failed to set fixture XDG environment");

        run("/usr/bin/git init -q -b main " + shell_quote(work));
        run("/usr/bin/git -C " + shell_quote(work) +
            " config user.email test@example.invalid");
        run("/usr/bin/git -C " + shell_quote(work) +
            " config user.name 'Moguet Test'");
        write_file(
            work / "PKGBUILD",
            "pkgname=reviewed-production-fixture\npkgver=1\npkgrel=1\n");
        run("/usr/bin/git -C " + shell_quote(work) + " add PKGBUILD");
        run("/usr/bin/git -C " + shell_quote(work) +
            " commit -q -m initial");
        run("/usr/bin/git clone -q --bare " + shell_quote(work) + " " +
            shell_quote(remote));

        ValidatedCacheRoot cache_root = prepare_process_cache_root();
        ValidatedCachePath checkout = create_trusted_cache_directory(
            cache_root, std::string(PACKAGE_BASE));
        run("/usr/bin/git -C " + shell_quote(checkout.path()) +
            " init -q -b main");
        run("/usr/bin/git -C " + shell_quote(checkout.path()) +
            " remote add origin " + shell_quote(remote));
        run("/usr/bin/git -C " + shell_quote(checkout.path()) +
            " fetch -q origin main");
        run("/usr/bin/git -C " + shell_quote(checkout.path()) +
            " checkout -q -B main origin/main");
        run("/usr/bin/git -C " + shell_quote(checkout.path()) +
            " remote set-url origin '" + std::string(CANONICAL_REMOTE) +
            "'");

        write_file(
            work / "PKGBUILD",
            "pkgname=reviewed-production-fixture\npkgver=2\npkgrel=1\n");
        run("/usr/bin/git -C " + shell_quote(work) + " add PKGBUILD");
        run("/usr/bin/git -C " + shell_quote(work) +
            " commit -q -m update");
        run("/usr/bin/git -C " + shell_quote(work) + " push -q " +
            shell_quote(remote) + " main");
        const std::string target_oid = capture(
            "/usr/bin/git -C " + shell_quote(work) +
            " rev-parse HEAD");
        const SourceRevisionIdentity target =
            SourceRevisionIdentity::git_commit(target_oid);

        write_file(
            wrapper,
            "#!/bin/sh\n"
            "set -eu\n"
            "is_fetch=0\n"
            "is_checkout=0\n"
            "for argument do\n"
            "  if [ \"$argument\" = fetch ]; then is_fetch=1; fi\n"
            "  if [ \"$argument\" = checkout ]; then is_checkout=1; fi\n"
            "done\n"
            "if [ \"$is_fetch\" -eq 1 ]; then\n"
            "  exec /usr/bin/git -c protocol.file.allow=always fetch -q " +
                shell_quote(remote) +
                " '+refs/heads/*:refs/remotes/origin/*'\n"
                "fi\n"
                "if [ \"${MOGUET_TEST_FAIL_PINNED_CHECKOUT:-0}\" = 1 ] && "
                "[ \"$is_checkout\" -eq 1 ]; then exit 75; fi\n"
                "exec /usr/bin/git \"$@\"\n",
            fs::perms::owner_read | fs::perms::owner_write |
                fs::perms::owner_exec);
        write_file(
            editor,
            "#!/bin/sh\n"
            "set -eu\n"
            "for target do :; done\n"
            "printf '%s\\n' '# invocation-local editor overlay' >>\"$target\"\n",
            fs::perms::owner_read | fs::perms::owner_write |
                fs::perms::owner_exec);
        require(setenv("MOGUET_TEST_GIT_EXECUTABLE", wrapper.c_str(), 1) == 0,
                "Failed to set trusted Git test executable");

        {
            ReviewAnswers answers("y\ny\ny\nn\n");
            ReviewOutput output;
            SourceBuildPreparationOutcome outcome =
                prepare_source_build_for_execution(
                    request(), std::string(PACKAGE_BASE),
                    SourceBuildUpdatePolicy::AlwaysBuild,
                    cache_root, reviewed_config(editor));
            const std::string text = output.text();
            const auto prompt = text.find(":: Accept this reviewed upstream revision? [y/n]");
            require(prompt >= 3 && prompt != std::string::npos &&
                        text.substr(prompt - 2, 2) == "\n\n" && text[prompt - 3] != '\n',
                    "Production review/prompt boundary must contain exactly one blank line");
            auto* prepared =
                std::get_if<PreparedSourceBuildNeedsBuild>(&outcome);
            require(prepared != nullptr,
                    "Accepted production review did not prepare a build");
            const auto& edit = prepared->accepted_recipe_edit();
            require(edit && edit->baseline_pkgbuild() == "pkgname=reviewed-production-fixture\npkgver=2\npkgrel=1\n" &&
                        edit->accepted_pkgbuild() == edit->baseline_pkgbuild() + "# invocation-local editor overlay\n" &&
                        edit->identity().package_base() == aur_identity() && edit->identity().target_revision() == target &&
                        edit->checkout_device() == checkout.device() && edit->checkout_inode() == checkout.inode(),
                    "Accepted PKGBUILD bytes or source/target/checkout correlation differs");
            const auto generated = generate_recipe_patch(*edit);
            const auto* patch = std::get_if<GeneratedRecipePatch>(&generated);
            require(patch && patch->identity() == edit->identity() &&
                        !validate_local_recipe_patch(patch->bytes()) &&
                        read_bytes(checkout.canonical_path() / "PKGBUILD") == edit->accepted_pkgbuild(),
                    "Slice 1 frozen correlation did not produce a verified patch or original recipe changed");
            // Separate save consent is tested with the exact production-minted
            // correlation, real Git producer, material owner and registry.
            const auto destination = tree.path() / "patches";
            fs::create_directory(destination);
            const auto config_home = tree.path() / "config";
            fs::create_directory(config_home);
            require(setenv("XDG_CONFIG_HOME", config_home.c_str(), 1) == 0, "Cannot isolate patch registry");
            auto save_config = reviewed_config(editor);
            save_config.command_start_directory = tree.path();
            unsigned generated_calls = 0;
            unsigned material_calls = 0;
            set_recipe_patch_diff_process_for_test([&](const auto& invocation, const auto& policy) {
                ++generated_calls;
                return capture_bounded_explicit_process_output_raw(invocation, policy);
            });
            set_patch_association_test_hook([&](auto, const auto&) { ++material_calls; });
            auto declined_config = save_config;
            declined_config.command_start_directory.clear();
            for(const std::string response : {"n\n", "\n"}) {
                ReviewAnswers declined(response);
                save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, declined_config, nullptr);
            }
            auto automatic = declined_config;
            automatic.no_confirm = true;
            {
                ReviewAnswers piped_yes("yes\n");
                save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, automatic, nullptr);
            }
            const int saved_stdin = ::dup(STDIN_FILENO);
            const int null_input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            require(saved_stdin >= 0 && null_input >= 0 && ::dup2(null_input, STDIN_FILENO) >= 0, "Cannot make stdin noninteractive");
            {
                ReviewAnswers piped_yes("yes\n");
                save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, declined_config, nullptr);
            }
            require(::dup2(saved_stdin, STDIN_FILENO) >= 0, "Cannot restore stdin");
            ::close(null_input);
            ::close(saved_stdin);
            require(generated_calls == 0 && material_calls == 0 && fs::is_empty(destination) &&
                        !fs::exists(config_home / "moguet"),
                    "Save No/automatic performed generation or persistence I/O");
            for(const std::string response : {"q\n", ""}) {
                bool stopped = false;
                try {
                    ReviewAnswers canceled(response);
                    save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, save_config, nullptr);
                } catch(const ConfirmationOperationStopped& error) {
                    stopped = std::holds_alternative<ConfirmationCancelled>(error.result());
                }
                require(stopped, "Save cancel/EOF was flattened to No");
            }
            {
                bool stopped = false;
                try {
                    ReviewAnswers broken("yes\n");
                    std::cin.setstate(std::ios::badbit);
                    save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, save_config, nullptr);
                } catch(const ConfirmationOperationStopped& error) {
                    stopped = std::holds_alternative<ConfirmationInputFailure>(error.result());
                }
                require(stopped && generated_calls == 0, "Save input failure became implicit consent or cancellation");
            }
            {
                bool failed = false;
                try {
                    ReviewAnswers empty("y\n\n");
                    save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, save_config, nullptr);
                } catch(const RecipePatchSaveError& error) {
                    failed = error.failure().stage == RecipePatchSaveStage::Destination;
                }
                require(failed && generated_calls == 0, "Empty destination used a default or generated patch");
            }
            {
                bool failed = false;
                try {
                    ReviewAnswers explicit_yes("y\n");
                    save_review_recipe_edit(nullptr, true, request(), checkout, cache_root, save_config, nullptr);
                } catch(const RecipePatchSaveError& error) {
                    failed = error.failure().stage == RecipePatchSaveStage::UnsupportedDevel;
                }
                require(failed && generated_calls == 0, "Authoritative devel save was not an unsupported hard stop");
            }
            for(const bool reproduce : {false, true}) {
                set_recipe_patch_diff_process_for_test([&](const auto&, const auto&) {
                    ++generated_calls;
                    return BoundedCapturedProcessResult{
                        reproduce ? "diff --git a/PKGBUILD b/PKGBUILD\n--- a/PKGBUILD\n+++ b/PKGBUILD\n@@ -1,3 +1,3 @@\n pkgname=reviewed-production-fixture\n-pkgver=2\n+pkgver=999\n pkgrel=1\n" : "", BoundedProcessExited{reproduce ? 1 : 75}, std::nullopt};
                });
                bool failed = false;
                try {
                    ReviewAnswers explicit_yes("y\npatches\n");
                    save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, save_config, nullptr);
                } catch(const RecipePatchSaveError& error) {
                    failed = error.failure().stage == RecipePatchSaveStage::Generation && error.failure().generation &&
                             error.failure().generation->reason == (reproduce ? RecipePatchGenerationFailureReason::ReproductionMismatch : RecipePatchGenerationFailureReason::DiffToolFailure);
                }
                require(failed && fs::is_empty(destination) && material_calls == 0, "Yes generation/reproduction failure reached persistence or continued");
            }
            set_recipe_patch_diff_process_for_test({});
            set_patch_association_test_hook({});
            // Minimal evaluator fixture: makepkg remains an external process;
            // dependency/build/install are outside this identity-only seam.
            const auto bin = tree.path() / "bin";
            fs::create_directory(bin);
            write_file(bin / "makepkg", "#!/bin/sh\nprintf 'pkgbase = reviewed-production-fixture\\n\\tpkgver = 2\\n\\tpkgrel = 1\\n\\tarch = any\\npkgname = reviewed-production-fixture\\n'\n", fs::perms::owner_all);
            const std::string original_path = std::getenv("PATH");
            require(setenv("PATH", (bin.string() + ":" + original_path).c_str(), 1) == 0, "Cannot configure evaluator fixture");
            fs::path retained;
            set_patch_association_test_hook([&](PatchAssociationTestPoint point, const fs::path& path) {
                if(point == PatchAssociationTestPoint::AfterMaterialVerification) retained = path;
            });
            fail_patch_association_operation_for_test(PatchAssociationTestPoint::BeforeWrite);
            {
                bool failed = false;
                try {
                    ReviewAnswers explicit_yes("yes\npatches\ny\n");
                    save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, save_config, nullptr);
                } catch(const RecipePatchSaveError& error) {
                    failed = error.failure().stage == RecipePatchSaveStage::Registration && error.failure().association &&
                             error.failure().retained_material == retained;
                }
                require(failed && fs::exists(retained) && read_bytes(retained) == patch->bytes() &&
                            std::holds_alternative<PatchAssociationAbsent>(read_patch_association(aur_identity())),
                        "Registration failure flattened success or removed published material");
            }
            // Never retry the same failed publication. The next explicit
            // operation chooses another directory and saves through the route.
            fs::create_directory(tree.path() / "registered-patches");
            {
                ReviewAnswers explicit_yes("y\nregistered-patches\ny\n");
                save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, save_config, nullptr);
            }
            const auto saved = read_patch_association(aur_identity());
            const auto* association = std::get_if<LoadedPatchAssociation>(&saved);
            require(association && association->entries().size() == 1 &&
                        association->entries().front().sha256 == xdg_generation_store_raw_contents_sha256(patch->bytes()) &&
                        read_bytes(association->material_root() / association->entries().front().file) == patch->bytes() &&
                        read_bytes(checkout.path() / "PKGBUILD") == edit->accepted_pkgbuild(),
                    "Successful save changed authority or accepted build bytes");
            require(std::holds_alternative<PatchAssociationForgotten>(forget_patch_association(*association)), "Cannot forget fixture association");
            fs::create_directory(tree.path() / "complete-patches");
            set_patch_association_test_hook([&](PatchAssociationTestPoint point, const fs::path&) {
                if(point == PatchAssociationTestPoint::AfterPublication)
                    write_file(checkout.path() / "PKGBUILD", edit->accepted_pkgbuild() + "# after registry commit\n");
            });
            bool continuation_stopped = false;
            try {
                ReviewAnswers explicit_yes("y\ncomplete-patches\ny\n");
                save_review_recipe_edit(&*edit, false, request(), checkout, cache_root, save_config, nullptr);
            } catch(const RecipePatchSaveError& error) {
                continuation_stopped = error.failure().registration_completed &&
                                       fs::exists(error.failure().retained_material) && error.failure().stage == RecipePatchSaveStage::IdentityValidation;
            }
            set_patch_association_test_hook({});
            const auto complete_record = read_patch_association(aur_identity());
            require(continuation_stopped && std::holds_alternative<LoadedPatchAssociation>(complete_record),
                    "Postregistration recipe drift lost both commit facts or continued build");
            require(std::holds_alternative<PatchAssociationForgotten>(forget_patch_association(std::get<LoadedPatchAssociation>(complete_record))), "Cannot forget complete fixture association");
            write_file(checkout.path() / "PKGBUILD", edit->accepted_pkgbuild());
            require(setenv("PATH", original_path.c_str(), 1) == 0, "Cannot restore fixture PATH");
            set_patch_association_test_hook({});
            const ProductionSourceBuildProvenance& provenance =
                prepared_source_build_provenance_for_test(*prepared);
            require(
                provenance.review_status ==
                        ProductionSourceReviewStatus::Reviewed &&
                    provenance.editor_overlay ==
                        ReviewedSourceEditorOverlayStatus::
                            InvocationLocal &&
                    provenance.reviewed_upstream_base_revision ==
                        std::optional<SourceRevisionIdentity>{
                            target} &&
                    provenance.publication_status ==
                        std::optional<
                            ReviewedSourcePublicationStatus>{
                            ReviewedSourcePublicationStatus::
                                Published} &&
                    provenance.reviewed_outcome ==
                        std::optional<
                            ProductionReviewedSourceOutcome>{
                            ProductionReviewedSourceOutcome::
                                InitialFullReview} &&
                    provenance.reviewed_state_generation ==
                        std::optional<std::uint64_t>{1},
                "Accepted review/editor provenance was flattened");
            require_loaded_state(target, 1);
            require(capture(
                        "/usr/bin/git -C " +
                        shell_quote(checkout.path()) +
                        " rev-parse HEAD") == target_oid,
                    "Production checkout did not retain the exact target OID");
            require(capture(
                        "/usr/bin/git -C " +
                        shell_quote(checkout.path()) +
                        " symbolic-ref -q HEAD || true")
                        .empty(),
                    "Production reviewed checkout is not detached");
            require(capture(
                        "/usr/bin/git -C " +
                        shell_quote(checkout.path()) +
                        " status --porcelain --untracked-files=all")
                            .find("PKGBUILD") != std::string::npos,
                    "Editor overlay did not remain invocation-local build input");
            bool contended = false;
            try {
                static_cast<void>(
                    acquire_reviewed_source_package_base_lease(
                        retain_trusted_cache_directory(
                            checkout)));
            } catch(const std::system_error& error) {
                contended = error.code().value() == EWOULDBLOCK ||
                            error.code().value() == EAGAIN;
            }
            require(contended,
                    "Prepared reviewed build did not retain its PackageBase lease");
        }

        {
            AppConfig already_config = reviewed_config(editor);
            already_config.user_config.review.pkgbuild = ReviewPolicy::Skip;
            SourceBuildPreparationOutcome already_outcome =
                prepare_source_build_for_execution(
                    request(), std::string(PACKAGE_BASE),
                    SourceBuildUpdatePolicy::AlwaysBuild,
                    cache_root, already_config);
            auto* already = std::get_if<PreparedSourceBuildNeedsBuild>(
                &already_outcome);
            require(already != nullptr,
                    "Already-reviewed production route did not prepare a build");
            const ProductionSourceBuildProvenance& already_provenance =
                prepared_source_build_provenance_for_test(*already);
            require(
                already_provenance.review_status ==
                        ProductionSourceReviewStatus::Reviewed &&
                    already_provenance.editor_overlay ==
                        ReviewedSourceEditorOverlayStatus::None &&
                    already_provenance.publication_status ==
                        std::optional<
                            ReviewedSourcePublicationStatus>{
                            ReviewedSourcePublicationStatus::
                                AlreadyPublishedSameTarget} &&
                    already_provenance.reviewed_outcome ==
                        std::optional<
                            ProductionReviewedSourceOutcome>{
                            ProductionReviewedSourceOutcome::
                                AlreadyReviewed} &&
                    already_provenance.reviewed_state_generation ==
                        std::optional<std::uint64_t>{1},
                "Already-reviewed production provenance differs");
            require_loaded_state(target, 1);
            require(capture(
                        "/usr/bin/git -C " +
                        shell_quote(checkout.path()) +
                        " status --porcelain=v2 -z --untracked-files=all --ignored=matching")
                        .empty(),
                    "Already-reviewed exact checkout retained editor residue");
        }

        {
            AppConfig late_failure_config = reviewed_config(editor);
            late_failure_config.user_config.review.pkgbuild =
                ReviewPolicy::Skip;
            reviewed_source_production_execution_stub::
                fail_next_install_transaction();
            try {
                static_cast<void>(execute_source_build_typed(
                    request(), cache_root,
                    DesiredInstallReason::Explicit,
                    PacmanDatabasePaths{"/", "/var/lib/pacman"},
                    late_failure_config));
                throw std::runtime_error(
                    "Reviewed pacman failure completed successfully");
            } catch(const SeparatedSourceBuildPhaseError& error) {
                require(
                    error.phase() ==
                            SeparatedSourceBuildFailurePhase::
                                InstallTransaction &&
                        error.production_outcome().source_provenance.review_status ==
                            ProductionSourceReviewStatus::
                                Reviewed &&
                        error.production_outcome().source_provenance.reviewed_outcome ==
                            std::optional<
                                ProductionReviewedSourceOutcome>{
                                ProductionReviewedSourceOutcome::
                                    AlreadyReviewed} &&
                        error.production_outcome().build_outcome ==
                            ProductionSourceBuildCommandOutcome::
                                Succeeded &&
                        error.production_outcome().install_outcome ==
                            ProductionSourceInstallOutcome::Failed,
                    "Reviewed pacman failure lost reviewed/build/install outcome");
            }
            require_loaded_state(target, 1);

            reviewed_source_production_execution_stub::
                fail_next_package_metadata();
            try {
                static_cast<void>(execute_source_build_typed(
                    request(), cache_root,
                    DesiredInstallReason::Explicit,
                    PacmanDatabasePaths{"/", "/var/lib/pacman"},
                    late_failure_config));
                throw std::runtime_error(
                    "Reviewed metadata failure completed successfully");
            } catch(const SeparatedSourceBuildPhaseError& error) {
                require(
                    error.phase() ==
                            SeparatedSourceBuildFailurePhase::
                                InstallPreparation &&
                        error.package_metadata_failure().has_value() &&
                        error.package_metadata_failure()->code ==
                            PackageMetadataErrorCode::QueryFailed &&
                        error.production_outcome().source_provenance.review_status ==
                            ProductionSourceReviewStatus::
                                Reviewed &&
                        error.production_outcome().build_outcome ==
                            ProductionSourceBuildCommandOutcome::
                                Succeeded &&
                        error.production_outcome().install_outcome ==
                            ProductionSourceInstallOutcome::Failed,
                    "Reviewed PackageMetadataError lost typed staged outcome");
            }
            require_loaded_state(target, 1);
        }

        write_file(
            work / "PKGBUILD",
            "pkgname=reviewed-production-fixture\npkgver=3\npkgrel=1\n");
        run("/usr/bin/git -C " + shell_quote(work) + " add PKGBUILD");
        run("/usr/bin/git -C " + shell_quote(work) +
            " commit -q -m compatibility-update");
        run("/usr/bin/git -C " + shell_quote(work) + " push -q " +
            shell_quote(remote) + " main");
        {
            AppConfig decline_config = reviewed_config(editor);
            ReviewAnswers answers("n\n");
            decline_config.user_config.review.pkgbuild = ReviewPolicy::Skip;
            SourceBuildPreparationOutcome decline_outcome =
                prepare_source_build_for_execution(
                    request(), std::string(PACKAGE_BASE),
                    SourceBuildUpdatePolicy::AlwaysBuild,
                    cache_root, decline_config);
            auto* declined =
                std::get_if<PreparedSourceBuildNeedsBuild>(
                    &decline_outcome);
            require(declined != nullptr,
                    "Explicit review decline did not continue compatibly");
            const ProductionSourceBuildProvenance& decline_provenance =
                prepared_source_build_provenance_for_test(*declined);
            require(
                decline_provenance.review_status ==
                        ProductionSourceReviewStatus::
                            CompatibilityWithoutReview &&
                    decline_provenance.compatibility_reason ==
                        std::optional<
                            ReviewedSourceCompatibilityBuildReason>{
                            ReviewedSourceCompatibilityBuildReason::
                                ExplicitReviewDecline} &&
                    !decline_provenance.publication_status
                         .has_value(),
                "Explicit review decline received reviewed authority");
            require_loaded_state(target, 1);
        }

        {
            AppConfig compatibility_config = reviewed_config(editor);
            compatibility_config.user_config.review.diff = ReviewPolicy::Skip;
            compatibility_config.user_config.review.pkgbuild =
                ReviewPolicy::Skip;
            SourceBuildPreparationOutcome compatibility_outcome =
                prepare_source_build_for_execution(
                    request(), std::string(PACKAGE_BASE),
                    SourceBuildUpdatePolicy::AlwaysBuild,
                    cache_root, compatibility_config);
            auto* compatibility =
                std::get_if<PreparedSourceBuildNeedsBuild>(
                    &compatibility_outcome);
            require(compatibility != nullptr,
                    "No-diff compatibility route did not prepare a build");
            require(!compatibility->accepted_recipe_edit(), "Compatibility route captured a recipe edit");
            const ProductionSourceBuildProvenance& compatibility_provenance =
                prepared_source_build_provenance_for_test(*compatibility);
            require(
                compatibility_provenance.review_status ==
                        ProductionSourceReviewStatus::
                            CompatibilityWithoutReview &&
                    compatibility_provenance.compatibility_reason ==
                        std::optional<
                            ReviewedSourceCompatibilityBuildReason>{
                            ReviewedSourceCompatibilityBuildReason::
                                NoDiff} &&
                    !compatibility_provenance
                         .reviewed_upstream_base_revision
                         .has_value() &&
                    !compatibility_provenance.publication_status
                         .has_value(),
                "No-diff compatibility route received reviewed authority");
            require_loaded_state(target, 1);
        }

        // Each interactive branch now owns its script; the remaining review
        // fault matrix only accepts upstream/no-op edit, never persistent save.
        ReviewAnswers fault_matrix_answers("y\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\ny\n");
        const auto publish_upstream_version =
            [&](int version, const std::string& message) {
                write_file(
                    work / "PKGBUILD",
                    "pkgname=reviewed-production-fixture\npkgver=" +
                        std::to_string(version) + "\npkgrel=1\n");
                run("/usr/bin/git -C " + shell_quote(work) +
                    " add PKGBUILD");
                run("/usr/bin/git -C " + shell_quote(work) +
                    " commit -q -m " + message);
                run("/usr/bin/git -C " + shell_quote(work) + " push -q " +
                    shell_quote(remote) + " main");
                const std::string oid = capture(
                    "/usr/bin/git -C " + shell_quote(work) +
                    " rev-parse HEAD");
                return SourceRevisionIdentity::git_commit(oid);
            };

        AppConfig failure_config = reviewed_config(editor);
        failure_config.user_config.review.pkgbuild = ReviewPolicy::Skip;

        const SourceRevisionIdentity checkout_failure_target =
            publish_upstream_version(4, "checkout-failure");
        require(setenv(
                    "MOGUET_TEST_FAIL_PINNED_CHECKOUT", "1", 1) == 0,
                "Failed to arm exact checkout failure");
        bool exact_checkout_failed = false;
        try {
            static_cast<void>(prepare_source_build_for_execution(
                request(), std::string(PACKAGE_BASE),
                SourceBuildUpdatePolicy::AlwaysBuild,
                cache_root, failure_config));
        } catch(const ReviewedSourceProductionError& error) {
            exact_checkout_failed =
                error.failure().reason ==
                    ReviewedSourceProductionFailureReason::
                        ExactCheckoutFailure &&
                std::holds_alternative<
                    ReviewedSourcePinnedCheckoutFailure>(
                    error.failure().detail);
        }
        require(unsetenv("MOGUET_TEST_FAIL_PINNED_CHECKOUT") == 0,
                "Failed to clear exact checkout failure");
        require(exact_checkout_failed,
                "Exact checkout failure was flattened in production");
        require_loaded_state(target, 1);

        const SourceRevisionIdentity conflict_target =
            publish_upstream_version(5, "publication-conflict");
        ReviewedSourceStateStoreReadResult before_conflict =
            read_reviewed_source_state(aur_identity());
        const auto* before_conflict_read =
            std::get_if<ReviewedSourceStateStoreRead>(
                &before_conflict);
        require(before_conflict_read != nullptr &&
                    before_conflict_read->observed.has_value(),
                "CAS conflict setup did not observe current state");
        g_concurrent_expected_record = before_conflict_read->observed;
        g_concurrent_reviewed_state = ReviewedSourceState::make(
            aur_identity(), checkout_failure_target);
        g_concurrent_publication_succeeded = false;
        set_reviewed_source_before_publication_hook_for_test(
            publish_concurrent_reviewed_state);
        bool publication_conflicted = false;
        try {
            static_cast<void>(prepare_source_build_for_execution(
                request(), std::string(PACKAGE_BASE),
                SourceBuildUpdatePolicy::AlwaysBuild,
                cache_root, failure_config));
        } catch(const ReviewedSourceProductionError& error) {
            publication_conflicted =
                error.failure().reason ==
                    ReviewedSourceProductionFailureReason::
                        PublicationConflict &&
                std::holds_alternative<
                    ReviewedSourcePublicationConflict>(
                    error.failure().detail);
        }
        require(publication_conflicted &&
                    g_concurrent_publication_succeeded,
                "CAS conflict payload was flattened in production");
        require_loaded_state(checkout_failure_target, 2);
        set_reviewed_source_before_publication_hook_for_test(nullptr);
        reset_reviewed_source_state_store_test_hooks();
        g_concurrent_reviewed_state.reset();
        g_concurrent_expected_record.reset();

        const SourceRevisionIdentity uncertain_target =
            publish_upstream_version(6, "publication-uncertain");
        fail_next_reviewed_source_state_store_operation_for_test(
            ReviewedSourceStateStoreTestFailurePoint::PostCommitVerify);
        bool publication_uncertain = false;
        try {
            static_cast<void>(prepare_source_build_for_execution(
                request(), std::string(PACKAGE_BASE),
                SourceBuildUpdatePolicy::AlwaysBuild,
                cache_root, failure_config));
        } catch(const ReviewedSourceProductionError& error) {
            publication_uncertain =
                error.failure().reason ==
                    ReviewedSourceProductionFailureReason::
                        PublishedUncertain &&
                std::holds_alternative<
                    ReviewedSourcePublicationUncertain>(
                    error.failure().detail) &&
                (!error.production_outcome().has_value() ||
                 error.production_outcome()->build_outcome !=
                     ProductionSourceBuildCommandOutcome::Succeeded);
        }
        require(publication_uncertain,
                "PublishedUncertain was flattened in production");
        require_loaded_state(uncertain_target, 3);
        reset_reviewed_source_state_store_test_hooks();

        const SourceRevisionIdentity post_publication_target =
            publish_upstream_version(7, "post-publication-drift");
        g_post_publication_mutation_checkout = checkout.path();
        run_reviewed_source_state_store_race_once_for_test(
            ReviewedSourceStateStoreTestRacePoint::AfterPublication,
            mutate_checkout_after_publication);
        bool post_publication_failed = false;
        try {
            static_cast<void>(prepare_source_build_for_execution(
                request(), std::string(PACKAGE_BASE),
                SourceBuildUpdatePolicy::AlwaysBuild,
                cache_root, failure_config));
        } catch(const ReviewedSourceProductionError& error) {
            post_publication_failed =
                error.failure().reason ==
                    ReviewedSourceProductionFailureReason::
                        PostPublicationCheckoutFailure &&
                std::holds_alternative<
                    ReviewedSourcePostPublicationCheckoutFailure>(
                    error.failure().detail);
        }
        require(post_publication_failed,
                "Post-publication checkout failure was flattened");
        require_loaded_state(post_publication_target, 4);
        reset_reviewed_source_state_store_test_hooks();
        g_post_publication_mutation_checkout.clear();

        const SourceRevisionIdentity no_op_editor_target =
            publish_upstream_version(8, "no-op-editor");
        write_file(
            editor,
            "#!/bin/sh\n"
            "set -eu\n"
            "exit 0\n",
            fs::perms::owner_read | fs::perms::owner_write |
                fs::perms::owner_exec);
        {
            SourceBuildPreparationOutcome no_op_outcome =
                prepare_source_build_for_execution(
                    request(), std::string(PACKAGE_BASE),
                    SourceBuildUpdatePolicy::AlwaysBuild,
                    cache_root, reviewed_config(editor));
            auto* no_op = std::get_if<PreparedSourceBuildNeedsBuild>(
                &no_op_outcome);
            require(no_op != nullptr,
                    "No-op editor did not prepare reviewed build");
            require(!no_op->accepted_recipe_edit(), "No-op editor minted an accepted PKGBUILD edit");
            const ProductionSourceBuildProvenance& provenance =
                prepared_source_build_provenance_for_test(*no_op);
            require(provenance.review_status ==
                            ProductionSourceReviewStatus::Reviewed &&
                        provenance.editor_overlay ==
                            ReviewedSourceEditorOverlayStatus::None &&
                        provenance.reviewed_outcome ==
                            std::optional<
                                ProductionReviewedSourceOutcome>{
                                ProductionReviewedSourceOutcome::
                                    UpdateReview} &&
                        provenance.reviewed_state_generation ==
                            std::optional<std::uint64_t>{5},
                    "No-op editor was promoted to a dirty overlay");
            require_loaded_state(no_op_editor_target, 5);
        }
        static_cast<void>(conflict_target);

        // Continue in the same small real-Git fixture. Each preparation owns a
        // fresh local pre/post pair; scripted stdin changes only prompt answers.
        write_file(work / "fixture.install", "# install script\n");
        run("/usr/bin/git -C " + shell_quote(work) + " add fixture.install");
        const auto install_target = publish_upstream_version(9, "install-only");
        write_file(editor, "#!/bin/sh\nfor target do :; done\nprintf '# install edit\\n' >>\"$target\"\n",
                   fs::perms::owner_all);
        {
            ReviewAnswers answers("y\nn\ny\ny\nn\n");
            auto outcome = prepare_source_build_for_execution(request(), std::string(PACKAGE_BASE),
                                                              SourceBuildUpdatePolicy::AlwaysBuild, cache_root, reviewed_config(editor));
            auto& prepared = std::get<PreparedSourceBuildNeedsBuild>(outcome);
            const auto& edit = prepared.accepted_recipe_edit();
            require(edit && edit->baseline_recipe().size() == 2 &&
                        edit->accepted_recipe().size() == 2 &&
                        edit->baseline_pkgbuild() == edit->accepted_pkgbuild() &&
                        edit->baseline_recipe()[1].relative_path == "fixture.install" &&
                        edit->baseline_recipe()[1].file.contents == "# install script\n" &&
                        edit->accepted_recipe()[1].file.contents == "# install script\n# install edit\n",
                    ".install-only edit did not mint exact supported recipe correlation");
            require(read_bytes(checkout.path() / "fixture.install") == "# install script\n# install edit\n",
                    ".install-only editor did not actually edit the script");
            require_loaded_state(install_target, 6);
        }

        const auto atomic_target = publish_upstream_version(10, "atomic-editor");
        // Include CRLF and a missing final newline to prove raw byte ownership.
        write_file(work / "PKGBUILD", read_bytes(work / "PKGBUILD") + "# exact baseline\r\n# no final newline");
        run("/usr/bin/git -C " + shell_quote(work) + " add PKGBUILD");
        run("/usr/bin/git -C " + shell_quote(work) + " commit -q -m exact-bytes");
        run("/usr/bin/git -C " + shell_quote(work) + " push -q " + shell_quote(remote) + " main");
        const auto exact_target = SourceRevisionIdentity::git_commit(capture(
            "/usr/bin/git -C " + shell_quote(work) + " rev-parse HEAD"));
        const std::string exact_baseline = read_bytes(work / "PKGBUILD");
        write_file(editor,
                   "#!/bin/sh\nfor target do :; done\ncat \"$target\" >\"$target.new\"\n"
                   "printf '\\n# atomic edit\\r\\n' >>\"$target.new\"\nmv \"$target.new\" \"$target\"\n",
                   fs::perms::owner_all);
        {
            ReviewAnswers answers("y\ny\nn\ny\nn\n");
            auto outcome = prepare_source_build_for_execution(request(), std::string(PACKAGE_BASE),
                                                              SourceBuildUpdatePolicy::AlwaysBuild, cache_root, reviewed_config(editor));
            const auto& edit = std::get<PreparedSourceBuildNeedsBuild>(outcome).accepted_recipe_edit();
            require(edit && edit->baseline_pkgbuild() == exact_baseline &&
                        edit->accepted_pkgbuild() == exact_baseline + "\n# atomic edit\r\n" &&
                        edit->identity().target_revision() == exact_target,
                    "Atomic replace lost exact bytes or reused a previous revision baseline");
            require_loaded_state(exact_target, 7);
            write_file(checkout.path() / "PKGBUILD", "# later filesystem contents\n");
            require(edit->baseline_pkgbuild() == exact_baseline &&
                        edit->accepted_pkgbuild() == exact_baseline + "\n# atomic edit\r\n",
                    "Correlation did not own frozen pre/post bytes");
            const auto generated = generate_recipe_patch(*edit);
            require(std::holds_alternative<GeneratedRecipePatch>(generated) &&
                        std::get<GeneratedRecipePatch>(generated).identity() == edit->identity() &&
                        read_bytes(checkout.path() / "PKGBUILD") == "# later filesystem contents\n",
                    "Generator reopened mutable checkout instead of using frozen recipe bytes");
        }
        static_cast<void>(atomic_target);

        for(const bool targetless : {false, true}) {
            SourceBuildRequest auto_sync_request = request();
            auto_sync_request.suppress_review_recipe_edit_capture = !targetless;
            auto_sync_request.ordinary_devel_package_base = targetless;
            g_recipe_fault_invoked = false;
            set_reviewed_recipe_after_snapshot_hook_for_test(mutate_after_recipe_snapshot);
            ReviewAnswers answers("y\nn\ny\n");
            auto outcome = prepare_source_build_for_execution(auto_sync_request, std::string(PACKAGE_BASE),
                                                              SourceBuildUpdatePolicy::AlwaysBuild, cache_root, reviewed_config(editor));
            const auto& prepared = std::get<PreparedSourceBuildNeedsBuild>(outcome);
            require(!prepared.accepted_recipe_edit() && !g_recipe_fault_invoked &&
                        read_bytes(checkout.path() / "PKGBUILD") == exact_baseline + "\n# atomic edit\r\n",
                    "Auto sync/targetless exclusion changed accepted editor behavior or captured bytes");
            set_reviewed_recipe_after_snapshot_hook_for_test(nullptr);
        }

        for(const std::string response : {"n\n", "q\n", ""}) {
            bool stopped = false;
            try {
                ReviewAnswers answers("y\nn\n" + response);
                static_cast<void>(prepare_source_build_for_execution(request(), std::string(PACKAGE_BASE),
                                                                     SourceBuildUpdatePolicy::AlwaysBuild, cache_root, reviewed_config(editor)));
            } catch(const ConfirmationOperationStopped&) {
                stopped = true;
            }
            require(stopped, "Proceed decline/cancel/EOF returned accepted recipe authority");
            require_loaded_state(exact_target, 7);
        }
        write_file(editor, "#!/bin/sh\nexit 75\n", fs::perms::owner_all);
        {
            bool stopped = false;
            try {
                ReviewAnswers answers("y\n");
                static_cast<void>(prepare_source_build_for_execution(request(), std::string(PACKAGE_BASE),
                                                                     SourceBuildUpdatePolicy::AlwaysBuild, cache_root, reviewed_config(editor)));
            } catch(const std::exception& error) {
                stopped = std::string(error.what()).find("Editor failed.") != std::string::npos;
            }
            require(stopped, "Failed editor returned accepted recipe authority");
            require_loaded_state(exact_target, 7);
        }

        write_file(editor, "#!/bin/sh\nfor target do :; done\nprintf '\\n# accepted edit\\n' >>\"$target\"\n",
                   fs::perms::owner_all);
        const auto expect_recipe_fault = [&](const std::string& command,
                                             std::optional<PackageBaseIdentity> mismatch = std::nullopt) {
            SourceBuildRequest current = request();
            g_recipe_fault_command = command;
            g_recipe_fault_request = mismatch ? &current : nullptr;
            g_recipe_fault_identity = std::move(mismatch);
            g_recipe_fault_invoked = false;
            set_reviewed_recipe_after_snapshot_hook_for_test(mutate_after_recipe_snapshot);
            bool stopped = false;
            try {
                ReviewAnswers answers("y\nn\ny\nn\n");
                static_cast<void>(prepare_source_build_for_execution(current, std::string(PACKAGE_BASE),
                                                                     SourceBuildUpdatePolicy::AlwaysBuild, cache_root, reviewed_config(editor)));
            } catch(const ReviewedSourceProductionError&) {
                stopped = true;
            } catch(const TrustedCacheError&) {
                stopped = true;
            }
            set_reviewed_recipe_after_snapshot_hook_for_test(nullptr);
            g_recipe_fault_request = nullptr;
            require(g_recipe_fault_invoked && stopped, "Post-snapshot drift/mismatch was not exercised or returned accepted recipe authority");
            require_loaded_state(exact_target, 7);
        };
        expect_recipe_fault("printf '\\n# late mutation\\n' >>" + shell_quote(checkout.path() / "PKGBUILD"));
        expect_recipe_fault("/usr/bin/git -C " + shell_quote(checkout.path()) + " checkout -q --force HEAD^");
        expect_recipe_fault({}, PackageBaseIdentity::make(aur_identity().source(), "different-base"));
        expect_recipe_fault({}, PackageBaseIdentity::make(PackageSourceIdentity::aur(
                                                              SourceLocationIdentity::known_git_remote("https://aur.archlinux.org/different-source.git")),
                                                          std::string(PACKAGE_BASE)));
        expect_recipe_fault("/usr/bin/git -C " + shell_quote(checkout.path()) +
                            " remote set-url origin https://aur.archlinux.org/different-source.git");
        run("/usr/bin/git -C " + shell_quote(checkout.path()) + " remote set-url origin " + std::string(CANONICAL_REMOTE));
        expect_recipe_fault("mv " + shell_quote(checkout.path() / "PKGBUILD") + " " + shell_quote(tree.path() / "unsafe-recipe") +
                            " && ln -s " + shell_quote(tree.path() / "unsafe-recipe") + " " + shell_quote(checkout.path() / "PKGBUILD"));
        fs::remove(checkout.path() / "PKGBUILD");
        fs::rename(tree.path() / "unsafe-recipe", checkout.path() / "PKGBUILD");
        // Named root replacement is rejected even if the contents are copied.
        expect_recipe_fault("mv " + shell_quote(checkout.path()) + " " + shell_quote(tree.path() / "old-checkout") +
                            " && cp -a " + shell_quote(tree.path() / "old-checkout") + " " + shell_quote(checkout.path()));

        std::cout << "reviewed source production connection tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        set_reviewed_source_before_publication_hook_for_test(nullptr);
        set_reviewed_recipe_after_snapshot_hook_for_test(nullptr);
        reset_reviewed_source_state_store_test_hooks();
        std::cerr << "reviewed source production connection test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
