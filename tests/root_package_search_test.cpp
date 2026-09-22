#include "root_package_search.hpp"
#include "root_package_selection.hpp"
#include "root_package_route_projection.hpp"

#include "stubs/root-package-search/search_stub.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace stub = root_package_search_test_stub;

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

RepositoryPackageSearchMatch repository_match(
    std::string repository_name,
    std::string package_name,
    std::optional<std::string> version = std::nullopt,
    std::optional<std::string> description = std::nullopt,
    RepositoryPackageSearchMatchKind kind =
        RepositoryPackageSearchMatchKind::Search,
    std::optional<std::string> group_name = std::nullopt) {
    return RepositoryPackageSearchMatch{
        std::move(repository_name), std::move(package_name),
        std::move(version), std::move(description), kind,
        std::move(group_name)};
}

AurPackageInfo aur_package(
    std::string package_name,
    std::string package_base,
    std::string version = {},
    std::string description = {}) {
    AurPackageInfo package;
    package.Name = std::move(package_name);
    package.PackageBase = std::move(package_base);
    package.Version = std::move(version);
    package.Description = std::move(description);
    return package;
}

RootPackageSearchSnapshot require_snapshot(
    RootPackageSearchResult result,
    std::string_view context) {
    const auto* snapshot = std::get_if<RootPackageSearchSnapshot>(&result);
    expect(
        snapshot != nullptr,
        std::string(context) + ": expected snapshot");
    return *snapshot;
}

InvalidRootPackageSearchSnapshot require_invalid(
    RootPackageSearchResult result,
    std::string_view context) {
    const auto* invalid =
        std::get_if<InvalidRootPackageSearchSnapshot>(&result);
    expect(
        invalid != nullptr,
        std::string(context) + ": expected invalid snapshot");
    return *invalid;
}

const RepositoryRootPackageIdentity& require_repository_identity(
    const RootPackageSearchCandidate& entry,
    std::string_view context) {
    const auto* identity = std::get_if<RepositoryRootPackageIdentity>(
        &entry.candidate.identity());
    expect(
        identity != nullptr,
        std::string(context) + ": expected repository identity");
    return *identity;
}

const AurRootPackageIdentity& require_aur_identity(
    const RootPackageSearchCandidate& entry,
    std::string_view context) {
    const auto* identity =
        std::get_if<AurRootPackageIdentity>(&entry.candidate.identity());
    expect(
        identity != nullptr,
        std::string(context) + ": expected AUR identity");
    return *identity;
}

void test_source_scope_queries_only_enabled_adapters() {
    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{
        {"core", "extra"}, {}});
    const RootPackageSearchSnapshot repository = require_snapshot(
        search_root_package_candidates(
            "repo-query", RootPackageSearchScope::Repository),
        "repository-only search");
    expect(repository.candidates.empty(), "repository-only empty differs");
    expect(
        repository.repository_order ==
            std::vector<std::string>{"core", "extra"},
        "repository-only search lost configured repository order");
    expect(
        stub::repository_queries() ==
                std::vector<std::string>{"repo-query"} &&
            stub::aur_query_count() == 0,
        "repository-only search crossed source boundary");

    stub::reset();
    stub::enqueue_aur_result({});
    const RootPackageSearchSnapshot aur = require_snapshot(
        search_root_package_candidates(
            "aur-query", RootPackageSearchScope::Aur),
        "AUR-only search");
    expect(aur.candidates.empty(), "AUR-only empty differs");
    expect(
        !aur.repository_order.has_value(),
        "AUR-only search flattened unqueried repository order to empty");
    expect(
        stub::repository_query_count() == 0 &&
            stub::aur_queries() ==
                std::vector<std::string>{"aur-query"},
        "AUR-only search crossed source boundary");

    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{});
    stub::enqueue_aur_result({});
    static_cast<void>(require_snapshot(
        search_root_package_candidates(
            "all-query", RootPackageSearchScope::All),
        "all-source search"));
    expect(
        stub::repository_queries() ==
                std::vector<std::string>{"all-query"} &&
            stub::aur_queries() ==
                std::vector<std::string>{"all-query"},
        "all-source query identity differs");
}

