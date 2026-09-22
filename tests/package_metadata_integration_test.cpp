#include "package_metadata.hpp"

#ifdef ALPM_H
#error "package_metadata.hpp must not expose or include raw libalpm types"
#endif

#include "process.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string path_template =
            (fs::temp_directory_path() /
             "moguet-cleanup-policy-metadata-XXXXXX")
                .string();
        std::vector<char> writable_path(
            path_template.begin(), path_template.end());
        writable_path.push_back('\0');
        char* created = ::mkdtemp(writable_path.data());
        if(created == nullptr) {
            throw std::runtime_error(
                "failed to create cleanup policy metadata fixture directory");
        }
        path_ = created;
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

struct FixturePackageMetadata {
    explicit FixturePackageMetadata(
        std::string package_name,
        std::string package_version = "1-1",
        std::vector<std::string> package_provides = {},
        std::vector<std::string> package_groups = {},
        std::vector<std::string> package_dependencies = {},
        std::string actual_package_base = {},
        std::string actual_architecture = "any")
        : name(std::move(package_name)),
          version(std::move(package_version)),
          package_base(
              actual_package_base.empty()
                  ? name
                  : std::move(actual_package_base)),
          architecture(std::move(actual_architecture)),
          provides(std::move(package_provides)),
          groups(std::move(package_groups)),
          dependencies(std::move(package_dependencies)) {
    }

    std::string name;
    std::string version;
    std::string package_base;
    std::string architecture;
    std::vector<std::string> provides;
    std::vector<std::string> groups;
    std::vector<std::string> dependencies;
};

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void write_fixture_file(
    const fs::path& path,
    const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error(
            "failed to create cleanup policy metadata fixture file");
    }
    output << contents;
    output.close();
    if(!output) {
        throw std::runtime_error(
            "failed to write cleanup policy metadata fixture file");
    }
}

void append_fixture_field(
    std::ostringstream& descriptor,
    const std::string& field,
    const std::vector<std::string>& values) {
    if(values.empty()) return;
    descriptor << '%' << field << "%\n";
    for(const std::string& value : values) {
        descriptor << value << '\n';
    }
    descriptor << '\n';
}

std::string local_package_descriptor(
    const FixturePackageMetadata& package) {
    std::ostringstream descriptor;
    descriptor << "%NAME%\n"
               << package.name << "\n\n"
               << "%VERSION%\n"
               << package.version << "\n\n"
               << "%BASE%\n"
               << package.package_base << "\n\n"
               << "%DESC%\ncleanup policy fixture\n\n"
               << "%ARCH%\n"
               << package.architecture << "\n\n"
               << "%REASON%\n1\n\n";
    append_fixture_field(descriptor, "PROVIDES", package.provides);
    append_fixture_field(descriptor, "GROUPS", package.groups);
    append_fixture_field(descriptor, "DEPENDS", package.dependencies);
    return descriptor.str();
}

std::string sync_package_descriptor(
    const FixturePackageMetadata& package) {
    std::ostringstream descriptor;
    descriptor << "%FILENAME%\n"
               << package.name << '-' << package.version
               << "-any.pkg.tar.zst\n\n"
               << "%NAME%\n"
               << package.name << "\n\n"
               << "%BASE%\n"
               << package.package_base << "\n\n"
               << "%VERSION%\n"
               << package.version << "\n\n"
               << "%DESC%\ncleanup policy fixture\n\n"
               << "%CSIZE%\n1\n\n"
               << "%ISIZE%\n1\n\n"
               << "%SHA256SUM%\n"
               << std::string(64, '0') << "\n\n"
               << "%URL%\nhttps://example.invalid/\n\n"
               << "%LICENSE%\nGPL\n\n"
               << "%ARCH%\n"
               << package.architecture << "\n\n"
               << "%BUILDDATE%\n1\n\n"
               << "%PACKAGER%\nMoguet test fixture\n\n";
    append_fixture_field(descriptor, "PROVIDES", package.provides);
    append_fixture_field(descriptor, "GROUPS", package.groups);
    append_fixture_field(descriptor, "DEPENDS", package.dependencies);
    return descriptor.str();
}

