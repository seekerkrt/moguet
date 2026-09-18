#include "git_remote_revision_observer.hpp"

#include "process.hpp"
#include "trusted_git_process_policy.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

namespace {

constexpr std::string_view GIT_REMOTE_OBSERVER_EXECUTABLE =
    "/usr/bin/git";

#if defined(MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS) || defined(MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS)
ExactGitBranchValidationProcessTestHook g_exact_branch_process_test_hook;
#endif

class OwnedObserverDescriptor final {
public:
    explicit OwnedObserverDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }

    OwnedObserverDescriptor(const OwnedObserverDescriptor&) = delete;
    OwnedObserverDescriptor& operator=(const OwnedObserverDescriptor&) =
        delete;

    ~OwnedObserverDescriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

struct CurlUrlDeleter {
    void operator()(CURLU* handle) const noexcept {
        curl_url_cleanup(handle);
    }
};

struct CurlStringDeleter {
    void operator()(char* value) const noexcept {
        curl_free(value);
    }
};

using CurlUrl = std::unique_ptr<CURLU, CurlUrlDeleter>;
using CurlString = std::unique_ptr<char, CurlStringDeleter>;

[[noreturn]] void throw_invalid_remote() {
    throw std::invalid_argument("HTTPS Git remote is invalid.");
}

void require_url_api_success(CURLUcode result) {
    if(result == CURLUE_OK) return;
    if(result == CURLUE_OUT_OF_MEMORY) throw std::bad_alloc();
    throw_invalid_remote();
}

bool has_forbidden_remote_input_byte(std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte <= 0x20 || byte == 0x7f;
    });
}

bool has_explicit_https_authority(std::string_view value) noexcept {
    // CURLU accepts and repairs forms such as `https:///host`. Network
    // authority must be explicit in the approved spelling, not guessed by
    // that convenience normalization.
    constexpr std::string_view HTTPS_PREFIX = "https://";
    if(value.size() <= HTTPS_PREFIX.size()) return false;
    for(std::size_t index = 0; index < HTTPS_PREFIX.size(); ++index) {
        char character = value[index];
        if(character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        if(character != HTTPS_PREFIX[index]) return false;
    }
    return value[HTTPS_PREFIX.size()] != '/';
}

std::string get_required_url_part(
    const CURLU* handle, CURLUPart part,
    unsigned int flags = 0) {
    char* raw_value = nullptr;
    require_url_api_success(
        curl_url_get(handle, part, &raw_value, flags));
    CurlString value(raw_value);
    if(value == nullptr || *value == '\0') throw_invalid_remote();
    return value.get();
}

bool has_url_part(const CURLU* handle, CURLUPart part) {
    char* raw_value = nullptr;
    const CURLUcode result = curl_url_get(
        handle, part, &raw_value, CURLU_GET_EMPTY);
    CurlString value(raw_value);
    if(result == CURLUE_OK) return true;
    switch(result) {
        case CURLUE_NO_USER:
        case CURLUE_NO_PASSWORD:
        case CURLUE_NO_OPTIONS:
        case CURLUE_NO_QUERY:
        case CURLUE_NO_FRAGMENT:
        case CURLUE_NO_ZONEID:
            return false;
        case CURLUE_OUT_OF_MEMORY:
            throw std::bad_alloc();
        default:
            throw_invalid_remote();
    }
}

bool is_ascii(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](char character) {
        return static_cast<unsigned char>(character) <= 0x7f;
    });
}