void test_source_failure_never_publishes_partial_snapshot() {
    stub::reset();
    stub::enqueue_repository_result(PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "repository search failed"});
    RootPackageSearchResult repository_failure =
        search_root_package_candidates(
            "partial", RootPackageSearchScope::All);
    const auto* repository_error =
        std::get_if<RepositoryRootPackageSearchFailure>(
            &repository_failure);
    expect(
        repository_error != nullptr &&
            repository_error->failure.code ==
                PackageMetadataErrorCode::QueryFailed,
        "repository failure was flattened");
    expect(
        stub::aur_query_count() == 0,
        "AUR query ran after repository source failure");

    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{
        {"core"},
        {repository_match("core", "partial-package")}});
    stub::enqueue_aur_failure("AUR search failed");
    RootPackageSearchResult aur_failure =
        search_root_package_candidates(
            "partial", RootPackageSearchScope::All);
    const auto* aur_error =
        std::get_if<AurRootPackageSearchFailure>(&aur_failure);
    expect(
        aur_error != nullptr &&
            aur_error->diagnostic == "AUR search failed",
        "AUR failure was flattened or lost");
    expect(
        std::get_if<RootPackageSearchSnapshot>(&aur_failure) == nullptr,
        "official partial snapshot escaped after AUR failure");
}

void test_merge_sort_and_source_identity_contract() {
    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{
        {"testing", "core", "aur"},
        {
            repository_match("core", "shared", "1.0-1"),
            repository_match("aur", "shared", "2.0-1"),
            repository_match("testing", "shared", "3.0-1"),
            repository_match("core", "z-last", "1.0-1"),
            repository_match("core", "a-first", "1.0-1"),
        }});
    stub::enqueue_aur_result({aur_package("shared", "shared-base", "4.0-1"),
                              aur_package("middle", "middle-base", "1.0-1")});

    const RootPackageSearchSnapshot snapshot = require_snapshot(
        search_root_package_candidates(
            "ordered", RootPackageSearchScope::All),
        "ordered aggregation");
    expect(snapshot.candidates.size() == 7, "ordered candidate count differs");
    expect(
        snapshot.candidates[0].candidate.package_name() == "a-first" &&
            snapshot.candidates[1].candidate.package_name() == "middle" &&
            snapshot.candidates[2].candidate.package_name() == "shared" &&
            snapshot.candidates[6].candidate.package_name() == "z-last",
        "bytewise package ordering differs");

    const auto& first_shared = require_repository_identity(
        snapshot.candidates[2], "first shared");
    const auto& second_shared = require_repository_identity(
        snapshot.candidates[3], "second shared");
    const auto& third_shared = require_repository_identity(
        snapshot.candidates[4], "third shared");
    const auto& aur_shared = require_aur_identity(
        snapshot.candidates[5], "AUR shared");
    expect(
        first_shared.repository_name == "testing" &&
            second_shared.repository_name == "core" &&
            third_shared.repository_name == "aur",
        "configured repository ordering differs");
    expect(
        aur_shared.package_base == "shared-base" &&
            snapshot.candidates[5].candidate.source_kind() ==
                RootPackageSourceKind::Aur,
        "AUR identity was flattened or repository aur was conflated");
}

void test_duplicate_metadata_and_group_membership_are_unioned() {
    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{
        {"core"},
        {
            repository_match(
                "core", "grouped", "1.0-1", std::nullopt),
            repository_match(
                "core", "grouped", std::nullopt, "description",
                RepositoryPackageSearchMatchKind::ExactGroup,
                "grouped"),
            repository_match(
                "core", "grouped", "1.0-1", "description",
                RepositoryPackageSearchMatchKind::ExactGroup,
                "grouped"),
        }});

    const RootPackageSearchSnapshot snapshot = require_snapshot(
        search_root_package_candidates(
            "grouped", RootPackageSearchScope::Repository),
        "duplicate/group aggregation");
    expect(snapshot.candidates.size() == 1, "duplicate identity was not merged");
    expect(
        snapshot.candidates[0].candidate.package_name() == "grouped" &&
            snapshot.candidates[0].candidate.presentation() ==
                RootPackageCandidatePresentation{
                    "1.0-1", "description"},
        "compatible presentation metadata was not merged");
    expect(
        snapshot.candidates[0].selectable_group_names ==
            std::vector<std::string>({"grouped"}),
        "group membership was not set-unioned and sorted");
}