void create_local_database(
    const fs::path& database_path,
    const std::vector<FixturePackageMetadata>& packages) {
    const fs::path local_database = database_path / "local";
    write_fixture_file(local_database / "ALPM_DB_VERSION", "9\n");
    for(const FixturePackageMetadata& package : packages) {
        const fs::path package_directory =
            local_database / (package.name + '-' + package.version);
        write_fixture_file(
            package_directory / "desc",
            local_package_descriptor(package));
    }
}

void create_sync_database(
    const fs::path& fixture_root,
    const fs::path& database_path,
    const std::string& repository_name,
    const std::vector<FixturePackageMetadata>& packages) {
    const fs::path staging =
        fixture_root / ("sync-stage-" + repository_name);
    fs::create_directories(staging);

    std::vector<std::string> archive_entries;
    archive_entries.reserve(packages.size());
    for(const FixturePackageMetadata& package : packages) {
        const std::string entry = package.name + '-' + package.version;
        archive_entries.push_back(entry);
        write_fixture_file(
            staging / entry / "desc",
            sync_package_descriptor(package));
    }

    if(archive_entries.empty()) {
        throw std::runtime_error(
            "cleanup policy sync fixture requires one anchor package");
    }

    const fs::path sync_directory = database_path / "sync";
    fs::create_directories(sync_directory);
    ExplicitProcessInvocation invocation;
    invocation.executable = "/usr/bin/bsdtar";
    invocation.arguments = {
        "-cf",
        (sync_directory / (repository_name + ".db")).string(),
        "-C",
        staging.string()};
    invocation.arguments.insert(
        invocation.arguments.end(),
        archive_entries.begin(), archive_entries.end());
    invocation.environment = {"PATH=/usr/bin", "LC_ALL=C"};
    if(run_explicit_process(invocation, true, false) != 0) {
        throw std::runtime_error(
            "failed to create cleanup policy sync database fixture");
    }
}

CleanupPolicyProtectionEvidence observe_fixture_policy(
    const std::vector<FixturePackageMetadata>& local_packages,
    const std::vector<std::pair<
        std::string,
        std::vector<FixturePackageMetadata>>>& sync_repositories,
    const std::string& candidate_package_name,
    const std::vector<std::string>& configured_repositories = {}) {
    TemporaryDirectory fixture;
    const fs::path database_path = fixture.path() / "database";
    create_local_database(database_path, local_packages);
    for(const auto& repository : sync_repositories) {
        create_sync_database(
            fixture.path(), database_path,
            repository.first, repository.second);
    }

    std::vector<std::string> repository_order = configured_repositories;
    if(repository_order.empty()) {
        repository_order.reserve(sync_repositories.size());
        for(const auto& repository : sync_repositories) {
            repository_order.push_back(repository.first);
        }
    }
    return query_cleanup_policy_protection_evidence(
        PacmanRepositoryConfiguration{
            PacmanDatabasePaths{"/", database_path},
            std::move(repository_order)},
        candidate_package_name);
}

void test_raw_capture_preserves_boundary_whitespace() {
    const std::string expected =
        "\nRootDir = /\nDBPath = /var/lib/pacman/\n\n";
    CapturedCommandResult result = capture_command_output_raw(
        "printf '\\nRootDir = /\\nDBPath = /var/lib/pacman/\\n\\n'");

    expect(result.exit_code == 0, "raw capture command failed");
    expect(result.output == expected, "raw capture changed boundary whitespace");
}

