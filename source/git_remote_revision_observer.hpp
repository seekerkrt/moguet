#pragma once

#include "devel_package_assessment_authority.hpp"
#include "process.hpp"
#include "vcs_source_identity.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

inline constexpr std::size_t
    VALIDATED_HTTPS_GIT_REMOTE_MAX_INPUT_BYTES = 8U * 1024U;
inline constexpr std::size_t
    VALIDATED_EXACT_GIT_BRANCH_MAX_INPUT_BYTES = 4U * 1024U;
inline constexpr std::chrono::seconds
    GIT_REMOTE_OBSERVER_PROCESS_HARD_TIMEOUT{30};
inline constexpr std::chrono::milliseconds
    GIT_REMOTE_OBSERVER_PROCESS_TERMINATION_GRACE{500};
inline constexpr std::size_t
    GIT_REMOTE_OBSERVER_STDOUT_CAPTURE_LIMIT = 16U * 1024U;

namespace git_remote_revision_observer_detail {
class ExactBranchFactory;
class ObservationResultFactory;
} // namespace git_remote_revision_observer_detail

// This capability means that a separate authority owner has already approved
// one effective Git source identity for remote observation. Syntax parsing,
// generic VCS classification, and source suffix evidence cannot mint it.
class AuthorityApprovedGitSourceIdentity final {
public:
    AuthorityApprovedGitSourceIdentity() = delete;
    AuthorityApprovedGitSourceIdentity(
        const AuthorityApprovedGitSourceIdentity&) = default;
    AuthorityApprovedGitSourceIdentity(
        AuthorityApprovedGitSourceIdentity&&) noexcept = default;
    AuthorityApprovedGitSourceIdentity& operator=(
        const AuthorityApprovedGitSourceIdentity&) = default;
    AuthorityApprovedGitSourceIdentity& operator=(
        AuthorityApprovedGitSourceIdentity&&) noexcept = default;
    ~AuthorityApprovedGitSourceIdentity() = default;

    [[nodiscard]] const VcsSourceIdentity& source() const noexcept;

    bool operator==(
        const AuthorityApprovedGitSourceIdentity&) const = default;

private:
    friend class DevelPackageAssessmentAuthority;
#ifdef MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS
    friend AuthorityApprovedGitSourceIdentity
    make_authority_approved_git_source_identity_fixture_for_test(
        VcsSourceIdentity source);
#endif

    explicit AuthorityApprovedGitSourceIdentity(
        VcsSourceIdentity source);

    VcsSourceIdentity source_;
};

// Stores only the library-backed canonical URL. The caller-provided spelling
// is never retained as a network argument sidecar.
class ValidatedHttpsGitRemote final {
public:
    ValidatedHttpsGitRemote() = delete;
    ValidatedHttpsGitRemote(const ValidatedHttpsGitRemote&) = default;
    ValidatedHttpsGitRemote(ValidatedHttpsGitRemote&&) noexcept = default;
    ValidatedHttpsGitRemote& operator=(
        const ValidatedHttpsGitRemote&) = default;
    ValidatedHttpsGitRemote& operator=(
        ValidatedHttpsGitRemote&&) noexcept = default;
    ~ValidatedHttpsGitRemote() = default;

    [[nodiscard]] static ValidatedHttpsGitRemote make(
        std::string_view remote);

    [[nodiscard]] const std::string& canonical_url() const noexcept;

    bool operator==(const ValidatedHttpsGitRemote&) const = default;

private:
    explicit ValidatedHttpsGitRemote(std::string canonical_url) noexcept;

    std::string canonical_url_;
};

// Raw spelling reaches this private construction boundary only through the
// isolated `git check-ref-format --branch` producer below.
class ValidatedExactGitBranch final {
public:
    ValidatedExactGitBranch() = delete;
    ValidatedExactGitBranch(const ValidatedExactGitBranch&) = default;
    ValidatedExactGitBranch(ValidatedExactGitBranch&&) noexcept = default;
    ValidatedExactGitBranch& operator=(
        const ValidatedExactGitBranch&) = default;
    ValidatedExactGitBranch& operator=(
        ValidatedExactGitBranch&&) noexcept = default;
    ~ValidatedExactGitBranch() = default;