std::string ascii_lowercase(std::string value) {
    for(char& character : value) {
        if(character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

std::string canonicalize_https_git_remote(std::string_view remote) {
    if(remote.empty() || !has_explicit_https_authority(remote) ||
       remote.size() > VALIDATED_HTTPS_GIT_REMOTE_MAX_INPUT_BYTES ||
       has_forbidden_remote_input_byte(remote) ||
       remote.find('?') != std::string_view::npos ||
       remote.find('#') != std::string_view::npos) {
        throw_invalid_remote();
    }

    CurlUrl handle(curl_url());
    if(handle == nullptr) throw std::bad_alloc();

    const std::string owned_remote(remote);
    require_url_api_success(curl_url_set(
        handle.get(),
        CURLUPART_URL,
        owned_remote.c_str(),
        CURLU_DISALLOW_USER));

    const std::string scheme =
        get_required_url_part(handle.get(), CURLUPART_SCHEME);
    if(scheme != "https") throw_invalid_remote();

    std::string host =
        get_required_url_part(handle.get(), CURLUPART_HOST);
    // libcurl 8.21 preserves DNS-host letter case and its PUNYCODE get flag
    // does not canonicalize every raw Unicode hostname. Slice 1 therefore
    // accepts deterministic ASCII (including punycode) and normalizes only
    // ASCII case; it does not invent an RFC/IDN normalizer.
    if(!is_ascii(host) || host.find('%') != std::string::npos) {
        throw_invalid_remote();
    }
    if(has_url_part(handle.get(), CURLUPART_USER) ||
       has_url_part(handle.get(), CURLUPART_PASSWORD) ||
       has_url_part(handle.get(), CURLUPART_OPTIONS) ||
       has_url_part(handle.get(), CURLUPART_QUERY) ||
       has_url_part(handle.get(), CURLUPART_FRAGMENT) ||
       has_url_part(handle.get(), CURLUPART_ZONEID)) {
        throw_invalid_remote();
    }

    host = ascii_lowercase(std::move(host));
    require_url_api_success(curl_url_set(
        handle.get(), CURLUPART_HOST, host.c_str(), 0));

    return get_required_url_part(
        handle.get(), CURLUPART_URL, CURLU_NO_DEFAULT_PORT);
}

#ifdef MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS
void require_no_embedded_nul(std::string_view value) {
    if(value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Exact Git branch contains NUL.");
    }
}
#endif

void require_request_matches_source(
    const AuthorityApprovedGitSourceIdentity& approved_source,
    const GitRemoteRevisionObservationKey& key) {
    const VcsSourceIdentity& source = approved_source.source();
    if(source.kind() != VcsKind::Git ||
       ValidatedHttpsGitRemote::make(source.source_location()) !=
           key.remote()) {
        throw std::invalid_argument(
            "Git remote revision request source and remote differ.");
    }

    switch(source.selector().kind()) {
        case VcsSelectorKind::DefaultHead:
            if(key.selector().kind() !=
               ValidatedGitRemoteSelectorKind::DefaultHead) {
                throw std::invalid_argument(
                    "Git remote revision request selectors differ.");
            }
            return;
        case VcsSelectorKind::Branch: {
            const std::string* source_branch = source.selector().value();
            const ValidatedExactGitBranch* key_branch =
                key.selector().exact_branch();
            if(source_branch == nullptr || key_branch == nullptr ||
               *source_branch != key_branch->name()) {
                throw std::invalid_argument(
                    "Git remote revision request selectors differ.");
            }
            return;
        }
        case VcsSelectorKind::FixedRevision:
        case VcsSelectorKind::Tag:
        case VcsSelectorKind::Unsupported:
        case VcsSelectorKind::Unrecognized:
            throw std::invalid_argument(
                "Git remote revision request selector is unsupported.");
    }
    throw std::invalid_argument(
        "Git remote revision request selector is invalid.");
}

bool has_ascii_whitespace(std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](char character) {
        switch(character) {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
            case '\f':
            case '\v':
                return true;
            default:
                return false;
        }
    });
}

struct ParsedOidRecord {
    SourceRevisionIdentity revision;
    std::string_view ref;
};

struct ParsedSymrefRecord {
    std::string_view target;
    std::string_view ref;
};

using ParsedRemoteRecord =
    std::variant<ParsedOidRecord, ParsedSymrefRecord>;

bool has_unexpected_transcript_control(std::string_view output) noexcept {
    return std::any_of(output.begin(), output.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte <= 0x1f && byte != '\t' && byte != '\n') ||
               byte == 0x7f;
    });
}

