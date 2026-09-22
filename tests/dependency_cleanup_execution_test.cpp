#include "dependency_cleanup_execution.hpp"
#include "app_config.hpp"
#include "invocation_owned_cleanup_adapter.hpp"
#include "process.hpp"
#include "stubs/package-metadata/alpm_stub.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <type_traits>

RemoteAurCleanupCollectionResult make_cleanup_execution_test_collection(std::size_t count);

namespace {
namespace stub = package_metadata_test_stub;
using Status = DependencyCleanupCandidateRevalidationStatus;
using State = DependencyCleanupRevalidationState;
std::size_t g_configuration_reads = 0;
bool g_configuration_failure = false;
std::size_t g_hold_package_queries = 0;
CapturedCommandResult g_hold_package_result{{}, 0};
std::vector<std::string> g_removal_commands;
int g_removal_exit_status = 0;
bool g_removal_throws = false;

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

DependencyCleanupInteractionResult approve(const RemoteAurCleanupCollectionResult& collection,
                                           const std::string& answer = "y\n", bool no_confirm = false, bool tty = true) {
    AppConfig config;
    config.no_confirm = no_confirm;
    std::istringstream input(answer);
    std::ostringstream output;
    auto result = interact_dependency_cleanup(make_dependency_cleanup_preview(collection), config, tty, input, output);
    if(result.status() == DependencyCleanupInteractionStatus::Approved) {
        const auto rendered = output.str();
        const auto question = rendered.find("Remove build dependencies?");
        expect(question != std::string::npos && rendered.find("Remove build dependencies?", question + 1) == std::string::npos,
               "explicit approval did not use exactly one cleanup prompt");
    }
    return result;
}

std::vector<stub::LocalPackageMetadata> installed(const DependencyCleanupApprovalSnapshot& approval) {
    std::vector<stub::LocalPackageMetadata> packages;
    for(const auto& candidate : approval.preview().eligible_candidates()) {
        packages.push_back({candidate.expected_installed().name, candidate.expected_installed().version, ALPM_PKG_REASON_DEPEND});
    }
    packages.push_back({"unapproved-extra", "1-1", ALPM_PKG_REASON_DEPEND});
    packages.push_back({"base-devel", "1-1", ALPM_PKG_REASON_EXPLICIT, {}, {{"unrelated-tool", std::nullopt, ALPM_DEP_MOD_ANY}}});
    return packages;
}

void fresh(const DependencyCleanupApprovalSnapshot& approval, const std::vector<stub::LocalPackageMetadata>& packages) {
    stub::reset_alpm_stub();
    g_configuration_failure = false;
    g_hold_package_result = {{}, 0};
    stub::set_local_packages(packages);
    stub::use_local_package_cache_for_queries();
    for(std::size_t index = 0; index < packages.size(); ++index) {
        for(const auto& candidate : approval.preview().eligible_candidates()) {
            if(packages[index].name == candidate.expected_installed().name) {
                stub::set_local_package_base(index, *candidate.expected_installed().package_base.value());
                stub::set_local_package_architecture(index, *candidate.expected_installed().architecture.value());
            }
        }
    }
}

std::vector<std::string> ready_names(const DependencyCleanupRevalidationResult& result) {
    std::vector<std::string> names;
    for(const auto& candidate : result.candidates()) {
        if(candidate.status == Status::Ready) names.push_back(candidate.approved.expected_installed().name);
    }
    return names;
}

void test_fresh_revalidation_matrix() {
    const auto collection = make_cleanup_execution_test_collection(1);
    const auto interaction = approve(collection);
    expect(interaction.approved_snapshot().has_value(), "collector fixture failed to approve");
    const auto& approval = *interaction.approved_snapshot();
    const auto packages = installed(approval);
    enum class Change { None,
                        Absent,
                        Version,
                        Base,
                        Architecture,
                        UnknownArchitecture,
                        UnknownBase,
                        Explicit,
                        UnknownReason,
                        Protected,
                        PolicyUnknown,
                        PolicyFailure,
                        StillRequired,
                        RuntimeUnknown,
                        SnapshotFailure,
                        OpenFailure,
                        ConfigurationFailure,
                        InvalidSnapshot };
    for(const auto change : {Change::None, Change::Absent, Change::Version, Change::Base, Change::Architecture,
                             Change::UnknownArchitecture, Change::UnknownBase, Change::Explicit, Change::UnknownReason,
                             Change::Protected, Change::PolicyUnknown, Change::PolicyFailure, Change::StillRequired,
                             Change::RuntimeUnknown, Change::SnapshotFailure, Change::OpenFailure, Change::ConfigurationFailure,
                             Change::InvalidSnapshot}) {
        auto current = packages;
        if(change == Change::Absent) current.erase(current.begin());
        if(change == Change::Version) current[0].version = "1.0-2";
        if(change == Change::Explicit) current[0].reason = ALPM_PKG_REASON_EXPLICIT;
        if(change == Change::UnknownReason) current[0].reason = static_cast<alpm_pkgreason_t>(999);
        if(change == Change::Protected) current.back().dependencies = {{current[0].name, std::nullopt, ALPM_DEP_MOD_ANY}};
        if(change == Change::PolicyUnknown) current.back().dependencies.clear();
        if(change == Change::StillRequired) current[1].dependencies = {{current[0].name, std::nullopt, ALPM_DEP_MOD_ANY}};
        if(change == Change::RuntimeUnknown) current[1].dependencies = {{std::nullopt, std::nullopt, ALPM_DEP_MOD_ANY}};
        fresh(approval, current);
        if(change == Change::Base) stub::set_local_package_base(0, "changed-base");
        if(change == Change::Architecture) stub::set_local_package_architecture(0, "aarch64");
        if(change == Change::UnknownArchitecture) stub::set_local_package_architecture_null(0);
        if(change == Change::UnknownBase) stub::set_local_package_base_null(0);
        if(change == Change::PolicyFailure) stub::enqueue_local_package_query_failure(current[0].name);
        if(change == Change::SnapshotFailure) stub::set_package_cache_failure();
        if(change == Change::OpenFailure) stub::set_initialize_failure(ALPM_ERR_DB_OPEN);
        if(change == Change::ConfigurationFailure) g_configuration_failure = true;
        if(change == Change::InvalidSnapshot) stub::set_local_package_version_null(0);
        const auto reads = g_configuration_reads;
        const auto hold_reads = g_hold_package_queries;
        const auto result = revalidate_dependency_cleanup(approval);
        expect(g_configuration_reads > reads, "revalidation reused old configuration authority");
        expect(g_hold_package_queries == hold_reads + (change == Change::ConfigurationFailure ? 0U : 1U), "HoldPkg was not queried exactly once per fresh read phase");
        expect(result.candidates().size() == 1 && result.candidates()[0].approved == approval.preview().eligible_candidates()[0],
               "revalidation changed approval binding or discovered an extra package");
        auto expected = Status::Unknown;
        if(change == Change::None) expected = Status::Ready;
        if(change == Change::Absent) expected = Status::AlreadyAbsent;
        if(change == Change::Version || change == Change::Base || change == Change::Architecture) expected = Status::IdentityChanged;
        if(change == Change::Explicit) expected = Status::InstallReasonChanged;
        if(change == Change::Protected) expected = Status::Protected;
        if(change == Change::StillRequired) expected = Status::StillRequired;
        expect(result.candidates()[0].status == expected, "fresh negative has incorrect typed skip reason: change=" + std::to_string(static_cast<int>(change)) + " status=" + std::to_string(static_cast<int>(result.candidates()[0].status)));
        const bool blocked = change == Change::SnapshotFailure || change == Change::OpenFailure ||
                             change == Change::ConfigurationFailure || change == Change::InvalidSnapshot;
        expect(result.state() == (blocked ? State::Blocked : change == Change::None ? State::Ready
                                                                                    : State::NoCandidatesReady),
               "fresh negative has incorrect aggregate state");
        expect(ready_names(result).size() == (change == Change::None ? 1U : 0U), "unsafe candidate became ready");
        if(change != Change::None) {
            if(change == Change::PolicyFailure) stub::enqueue_local_package_query_failure(current[0].name);
            g_removal_commands.clear();
            const auto execution = execute_dependency_cleanup(interaction);
            expect(execution.status != DependencyCleanupExecutionStatus::Removed && execution.attempted.empty() &&
                       g_removal_commands.empty(),
                   "fresh negative reached external removal");
        }

        if(change == Change::None) {
            expect(stub::initialize_call_count() == 2 && stub::release_call_count() == 2,
                   "identity/runtime must share one fresh session plus fresh policy session");
            expect(result.candidates()[0].current == approval.preview().eligible_candidates()[0].expected_installed(),
                   "fresh exact installed facts not retained");
        }
    }
    expect(collection.invocation_result().is_success(), "revalidation changed completed build/install result");
}

void test_shrink_and_retained_consumer_closure() {
    const auto collection = make_cleanup_execution_test_collection(3);
    const auto interaction = approve(collection);
    expect(interaction.approved_snapshot().has_value(), "three-candidate fixture failed to approve");
    const auto& approval = *interaction.approved_snapshot();
    auto packages = installed(approval);
    packages[1].reason = ALPM_PKG_REASON_EXPLICIT;
    packages[2].version = "3.0-2";
    fresh(approval, packages);
    auto result = revalidate_dependency_cleanup(approval);
    expect(result.state() == State::Ready && ready_names(result) == std::vector<std::string>{packages[0].name} &&
               result.candidates()[1].status == Status::InstallReasonChanged && result.candidates()[2].status == Status::IdentityChanged,
           "approved A/B/C did not shrink to exact safe A");
    // A depends on B depends on C. If A stays, both B and C must stay.
    packages = installed(approval);
    packages[0].reason = ALPM_PKG_REASON_EXPLICIT;
    packages[0].dependencies = {{packages[1].name, std::nullopt, ALPM_DEP_MOD_ANY}};
    packages[1].dependencies = {{packages[2].name, std::nullopt, ALPM_DEP_MOD_ANY}};
    fresh(approval, packages);
    result = revalidate_dependency_cleanup(approval);
    expect(result.state() == State::NoCandidatesReady && result.candidates()[1].status == Status::StillRequired &&
               result.candidates()[2].status == Status::StillRequired,
           "skipped candidate dependency closure was unsafe");
    packages[0].reason = ALPM_PKG_REASON_DEPEND;
    fresh(approval, packages);
    result = revalidate_dependency_cleanup(approval);
    expect(ready_names(result) == std::vector<std::string>{packages[0].name, packages[1].name, packages[2].name},
           "batch-internal dependencies changed approval order or blocked a safe batch");
    // A candidate-local policy failure does not erase an independent safe one.
    packages = installed(approval);
    packages[2].provides = {{std::nullopt, std::nullopt, ALPM_DEP_MOD_ANY}};
    fresh(approval, packages);
    result = revalidate_dependency_cleanup(approval);
    expect(ready_names(result) == std::vector<std::string>{packages[0].name, packages[1].name} &&
               result.candidates()[2].status == Status::Unknown,
           "candidate-local metadata failure blocked independent candidates");
}
void test_exact_executor_and_composition() {
    using Execution = DependencyCleanupExecutionStatus;
    for(const std::size_t count : {1U, 2U}) {
        const auto collection = make_cleanup_execution_test_collection(count);
        const auto interaction = approve(collection);
        const auto& approval = *interaction.approved_snapshot();
        const auto packages = installed(approval);
        const std::string expected = count == 1
                                         ? "'sudo' 'pacman' '-R' '--noconfirm' '--' 'collector-dependency'"
                                         : "'sudo' 'pacman' '-R' '--noconfirm' '--' 'collector-dependency' 'aaa-second'";
        for(const auto exit_status : {0, 23}) {
            fresh(approval, packages);
            g_removal_commands.clear();
            g_removal_exit_status = exit_status;
            const auto result = execute_dependency_cleanup(interaction);
            expect(result.status == (exit_status == 0 ? Execution::Removed : Execution::RemovalFailed) &&
                       result.removal_exit_status == exit_status && result.attempted == approval.preview().eligible_candidates(),
                   "batch result lost exact attempted set or failure status");
            expect(g_removal_commands == std::vector<std::string>{expected},
                   "executor changed exact argv/order or performed a retry");
            for(const std::string forbidden : {"-Rs", "-Rns", "-Rc", "-Rdd", "--nodeps", "--cascade", "--recursive", "Qdt", "autoremove", "unapproved-extra"}) {
                expect(g_removal_commands[0].find(forbidden) == std::string::npos, "broad option or unapproved operand reached removal");
            }
            expect(collection.invocation_result().is_success(), "cleanup failure changed completed build/install success");
        }
        fresh(approval, packages);
        g_removal_commands.clear();
        g_removal_throws = true;
        const auto unknown = execute_dependency_cleanup(interaction);
        g_removal_throws = false;
        expect(unknown.status == Execution::RemovalFailed && !unknown.removal_exit_status &&
                   unknown.attempted == approval.preview().eligible_candidates() && g_removal_commands == std::vector<std::string>{expected},
               "unknown process outcome lost attempted set or retried");
        g_removal_exit_status = 0;
    }
    const auto collection = make_cleanup_execution_test_collection(2);
    const auto yes = approve(collection);
    const auto& approval = *yes.approved_snapshot();
    auto packages = installed(approval);
    packages[1].reason = ALPM_PKG_REASON_EXPLICIT;
    fresh(approval, packages);
    g_removal_commands.clear();
    const auto partial = execute_dependency_cleanup(yes);
    expect(partial.status == Execution::Removed && partial.attempted.size() == 1 &&
               partial.revalidation->candidates()[1].status == Status::InstallReasonChanged &&
               g_removal_commands == std::vector<std::string>{"'sudo' 'pacman' '-R' '--noconfirm' '--' 'collector-dependency'"},
           "partial cleanup success flattened skipped reason or expanded removal");
    packages = installed(approval);
    packages.erase(packages.begin());
    fresh(approval, packages);
    g_removal_commands.clear();
    const auto absent = execute_dependency_cleanup(yes);
    expect(absent.status == Execution::Removed && absent.attempted.size() == 1 &&
               absent.revalidation->candidates()[0].status == Status::AlreadyAbsent &&
               g_removal_commands == std::vector<std::string>{"'sudo' 'pacman' '-R' '--noconfirm' '--' 'aaa-second'"},
           "already absent candidate reached removal argv");
    // Retaining a prior Ready observation cannot bypass a fresh reason change.
    packages = installed(approval);
    fresh(approval, packages);
    const auto earlier_ready = revalidate_dependency_cleanup(approval);
    expect(earlier_ready.state() == State::Ready, "stale-read fixture did not begin Ready");
    packages[0].reason = ALPM_PKG_REASON_EXPLICIT;
    packages[1].reason = ALPM_PKG_REASON_EXPLICIT;
    fresh(approval, packages);
    g_removal_commands.clear();
    const auto empty = execute_dependency_cleanup(yes);
    expect(empty.status == Execution::NoCandidatesReady && empty.attempted.empty() && !empty.removal_exit_status && g_removal_commands.empty(),
           "empty fresh subset used old readiness or invoked process");
    fresh(approval, packages);
    stub::set_initialize_failure(ALPM_ERR_DB_OPEN);
    const auto blocked = execute_dependency_cleanup(yes);
    expect(blocked.status == Execution::Blocked && blocked.revalidation->state() == State::Blocked &&
               blocked.attempted.empty() && g_removal_commands.empty(),
           "blocked fresh authority invoked process");
    for(const auto& answer : std::vector<std::string>{"n\n", "\n", "q\n", "cancel\n", ""}) {
        const auto interaction = approve(collection, answer);
        const auto reads = g_configuration_reads;
        const auto hold_reads = g_hold_package_queries;
        const auto opens = stub::initialize_call_count();
        const auto result = execute_dependency_cleanup(interaction);
        expect(result.status == Execution::Blocked && !result.revalidation && result.attempted.empty() &&
                   g_configuration_reads == reads && g_hold_package_queries == hold_reads && stub::initialize_call_count() == opens && g_removal_commands.empty(),
               "No/cancel/EOF reached revalidation or executor");
    }
    for(const bool no_confirm : {true, false}) {
        const auto interaction = approve(collection, "y\n", no_confirm, no_confirm);
        const auto reads = g_configuration_reads;
        const auto hold_reads = g_hold_package_queries;
        const auto opens = stub::initialize_call_count();
        const auto result = execute_dependency_cleanup(interaction);
        expect(interaction.status() == DependencyCleanupInteractionStatus::InteractionUnavailable &&
                   interaction.unavailable_reason() == (no_confirm ? DependencyCleanupUnavailableReason::NoConfirm
                                                                   : DependencyCleanupUnavailableReason::NonInteractiveInput) &&
                   !interaction.approved_snapshot() && result.attempted.empty() &&
                   result.status == Execution::Blocked && !result.revalidation && g_configuration_reads == reads && g_hold_package_queries == hold_reads &&
                   stub::initialize_call_count() == opens && g_removal_commands.empty(),
               "noconfirm/non-TTY reached executor or fresh metadata");
    }
    expect(collection.invocation_result().is_success(), "skip/cancel changed build/install success");
}

void test_fresh_hold_package_protection() {
    using Execution = DependencyCleanupExecutionStatus;
    const auto collection = make_cleanup_execution_test_collection(2);
    const auto before_approval = g_hold_package_queries;
    const auto interaction = approve(collection);
    expect(g_hold_package_queries == before_approval, "HoldPkg observation moved before explicit approval");
    const auto& approval = *interaction.approved_snapshot();
    const auto packages = installed(approval);
    struct Case {
        std::string patterns;
        bool first_held;
        bool second_held;
    };
    for(const auto& test : std::vector<Case>{
            {"", false, false}, {"collector-dependency\n", true, false}, {"collector-dependency\naaa-second\n", true, true}, {"collector-*\n", true, false}, {"*-dependency\n", true, false}, {"collector-dependenc?\n", true, false}, {"collector-dependenc[yx]\n", true, false}, {"collector-dependenc[[:alpha:]]\n", true, false}, {"[!a]*\n", true, false}, {"collector-\\dependency\n", true, false}, {"linux-*\n", false, false}, {"collector\n", false, false}, {"{collector-dependency,aaa-second}\n", false, false}, {"@(collector-dependency|aaa-second)\n", false, false}, {"!collector-dependency\n", false, false}, {"unapproved-extra\ncollector-dependency\naaa-second\n", true, true}}) {
        fresh(approval, packages);
        g_hold_package_result = {test.patterns, 0};
        g_removal_commands.clear();
        const auto reads = g_hold_package_queries;
        const auto result = execute_dependency_cleanup(interaction);
        expect(g_hold_package_queries == reads + 1 && result.revalidation && result.revalidation->candidates().size() == 2,
               "HoldPkg was cached, queried per candidate, or expanded approved set");
        const auto& candidates = result.revalidation->candidates();
        expect(candidates[0].status == (test.first_held ? Status::Protected : Status::Ready) &&
                   candidates[1].status == (test.second_held ? Status::Protected : Status::Ready),
               "HoldPkg fnmatch(flags=0) semantics differ");
        std::string command = "'sudo' 'pacman' '-R' '--noconfirm' '--'";
        if(!test.first_held) command += " 'collector-dependency'";
        if(!test.second_held) command += " 'aaa-second'";
        if(test.first_held && test.second_held) {
            expect(result.status == Execution::NoCandidatesReady && result.attempted.empty() && g_removal_commands.empty(),
                   "all held candidates invoked removal");
        } else {
            expect(result.status == Execution::Removed && g_removal_commands == std::vector<std::string>{command},
                   "held package reached operands or safe subset lost fixed one-shot command");
        }
    }
    for(const auto& observation : std::vector<CapturedCommandResult>{{{}, 1}, {"collector-*\n", 1}, {"HoldPkg = collector-*\n", 0}, {"collector-*", 0}, {std::string("collector-*\0\n", 13), 0}, {{}, 0, true}}) {
        fresh(approval, packages);
        g_hold_package_result = observation;
        g_removal_commands.clear();
        const auto result = execute_dependency_cleanup(interaction);
        expect(result.status == Execution::Blocked && result.revalidation->state() == State::Blocked &&
                   result.revalidation->failure() && ready_names(*result.revalidation).empty() && result.attempted.empty() &&
                   g_removal_commands.empty(),
               "unavailable/malformed HoldPkg became empty policy or a partial removal");
    }
    // A previous read-only Ready result cannot bypass a newly configured HoldPkg.
    fresh(approval, packages);
    const auto old_ready = revalidate_dependency_cleanup(approval);
    expect(old_ready.state() == State::Ready, "freshness fixture did not begin Ready");
    g_hold_package_result = {"*\n", 0};
    g_removal_commands.clear();
    expect(execute_dependency_cleanup(interaction).status == Execution::NoCandidatesReady && g_removal_commands.empty(),
           "execution reused old HoldPkg evidence");

    const auto triple_collection = make_cleanup_execution_test_collection(3);
    const auto triple = approve(triple_collection);
    const auto& triple_approval = *triple.approved_snapshot();
    auto current = installed(triple_approval);
    current[1].reason = ALPM_PKG_REASON_EXPLICIT;
    fresh(triple_approval, current);
    g_hold_package_result = {"collector-dependency\n", 0};
    g_removal_commands.clear();
    const auto mixed = execute_dependency_cleanup(triple);
    expect(mixed.status == Execution::Removed && mixed.revalidation->candidates()[0].status == Status::Protected &&
               mixed.revalidation->candidates()[1].status == Status::InstallReasonChanged &&
               g_removal_commands == std::vector<std::string>{"'sudo' 'pacman' '-R' '--noconfirm' '--' 'third-dependency'"},
           "HoldPkg/Explicit mixed shrink did not preserve exact safe C");
    current = installed(triple_approval);
    current[0].dependencies = {{current[1].name, std::nullopt, ALPM_DEP_MOD_ANY}};
    current[1].dependencies = {{current[2].name, std::nullopt, ALPM_DEP_MOD_ANY}};
    fresh(triple_approval, current);
    g_hold_package_result = {"collector-*\n", 0};
    g_removal_commands.clear();
    const auto chain = execute_dependency_cleanup(triple);
    expect(chain.status == Execution::NoCandidatesReady && chain.revalidation->candidates()[0].status == Status::Protected &&
               chain.revalidation->candidates()[1].status == Status::StillRequired &&
               chain.revalidation->candidates()[2].status == Status::StillRequired && g_removal_commands.empty(),
           "held A failed to protect dependent B/C through iterative shrink");
    expect(collection.invocation_result().is_success() && triple_collection.invocation_result().is_success(),
           "HoldPkg cleanup changed build/install success");
}

} // namespace