void run_pacman_metadata_smoke_test() {
    PacmanDatabasePaths paths = resolve_pacman_database_paths();
    PackageMetadataSession session = PackageMetadataSession::open(paths);

    InstalledPackageQueryResult result = session.query_installed_package("pacman");
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&result)) {
        throw std::runtime_error("pacman metadata query failed: " + failure->diagnostic);
    }
    if(std::holds_alternative<PackageNotFound>(result)) {
        throw std::runtime_error("installed pacman package was not found");
    }

    const InstalledPackageMetadata& metadata = std::get<InstalledPackageMetadata>(result);
    expect(metadata.name == "pacman", "pacman metadata returned a different package name");
    expect(!metadata.version.empty(), "pacman metadata returned an empty version");
    expect(
        metadata.package_base.state() ==
                InstalledPackageMetadataValueState::Known &&
            metadata.package_base.value() != nullptr &&
            !metadata.package_base.value()->empty(),
        "pacman metadata did not retain actual PackageBase");
    expect(
        metadata.architecture.state() ==
                InstalledPackageMetadataValueState::Known &&
            metadata.architecture.value() != nullptr &&
            !metadata.architecture.value()->empty(),
        "pacman metadata did not retain actual architecture");
    expect(
        metadata.reason == InstalledPackageReason::Explicit ||
            metadata.reason == InstalledPackageReason::Dependency ||
            metadata.reason == InstalledPackageReason::Unknown,
        "pacman metadata returned an unknown public install reason");

    InstalledPackageStateSnapshotResult snapshot_result =
        session.snapshot_installed_package_states();
    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&snapshot_result)) {
        throw std::runtime_error(
            "installed package state snapshot failed: " +
            failure->diagnostic);
    }
    const InstalledPackageStateSnapshot& snapshot =
        std::get<InstalledPackageStateSnapshot>(snapshot_result);
    const auto pacman = snapshot.find("pacman");
    expect(
        pacman != snapshot.end(),
        "installed package state snapshot omitted pacman");
    expect(
        pacman->second.name == metadata.name &&
            pacman->second.version == metadata.version &&
            pacman->second.reason == metadata.reason &&
            pacman->second.package_base == metadata.package_base &&
            pacman->second.architecture == metadata.architecture,
        "installed package state snapshot differs from exact metadata");
}

void run_installed_identity_fixture_test() {
    TemporaryDirectory fixture;
    const fs::path database_path = fixture.path() / "database";
    create_local_database(
        database_path,
        {FixturePackageMetadata{
            "split-child", "2-3", {}, {}, {}, "split-base", "aarch64"}});

    PackageMetadataSession session = PackageMetadataSession::open(
        PacmanDatabasePaths{"/", database_path});
    const InstalledPackageMetadata metadata =
        std::get<InstalledPackageMetadata>(
            session.query_installed_package("split-child"));
    expect(
        metadata.package_base.state() ==
                InstalledPackageMetadataValueState::Known &&
            metadata.package_base.value() != nullptr &&
            *metadata.package_base.value() == "split-base",
        "synthetic local DB PackageBase was replaced by child identity");
    expect(
        metadata.architecture.state() ==
                InstalledPackageMetadataValueState::Known &&
            metadata.architecture.value() != nullptr &&
            *metadata.architecture.value() == "aarch64",
        "synthetic local DB architecture was not retained exactly");

    const InstalledPackageStateSnapshot snapshot =
        std::get<InstalledPackageStateSnapshot>(
            session.snapshot_installed_package_states());
    expect(
        snapshot.at("split-child").package_base == metadata.package_base &&
            snapshot.at("split-child").architecture == metadata.architecture,
        "synthetic full snapshot changed installed identity metadata");
}

void run_repository_metadata_smoke_test() {
    PacmanRepositoryConfiguration configuration =
        resolve_pacman_repository_configuration();
    expect(
        !configuration.repository_names.empty(),
        "pacman configuration did not return any repositories");

    RepositoryPackageMetadataSession session =
        RepositoryPackageMetadataSession::open(configuration);
    RepositoryPackageQueryResult pacman_result = session.query_repository_package(
        RepositoryPackageLookup{"pacman", std::nullopt});
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&pacman_result)) {
        throw std::runtime_error(
            "repository pacman metadata query failed: " + failure->diagnostic);
    }
    if(std::holds_alternative<PackageNotFound>(pacman_result)) {
        throw std::runtime_error("repository pacman package was not found");
    }

    const RepositoryPackageMetadata& metadata =
        std::get<RepositoryPackageMetadata>(pacman_result);
    expect(metadata.package_name == "pacman", "repository query returned a different package");
    expect(
        std::find(
            configuration.repository_names.begin(),
            configuration.repository_names.end(),
            metadata.repository_name) != configuration.repository_names.end(),
        "repository query returned an unconfigured repository");

    RepositoryPackageQueryResult missing_result = session.query_repository_package(
        RepositoryPackageLookup{
            "moguet-issue-125-package-that-does-not-exist",
            std::nullopt});
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&missing_result)) {
        throw std::runtime_error(
            "missing repository package query failed: " + failure->diagnostic);
    }
    expect(
        std::holds_alternative<PackageNotFound>(missing_result),
        "missing repository package was not reported as not found");
}