void test_unsafe_and_inexact_group_selectors_fail_closed() {
    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{
        {"core"},
        {repository_match(
            "core", "unsafe-member", "1.0-1", std::nullopt,
            RepositoryPackageSearchMatchKind::ExactGroup,
            "unsafe group")}});
    const RootPackageSearchSnapshot unsafe_group = require_snapshot(
        search_root_package_candidates(
            "unsafe group", RootPackageSearchScope::Repository),
        "unsafe group aggregation");
    expect(
        unsafe_group.candidates.size() == 1 &&
            unsafe_group.candidates[0].candidate.package_name() ==
                "unsafe-member" &&
            unsafe_group.candidates[0].selectable_group_names.empty(),
        "unsafe group selector leaked or member candidate was dropped");

    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{
        {"core"},
        {
            repository_match(
                "core", "wrong-group", "1.0-1", std::nullopt,
                RepositoryPackageSearchMatchKind::ExactGroup,
                "different-query"),
            repository_match(
                "core", "missing-group", "1.0-1", std::nullopt,
                RepositoryPackageSearchMatchKind::ExactGroup),
            repository_match(
                "core", "unexpected-group", "1.0-1",
                std::nullopt,
                RepositoryPackageSearchMatchKind::Search,
                "query"),
        }});
    const InvalidRootPackageSearchSnapshot invalid = require_invalid(
        search_root_package_candidates(
            "query", RootPackageSearchScope::Repository),
        "inexact group provenance");
    expect(
        invalid.invalid_group_matches.size() == 3,
        "inexact or malformed group provenance was accepted");
}

void test_candidate_validation_and_pair_inconsistency_fail_snapshot() {
    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{
        {"core"},
        {repository_match("core", "bad package")}});
    const InvalidRootPackageSearchSnapshot validation = require_invalid(
        search_root_package_candidates(
            "invalid", RootPackageSearchScope::Repository),
        "invalid candidate");
    expect(
        validation.validation_failures.size() == 1 &&
            validation.candidate_pair_issues.empty(),
        "candidate validation failure detail differs");

    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{
        {"core"},
        {
            repository_match("core", "conflict", "1.0-1"),
            repository_match("core", "conflict", "2.0-1"),
        }});
    const InvalidRootPackageSearchSnapshot metadata_conflict = require_invalid(
        search_root_package_candidates(
            "conflict", RootPackageSearchScope::Repository),
        "metadata conflict");
    expect(
        metadata_conflict.candidate_pair_issues.size() == 1 &&
            std::get_if<ConflictingRootPackageCandidateMetadata>(
                &metadata_conflict.candidate_pair_issues.front()) !=
                nullptr,
        "metadata conflict was not retained");

    stub::reset();
    stub::enqueue_aur_result({aur_package("split-child", "first-base"),
                              aur_package("split-child", "second-base")});
    const InvalidRootPackageSearchSnapshot package_base_conflict =
        require_invalid(
            search_root_package_candidates(
                "split-child", RootPackageSearchScope::Aur),
            "PackageBase inconsistency");
    expect(
        package_base_conflict.candidate_pair_issues.size() == 1 &&
            std::get_if<InconsistentAurRootPackageBase>(
                &package_base_conflict.candidate_pair_issues.front()) !=
                nullptr,
        "AUR PackageBase inconsistency was not retained");
}

void test_repository_order_is_validated_before_sorting() {
    stub::reset();
    stub::enqueue_repository_result(RepositoryPackageSearchSnapshot{
        {"core", "core"},
        {repository_match("missing-rank", "package")}});
    const InvalidRootPackageSearchSnapshot invalid = require_invalid(
        search_root_package_candidates(
            "rank", RootPackageSearchScope::Repository),
        "repository rank validation");
    const std::vector<RepositoryRootPackageIdentity> expected_unranked{
        {"missing-rank", "package"}};
    expect(
        invalid.duplicate_repository_order_entries ==
                std::vector<std::string>{"core"} &&
            invalid.unranked_repository_candidates == expected_unranked,
        "invalid repository ordering was accepted");
}

