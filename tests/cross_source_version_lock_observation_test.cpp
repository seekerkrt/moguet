#include "cross_source_version_lock_observation.hpp"

#include "aur_rpc.hpp"
#include "installed_package_relation_inventory.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

enum class AurResponseKind {
    Success,
    NotFound,
    SchemaFailure,
    QueryFailure,
    AllocationFailure,
};

struct AurResponse {
    AurResponseKind kind = AurResponseKind::NotFound;
    std::optional<AurPackageInfo> package;
    std::string diagnostic;
};

struct ObservationFixture {
    PacmanRepositoryConfiguration configuration;
    ForeignPackageInventoryResult foreign_inventory;
    InstalledPackageRelationInventoryResult installed_relations;
    InstalledPackageRuntimeDependencyMetadataInventoryResult
        runtime_dependencies;
    std::map<std::string, StrictRepositoryPackageQueryResult>
        repository_results;
    std::map<std::string, AurResponse> aur_results;
    std::vector<std::string> calls;
};

ObservationFixture g_fixture;

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

PackageRelationInstalledDatabaseIdentity installed_database_identity() {
    return PackageRelationInstalledDatabaseIdentity{
        std::filesystem::path("/"),
        std::filesystem::path("/var/lib/pacman")};
}

PackageRelationObservedPackage installed_package(
    std::string package_name, std::string version) {
    return PackageRelationObservedPackage{
        std::move(package_name),
        std::nullopt,
        ObservedVersion::available(
            ObservedVersionSource::InstalledExactPackage,
            std::move(version)),
        {},
        installed_database_identity(),
        PackageRelationObservationRole::Installed,
        {}};
}

DependencyRequirement dependency_requirement(
    const std::string& specification) {
    const DependencyRequirementParseResult parsed =
        parse_dependency_requirement(specification);
    expect(
        parsed.failure() == nullptr,
        "Fixture dependency parse failed: " + specification);
    const DependencyRequirement* requirement = parsed.requirement();
    expect(requirement != nullptr, "Fixture dependency requirement is missing");
    return *requirement;
}

AurPackageInfo aur_package(
    std::string package_name,
    std::string package_base,
    std::string version,
    std::vector<DependencyRequirement> dependencies) {
    AurPackageInfo package;
    package.Name = package_name;
    package.PackageBase = package_base;
    package.Version = version;
    package.constraint_metadata = AurPackageConstraintMetadata{
        std::move(package_name),
        std::move(package_base),
        ObservedVersion::available(
            ObservedVersionSource::AurExactPackage,
            std::move(version)),
        std::move(dependencies),
        {},
        {},
        {},
        {}};
    return package;
}

RepositoryPackagePresent repository_candidate(
    std::string package_name, std::string version) {
    return RepositoryPackagePresent{
        "extra",
        1,
        package_name,
        package_name,
        ObservedVersion::available(
            ObservedVersionSource::RepositoryExactPackage,
            std::move(version)),
        std::vector<std::string>{"core", "extra"},
        {}};
}

void reset_fixture() {
    g_fixture = ObservationFixture{};
    g_fixture.configuration = PacmanRepositoryConfiguration{
        PacmanDatabasePaths{"/", "/var/lib/pacman"},
        {"core", "extra"}};
    g_fixture.foreign_inventory = ForeignPackageInventory{};
    g_fixture.installed_relations = InstalledPackageRelationInventory{
        installed_database_identity(), {}};
    g_fixture.runtime_dependencies =
        InstalledPackageRuntimeDependencyMetadataInventory{};
}