    [[nodiscard]] const std::string& name() const noexcept;

    bool operator==(const ValidatedExactGitBranch&) const = default;

private:
    friend class git_remote_revision_observer_detail::ExactBranchFactory;
#ifdef MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS
    friend ValidatedExactGitBranch
    make_validated_exact_git_branch_fixture_for_test(
        std::string branch_name);
#endif

    explicit ValidatedExactGitBranch(std::string name) noexcept;

    std::string name_;
};

enum class InvalidExactGitBranchReason {
    Empty,
    EmbeddedNul,
    InputTooLong,
    GitRejected,
};

struct InvalidExactGitBranch {
    InvalidExactGitBranchReason reason;
    std::optional<int> git_exit_code;

    bool operator==(const InvalidExactGitBranch&) const = default;
};

enum class ExactGitBranchValidationProcessFailureReason {
    LaunchOrSetup,
    IoOrWait,
    TimedOut,
    CaptureLimitExceeded,
    Signaled,
    UnexpectedOutput,
};

struct ExactGitBranchValidationProcessFailure {
    ExactGitBranchValidationProcessFailureReason reason;
    std::optional<int> detail;

    bool operator==(
        const ExactGitBranchValidationProcessFailure&) const = default;
};

using ExactGitBranchValidationResult = std::variant<
    ValidatedExactGitBranch,
    InvalidExactGitBranch,
    ExactGitBranchValidationProcessFailure>;

// Git owns refname grammar. This producer performs only the empty/NUL/size
// resource preflight, then delegates to fixed /usr/bin/git under the isolated
// observer policy and accepts only an exact "<input>\n" success transcript.
[[nodiscard]] ExactGitBranchValidationResult validate_exact_git_branch(
    std::string_view branch_name);

enum class ValidatedGitRemoteSelectorKind {
    DefaultHead,
    ExactBranch,
};

class ValidatedGitRemoteSelector final {
public:
    ValidatedGitRemoteSelector() = delete;
    ValidatedGitRemoteSelector(const ValidatedGitRemoteSelector&) = default;
    ValidatedGitRemoteSelector(
        ValidatedGitRemoteSelector&&) noexcept = default;
    ValidatedGitRemoteSelector& operator=(
        const ValidatedGitRemoteSelector&) = default;
    ValidatedGitRemoteSelector& operator=(
        ValidatedGitRemoteSelector&&) noexcept = default;
    ~ValidatedGitRemoteSelector() = default;

    [[nodiscard]] static ValidatedGitRemoteSelector default_head() noexcept;
    [[nodiscard]] static ValidatedGitRemoteSelector exact_branch(
        ValidatedExactGitBranch branch) noexcept;

    [[nodiscard]] ValidatedGitRemoteSelectorKind kind() const noexcept;
    [[nodiscard]] const ValidatedExactGitBranch* exact_branch()
        const noexcept;

    bool operator==(const ValidatedGitRemoteSelector&) const = default;

private:
    ValidatedGitRemoteSelector(
        ValidatedGitRemoteSelectorKind kind,
        std::optional<ValidatedExactGitBranch> exact_branch) noexcept;

    ValidatedGitRemoteSelectorKind kind_;
    std::optional<ValidatedExactGitBranch> exact_branch_;
};

class GitRemoteRevisionObservationKey final {
public:
    GitRemoteRevisionObservationKey() = delete;
    GitRemoteRevisionObservationKey(
        const GitRemoteRevisionObservationKey&) = default;
    GitRemoteRevisionObservationKey(
        GitRemoteRevisionObservationKey&&) noexcept = default;
    GitRemoteRevisionObservationKey& operator=(
        const GitRemoteRevisionObservationKey&) = default;
    GitRemoteRevisionObservationKey& operator=(
        GitRemoteRevisionObservationKey&&) noexcept = default;
    ~GitRemoteRevisionObservationKey() = default;

    [[nodiscard]] static GitRemoteRevisionObservationKey make(
        ValidatedHttpsGitRemote remote,
        ValidatedGitRemoteSelector selector) noexcept;