void test_compatible_aur_duplicates_merge_owned_metadata() {
    stub::reset();
    std::string package_name = "owned-child";
    stub::enqueue_aur_result({aur_package(package_name, "owned-base", "1.0-1"),
                              aur_package(package_name, "owned-base", {}, "owned description")});

    RootPackageSearchResult result = search_root_package_candidates(
        package_name, RootPackageSearchScope::Aur);
    package_name.assign("changed");
    const RootPackageSearchSnapshot snapshot =
        require_snapshot(result, "owned AUR duplicate");
    expect(
        snapshot.candidates.size() == 1 &&
            require_aur_identity(snapshot.candidates.front(), "owned AUR") ==
                AurRootPackageIdentity{
                    "owned-child", "owned-base"} &&
            snapshot.candidates.front().candidate.presentation() ==
                RootPackageCandidatePresentation{
                    "1.0-1", "owned description"},
        "AUR owned duplicate metadata differs");
}

RootPackageSearchSnapshot search_fixture(
    const std::string& query,
    RepositoryPackageSearchSnapshot repository,
    std::vector<AurPackageInfo> aur) {
    stub::reset();
    stub::enqueue_repository_result(std::move(repository));
    stub::enqueue_aur_result(std::move(aur));
    auto snapshot = require_snapshot(
        search_root_package_candidates(query, RootPackageSearchScope::All),
        "ranking fixture");
    expect(stub::repository_queries() == std::vector<std::string>{query} &&
               stub::aur_queries() == std::vector<std::string>{query},
           "ranking changed the query or added provider calls");
    return snapshot;
}

void test_match_quality_large_fixture_and_membership_parity() {
    RepositoryPackageSearchSnapshot repository{
        {"testing", "core"},
        {repository_match("core", "chrome", "1"),
         repository_match("testing", "chrome", "2"),
         repository_match("core", "chromedriver", "3"),
         repository_match("core", "a-description", "4", "Chrome browser integration"),
         repository_match("core", "a-native-only", "5")}};
    std::vector<AurPackageInfo> aur{
        aur_package("chrome", "chrome-base", "6"),
        aur_package("google-chrome", "google-chrome", "7"),
        aur_package("achrome", "achrome", "8"),
        aur_package("a-base-child", "chrome", "9"),
        aur_package("b-base-child", "chrome", "10"),
        aur_package("a-base-substring", "xchromey", "11"),
        aur_package("z-description", "z-description", "12", "Chrome browser integration")};
    for(int index = 0; index < 96; ++index) {
        aur.push_back(aur_package("a-description-" + std::to_string(index),
                                  "description-suite", "1", "Chrome integration"));
    }
    // A compatible repeated identity must still merge before ranking.
    aur.push_back(aur_package("google-chrome", "google-chrome", {}, "Browser"));
    const auto canonical = search_fixture("unmatched-query", repository, aur);
    const auto ranked = search_fixture("chrome", repository, aur);
    expect(canonical.candidates.size() == repository.matches.size() + aur.size() - 1 &&
               ranked.candidates.size() == canonical.candidates.size(),
           "ranking changed the deduplicated candidate count");
    expect(std::is_permutation(canonical.candidates.begin(), canonical.candidates.end(),
                               ranked.candidates.begin(), ranked.candidates.end()),
           "ranking changed identities, metadata, or membership");
    expect(ranked.repository_order == canonical.repository_order,
           "ranking changed configured repository authority");
    const std::vector<std::string> leading_names{
        "chrome", "chrome", "chrome", "chromedriver", "google-chrome",
        "achrome", "a-base-child", "a-base-substring", "b-base-child"};
    for(std::size_t index = 0; index < leading_names.size(); ++index) {
        expect(ranked.candidates[index].candidate.package_name() == leading_names[index],
               "semantic match-quality precedence differs at " + std::to_string(index));
    }
    expect(require_repository_identity(ranked.candidates[0], "exact testing").repository_name == "testing" &&
               require_repository_identity(ranked.candidates[1], "exact core").repository_name == "core" &&
               require_aur_identity(ranked.candidates[2], "exact AUR").package_base == "chrome-base",
           "same-quality canonical source tie-break drifted");
    for(std::size_t index = 0; index < 3; ++index) {
        const auto result = parse_root_package_selection(std::to_string(index + 1), ranked);
        const auto* selection = std::get_if<RootPackageSelection>(&result);
        expect(selection != nullptr && selection->targets().front().identity() ==
                                           ranked.candidates[index].candidate.identity(),
               "same-name selection lost the displayed source identity");
    }
    expect(std::holds_alternative<InvalidRootPackageSelection>(
               parse_root_package_selection("1-3", ranked)),
           "ranking bypassed the same-name alternative source guard");
    std::vector<RootPackageSearchCandidate> canonical_other;
    for(const auto& entry : canonical.candidates) {
        const auto& name = entry.candidate.package_name();
        if(name.starts_with("a-description") || name == "a-native-only" || name == "z-description") {
            canonical_other.push_back(entry);
        }
    }
    expect(std::equal(canonical_other.begin(), canonical_other.end(),
                      ranked.candidates.begin() + leading_names.size(), ranked.candidates.end()),
           "description/native-only ties no longer use canonical order");
    expect(canonical.candidates.front().candidate.package_name() == "a-base-child" &&
               canonical.candidates != ranked.candidates,
           "fixture did not demonstrate canonical-to-quality reordering");
    std::reverse(repository.matches.begin(), repository.matches.end());
    std::reverse(aur.begin(), aur.end());
    expect(search_fixture("chrome", repository, aur) == ranked,
           "ranking depends on native result order");
    expect(search_fixture("ChRoMe", repository, aur) == ranked,
           "ASCII case-insensitive identity quality differs");
    const auto child = search_fixture("b-base-child", repository, aur);
    expect(child.candidates.front().candidate.package_name() == "b-base-child" &&
               require_aur_identity(child.candidates.front(), "exact child").package_base == "chrome",
           "exact child was flattened to PackageBase identity");
}