void run_current_arch_base_devel_policy_smoke_test() {
    const CleanupPolicyProtectionEvidence evidence =
        query_cleanup_policy_protection_evidence(
            resolve_pacman_repository_configuration(), "make");
    const CleanupPolicyAuthorityEvidence* authority = nullptr;
    if(evidence.installed_base_devel.observation ==
       CleanupPolicyAuthorityObservation::Present) {
        authority = &evidence.installed_base_devel;
    } else if(evidence.configured_sync_base_devel.observation ==
              CleanupPolicyAuthorityObservation::Present) {
        authority = &evidence.configured_sync_base_devel;
    }

    expect(
        authority != nullptr &&
            authority->candidate_evaluation ==
                CleanupPolicyCandidateEvaluation::Protected &&
            authority->inventory_completeness ==
                CleanupPolicyMetadataCompleteness::Complete &&
            !authority->meta_packages.empty() &&
            std::find(
                authority->meta_packages.front().dependencies.begin(),
                authority->meta_packages.front().dependencies.end(),
                "make") !=
                authority->meta_packages.front().dependencies.end() &&
            evidence.base_devel_group.observation ==
                CleanupPolicyAuthorityObservation::NotObserved,
        "current Arch base-devel meta dependency authority is unavailable");
}

struct PacmanForeignPackage {
    std::string name;
    std::string version;
};

std::vector<PacmanForeignPackage> query_pacman_foreign_packages() {
    CapturedCommandResult result =
        capture_command_output_raw("pacman -Qm 2>/dev/null");
    // pacman reports a valid query with no foreign package matches as exit 1
    // and empty output. The corresponding libalpm inventory is an empty vector.
    const bool is_empty_inventory = result.exit_code == 1 && result.output.empty();
    if(result.exit_code != 0 && !is_empty_inventory) {
        throw std::runtime_error(
            "pacman -Qm failed with exit code " +
            std::to_string(result.exit_code));
    }

    std::vector<PacmanForeignPackage> packages;
    std::stringstream output_stream(result.output);
    std::string line;
    while(std::getline(output_stream, line)) {
        if(line.empty()) {
            throw std::runtime_error("pacman -Qm returned an empty output line");
        }

        std::stringstream line_stream(line);
        PacmanForeignPackage package;
        std::string unexpected_field;
        if(!(line_stream >> package.name >> package.version) ||
           (line_stream >> unexpected_field)) {
            throw std::runtime_error("pacman -Qm returned a malformed output line");
        }
        packages.push_back(std::move(package));
    }
    return packages;
}

void run_foreign_package_inventory_compatibility_test() {
    std::vector<PacmanForeignPackage> pacman_packages =
        query_pacman_foreign_packages();
    PacmanRepositoryConfiguration configuration =
        resolve_pacman_repository_configuration();
    ForeignPackageInventoryResult result =
        query_foreign_package_inventory(configuration);
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&result)) {
        throw std::runtime_error(
            "foreign package inventory failed: " + failure->diagnostic);
    }

    const ForeignPackageInventory& inventory =
        std::get<ForeignPackageInventory>(result);
    expect(
        inventory.size() == pacman_packages.size(),
        "foreign package inventory count differs from pacman -Qm");
    for(std::size_t index = 0; index < inventory.size(); ++index) {
        expect(
            inventory[index].name == pacman_packages[index].name,
            "foreign package inventory name/order differs from pacman -Qm");
        expect(
            inventory[index].version == pacman_packages[index].version,
            "foreign package inventory version differs from pacman -Qm");
    }
}

void run_cleanup_policy_installed_meta_fixture_test() {
    const CleanupPolicyProtectionEvidence evidence =
        observe_fixture_policy(
            {FixturePackageMetadata{"toolchain"},
             FixturePackageMetadata{
                 "base-devel", "1-2", {}, {}, {"toolchain"}}},
            {}, "toolchain", {"missing-sync-database"});

    expect(
        evidence.local_database_completeness ==
                CleanupPolicyMetadataCompleteness::Complete &&
            evidence.candidate_metadata_completeness ==
                CleanupPolicyMetadataCompleteness::Complete &&
            evidence.installed_base_devel.observation ==
                CleanupPolicyAuthorityObservation::Present &&
            evidence.installed_base_devel.candidate_evaluation ==
                CleanupPolicyCandidateEvaluation::Protected &&
            evidence.configured_sync_base_devel.observation ==
                CleanupPolicyAuthorityObservation::NotObserved,
        "installed exact base-devel meta authority did not stay primary");
}