    [[nodiscard]] const ValidatedHttpsGitRemote& remote() const noexcept;
    [[nodiscard]] const ValidatedGitRemoteSelector& selector()
        const noexcept;

    bool operator==(const GitRemoteRevisionObservationKey&) const = default;

private:
    GitRemoteRevisionObservationKey(
        ValidatedHttpsGitRemote remote,
        ValidatedGitRemoteSelector selector) noexcept;

    ValidatedHttpsGitRemote remote_;
    ValidatedGitRemoteSelector selector_;
};

class ValidatedGitRemoteRevisionRequest final {
public:
    ValidatedGitRemoteRevisionRequest() = delete;
    ValidatedGitRemoteRevisionRequest(
        const ValidatedGitRemoteRevisionRequest&) = default;
    ValidatedGitRemoteRevisionRequest(
        ValidatedGitRemoteRevisionRequest&&) noexcept = default;
    ValidatedGitRemoteRevisionRequest& operator=(
        const ValidatedGitRemoteRevisionRequest&) = default;
    ValidatedGitRemoteRevisionRequest& operator=(
        ValidatedGitRemoteRevisionRequest&&) noexcept = default;
    ~ValidatedGitRemoteRevisionRequest() = default;

    [[nodiscard]] static ValidatedGitRemoteRevisionRequest make(
        AuthorityApprovedGitSourceIdentity source,
        GitRemoteRevisionObservationKey key);

    [[nodiscard]] const AuthorityApprovedGitSourceIdentity& source()
        const noexcept;
    [[nodiscard]] const GitRemoteRevisionObservationKey& key()
        const noexcept;

    bool operator==(const ValidatedGitRemoteRevisionRequest&) const = default;

private:
    ValidatedGitRemoteRevisionRequest(
        AuthorityApprovedGitSourceIdentity source,
        GitRemoteRevisionObservationKey key) noexcept;

    AuthorityApprovedGitSourceIdentity source_;
    GitRemoteRevisionObservationKey key_;
};

class ObservedGitRemoteRevision final {
public:
    ObservedGitRemoteRevision() = delete;
    ObservedGitRemoteRevision(const ObservedGitRemoteRevision&) = default;
    ObservedGitRemoteRevision(ObservedGitRemoteRevision&&) noexcept = default;
    ObservedGitRemoteRevision& operator=(
        const ObservedGitRemoteRevision&) = default;
    ObservedGitRemoteRevision& operator=(
        ObservedGitRemoteRevision&&) noexcept = default;
    ~ObservedGitRemoteRevision() = default;

    [[nodiscard]] const ValidatedGitRemoteRevisionRequest& request()
        const noexcept;
    [[nodiscard]] const UpstreamGitRevision& revision() const noexcept;

    bool operator==(const ObservedGitRemoteRevision&) const = default;

private:
    friend class
        git_remote_revision_observer_detail::ObservationResultFactory;

    ObservedGitRemoteRevision(
        ValidatedGitRemoteRevisionRequest request,
        UpstreamGitRevision revision) noexcept;

    ValidatedGitRemoteRevisionRequest request_;
    UpstreamGitRevision revision_;
};

struct GitRemoteRevisionRefNotFound {
    GitRemoteRevisionObservationKey key;

    bool operator==(const GitRemoteRevisionRefNotFound&) const = default;
};

struct GitRemoteRevisionTimeout {
    GitRemoteRevisionObservationKey key;

    bool operator==(const GitRemoteRevisionTimeout&) const = default;
};

struct GitRemoteRevisionProcessFailure {
    GitRemoteRevisionObservationKey key;
    std::variant<
        BoundedProcessSignaled,
        BoundedProcessLaunchOrSetupFailure,
        BoundedProcessIoOrWaitFailure>
        cause;

    bool operator==(const GitRemoteRevisionProcessFailure&) const = default;
};

struct GitRemoteRevisionGitExitFailure {
    GitRemoteRevisionObservationKey key;
    int exit_code;

    bool operator==(const GitRemoteRevisionGitExitFailure&) const = default;
};

