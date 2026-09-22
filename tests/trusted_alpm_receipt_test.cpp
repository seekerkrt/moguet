#include "invocation_owned_cleanup_adapter.hpp"
#include "process.hpp"
#include "trusted_alpm_receipt_helper_state.hpp"
#include "trusted_alpm_receipt_protocol.hpp"
#include "trusted_alpm_receipt_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>

#ifndef MOGUET_ALPM_RECEIPT_HELPER_PATH
#error "MOGUET_ALPM_RECEIPT_HELPER_PATH is required"
#endif

namespace {

namespace fs = std::filesystem;

void expect(bool condition, const std::string& diagnostic) {
    if(!condition) throw std::runtime_error(diagnostic);
}

template <typename Callable>
void expect_failure(Callable&& callable, const std::string& diagnostic) {
    try {
        std::forward<Callable>(callable)();
    } catch(const std::exception&) {
        return;
    }
    throw std::runtime_error(diagnostic);
}

std::string transaction_token(char digit) {
    return std::string(TRUSTED_ALPM_RECEIPT_TOKEN_HEX_LENGTH, digit);
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/moguet-receipt-test-XXXXXX";
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back('\0');
        char* created = mkdtemp(bytes.data());
        if(created == nullptr) {
            throw std::runtime_error(
                "unable to create trusted receipt test directory");
        }
        path_ = created;
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        fs::remove_all(path_);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

TrustedAlpmReceiptStateStore open_test_store(
    const TemporaryDirectory& directory,
    uid_t expected_owner = geteuid()) {
    const int descriptor = open(
        directory.path().c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(descriptor == -1) {
        throw std::runtime_error("unable to open receipt test root");
    }
    try {
        TrustedAlpmReceiptStateStore store =
            TrustedAlpmReceiptStateStore::open_below_runtime_parent(
                descriptor, expected_owner);
        static_cast<void>(close(descriptor));
        return store;
    } catch(...) {
        static_cast<void>(close(descriptor));
        throw;
    }
}

fs::path active_root(const TemporaryDirectory& directory) {
    return directory.path() / "moguet" / "alpm-receipts" / "active";
}

fs::path used_root(const TemporaryDirectory& directory) {
    return directory.path() / "moguet" / "alpm-receipts" / "used";
}

mode_t permissions(const fs::path& path) {
    struct stat metadata{};
    if(lstat(path.c_str(), &metadata) == -1) {
        throw std::runtime_error("unable to inspect receipt fixture mode");
    }
    return metadata.st_mode & 07777;
}

void write_regular_file(
    const fs::path& path, const std::string& contents,
    mode_t mode = 0600) {
    const int descriptor = open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        mode);
    if(descriptor == -1) {
        throw std::runtime_error("unable to create receipt fixture file");
    }
    std::size_t offset = 0;
    while(offset < contents.size()) {
        const ssize_t written = write(
            descriptor, contents.data() + offset,
            contents.size() - offset);
        if(written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if(written == -1 && errno == EINTR) continue;
        static_cast<void>(close(descriptor));
        throw std::runtime_error("unable to write receipt fixture file");
    }
    if(fchmod(descriptor, mode) == -1 || close(descriptor) == -1) {
        throw std::runtime_error("unable to finalize receipt fixture file");
    }
}

void record_input(
    TrustedAlpmReceiptStateStore& store,
    const TemporaryDirectory& directory,
    const std::string& transaction_token,
    const std::string& input) {
    const fs::path input_path = directory.path() / "needs-targets-input";
    const int descriptor = open(
        input_path.c_str(),
        O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if(descriptor == -1) {
        throw std::runtime_error("unable to create NeedsTargets fixture");
    }
    std::size_t offset = 0;
    while(offset < input.size()) {
        const ssize_t written = write(
            descriptor, input.data() + offset, input.size() - offset);
        if(written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if(written == -1 && errno == EINTR) continue;
        static_cast<void>(close(descriptor));
        throw std::runtime_error("unable to write NeedsTargets fixture");
    }
    if(lseek(descriptor, 0, SEEK_SET) == -1) {
        static_cast<void>(close(descriptor));
        throw std::runtime_error("unable to rewind NeedsTargets fixture");
    }
    try {
        store.record(transaction_token, descriptor);
        static_cast<void>(close(descriptor));
    } catch(...) {
        static_cast<void>(close(descriptor));
        throw;
    }
}

void test_protocol_rejects_capability_injection() {
    const std::string token = transaction_token('a');
    const std::string owner(
        trusted_alpm_receipt_selected_repository_provider_owner());
    const auto valid = parse_trusted_alpm_receipt_helper_arguments(
        {"prepare", token, owner, "--", "valid-package"});
    expect(
        std::holds_alternative<TrustedAlpmReceiptHelperInvocation>(valid),
        "valid helper prepare protocol was rejected");

    for(const std::vector<std::string>& arguments : {
            std::vector<std::string>{
                "prepare", "../unsafe", owner, "--", "pkg"},
            {"prepare", transaction_token('A'), owner, "--", "pkg"},
            {"prepare", std::string(63, 'a'), owner, "--", "pkg"},
            {"prepare", std::string(65, 'a'), owner, "--", "pkg"},
            {"prepare", token + "\n", owner, "--", "pkg"},
            {"prepare", token, "source-artifact-install", "--", "pkg"},
            {"prepare", token, owner, "--", "../x"},
            {"prepare", token, owner, "--", "bad\nname"},
            {"prepare", token, owner, "--", "same", "same"},
            {"consume", token, owner, "/tmp/output"},
            {"record", token, owner, "/bin/sh"},
            {"abort", token, owner, "arbitrary-executable"},
        }) {
        expect(
            std::holds_alternative<
                TrustedAlpmReceiptProtocolFailure>(
                parse_trusted_alpm_receipt_helper_arguments(
                    arguments)),
            "helper protocol accepted invalid/capability-bearing argv");
    }
}

void test_protocol_completion_and_truncation() {
    const std::string token = transaction_token('b');
    const std::string protocol =
        serialize_trusted_alpm_receipt_machine_receipt(
            TrustedAlpmReceiptMachineReceipt{
                TrustedAlpmReceiptMachineState::Complete, token, {"requested", "solver-introduced"}});
    const TrustedAlpmReceiptMachineReceiptResult parsed =
        parse_trusted_alpm_receipt_machine_receipt(protocol);
    const auto* receipt =
        std::get_if<TrustedAlpmReceiptMachineReceipt>(&parsed);
    expect(
        receipt != nullptr &&
            receipt->installed_package_names ==
                std::vector<std::string>{
                    "requested", "solver-introduced"},
        "complete machine receipt lost actual Install packages");

    std::string truncated = protocol;
    truncated.resize(truncated.size() - 4);
    expect(
        std::holds_alternative<TrustedAlpmReceiptProtocolFailure>(
            parse_trusted_alpm_receipt_machine_receipt(truncated)),
        "truncated receipt became Complete");
    expect(
        std::holds_alternative<TrustedAlpmReceiptProtocolFailure>(
            parse_trusted_alpm_receipt_machine_receipt(
                "MOGUET-ALPM-RECEIPT\t1\nTOKEN\t" + token +
                "\nOWNER\tselected-repository-provider\n"
                "STATE\tComplete\nINSTALL\tdup\nINSTALL\tdup\nEND\n")),
        "duplicate machine receipt package was accepted");
    expect(
        std::holds_alternative<TrustedAlpmReceiptProtocolFailure>(
            parse_trusted_alpm_receipt_needs_targets(
                std::string(
                    TRUSTED_ALPM_RECEIPT_MAXIMUM_PACKAGE_NAME_BYTES +
                        1,
                    'a') +
                "\n")),
        "oversized package record was accepted");
    expect(
        std::holds_alternative<TrustedAlpmReceiptProtocolFailure>(
            parse_trusted_alpm_receipt_needs_targets(
                std::string(
                    TRUSTED_ALPM_RECEIPT_MAXIMUM_BYTES + 1,
                    'a'))),
        "oversized NeedsTargets protocol was accepted");
}

void test_state_store_complete_missing_abort_and_replay() {
    TemporaryDirectory directory;
    TrustedAlpmReceiptStateStore store = open_test_store(directory);

    const std::string complete_token = transaction_token('c');
    store.prepare(complete_token, {"requested-target"});
    expect_failure(
        [&]() { store.prepare(complete_token, {"requested-target"}); },
        "active duplicate token state was reused");
    const fs::path transaction_path = active_root(directory) / complete_token;
    const fs::path hook_path = transaction_path / "hooks" /
                               trusted_alpm_receipt_hook_filename(complete_token);
    expect(
        permissions(transaction_path) == 0700 &&
            permissions(transaction_path / "prepared") == 0600 &&
            permissions(transaction_path / "hooks") == 0700 &&
            permissions(hook_path) == 0600 &&
            !fs::is_symlink(hook_path),
        "prepared state ownership modes are not restrictive");
    std::ifstream hook_file(hook_path);
    const std::string hook_contents{
        std::istreambuf_iterator<char>(hook_file),
        std::istreambuf_iterator<char>()};
    expect(
        hook_contents.find(MOGUET_ALPM_RECEIPT_HELPER_PATH) !=
                std::string::npos &&
            hook_contents.find(" record " + complete_token + " ") !=
                std::string::npos &&
            hook_contents.find("requested-target") ==
                std::string::npos &&
            hook_contents.find("sh -c") == std::string::npos,
        "hook Exec is not fixed to helper/token/owner only");

    record_input(
        store, directory, complete_token,
        "requested-target\nsolver-introduced\n");
    expect(
        permissions(transaction_path / "receipt") == 0600 &&
            !fs::exists(transaction_path / "receipt.partial"),
        "receipt was not atomically published as the final file");
    const TrustedAlpmReceiptMachineReceiptResult consumed =
        parse_trusted_alpm_receipt_machine_receipt(
            store.consume(complete_token));
    const auto* complete =
        std::get_if<TrustedAlpmReceiptMachineReceipt>(&consumed);
    expect(
        complete != nullptr &&
            complete->state ==
                TrustedAlpmReceiptMachineState::Complete &&
            complete->installed_package_names ==
                std::vector<std::string>{
                    "requested-target", "solver-introduced"},
        "consumed receipt did not preserve actual Install set");
    expect(
        !fs::exists(transaction_path) &&
            fs::is_directory(used_root(directory) / complete_token) &&
            fs::is_empty(used_root(directory) / complete_token),
        "consume did not retire state to an empty replay tombstone");
    expect_failure(
        [&]() { static_cast<void>(store.consume(complete_token)); },
        "second consume was not rejected");
    expect_failure(
        [&]() { store.prepare(complete_token, {"requested-target"}); },
        "consumed token was reusable");

    const std::string missing_token = transaction_token('d');
    store.prepare(missing_token, {"already-installed"});
    const auto missing = parse_trusted_alpm_receipt_machine_receipt(
        store.consume(missing_token));
    const auto* missing_receipt =
        std::get_if<TrustedAlpmReceiptMachineReceipt>(&missing);
    expect(
        missing_receipt != nullptr &&
            missing_receipt->state ==
                TrustedAlpmReceiptMachineState::Missing &&
            missing_receipt->installed_package_names.empty(),
        "Install-hook non-match did not remain Missing");

    const std::string aborted_token = transaction_token('e');
    store.prepare(aborted_token, {"failed-target"});
    store.abort(aborted_token);
    expect(
        !fs::exists(active_root(directory) / aborted_token) &&
            fs::is_directory(used_root(directory) / aborted_token),
        "abort did not retire exact transaction state");
    expect_failure(
        [&]() { store.prepare(aborted_token, {"failed-target"}); },
        "aborted token was reusable");
}

void test_state_store_security_negatives() {
    TemporaryDirectory directory;
    TrustedAlpmReceiptStateStore store = open_test_store(directory);

    const fs::path outside = directory.path() / "outside";
    write_regular_file(outside, "outside-safe\n");
    const std::string symlink_token = transaction_token('f');
    fs::create_symlink(outside, active_root(directory) / symlink_token);
    expect_failure(
        [&]() { store.prepare(symlink_token, {"target"}); },
        "transaction-path symlink was followed");
    expect(
        std::ifstream(outside).peek() == 'o',
        "transaction symlink changed the outside file");
    fs::remove(active_root(directory) / symlink_token);

    const std::string receipt_symlink_token = transaction_token('1');
    store.prepare(receipt_symlink_token, {"target"});
    fs::create_symlink(
        outside,
        active_root(directory) / receipt_symlink_token / "receipt");
    expect_failure(
        [&]() {
            static_cast<void>(store.consume(receipt_symlink_token));
        },
        "receipt symlink was followed");
    expect(
        std::ifstream(outside).peek() == 'o',
        "receipt symlink changed the outside file");

    const std::string partial_symlink_token = transaction_token('9');
    store.prepare(partial_symlink_token, {"target"});
    fs::create_symlink(
        outside,
        active_root(directory) / partial_symlink_token /
            "receipt.partial");
    expect_failure(
        [&]() {
            record_input(
                store, directory, partial_symlink_token,
                "target\n");
        },
        "partial-receipt symlink was followed or replaced");
    expect(
        std::ifstream(outside).peek() == 'o',
        "partial-receipt symlink changed the outside file");

    const std::string hook_symlink_token = transaction_token('a');
    store.prepare(hook_symlink_token, {"target"});
    const fs::path hook_path =
        active_root(directory) / hook_symlink_token / "hooks" /
        trusted_alpm_receipt_hook_filename(hook_symlink_token);
    fs::remove(hook_path);
    fs::create_symlink(outside, hook_path);
    expect_failure(
        [&]() {
            record_input(
                store, directory, hook_symlink_token, "target\n");
        },
        "transaction hook symlink was followed");
    expect(
        std::ifstream(outside).peek() == 'o',
        "transaction hook symlink changed the outside file");

    const std::string partial_token = transaction_token('2');
    store.prepare(partial_token, {"target"});
    write_regular_file(
        active_root(directory) / partial_token / "receipt.partial",
        "truncated");
    expect_failure(
        [&]() { static_cast<void>(store.consume(partial_token)); },
        "partial receipt became Complete");
    store.abort(partial_token);

    const std::string truncated_final_token = transaction_token('b');
    store.prepare(truncated_final_token, {"target"});
    write_regular_file(
        active_root(directory) / truncated_final_token / "receipt",
        "MOGUET-ALPM-RECEIPT\t1\n");
    expect_failure(
        [&]() {
            static_cast<void>(store.consume(truncated_final_token));
        },
        "truncated final receipt became Complete");
    store.abort(truncated_final_token);

    const std::string wrong_mode_token = transaction_token('3');
    store.prepare(wrong_mode_token, {"target"});
    record_input(store, directory, wrong_mode_token, "target\n");
    fs::permissions(
        active_root(directory) / wrong_mode_token / "receipt",
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::group_read,
        fs::perm_options::replace);
    expect_failure(
        [&]() { static_cast<void>(store.consume(wrong_mode_token)); },
        "wrong-mode receipt was consumed");

    const std::string duplicate_record_token = transaction_token('4');
    store.prepare(duplicate_record_token, {"target"});
    record_input(store, directory, duplicate_record_token, "target\n");
    expect_failure(
        [&]() {
            record_input(
                store, directory, duplicate_record_token, "target\n");
        },
        "duplicate receipt publication was accepted");
    store.abort(duplicate_record_token);

    for(const std::pair<char, std::string>& invalid_input : {
            std::pair<char, std::string>{'5', "same\nsame\n"},
            {'6', "../x\n"},
            {'7', "target"},
        }) {
        const std::string token = transaction_token(invalid_input.first);
        store.prepare(token, {"target"});
        expect_failure(
            [&]() {
                record_input(
                    store, directory, token, invalid_input.second);
            },
            "invalid NeedsTargets input was recorded");
        store.abort(token);
    }

    const std::string unexpected_token = transaction_token('8');
    store.prepare(unexpected_token, {"target"});
    write_regular_file(
        active_root(directory) / unexpected_token / "unexpected",
        "unexpected\n");
    expect_failure(
        [&]() { static_cast<void>(store.consume(unexpected_token)); },
        "unexpected transaction state was ignored");
    expect_failure(
        [&]() { store.abort(unexpected_token); },
        "abort hid unexpected transaction state");

    TemporaryDirectory wrong_owner_directory;
    expect_failure(
        [&]() {
            static_cast<void>(open_test_store(
                wrong_owner_directory,
                static_cast<uid_t>(geteuid() + 1)));
        },
        "wrong-owner runtime root was accepted");
}

enum class ExpectedProcessKind {
    Capture,
    Run,
};

struct ExpectedProcess {
    ExpectedProcessKind kind;
    ExplicitProcessInvocation invocation;
    CapturedCommandResult capture_result;
    ExplicitProcessExecutionResult run_result{
        ExplicitProcessExecutionStatus::StartedKnownOutcome, 0};
};

std::deque<ExpectedProcess> expected_processes;

bool same_invocation(
    const ExplicitProcessInvocation& lhs,
    const ExplicitProcessInvocation& rhs) {
    return lhs.executable == rhs.executable &&
           lhs.arguments == rhs.arguments &&
           lhs.environment == rhs.environment &&
           lhs.stdout_capture_limit == rhs.stdout_capture_limit &&
           lhs.working_directory_fd == rhs.working_directory_fd &&
           lhs.standard_input_fd == rhs.standard_input_fd &&
           lhs.parent_independent_lifetime_guard_fd ==
               rhs.parent_independent_lifetime_guard_fd;
}

ExplicitProcessInvocation expected_helper(
    const std::string& command, const std::string& token,
    std::vector<std::string> trailing = {},
    std::optional<std::size_t> capture_limit = std::nullopt) {
    ExplicitProcessInvocation invocation;
    invocation.executable = "/usr/bin/sudo";
    invocation.arguments = {
        "--", MOGUET_ALPM_RECEIPT_HELPER_PATH, command, token,
        std::string(
            trusted_alpm_receipt_selected_repository_provider_owner())};
    invocation.arguments.insert(
        invocation.arguments.end(), trailing.begin(), trailing.end());
    invocation.environment = {"PATH=/usr/bin", "LC_ALL=C"};
    invocation.stdout_capture_limit = capture_limit;
    return invocation;
}

ExplicitProcessInvocation expected_pacman(
    const std::string& token, bool no_confirm = true) {
    ExplicitProcessInvocation invocation;
    invocation.executable = "/usr/bin/sudo";
    invocation.arguments = {
        "--", "/usr/bin/pacman", "-S", "--asdeps", "--needed"};
    if(no_confirm) invocation.arguments.push_back("--noconfirm");
    invocation.arguments.insert(
        invocation.arguments.end(),
        {"--hookdir", trusted_alpm_receipt_hook_directory(token), "--",
         "core/requested-target"});
    invocation.environment = {"PATH=/usr/bin", "LC_ALL=C"};
    return invocation;
}

void expect_capture(
    ExplicitProcessInvocation invocation,
    CapturedCommandResult result) {
    expected_processes.push_back(ExpectedProcess{
        ExpectedProcessKind::Capture, std::move(invocation), std::move(result), {}});
}

void expect_run(ExplicitProcessInvocation invocation, int status) {
    expected_processes.push_back(ExpectedProcess{
        ExpectedProcessKind::Run, std::move(invocation), {}, {ExplicitProcessExecutionStatus::StartedKnownOutcome, status}});
}

void expect_run_outcome(
    ExplicitProcessInvocation invocation,
    ExplicitProcessExecutionStatus status) {
    expected_processes.push_back(ExpectedProcess{
        ExpectedProcessKind::Run, std::move(invocation), {}, {status, std::nullopt}});
}

TrustedAlpmReceiptSelectedProviderRequest selected_provider_request() {
    return TrustedAlpmReceiptSelectedProviderRequest{
        {{"core", "requested-target"}},
        TrustedAlpmReceiptRepositoryInstallDirective::AsDependency,
        true};
}

void require_processes_consumed() {
    expect(expected_processes.empty(), "transport process expectations remain");
}

void test_transport_complete_and_solver_introduced_projection() {
    expected_processes.clear();
    const std::string token = transaction_token('9');
    expect_capture(
        expected_helper(
            "prepare", token, {"--", "requested-target"}, 4096),
        CapturedCommandResult{
            serialize_trusted_alpm_receipt_prepare_response(
                TrustedAlpmReceiptPrepareResponse{
                    token,
                    trusted_alpm_receipt_hook_directory(token)}),
            0, false});
    expect_run(expected_pacman(token), 0);
    expect_capture(
        expected_helper(
            "consume", token, {},
            TRUSTED_ALPM_RECEIPT_MAXIMUM_BYTES),
        CapturedCommandResult{
            serialize_trusted_alpm_receipt_machine_receipt(
                TrustedAlpmReceiptMachineReceipt{
                    TrustedAlpmReceiptMachineState::Complete,
                    token,
                    {"requested-target",
                     "solver-introduced"}}),
            0, false});
    const TrustedAlpmReceiptCaptureResult result =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            selected_provider_request(), token);
    require_processes_consumed();
    expect(
        result.status == TrustedAlpmReceiptCaptureStatus::Complete &&
            result.pacman_exit_status == 0 &&
            result.transaction_ledger.transactions.size() == 1 &&
            result.transaction_ledger.transactions[0]
                    .requested_package_names ==
                std::vector<std::string>{"requested-target"} &&
            result.transaction_ledger.transactions[0]
                .receipt.contains_newly_installed_package(
                    "solver-introduced"),
        "trusted transport did not preserve requested vs actual Install set");

    const CleanupCausalOwnership causal =
        project_cleanup_causal_ownership_from_raw_ledger_for_test(
            "solver-introduced",
            CleanupBaselineObservation::NewlyObserved,
            CleanupCurrentPackageEvidence{
                CleanupInstalledState::Present,
                InstalledPackageMetadata{
                    "solver-introduced", "1-1",
                    InstalledPackageReason::Dependency},
                CleanupEvidenceVerification::Verified},
            result.transaction_ledger);
    expect(
        causal == CleanupCausalOwnership::InvocationOwned,
        "complete trusted solver Install did not reach InvocationOwned");
}

void test_transport_external_race_and_failures() {
    const TrustedAlpmReceiptSelectedProviderRequest request =
        selected_provider_request();

    expected_processes.clear();
    const std::string missing_token = transaction_token('a');
    expect_capture(
        expected_helper(
            "prepare", missing_token,
            {"--", "requested-target"}, 4096),
        {serialize_trusted_alpm_receipt_prepare_response(
             {missing_token,
              trusted_alpm_receipt_hook_directory(missing_token)}),
         0, false});
    expect_run(expected_pacman(missing_token), 0);
    expect_capture(
        expected_helper(
            "consume", missing_token, {},
            TRUSTED_ALPM_RECEIPT_MAXIMUM_BYTES),
        {serialize_trusted_alpm_receipt_machine_receipt(
             {TrustedAlpmReceiptMachineState::Missing,
              missing_token,
              {}}),
         0, false});
    const auto missing =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            request, missing_token);
    require_processes_consumed();
    expect(
        missing.status == TrustedAlpmReceiptCaptureStatus::Missing &&
            project_cleanup_causal_ownership_from_raw_ledger_for_test(
                "requested-target",
                CleanupBaselineObservation::NewlyObserved,
                CleanupCurrentPackageEvidence{
                    CleanupInstalledState::Present,
                    InstalledPackageMetadata{
                        "requested-target", "1-1",
                        InstalledPackageReason::
                            Dependency},
                    CleanupEvidenceVerification::Verified},
                missing.transaction_ledger) ==
                CleanupCausalOwnership::Unknown,
        "external pre-install race became InvocationOwned without Install");

    expected_processes.clear();
    const std::string failed_token = transaction_token('b');
    expect_capture(
        expected_helper(
            "prepare", failed_token,
            {"--", "requested-target"}, 4096),
        {serialize_trusted_alpm_receipt_prepare_response(
             {failed_token,
              trusted_alpm_receipt_hook_directory(failed_token)}),
         0, false});
    expect_run(expected_pacman(failed_token), 73);
    expect_run(expected_helper("abort", failed_token), 0);
    const auto failed =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            request, failed_token);
    require_processes_consumed();
    expect(
        failed.status == TrustedAlpmReceiptCaptureStatus::PacmanFailed &&
            failed.transaction_ledger.transactions[0]
                    .command_outcome ==
                InvocationDependencyTransactionCommandOutcome::
                    Failed &&
            project_cleanup_causal_ownership_from_raw_ledger_for_test(
                "requested-target",
                CleanupBaselineObservation::NewlyObserved,
                CleanupCurrentPackageEvidence{
                    CleanupInstalledState::Present,
                    InstalledPackageMetadata{
                        "requested-target", "1-1",
                        InstalledPackageReason::
                            Dependency},
                    CleanupEvidenceVerification::Verified},
                failed.transaction_ledger) ==
                CleanupCausalOwnership::Unknown,
        "failed command produced InvocationOwned evidence");

    expected_processes.clear();
    const std::string post_launch_unknown_token = transaction_token('0');
    expect_capture(
        expected_helper(
            "prepare", post_launch_unknown_token,
            {"--", "requested-target"}, 4096),
        {serialize_trusted_alpm_receipt_prepare_response(
             {post_launch_unknown_token,
              trusted_alpm_receipt_hook_directory(
                  post_launch_unknown_token)}),
         0, false});
    expect_run_outcome(
        expected_pacman(post_launch_unknown_token),
        ExplicitProcessExecutionStatus::StartedOutcomeUnknown);
    expect_run(
        expected_helper("abort", post_launch_unknown_token), 0);
    const auto post_launch_unknown =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            request, post_launch_unknown_token);
    require_processes_consumed();
    expect(
        post_launch_unknown.status ==
                TrustedAlpmReceiptCaptureStatus::OutcomeUnknown &&
            !post_launch_unknown.pacman_exit_status.has_value() &&
            post_launch_unknown.transaction_ledger.transactions.size() ==
                1 &&
            post_launch_unknown.transaction_ledger.transactions.front()
                    .command_outcome ==
                InvocationDependencyTransactionCommandOutcome::Unknown &&
            post_launch_unknown.transaction_ledger.transactions.front()
                    .receipt.state() !=
                PacmanTransactionReceiptState::Complete,
        "post-launch unknown outcome was flattened into NotAttempted");

    expected_processes.clear();
    const std::string pre_launch_failure_token = transaction_token('3');
    expect_capture(
        expected_helper(
            "prepare", pre_launch_failure_token,
            {"--", "requested-target"}, 4096),
        {serialize_trusted_alpm_receipt_prepare_response(
             {pre_launch_failure_token,
              trusted_alpm_receipt_hook_directory(
                  pre_launch_failure_token)}),
         0, false});
    expect_run_outcome(
        expected_pacman(pre_launch_failure_token),
        ExplicitProcessExecutionStatus::NotStarted);
    expect_run(expected_helper("abort", pre_launch_failure_token), 0);
    const auto pre_launch_failure =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            request, pre_launch_failure_token);
    require_processes_consumed();
    expect(
        pre_launch_failure.status ==
                TrustedAlpmReceiptCaptureStatus::PrepareFailed &&
            !pre_launch_failure.pacman_exit_status.has_value() &&
            pre_launch_failure.transaction_ledger.transactions.size() ==
                1 &&
            pre_launch_failure.transaction_ledger.transactions.front()
                    .command_outcome ==
                InvocationDependencyTransactionCommandOutcome::
                    NotAttempted,
        "confirmed pre-launch failure did not remain NotAttempted");