void arrange_virtualbox() {
    reset_fixture();
    g_fixture.foreign_inventory = ForeignPackageInventory{
        InstalledPackageMetadata{
            "virtualbox-ext-oracle",
            "7.2.14-1",
            InstalledPackageReason::Explicit}};
    g_fixture.installed_relations = InstalledPackageRelationInventory{
        installed_database_identity(),
        {installed_package("virtualbox", "7.2.14-1"),
         installed_package(
             "virtualbox-ext-oracle", "7.2.14-1")}};
    g_fixture.runtime_dependencies =
        InstalledPackageRuntimeDependencyMetadataInventory{
            InstalledPackageRuntimeDependencyMetadata{
                "virtualbox-ext-oracle",
                {"virtualbox=7.2.14"}}};
    g_fixture.repository_results["virtualbox"] =
        repository_candidate("virtualbox", "7.2.16-1");
    g_fixture.aur_results["virtualbox-ext-oracle"] = AurResponse{
        AurResponseKind::Success,
        aur_package(
            "virtualbox-ext-oracle",
            "virtualbox-ext-oracle",
            "7.2.16-1",
            {dependency_requirement("virtualbox=7.2.16")}),
        {}};
}

bool has_issue(
    const CrossSourceVersionLockObservationResult& result,
    CrossSourceVersionLockObservationIssueKind kind) {
    for(const CrossSourceVersionLockObservationIssue& issue : result.issues) {
        if(issue.kind == kind) return true;
    }
    return false;
}

CrossSourceVersionLockAssessment require_single_assessment(
    const CrossSourceVersionLockObservationResult& result,
    const std::string& context) {
    expect(result.candidates.size() == 1, context + ": candidate count differs");
    return assess_cross_source_version_lock_candidate(
        result.candidates.front());
}

} // namespace

PacmanRepositoryConfiguration resolve_pacman_repository_configuration() {
    g_fixture.calls.push_back("repository-configuration");
    return g_fixture.configuration;
}

ForeignPackageInventoryResult query_foreign_package_inventory(
    const PacmanRepositoryConfiguration& configuration) {
    g_fixture.calls.push_back("foreign-inventory");
    if(configuration.repository_names !=
           g_fixture.configuration.repository_names ||
       configuration.database_paths.root_dir !=
           g_fixture.configuration.database_paths.root_dir ||
       configuration.database_paths.db_path !=
           g_fixture.configuration.database_paths.db_path) {
        throw std::runtime_error(
            "Foreign inventory received a different repository configuration");
    }
    return g_fixture.foreign_inventory;
}

InstalledPackageRelationInventoryResult query_installed_package_relations(
    const PacmanDatabasePaths& paths) {
    g_fixture.calls.push_back("installed-relations");
    if(paths.root_dir != g_fixture.configuration.database_paths.root_dir ||
       paths.db_path != g_fixture.configuration.database_paths.db_path) {
        throw std::runtime_error(
            "Installed relation query received different database paths");
    }
    return g_fixture.installed_relations;
}

InstalledPackageRuntimeDependencyMetadataInventoryResult
query_installed_package_runtime_dependency_metadata(
    const PacmanDatabasePaths& paths) {
    g_fixture.calls.push_back("installed-runtime-dependencies");
    if(paths.root_dir != g_fixture.configuration.database_paths.root_dir ||
       paths.db_path != g_fixture.configuration.database_paths.db_path) {
        throw std::runtime_error(
            "Runtime dependency query received different database paths");
    }
    return g_fixture.runtime_dependencies;
}

StrictRepositoryPackageQueryResult query_repository_package_strict(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& package_name) {
    g_fixture.calls.push_back("repository:" + package_name);
    if(configuration.repository_names !=
           g_fixture.configuration.repository_names ||
       configuration.database_paths.root_dir !=
           g_fixture.configuration.database_paths.root_dir ||
       configuration.database_paths.db_path !=
           g_fixture.configuration.database_paths.db_path) {
        throw std::runtime_error(
            "Repository candidate query received a different configuration");
    }
    const auto result = g_fixture.repository_results.find(package_name);
    return result == g_fixture.repository_results.end()
               ? StrictRepositoryPackageQueryResult{
                     RepositoryPackageNotFound{
                         configuration.repository_names}}
               : result->second;
}

