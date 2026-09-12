#include "xdg_directory_safety.hpp"

#include "application_identity.hpp"
#include "localization.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace xdg_directory_safety {
namespace {

namespace fs = std::filesystem;

#if defined(MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS) || defined(MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS)
ManagedParentSyncTestHook g_managed_parent_sync_hook = nullptr;
#endif

constexpr mode_t NEW_DIRECTORY_MODE = 0700;
constexpr mode_t REQUIRED_OWNER_PERMISSIONS = S_IRUSR | S_IWUSR | S_IXUSR;
constexpr mode_t FORBIDDEN_WRITE_PERMISSIONS = S_IWGRP | S_IWOTH;
constexpr std::string_view SOURCE_PREFERENCE_DIRECTORY_NAME =
    "source-build.d";

struct DirectoryRequest {
    xdg_paths::DirectoryKind directory_kind;
    const fs::path& directory;
    const xdg_paths::DirectoryCreationBoundary& creation_boundary;
    bool derived_paths_match;
    std::vector<std::string> extra_managed_components;
    bool final_directory_requires_private_mode = false;
};

enum class MissingManagedComponentPolicy {
    Create,
    ReturnAbsent,
};

class OwnedFileDescriptor final {
    int descriptor_ = -1;

public:
    explicit OwnedFileDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }

    OwnedFileDescriptor(const OwnedFileDescriptor&) = delete;
    OwnedFileDescriptor& operator=(const OwnedFileDescriptor&) = delete;

    OwnedFileDescriptor(OwnedFileDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }

    OwnedFileDescriptor& operator=(OwnedFileDescriptor&& other) noexcept {
        if(this == &other) return *this;
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }

    ~OwnedFileDescriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
    }

    int get() const noexcept {
        return descriptor_;
    }

    int release() noexcept {
        return std::exchange(descriptor_, -1);
    }
};

struct OpenedDirectory {
    OwnedFileDescriptor descriptor;
    struct stat status{};
};

struct RetainedDirectoryState {
    OwnedFileDescriptor descriptor;
    std::string leaf_name;
    struct stat status{};
    bool requires_security_validation = false;
    bool requires_private_mode = false;
};

struct PreparedDirectoryState {
    OwnedFileDescriptor parent_descriptor;
    OwnedFileDescriptor directory_descriptor;
    std::string leaf_name;
    struct stat status{};
    std::uintmax_t observed_owner = 0;
    std::size_t created_component_count = 0;
    std::vector<RetainedDirectoryState> retained_lineage;
};

#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
using TestOverrides = DirectorySafetyTestOverrides;
#else
struct TestOverrides final {};
#endif

std::string_view directory_kind_name(
    xdg_paths::DirectoryKind directory_kind) {
    // NO_TRANSLATE: These values are stable XDG directory-kind tokens and are
    // passed as runtime data to complete diagnostic msgids.
    switch(directory_kind) {
        case xdg_paths::DirectoryKind::Config:
            return "config";
        case xdg_paths::DirectoryKind::State:
            return "state";
        case xdg_paths::DirectoryKind::Cache:
            return "cache";
    }
    throw std::logic_error(localization::format_translated_message(
        "Unknown {} directory kind.", "XDG"));
}

std::string_view preparation_stage_name(PreparationStage stage) {
    // NO_TRANSLATE: These values are stable internal stage tokens and are
    // passed as runtime data to complete diagnostic msgids.
    switch(stage) {
        case PreparationStage::BoundaryValidation:
            return "boundary-validation";
        case PreparationStage::FilesystemRootOpen:
            return "filesystem-root-open";
        case PreparationStage::AnchorTraversal:
            return "anchor-traversal";
        case PreparationStage::AnchorValidation:
            return "anchor-validation";
        case PreparationStage::ComponentInspection:
            return "component-inspection";
        case PreparationStage::ComponentCreation:
            return "component-creation";
        case PreparationStage::ComponentOpen:
            return "component-open";
        case PreparationStage::ComponentValidation:
            return "component-validation";
        case PreparationStage::DirectoryRevalidation:
            return "directory-revalidation";
    }
    throw std::logic_error(localization::format_translated_message(
        "Unknown {} directory preparation stage.", "XDG"));
}

std::string preparation_diagnostic(const PreparationFailure& failure) {
    std::string diagnostic;
    switch(failure.code) {
        case PreparationErrorCode::MissingAnchor:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: the required existing anchor is missing.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
        case PreparationErrorCode::Symlink:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: a symlink component is not allowed.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
        case PreparationErrorCode::NotDirectory:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: a path component is not a directory.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
        case PreparationErrorCode::OwnershipMismatch:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: directory ownership does not match the effective user.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
        case PreparationErrorCode::UnsafePermissions:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: directory permissions are unsafe.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
        case PreparationErrorCode::PermissionDenied:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: filesystem permission was denied.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
        case PreparationErrorCode::CreationFailed:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: directory creation failed.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
        case PreparationErrorCode::MetadataFailure:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: filesystem metadata could not be obtained safely.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
        case PreparationErrorCode::ConcurrentReplacement:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: a path component changed during validation.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
        case PreparationErrorCode::InvalidCreationBoundary:
            // TRANSLATORS: The placeholders are the project identity, an XDG directory-kind token, and a stable stage token.
            diagnostic = localization::format_translated_message(
                "Cannot prepare the {} {} directory during stage {}: the resolved creation boundary is invalid.",
                application_identity::PROJECT_NAME,
                directory_kind_name(failure.directory_kind),
                preparation_stage_name(failure.stage));
            break;
    }
    if(diagnostic.empty()) {
        throw std::logic_error(localization::format_translated_message(
            "Unknown {} directory preparation error code.", "XDG"));
    }
    if(failure.system_error.has_value()) {
        // TRANSLATORS: The placeholder is an operating-system error message.
        diagnostic += " " + localization::format_translated_message(
                                "System error: {}.", failure.system_error->message());
    }
    return diagnostic;
}

[[noreturn]] void throw_preparation_error(
    xdg_paths::DirectoryKind directory_kind,
    PreparationStage stage,
    PreparationErrorCode code,
    std::optional<int> error_number = std::nullopt,
    std::optional<std::size_t> component_index = std::nullopt) {
    std::optional<std::error_code> system_error;
    if(error_number.has_value()) {
        system_error =
            std::error_code(error_number.value(), std::generic_category());
    }
    throw PreparationError(PreparationFailure{
        directory_kind, stage, code, system_error,
        component_index});
}

bool has_ambiguous_leading_double_slash(const fs::path& path) {
    const std::string& value = path.native();
    return value.size() >= 2 && value[0] == '/' && value[1] == '/' &&
           (value.size() == 2 || value[2] != '/');
}

bool is_normalized_absolute_authority_path(const fs::path& path) {
    if(!path.is_absolute() || path.root_path() != fs::path("/")) return false;
    if(path.native().find('\0') != std::string::npos ||
       has_ambiguous_leading_double_slash(path)) {
        return false;
    }
    for(const auto& component : path.relative_path()) {
        if(component == "." || component == "..") return false;
    }
    try {
        return path.lexically_normal() == path;
    } catch(const fs::filesystem_error&) {
        return false;
    }
}