    expected_processes.clear();
    const std::string malformed_token = transaction_token('c');
    expect_capture(
        expected_helper(
            "prepare", malformed_token,
            {"--", "requested-target"}, 4096),
        {serialize_trusted_alpm_receipt_prepare_response(
             {malformed_token,
              trusted_alpm_receipt_hook_directory(malformed_token)}),
         0, false});
    expect_run(expected_pacman(malformed_token), 0);
    expect_capture(
        expected_helper(
            "consume", malformed_token, {},
            TRUSTED_ALPM_RECEIPT_MAXIMUM_BYTES),
        {"MOGUET-ALPM-RECEIPT\t1\n", 0, false});
    const auto malformed =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            request, malformed_token);
    require_processes_consumed();
    expect(
        malformed.status ==
                TrustedAlpmReceiptCaptureStatus::MalformedReceipt &&
            malformed.transaction_ledger.transactions[0]
                    .receipt.state() !=
                PacmanTransactionReceiptState::Complete,
        "malformed receipt became Complete");

    expected_processes.clear();
    const std::string prepare_failure_token = transaction_token('d');
    expect_capture(
        expected_helper(
            "prepare", prepare_failure_token,
            {"--", "requested-target"}, 4096),
        {"", 1, false});
    const auto prepare_failure =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            request, prepare_failure_token);
    require_processes_consumed();
    expect(
        prepare_failure.status ==
                TrustedAlpmReceiptCaptureStatus::PrepareFailed &&
            !prepare_failure.pacman_exit_status.has_value(),
        "missing PREPARE reached pacman");

    expected_processes.clear();
    const std::string consume_failure_token = transaction_token('e');
    expect_capture(
        expected_helper(
            "prepare", consume_failure_token,
            {"--", "requested-target"}, 4096),
        {serialize_trusted_alpm_receipt_prepare_response(
             {consume_failure_token,
              trusted_alpm_receipt_hook_directory(
                  consume_failure_token)}),
         0, false});
    expect_run(expected_pacman(consume_failure_token), 0);
    expect_capture(
        expected_helper(
            "consume", consume_failure_token, {},
            TRUSTED_ALPM_RECEIPT_MAXIMUM_BYTES),
        {"", 61, false});
    expect_run(expected_helper("abort", consume_failure_token), 0);
    const auto consume_failure =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            request, consume_failure_token);
    require_processes_consumed();
    expect(
        consume_failure.status ==
                TrustedAlpmReceiptCaptureStatus::ConsumeFailed &&
            consume_failure.transaction_ledger.transactions[0]
                    .receipt.state() !=
                PacmanTransactionReceiptState::Complete,
        "consume failure exposed a Complete receipt");

    expected_processes.clear();
    const std::string abort_failure_token = transaction_token('f');
    expect_capture(
        expected_helper(
            "prepare", abort_failure_token,
            {"--", "requested-target"}, 4096),
        {serialize_trusted_alpm_receipt_prepare_response(
             {abort_failure_token,
              trusted_alpm_receipt_hook_directory(
                  abort_failure_token)}),
         0, false});
    expect_run(expected_pacman(abort_failure_token), 71);
    expect_run(expected_helper("abort", abort_failure_token), 72);
    const auto abort_failure =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            request, abort_failure_token);
    require_processes_consumed();
    expect(
        abort_failure.status ==
                TrustedAlpmReceiptCaptureStatus::AbortFailed &&
            abort_failure.pacman_exit_status == 71,
        "exact abort failure was hidden");

    expected_processes.clear();
    const std::string wrong_token = transaction_token('1');
    expect_capture(
        expected_helper(
            "prepare", wrong_token,
            {"--", "requested-target"}, 4096),
        {serialize_trusted_alpm_receipt_prepare_response(
             {wrong_token,
              trusted_alpm_receipt_hook_directory(wrong_token)}),
         0, false});
    expect_run(expected_pacman(wrong_token), 0);
    expect_capture(
        expected_helper(
            "consume", wrong_token, {},
            TRUSTED_ALPM_RECEIPT_MAXIMUM_BYTES),
        {serialize_trusted_alpm_receipt_machine_receipt(
             {TrustedAlpmReceiptMachineState::Complete,
              transaction_token('2'),
              {"requested-target"}}),
         0, false});
    const auto wrong_token_result =
        execute_trusted_alpm_receipt_selected_provider_transaction_for_test(
            request, wrong_token);
    require_processes_consumed();
    expect(
        wrong_token_result.status ==
                TrustedAlpmReceiptCaptureStatus::MalformedReceipt &&
            wrong_token_result.transaction_ledger.transactions[0]
                    .receipt.state() !=
                PacmanTransactionReceiptState::Complete,
        "wrong-token consume response became authoritative");
}