std::optional<AurPackageInfo> AurClient::info_strict(
    const std::string& package_name) {
    g_fixture.calls.push_back("aur:" + package_name);
    const auto response = g_fixture.aur_results.find(package_name);
    if(response == g_fixture.aur_results.end()) {
        throw std::runtime_error("Unexpected exact AUR query: " + package_name);
    }
    switch(response->second.kind) {
        case AurResponseKind::Success:
            return response->second.package;
        case AurResponseKind::NotFound:
            return std::nullopt;
        case AurResponseKind::SchemaFailure:
            throw AurRpcResponseError(response->second.diagnostic);
        case AurResponseKind::QueryFailure:
            throw std::runtime_error(response->second.diagnostic);
        case AurResponseKind::AllocationFailure:
            throw std::bad_alloc();
    }
    throw std::logic_error("Unknown exact AUR fixture response");
}

namespace {

void test_virtualbox_candidate_observation_is_complete_and_compatible() {
    arrange_virtualbox();

    const CrossSourceVersionLockObservationResult result =
        observe_cross_source_version_lock_candidates();
    expect(
        result.status == CrossSourceVersionLockObservationStatus::Complete &&
            result.issues.empty(),
        "VirtualBox candidate observation was not complete");
    const CrossSourceVersionLockAssessment assessment =
        require_single_assessment(result, "VirtualBox candidate");
    expect(
        assessment.status ==
            CrossSourceVersionLockStatus::CompatibleReplacement,
        "VirtualBox candidate did not project CompatibleReplacement");
    expect(
        g_fixture.calls ==
            std::vector<std::string>{
                "repository-configuration",
                "foreign-inventory",
                "installed-relations",
                "installed-runtime-dependencies",
                "repository:virtualbox",
                "aur:virtualbox-ext-oracle"},
        "Candidate observation crossed an unexpected authority boundary");
}

void test_complete_zero_differs_from_observation_failure() {
    arrange_virtualbox();
    g_fixture.repository_results["virtualbox"] =
        RepositoryPackageNotFound{
            std::vector<std::string>{"core", "extra"}};

    const CrossSourceVersionLockObservationResult no_candidate =
        observe_cross_source_version_lock_candidates();
    expect(
        no_candidate.status ==
                CrossSourceVersionLockObservationStatus::Complete &&
            no_candidate.candidates.empty() &&
            no_candidate.issues.empty(),
        "Confirmed repository absence was not complete zero correlation");

    reset_fixture();
    g_fixture.foreign_inventory = PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "fixture foreign inventory failure"};
    const CrossSourceVersionLockObservationResult failed =
        observe_cross_source_version_lock_candidates();
    expect(
        failed.status == CrossSourceVersionLockObservationStatus::Failed &&
            failed.candidates.empty() &&
            has_issue(
                failed,
                CrossSourceVersionLockObservationIssueKind::
                    ForeignInventoryUnavailable),
        "Observation failure was flattened into complete empty candidates");
}

void test_installed_dependency_metadata_unavailable_fails_closed() {
    arrange_virtualbox();
    g_fixture.runtime_dependencies =
        InstalledPackageRuntimeDependencyMetadataInventoryFailure{
            {},
            std::size_t(0),
            PackageMetadataFailure{
                PackageMetadataErrorCode::QueryFailed,
                "fixture installed dependency failure"}};

    const CrossSourceVersionLockObservationResult result =
        observe_cross_source_version_lock_candidates();
    expect(
        result.status == CrossSourceVersionLockObservationStatus::Failed &&
            result.candidates.empty() &&
            has_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    InstalledRuntimeDependencyInventoryUnavailable),
        "Installed dependency failure became an empty success");
}