std::optional<ParsedRemoteRecord> parse_remote_record(
    std::string_view line) {
    if(line.empty()) return std::nullopt;
    const std::size_t delimiter = line.find('\t');
    if(delimiter == std::string_view::npos ||
       line.find('\t', delimiter + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view first = line.substr(0, delimiter);
    const std::string_view ref = line.substr(delimiter + 1);
    if(ref.empty() || has_ascii_whitespace(ref)) return std::nullopt;

    constexpr std::string_view SYMREF_PREFIX = "ref: ";
    if(first.starts_with(SYMREF_PREFIX)) {
        const std::string_view target = first.substr(SYMREF_PREFIX.size());
        if(target.empty() || has_ascii_whitespace(target)) {
            return std::nullopt;
        }
        return ParsedSymrefRecord{target, ref};
    }

    try {
        return ParsedOidRecord{
            SourceRevisionIdentity::git_commit(std::string(first)), ref};
    } catch(const std::invalid_argument&) {
        return std::nullopt;
    }
}

std::optional<std::vector<ParsedRemoteRecord>> parse_remote_records(
    std::string_view output) {
    std::vector<ParsedRemoteRecord> records;
    std::size_t offset = 0;
    while(offset < output.size()) {
        const std::size_t line_feed = output.find('\n', offset);
        if(line_feed == std::string_view::npos) return std::nullopt;
        const auto record = parse_remote_record(
            output.substr(offset, line_feed - offset));
        if(!record.has_value()) return std::nullopt;
        records.push_back(*record);
        offset = line_feed + 1;
    }
    return records;
}

bool is_expected_oid_record(
    const ParsedRemoteRecord& record, std::string_view expected_ref) {
    const auto* oid = std::get_if<ParsedOidRecord>(&record);
    return oid != nullptr && oid->ref == expected_ref;
}

std::size_t count_oid_records(
    const std::vector<ParsedRemoteRecord>& records) {
    return static_cast<std::size_t>(std::count_if(
        records.begin(), records.end(), [](const ParsedRemoteRecord& record) {
            return std::holds_alternative<ParsedOidRecord>(record);
        }));
}

std::size_t count_expected_oid_records(
    const std::vector<ParsedRemoteRecord>& records,
    std::string_view expected_ref) {
    return static_cast<std::size_t>(std::count_if(
        records.begin(), records.end(),
        [expected_ref](const ParsedRemoteRecord& record) {
            return is_expected_oid_record(record, expected_ref);
        }));
}

bool is_safe_symbolic_head_target(std::string_view target) noexcept {
    constexpr std::string_view HEADS_PREFIX = "refs/heads/";
    return target.starts_with(HEADS_PREFIX) &&
           target.size() > HEADS_PREFIX.size() &&
           !has_ascii_whitespace(target);
}

GitRemoteRevisionMalformedOutput malformed(
    const ValidatedGitRemoteRevisionRequest& request,
    GitRemoteRevisionMalformedOutputReason reason) {
    return GitRemoteRevisionMalformedOutput{request.key(), reason};
}

GitRemoteRevisionAmbiguousOutput ambiguous(
    const ValidatedGitRemoteRevisionRequest& request,
    GitRemoteRevisionAmbiguousOutputReason reason,
    std::size_t record_count) {
    return GitRemoteRevisionAmbiguousOutput{
        request.key(), reason, record_count};
}

} // namespace

namespace git_remote_revision_observer_detail {

class ExactBranchFactory final {
public:
    [[nodiscard]] static ValidatedExactGitBranch make(
        std::string name) noexcept {
        return ValidatedExactGitBranch(std::move(name));
    }
};

class ObservationResultFactory final {
public:
    [[nodiscard]] static ObservedGitRemoteRevision observed(
        const ValidatedGitRemoteRevisionRequest& request,
        const SourceRevisionIdentity& revision) {
        const std::string* object_id = revision.git_commit();
        if(object_id == nullptr) {
            throw std::logic_error(
                "Observed Git revision has no commit identity.");
        }
        return ObservedGitRemoteRevision(
            request,
            UpstreamGitRevision::git_commit(
                request.source().source(), *object_id));
    }
};

} // namespace git_remote_revision_observer_detail

namespace {

ExactGitBranchValidationResult classify_exact_branch_exited_process(
    std::string_view branch_name,
    int exit_code,
    std::string_view output) {
    if(exit_code != 0) {
        return InvalidExactGitBranch{
            InvalidExactGitBranchReason::GitRejected, exit_code};
    }
    std::string expected_output(branch_name);
    expected_output.push_back('\n');
    if(output != expected_output) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::UnexpectedOutput,
            std::nullopt};
    }
    return git_remote_revision_observer_detail::ExactBranchFactory::make(
        std::string(branch_name));
}