void run_cleanup_policy_sync_meta_fixture_test() {
    const CleanupPolicyProtectionEvidence direct =
        observe_fixture_policy(
            {FixturePackageMetadata{"toolchain"}},
            {{"core",
              {FixturePackageMetadata{
                  "base-devel", "1-2", {}, {}, {"toolchain"}}}}},
            "toolchain");
    expect(
        direct.installed_base_devel.observation ==
                CleanupPolicyAuthorityObservation::Absent &&
            direct.configured_sync_base_devel.observation ==
                CleanupPolicyAuthorityObservation::Present &&
            direct.configured_sync_base_devel.candidate_evaluation ==
                CleanupPolicyCandidateEvaluation::Protected &&
            direct.configured_sync_base_devel.meta_packages.size() == 1 &&
            direct.configured_sync_base_devel.meta_packages[0]
                    .dependencies ==
                std::vector<std::string>{"toolchain"},
        "sync exact base-devel direct dependency was not protected");

    const CleanupPolicyProtectionEvidence not_protected =
        observe_fixture_policy(
            {FixturePackageMetadata{"unrelated"}},
            {{"core",
              {FixturePackageMetadata{
                  "base-devel", "1-2", {}, {}, {"toolchain"}}}}},
            "unrelated");
    expect(
        not_protected.configured_sync_base_devel
                    .candidate_evaluation ==
                CleanupPolicyCandidateEvaluation::NotProtected &&
            not_protected.configured_sync_base_devel
                    .inventory_completeness ==
                CleanupPolicyMetadataCompleteness::Complete &&
            not_protected.configured_sync_base_devel
                    .evaluation_completeness ==
                CleanupPolicyMetadataCompleteness::Complete &&
            not_protected.failures.empty(),
        "complete sync meta non-satisfier did not retain negative authority");
}

void run_cleanup_policy_provides_fixture_test() {
    const CleanupPolicyProtectionEvidence evidence =
        observe_fixture_policy(
            {FixturePackageMetadata{
                "local-provider", "2-1", {"virtual-tool=2"}, {}, {}}},
            {{"core",
              {FixturePackageMetadata{
                  "base-devel", "1-2", {}, {}, {"virtual-tool>=1"}}}}},
            "local-provider");

    expect(
        evidence.candidate.has_value() &&
            evidence.candidate->package_name == "local-provider" &&
            evidence.candidate->provides ==
                std::vector<std::string>{"virtual-tool=2"} &&
            evidence.configured_sync_base_devel.candidate_evaluation ==
                CleanupPolicyCandidateEvaluation::Protected,
        "libalpm satisfier semantics did not protect a local-only provider");
}

void run_cleanup_policy_group_fixture_test() {
    const std::vector<FixturePackageMetadata> group_repository = {
        FixturePackageMetadata{
            "compatibility-member", "1-1", {}, {"base-devel"}, {}}};
    const CleanupPolicyProtectionEvidence protected_group =
        observe_fixture_policy(
            {FixturePackageMetadata{
                "local-group-member", "1-1", {}, {"other", "base-devel", "third"}, {}}},
            {{"core", group_repository}}, "local-group-member");
    expect(
        protected_group.configured_sync_base_devel.observation ==
                CleanupPolicyAuthorityObservation::Absent &&
            protected_group.base_devel_group.observation ==
                CleanupPolicyAuthorityObservation::Present &&
            protected_group.base_devel_group.candidate_evaluation ==
                CleanupPolicyCandidateEvaluation::Protected &&
            protected_group.candidate->groups ==
                std::vector<std::string>{
                    "other", "base-devel", "third"} &&
            protected_group.base_devel_group.group.has_value() &&
            protected_group.base_devel_group.group->members.size() == 1,
        "exact base-devel compatibility group or multiple groups were lost");

    const CleanupPolicyProtectionEvidence not_protected_group =
        observe_fixture_policy(
            {FixturePackageMetadata{
                "not-a-group-member", "1-1", {}, {"other"}, {}}},
            {{"core", group_repository}}, "not-a-group-member");
    expect(
        not_protected_group.base_devel_group.candidate_evaluation ==
                CleanupPolicyCandidateEvaluation::NotProtected &&
            not_protected_group.base_devel_group.inventory_completeness ==
                CleanupPolicyMetadataCompleteness::Complete,
        "complete exact group non-membership was not retained");
}