void test_repository_candidate_query_failure_is_partial() {
    arrange_virtualbox();
    g_fixture.repository_results["virtualbox"] = RepositoryMetadataFailure{
        RepositoryMetadataFailureKind::SyncDatabaseUnavailable,
        std::string("extra"),
        "fixture repository query failure",
        std::vector<std::string>{"core", "extra"}};

    const CrossSourceVersionLockObservationResult result =
        observe_cross_source_version_lock_candidates();
    expect(
        result.status == CrossSourceVersionLockObservationStatus::Partial &&
            result.candidates.empty() &&
            has_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    RepositoryCandidateUnavailable),
        "Repository query failure was treated as confirmed absence");
}

void test_aur_replacement_not_found_is_complete() {
    arrange_virtualbox();
    g_fixture.aur_results["virtualbox-ext-oracle"] = AurResponse{
        AurResponseKind::NotFound, std::nullopt, {}};

    const CrossSourceVersionLockObservationResult result =
        observe_cross_source_version_lock_candidates();
    const CrossSourceVersionLockAssessment assessment =
        require_single_assessment(result, "AUR replacement not found");
    expect(
        result.status == CrossSourceVersionLockObservationStatus::Complete &&
            result.issues.empty() &&
            assessment.status ==
                CrossSourceVersionLockStatus::MissingReplacement,
        "Confirmed AUR absence was not retained losslessly");
}

void test_aur_schema_failure_is_metadata_unavailable() {
    arrange_virtualbox();
    g_fixture.aur_results["virtualbox-ext-oracle"] = AurResponse{
        AurResponseKind::SchemaFailure,
        std::nullopt,
        "fixture AUR schema failure"};

    const CrossSourceVersionLockObservationResult result =
        observe_cross_source_version_lock_candidates();
    const CrossSourceVersionLockAssessment assessment =
        require_single_assessment(result, "AUR metadata unavailable");
    expect(
        result.status == CrossSourceVersionLockObservationStatus::Partial &&
            assessment.status == CrossSourceVersionLockStatus::Unknown &&
            has_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    AurReplacementMetadataUnavailable),
        "AUR schema failure was not typed metadata-unavailable");
}

void test_aur_query_failure_is_not_not_found() {
    arrange_virtualbox();
    g_fixture.aur_results["virtualbox-ext-oracle"] = AurResponse{
        AurResponseKind::QueryFailure,
        std::nullopt,
        "fixture AUR transport failure"};

    const CrossSourceVersionLockObservationResult result =
        observe_cross_source_version_lock_candidates();
    const CrossSourceVersionLockAssessment assessment =
        require_single_assessment(result, "AUR query failure");
    expect(
        result.status == CrossSourceVersionLockObservationStatus::Partial &&
            assessment.status ==
                CrossSourceVersionLockStatus::QueryFailure &&
            assessment.status !=
                CrossSourceVersionLockStatus::MissingReplacement &&
            has_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    AurReplacementQueryFailure),
        "AUR query failure was flattened into absence");
}

void test_aur_allocation_failure_propagates() {
    arrange_virtualbox();
    g_fixture.aur_results["virtualbox-ext-oracle"] = AurResponse{
        AurResponseKind::AllocationFailure,
        std::nullopt,
        {}};

    bool propagated = false;
    try {
        static_cast<void>(observe_cross_source_version_lock_candidates());
    } catch(const std::bad_alloc&) {
        propagated = true;
    }
    expect(
        propagated,
        "AUR allocation failure was flattened into a query failure");
}

void test_indirect_replacement_dependency_remains_unknown() {
    arrange_virtualbox();
    g_fixture.aur_results["virtualbox-ext-oracle"] = AurResponse{
        AurResponseKind::Success,
        aur_package(
            "virtualbox-ext-oracle",
            "virtualbox-ext-oracle",
            "7.2.16-1",
            {dependency_requirement(
                "virtualbox-provider=7.2.16")}),
        {}};

    const CrossSourceVersionLockObservationResult result =
        observe_cross_source_version_lock_candidates();
    const CrossSourceVersionLockAssessment assessment =
        require_single_assessment(result, "indirect replacement dependency");
    expect(
        result.status == CrossSourceVersionLockObservationStatus::Complete &&
            assessment.status == CrossSourceVersionLockStatus::Unknown,
        "Indirect/provider dependency was resolved implicitly");
}