std::vector<std::string> git_remote_revision_observer_arguments(
    const ValidatedGitRemoteRevisionRequest& request) {
    std::vector<std::string> arguments =
        trusted_git_observer_process_arguments();
    arguments.push_back("ls-remote");
    arguments.push_back("--quiet");

    switch(request.key().selector().kind()) {
        case ValidatedGitRemoteSelectorKind::DefaultHead:
            arguments.push_back("--symref");
            arguments.push_back("--exit-code");
            arguments.push_back(
                request.key().remote().canonical_url());
            arguments.push_back("HEAD");
            return arguments;
        case ValidatedGitRemoteSelectorKind::ExactBranch: {
            const ValidatedExactGitBranch* branch =
                request.key().selector().exact_branch();
            if(branch == nullptr) {
                throw std::logic_error(
                    "Exact branch selector has no validated branch.");
            }
            arguments.push_back("--refs");
            arguments.push_back("--branches");
            arguments.push_back("--exit-code");
            arguments.push_back(
                request.key().remote().canonical_url());
            arguments.push_back("refs/heads/" + branch->name());
            return arguments;
        }
    }
    throw std::logic_error("Unknown Git remote observer selector.");
}

GitRemoteRevisionProcessFailure observer_process_failure(
    const ValidatedGitRemoteRevisionRequest& request,
    BoundedProcessSignaled cause) {
    return GitRemoteRevisionProcessFailure{
        request.key(), std::move(cause)};
}

GitRemoteRevisionProcessFailure observer_process_failure(
    const ValidatedGitRemoteRevisionRequest& request,
    BoundedProcessLaunchOrSetupFailure cause) {
    return GitRemoteRevisionProcessFailure{
        request.key(), std::move(cause)};
}

GitRemoteRevisionProcessFailure observer_process_failure(
    const ValidatedGitRemoteRevisionRequest& request,
    BoundedProcessIoOrWaitFailure cause) {
    return GitRemoteRevisionProcessFailure{
        request.key(), std::move(cause)};
}

GitRemoteRevisionObservationResult classify_observer_process_result(
    const ValidatedGitRemoteRevisionRequest& request,
    const BoundedCapturedProcessResult& process_result) {
    if(const auto* exited =
           std::get_if<BoundedProcessExited>(&process_result.outcome)) {
        return parse_git_remote_revision_observation(
            request, exited->exit_code, process_result.output);
    }
    if(const auto* signaled =
           std::get_if<BoundedProcessSignaled>(&process_result.outcome)) {
        return observer_process_failure(request, *signaled);
    }
    if(const auto* launch_failure =
           std::get_if<BoundedProcessLaunchOrSetupFailure>(
               &process_result.outcome)) {
        return observer_process_failure(request, *launch_failure);
    }
    if(const auto* io_failure =
           std::get_if<BoundedProcessIoOrWaitFailure>(
               &process_result.outcome)) {
        return observer_process_failure(request, *io_failure);
    }
    if(std::holds_alternative<BoundedProcessTimedOut>(
           process_result.outcome)) {
        return GitRemoteRevisionTimeout{request.key()};
    }
    if(const auto* capture_failure =
           std::get_if<BoundedProcessCaptureLimitExceeded>(
               &process_result.outcome)) {
        return GitRemoteRevisionCaptureLimitExceeded{
            request.key(), capture_failure->capture_limit};
    }
    throw std::logic_error("Unknown Git remote observer process outcome.");
}

