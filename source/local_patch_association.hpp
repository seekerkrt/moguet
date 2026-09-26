#pragma once

#include "local_recipe_candidate.hpp"

#include <functional>
#include <memory>
#include <system_error>
#include <variant>

enum class PatchAssociationFailureKind {
    Missing,
    Changed,
    Unsafe,
    Corrupt,
    Unsupported,
    InvalidMaterial,
    InvalidIdentity,
    AssociationMismatch,
    ConcurrentChange,
    AlreadyExists,
    IoFailure,
    ToolFailure,
    PublicationUncertain
};

struct PatchAssociationFailure {
    PatchAssociationFailureKind kind;
    std::filesystem::path path;
    std::error_code system_error;
    std::optional<std::filesystem::path> leftover;
    std::optional<LocalSourceWorkspaceFailure> cleanup_failure;
};

struct PatchAssociationAbsent {};
struct PatchAssociationForgotten {};

// Registration identity comes from an evaluated owned snapshot, never stale
// .SRCINFO or a guessed child name. Caller authorizes evaluation separately.
class ObservedLocalPatchSource final {
    LocalSourceRoot original_;
    PackageBaseIdentity identity_;
    ObservedLocalPatchSource(LocalSourceRoot original, PackageBaseIdentity identity);
    friend struct PatchAssociationAccess;

public:
    ObservedLocalPatchSource(ObservedLocalPatchSource&&) noexcept = default;
    ObservedLocalPatchSource(const ObservedLocalPatchSource&) = delete;
    ObservedLocalPatchSource& operator=(ObservedLocalPatchSource&&) = delete;
    const PackageBaseIdentity& identity() const noexcept;
    void require_unchanged_identity() const;
};

struct PatchMaterialEntry {
    std::string file;
    std::string sha256;
    bool operator==(const PatchMaterialEntry&) const = default;
};

class LoadedPatchAssociation final {
    struct Observation;
    std::shared_ptr<const Observation> observation_;
    PackageBaseIdentity identity_;
    std::filesystem::path material_root_;
    std::vector<PatchMaterialEntry> entries_;
    LoadedPatchAssociation(std::shared_ptr<const Observation>, PackageBaseIdentity,
                           std::filesystem::path, std::vector<PatchMaterialEntry>);
    friend struct PatchAssociationAccess;

public:
    const PackageBaseIdentity& identity() const noexcept;
    const std::filesystem::path& material_root() const noexcept;
    const std::vector<PatchMaterialEntry>& entries() const noexcept;
    // The version accepted by the strict record decoder, not material health.
    int schema_version() const noexcept;
};

class AcquiredLocalRecipeSeries final {
    PackageBaseIdentity identity_;
    std::vector<LocalRecipePatch> patches_;
    AcquiredLocalRecipeSeries(PackageBaseIdentity, std::vector<LocalRecipePatch>);
    friend struct PatchAssociationAccess;

public:
    AcquiredLocalRecipeSeries(AcquiredLocalRecipeSeries&&) noexcept = default;
    AcquiredLocalRecipeSeries(const AcquiredLocalRecipeSeries&) = delete;
    const PackageBaseIdentity& identity() const noexcept;
    const std::vector<LocalRecipePatch>& patches() const noexcept;
    // Pass identity() as expected_source to the candidate consumer, whose
    // fresh prepatch guard is still required. No paths are reopened here.
    std::vector<LocalRecipePatch> take_patches() &&;
};

using PatchAssociationReadResult = std::variant<PatchAssociationAbsent, LoadedPatchAssociation, PatchAssociationFailure>;
using PatchAssociationWriteResult = std::variant<LoadedPatchAssociation, PatchAssociationFailure>;
using PatchAssociationAcquireResult = std::variant<AcquiredLocalRecipeSeries, PatchAssociationFailure>;

std::variant<ObservedLocalPatchSource, PatchAssociationFailure> observe_local_patch_source(
    LocalSourceRoot original, const ValidatedCacheRoot& cache_root,
    SourceBuildEnvironment environment);

// Reader is no-create. Value identity selects a record, not execution consent.
PatchAssociationReadResult read_local_patch_association(const PackageBaseIdentity& identity);
// Complete registry snapshot or failure, ordered by PackageBase then source
// location. Reuses the strict record reader; never opens source/material paths
// or creates the store. A missing store is an empty registry, not a bad record.
std::variant<std::vector<LoadedPatchAssociation>, PatchAssociationFailure> list_patch_associations();
PatchAssociationWriteResult register_local_patch_association(
    const ObservedLocalPatchSource& source, const std::filesystem::path& material_root,
    const std::vector<std::string>& ordered_files);
PatchAssociationWriteResult update_local_patch_association(
    const ObservedLocalPatchSource& source, const LoadedPatchAssociation& previous,
    const std::filesystem::path& material_root, const std::vector<std::string>& ordered_files);
std::variant<PatchAssociationForgotten, PatchAssociationFailure> forget_local_patch_association(
    const LoadedPatchAssociation& previous);

// Complete, digest-verified series or failure: no partial input can escape.
// A fresh record observation is required; a stale update token is rejected.
PatchAssociationAcquireResult acquire_local_patch_series(const LoadedPatchAssociation& association);

// Display/diagnostic path only, never a filesystem capability.
std::filesystem::path local_patch_association_record_path(const PackageBaseIdentity& identity);

#ifdef MOGUET_ENABLE_PATCH_ASSOCIATION_TEST_HOOKS
enum class PatchAssociationTestPoint {
    AfterMaterialOpen,
    AfterMaterialRead,
    AfterSeriesRead,
    BeforePublication,
    BeforeWrite,
    BeforeFileSync,
    AfterPublication,
    BeforeDirectorySync,
    PartialRead,
    WrongMaterialOwner
};
using PatchAssociationTestHook = std::function<void(PatchAssociationTestPoint, const std::filesystem::path&)>;
void set_patch_association_test_hook(PatchAssociationTestHook hook);
void fail_patch_association_operation_for_test(PatchAssociationTestPoint point);
#endif