void test_duplicate_matching_installed_dependency_is_partial() {
    arrange_virtualbox();
    g_fixture.runtime_dependencies =
        InstalledPackageRuntimeDependencyMetadataInventory{
            InstalledPackageRuntimeDependencyMetadata{
                "virtualbox-ext-oracle",
                {"virtualbox=7.2.14",
                 "virtualbox=7.2.14"}}};

    const CrossSourceVersionLockObservationResult result =
        observe_cross_source_version_lock_candidates();
    expect(
        result.status == CrossSourceVersionLockObservationStatus::Partial &&
            result.candidates.empty() &&
            has_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    DuplicateInstalledRuntimeDependency) &&
            std::find(
                g_fixture.calls.begin(),
                g_fixture.calls.end(),
                "aur:virtualbox-ext-oracle") ==
                g_fixture.calls.end(),
        "Duplicate installed dependency was selected implicitly");
}

void test_ambiguous_repository_candidate_identity_reaches_assessment() {
    arrange_virtualbox();
    RepositoryPackagePresent ambiguous =
        repository_candidate("virtualbox-alternate", "7.2.16-1");
    g_fixture.repository_results["virtualbox"] = std::move(ambiguous);

    const CrossSourceVersionLockObservationResult result =
        observe_cross_source_version_lock_candidates();
    const CrossSourceVersionLockAssessment assessment =
        require_single_assessment(result, "ambiguous candidate identity");
    expect(
        result.status == CrossSourceVersionLockObservationStatus::Complete &&
            assessment.status ==
                CrossSourceVersionLockStatus::Ambiguous,
        "Ambiguous repository identity was accepted as a correlation");
}


void arrange_coordinated_transition() {
    arrange_virtualbox();
    g_fixture.foreign_inventory = ForeignPackageInventory{
        {"virtualbox-ext-oracle", "7.2.16-1", InstalledPackageReason::Dependency,
         InstalledPackageBaseIdentity::known("virtualbox-ext-oracle")}};
    g_fixture.installed_relations = InstalledPackageRelationInventory{
        installed_database_identity(), {installed_package("virtualbox", "7.2.16-1"), installed_package("virtualbox-ext-oracle", "7.2.16-1")}};
    g_fixture.runtime_dependencies = InstalledPackageRuntimeDependencyMetadataInventory{
        {"virtualbox", {}, "7.2.16-1"},
        {"virtualbox-ext-oracle", {"virtualbox=7.2.16"}, "7.2.16-1"}};
    g_fixture.repository_results["virtualbox"] = repository_candidate("virtualbox", "7.2.18-1");
    g_fixture.aur_results["virtualbox-ext-oracle"] = AurResponse{
        AurResponseKind::Success,
        aur_package("virtualbox-ext-oracle", "virtualbox-ext-oracle", "7.2.18-1",
                    {dependency_requirement("virtualbox=7.2.18")}),
        {}};
}

CrossSourceVersionLockCorrelationResult observe_transition() {
    return observe_cross_source_version_lock_correlation(
        CrossSourceVersionLockObservationBasis::BeforeRepositoryMutation);
}