struct GitRemoteRevisionCaptureLimitExceeded {
    GitRemoteRevisionObservationKey key;
    std::size_t capture_limit;

    bool operator==(
        const GitRemoteRevisionCaptureLimitExceeded&) const = default;
};

enum class GitRemoteRevisionMalformedOutputReason {
    EmptyOutput,
    RefNotFoundStatusWithOutput,
    MissingFinalLineFeed,
    UnexpectedControlByte,
    InvalidRecord,
    WrongRef,
    UnexpectedSymref,
    InvalidSymbolicHeadTarget,
    WrongRecordOrder,
};

struct GitRemoteRevisionMalformedOutput {
    GitRemoteRevisionObservationKey key;
    GitRemoteRevisionMalformedOutputReason reason;

    bool operator==(
        const GitRemoteRevisionMalformedOutput&) const = default;
};

enum class GitRemoteRevisionAmbiguousOutputReason {
    DuplicateExpectedRecord,
    ExtraOidRecord,
};

struct GitRemoteRevisionAmbiguousOutput {
    GitRemoteRevisionObservationKey key;
    GitRemoteRevisionAmbiguousOutputReason reason;
    std::size_t record_count;

    bool operator==(
        const GitRemoteRevisionAmbiguousOutput&) const = default;
};

using GitRemoteRevisionObservationResult = std::variant<
    ObservedGitRemoteRevision,
    GitRemoteRevisionRefNotFound,
    GitRemoteRevisionTimeout,
    GitRemoteRevisionProcessFailure,
    GitRemoteRevisionGitExitFailure,
    GitRemoteRevisionCaptureLimitExceeded,
    GitRemoteRevisionMalformedOutput,
    GitRemoteRevisionAmbiguousOutput>;

// Pure mapping for a normally exited Git process. stderr is intentionally not
// an input and raw stdout bytes are never retained in a result diagnostic.
[[nodiscard]] GitRemoteRevisionObservationResult
parse_git_remote_revision_observation(
    const ValidatedGitRemoteRevisionRequest& request,
    int exit_status,
    std::string_view stdout_bytes);

// Executes one repositoryless observation with the fixed trusted Git profile.
// The executable, argv shape, complete environment, cwd, stdin/stderr policy,
// timeout, termination grace, and capture limit are owned by this function and
// cannot be weakened by a caller.
[[nodiscard]] GitRemoteRevisionObservationResult
observe_git_remote_revision(
    const ValidatedGitRemoteRevisionRequest& request);

#ifdef MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS
// These fixtures exist only in the focused observer target. They do not form
// a production authority producer or a production branch validator.
[[nodiscard]] AuthorityApprovedGitSourceIdentity
make_authority_approved_git_source_identity_fixture_for_test(
    VcsSourceIdentity source);

[[nodiscard]] ValidatedExactGitBranch
make_validated_exact_git_branch_fixture_for_test(std::string branch_name);

[[nodiscard]] ExactGitBranchValidationResult
classify_exact_git_branch_exited_process_fixture_for_test(
    std::string branch_name,
    int exit_code,
    std::string stdout_bytes);

// The focused observer target may replace only the process executable and
// child argv, plus shorten the lifecycle deadline. It still uses the trusted
// observer env/cwd/fd profile and the production 16 KiB capture limit, then
// enters the same mechanical-result-to-parser composition as production.
[[nodiscard]] GitRemoteRevisionObservationResult
observe_git_remote_revision_process_fixture_for_test(
    const ValidatedGitRemoteRevisionRequest& request,
    std::string executable,
    std::vector<std::string> arguments,
    std::chrono::milliseconds hard_timeout,
    std::chrono::milliseconds termination_grace);

[[nodiscard]] std::vector<std::string>
git_remote_revision_observer_arguments_fixture_for_test(
    const ValidatedGitRemoteRevisionRequest& request);

[[nodiscard]] GitRemoteRevisionObservationResult
classify_git_remote_revision_bounded_process_fixture_for_test(
    const ValidatedGitRemoteRevisionRequest& request,
    BoundedCapturedProcessResult process_result);
#endif