// Only the process boundary is replaced. All metadata/session/policy/collector/
// interaction and revalidation implementations above are production code.
CapturedCommandResult capture_command_output_raw(const char* command) {
    ++g_configuration_reads;
    if(std::string(command) == "pacman-conf HoldPkg 2>/dev/null") {
        ++g_hold_package_queries;
        return g_hold_package_result;
    }
    if(g_configuration_failure) return {{}, 1};
    if(std::string(command) == "pacman-conf --verbose RootDir DBPath 2>/dev/null") return {"RootDir = /\nDBPath = /var/lib/pacman\n", 0};
    if(std::string(command) == "pacman-conf --repo-list 2>/dev/null") return {"core\nextra\n", 0};
    throw std::runtime_error("unexpected cleanup metadata process");
}

int run_command(const std::string& command) {
    g_removal_commands.push_back(command);
    if(g_removal_throws) throw std::runtime_error("synthetic process wait failure");
    return g_removal_exit_status;
}

void run_dependency_cleanup_execution_tests() {
    static_assert(!std::is_default_constructible_v<DependencyCleanupRevalidationResult>);
    static_assert(!std::is_invocable_v<decltype(execute_dependency_cleanup), const DependencyCleanupApprovalSnapshot&>);
    static_assert(!std::is_invocable_v<decltype(execute_dependency_cleanup), const DependencyCleanupRevalidationResult&>);
    test_fresh_revalidation_matrix();
    test_shrink_and_retained_consumer_closure();
    test_exact_executor_and_composition();
    test_fresh_hold_package_protection();
}