GitRemoteRevisionObservationResult execute_git_remote_revision_observer(
    const ValidatedGitRemoteRevisionRequest& request,
    std::string executable,
    std::vector<std::string> arguments,
    std::chrono::milliseconds hard_timeout,
    std::chrono::milliseconds termination_grace) {
    int directory_descriptor;
    do {
        directory_descriptor = open(
            "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while(directory_descriptor == -1 && errno == EINTR);
    if(directory_descriptor == -1) {
        return observer_process_failure(
            request,
            BoundedProcessLaunchOrSetupFailure{
                BoundedProcessLaunchStage::WorkingDirectory, errno});
    }
    OwnedObserverDescriptor directory(directory_descriptor);

    int input_descriptor;
    do {
        input_descriptor = open(
            "/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    } while(input_descriptor == -1 && errno == EINTR);
    if(input_descriptor == -1) {
        return observer_process_failure(
            request,
            BoundedProcessLaunchOrSetupFailure{
                BoundedProcessLaunchStage::StandardInput, errno});
    }
    OwnedObserverDescriptor input(input_descriptor);

    std::vector<std::string> environment;
    try {
        environment = trusted_git_process_environment(
            TrustedGitProcessEnvironmentMode::ReadOnlyObservation);
    } catch(const std::runtime_error&) {
        // The trusted environment owner supplies a value-free failure. Keep
        // observer control flow typed without retaining a
        // CA/proxy value or converting setup failure into a Git exit.
        return observer_process_failure(
            request,
            BoundedProcessLaunchOrSetupFailure{
                BoundedProcessLaunchStage::InvocationValidation, EINVAL});
    }

    ExplicitProcessInvocation invocation{
        std::move(executable), std::move(arguments),
        std::move(environment)};
    invocation.working_directory_fd = directory.get();
    invocation.standard_input_fd = input.get();
    const BoundedProcessPolicy policy{
        hard_timeout,
        termination_grace,
        GIT_REMOTE_OBSERVER_STDOUT_CAPTURE_LIMIT,
        true};
    return classify_observer_process_result(
        request,
        capture_bounded_explicit_process_output_raw(invocation, policy));
}

} // namespace

AuthorityApprovedGitSourceIdentity::AuthorityApprovedGitSourceIdentity(
    VcsSourceIdentity source)
    : source_(std::move(source)) {
    if(source_.kind() != VcsKind::Git) {
        throw std::invalid_argument(
            "Authority-approved Git source must have Git kind.");
    }
}

const VcsSourceIdentity&
AuthorityApprovedGitSourceIdentity::source() const noexcept {
    return source_;
}

ValidatedHttpsGitRemote::ValidatedHttpsGitRemote(
    std::string canonical_url) noexcept
    : canonical_url_(std::move(canonical_url)) {
}

ValidatedHttpsGitRemote ValidatedHttpsGitRemote::make(
    std::string_view remote) {
    return ValidatedHttpsGitRemote(canonicalize_https_git_remote(remote));
}

const std::string& ValidatedHttpsGitRemote::canonical_url() const noexcept {
    return canonical_url_;
}

ValidatedExactGitBranch::ValidatedExactGitBranch(std::string name) noexcept
    : name_(std::move(name)) {
}

const std::string& ValidatedExactGitBranch::name() const noexcept {
    return name_;
}

ExactGitBranchValidationResult validate_exact_git_branch(
    std::string_view branch_name) {
    if(branch_name.empty()) {
        return InvalidExactGitBranch{
            InvalidExactGitBranchReason::Empty, std::nullopt};
    }
    if(branch_name.find('\0') != std::string_view::npos) {
        return InvalidExactGitBranch{
            InvalidExactGitBranchReason::EmbeddedNul, std::nullopt};
    }
    if(branch_name.size() >
       VALIDATED_EXACT_GIT_BRANCH_MAX_INPUT_BYTES) {
        return InvalidExactGitBranch{
            InvalidExactGitBranchReason::InputTooLong, std::nullopt};
    }

    int directory_descriptor;
    do {
        directory_descriptor = open(
            "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while(directory_descriptor == -1 && errno == EINTR);
    if(directory_descriptor == -1) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::LaunchOrSetup,
            errno};
    }
    OwnedObserverDescriptor directory(directory_descriptor);

    int input_descriptor;
    do {
        input_descriptor = open(
            "/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    } while(input_descriptor == -1 && errno == EINTR);
    if(input_descriptor == -1) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::LaunchOrSetup,
            errno};
    }
    OwnedObserverDescriptor input(input_descriptor);

    std::vector<std::string> environment;
    try {
        environment = trusted_git_process_environment(
            TrustedGitProcessEnvironmentMode::ReadOnlyObservation);
    } catch(const std::runtime_error&) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::LaunchOrSetup,
            EINVAL};
    }

    std::vector<std::string> arguments =
        trusted_git_observer_process_arguments();
    arguments.push_back("check-ref-format");
    arguments.push_back("--branch");
    arguments.emplace_back(branch_name);

    ExplicitProcessInvocation invocation{
        "/usr/bin/git", std::move(arguments), std::move(environment)};
    invocation.working_directory_fd = directory.get();
    invocation.standard_input_fd = input.get();
    const BoundedProcessPolicy policy{
        GIT_REMOTE_OBSERVER_PROCESS_HARD_TIMEOUT,
        GIT_REMOTE_OBSERVER_PROCESS_TERMINATION_GRACE,
        VALIDATED_EXACT_GIT_BRANCH_MAX_INPUT_BYTES + 1U,
        true};
    BoundedCapturedProcessResult process_result = [&] {
#if defined(MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS) || defined(MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS)
        if(g_exact_branch_process_test_hook) return g_exact_branch_process_test_hook(invocation, policy);
#endif
        return capture_bounded_explicit_process_output_raw(invocation, policy);
    }();

    if(process_result.cancellation_signal) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::Cancelled,
            std::nullopt, std::move(process_result.outcome), process_result.cancellation_signal};
    }

    if(const auto* exited =
           std::get_if<BoundedProcessExited>(&process_result.outcome)) {
        return classify_exact_branch_exited_process(
            branch_name, exited->exit_code, process_result.output);
    }
    if(const auto* signaled =
           std::get_if<BoundedProcessSignaled>(
               &process_result.outcome)) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::Signaled,
            signaled->signal_number};
    }
    if(const auto* launch_failure =
           std::get_if<BoundedProcessLaunchOrSetupFailure>(
               &process_result.outcome)) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::LaunchOrSetup,
            launch_failure->error_number};
    }
    if(const auto* io_failure =
           std::get_if<BoundedProcessIoOrWaitFailure>(
               &process_result.outcome)) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::IoOrWait,
            io_failure->error_number};
    }
    if(std::holds_alternative<BoundedProcessTimedOut>(
           process_result.outcome)) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::TimedOut,
            std::nullopt};
    }
    if(const auto* capture_failure =
           std::get_if<BoundedProcessCaptureLimitExceeded>(
               &process_result.outcome)) {
        return ExactGitBranchValidationProcessFailure{
            ExactGitBranchValidationProcessFailureReason::
                CaptureLimitExceeded,
            static_cast<int>(capture_failure->capture_limit)};
    }
    throw std::logic_error(
        "Unknown exact Git branch validation process outcome.");
}