void run_cleanup_policy_meta_preferred_fixture_test() {
    const CleanupPolicyProtectionEvidence evidence =
        observe_fixture_policy(
            {FixturePackageMetadata{
                "group-only-candidate", "1-1", {}, {"base-devel"}, {}}},
            {{"core",
              {FixturePackageMetadata{
                   "base-devel", "1-2", {}, {}, {"different-tool"}},
               FixturePackageMetadata{
                   "compatibility-member", "1-1", {}, {"base-devel"}, {}}}}},
            "group-only-candidate");

    expect(
        evidence.configured_sync_base_devel.observation ==
                CleanupPolicyAuthorityObservation::Present &&
            evidence.configured_sync_base_devel.candidate_evaluation ==
                CleanupPolicyCandidateEvaluation::NotProtected &&
            evidence.base_devel_group.observation ==
                CleanupPolicyAuthorityObservation::NotObserved,
        "group fallback ran despite present exact meta authority");
}

void run_cleanup_policy_unknown_fixture_test() {
    const CleanupPolicyProtectionEvidence no_authority =
        observe_fixture_policy(
            {FixturePackageMetadata{"candidate"}},
            {{"core", {FixturePackageMetadata{"anchor"}}}},
            "candidate");
    expect(
        no_authority.configured_sync_base_devel.observation ==
                CleanupPolicyAuthorityObservation::Absent &&
            no_authority.base_devel_group.observation ==
                CleanupPolicyAuthorityObservation::Absent,
        "absent meta/group authority was not retained distinctly");

    const CleanupPolicyProtectionEvidence sync_unavailable =
        observe_fixture_policy(
            {FixturePackageMetadata{"candidate"}}, {}, "candidate",
            {"missing"});
    expect(
        sync_unavailable.configured_sync_base_devel.observation ==
                CleanupPolicyAuthorityObservation::Unavailable &&
            sync_unavailable.configured_sync_base_devel
                    .inventory_completeness ==
                CleanupPolicyMetadataCompleteness::Failed &&
            !sync_unavailable.failures.empty(),
        "required unavailable sync DB was flattened to absence");

    const CleanupPolicyProtectionEvidence malformed_meta =
        observe_fixture_policy(
            {FixturePackageMetadata{"candidate"}},
            {{"core", {FixturePackageMetadata{"base-devel", "1-2"}}}},
            "candidate");
    expect(
        malformed_meta.configured_sync_base_devel.observation ==
                CleanupPolicyAuthorityObservation::Present &&
            malformed_meta.configured_sync_base_devel
                    .inventory_completeness ==
                CleanupPolicyMetadataCompleteness::Incomplete &&
            malformed_meta.configured_sync_base_devel
                    .evaluation_completeness ==
                CleanupPolicyMetadataCompleteness::Failed &&
            !malformed_meta.failures.empty(),
        "malformed meta dependency inventory became negative evidence");

    const CleanupPolicyProtectionEvidence candidate_missing =
        observe_fixture_policy(
            {FixturePackageMetadata{"anchor"}},
            {{"core",
              {FixturePackageMetadata{
                  "base-devel", "1-2", {}, {}, {"candidate"}}}}},
            "candidate");
    expect(
        candidate_missing.candidate_metadata_completeness ==
                CleanupPolicyMetadataCompleteness::Incomplete &&
            !candidate_missing.candidate.has_value(),
        "missing candidate metadata was treated as complete");
}

void run_cleanup_policy_local_database_failure_fixture_test() {
    TemporaryDirectory fixture;
    const fs::path missing_database = fixture.path() / "missing-database";
    const CleanupPolicyProtectionEvidence evidence =
        query_cleanup_policy_protection_evidence(
            PacmanRepositoryConfiguration{
                PacmanDatabasePaths{"/", missing_database}, {"core"}},
            "candidate");
    expect(
        evidence.local_database_completeness ==
                CleanupPolicyMetadataCompleteness::Failed &&
            evidence.candidate_metadata_completeness ==
                CleanupPolicyMetadataCompleteness::Incomplete &&
            !evidence.failures.empty(),
        "local DB failure was flattened to candidate absence");
}