void test_getrandom_token_generation() {
    const std::optional<std::string> first =
        generate_trusted_alpm_receipt_transaction_token();
    const std::optional<std::string> second =
        generate_trusted_alpm_receipt_transaction_token();
    expect(
        first.has_value() && second.has_value() &&
            is_valid_pacman_transaction_token(*first) &&
            is_valid_pacman_transaction_token(*second) &&
            *first != *second,
        "getrandom token generation was not canonical and unique");
}

} // namespace

namespace {
std::size_t g_observed_process_calls = 0;
}

std::size_t cleanup_test_observed_process_calls() {
    return g_observed_process_calls;
}

CapturedCommandResult capture_explicit_process_output_raw(
    const ExplicitProcessInvocation& invocation,
    bool suppress_standard_error) {
    ++g_observed_process_calls;
    expect(
        !suppress_standard_error && !expected_processes.empty() &&
            expected_processes.front().kind ==
                ExpectedProcessKind::Capture &&
            same_invocation(
                expected_processes.front().invocation,
                invocation),
        "unexpected trusted transport capture argv/environment");
    ExpectedProcess expected = std::move(expected_processes.front());
    expected_processes.pop_front();
    return std::move(expected.capture_result);
}

int run_explicit_process(
    const ExplicitProcessInvocation& invocation,
    bool suppress_standard_output,
    bool suppress_standard_error) {
    ++g_observed_process_calls;
    expect(
        !suppress_standard_output && !suppress_standard_error &&
            !expected_processes.empty() &&
            expected_processes.front().kind ==
                ExpectedProcessKind::Run &&
            same_invocation(
                expected_processes.front().invocation,
                invocation),
        "unexpected trusted transport run argv/environment");
    ExpectedProcess expected = std::move(expected_processes.front());
    expected_processes.pop_front();
    expect(
        expected.run_result.status ==
                ExplicitProcessExecutionStatus::StartedKnownOutcome &&
            expected.run_result.exit_code.has_value(),
        "legacy run stub received a non-known process outcome");
    return expected.run_result.exit_code.value();
}

ExplicitProcessExecutionResult run_explicit_process_with_outcome(
    const ExplicitProcessInvocation& invocation,
    bool suppress_standard_output,
    bool suppress_standard_error) noexcept {
    ++g_observed_process_calls;
    try {
        expect(
            !suppress_standard_output && !suppress_standard_error &&
                !expected_processes.empty() &&
                expected_processes.front().kind ==
                    ExpectedProcessKind::Run &&
                same_invocation(
                    expected_processes.front().invocation,
                    invocation),
            "unexpected trusted transport outcome-aware run argv/environment");
        ExpectedProcess expected = std::move(expected_processes.front());
        expected_processes.pop_front();
        return expected.run_result;
    } catch(...) {
        return ExplicitProcessExecutionResult{
            ExplicitProcessExecutionStatus::NotStarted,
            std::nullopt};
    }
}

void run_trusted_alpm_receipt_tests() {
    test_protocol_rejects_capability_injection();
    test_protocol_completion_and_truncation();
    test_state_store_complete_missing_abort_and_replay();
    test_state_store_security_negatives();
    test_transport_complete_and_solver_introduced_projection();
    test_transport_external_race_and_failures();
    test_getrandom_token_generation();
}