#if defined(MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS) || defined(MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS)
void set_exact_git_branch_validation_process_test_hook(ExactGitBranchValidationProcessTestHook hook) {
    g_exact_branch_process_test_hook = std::move(hook);
}
#endif

ValidatedGitRemoteSelector::ValidatedGitRemoteSelector(
    ValidatedGitRemoteSelectorKind kind,
    std::optional<ValidatedExactGitBranch> exact_branch) noexcept
    : kind_(kind), exact_branch_(std::move(exact_branch)) {
}

ValidatedGitRemoteSelector
ValidatedGitRemoteSelector::default_head() noexcept {
    return ValidatedGitRemoteSelector(
        ValidatedGitRemoteSelectorKind::DefaultHead, std::nullopt);
}

ValidatedGitRemoteSelector ValidatedGitRemoteSelector::exact_branch(
    ValidatedExactGitBranch branch) noexcept {
    return ValidatedGitRemoteSelector(
        ValidatedGitRemoteSelectorKind::ExactBranch,
        std::move(branch));
}

ValidatedGitRemoteSelectorKind ValidatedGitRemoteSelector::kind()
    const noexcept {
    return kind_;
}

const ValidatedExactGitBranch*
ValidatedGitRemoteSelector::exact_branch() const noexcept {
    return exact_branch_.has_value() ? &exact_branch_.value() : nullptr;
}

GitRemoteRevisionObservationKey::GitRemoteRevisionObservationKey(
    ValidatedHttpsGitRemote remote,
    ValidatedGitRemoteSelector selector) noexcept
    : remote_(std::move(remote)), selector_(std::move(selector)) {
}

GitRemoteRevisionObservationKey GitRemoteRevisionObservationKey::make(
    ValidatedHttpsGitRemote remote,
    ValidatedGitRemoteSelector selector) noexcept {
    return GitRemoteRevisionObservationKey(
        std::move(remote), std::move(selector));
}

const ValidatedHttpsGitRemote& GitRemoteRevisionObservationKey::remote()
    const noexcept {
    return remote_;
}

const ValidatedGitRemoteSelector&
GitRemoteRevisionObservationKey::selector() const noexcept {
    return selector_;
}

ValidatedGitRemoteRevisionRequest::ValidatedGitRemoteRevisionRequest(
    AuthorityApprovedGitSourceIdentity source,
    GitRemoteRevisionObservationKey key) noexcept
    : source_(std::move(source)), key_(std::move(key)) {
}

ValidatedGitRemoteRevisionRequest ValidatedGitRemoteRevisionRequest::make(
    AuthorityApprovedGitSourceIdentity source,
    GitRemoteRevisionObservationKey key) {
    require_request_matches_source(source, key);
    return ValidatedGitRemoteRevisionRequest(
        std::move(source), std::move(key));
}

const AuthorityApprovedGitSourceIdentity&
ValidatedGitRemoteRevisionRequest::source() const noexcept {
    return source_;
}

const GitRemoteRevisionObservationKey&
ValidatedGitRemoteRevisionRequest::key() const noexcept {
    return key_;
}