void test_coordinated_transition_snapshot_and_phases() {
    arrange_coordinated_transition();
    auto correlation = observe_transition();
    expect(correlation.transition_plans.size() == 1, "Transition plan is missing");
    const auto plan = correlation.transition_plans.front();
    expect(plan.status == CrossSourceTransitionPlanStatus::ReadOnlyReady &&
               plan.reason == CrossSourceTransitionPlanReason::None &&
               plan.removal_safety.status == CrossSourceRemovalSafetyStatus::PreservesRuntimeDependencies &&
               plan.removal_safety.affected_requirements.empty(),
           "Complete VirtualBox transition was not structurally ready");
    expect(plan.phases == std::vector<CrossSourceTransitionPhase>{
                              CrossSourceTransitionPhase::RemoveInstalledForeign,
                              CrossSourceTransitionPhase::RepositorySystemUpgrade,
                              CrossSourceTransitionPhase::InstallAurReplacement,
                              CrossSourceTransitionPhase::VerifyPostState},
           "Transition phase ownership/order changed");
    expect(plan.installed_foreign->version == "7.2.16-1" &&
               *plan.installed_foreign->package_base.value() == "virtualbox-ext-oracle" &&
               plan.expected_install_reason == InstalledPackageReason::Dependency &&
               plan.correlation.evidence.installed_consumer.requirement.raw_specification() == "virtualbox=7.2.16" &&
               plan.correlation.replacement_requirement->raw_specification() == "virtualbox=7.2.18" &&
               *plan.correlation.evidence.repository_upgrade.repository_candidate.package_version->version() == "7.2.18-1",
           "Expected-state identity, PackageBase, reason or exact constraints were lost");
    expect(plan.requires_explicit_confirmation && plan.requires_mutation_time_revalidation &&
               !plan.is_atomic && !plan.automatic_rollback,
           "Read-only execution boundary changed");
    expect(g_fixture.calls == std::vector<std::string>{"repository-configuration", "foreign-inventory",
                                                       "installed-relations", "installed-runtime-dependencies", "repository:virtualbox", "aur:virtualbox-ext-oracle"},
           "Planning added calls outside the read-only metadata boundaries");
    auto changed_snapshot = plan;
    expect(changed_snapshot == plan, "Owned expected-state snapshot is not comparable");
    changed_snapshot.installed_snapshot->runtime_requirements.front().requirements.push_back(dependency_requirement("new-dependency"));
    expect(changed_snapshot != plan, "Runtime assumption drift is absent from snapshot comparison");
    changed_snapshot = plan;
    changed_snapshot.installed_foreign->reason = InstalledPackageReason::Explicit;
    expect(changed_snapshot != plan, "Install reason drift is absent from snapshot comparison");
    reset_fixture();
    expect(plan.installed_snapshot->packages.size() == 2 &&
               plan.installed_snapshot->runtime_requirements.size() == 2,
           "Plan snapshot borrowed the observation session");
    correlation.basis = CrossSourceVersionLockObservationBasis::AfterRepositoryFailure;
    auto after_failure = plan_cross_source_coordinated_transitions(correlation);
    expect(after_failure.front().status == CrossSourceTransitionPlanStatus::Unsupported &&
               after_failure.front().phases.empty(),
           "Post-failure authority became a preflight plan");
}