bool is_safe_leaf_name(const std::string& component) {
    if(component.empty() || component == "." || component == ".." ||
       component.find('\0') != std::string::npos) {
        return false;
    }
    const fs::path path(component);
    return path.is_relative() && path.has_filename() &&
           path.parent_path().empty() && path.filename().string() == component;
}

std::vector<std::string> expected_fallback_components(
    xdg_paths::DirectoryKind directory_kind,
    const std::vector<std::string>& extra_managed_components) {
    const std::string application_component(application_identity::XDG_IDENTITY);
    std::vector<std::string> components;
    switch(directory_kind) {
        case xdg_paths::DirectoryKind::Config:
            components = {".config", application_component};
            break;
        case xdg_paths::DirectoryKind::State:
            components = {".local", "state", application_component};
            break;
        case xdg_paths::DirectoryKind::Cache:
            components = {".cache", application_component};
            break;
    }
    if(components.empty()) {
        throw std::logic_error(localization::format_translated_message(
            "Unknown {} directory kind.", "XDG"));
    }
    components.insert(
        components.end(), extra_managed_components.begin(),
        extra_managed_components.end());
    return components;
}

std::vector<std::string> expected_explicit_components(
    const std::vector<std::string>& extra_managed_components) {
    std::vector<std::string> components{
        std::string(application_identity::XDG_IDENTITY)};
    components.insert(
        components.end(), extra_managed_components.begin(),
        extra_managed_components.end());
    return components;
}

void validate_creation_boundary(const DirectoryRequest& request) {
    const xdg_paths::DirectoryCreationBoundary& boundary =
        request.creation_boundary;
    const fs::path filesystem_root("/");

    if(!request.derived_paths_match ||
       !is_normalized_absolute_authority_path(request.directory) ||
       !is_normalized_absolute_authority_path(boundary.base_directory) ||
       !is_normalized_absolute_authority_path(boundary.existing_anchor) ||
       boundary.existing_anchor == filesystem_root ||
       boundary.creatable_components.empty()) {
        throw_preparation_error(
            request.directory_kind,
            PreparationStage::BoundaryValidation,
            PreparationErrorCode::InvalidCreationBoundary);
    }

    for(const std::string& component : boundary.creatable_components) {
        if(!is_safe_leaf_name(component)) {
            throw_preparation_error(
                request.directory_kind,
                PreparationStage::BoundaryValidation,
                PreparationErrorCode::InvalidCreationBoundary);
        }
    }

    fs::path reconstructed_directory = boundary.existing_anchor;
    for(const std::string& component : boundary.creatable_components)
        reconstructed_directory /= component;

    fs::path reconstructed_base = boundary.existing_anchor;
    const std::size_t managed_component_count =
        1 + request.extra_managed_components.size();
    if(boundary.creatable_components.size() < managed_component_count) {
        throw_preparation_error(
            request.directory_kind,
            PreparationStage::BoundaryValidation,
            PreparationErrorCode::InvalidCreationBoundary);
    }
    const std::size_t base_component_count =
        boundary.creatable_components.size() - managed_component_count;
    for(std::size_t index = 0; index < base_component_count; ++index) {
        reconstructed_base /= boundary.creatable_components[index];
    }

    if(reconstructed_directory != request.directory ||
       reconstructed_base != boundary.base_directory) {
        throw_preparation_error(
            request.directory_kind,
            PreparationStage::BoundaryValidation,
            PreparationErrorCode::InvalidCreationBoundary);
    }

    if(boundary.source == xdg_paths::DirectorySource::ExplicitXdg) {
        if(boundary.existing_anchor != boundary.base_directory ||
           boundary.creatable_components !=
               expected_explicit_components(
                   request.extra_managed_components)) {
            throw_preparation_error(
                request.directory_kind,
                PreparationStage::BoundaryValidation,
                PreparationErrorCode::InvalidCreationBoundary);
        }
        return;
    }

    if(boundary.source != xdg_paths::DirectorySource::HomeFallback ||
       boundary.creatable_components !=
           expected_fallback_components(
               request.directory_kind,
               request.extra_managed_components)) {
        throw_preparation_error(
            request.directory_kind,
            PreparationStage::BoundaryValidation,
            PreparationErrorCode::InvalidCreationBoundary);
    }
}

bool is_permission_error(int error_number) {
    return error_number == EACCES || error_number == EPERM ||
           error_number == EROFS;
}

bool is_replacement_error(int error_number) {
    return error_number == ENOENT || error_number == ENOTDIR ||
           error_number == ELOOP;
}

std::uintmax_t effective_user(const TestOverrides* overrides) {
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
    if(overrides != nullptr && overrides->effective_user.has_value())
        return overrides->effective_user.value();
#else
    static_cast<void>(overrides);
#endif
    return static_cast<std::uintmax_t>(geteuid());
}

std::uintmax_t observed_owner(
    const struct stat& status, const TestOverrides* overrides) {
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
    if(overrides != nullptr && overrides->observed_owner.has_value())
        return overrides->observed_owner.value();
#else
    static_cast<void>(overrides);
#endif
    return static_cast<std::uintmax_t>(status.st_uid);
}

#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
bool inject_failure(
    const TestOverrides* overrides,
    DirectorySafetyTestFailurePoint failure_point,
    std::size_t component_index) {
    if(overrides == nullptr || !overrides->injected_failure.has_value())
        return false;
    const DirectorySafetyInjectedFailure& failure =
        overrides->injected_failure.value();
    if(failure.failure_point != failure_point ||
       failure.component_index != component_index) {
        return false;
    }
    errno = failure.error_number;
    return true;
}

void emit_test_event(
    const TestOverrides* overrides, DirectorySafetyTestEvent event,
    xdg_paths::DirectoryKind directory_kind, std::size_t component_index,
    const fs::path& path) {
    if(overrides != nullptr && overrides->event_hook)
        overrides->event_hook(event, directory_kind, component_index, path);
}
#else
void emit_test_event(
    const TestOverrides*, xdg_paths::DirectoryKind, std::size_t,
    const fs::path&) {
}
#endif

bool same_filesystem_identity(
    const struct stat& expected, const struct stat& actual) {
    return expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino &&
           (expected.st_mode & S_IFMT) == (actual.st_mode & S_IFMT);
}

void validate_directory_type(
    const struct stat& status,
    xdg_paths::DirectoryKind directory_kind,
    PreparationStage stage,
    std::size_t component_index,
    bool replacement_is_possible) {
    if(S_ISLNK(status.st_mode)) {
        throw_preparation_error(
            directory_kind, stage,
            replacement_is_possible
                ? PreparationErrorCode::ConcurrentReplacement
                : PreparationErrorCode::Symlink,
            std::nullopt, component_index);
    }
    if(!S_ISDIR(status.st_mode)) {
        throw_preparation_error(
            directory_kind, stage,
            replacement_is_possible
                ? PreparationErrorCode::ConcurrentReplacement
                : PreparationErrorCode::NotDirectory,
            std::nullopt, component_index);
    }
}