ObservedGitRemoteRevision::ObservedGitRemoteRevision(
    ValidatedGitRemoteRevisionRequest request,
    UpstreamGitRevision revision) noexcept
    : request_(std::move(request)), revision_(std::move(revision)) {
}

const ValidatedGitRemoteRevisionRequest&
ObservedGitRemoteRevision::request() const noexcept {
    return request_;
}

const UpstreamGitRevision&
ObservedGitRemoteRevision::revision() const noexcept {
    return revision_;
}

GitRemoteRevisionObservationResult parse_git_remote_revision_observation(
    const ValidatedGitRemoteRevisionRequest& request,
    int exit_status,
    std::string_view stdout_bytes) {
    if(exit_status == 2) {
        if(stdout_bytes.empty()) {
            return GitRemoteRevisionRefNotFound{request.key()};
        }
        return malformed(
            request,
            GitRemoteRevisionMalformedOutputReason::
                RefNotFoundStatusWithOutput);
    }
    if(exit_status != 0) {
        return GitRemoteRevisionGitExitFailure{
            request.key(), exit_status};
    }
    if(stdout_bytes.empty()) {
        return malformed(
            request,
            GitRemoteRevisionMalformedOutputReason::EmptyOutput);
    }
    if(stdout_bytes.back() != '\n') {
        return malformed(
            request,
            GitRemoteRevisionMalformedOutputReason::MissingFinalLineFeed);
    }
    if(has_unexpected_transcript_control(stdout_bytes)) {
        return malformed(
            request,
            GitRemoteRevisionMalformedOutputReason::
                UnexpectedControlByte);
    }

    const auto parsed = parse_remote_records(stdout_bytes);
    if(!parsed.has_value() || parsed->empty()) {
        return malformed(
            request,
            GitRemoteRevisionMalformedOutputReason::InvalidRecord);
    }
    const std::vector<ParsedRemoteRecord>& records = *parsed;

    if(request.key().selector().kind() ==
       ValidatedGitRemoteSelectorKind::ExactBranch) {
        const ValidatedExactGitBranch* branch =
            request.key().selector().exact_branch();
        if(branch == nullptr) {
            throw std::logic_error(
                "Exact branch selector has no validated branch.");
        }
        const std::string expected_ref =
            "refs/heads/" + branch->name();
        const std::size_t expected_count =
            count_expected_oid_records(records, expected_ref);
        if(records.size() != 1) {
            if(expected_count > 1) {
                return ambiguous(
                    request,
                    GitRemoteRevisionAmbiguousOutputReason::
                        DuplicateExpectedRecord,
                    records.size());
            }
            if(expected_count == 1 && count_oid_records(records) > 1) {
                return ambiguous(
                    request,
                    GitRemoteRevisionAmbiguousOutputReason::ExtraOidRecord,
                    records.size());
            }
            return malformed(
                request,
                count_oid_records(records) == records.size()
                    ? GitRemoteRevisionMalformedOutputReason::WrongRef
                    : GitRemoteRevisionMalformedOutputReason::
                          UnexpectedSymref);
        }

        const auto* oid = std::get_if<ParsedOidRecord>(&records.front());
        if(oid == nullptr) {
            return malformed(
                request,
                GitRemoteRevisionMalformedOutputReason::UnexpectedSymref);
        }
        if(oid->ref != expected_ref) {
            return malformed(
                request,
                GitRemoteRevisionMalformedOutputReason::WrongRef);
        }
        return git_remote_revision_observer_detail::ObservationResultFactory::
            observed(request, oid->revision);
    }

    if(records.size() == 1) {
        const auto* oid = std::get_if<ParsedOidRecord>(&records.front());
        if(oid == nullptr) {
            return malformed(
                request,
                GitRemoteRevisionMalformedOutputReason::UnexpectedSymref);
        }
        if(oid->ref != "HEAD") {
            return malformed(
                request,
                GitRemoteRevisionMalformedOutputReason::WrongRef);
        }
        return git_remote_revision_observer_detail::ObservationResultFactory::
            observed(request, oid->revision);
    }

    if(records.size() == 2) {
        const auto* symref =
            std::get_if<ParsedSymrefRecord>(&records.front());
        const auto* oid = std::get_if<ParsedOidRecord>(&records.back());
        if(symref != nullptr && oid != nullptr) {
            if(symref->ref != "HEAD" || oid->ref != "HEAD") {
                return malformed(
                    request,
                    GitRemoteRevisionMalformedOutputReason::WrongRef);
            }
            if(!is_safe_symbolic_head_target(symref->target)) {
                return malformed(
                    request,
                    GitRemoteRevisionMalformedOutputReason::
                        InvalidSymbolicHeadTarget);
            }
            return git_remote_revision_observer_detail::
                ObservationResultFactory::observed(request, oid->revision);
        }

        const auto* first_oid =
            std::get_if<ParsedOidRecord>(&records.front());
        const auto* second_symref =
            std::get_if<ParsedSymrefRecord>(&records.back());
        if(first_oid != nullptr && first_oid->ref == "HEAD" &&
           second_symref != nullptr) {
            if(second_symref->ref != "HEAD") {
                return malformed(
                    request,
                    GitRemoteRevisionMalformedOutputReason::WrongRef);
            }
            if(!is_safe_symbolic_head_target(second_symref->target)) {
                return malformed(
                    request,
                    GitRemoteRevisionMalformedOutputReason::
                        InvalidSymbolicHeadTarget);
            }
            return malformed(
                request,
                GitRemoteRevisionMalformedOutputReason::WrongRecordOrder);
        }
    }

    const std::size_t expected_oid_count =
        count_expected_oid_records(records, "HEAD");
    const std::size_t oid_count = count_oid_records(records);
    if(expected_oid_count > 1) {
        return ambiguous(
            request,
            GitRemoteRevisionAmbiguousOutputReason::DuplicateExpectedRecord,
            records.size());
    }
    if(expected_oid_count == 1 && oid_count > 1) {
        return ambiguous(
            request,
            GitRemoteRevisionAmbiguousOutputReason::ExtraOidRecord,
            records.size());
    }
    if(expected_oid_count == 1) {
        return malformed(
            request,
            GitRemoteRevisionMalformedOutputReason::UnexpectedSymref);
    }
    return malformed(
        request, GitRemoteRevisionMalformedOutputReason::WrongRef);
}