void test_coordinated_transition_negative_matrix() {
    for(const std::string scenario : {"missing", "query", "incompatible", "ambiguous", "partial", "failed",
                                      "unknown-reason", "inventory-gap", "version-drift", "version-drift-with-gap", "reverse-dependent",
                                      "duplicate-installed", "overlap", "independent-multiple", "non-exact", "replacement-candidates", "replacement-relation", "collector-failure", "unrelated"}) {
        arrange_coordinated_transition();
        auto& dependencies = std::get<InstalledPackageRuntimeDependencyMetadataInventory>(g_fixture.runtime_dependencies);
        auto& packages = std::get<InstalledPackageRelationInventory>(g_fixture.installed_relations).packages;
        auto& replacement = *g_fixture.aur_results["virtualbox-ext-oracle"].package;
        auto expected_status = CrossSourceTransitionPlanStatus::Incomplete;
        if(scenario == "missing") {
            g_fixture.aur_results["virtualbox-ext-oracle"].kind = AurResponseKind::NotFound;
            expected_status = CrossSourceTransitionPlanStatus::Blocked;
        } else if(scenario == "query") {
            g_fixture.aur_results["virtualbox-ext-oracle"].kind = AurResponseKind::QueryFailure;
        } else if(scenario == "incompatible") {
            replacement.constraint_metadata->depends = {dependency_requirement("virtualbox=7.2.16")};
            expected_status = CrossSourceTransitionPlanStatus::Blocked;
        } else if(scenario == "ambiguous") {
            replacement.constraint_metadata->depends.push_back(dependency_requirement("virtualbox=7.2.18"));
            expected_status = CrossSourceTransitionPlanStatus::Ambiguous;
        } else if(scenario == "replacement-relation") {
            replacement.constraint_metadata->relations.emplace_back("virtualbox-ext-oracle", "virtualbox-ext-oracle",
                                                                    PackageRelationKind::Conflict, "other-package", "other-package", std::nullopt);
            expected_status = CrossSourceTransitionPlanStatus::Unsupported;
        } else if(scenario == "unknown-reason") {
            std::get<ForeignPackageInventory>(g_fixture.foreign_inventory).front().reason = InstalledPackageReason::Unknown;
        } else if(scenario == "inventory-gap") {
            dependencies.erase(dependencies.begin());
        } else if(scenario == "version-drift" || scenario == "version-drift-with-gap") {
            dependencies.front().installed_version = "7.2.14-1";
            if(scenario == "version-drift-with-gap") {
                packages.push_back(installed_package("unrelated", "1-1"));
            }
        } else if(scenario == "reverse-dependent") {
            packages.push_back(installed_package("other-package", "1-1"));
            dependencies.push_back({"other-package", {"virtualbox-ext-oracle"}, "1-1"});
            expected_status = CrossSourceTransitionPlanStatus::Blocked;
        } else if(scenario == "duplicate-installed") {
            packages.push_back(installed_package("unrelated", "1-1"));
            packages.push_back(installed_package("unrelated", "1-1"));
        } else if(scenario == "non-exact") {
            replacement.constraint_metadata->depends = {dependency_requirement("virtualbox>=7.2.18")};
            expected_status = CrossSourceTransitionPlanStatus::Unsupported;
        } else if(scenario == "unrelated") {
            g_fixture.repository_results["virtualbox"] = repository_candidate("virtualbox", "7.2.16-1");
        }
        auto correlation = observe_transition();
        if(scenario == "unrelated") {
            expect(correlation.transition_plans.empty(), "Unrelated update created a coordinated plan");
            continue;
        }
        if(scenario == "replacement-candidates") {
            auto& replacements = std::get<AurReplacementCandidateQuerySuccess>(correlation.assessments.front().evidence.aur_replacement).candidates;
            replacements.push_back(replacements.front());
            expected_status = CrossSourceTransitionPlanStatus::Ambiguous;
        } else if(scenario == "collector-failure") {
            correlation.failure = CrossSourceVersionLockCorrelationFailure{CrossSourceVersionLockCorrelationFailureKind::UnexpectedException, "fixture failure"};
        } else if(scenario == "partial" || scenario == "failed") {
            correlation.observation->status = scenario == "partial" ? CrossSourceVersionLockObservationStatus::Partial
                                                                    : CrossSourceVersionLockObservationStatus::Failed;
        } else if(scenario == "overlap" || scenario == "independent-multiple") {
            auto second = correlation.assessments.front();
            if(scenario == "independent-multiple") {
                second.evidence.installed_consumer.package.package_name = "another-consumer";
                second.evidence.repository_upgrade.repository_candidate.package_name = "another-repo";
            }
            correlation.assessments.push_back(second);
            correlation.possible_blocker_assessment_indices.push_back(1);
            expected_status = CrossSourceTransitionPlanStatus::Unsupported;
        }
        const auto plans = plan_cross_source_coordinated_transitions(correlation);
        expect(!plans.empty(), scenario + ": expected typed non-ready plan");
        for(const auto& plan : plans) {
            expect(plan.status == expected_status && plan.phases.empty(), scenario + ": unsafe ready/removal phase");
            if(scenario == "version-drift" || scenario == "version-drift-with-gap" ||
               scenario == "duplicate-installed" || scenario == "inventory-gap") {
                const auto expected_completeness = scenario == "inventory-gap"
                                                       ? PackageRelationObservationCompleteness::Partial
                                                       : PackageRelationObservationCompleteness::Invalid;
                expect(plan.installed_snapshot.has_value() &&
                           plan.installed_snapshot->completeness == expected_completeness,
                       scenario + ": installed snapshot lost invalidity or misclassified a coverage gap");
            }
        }
        if(scenario == "query") expect(plans.front().reason == CrossSourceTransitionPlanReason::ReplacementQueryFailure,
                                       "Query failure became missing");
        if(scenario == "unknown-reason") expect(plans.front().expected_install_reason == InstalledPackageReason::Unknown,
                                                "Unknown reason became Explicit");
        if(scenario == "reverse-dependent") expect(plans.front().removal_safety.affected_requirements.front().dependent_package_name == "other-package",
                                                   "Reverse dependent evidence was lost");
        if(scenario == "overlap") expect(plans.front().reason == CrossSourceTransitionPlanReason::OverlappingCandidates, "Overlap was not distinguished");
    }
}