void validate_directory_security(
    const struct stat& status,
    xdg_paths::DirectoryKind directory_kind,
    PreparationStage stage,
    std::size_t component_index,
    std::uintmax_t expected_effective_user,
    const TestOverrides* overrides,
    bool was_created,
    bool requires_private_mode) {
    if(observed_owner(status, overrides) != expected_effective_user) {
        throw_preparation_error(
            directory_kind, stage,
            PreparationErrorCode::OwnershipMismatch,
            std::nullopt, component_index);
    }

    const mode_t permissions = status.st_mode & 07777;
    const bool has_required_owner_permissions =
        (permissions & REQUIRED_OWNER_PERMISSIONS) ==
        REQUIRED_OWNER_PERMISSIONS;
    const bool has_forbidden_write_permissions =
        (permissions & FORBIDDEN_WRITE_PERMISSIONS) != 0;
    const bool has_expected_mode =
        (!was_created && !requires_private_mode) ||
        permissions == NEW_DIRECTORY_MODE;
    if(!has_required_owner_permissions || has_forbidden_write_permissions ||
       !has_expected_mode) {
        throw_preparation_error(
            directory_kind, stage,
            PreparationErrorCode::UnsafePermissions,
            std::nullopt, component_index);
    }
}

OpenedDirectory open_observed_directory(
    int parent_descriptor, const std::string& leaf_name,
    const struct stat& observed_status,
    xdg_paths::DirectoryKind directory_kind,
    PreparationStage open_stage,
    PreparationStage validation_stage,
    std::size_t component_index,
    std::uintmax_t expected_effective_user,
    const TestOverrides* overrides,
    bool allow_test_injection,
    bool validate_security,
    bool was_created,
    bool requires_private_mode) {
#ifndef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
    static_cast<void>(allow_test_injection);
#endif
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
    const bool injected_open_failure =
        allow_test_injection && inject_failure(
                                    overrides,
                                    DirectorySafetyTestFailurePoint::ComponentOpen,
                                    component_index);
#else
    const bool injected_open_failure = false;
#endif
    const int descriptor = injected_open_failure
                               ? -1
                               : openat(
                                     parent_descriptor,
                                     leaf_name.c_str(),
                                     O_PATH | O_DIRECTORY | O_CLOEXEC |
                                         O_NOFOLLOW);
    if(descriptor < 0) {
        const int open_error = errno;
        if(is_permission_error(open_error)) {
            throw_preparation_error(
                directory_kind, open_stage,
                PreparationErrorCode::PermissionDenied, open_error,
                component_index);
        }
        throw_preparation_error(
            directory_kind, open_stage,
            is_replacement_error(open_error)
                ? PreparationErrorCode::ConcurrentReplacement
                : PreparationErrorCode::MetadataFailure,
            open_error, component_index);
    }
    OwnedFileDescriptor opened(descriptor);

    struct stat opened_status{};
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
    const bool injected_metadata_failure =
        allow_test_injection && inject_failure(
                                    overrides,
                                    DirectorySafetyTestFailurePoint::DescriptorMetadata,
                                    component_index);
#else
    const bool injected_metadata_failure = false;
#endif
    if(injected_metadata_failure || fstat(opened.get(), &opened_status) != 0) {
        const int metadata_error = errno;
        throw_preparation_error(
            directory_kind, validation_stage,
            is_permission_error(metadata_error)
                ? PreparationErrorCode::PermissionDenied
                : PreparationErrorCode::MetadataFailure,
            metadata_error, component_index);
    }
    if(!same_filesystem_identity(observed_status, opened_status)) {
        throw_preparation_error(
            directory_kind, validation_stage,
            PreparationErrorCode::ConcurrentReplacement,
            std::nullopt, component_index);
    }

    struct stat revalidated_status{};
    if(fstatat(
           parent_descriptor, leaf_name.c_str(), &revalidated_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_preparation_error(
            directory_kind, validation_stage,
            is_replacement_error(metadata_error)
                ? PreparationErrorCode::ConcurrentReplacement
                : (is_permission_error(metadata_error)
                       ? PreparationErrorCode::PermissionDenied
                       : PreparationErrorCode::MetadataFailure),
            metadata_error, component_index);
    }
    if(!same_filesystem_identity(opened_status, revalidated_status)) {
        throw_preparation_error(
            directory_kind, validation_stage,
            PreparationErrorCode::ConcurrentReplacement,
            std::nullopt, component_index);
    }

    validate_directory_type(
        opened_status, directory_kind, validation_stage,
        component_index, true);
    if(validate_security) {
        validate_directory_security(
            opened_status, directory_kind, validation_stage,
            component_index, expected_effective_user, overrides,
            was_created, requires_private_mode);
        validate_directory_security(
            revalidated_status, directory_kind, validation_stage,
            component_index, expected_effective_user, overrides,
            was_created, requires_private_mode);
    }
    return OpenedDirectory{std::move(opened), opened_status};
}

OwnedFileDescriptor duplicate_lineage_descriptor(
    int descriptor, xdg_paths::DirectoryKind directory_kind,
    PreparationStage stage, std::size_t component_index) {
    const int duplicated = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    if(duplicated < 0) {
        const int duplication_error = errno;
        throw_preparation_error(
            directory_kind, stage,
            is_permission_error(duplication_error)
                ? PreparationErrorCode::PermissionDenied
                : PreparationErrorCode::MetadataFailure,
            duplication_error, component_index);
    }
    return OwnedFileDescriptor(duplicated);
}

RetainedDirectoryState retain_directory_identity(
    int descriptor, std::string leaf_name,
    const struct stat& status,
    xdg_paths::DirectoryKind directory_kind,
    PreparationStage stage, std::size_t component_index,
    bool requires_security_validation,
    bool requires_private_mode) {
    return RetainedDirectoryState{
        duplicate_lineage_descriptor(
            descriptor, directory_kind, stage, component_index),
        std::move(leaf_name), status,
        requires_security_validation, requires_private_mode};
}

void require_retained_lineage_unchanged(
    const std::vector<RetainedDirectoryState>& retained_lineage,
    const DirectoryRequest& request,
    std::uintmax_t expected_effective_user,
    const TestOverrides* overrides) {
    if(retained_lineage.empty()) {
        throw_preparation_error(
            request.directory_kind,
            PreparationStage::DirectoryRevalidation,
            PreparationErrorCode::MetadataFailure);
    }

    for(std::size_t index = 0; index < retained_lineage.size(); ++index) {
        const RetainedDirectoryState& identity = retained_lineage[index];
        if(identity.descriptor.get() < 0 ||
           (index == 0 ? !identity.leaf_name.empty()
                       : identity.leaf_name.empty())) {
            throw_preparation_error(
                request.directory_kind,
                PreparationStage::DirectoryRevalidation,
                PreparationErrorCode::MetadataFailure,
                std::nullopt, index);
        }

        struct stat retained_status{};
        if(fstat(identity.descriptor.get(), &retained_status) != 0) {
            const int metadata_error = errno;
            throw_preparation_error(
                request.directory_kind,
                PreparationStage::DirectoryRevalidation,
                is_permission_error(metadata_error)
                    ? PreparationErrorCode::PermissionDenied
                    : PreparationErrorCode::MetadataFailure,
                metadata_error, index);
        }
        if(!S_ISDIR(retained_status.st_mode) ||
           !same_filesystem_identity(identity.status, retained_status)) {
            throw_preparation_error(
                request.directory_kind,
                PreparationStage::DirectoryRevalidation,
                PreparationErrorCode::ConcurrentReplacement,
                std::nullopt, index);
        }

        if(index == 0) continue;

        const RetainedDirectoryState& parent = retained_lineage[index - 1];
        struct stat named_status{};
        if(fstatat(
               parent.descriptor.get(), identity.leaf_name.c_str(),
               &named_status, AT_SYMLINK_NOFOLLOW) != 0) {
            const int metadata_error = errno;
            throw_preparation_error(
                request.directory_kind,
                PreparationStage::DirectoryRevalidation,
                is_replacement_error(metadata_error)
                    ? PreparationErrorCode::ConcurrentReplacement
                    : (is_permission_error(metadata_error)
                           ? PreparationErrorCode::PermissionDenied
                           : PreparationErrorCode::MetadataFailure),
                metadata_error, index);
        }
        if(!S_ISDIR(named_status.st_mode) ||
           !same_filesystem_identity(retained_status, named_status)) {
            throw_preparation_error(
                request.directory_kind,
                PreparationStage::DirectoryRevalidation,
                PreparationErrorCode::ConcurrentReplacement,
                std::nullopt, index);
        }
        if(identity.requires_security_validation) {
            validate_directory_security(
                retained_status, request.directory_kind,
                PreparationStage::DirectoryRevalidation, index,
                expected_effective_user, overrides, false,
                identity.requires_private_mode);
            validate_directory_security(
                named_status, request.directory_kind,
                PreparationStage::DirectoryRevalidation, index,
                expected_effective_user, overrides, false,
                identity.requires_private_mode);
        }
    }
}

OpenedDirectory open_existing_anchor(
    const DirectoryRequest& request,
    std::uintmax_t expected_effective_user,
    const TestOverrides* overrides,
    std::vector<RetainedDirectoryState>& retained_lineage) {
    const int root_descriptor = open(
        "/", O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(root_descriptor < 0) {
        const int open_error = errno;
        throw_preparation_error(
            request.directory_kind,
            PreparationStage::FilesystemRootOpen,
            is_permission_error(open_error)
                ? PreparationErrorCode::PermissionDenied
                : PreparationErrorCode::MetadataFailure,
            open_error);
    }
    OwnedFileDescriptor current_directory(root_descriptor);
    struct stat current_status{};
    if(fstat(current_directory.get(), &current_status) != 0) {
        const int metadata_error = errno;
        throw_preparation_error(
            request.directory_kind,
            PreparationStage::FilesystemRootOpen,
            is_permission_error(metadata_error)
                ? PreparationErrorCode::PermissionDenied
                : PreparationErrorCode::MetadataFailure,
            metadata_error);
    }
    validate_directory_type(
        current_status, request.directory_kind,
        PreparationStage::FilesystemRootOpen, 0, true);
    retained_lineage.push_back(retain_directory_identity(
        current_directory.get(), {}, current_status,
        request.directory_kind,
        PreparationStage::FilesystemRootOpen, 0, false, false));

    fs::path current_path("/");
    std::size_t component_index = 0;
    for(const auto& path_component :
        request.creation_boundary.existing_anchor.relative_path()) {
        const std::string leaf_name = path_component.string();
        struct stat observed_status{};
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
        const bool injected_metadata_failure =
            inject_failure(
                overrides,
                DirectorySafetyTestFailurePoint::AnchorMetadata,
                component_index);
#else
        const bool injected_metadata_failure = false;
#endif
        if(injected_metadata_failure ||
           fstatat(
               current_directory.get(), leaf_name.c_str(),
               &observed_status, AT_SYMLINK_NOFOLLOW) != 0) {
            const int metadata_error = errno;
            if(metadata_error == ENOENT) {
                throw_preparation_error(
                    request.directory_kind,
                    PreparationStage::AnchorTraversal,
                    PreparationErrorCode::MissingAnchor,
                    std::nullopt, component_index);
            }
            throw_preparation_error(
                request.directory_kind,
                PreparationStage::AnchorTraversal,
                is_permission_error(metadata_error)
                    ? PreparationErrorCode::PermissionDenied
                    : PreparationErrorCode::MetadataFailure,
                metadata_error, component_index);
        }
        current_path /= leaf_name;
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
        emit_test_event(
            overrides, DirectorySafetyTestEvent::AfterAnchorMetadata,
            request.directory_kind, component_index, current_path);
#else
        emit_test_event(
            overrides, request.directory_kind, component_index,
            current_path);
#endif
        validate_directory_type(
            observed_status, request.directory_kind,
            PreparationStage::AnchorTraversal, component_index, false);

        const bool is_final_anchor_component =
            current_path ==
            request.creation_boundary.existing_anchor;
        OpenedDirectory opened = open_observed_directory(
            current_directory.get(), leaf_name, observed_status,
            request.directory_kind,
            PreparationStage::AnchorTraversal,
            is_final_anchor_component
                ? PreparationStage::AnchorValidation
                : PreparationStage::AnchorTraversal,
            component_index,
            expected_effective_user, overrides, false,
            is_final_anchor_component,
            false, false);
        retained_lineage.push_back(retain_directory_identity(
            opened.descriptor.get(), leaf_name, opened.status,
            request.directory_kind,
            is_final_anchor_component
                ? PreparationStage::AnchorValidation
                : PreparationStage::AnchorTraversal,
            component_index, is_final_anchor_component, false));
        current_directory = std::move(opened.descriptor);
        current_status = opened.status;
        ++component_index;
    }

    validate_directory_security(
        current_status, request.directory_kind,
        PreparationStage::AnchorValidation,
        component_index == 0 ? 0 : component_index - 1,
        expected_effective_user, overrides, false, false);
    return OpenedDirectory{std::move(current_directory), current_status};
}

std::optional<struct stat> inspect_managed_component(
    int parent_descriptor, const std::string& leaf_name,
    const DirectoryRequest& request, std::size_t component_index,
    const TestOverrides* overrides) {
#ifndef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
    static_cast<void>(overrides);
#endif
    struct stat observed_status{};
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
    const bool injected_metadata_failure =
        inject_failure(
            overrides,
            DirectorySafetyTestFailurePoint::ManagedMetadata,
            component_index);
#else
    const bool injected_metadata_failure = false;
#endif
    if(!injected_metadata_failure &&
       fstatat(
           parent_descriptor, leaf_name.c_str(), &observed_status,
           AT_SYMLINK_NOFOLLOW) == 0) {
        return observed_status;
    }

    const int metadata_error = errno;
    if(metadata_error == ENOENT) return std::nullopt;
    throw_preparation_error(
        request.directory_kind,
        PreparationStage::ComponentInspection,
        is_permission_error(metadata_error)
            ? PreparationErrorCode::PermissionDenied
            : PreparationErrorCode::MetadataFailure,
        metadata_error, component_index);
}

std::optional<PreparedDirectoryState> prepare_directory_state(
    const DirectoryRequest& request, const TestOverrides* overrides,
    MissingManagedComponentPolicy missing_component_policy,
    const DirectoryCreationPrecondition& creation_precondition) {
    validate_creation_boundary(request);
    const std::uintmax_t expected_effective_user =
        effective_user(overrides);
    std::vector<RetainedDirectoryState> retained_lineage;
    OpenedDirectory anchor = open_existing_anchor(
        request, expected_effective_user, overrides,
        retained_lineage);
    OwnedFileDescriptor current_directory = std::move(anchor.descriptor);

    fs::path current_path = request.creation_boundary.existing_anchor;
    std::size_t created_component_count = 0;
    OwnedFileDescriptor final_parent;
    struct stat final_status{};
    std::uintmax_t final_observed_owner = 0;

    for(std::size_t component_index = 0;
        component_index <
        request.creation_boundary.creatable_components.size();
        ++component_index) {
        const std::string& leaf_name =
            request.creation_boundary
                .creatable_components[component_index];
        const bool is_final_component =
            component_index + 1 ==
            request.creation_boundary.creatable_components.size();
        const bool requires_private_mode =
            is_final_component &&
            request.final_directory_requires_private_mode;
        current_path /= leaf_name;
        std::optional<struct stat> observed_status =
            inspect_managed_component(
                current_directory.get(), leaf_name, request,
                component_index, overrides);
        bool was_created = false;
        bool appeared_concurrently = false;

        if(!observed_status.has_value() &&
           missing_component_policy ==
               MissingManagedComponentPolicy::ReturnAbsent) {
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
            emit_test_event(
                overrides,
                DirectorySafetyTestEvent::BeforeAbsentLineageRevalidation,
                request.directory_kind, component_index,
                current_path);
#else
            emit_test_event(
                overrides, request.directory_kind,
                component_index, current_path);
#endif
            require_retained_lineage_unchanged(
                retained_lineage, request,
                expected_effective_user, overrides);
            observed_status = inspect_managed_component(
                current_directory.get(), leaf_name, request,
                component_index, overrides);
            if(!observed_status.has_value()) {
                // A finite second lineage proof distinguishes stable
                // absence from an observed authority replacement. A
                // non-cooperating same-euid process can still race the
                // final check; complete pathname linearizability is not
                // part of this boundary.
                require_retained_lineage_unchanged(
                    retained_lineage, request,
                    expected_effective_user, overrides);
                return std::nullopt;
            }
        }

        if(!observed_status.has_value()) {
            // The callback receives the descriptor-retained parent identity,
            // never a path-derived approximation. Revalidate both the named
            // lineage and stable absence around it so a rejecting callback
            // runs before this component can be created.
            require_retained_lineage_unchanged(
                retained_lineage, request,
                expected_effective_user, overrides);
            struct stat current_parent_status{};
            if(fstat(current_directory.get(), &current_parent_status) != 0) {
                const int metadata_error = errno;
                throw_preparation_error(
                    request.directory_kind,
                    PreparationStage::DirectoryRevalidation,
                    is_permission_error(metadata_error)
                        ? PreparationErrorCode::PermissionDenied
                        : PreparationErrorCode::MetadataFailure,
                    metadata_error, component_index);
            }
            if(retained_lineage.empty() ||
               !same_filesystem_identity(
                   retained_lineage.back().status,
                   current_parent_status)) {
                throw_preparation_error(
                    request.directory_kind,
                    PreparationStage::DirectoryRevalidation,
                    PreparationErrorCode::ConcurrentReplacement,
                    std::nullopt, component_index);
            }
            if(creation_precondition) {
                creation_precondition(DirectoryIdentity{
                    static_cast<std::uintmax_t>(
                        current_parent_status.st_dev),
                    static_cast<std::uintmax_t>(
                        current_parent_status.st_ino)});
            }
            require_retained_lineage_unchanged(
                retained_lineage, request,
                expected_effective_user, overrides);
            observed_status = inspect_managed_component(
                current_directory.get(), leaf_name, request,
                component_index, overrides);
            if(observed_status.has_value()) {
                appeared_concurrently = true;
            }
        }

        if(!observed_status.has_value()) {
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
            const bool injected_creation_failure =
                inject_failure(
                    overrides,
                    DirectorySafetyTestFailurePoint::ComponentCreation,
                    component_index);
#else
            const bool injected_creation_failure = false;
#endif
            // THREAT MODEL: mkdirat() and the first descriptor acquisition
            // cannot be one atomic Linux operation. A hostile same-euid
            // process can race this creation-to-open interval. Once the
            // descriptor is acquired below, nofollow validation, retained
            // identity, and root-relative named-lineage checks apply.
            if(injected_creation_failure ||
               mkdirat(
                   current_directory.get(), leaf_name.c_str(),
                   NEW_DIRECTORY_MODE) != 0) {
                const int creation_error = errno;
                if(creation_error != EEXIST) {
                    throw_preparation_error(
                        request.directory_kind,
                        PreparationStage::ComponentCreation,
                        is_permission_error(creation_error)
                            ? PreparationErrorCode::PermissionDenied
                            : (is_replacement_error(creation_error)
                                   ? PreparationErrorCode::ConcurrentReplacement
                                   : PreparationErrorCode::CreationFailed),
                        creation_error, component_index);
                }
                appeared_concurrently = true;
            } else {
                was_created = true;
                ++created_component_count;
#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
                emit_test_event(
                    overrides,
                    DirectorySafetyTestEvent::AfterComponentCreation,
                    request.directory_kind, component_index,
                    current_path);
#else
                emit_test_event(
                    overrides, request.directory_kind,
                    component_index, current_path);
#endif
            }

            observed_status = inspect_managed_component(
                current_directory.get(), leaf_name, request,
                component_index, overrides);
            if(!observed_status.has_value()) {
                throw_preparation_error(
                    request.directory_kind,
                    PreparationStage::ComponentValidation,
                    PreparationErrorCode::ConcurrentReplacement,
                    std::nullopt, component_index);
            }
        }

#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
        emit_test_event(
            overrides, DirectorySafetyTestEvent::AfterManagedMetadata,
            request.directory_kind, component_index, current_path);
#else
        emit_test_event(
            overrides, request.directory_kind, component_index,
            current_path);
#endif
        validate_directory_type(
            observed_status.value(), request.directory_kind,
            PreparationStage::ComponentValidation, component_index,
            was_created || appeared_concurrently);
        validate_directory_security(
            observed_status.value(), request.directory_kind,
            PreparationStage::ComponentValidation, component_index,
            expected_effective_user, overrides, was_created,
            requires_private_mode);

        OpenedDirectory opened = open_observed_directory(
            current_directory.get(), leaf_name,
            observed_status.value(), request.directory_kind,
            PreparationStage::ComponentOpen,
            PreparationStage::ComponentValidation, component_index,
            expected_effective_user, overrides, true, true,
            was_created, requires_private_mode);
        retained_lineage.push_back(retain_directory_identity(
            opened.descriptor.get(), leaf_name, opened.status,
            request.directory_kind,
            PreparationStage::ComponentValidation,
            component_index, true, requires_private_mode));

        if(is_final_component) {
            final_parent = std::move(current_directory);
            final_status = opened.status;
            final_observed_owner = observed_owner(opened.status, overrides);
        }
        current_directory = std::move(opened.descriptor);
    }

    return std::optional<PreparedDirectoryState>{PreparedDirectoryState{
        std::move(final_parent), std::move(current_directory),
        request.creation_boundary.creatable_components.back(),
        final_status, final_observed_owner,
        created_component_count, std::move(retained_lineage)}};
}

std::uintmax_t status_permissions(const struct stat& status) {
    return static_cast<std::uintmax_t>(status.st_mode & 07777);
}

std::uintmax_t status_device(const struct stat& status) {
    return static_cast<std::uintmax_t>(status.st_dev);
}

std::uintmax_t status_inode(const struct stat& status) {
    return static_cast<std::uintmax_t>(status.st_ino);
}

DirectoryRequest make_request(const xdg_paths::ConfigPaths& paths) {
    return DirectoryRequest{
        xdg_paths::DirectoryKind::Config, paths.directory, paths.creation_boundary, paths.config_file == paths.directory / "config.toml", {}};
}

DirectoryRequest make_request(const xdg_paths::StatePaths& paths) {
    fs::path expected_log_file =
        paths.directory /
        std::string(application_identity::XDG_IDENTITY);
    expected_log_file += ".log";
    return DirectoryRequest{
        xdg_paths::DirectoryKind::State, paths.directory, paths.creation_boundary, paths.default_log_file == expected_log_file, {}};
}

DirectoryRequest make_request(const xdg_paths::CachePaths& paths) {
    return DirectoryRequest{
        xdg_paths::DirectoryKind::Cache, paths.directory, paths.creation_boundary, true, {}};
}

DirectoryRequest make_request(
    const xdg_paths::SourcePreferencePaths& paths) {
    const fs::path expected_directory =
        paths.creation_boundary.base_directory /
        std::string(application_identity::XDG_IDENTITY) /
        std::string(SOURCE_PREFERENCE_DIRECTORY_NAME);
    return DirectoryRequest{
        xdg_paths::DirectoryKind::Config, paths.directory, paths.creation_boundary, paths.directory == expected_directory, {std::string(SOURCE_PREFERENCE_DIRECTORY_NAME)}, true};
}

DirectoryRequest make_request(
    const xdg_paths::StateStorePaths& paths) {
    fs::path expected_directory =
        paths.creation_boundary.base_directory /
        std::string(application_identity::XDG_IDENTITY);
    for(const std::string& component : paths.managed_components) {
        expected_directory /= component;
    }
    return DirectoryRequest{
        xdg_paths::DirectoryKind::State, paths.directory,
        paths.creation_boundary,
        !paths.managed_components.empty() &&
            paths.directory == expected_directory,
        paths.managed_components, true};
}

} // namespace

PreparationError::PreparationError(PreparationFailure failure)
    : std::runtime_error(preparation_diagnostic(failure)),
      failure_(failure) {
}

PreparedDirectory::PreparedDirectory(
    xdg_paths::DirectoryKind directory_kind, fs::path path,
    int parent_descriptor, int directory_descriptor,
    std::string leaf_name, std::uintmax_t device,
    std::uintmax_t inode, std::uintmax_t owner,
    std::uintmax_t filesystem_owner, std::uintmax_t permissions,
    std::size_t created_component_count,
    std::size_t managed_component_count,
    std::vector<RetainedDirectoryIdentity> retained_lineage) noexcept
    : directory_kind_(directory_kind), path_(std::move(path)),
      parent_descriptor_(parent_descriptor),
      directory_descriptor_(directory_descriptor),
      leaf_name_(std::move(leaf_name)), device_(device), inode_(inode),
      owner_(owner), filesystem_owner_(filesystem_owner),
      permissions_(permissions),
      created_component_count_(created_component_count),
      managed_component_count_(managed_component_count),
      retained_lineage_(std::move(retained_lineage)) {
}

PreparedDirectory::PreparedDirectory(PreparedDirectory&& other) noexcept
    : directory_kind_(other.directory_kind_), path_(std::move(other.path_)),
      parent_descriptor_(std::exchange(other.parent_descriptor_, -1)),
      directory_descriptor_(std::exchange(other.directory_descriptor_, -1)),
      leaf_name_(std::move(other.leaf_name_)), device_(other.device_),
      inode_(other.inode_), owner_(other.owner_),
      filesystem_owner_(other.filesystem_owner_),
      permissions_(other.permissions_),
      created_component_count_(other.created_component_count_),
      managed_component_count_(std::exchange(other.managed_component_count_, 0)),
      retained_lineage_(std::move(other.retained_lineage_)) {
    for(RetainedDirectoryIdentity& identity : other.retained_lineage_)
        identity.descriptor = -1;
}

PreparedDirectory::~PreparedDirectory() noexcept {
    for(const RetainedDirectoryIdentity& identity : retained_lineage_) {
        if(identity.descriptor >= 0)
            static_cast<void>(close(identity.descriptor));
    }
    if(directory_descriptor_ >= 0)
        static_cast<void>(close(directory_descriptor_));
    if(parent_descriptor_ >= 0)
        static_cast<void>(close(parent_descriptor_));
}

struct DirectorySafetyAccess {
    static PreparedDirectory adopt(
        const DirectoryRequest& request,
        PreparedDirectoryState state) {
        fs::path prepared_path = request.directory;
        std::vector<PreparedDirectory::RetainedDirectoryIdentity>
            retained_lineage;
        retained_lineage.reserve(state.retained_lineage.size());
        for(const RetainedDirectoryState& identity :
            state.retained_lineage) {
            retained_lineage.push_back(
                PreparedDirectory::RetainedDirectoryIdentity{
                    -1, identity.leaf_name,
                    status_device(identity.status),
                    status_inode(identity.status),
                    static_cast<std::uintmax_t>(
                        identity.status.st_uid),
                    identity.requires_security_validation,
                    identity.requires_private_mode});
        }
        for(std::size_t index = 0; index < retained_lineage.size(); ++index) {
            retained_lineage[index].descriptor =
                state.retained_lineage[index].descriptor.release();
        }
        PreparedDirectory prepared(
            request.directory_kind, std::move(prepared_path),
            state.parent_descriptor.get(),
            state.directory_descriptor.get(),
            std::move(state.leaf_name), status_device(state.status),
            status_inode(state.status), state.observed_owner,
            static_cast<std::uintmax_t>(state.status.st_uid),
            status_permissions(state.status),
            state.created_component_count,
            request.creation_boundary.creatable_components.size(),
            std::move(retained_lineage));
        static_cast<void>(state.parent_descriptor.release());
        static_cast<void>(state.directory_descriptor.release());
        prepared.require_unchanged_identity();
        return prepared;
    }

    static PreparedDirectory prepare(
        const DirectoryRequest& request,
        const TestOverrides* overrides,
        const DirectoryCreationPrecondition& creation_precondition = {}) {
        std::optional<PreparedDirectoryState> state =
            prepare_directory_state(
                request, overrides,
                MissingManagedComponentPolicy::Create,
                creation_precondition);
        if(!state.has_value()) {
            throw_preparation_error(
                request.directory_kind,
                PreparationStage::ComponentCreation,
                PreparationErrorCode::CreationFailed);
        }
        return adopt(request, std::move(state).value());
    }

    static std::optional<PreparedDirectory> open_existing(
        const DirectoryRequest& request,
        const TestOverrides* overrides) {
        std::optional<PreparedDirectoryState> state =
            prepare_directory_state(
                request, overrides,
                MissingManagedComponentPolicy::ReturnAbsent, {});
        if(!state.has_value()) return std::nullopt;
        return std::optional<PreparedDirectory>{
            adopt(request, std::move(state).value())};
    }
};

#if defined(MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS) || defined(MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS)
void set_managed_parent_sync_hook_for_test(ManagedParentSyncTestHook hook) {
    g_managed_parent_sync_hook = hook;
}
#endif

std::optional<std::error_code> PreparedDirectory::synchronize_managed_parent_entries() const {
    require_unchanged_identity();
    if(managed_component_count_ == 0 || managed_component_count_ >= retained_lineage_.size()) {
        throw_preparation_error(directory_kind_, PreparationStage::DirectoryRevalidation,
                                PreparationErrorCode::InvalidCreationBoundary);
    }
    const std::size_t anchor_index = retained_lineage_.size() - 1 - managed_component_count_;
    // Every managed edge needs its containing parent persisted, including
    // edges adopted from a failed earlier attempt. The final store directory
    // itself (parent of the package unit) is synced by the generation store.
    for(std::size_t index = retained_lineage_.size() - 1; index > anchor_index;) {
        --index;
        require_unchanged_identity();
        const auto& identity = retained_lineage_[index];
        int descriptor;
        do {
            descriptor = ::openat(identity.descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        } while(descriptor < 0 && errno == EINTR);
        if(descriptor < 0) return std::error_code(errno, std::generic_category());
        OwnedFileDescriptor sync_descriptor(descriptor);
        struct stat status{};
        if(::fstat(descriptor, &status) != 0) return std::error_code(errno, std::generic_category());
        if(!S_ISDIR(status.st_mode) || status_device(status) != identity.device || status_inode(status) != identity.inode) {
            throw_preparation_error(directory_kind_, PreparationStage::DirectoryRevalidation,
                                    PreparationErrorCode::ConcurrentReplacement);
        }
        require_unchanged_identity();
#if defined(MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS) || defined(MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS)
        if(g_managed_parent_sync_hook) {
            const auto failure = g_managed_parent_sync_hook(descriptor);
            require_unchanged_identity();
            if(failure) return failure;
        }
#endif
        int result;
        do {
            result = ::fsync(descriptor);
        } while(result < 0 && errno == EINTR);
        if(result != 0) return std::error_code(errno, std::generic_category());
        require_unchanged_identity();
        // close must not be retried: the descriptor may already be released.
        if(::close(sync_descriptor.release()) != 0) return std::error_code(errno, std::generic_category());
    }
    require_unchanged_identity();
    return std::nullopt;
}

void PreparedDirectory::require_unchanged_identity() const {
    if(parent_descriptor_ < 0 || directory_descriptor_ < 0 ||
       retained_lineage_.size() < 2) {
        throw_preparation_error(
            directory_kind_, PreparationStage::DirectoryRevalidation,
            PreparationErrorCode::MetadataFailure);
    }

    const auto validate_current_security =
        [this](const struct stat& status,
               std::uintmax_t expected_owner,
               bool requires_private_mode) {
            if(static_cast<std::uintmax_t>(status.st_uid) != expected_owner) {
                throw_preparation_error(
                    directory_kind_,
                    PreparationStage::DirectoryRevalidation,
                    PreparationErrorCode::OwnershipMismatch);
            }
            const mode_t permissions = status.st_mode & 07777;
            if((permissions & REQUIRED_OWNER_PERMISSIONS) !=
                   REQUIRED_OWNER_PERMISSIONS ||
               (permissions & FORBIDDEN_WRITE_PERMISSIONS) != 0 ||
               (requires_private_mode &&
                permissions != NEW_DIRECTORY_MODE)) {
                throw_preparation_error(
                    directory_kind_,
                    PreparationStage::DirectoryRevalidation,
                    PreparationErrorCode::UnsafePermissions);
            }
        };

    for(std::size_t index = 0; index < retained_lineage_.size(); ++index) {
        const RetainedDirectoryIdentity& identity = retained_lineage_[index];
        if(identity.descriptor < 0 ||
           (index == 0 ? !identity.leaf_name.empty()
                       : identity.leaf_name.empty())) {
            throw_preparation_error(
                directory_kind_,
                PreparationStage::DirectoryRevalidation,
                PreparationErrorCode::MetadataFailure);
        }

        struct stat retained_lineage_status{};
        if(fstat(identity.descriptor, &retained_lineage_status) != 0) {
            const int metadata_error = errno;
            throw_preparation_error(
                directory_kind_,
                PreparationStage::DirectoryRevalidation,
                is_permission_error(metadata_error)
                    ? PreparationErrorCode::PermissionDenied
                    : PreparationErrorCode::MetadataFailure,
                metadata_error, index);
        }
        if(!S_ISDIR(retained_lineage_status.st_mode) ||
           status_device(retained_lineage_status) != identity.device ||
           status_inode(retained_lineage_status) != identity.inode) {
            throw_preparation_error(
                directory_kind_,
                PreparationStage::DirectoryRevalidation,
                PreparationErrorCode::ConcurrentReplacement,
                std::nullopt, index);
        }

        if(index == 0) continue;

        const RetainedDirectoryIdentity& parent =
            retained_lineage_[index - 1];
        struct stat named_lineage_status{};
        if(fstatat(
               parent.descriptor, identity.leaf_name.c_str(),
               &named_lineage_status, AT_SYMLINK_NOFOLLOW) != 0) {
            const int metadata_error = errno;
            throw_preparation_error(
                directory_kind_,
                PreparationStage::DirectoryRevalidation,
                is_replacement_error(metadata_error)
                    ? PreparationErrorCode::ConcurrentReplacement
                    : (is_permission_error(metadata_error)
                           ? PreparationErrorCode::PermissionDenied
                           : PreparationErrorCode::MetadataFailure),
                metadata_error, index);
        }
        if(!S_ISDIR(named_lineage_status.st_mode) ||
           !same_filesystem_identity(
               retained_lineage_status, named_lineage_status)) {
            throw_preparation_error(
                directory_kind_,
                PreparationStage::DirectoryRevalidation,
                PreparationErrorCode::ConcurrentReplacement,
                std::nullopt, index);
        }
        if(identity.requires_security_validation) {
            validate_current_security(
                retained_lineage_status,
                identity.filesystem_owner,
                identity.requires_private_mode);
            validate_current_security(
                named_lineage_status,
                identity.filesystem_owner,
                identity.requires_private_mode);
        }
    }

    struct stat retained_status{};
    if(fstat(directory_descriptor_, &retained_status) != 0) {
        const int metadata_error = errno;
        throw_preparation_error(
            directory_kind_, PreparationStage::DirectoryRevalidation,
            is_permission_error(metadata_error)
                ? PreparationErrorCode::PermissionDenied
                : PreparationErrorCode::MetadataFailure,
            metadata_error);
    }

    struct stat retained_parent_status{};
    if(fstat(parent_descriptor_, &retained_parent_status) != 0) {
        const int metadata_error = errno;
        throw_preparation_error(
            directory_kind_, PreparationStage::DirectoryRevalidation,
            is_permission_error(metadata_error)
                ? PreparationErrorCode::PermissionDenied
                : PreparationErrorCode::MetadataFailure,
            metadata_error);
    }

    struct stat named_status{};
    if(fstatat(
           parent_descriptor_, leaf_name_.c_str(), &named_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_preparation_error(
            directory_kind_, PreparationStage::DirectoryRevalidation,
            is_replacement_error(metadata_error)
                ? PreparationErrorCode::ConcurrentReplacement
                : (is_permission_error(metadata_error)
                       ? PreparationErrorCode::PermissionDenied
                       : PreparationErrorCode::MetadataFailure),
            metadata_error);
    }

    const RetainedDirectoryIdentity& lineage_parent =
        retained_lineage_[retained_lineage_.size() - 2];
    const RetainedDirectoryIdentity& lineage_directory =
        retained_lineage_.back();
    if(lineage_directory.leaf_name != leaf_name_ ||
       !S_ISDIR(retained_status.st_mode) ||
       !S_ISDIR(retained_parent_status.st_mode) ||
       !same_filesystem_identity(retained_status, named_status) ||
       status_device(retained_status) != device_ ||
       status_inode(retained_status) != inode_ ||
       status_device(retained_status) != lineage_directory.device ||
       status_inode(retained_status) != lineage_directory.inode ||
       status_device(retained_parent_status) != lineage_parent.device ||
       status_inode(retained_parent_status) != lineage_parent.inode) {
        throw_preparation_error(
            directory_kind_, PreparationStage::DirectoryRevalidation,
            PreparationErrorCode::ConcurrentReplacement);
    }

    validate_current_security(
        retained_status, filesystem_owner_,
        lineage_directory.requires_private_mode);
    validate_current_security(
        named_status, filesystem_owner_,
        lineage_directory.requires_private_mode);
}

PreparedDirectory prepare_directory(const xdg_paths::ConfigPaths& paths) {
    const DirectoryRequest request = make_request(paths);
    return DirectorySafetyAccess::prepare(request, nullptr);
}

PreparedDirectory prepare_directory(const xdg_paths::StatePaths& paths) {
    const DirectoryRequest request = make_request(paths);
    return DirectorySafetyAccess::prepare(request, nullptr);
}

PreparedDirectory prepare_directory(
    const xdg_paths::StatePaths& paths,
    const DirectoryCreationPrecondition& creation_precondition) {
    const DirectoryRequest request = make_request(paths);
    return DirectorySafetyAccess::prepare(
        request, nullptr, creation_precondition);
}

std::optional<PreparedDirectory> open_existing_directory(const xdg_paths::CachePaths& paths) {
    return DirectorySafetyAccess::open_existing(make_request(paths), nullptr);
}

PreparedDirectory prepare_directory(const xdg_paths::CachePaths& paths) {
    const DirectoryRequest request = make_request(paths);
    return DirectorySafetyAccess::prepare(request, nullptr);
}

PreparedDirectory prepare_directory(
    const xdg_paths::CachePaths& paths,
    const DirectoryCreationPrecondition& creation_precondition) {
    const DirectoryRequest request = make_request(paths);
    return DirectorySafetyAccess::prepare(
        request, nullptr, creation_precondition);
}

PreparedDirectory prepare_directory(
    const xdg_paths::SourcePreferencePaths& paths) {
    const DirectoryRequest request = make_request(paths);
    return DirectorySafetyAccess::prepare(request, nullptr);
}

std::optional<PreparedDirectory> open_existing_directory(
    const xdg_paths::SourcePreferencePaths& paths) {
    const DirectoryRequest request = make_request(paths);
    return DirectorySafetyAccess::open_existing(request, nullptr);
}

PreparedDirectory prepare_directory(
    const xdg_paths::StateStorePaths& paths) {
    const DirectoryRequest request = make_request(paths);
    return DirectorySafetyAccess::prepare(request, nullptr);
}

std::optional<PreparedDirectory> open_existing_directory(
    const xdg_paths::StateStorePaths& paths) {
    const DirectoryRequest request = make_request(paths);
    return DirectorySafetyAccess::open_existing(request, nullptr);
}

#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
PreparedDirectory prepare_directory_for_test(
    const xdg_paths::ConfigPaths& paths,
    const DirectorySafetyTestOverrides& overrides) {
    return DirectorySafetyAccess::prepare(make_request(paths), &overrides);
}

PreparedDirectory prepare_directory_for_test(
    const xdg_paths::StatePaths& paths,
    const DirectorySafetyTestOverrides& overrides) {
    return DirectorySafetyAccess::prepare(make_request(paths), &overrides);
}

PreparedDirectory prepare_directory_for_test(
    const xdg_paths::CachePaths& paths,
    const DirectorySafetyTestOverrides& overrides) {
    return DirectorySafetyAccess::prepare(make_request(paths), &overrides);
}

PreparedDirectory prepare_directory_for_test(
    const xdg_paths::SourcePreferencePaths& paths,
    const DirectorySafetyTestOverrides& overrides) {
    return DirectorySafetyAccess::prepare(make_request(paths), &overrides);
}

std::optional<PreparedDirectory> open_existing_directory_for_test(
    const xdg_paths::SourcePreferencePaths& paths,
    const DirectorySafetyTestOverrides& overrides) {
    return DirectorySafetyAccess::open_existing(
        make_request(paths), &overrides);
}

PreparedDirectory prepare_directory_for_test(
    const xdg_paths::StateStorePaths& paths,
    const DirectorySafetyTestOverrides& overrides) {
    return DirectorySafetyAccess::prepare(make_request(paths), &overrides);
}

std::optional<PreparedDirectory> open_existing_directory_for_test(
    const xdg_paths::StateStorePaths& paths,
    const DirectorySafetyTestOverrides& overrides) {
    return DirectorySafetyAccess::open_existing(
        make_request(paths), &overrides);
}
#endif

} // namespace xdg_directory_safety