GitRemoteRevisionObservationResult observe_git_remote_revision(
    const ValidatedGitRemoteRevisionRequest& request) {
    return execute_git_remote_revision_observer(
        request,
        std::string(GIT_REMOTE_OBSERVER_EXECUTABLE),
        git_remote_revision_observer_arguments(request),
        GIT_REMOTE_OBSERVER_PROCESS_HARD_TIMEOUT,
        GIT_REMOTE_OBSERVER_PROCESS_TERMINATION_GRACE);
}

#ifdef MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS
AuthorityApprovedGitSourceIdentity
make_authority_approved_git_source_identity_fixture_for_test(
    VcsSourceIdentity source) {
    return AuthorityApprovedGitSourceIdentity(std::move(source));
}

ValidatedExactGitBranch make_validated_exact_git_branch_fixture_for_test(
    std::string branch_name) {
    if(branch_name.empty() ||
       branch_name.size() >
           VALIDATED_EXACT_GIT_BRANCH_MAX_INPUT_BYTES) {
        throw std::invalid_argument(
            "Exact Git branch fixture exceeds its structural bound.");
    }
    require_no_embedded_nul(branch_name);
    return ValidatedExactGitBranch(std::move(branch_name));
}

ExactGitBranchValidationResult
classify_exact_git_branch_exited_process_fixture_for_test(
    std::string branch_name,
    int exit_code,
    std::string stdout_bytes) {
    return classify_exact_branch_exited_process(
        branch_name, exit_code, stdout_bytes);
}

GitRemoteRevisionObservationResult
observe_git_remote_revision_process_fixture_for_test(
    const ValidatedGitRemoteRevisionRequest& request,
    std::string executable,
    std::vector<std::string> arguments,
    std::chrono::milliseconds hard_timeout,
    std::chrono::milliseconds termination_grace) {
    return execute_git_remote_revision_observer(
        request,
        std::move(executable),
        std::move(arguments),
        hard_timeout,
        termination_grace);
}

std::vector<std::string>
git_remote_revision_observer_arguments_fixture_for_test(
    const ValidatedGitRemoteRevisionRequest& request) {
    return git_remote_revision_observer_arguments(request);
}

GitRemoteRevisionObservationResult
classify_git_remote_revision_bounded_process_fixture_for_test(
    const ValidatedGitRemoteRevisionRequest& request,
    BoundedCapturedProcessResult process_result) {
    return classify_observer_process_result(request, process_result);
}
#endif