void test_removal_provides_and_nonexact_requirements() {
    for(const bool has_alternative : {false, true}) {
        for(const std::string specification : {"virtualbox-extension", "virtualbox-extension=7.2.16", "virtualbox-extension>=7.2"}) {
            arrange_coordinated_transition();
            auto& packages = std::get<InstalledPackageRelationInventory>(g_fixture.installed_relations).packages;
            const auto parsed = parse_provider_capability("virtualbox-extension=7.2.16");
            expect(parsed.capability() != nullptr, "Provider fixture parse failed");
            const PackageRelationObservedCapability capability{*parsed.capability(),
                                                               ObservedVersion::from_provider_capability(ObservedVersionSource::InstalledProviderCapability, *parsed.capability())};
            packages.back().provides.push_back(capability);
            packages.push_back(installed_package("other-package", "1-1"));
            auto& dependencies = std::get<InstalledPackageRuntimeDependencyMetadataInventory>(g_fixture.runtime_dependencies);
            dependencies.push_back({"other-package", {specification}, "1-1"});
            if(has_alternative) {
                packages.push_back(installed_package("retained-provider", "99-1"));
                packages.back().provides.push_back(capability);
                dependencies.push_back({"retained-provider", {}, "99-1"});
            }
            const auto correlation = observe_transition();
            const auto& plan = correlation.transition_plans.front();
            expect(plan.status == (has_alternative ? CrossSourceTransitionPlanStatus::ReadOnlyReady : CrossSourceTransitionPlanStatus::Blocked),
                   specification + ": remaining provider satisfaction was lost or guessed");
            expect(plan.removal_safety.affected_requirements.size() == 1 &&
                       plan.removal_safety.affected_requirements.front().remaining_satisfier_indices.size() == (has_alternative ? 1U : 0U),
                   "Removal provider evidence was not retained");
        }
    }
}

} // namespace

void run_cross_source_version_lock_observation_tests() {
    test_coordinated_transition_snapshot_and_phases();
    test_coordinated_transition_negative_matrix();
    test_removal_provides_and_nonexact_requirements();
    test_virtualbox_candidate_observation_is_complete_and_compatible();
    test_complete_zero_differs_from_observation_failure();
    test_installed_dependency_metadata_unavailable_fails_closed();
    test_repository_candidate_query_failure_is_partial();
    test_aur_replacement_not_found_is_complete();
    test_aur_schema_failure_is_metadata_unavailable();
    test_aur_query_failure_is_not_not_found();
    test_aur_allocation_failure_propagates();
    test_indirect_replacement_dependency_remains_unknown();
    test_duplicate_matching_installed_dependency_is_partial();
    test_ambiguous_repository_candidate_identity_reaches_assessment();
}