void test_literal_query_and_package_boundaries() {
    RepositoryPackageSearchSnapshot repository{{"core"}, {}};
    std::vector<AurPackageInfo> aur;
    const std::vector<std::string> strong_names{
        "chrome-tool", "x+chrome", "x-chrome", "x.chrome", "x@chrome", "x_chrome",
        "xchrome-chrome"};
    aur.push_back(aur_package("achrome", "achrome"));
    for(const auto& name : strong_names)
        aur.push_back(aur_package(name, name));
    const auto ranked = search_fixture("chrome", repository, aur);
    for(std::size_t index = 0; index < strong_names.size(); ++index) {
        expect(ranked.candidates[index].candidate.package_name() == strong_names[index],
               "prefix/punctuation boundary precedence differs");
    }
    expect(ranked.candidates.back().candidate.package_name() == "achrome",
           "interior substring was treated as a boundary");
    const auto canonical = search_fixture("unmatched-query", repository, aur);
    for(const std::string query : {" chrome ", "chrome tool", "^chrome", "", "CHRÖME"}) {
        expect(search_fixture(query, repository, aur) == canonical,
               "ranking trimmed, tokenized, interpreted regex, or Unicode-folded query");
    }
    const auto mixed_case = search_fixture(
        "chrome", repository,
        {aur_package("chrome", "chrome"), aur_package("Chrome", "Chrome"),
         aur_package("a-child", "CHROME"), aur_package("a-other", "a-other")});
    expect(mixed_case.candidates[0].candidate.package_name() == "Chrome" &&
               mixed_case.candidates[1].candidate.package_name() == "chrome" &&
               mixed_case.candidates[2].candidate.package_name() == "a-child",
           "identity folding changed stored spelling or bytewise tie-break");
    const auto literal = search_fixture("x.chrome", repository, aur);
    expect(literal.candidates.front().candidate.package_name() == "x.chrome",
           "package punctuation was interpreted as regex");
}