void run_cleanup_policy_contradiction_fixture_test() {
    const CleanupPolicyProtectionEvidence evidence =
        observe_fixture_policy(
            {FixturePackageMetadata{"toolchain"}},
            {{"core",
              {FixturePackageMetadata{
                  "base-devel", "1-2", {}, {}, {"toolchain"}}}},
             {"extra",
              {FixturePackageMetadata{
                  "base-devel", "1-3", {}, {}, {"different-tool"}}}}},
            "toolchain");
    expect(
        evidence.configured_sync_base_devel.meta_packages.size() == 2 &&
            evidence.consistency ==
                CleanupPolicyEvidenceConsistency::Contradictory,
        "contradictory configured meta inventories were not retained");
}

void run_runtime_consumers_fixture_test() {
    TemporaryDirectory fixture;
    const auto database = fixture.path() / "database";
    create_local_database(database, {FixturePackageMetadata{"candidate", "2-1", {"virtual-tool=3", "unversioned-tool"}},
                                     FixturePackageMetadata{"direct", "1-1", {}, {}, {"candidate>=2"}},
                                     FixturePackageMetadata{"version-mismatch", "1-1", {}, {}, {"candidate>=9"}},
                                     FixturePackageMetadata{"provider", "1-1", {}, {}, {"virtual-tool=3"}},
                                     FixturePackageMetadata{"provider-mismatch", "1-1", {}, {}, {"virtual-tool>=4"}},
                                     FixturePackageMetadata{"unversioned", "1-1", {}, {}, {"unversioned-tool"}},
                                     FixturePackageMetadata{"unversioned-mismatch", "1-1", {}, {}, {"unversioned-tool>=1"}},
                                     FixturePackageMetadata{"alternative", "1-1", {"virtual-tool=3"}},
                                     FixturePackageMetadata{"independent"}});
    auto session = PackageMetadataSession::open({"/", database});
    const auto result = session.query_installed_runtime_consumers("candidate");
    const auto* consumers = std::get_if<std::vector<std::string>>(&result);
    expect(consumers != nullptr, "real libalpm runtime consumer observation failed");
    auto sorted = *consumers;
    std::sort(sorted.begin(), sorted.end());
    expect(sorted == std::vector<std::string>{"direct", "provider", "unversioned"},
           "runtime consumers lost libalpm version/Provides semantics or used alternative-provider inference");
    const auto independent = session.query_installed_runtime_consumers("independent");
    expect(std::holds_alternative<std::vector<std::string>>(independent) &&
               std::get<std::vector<std::string>>(independent).empty(),
           "independent package acquired consumers");
    expect(std::holds_alternative<PackageMetadataFailure>(session.query_installed_runtime_consumers("absent")),
           "absent candidate became an authoritative empty consumer inventory");
    auto moved = std::move(session);
    expect(std::holds_alternative<PackageMetadataFailure>(session.query_installed_runtime_consumers("candidate")),
           "closed session became an authoritative empty consumer inventory");
}

void run_hold_package_configuration_smoke_test() {
    // No assertions about the host's configured values; only the real
    // pacman-conf/default configuration serialization boundary is exercised.
    const auto result = query_configured_hold_package_patterns();
    expect(std::holds_alternative<ConfiguredHoldPackagePatterns>(result),
           "fresh host HoldPkg configuration observation failed");
}

} // namespace

int main() {
    try {
        test_raw_capture_preserves_boundary_whitespace();
        run_hold_package_configuration_smoke_test();
        run_pacman_metadata_smoke_test();
        run_installed_identity_fixture_test();
        run_runtime_consumers_fixture_test();
        run_repository_metadata_smoke_test();
        run_current_arch_base_devel_policy_smoke_test();
        run_foreign_package_inventory_compatibility_test();
        run_cleanup_policy_installed_meta_fixture_test();
        run_cleanup_policy_sync_meta_fixture_test();
        run_cleanup_policy_provides_fixture_test();
        run_cleanup_policy_group_fixture_test();
        run_cleanup_policy_meta_preferred_fixture_test();
        run_cleanup_policy_unknown_fixture_test();
        run_cleanup_policy_local_database_failure_fixture_test();
        run_cleanup_policy_contradiction_fixture_test();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "package metadata integration test: all checks passed\n";
    return 0;
}
