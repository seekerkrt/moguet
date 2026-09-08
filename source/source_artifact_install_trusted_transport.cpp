#include "source_artifact_install_trusted_transport.hpp"

#include "evaluated_devel_source_build.hpp"
#include "devel_source_artifact_install_state.hpp"
#include "evaluated_devel_source_artifact_transport.hpp"
#include "exact_artifact_transaction_receipt.hpp"
#include "fresh_installed_artifact_binding.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "shell_words.hpp"
#include "source_artifact_install_trusted_protocol.hpp"
#include "source_package_identity_projection.hpp"
#include "trusted_alpm_receipt_protocol.hpp"
#include "trusted_alpm_receipt_transport.hpp"
#include "xdg_generation_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <optional>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <variant>

#include <linux/memfd.h>

#ifndef MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH
#error "MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH is required"
#endif

namespace {

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
EvaluatedDevelSourceArtifactTransportTestHooks g_evaluated_transport_test_hooks;
#endif

constexpr std::string_view SUDO_PATH = "/usr/bin/sudo";
constexpr std::string_view PACMAN_PATH = "/usr/bin/pacman";

class OwnedDescriptor final {
public:
    explicit OwnedDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }
    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;
    OwnedDescriptor(OwnedDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }
    OwnedDescriptor& operator=(OwnedDescriptor&& other) noexcept {
        if(this == &other) return *this;
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }
    ~OwnedDescriptor() {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

bool same_executable_identity(
    const struct stat& lhs, const struct stat& rhs) noexcept {
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino &&
           lhs.st_mode == rhs.st_mode && lhs.st_uid == rhs.st_uid;
}

bool trusted_directory_metadata(const struct stat& metadata) noexcept {
    return S_ISDIR(metadata.st_mode) && metadata.st_uid == 0 &&
           (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool validate_fixed_executable(
    std::string_view executable_path, bool require_helper_mode) noexcept {
    if(executable_path.empty() || executable_path.front() != '/' ||
       executable_path.find('\0') != std::string_view::npos) {
        return false;
    }

    const int root_fd = open(
        "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(root_fd == -1) return false;
    OwnedDescriptor current(root_fd);
    struct stat root_metadata{};
    if(fstat(current.get(), &root_metadata) == -1 ||
       !trusted_directory_metadata(root_metadata)) {
        return false;
    }

    std::size_t component_begin = 1;
    while(component_begin < executable_path.size()) {
        const std::size_t separator =
            executable_path.find('/', component_begin);
        const bool is_final = separator == std::string_view::npos;
        const std::size_t component_end =
            is_final ? executable_path.size() : separator;
        const std::string component(executable_path.substr(
            component_begin, component_end - component_begin));
        if(component.empty() || component == "." || component == "..") {
            return false;
        }

        if(is_final) {
            const int file_fd = openat(
                current.get(), component.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if(file_fd == -1) return false;
            OwnedDescriptor file(file_fd);
            struct stat descriptor_metadata{};
            struct stat named_metadata{};
            if(fstat(file.get(), &descriptor_metadata) == -1 ||
               fstatat(
                   current.get(), component.c_str(), &named_metadata,
                   AT_SYMLINK_NOFOLLOW) == -1 ||
               !same_executable_identity(
                   descriptor_metadata, named_metadata) ||
               !S_ISREG(descriptor_metadata.st_mode) ||
               descriptor_metadata.st_uid != 0 ||
               (descriptor_metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
               (descriptor_metadata.st_mode & S_IXUSR) == 0) {
                return false;
            }
            if(require_helper_mode &&
               (descriptor_metadata.st_mode & 07777) != 0755) {
                return false;
            }
            return true;
        }

        const int next_fd = openat(
            current.get(), component.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if(next_fd == -1) return false;
        OwnedDescriptor next(next_fd);
        struct stat descriptor_metadata{};
        struct stat named_metadata{};
        if(fstat(next.get(), &descriptor_metadata) == -1 ||
           fstatat(
               current.get(), component.c_str(), &named_metadata,
               AT_SYMLINK_NOFOLLOW) == -1 ||
           !same_executable_identity(
               descriptor_metadata, named_metadata) ||
           !trusted_directory_metadata(descriptor_metadata)) {
            return false;
        }
        current = std::move(next);
        component_begin = separator + 1;
    }
    return false;
}

bool fixed_executables_are_trusted() noexcept {
    return validate_fixed_executable(SUDO_PATH, false) &&
           validate_fixed_executable(PACMAN_PATH, false) &&
           validate_fixed_executable(
               MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH, true);
}

std::vector<std::string> minimal_root_command_environment() {
    return {"PATH=/usr/bin", "LC_ALL=C"};
}

void log_explicit_invocation(const ExplicitProcessInvocation& invocation) {
    std::vector<std::string> display;
    display.reserve(invocation.arguments.size() + 1);
    display.push_back(invocation.executable);
    display.insert(
        display.end(), invocation.arguments.begin(),
        invocation.arguments.end());
    Logger::raw_cmd(shell_words::join(display));
}

CapturedCommandResult capture_explicit(
    const ExplicitProcessInvocation& invocation) {
    log_explicit_invocation(invocation);
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
    if(g_evaluated_transport_test_hooks.capture)
        return g_evaluated_transport_test_hooks.capture(invocation);
#endif
    return capture_explicit_process_output_raw(invocation);
}

int run_explicit_unlogged(const ExplicitProcessInvocation& invocation) {
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
    if(g_evaluated_transport_test_hooks.execute)
        return g_evaluated_transport_test_hooks.execute(invocation).exit_code.value_or(127);
#endif
    return run_explicit_process(invocation);
}

int run_explicit(const ExplicitProcessInvocation& invocation) {
    log_explicit_invocation(invocation);
    return run_explicit_unlogged(invocation);
}

bool roots_are_valid_and_unique(
    const std::vector<RootTargetIdentity>& roots) {
    if(roots.empty()) return false;
    std::set<std::pair<std::size_t, std::string>> unique;
    for(const RootTargetIdentity& root : roots) {
        if(!is_valid_package_name(root.requested_name) ||
           !unique.emplace(root.invocation_index, root.requested_name)
                .second) {
            return false;
        }
    }
    return true;
}

bool same_root_set(
    const std::vector<RootTargetIdentity>& lhs,
    const std::vector<RootTargetIdentity>& rhs) {
    if(lhs.size() != rhs.size()) return false;
    return std::all_of(lhs.begin(), lhs.end(), [&rhs](const auto& root) {
        return std::find(rhs.begin(), rhs.end(), root) != rhs.end();
    });
}

bool root_set_is_subset(
    const std::vector<RootTargetIdentity>& subset,
    const std::vector<RootTargetIdentity>& superset) {
    return std::all_of(
        subset.begin(), subset.end(), [&superset](const auto& root) {
            return std::find(superset.begin(), superset.end(), root) !=
                   superset.end();
        });
}

bool roles_are_valid_and_unique(const std::vector<PackageRole>& roles) {
    if(roles.empty()) return false;
    std::set<int> unique;
    for(PackageRole role : roles) {
        switch(role) {
            case PackageRole::BuildDependency:
            case PackageRole::CheckDependency:
                break;
            case PackageRole::Root:
            case PackageRole::RuntimeDependency:
            default:
                return false;
        }
        if(!unique.insert(static_cast<int>(role)).second) return false;
    }
    return true;
}

bool edge_indices_are_unique(
    const std::vector<std::size_t>& edge_indices) {
    std::set<std::size_t> unique;
    return std::all_of(
        edge_indices.begin(), edge_indices.end(),
        [&unique](std::size_t edge_index) {
            return unique.insert(edge_index).second;
        });
}

bool binding_is_coherent(
    const PreparedPackageBaseArtifactInstall& install,
    const SourceArtifactInstallTrustedBinding& binding) {
    if(!is_valid_package_name(binding.work_item.package_base) ||
       binding.work_item.package_base != install.package_base() ||
       !roots_are_valid_and_unique(binding.work_item.requested_roots) ||
       binding.selected_artifacts.size() !=
           install.selected_artifacts().size() ||
       binding.selected_artifacts.empty()) {
        return false;
    }
    if(install.transaction_directive() != InstallReasonDirective::Default &&
       install.transaction_directive() !=
           InstallReasonDirective::AsDependency) {
        return false;
    }

    std::vector<RootTargetIdentity> attributed_roots;
    std::set<std::size_t> artifact_indices;
    std::set<std::string> package_names;
    for(std::size_t index = 0;
        index < binding.selected_artifacts.size(); ++index) {
        const auto& expected = binding.selected_artifacts[index];
        const auto& prepared = install.selected_artifacts()[index];
        if(expected.artifact_index != prepared.artifact_index ||
           expected.desired_reason != DesiredInstallReason::Dependency ||
           prepared.desired_reason != DesiredInstallReason::Dependency ||
           !roles_are_valid_and_unique(expected.dependency_roles) ||
           !edge_indices_are_unique(
               expected.build_plan_dependency_edge_indices) ||
           !roots_are_valid_and_unique(expected.requested_roots) ||
           !root_set_is_subset(
               expected.requested_roots,
               binding.work_item.requested_roots) ||
           !artifact_indices.insert(expected.artifact_index).second ||
           !package_names.insert(prepared.identity.package_name).second ||
           expected.expected_identity.package()
                   .package_base()
                   .package_base() != binding.work_item.package_base ||
           !project_artifact_source_package_identity(
                expected.expected_identity, prepared.identity)
                .is_success()) {
            return false;
        }
        for(const RootTargetIdentity& root : expected.requested_roots) {
            if(std::find(
                   attributed_roots.begin(), attributed_roots.end(), root) ==
               attributed_roots.end()) {
                attributed_roots.push_back(root);
            }
        }
    }
    return same_root_set(
        attributed_roots, binding.work_item.requested_roots);
}

bool same_timespec(const timespec& lhs, const timespec& rhs) noexcept {
    return lhs.tv_sec == rhs.tv_sec && lhs.tv_nsec == rhs.tv_nsec;
}

bool same_snapshot_metadata(
    const struct stat& lhs, const struct stat& rhs) noexcept {
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino &&
           lhs.st_mode == rhs.st_mode && lhs.st_uid == rhs.st_uid &&
           lhs.st_gid == rhs.st_gid && lhs.st_nlink == rhs.st_nlink &&
           lhs.st_size == rhs.st_size &&
           same_timespec(lhs.st_mtim, rhs.st_mtim) &&
           same_timespec(lhs.st_ctim, rhs.st_ctim);
}

struct stat require_snapshot_source(
    int descriptor, std::uintmax_t expected_device,
    std::uintmax_t expected_inode, std::uintmax_t expected_owner,
    std::uint64_t maximum_size) {
    struct stat metadata{};
    if(descriptor < 0 || fstat(descriptor, &metadata) == -1 ||
       !S_ISREG(metadata.st_mode) || metadata.st_size <= 0 ||
       static_cast<std::uintmax_t>(metadata.st_dev) != expected_device ||
       static_cast<std::uintmax_t>(metadata.st_ino) != expected_inode ||
       static_cast<std::uintmax_t>(metadata.st_uid) != expected_owner ||
       static_cast<std::uint64_t>(metadata.st_size) > maximum_size) {
        throw std::runtime_error(
            "validated artifact descriptor cannot be snapshotted");
    }
    return metadata;
}

void append_descriptor_bytes(
    int source_fd, const struct stat& expected_source,
    int destination_fd) {
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t offset = 0;
    const std::uint64_t byte_count =
        static_cast<std::uint64_t>(expected_source.st_size);
    while(offset < byte_count) {
        const std::size_t request_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(byte_count - offset, buffer.size()));
        const ssize_t count = pread(
            source_fd, buffer.data(), request_size,
            static_cast<off_t>(offset));
        if(count > 0) {
            std::size_t written_offset = 0;
            const std::size_t count_size = static_cast<std::size_t>(count);
            while(written_offset < count_size) {
                const ssize_t written = write(
                    destination_fd, buffer.data() + written_offset,
                    count_size - written_offset);
                if(written > 0) {
                    written_offset += static_cast<std::size_t>(written);
                    continue;
                }
                if(written == -1 && errno == EINTR) continue;
                throw std::runtime_error(
                    "unable to write sealed source-artifact snapshot");
            }
            offset += count_size;
            continue;
        }
        if(count == -1 && errno == EINTR) continue;
        throw std::runtime_error(
            "unable to read validated artifact descriptor");
    }
    struct stat after{};
    if(fstat(source_fd, &after) == -1 ||
       !same_snapshot_metadata(expected_source, after)) {
        throw std::runtime_error(
            "validated artifact changed while being snapshotted");
    }
}

OwnedDescriptor create_sealable_memfd() {
#ifdef SYS_memfd_create
    const long descriptor = syscall(
        SYS_memfd_create, "moguet-source-artifact-snapshot",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if(descriptor < 0) {
        throw std::runtime_error(
            "unable to create source-artifact snapshot");
    }
    return OwnedDescriptor(static_cast<int>(descriptor));
#else
    throw std::runtime_error(
        "memfd_create is unavailable for source-artifact staging");
#endif
}

// Shared by both retained-input routes. No path is reopened by snapshotting.
void seal_snapshot(int descriptor, const SourceArtifactInstallRootPrepareRequest& request) {
    if(!is_valid_source_artifact_install_root_request(request) || fsync(descriptor) == -1)
        throw std::runtime_error("source-artifact snapshot is invalid");
    constexpr int SEALS = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if(fcntl(descriptor, F_ADD_SEALS, SEALS) == -1 ||
       (fcntl(descriptor, F_GET_SEALS) & SEALS) != SEALS ||
       lseek(descriptor, 0, SEEK_SET) == -1)
        throw std::runtime_error("source-artifact snapshot could not be sealed");
}

struct PreparedTransportInput {
    OwnedDescriptor sealed_input;
    SourceArtifactInstallRootPrepareRequest root_request;
    std::vector<SourceArtifactInstallObservedSelectedArtifact>
        observed_artifacts;
    std::vector<std::string> requested_package_names;
};

// Internal capture shared only by the existing engine and the retained-proof
// owner in this TU. Raw wire values never leave here as a receipt capability.
struct ExactArtifactTransportCapture {
    DevelSourceArtifactInstallState& terminal;
    bool known_success() const noexcept {
        return terminal.operation == DevelSourceArtifactInstallOperation::Succeeded && terminal.pacman_exit_status == 0;
    }
    std::optional<SourceArtifactInstallRootPrepareRequest> manifest;
    std::string stage;
    std::optional<ExactArtifactRootEvidence> evidence;
    ExactArtifactOperationRecords records;
    ExactArtifactReceiptIssue issue = ExactArtifactReceiptIssue::Missing;
};

std::vector<std::string> prepare_arguments(
    const SourceArtifactInstallRootPrepareRequest& request) {
    std::vector<std::string> arguments{
        "--",
        MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH,
        request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding ? "prepare-exact" : "prepare",
        request.transaction_token,
        request.package_base,
        request.directive == SourceArtifactInstallTrustedDirective::
                                 AsDependency
            ? "AsDependency"
            : "PreserveExistingReason",
        request.needed ? "1" : "0",
        request.no_confirm ? "1" : "0",
        "--"};
    arguments.reserve(arguments.size() + request.artifacts.size() * 9);
    for(const auto& artifact : request.artifacts) {
        arguments.push_back(std::to_string(artifact.artifact_index));
        arguments.push_back(artifact.package_name);
        arguments.push_back(artifact.full_version);
        arguments.push_back(artifact.package_base);
        arguments.push_back(artifact.architecture);
        arguments.push_back(std::to_string(artifact.artifact_size));
        arguments.push_back(std::to_string(artifact.signature_size));
        arguments.push_back(artifact.archive_sha256);
        arguments.push_back(artifact.signature_sha256);
        if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding)
            arguments.push_back(artifact.raw_mtree_sha256);
    }
    return arguments;
}

ExplicitProcessInvocation prepare_invocation(
    const SourceArtifactInstallRootPrepareRequest& request,
    int sealed_input_fd) {
    ExplicitProcessInvocation invocation;
    invocation.executable = std::string(SUDO_PATH);
    invocation.arguments = prepare_arguments(request);
    invocation.environment = minimal_root_command_environment();
    invocation.stdout_capture_limit =
        SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES;
    invocation.standard_input_fd = sealed_input_fd;
    return invocation;
}

ExplicitProcessInvocation helper_invocation(
    const std::string& command,
    const std::string& transaction_token) {
    ExplicitProcessInvocation invocation;
    invocation.executable = std::string(SUDO_PATH);
    invocation.arguments = {
        "--", MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH, command,
        transaction_token};
    invocation.environment = minimal_root_command_environment();
    return invocation;
}

ExplicitProcessInvocation pacman_invocation(
    const SourceArtifactInstallRootPrepareRequest& request,
    const SourceArtifactInstallRootPrepareResponse& response) {
    ExplicitProcessInvocation invocation;
    invocation.executable = std::string(SUDO_PATH);
    invocation.arguments = {"--", std::string(PACMAN_PATH), "-U"};
    if(request.needed) invocation.arguments.push_back("--needed");
    if(request.directive ==
       SourceArtifactInstallTrustedDirective::AsDependency) {
        invocation.arguments.push_back("--asdeps");
    }
    if(request.no_confirm) invocation.arguments.push_back("--noconfirm");
    invocation.arguments.push_back("--hookdir");
    invocation.arguments.push_back(response.hook_directory);
    invocation.arguments.push_back("--");
    for(const auto& artifact : response.artifacts) {
        invocation.arguments.push_back(artifact.path);
    }
    invocation.environment = minimal_root_command_environment();
    return invocation;
}

int abort_prepared_state_noexcept(
    const std::string& transaction_token) noexcept {
    try {
        ExplicitProcessInvocation abort =
            helper_invocation("abort", transaction_token);
        try {
            return run_explicit(abort);
        } catch(...) {
            return run_explicit_unlogged(abort);
        }
    } catch(...) {
        return 127;
    }
}

PacmanTransactionReceiptObservation missing_observation() {
    return PacmanTransactionReceiptObservation{
        PacmanTransactionReceiptObservationState::Missing,
        std::nullopt,
        std::nullopt,
        {}};
}

PacmanTransactionReceiptObservation incomplete_observation(
    const std::string& transaction_token) {
    return PacmanTransactionReceiptObservation{
        PacmanTransactionReceiptObservationState::Incomplete,
        transaction_token,
        InvocationDependencyTransactionOwner::SourceArtifactInstall,
        {}};
}

PacmanTransactionReceiptObservation invalid_observation(
    const std::string& transaction_token) {
    return PacmanTransactionReceiptObservation{
        PacmanTransactionReceiptObservationState::Complete,
        transaction_token,
        InvocationDependencyTransactionOwner::Unknown,
        {}};
}

InvocationDependencyTransaction make_transaction(
    const std::string& transaction_token,
    std::vector<std::string> requested_packages,
    InvocationDependencyTransactionCommandOutcome command_outcome,
    PacmanTransactionReceiptObservation observation) {
    constexpr auto OWNER =
        InvocationDependencyTransactionOwner::SourceArtifactInstall;
    return InvocationDependencyTransaction{
        transaction_token,
        OWNER,
        std::move(requested_packages),
        command_outcome,
        validate_pacman_transaction_receipt(
            transaction_token, OWNER, observation)};
}

std::optional<SourceArtifactInstallSealingRefusal> parse_helper_refusal(
    const CapturedCommandResult& result, const std::string& token) {
    if(result.stdout_capture_limit_exceeded) return std::nullopt;
    const auto parsed = parse_source_artifact_install_execution_observation(result.output);
    const auto* observation = std::get_if<SourceArtifactInstallExecutionObservation>(&parsed);
    if(!observation || observation->transaction_token != token) return std::nullopt;
    return observation->refusal;
}

} // namespace

class SourceArtifactInstallTrustedTransport final {
    static bool session_binding_is_coherent(
        const SourceArtifactInstallTrustedBinding& binding) noexcept {
        if(!binding.work_item.invocation_authority.has_value()) return true;
        if(!binding.work_item.invocation_authority->is_active()) return false;
        const PreparedRemoteSourceBuild& prepared =
            binding.work_item.invocation_authority->prepared();
        const std::size_t work_item_index =
            binding.work_item.work_item_index;
        if(!prepared.aur_build_plan.has_value() ||
           work_item_index >= prepared.invocation.work_items.size()) {
            return false;
        }
        const ProductionSourceBuildWorkItem& work_item =
            prepared.invocation.work_items[work_item_index];
        if(work_item.request.checkout_name !=
           binding.work_item.package_base) {
            return false;
        }
        for(const SourceArtifactInstallExpectedSelectedArtifact& selected :
            binding.selected_artifacts) {
            for(const std::size_t edge_index :
                selected.build_plan_dependency_edge_indices) {
                if(edge_index >=
                       prepared.aur_build_plan->dependency_edges.size() ||
                   std::find(
                       work_item.build_plan_dependency_edge_indices.begin(),
                       work_item.build_plan_dependency_edge_indices.end(),
                       edge_index) ==
                       work_item.build_plan_dependency_edge_indices.end()) {
                    return false;
                }
            }
        }
        return true;
    }

    static const PlannedPackageTarget* find_unique_target(
        const BuildPlan& plan,
        const std::string& package_name,
        const std::string& package_base) noexcept {
        const PlannedPackageTarget* match = nullptr;
        for(const PlannedPackageTarget& target : plan.package_targets) {
            if(target.package_name != package_name ||
               target.package_base != package_base) {
                continue;
            }
            if(match != nullptr) return nullptr;
            match = &target;
        }
        return match;
    }

    static bool edge_identity_matches_artifact(
        const BuildPlanDependencyEdge& edge,
        const ArtifactPackageIdentity& artifact) {
        if(!edge.resolved_candidate.has_value() ||
           !edge.requirement.has_value() ||
           !edge.constraint_evaluation.has_value()) {
            return false;
        }
        const ConstraintSatisfaction satisfaction =
            edge.constraint_evaluation->satisfaction();
        if(satisfaction != ConstraintSatisfaction::Satisfied &&
           satisfaction != ConstraintSatisfaction::Unconstrained) {
            return false;
        }
        SourcePackageIdentityProjectionResult projection =
            project_dependency_source_package_identity(
                edge.resolved_candidate.value());
        const SourcePackageIdentityProjectionSuccess* success =
            projection.success();
        if(success == nullptr || success->identities.size() != 1) {
            return false;
        }
        const SourceAwarePackageIdentity& identity =
            success->identities.front();
        const std::string* version =
            identity.package_version().full_version();
        return identity.package()
                       .package_base()
                       .source()
                       .kind() == PackageSourceKind::Aur &&
               identity.package().package_name() == artifact.package_name &&
               identity.package().package_base().package_base() ==
                   (artifact.package_base.value() == nullptr
                        ? std::string{}
                        : *artifact.package_base.value()) &&
               identity.package_version().state() ==
                   PackageVersionState::Known &&
               version != nullptr && *version == artifact.full_version;
    }

public:
    static std::optional<SourceArtifactInstallTrustedBinding>
    project_session_binding(
        const PreparedPackageBaseArtifactInstall& install,
        const CleanupInvocationAuthority& authority,
        std::size_t work_item_index) {
        if(!authority.is_active() || !authority.baseline_was_observed()) {
            return std::nullopt;
        }
        const PreparedRemoteSourceBuild& prepared = authority.prepared();
        if(prepared.source.source_kind() != SourceBuildSourceKind::Aur ||
           !prepared.aur_build_plan.has_value() ||
           work_item_index >= prepared.invocation.work_items.size()) {
            return std::nullopt;
        }
        const BuildPlan& plan = prepared.aur_build_plan.value();
        const ProductionSourceBuildWorkItem& work_item =
            prepared.invocation.work_items[work_item_index];
        if(work_item.request.checkout_name != install.package_base_ ||
           work_item.required_target_provenance !=
               RequiredTargetProvenance::AurBuildPlanProjection ||
           !work_item.request.aur_review_identity.has_value() ||
           work_item.request.aur_review_identity->package_base() !=
               install.package_base_ ||
           work_item.request.aur_review_identity->source().kind() !=
               PackageSourceKind::Aur ||
           install.selected_artifacts_.empty()) {
            return std::nullopt;
        }

        SourceArtifactInstallTrustedBinding binding;
        binding.work_item = SourceArtifactInstallWorkItemBinding{
            authority, work_item_index, install.package_base_, {}};
        binding.selected_artifacts.reserve(
            install.selected_artifacts_.size());
        for(const PreparedPackageBaseArtifactInstallSelectedArtifact&
                selected : install.selected_artifacts_) {
            const std::string* package_base =
                selected.identity.package_base.value();
            const std::string* architecture =
                selected.identity.architecture.value();
            if(selected.desired_reason !=
                   DesiredInstallReason::Dependency ||
               package_base == nullptr || architecture == nullptr ||
               *package_base != install.package_base_) {
                return std::nullopt;
            }

            const auto required = std::find_if(
                work_item.required_targets.begin(),
                work_item.required_targets.end(),
                [&selected, &install](
                    const RequiredPackageArtifactTarget& target) {
                    return target.package_base == install.package_base_ &&
                           target.package_name ==
                               selected.identity.package_name &&
                           target.desired_reason ==
                               DesiredInstallReason::Dependency;
                });
            if(required == work_item.required_targets.end() ||
               std::count_if(
                   work_item.required_targets.begin(),
                   work_item.required_targets.end(),
                   [&selected, &install](
                       const RequiredPackageArtifactTarget& target) {
                       return target.package_base == install.package_base_ &&
                              target.package_name ==
                                  selected.identity.package_name;
                   }) != 1) {
                return std::nullopt;
            }

            const PlannedPackageTarget* target = find_unique_target(
                plan, selected.identity.package_name, install.package_base_);
            if(target == nullptr || target->roots.empty() ||
               !roots_are_valid_and_unique(target->roots) ||
               !roles_are_valid_and_unique(target->roles)) {
                return std::nullopt;
            }

            std::vector<std::size_t> edge_indices;
            std::vector<PackageRole> roles;
            for(const std::size_t edge_index :
                work_item.build_plan_dependency_edge_indices) {
                if(edge_index >= plan.dependency_edges.size()) {
                    return std::nullopt;
                }
                const BuildPlanDependencyEdge& edge =
                    plan.dependency_edges[edge_index];
                if(!edge_identity_matches_artifact(
                       edge, selected.identity)) {
                    continue;
                }
                if(edge.role != PackageRole::BuildDependency &&
                   edge.role != PackageRole::CheckDependency) {
                    return std::nullopt;
                }
                edge_indices.push_back(edge_index);
                if(std::find(roles.begin(), roles.end(), edge.role) ==
                   roles.end()) {
                    roles.push_back(edge.role);
                }
            }
            if(edge_indices.empty() || roles.size() != target->roles.size() ||
               std::any_of(
                   target->roles.begin(), target->roles.end(),
                   [&roles](PackageRole role) {
                       return std::find(roles.begin(), roles.end(), role) ==
                              roles.end();
                   })) {
                return std::nullopt;
            }

            SourceAwarePackageIdentity expected_identity =
                SourceAwarePackageIdentity::make(
                    PackageChildIdentity::make(
                        work_item.request.aur_review_identity.value(),
                        selected.identity.package_name),
                    SourceRevisionIdentity::unknown(),
                    PackageVersionIdentity::composite(
                        selected.identity.full_version),
                    PackageArchitectureIdentity::known({*architecture}));
            binding.selected_artifacts.push_back(
                SourceArtifactInstallExpectedSelectedArtifact{
                    selected.artifact_index,
                    std::move(expected_identity),
                    DesiredInstallReason::Dependency,
                    std::move(roles),
                    target->roots,
                    std::move(edge_indices)});
            for(const RootTargetIdentity& root : target->roots) {
                if(std::find(
                       binding.work_item.requested_roots.begin(),
                       binding.work_item.requested_roots.end(), root) ==
                   binding.work_item.requested_roots.end()) {
                    binding.work_item.requested_roots.push_back(root);
                }
            }
        }
        if(!binding_is_coherent(install, binding)) return std::nullopt;
        return binding;
    }

    static bool register_transaction_token(
        const SourceArtifactInstallTrustedBinding& binding,
        const std::string& transaction_token) {
        if(!binding.work_item.invocation_authority.has_value()) return true;
        return binding.work_item.invocation_authority
            ->register_trusted_transaction_token(
                InvocationDependencyTransactionOwner::
                    SourceArtifactInstall,
                transaction_token,
                {binding.work_item.work_item_index});
    }

private:
    static PackageBaseArtifactInstallExecutionResult make_operation_result(
        const PreparedPackageBaseArtifactInstall& install) {
        std::vector<PackageBaseArtifactInstallExecutionArtifactResult>
            artifacts;
        artifacts.reserve(install.selected_artifacts_.size());
        for(const PreparedPackageBaseArtifactInstallSelectedArtifact&
                artifact : install.selected_artifacts_) {
            artifacts.push_back(
                PackageBaseArtifactInstallExecutionArtifactResult{
                    artifact.artifact_index,
                    artifact.identity,
                    artifact.desired_reason,
                    artifact.expected_outcome});
        }
        return PackageBaseArtifactInstallExecutionResult(
            install.package_base_, std::move(artifacts));
    }

    static PreparedTransportInput snapshot_selected_artifacts(
        PreparedPackageBaseArtifactInstall& install,
        const SourceArtifactInstallTrustedBinding& binding,
        const std::string& transaction_token,
        const ArtifactInstallExecutionOptions& options) {
        install.require_execution_coherence();
        if(!binding_is_coherent(install, binding)) {
            throw std::runtime_error(
                "source-artifact trusted binding is incoherent");
        }

        OwnedDescriptor snapshot = create_sealable_memfd();
        SourceArtifactInstallRootPrepareRequest root_request{
            transaction_token,
            install.package_base_,
            install.transaction_directive_ ==
                    InstallReasonDirective::AsDependency
                ? SourceArtifactInstallTrustedDirective::AsDependency
                : SourceArtifactInstallTrustedDirective::
                      PreserveExistingReason,
            install.needed_,
            options.no_confirm,
            {}};
        std::vector<SourceArtifactInstallObservedSelectedArtifact>
            observed_artifacts;
        std::vector<std::string> requested_names;
        root_request.artifacts.reserve(install.selected_artifacts_.size());
        observed_artifacts.reserve(install.selected_artifacts_.size());
        requested_names.reserve(install.selected_artifacts_.size());

        for(std::size_t position = 0;
            position < install.selected_artifacts_.size(); ++position) {
            const auto& selected = install.selected_artifacts_[position];
            const auto& expected = binding.selected_artifacts[position];
            const auto& record =
                install.artifacts_.records_[selected.artifact_index];
            const struct stat artifact_metadata = require_snapshot_source(
                record.artifact_descriptor, record.artifact_device,
                record.artifact_inode, record.artifact_owner,
                SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES);
            const std::string archive_digest = xdg_generation_store_file_descriptor_sha256(
                record.artifact_descriptor, static_cast<std::uintmax_t>(artifact_metadata.st_size),
                SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES);
            append_descriptor_bytes(
                record.artifact_descriptor, artifact_metadata,
                snapshot.get());

            std::uint64_t signature_size = 0;
            std::string signature_digest = "-";
            if(record.has_signature) {
                const struct stat signature_metadata =
                    require_snapshot_source(
                        record.signature_descriptor,
                        record.signature_device,
                        record.signature_inode,
                        record.signature_owner,
                        SOURCE_ARTIFACT_INSTALL_MAXIMUM_SIGNATURE_BYTES);
                signature_digest = xdg_generation_store_file_descriptor_sha256(
                    record.signature_descriptor, static_cast<std::uintmax_t>(signature_metadata.st_size),
                    SOURCE_ARTIFACT_INSTALL_MAXIMUM_SIGNATURE_BYTES);
                append_descriptor_bytes(
                    record.signature_descriptor, signature_metadata,
                    snapshot.get());
                signature_size =
                    static_cast<std::uint64_t>(signature_metadata.st_size);
            } else if(record.signature_descriptor >= 0) {
                throw std::runtime_error(
                    "source-artifact signature descriptor is incoherent");
            }

            const std::string* package_base =
                selected.identity.package_base.value();
            const std::string* architecture =
                selected.identity.architecture.value();
            if(selected.identity.package_base.state() !=
                   ArtifactMetadataValueState::Known ||
               selected.identity.architecture.state() !=
                   ArtifactMetadataValueState::Known ||
               package_base == nullptr || architecture == nullptr) {
                throw std::runtime_error(
                    "source-artifact archive identity is incomplete");
            }
            root_request.artifacts.push_back(
                SourceArtifactInstallRootArtifactExpectation{
                    selected.artifact_index,
                    selected.identity.package_name,
                    selected.identity.full_version,
                    *package_base,
                    *architecture,
                    static_cast<std::uint64_t>(
                        artifact_metadata.st_size),
                    signature_size, archive_digest, signature_digest});
            observed_artifacts.push_back(
                SourceArtifactInstallObservedSelectedArtifact{
                    selected.artifact_index,
                    selected.identity,
                    selected.desired_reason,
                    expected.dependency_roles,
                    expected.requested_roots,
                    expected.build_plan_dependency_edge_indices});
            requested_names.push_back(selected.identity.package_name);
        }

        install.require_execution_coherence();
        seal_snapshot(snapshot.get(), root_request);
        return PreparedTransportInput{
            std::move(snapshot), std::move(root_request),
            std::move(observed_artifacts), std::move(requested_names)};
    }

    static std::optional<SourceArtifactInstallReceiptObservation> make_observation(
        const SourceArtifactInstallTrustedBinding* binding,
        std::vector<SourceArtifactInstallObservedSelectedArtifact>
            observed_artifacts,
        InvocationDependencyTransaction transaction) {
        if(binding == nullptr) return std::nullopt;
        InvocationDependencyTransactionLedger ledger;
        ledger.transactions.push_back(std::move(transaction));
        return SourceArtifactInstallReceiptObservation(
            binding->work_item, std::move(observed_artifacts),
            std::move(ledger));
    }

public:
    static bool capability_is_active(
        const PreparedPackageBaseArtifactInstall& install) noexcept {
        return install.state_ ==
               PreparedPackageBaseArtifactInstall::State::Active;
    }

    static SourceArtifactInstallTrustedExecutionResult
    unknown_after_consumption(
        const SourceArtifactInstallTrustedBinding* binding,
        const std::string& transaction_token) {
        // No abort/consume/retry: the privileged transaction may still be alive.
        std::optional<SourceArtifactInstallReceiptExpectation> expectation;
        if(binding != nullptr) expectation.emplace(SourceArtifactInstallReceiptExpectation{
            binding->work_item, binding->selected_artifacts, transaction_token});
        return SourceArtifactInstallTrustedExecutionResult(
            SourceArtifactInstallTrustedExecutionStatus::OutcomeUnknown,
            std::nullopt, std::move(expectation), std::nullopt,
            "source-artifact execution outcome is unknown after the one-shot capability was consumed");
    }

    static bool request_is_valid(
        PreparedPackageBaseArtifactInstall& install,
        const SourceArtifactInstallTrustedBinding& binding) noexcept {
        try {
            install.require_execution_coherence();
            return binding_is_coherent(install, binding) &&
                   session_binding_is_coherent(binding);
        } catch(...) {
            return false;
        }
    }

    static SourceArtifactInstallTrustedExecutionResult invalid_result(
        SourceArtifactInstallTrustedExecutionStatus status,
        std::string diagnostic) {
        return SourceArtifactInstallTrustedExecutionResult(
            status, std::nullopt, std::nullopt, std::nullopt,
            std::move(diagnostic));
    }

    static SourceArtifactInstallTrustedExecutionResult exact_postprocessing_failure(bool has_receipt) noexcept {
        return SourceArtifactInstallTrustedExecutionResult(
            has_receipt ? SourceArtifactInstallTrustedExecutionStatus::Complete : SourceArtifactInstallTrustedExecutionStatus::ConsumeFailed,
            0, std::nullopt, std::nullopt, std::nullopt);
    }

    static SourceArtifactInstallTrustedExecutionResult terminal_result(
        const DevelSourceArtifactInstallState& state) noexcept {
        using Operation = DevelSourceArtifactInstallOperation;
        using Status = SourceArtifactInstallTrustedExecutionStatus;
        const auto status = state.execution                                ? state.execution->status()
                            : state.operation == Operation::Succeeded      ? (state.receipt ? Status::Complete : Status::ConsumeFailed)
                            : state.operation == Operation::Failed         ? Status::PacmanFailed
                            : state.operation == Operation::OutcomeUnknown ? Status::OutcomeUnknown
                                                                           : Status::InvalidRequest;
        return SourceArtifactInstallTrustedExecutionResult(status, state.pacman_exit_status,
                                                           std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                                           state.execution ? state.execution->sealing_failure() : std::nullopt);
    }

    static SourceArtifactInstallTrustedExecutionResult execute(
        PreparedPackageBaseArtifactInstall& install,
        const SourceArtifactInstallTrustedBinding& binding,
        const ArtifactInstallExecutionOptions& options,
        const std::string& transaction_token) {
        // Allocate the legacy correlation before snapshot/consumption, as in
        // the original route: allocation failure must leave that input active.
        SourceArtifactInstallReceiptExpectation expectation{
            binding.work_item, binding.selected_artifacts, transaction_token};
        PreparedTransportInput input = snapshot_selected_artifacts(
            install, binding, transaction_token, options);
        // Preserve the old route's consumption point: snapshot failure leaves
        // it active, any privileged preparation attempt consumes it.
        install.state_ = PreparedPackageBaseArtifactInstall::State::Consumed;
        return execute_snapshot(std::move(input), transaction_token, &install, &binding, std::move(expectation));
    }

    // One privileged engine. Optional legacy projection never fabricates a
    // cleanup work item, reason prediction, or session for the Slice 4 input.
    static SourceArtifactInstallTrustedExecutionResult execute_snapshot(
        PreparedTransportInput input,
        const std::string& transaction_token,
        const PreparedPackageBaseArtifactInstall* install = nullptr,
        const SourceArtifactInstallTrustedBinding* binding = nullptr,
        std::optional<SourceArtifactInstallReceiptExpectation> expectation = std::nullopt,
        ExactArtifactTransportCapture* exact_capture = nullptr) {
        CapturedCommandResult prepare_result;
        if(exact_capture) exact_capture->terminal.cleanup = {
                              DevelSourceArtifactInstallCleanupState::OutcomeUnknown, DevelSourceArtifactInstallCleanupIssue::PrepareUnconfirmed, {}};
        try {
            prepare_result = capture_explicit(prepare_invocation(
                input.root_request, input.sealed_input.get()));
        } catch(...) {
            const int abort_status =
                abort_prepared_state_noexcept(transaction_token);
            if(exact_capture) exact_capture->terminal.cleanup = {
                                  abort_status == 0 ? DevelSourceArtifactInstallCleanupState::Complete : DevelSourceArtifactInstallCleanupState::Failed,
                                  abort_status == 0 ? std::nullopt : std::optional{DevelSourceArtifactInstallCleanupIssue::AbortFailed}, abort_status};
            auto observation = make_observation(
                binding, std::move(input.observed_artifacts),
                make_transaction(
                    transaction_token,
                    std::move(input.requested_package_names),
                    InvocationDependencyTransactionCommandOutcome::
                        NotAttempted,
                    missing_observation()));
            return SourceArtifactInstallTrustedExecutionResult(
                abort_status == 0
                    ? SourceArtifactInstallTrustedExecutionStatus::
                          PrepareFailed
                    : SourceArtifactInstallTrustedExecutionStatus::
                          AbortFailed,
                std::nullopt, std::move(expectation),
                std::move(observation),
                abort_status == 0
                    ? "source-artifact preparation observation failed"
                    : "source-artifact preparation observation and exact abort failed");
        }
        const SourceArtifactInstallRootPrepareResponseResult parsed_prepare =
            prepare_result.exit_code == 0 &&
                    !prepare_result.stdout_capture_limit_exceeded
                ? parse_source_artifact_install_root_prepare_response(
                      prepare_result.output, input.root_request)
                : SourceArtifactInstallRootPrepareResponseResult{
                      SourceArtifactInstallTrustedProtocolFailure{
                          SourceArtifactInstallTrustedProtocolIssueKind::
                              TruncatedProtocol}};
        const auto* prepared =
            std::get_if<SourceArtifactInstallRootPrepareResponse>(
                &parsed_prepare);
        if(prepared == nullptr) {
            int abort_status = 0;
            if(prepare_result.exit_code == 0) {
                abort_status =
                    abort_prepared_state_noexcept(transaction_token);
            }
            if(exact_capture) {
                if(prepare_result.exit_code == 0)
                    exact_capture->terminal.cleanup = {
                        abort_status == 0 ? DevelSourceArtifactInstallCleanupState::Complete : DevelSourceArtifactInstallCleanupState::Failed,
                        abort_status == 0 ? std::nullopt : std::optional{DevelSourceArtifactInstallCleanupIssue::AbortFailed}, abort_status};
                else
                    exact_capture->terminal.cleanup.helper_exit_status = prepare_result.exit_code;
            }
            auto observation = make_observation(
                binding, std::move(input.observed_artifacts),
                make_transaction(
                    transaction_token,
                    std::move(input.requested_package_names),
                    InvocationDependencyTransactionCommandOutcome::
                        NotAttempted,
                    missing_observation()));
            return SourceArtifactInstallTrustedExecutionResult(
                SourceArtifactInstallTrustedExecutionStatus::ArtifactSealingFailed,
                std::nullopt, std::move(expectation), std::move(observation),
                abort_status == 0 ? "source-artifact preparation authority failed"
                                  : "source-artifact preparation authority and exact abort failed",
                std::nullopt, parse_helper_refusal(prepare_result, transaction_token).value_or(SourceArtifactInstallSealingRefusal{SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch}));
        }

        if(exact_capture) {
            exact_capture->terminal.cleanup = {DevelSourceArtifactInstallCleanupState::Retained, {}, {}};
            exact_capture->stage = prepared->staged_identity_sha256;
        }

        int pacman_status = 127;
        try {
            // Display the unchanged pacman options/paths. The installed helper
            // now retains and reproves the stage itself immediately before exec.
            log_explicit_invocation(pacman_invocation(input.root_request, *prepared));
            auto invocation = helper_invocation("execute", transaction_token);
            log_explicit_invocation(invocation);
            if(exact_capture) {
                exact_capture->terminal.operation = DevelSourceArtifactInstallOperation::OutcomeUnknown;
                exact_capture->terminal.cleanup = {DevelSourceArtifactInstallCleanupState::Retained, {}, {}};
            }
            const auto execution =
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
                g_evaluated_transport_test_hooks.execute ? g_evaluated_transport_test_hooks.execute(invocation) :
#endif
                                                         run_explicit_process_with_outcome(invocation);
            if(execution.status == ExplicitProcessExecutionStatus::StartedOutcomeUnknown ||
               (execution.status == ExplicitProcessExecutionStatus::StartedKnownOutcome && !execution.exit_code)) {
                return unknown_after_consumption(binding, transaction_token);
            }
            if(execution.status == ExplicitProcessExecutionStatus::NotStarted) {
                if(exact_capture) exact_capture->terminal.operation = DevelSourceArtifactInstallOperation::NotAttempted;
                throw std::runtime_error("privileged execution helper was not started");
            }
            pacman_status = *execution.exit_code;
        } catch(...) {
            if(exact_capture && exact_capture->terminal.operation == DevelSourceArtifactInstallOperation::OutcomeUnknown)
                return terminal_result(exact_capture->terminal);
            const int abort_status =
                abort_prepared_state_noexcept(transaction_token);
            if(exact_capture) exact_capture->terminal.cleanup = {
                                  abort_status == 0 ? DevelSourceArtifactInstallCleanupState::Complete : DevelSourceArtifactInstallCleanupState::Failed,
                                  abort_status == 0 ? std::nullopt : std::optional{DevelSourceArtifactInstallCleanupIssue::AbortFailed}, abort_status};
            auto observation = make_observation(
                binding, std::move(input.observed_artifacts),
                make_transaction(
                    transaction_token,
                    std::move(input.requested_package_names),
                    InvocationDependencyTransactionCommandOutcome::
                        NotAttempted,
                    missing_observation()));
            return SourceArtifactInstallTrustedExecutionResult(
                abort_status == 0
                    ? SourceArtifactInstallTrustedExecutionStatus::
                          PrepareFailed
                    : SourceArtifactInstallTrustedExecutionStatus::
                          AbortFailed,
                std::nullopt, std::move(expectation),
                std::move(observation),
                abort_status == 0
                    ? "source-artifact pacman invocation failed before execution"
                    : "source-artifact pacman invocation and exact abort failed");
        }
        {
            // Authorization only permits exec. A trusted package-manager hook
            // must independently prove a reached phase before any numeric exit
            // can be attributed to pacman. Even zero cannot replace that proof.
            std::optional<SourceArtifactInstallExecutionObservation> execution;
            try {
                auto query = helper_invocation("execution-status", transaction_token);
                query.stdout_capture_limit = SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES;
                const auto status = capture_explicit(query);
                if(status.exit_code == 0 && !status.stdout_capture_limit_exceeded) {
                    auto parsed = parse_source_artifact_install_execution_observation(status.output);
                    if(auto* observed = std::get_if<SourceArtifactInstallExecutionObservation>(&parsed);
                       observed && observed->transaction_token == transaction_token) execution = *observed;
                }
            } catch(...) {
            }
            if(!execution || execution->refusal || !execution->authorized ||
               execution->execution_evidence == SourceArtifactInstallExecutionEvidence::Unobserved) {
                const bool refused = execution && execution->refusal.has_value();
                // Missing/unparseable status proves neither refusal nor the
                // package-manager outcome. Keep the exact private evidence for
                // diagnosis; the consumed capability must not restart or retry.
                if(exact_capture && refused) exact_capture->terminal.operation = DevelSourceArtifactInstallOperation::NotAttempted;
                const int abort_status = refused ? abort_prepared_state_noexcept(transaction_token) : 0;
                if(exact_capture && refused) exact_capture->terminal.cleanup = {
                                                 abort_status == 0 ? DevelSourceArtifactInstallCleanupState::Complete : DevelSourceArtifactInstallCleanupState::Failed,
                                                 abort_status == 0 ? std::nullopt : std::optional{DevelSourceArtifactInstallCleanupIssue::AbortFailed}, abort_status};
                auto observation = make_observation(binding, std::move(input.observed_artifacts),
                                                    make_transaction(transaction_token, std::move(input.requested_package_names),
                                                                     refused ? InvocationDependencyTransactionCommandOutcome::NotAttempted
                                                                             : InvocationDependencyTransactionCommandOutcome::Unknown,
                                                                     incomplete_observation(transaction_token)));
                return SourceArtifactInstallTrustedExecutionResult(
                    refused ? SourceArtifactInstallTrustedExecutionStatus::ArtifactSealingFailed
                            : SourceArtifactInstallTrustedExecutionStatus::OutcomeUnknown,
                    std::nullopt, std::move(expectation), std::move(observation),
                    !refused            ? "source-artifact execution outcome is unknown; private state retained"
                    : abort_status == 0 ? "source-artifact sealed handoff was refused"
                                        : "source-artifact sealed handoff and exact abort failed",
                    std::nullopt, refused ? execution->refusal : std::nullopt);
            }
        }
        if(exact_capture) {
            exact_capture->terminal.operation = pacman_status == 0 ? DevelSourceArtifactInstallOperation::Succeeded : DevelSourceArtifactInstallOperation::Failed;
            exact_capture->terminal.pacman_exit_status = pacman_status;
        }
        if(pacman_status != 0) {
            const int abort_status =
                abort_prepared_state_noexcept(transaction_token);
            if(exact_capture) exact_capture->terminal.cleanup = {
                                  abort_status == 0 ? DevelSourceArtifactInstallCleanupState::Complete : DevelSourceArtifactInstallCleanupState::Failed,
                                  abort_status == 0 ? std::nullopt : std::optional{DevelSourceArtifactInstallCleanupIssue::AbortFailed}, abort_status};
            auto observation = make_observation(
                binding, std::move(input.observed_artifacts),
                make_transaction(
                    transaction_token,
                    std::move(input.requested_package_names),
                    InvocationDependencyTransactionCommandOutcome::Failed,
                    missing_observation()));
            return SourceArtifactInstallTrustedExecutionResult(
                abort_status == 0
                    ? SourceArtifactInstallTrustedExecutionStatus::
                          PacmanFailed
                    : SourceArtifactInstallTrustedExecutionStatus::
                          AbortFailed,
                pacman_status, std::move(expectation),
                std::move(observation),
                abort_status == 0
                    ? "source-artifact pacman transaction failed"
                    : "source-artifact pacman transaction and exact abort failed");
        }

        if(exact_capture) {
            // Fix success before any receipt/DB allocation. The owner can retain
            // this fact even if an exception escapes post-transaction processing.
            try {
                auto invocation = helper_invocation("consume-exact", transaction_token);
                invocation.stdout_capture_limit = SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES;
                exact_capture->terminal.cleanup = {DevelSourceArtifactInstallCleanupState::OutcomeUnknown,
                                                   DevelSourceArtifactInstallCleanupIssue::ConsumeUnconfirmed,
                                                   {}};
                auto consumed = capture_explicit(invocation);
                if(consumed.exit_code != 0 || consumed.stdout_capture_limit_exceeded) {
                    exact_capture->issue = ExactArtifactReceiptIssue::Invalid;
                    if(consumed.exit_code != 0) exact_capture->terminal.cleanup = {
                                                    DevelSourceArtifactInstallCleanupState::Failed, DevelSourceArtifactInstallCleanupIssue::ConsumeFailed, consumed.exit_code};
                    return exact_postprocessing_failure(false);
                }
                auto parsed = parse_exact_artifact_root_evidence(consumed.output, input.root_request, prepared->staged_identity_sha256);
                auto* evidence = std::get_if<ExactArtifactRootEvidence>(&parsed);
                if(!evidence) {
                    exact_capture->issue = std::get<ExactArtifactReceiptIssue>(parsed);
                    return SourceArtifactInstallTrustedExecutionResult(SourceArtifactInstallTrustedExecutionStatus::MalformedReceipt,
                                                                       0, std::nullopt, std::nullopt, std::nullopt);
                }
                exact_capture->terminal.cleanup = {
                    evidence->cleanup == ExactArtifactRootCleanup::Complete ? DevelSourceArtifactInstallCleanupState::Complete : DevelSourceArtifactInstallCleanupState::Failed,
                    evidence->cleanup == ExactArtifactRootCleanup::Complete ? std::nullopt : std::optional{evidence->cleanup == ExactArtifactRootCleanup::RetirementFailed ? DevelSourceArtifactInstallCleanupIssue::RetirementFailed : DevelSourceArtifactInstallCleanupIssue::PrivateStageCleanupFailed},
                    consumed.exit_code};
                auto records = join_exact_artifact_operation_fragments(evidence->installs, evidence->upgrades,
                                                                       input.root_request, prepared->staged_identity_sha256);
                if(const auto* issue = std::get_if<ExactArtifactReceiptIssue>(&records)) {
                    exact_capture->issue = *issue;
                    return SourceArtifactInstallTrustedExecutionResult(
                        *issue == ExactArtifactReceiptIssue::Missing ? SourceArtifactInstallTrustedExecutionStatus::Missing
                                                                     : SourceArtifactInstallTrustedExecutionStatus::MalformedReceipt,
                        0, std::nullopt, std::nullopt, std::nullopt);
                }
                exact_capture->records = std::move(std::get<ExactArtifactOperationRecords>(records));
                exact_capture->evidence.emplace(std::move(*evidence));
                return SourceArtifactInstallTrustedExecutionResult(SourceArtifactInstallTrustedExecutionStatus::Complete,
                                                                   0, std::nullopt, std::nullopt, std::nullopt);
            } catch(const std::bad_alloc&) {
                exact_capture->issue = ExactArtifactReceiptIssue::ResourceFailure;
                return exact_postprocessing_failure(false);
            } catch(...) {
                exact_capture->issue = ExactArtifactReceiptIssue::Invalid;
                return exact_postprocessing_failure(false);
            }
        }

        // A known zero outcome plus independently observed execution fixes
        // operation success. The
        // later receipt/consume path may fail closed for cleanup authority,
        // but it must not rewrite this completed installation as failure.
        std::optional<PackageBaseArtifactInstallExecutionResult> operation_result;
        if(install != nullptr) operation_result.emplace(make_operation_result(*install));

        ExplicitProcessInvocation consume =
            helper_invocation("consume", transaction_token);
        consume.stdout_capture_limit =
            SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES;
        CapturedCommandResult consume_result;
        try {
            consume_result = capture_explicit(consume);
        } catch(...) {
            const int abort_status =
                abort_prepared_state_noexcept(transaction_token);
            auto observation = make_observation(
                binding, std::move(input.observed_artifacts),
                make_transaction(
                    transaction_token,
                    std::move(input.requested_package_names),
                    InvocationDependencyTransactionCommandOutcome::Succeeded,
                    incomplete_observation(transaction_token)));
            return SourceArtifactInstallTrustedExecutionResult(
                abort_status == 0
                    ? SourceArtifactInstallTrustedExecutionStatus::
                          ConsumeFailed
                    : SourceArtifactInstallTrustedExecutionStatus::
                          AbortFailed,
                0, std::move(expectation), std::move(observation),
                abort_status == 0
                    ? "source-artifact receipt consume observation failed"
                    : "source-artifact receipt consume observation and exact abort failed",
                std::move(operation_result));
        }
        if(consume_result.exit_code != 0) {
            const int abort_status =
                abort_prepared_state_noexcept(transaction_token);
            auto observation = make_observation(
                binding, std::move(input.observed_artifacts),
                make_transaction(
                    transaction_token,
                    std::move(input.requested_package_names),
                    InvocationDependencyTransactionCommandOutcome::Succeeded,
                    incomplete_observation(transaction_token)));
            const auto refusal = parse_helper_refusal(consume_result, transaction_token);
            return SourceArtifactInstallTrustedExecutionResult(
                refusal ? SourceArtifactInstallTrustedExecutionStatus::ArtifactSealingFailed
                        : (abort_status == 0 ? SourceArtifactInstallTrustedExecutionStatus::ConsumeFailed
                                             : SourceArtifactInstallTrustedExecutionStatus::AbortFailed),
                0, std::move(expectation), std::move(observation),
                abort_status == 0 ? "source-artifact receipt consume failed"
                                  : "source-artifact receipt consume and exact abort failed",
                std::move(operation_result), refusal);
        }
        if(consume_result.stdout_capture_limit_exceeded) {
            auto observation = make_observation(
                binding, std::move(input.observed_artifacts),
                make_transaction(
                    transaction_token,
                    std::move(input.requested_package_names),
                    InvocationDependencyTransactionCommandOutcome::Succeeded,
                    invalid_observation(transaction_token)));
            return SourceArtifactInstallTrustedExecutionResult(
                SourceArtifactInstallTrustedExecutionStatus::MalformedReceipt,
                0, std::move(expectation), std::move(observation),
                "source-artifact receipt exceeded its capture limit",
                std::move(operation_result));
        }

        const SourceArtifactInstallRootReceiptResult parsed_receipt =
            parse_source_artifact_install_root_receipt(
                consume_result.output);
        const auto* receipt =
            std::get_if<SourceArtifactInstallRootReceipt>(&parsed_receipt);
        if(receipt == nullptr ||
           receipt->transaction_token != transaction_token) {
            auto observation = make_observation(
                binding, std::move(input.observed_artifacts),
                make_transaction(
                    transaction_token,
                    std::move(input.requested_package_names),
                    InvocationDependencyTransactionCommandOutcome::Succeeded,
                    invalid_observation(transaction_token)));
            return SourceArtifactInstallTrustedExecutionResult(
                SourceArtifactInstallTrustedExecutionStatus::MalformedReceipt,
                0, std::move(expectation), std::move(observation),
                "source-artifact receipt protocol was malformed or mismatched",
                std::move(operation_result));
        }
        if(receipt->state ==
           SourceArtifactInstallRootReceiptState::Missing) {
            auto observation = make_observation(
                binding, std::move(input.observed_artifacts),
                make_transaction(
                    transaction_token,
                    std::move(input.requested_package_names),
                    InvocationDependencyTransactionCommandOutcome::Succeeded,
                    missing_observation()));
            return SourceArtifactInstallTrustedExecutionResult(
                SourceArtifactInstallTrustedExecutionStatus::Missing, 0,
                std::move(expectation), std::move(observation),
                std::nullopt, std::move(operation_result));
        }

        std::vector<PacmanTransactionPackageObservation> operations;
        operations.reserve(receipt->installed_package_names.size());
        for(const std::string& package_name :
            receipt->installed_package_names) {
            operations.push_back(PacmanTransactionPackageObservation{
                PacmanTransactionPackageOperation::Install,
                package_name});
        }
        auto observation = make_observation(
            binding, std::move(input.observed_artifacts),
            make_transaction(
                transaction_token,
                std::move(input.requested_package_names),
                InvocationDependencyTransactionCommandOutcome::Succeeded,
                PacmanTransactionReceiptObservation{
                    PacmanTransactionReceiptObservationState::Complete,
                    transaction_token,
                    InvocationDependencyTransactionOwner::
                        SourceArtifactInstall,
                    std::move(operations)}));
        const bool session_completion_recorded =
            binding == nullptr || !binding->work_item.invocation_authority.has_value() ||
            binding->work_item.invocation_authority
                ->mark_trusted_transaction_completed(
                    InvocationDependencyTransactionOwner::
                        SourceArtifactInstall,
                    transaction_token);
        return SourceArtifactInstallTrustedExecutionResult(
            session_completion_recorded
                ? SourceArtifactInstallTrustedExecutionStatus::Complete
                : SourceArtifactInstallTrustedExecutionStatus::
                      OutcomeUnknown,
            0, std::move(expectation), std::move(observation),
            session_completion_recorded
                ? std::nullopt
                : std::optional<std::string>{
                      "source-artifact cleanup session rejected transaction completion"},
            std::move(operation_result));
    }
};

SourceArtifactInstallTrustedExecutionResult::
    SourceArtifactInstallTrustedExecutionResult(
        SourceArtifactInstallTrustedExecutionStatus status,
        std::optional<int> pacman_exit_status,
        std::optional<SourceArtifactInstallReceiptExpectation> expectation,
        std::optional<SourceArtifactInstallReceiptObservation> observation,
        std::optional<std::string> diagnostic,
        std::optional<PackageBaseArtifactInstallExecutionResult>
            operation_result,
        std::optional<SourceArtifactInstallSealingRefusal> sealing_failure) noexcept
    : status_(status), pacman_exit_status_(pacman_exit_status),
      expectation_(std::move(expectation)),
      observation_(std::move(observation)),
      diagnostic_(std::move(diagnostic)),
      operation_result_(std::move(operation_result)), sealing_failure_(std::move(sealing_failure)) {
}

SourceArtifactInstallTrustedExecutionStatus
SourceArtifactInstallTrustedExecutionResult::status() const noexcept {
    return status_;
}

EvaluatedDevelSourceArtifactTransport::EvaluatedDevelSourceArtifactTransport(
    std::unique_ptr<DevelSourceArtifactInstallState> state) noexcept : state_(std::move(state)) {
}

EvaluatedDevelSourceArtifactTransport::EvaluatedDevelSourceArtifactTransport(
    EvaluatedDevelSourceArtifactTransport&&) noexcept = default;
EvaluatedDevelSourceArtifactTransport::~EvaluatedDevelSourceArtifactTransport() noexcept = default;

bool EvaluatedDevelSourceArtifactTransport::active() const noexcept {
    return state_ && !state_->consumed && state_->proof.valid();
}

const std::optional<std::string>&
EvaluatedDevelSourceArtifactTransport::transaction_token() const {
    if(!state_) throw std::logic_error("evaluated artifact transport is moved from");
    return state_->transaction_token;
}

EvaluatedDevelSourceArtifactTransport prepare_evaluated_devel_source_artifact_transport(
    EvaluatedDevelSourceBuildProof proof) {
    if(!proof.valid()) throw std::logic_error("evaluated build proof is inactive");
    return EvaluatedDevelSourceArtifactTransport(
        std::make_unique<DevelSourceArtifactInstallState>(std::move(proof)));
}

SourceArtifactInstallTrustedExecutionResult EvaluatedDevelSourceArtifactTransport::execute(
    const ArtifactInstallExecutionOptions& options) {
    return execute_impl(options, nullptr);
}

SourceArtifactInstallTrustedExecutionResult EvaluatedDevelSourceArtifactTransport::execute_exact(
    const ArtifactInstallExecutionOptions& options) {
    return execute_exact_impl(options, nullptr);
}

SourceArtifactInstallTrustedExecutionResult EvaluatedDevelSourceArtifactTransport::execute_exact_impl(
    const ArtifactInstallExecutionOptions& options, const std::string* test_token) {
    if(!active()) return SourceArtifactInstallTrustedTransport::invalid_result(
        SourceArtifactInstallTrustedExecutionStatus::InvalidRequest, "evaluated artifact transport is inactive");
    state_->exact_attempted = true;
    try {
        auto execution = execute_impl(options, test_token, true);
        state_->execution.emplace(std::move(execution));
        // Preserve a copy for S5-B callers. If its diagnostic allocation fails,
        // the original result and all fixed operation facts still belong here.
        return *state_->execution;
    } catch(...) {
        if(!state_->execution) state_->execution.emplace(SourceArtifactInstallTrustedTransport::terminal_result(*state_));
        return SourceArtifactInstallTrustedTransport::terminal_result(*state_);
    }
}

std::optional<DevelSourceArtifactInstallResult> EvaluatedDevelSourceArtifactTransport::finalize() noexcept {
    if(!state_ || (state_->consumed && !state_->exact_attempted)) return std::nullopt;
    return DevelSourceArtifactInstallAuthority::finalize(std::move(state_));
}

const ExactArtifactTransactionReceipt* EvaluatedDevelSourceArtifactTransport::exact_receipt() const noexcept {
    return state_ && state_->receipt ? &*state_->receipt : nullptr;
}
std::optional<ExactArtifactReceiptIssue> EvaluatedDevelSourceArtifactTransport::exact_receipt_issue() const noexcept {
    return state_ ? state_->receipt_issue : std::nullopt;
}
const FreshInstalledArtifactBinding* EvaluatedDevelSourceArtifactTransport::fresh_binding() const noexcept {
    return state_ && state_->fresh_binding ? &*state_->fresh_binding : nullptr;
}
std::optional<InstalledRecordObservationIssue> EvaluatedDevelSourceArtifactTransport::installed_binding_issue() const noexcept {
    return state_ ? state_->binding_issue : std::nullopt;
}

SourceArtifactInstallTrustedExecutionResult EvaluatedDevelSourceArtifactTransport::execute_impl(
    const ArtifactInstallExecutionOptions& options, const std::string* test_token, bool exact) {
    using Status = SourceArtifactInstallTrustedExecutionStatus;
    if(!active()) return SourceArtifactInstallTrustedTransport::invalid_result(
        Status::InvalidRequest, "evaluated artifact transport is inactive");
    // The moved-in build capability cannot be recovered/retried after even a
    // local refusal. Keep its FD/context alive through every returned outcome.
    state_->consumed = true;
    if(exact) state_->receipt_issue = ExactArtifactReceiptIssue::Missing;
    if(test_token == nullptr && !fixed_executables_are_trusted())
        return SourceArtifactInstallTrustedTransport::invalid_result(
            Status::TrustedExecutableUnavailable, "installed source-artifact trusted executables are unavailable");
    state_->transaction_token = test_token ? std::optional<std::string>(*test_token)
                                           : generate_trusted_alpm_receipt_transaction_token();
    if(!state_->transaction_token || !is_valid_trusted_alpm_receipt_token(*state_->transaction_token))
        return SourceArtifactInstallTrustedTransport::invalid_result(
            Status::TokenGenerationFailed, "evaluated artifact transport token generation failed");

    bool privileged_attempted = false;
    ExactArtifactTransportCapture exact_capture{*state_, {}, {}, {}, {}, ExactArtifactReceiptIssue::Missing};
    try {
        const auto& artifact = state_->proof.artifact();
        const auto& evidence = artifact.evidence();
        const struct stat metadata = require_snapshot_source(
            artifact.descriptor_, artifact.device_, artifact.inode_, artifact.owner_,
            SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES);
        const auto require_original_entry = [&] {
            struct stat named{};
            // A named-entry check only rejects replacement. It never opens or
            // adopts a new object, even when that object's bytes are identical.
            if(metadata.st_nlink != 1 || static_cast<std::uintmax_t>(metadata.st_size) != artifact.size_ ||
               lstat(artifact.path_.c_str(), &named) == -1 || !same_snapshot_metadata(metadata, named))
                throw std::runtime_error("evaluated artifact retained object was replaced");
        };
        require_original_entry();
        // This comparison MUST precede copying. Hashing whatever is readable
        // now cannot replace the digest saved by the original build proof.
        const std::string digest = xdg_generation_store_file_descriptor_sha256(
            artifact.descriptor_, artifact.size_, SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES);
        if(digest != evidence.archive_digest.value())
            throw std::runtime_error("evaluated artifact differs from the Slice 4 saved archive digest");

        OwnedDescriptor snapshot = create_sealable_memfd();
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
        if(g_evaluated_transport_test_hooks.before_snapshot_copy)
            g_evaluated_transport_test_hooks.before_snapshot_copy(artifact.descriptor_);
#endif
        append_descriptor_bytes(artifact.descriptor_, metadata, snapshot.get());
        require_original_entry();
        const auto& identity = evidence.identity;
        const auto* package_base = identity.package_base.value();
        const auto* architecture = identity.architecture.value();
        if(identity.package_base.state() != ArtifactMetadataValueState::Known ||
           identity.architecture.state() != ArtifactMetadataValueState::Known ||
           !package_base || !architecture)
            throw std::runtime_error("evaluated artifact identity is incomplete");
        // Slice 4 accepts exactly one archive and no detached signature.
        // PreserveExistingReason adds no cleanup-specific --asdeps authority;
        // needed=false installs the proved build without introducing skip policy.
        SourceArtifactInstallRootPrepareRequest request{
            *state_->transaction_token, *package_base, SourceArtifactInstallTrustedDirective::PreserveExistingReason, false, options.no_confirm, {{0, identity.package_name, identity.full_version, *package_base, *architecture, static_cast<std::uint64_t>(artifact.size_), 0, digest, "-"}}};
        if(exact) {
            request.purpose = SourceArtifactInstallTrustedPurpose::ExactInstalledBinding;
            request.artifacts.front().raw_mtree_sha256 = evidence.mtree_digest.value();
            exact_capture.manifest = request;
        }
        seal_snapshot(snapshot.get(), request);
        // Close hash-to-copy mutation as well: the immutable bytes sent across
        // privilege must carry the very same saved digest, not just a new hash.
        if(xdg_generation_store_file_descriptor_sha256(
               snapshot.get(), artifact.size_, SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES) != digest)
            throw std::runtime_error("evaluated artifact snapshot differs from the Slice 4 saved archive digest");
        PreparedTransportInput input{
            std::move(snapshot), std::move(request), {}, {identity.package_name}};
        privileged_attempted = true;
        auto execution = SourceArtifactInstallTrustedTransport::execute_snapshot(
            std::move(input), *state_->transaction_token, nullptr, nullptr, std::nullopt, exact ? &exact_capture : nullptr);
        if(exact_capture.known_success()) {
            state_->receipt_issue = exact_capture.issue;
            if(exact_capture.evidence) {
                state_->receipt.emplace(ExactArtifactTransactionReceipt(std::move(*exact_capture.manifest), std::move(exact_capture.stage),
                                                                        std::move(exact_capture.records), std::move(*exact_capture.evidence),
                                                                        state_->proof.lineage_, state_->transaction_lineage));
                state_->receipt_issue.reset();
                auto observed = InstalledArtifactBindingObserver::observe(*state_->receipt, state_->proof);
                if(auto* binding = std::get_if<FreshInstalledArtifactBinding>(&observed)) {
                    state_->fresh_binding.emplace(std::move(*binding));
                    state_->fresh_binding_count = 1;
                } else
                    state_->binding_issue = std::get<FreshInstalledArtifactBindingFailure>(observed).reason;
            }
        }
        return execution;
    } catch(const std::exception& error) {
        if(exact_capture.known_success()) {
            if(state_->receipt)
                state_->binding_issue = InstalledRecordObservationIssue::ResourceFailure;
            else
                state_->receipt_issue = ExactArtifactReceiptIssue::ResourceFailure;
            return SourceArtifactInstallTrustedTransport::exact_postprocessing_failure(state_->receipt.has_value());
        }
        if(exact && privileged_attempted)
            return SourceArtifactInstallTrustedTransport::terminal_result(*state_);
        if(privileged_attempted) return SourceArtifactInstallTrustedTransport::unknown_after_consumption(
            nullptr, *state_->transaction_token);
        return SourceArtifactInstallTrustedTransport::invalid_result(Status::ArtifactSnapshotFailed, error.what());
    } catch(...) {
        if(exact_capture.known_success()) {
            if(state_->receipt)
                state_->binding_issue = InstalledRecordObservationIssue::ResourceFailure;
            else
                state_->receipt_issue = ExactArtifactReceiptIssue::ResourceFailure;
            return SourceArtifactInstallTrustedTransport::exact_postprocessing_failure(state_->receipt.has_value());
        }
        if(exact && privileged_attempted)
            return SourceArtifactInstallTrustedTransport::terminal_result(*state_);
        if(privileged_attempted) return SourceArtifactInstallTrustedTransport::unknown_after_consumption(
            nullptr, *state_->transaction_token);
        return SourceArtifactInstallTrustedTransport::invalid_result(
            Status::ArtifactSnapshotFailed, "evaluated artifact immutable snapshot failed");
    }
}

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
SourceArtifactInstallTrustedExecutionResult EvaluatedDevelSourceArtifactTransport::execute_for_test(
    const ArtifactInstallExecutionOptions& options, const std::string& transaction_token) {
    return execute_impl(options, &transaction_token);
}

SourceArtifactInstallTrustedExecutionResult EvaluatedDevelSourceArtifactTransport::execute_exact_for_test(
    const ArtifactInstallExecutionOptions& options, const std::string& transaction_token) {
    return execute_exact_impl(options, &transaction_token);
}

void set_evaluated_devel_source_artifact_transport_test_hooks(
    EvaluatedDevelSourceArtifactTransportTestHooks hooks) {
    g_evaluated_transport_test_hooks = std::move(hooks);
}
#endif

const std::optional<int>&
SourceArtifactInstallTrustedExecutionResult::pacman_exit_status()
    const noexcept {
    return pacman_exit_status_;
}

const std::optional<SourceArtifactInstallReceiptExpectation>&
SourceArtifactInstallTrustedExecutionResult::expectation() const noexcept {
    return expectation_;
}

const std::optional<SourceArtifactInstallReceiptObservation>&
SourceArtifactInstallTrustedExecutionResult::observation() const noexcept {
    return observation_;
}

const std::optional<PackageBaseArtifactInstallExecutionResult>&
SourceArtifactInstallTrustedExecutionResult::operation_result()
    const noexcept {
    return operation_result_;
}

const std::optional<std::string>&
SourceArtifactInstallTrustedExecutionResult::diagnostic() const noexcept {
    return diagnostic_;
}

const std::optional<SourceArtifactInstallSealingRefusal>&
SourceArtifactInstallTrustedExecutionResult::sealing_failure() const noexcept {
    return sealing_failure_;
}

SourceArtifactInstallTrustedExecutionResult
execute_source_artifact_install_trusted_transaction(
    PreparedPackageBaseArtifactInstall& install,
    const SourceArtifactInstallTrustedBinding& binding,
    const ArtifactInstallExecutionOptions& options) {
    if(!SourceArtifactInstallTrustedTransport::request_is_valid(
           install, binding)) {
        return SourceArtifactInstallTrustedTransport::invalid_result(
            SourceArtifactInstallTrustedExecutionStatus::InvalidRequest,
            "source-artifact trusted request is invalid");
    }
    if(!fixed_executables_are_trusted()) {
        return SourceArtifactInstallTrustedTransport::invalid_result(
            SourceArtifactInstallTrustedExecutionStatus::
                TrustedExecutableUnavailable,
            "installed source-artifact trusted executables are unavailable");
    }
    const std::optional<std::string> transaction_token =
        generate_trusted_alpm_receipt_transaction_token();
    if(!transaction_token.has_value()) {
        return SourceArtifactInstallTrustedTransport::invalid_result(
            SourceArtifactInstallTrustedExecutionStatus::
                TokenGenerationFailed,
            "cryptographic source-artifact transaction token generation failed");
    }
    if(!SourceArtifactInstallTrustedTransport::register_transaction_token(
           binding, *transaction_token)) {
        return SourceArtifactInstallTrustedTransport::invalid_result(
            SourceArtifactInstallTrustedExecutionStatus::InvalidRequest,
            "source-artifact cleanup session rejected the transaction token");
    }
    try {
        return SourceArtifactInstallTrustedTransport::execute(
            install, binding, options, *transaction_token);
    } catch(...) {
        if(!SourceArtifactInstallTrustedTransport::capability_is_active(
               install)) {
            return SourceArtifactInstallTrustedTransport::
                unknown_after_consumption(
                    &binding, *transaction_token);
        }
        return SourceArtifactInstallTrustedTransport::invalid_result(
            SourceArtifactInstallTrustedExecutionStatus::
                ArtifactSnapshotFailed,
            "source-artifact immutable snapshot failed before root preparation");
    }
}

SourceArtifactInstallTrustedExecutionResult
execute_source_artifact_install_trusted_transaction(
    PreparedPackageBaseArtifactInstall& install,
    CleanupInvocationAuthority invocation_authority,
    std::size_t work_item_index,
    const ArtifactInstallExecutionOptions& options) {
    std::optional<SourceArtifactInstallTrustedBinding> binding;
    try {
        binding = SourceArtifactInstallTrustedTransport::
            project_session_binding(
                install, invocation_authority, work_item_index);
    } catch(...) {
        binding.reset();
    }
    if(!binding.has_value()) {
        return SourceArtifactInstallTrustedTransport::invalid_result(
            SourceArtifactInstallTrustedExecutionStatus::InvalidRequest,
            "source-artifact cleanup session binding is invalid");
    }
    return execute_source_artifact_install_trusted_transaction(
        install, *binding, options);
}

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
SourceArtifactInstallTrustedExecutionResult
execute_source_artifact_install_trusted_transaction_for_test(
    PreparedPackageBaseArtifactInstall& install,
    const SourceArtifactInstallTrustedBinding& binding,
    const ArtifactInstallExecutionOptions& options,
    const std::string& transaction_token) {
    if(!is_valid_trusted_alpm_receipt_token(transaction_token) ||
       !SourceArtifactInstallTrustedTransport::request_is_valid(
           install, binding)) {
        return SourceArtifactInstallTrustedTransport::invalid_result(
            SourceArtifactInstallTrustedExecutionStatus::InvalidRequest,
            "source-artifact trusted test token is invalid");
    }
    if(!SourceArtifactInstallTrustedTransport::register_transaction_token(
           binding, transaction_token)) {
        return SourceArtifactInstallTrustedTransport::invalid_result(
            SourceArtifactInstallTrustedExecutionStatus::InvalidRequest,
            "source-artifact cleanup test session rejected the transaction token");
    }
    try {
        return SourceArtifactInstallTrustedTransport::execute(
            install, binding, options, transaction_token);
    } catch(...) {
        if(!SourceArtifactInstallTrustedTransport::capability_is_active(
               install)) {
            return SourceArtifactInstallTrustedTransport::
                unknown_after_consumption(&binding, transaction_token);
        }
        return SourceArtifactInstallTrustedTransport::invalid_result(
            SourceArtifactInstallTrustedExecutionStatus::
                ArtifactSnapshotFailed,
            "source-artifact immutable snapshot failed before root preparation");
    }
}
#endif