void test_ranked_snapshot_selection_retry_cancel_and_route_mapping() {
    const auto snapshot = search_fixture(
        "chrome",
        RepositoryPackageSearchSnapshot{
            {"extra"},
            {repository_match("extra", "a-tool", "1", "Chrome integration",
                              RepositoryPackageSearchMatchKind::ExactGroup, "chrome"),
             repository_match("extra", "chrome", "2")}},
        {aur_package("google-chrome", "browser-suite", "3")});
    expect(snapshot.candidates[0].candidate.package_name() == "chrome" &&
               snapshot.candidates[1].candidate.package_name() == "google-chrome" &&
               snapshot.candidates[2].candidate.package_name() == "a-tool" &&
               snapshot.candidates[2].selectable_group_names == std::vector<std::string>{"chrome"},
           "ranking lost group metadata or expected displayed order");
    std::istringstream input("0\n3 1 2\n");
    int presentations = 0;
    int invalid_attempts = 0;
    RootPackageSelectionSession session(
        input,
        [&](const RootPackageSelectionInteractionEvent& event, const RootPackageSearchSnapshot& displayed) {
            expect(displayed == snapshot, "retry changed the displayed snapshot");
            if(std::holds_alternative<PresentRootPackageSelectionCandidates>(event)) ++presentations;
            if(std::holds_alternative<InvalidRootPackageSelectionAttempt>(event)) ++invalid_attempts;
        },
        RootPackageSelectionInputGate::Interactive);
    const auto result = session.select(snapshot);
    const auto* selection = std::get_if<RootPackageSelection>(&result);
    expect(selection != nullptr && presentations == 1 && invalid_attempts == 1,
           "ranked selection/retry session differs");
    expect(selection->targets().size() == snapshot.candidates.size(), "selected count differs");
    for(std::size_t index = 0; index < snapshot.candidates.size(); ++index) {
        expect(selection->targets()[index].identity() == snapshot.candidates[index].candidate.identity(),
               "display number no longer maps to typed selected identity");
        const auto single_result = parse_root_package_selection(std::to_string(index + 1), snapshot);
        const auto* single = std::get_if<RootPackageSelection>(&single_result);
        expect(single != nullptr && single->targets().size() == 1 &&
                   single->targets().front().identity() == snapshot.candidates[index].candidate.identity(),
               "individual displayed number maps to a stale index");
    }
    const auto routing = project_root_package_routing(*selection);
    expect(routing.is_valid(), "ranked route projection failed");
    const auto& repositories = routing.projection()->repository_targets();
    const auto& aur = routing.projection()->aur_targets();
    expect(repositories.size() == 2 && repositories[0].exact_package_target() == "extra/chrome" &&
               repositories[0].selection_index() == 0 && repositories[1].exact_package_target() == "extra/a-tool" &&
               repositories[1].selection_index() == 2 && aur.size() == 1 && aur[0].selection_index() == 1 &&
               aur[0].identity() == AurRootPackageIdentity{"google-chrome", "browser-suite"},
           "ranked selection projected a different source or execution target");
    const auto group = parse_root_package_selection("@chrome", snapshot);
    expect(std::get<RootPackageSelection>(group).targets().front().package_name() == "a-tool",
           "group selector retained a pre-ranking index");
    for(const std::string text : {"\n", "q\n", "quit\n", "cancel\n", ""}) {
        std::istringstream cancel_input(text);
        RootPackageSelectionSession cancel(cancel_input, {}, RootPackageSelectionInputGate::Interactive);
        expect(std::holds_alternative<CancelledRootPackageSelection>(cancel.select(snapshot)),
               "ranking changed cancellation or EOF semantics");
    }
}

} // namespace

int main() {
    try {
        test_match_quality_large_fixture_and_membership_parity();
        test_literal_query_and_package_boundaries();
        test_ranked_snapshot_selection_retry_cancel_and_route_mapping();
        test_source_scope_queries_only_enabled_adapters();
        test_source_failure_never_publishes_partial_snapshot();
        test_merge_sort_and_source_identity_contract();
        test_duplicate_metadata_and_group_membership_are_unioned();
        test_unsafe_and_inexact_group_selectors_fail_closed();
        test_candidate_validation_and_pair_inconsistency_fail_snapshot();
        test_repository_order_is_validated_before_sorting();
        test_compatible_aur_duplicates_merge_owned_metadata();
        std::cout << "root package search tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "root package search test failed: " << error.what()
                  << '\n';
        return 1;
    }
}
