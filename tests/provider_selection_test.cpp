#include "provider_selection.hpp"
#include "package_text_style.hpp"
#include "provider_installed_state_presentation.hpp"

#include "stubs/package-metadata/alpm_stub.hpp"
#include "stubs/package-metadata/process_stub.hpp"

#include <alpm.h>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace stub = package_metadata_test_stub;

constexpr const char* DATABASE_PATH_COMMAND =
    "pacman-conf --verbose RootDir DBPath 2>/dev/null";

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

ProvidedDependency repository_candidate(
    std::string package_version = "1.2.3-1") {
    return ProvidedDependency::from_repository(
        "extra", "shared-provider", "virtual-dependency",
        "virtual-dependency=1.2", std::move(package_version));
}

ProvidedDependency aur_candidate(
    std::string package_version = "2.4.0-1") {
    return ProvidedDependency::from_aur(
        "shared-provider", "shared-provider-base",
        "virtual-dependency", "virtual-dependency>=2.4",
        std::move(package_version));
}

ProvidedDependency typed_aur_candidate(
    const std::string& capability_specification,
    const std::string& package_version) {
    ProviderCapabilityParseResult parsed =
        parse_provider_capability(capability_specification);
    const ProviderCapability* capability = parsed.capability();
    if(capability == nullptr) {
        throw std::runtime_error(
            "typed provider test capability did not parse");
    }
    ObservedVersion provided_version = capability->version().has_value()
                                           ? ObservedVersion::available(
                                                 ObservedVersionSource::AurProviderCapability,
                                                 capability->version().value())
                                           : ObservedVersion::unknown(
                                                 ObservedVersionSource::AurProviderCapability,
                                                 ObservedVersionUnknownReason::
                                                     UnversionedProviderCapability);
    return ProvidedDependency::from_aur_constraint_metadata(
        "shared-provider",
        "shared-provider-base",
        ProviderConstraintMetadata{
            *capability,
            ObservedVersion::available(
                ObservedVersionSource::AurExactPackage,
                package_version),
            std::move(provided_version)});
}

std::vector<ProvidedDependency> candidates() {
    return {repository_candidate(), aur_candidate()};
}

std::vector<ProvidedDependency> installed_state_candidates() {
    return {
        ProvidedDependency::from_repository(
            "extra", "repository-provider", "virtual-dependency",
            "virtual-dependency=1.2", "1.2.3-1"),
        ProvidedDependency::from_aur(
            "aur-provider", "aur-provider-base", "virtual-dependency",
            "virtual-dependency>=2.4", "2.4.0-1")};
}

ProvidedDependency repository_identity_candidate(
    const std::string& repository,
    const std::string& dependency,
    std::string version = "1.0-1") {
    return ProvidedDependency::from_repository(
        repository, "shared-provider", dependency,
        dependency + "=1", std::move(version));
}

ProvidedDependency aur_identity_candidate(
    const std::string& package_base,
    const std::string& dependency,
    std::string version = "1.0-1") {
    return ProvidedDependency::from_aur(
        "shared-provider", package_base, dependency,
        dependency + "=1", std::move(version));
}

ProvidedDependency decoy_candidate(const std::string& dependency) {
    return ProvidedDependency::from_repository(
        "extra", "decoy-provider", dependency,
        dependency + "=1", "1.0-1");
}

std::size_t occurrence_count(
    std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

void reset_metadata_stubs() {
    stub::reset_alpm_stub();
    stub::reset_process_stub();
}

void enqueue_valid_database_paths() {
    stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{
            "RootDir = /\nDBPath = /var/lib/pacman/\n", 0});
}

ProviderCandidatePresenter make_installed_state_presenter(
    ProviderInstalledStateLookup& lookup) {
    return make_provider_installed_state_candidate_presenter(lookup);
}

void test_presentation_modes_preserve_candidates_and_selection() {
    const auto candidate_set = installed_state_candidates();
    for(const bool unknown_state : {false, true}) {
        for(const std::string& answer : {std::string("1\n"), std::string("0\n2\n"), std::string("q\n"), std::string("\n"), std::string()}) {
            for(const PresentationDetail detail : {PresentationDetail::Normal, PresentationDetail::Detailed}) {
                reset_metadata_stubs();
                if(unknown_state) {
                    stub::enqueue_captured_command_result(
                        DATABASE_PATH_COMMAND, CapturedCommandResult{"", 127});
                } else {
                    enqueue_valid_database_paths();
                    stub::enqueue_local_package_query_present(
                        "repository-provider", "repository-provider", "1.2.3-1", ALPM_PKG_REASON_EXPLICIT);
                    stub::enqueue_local_package_query_absent("aur-provider");
                }
                std::istringstream input(answer);
                std::ostringstream output;
                ProviderSelectionSession session(input, output, true);
                auto presenter = make_provider_installed_state_candidate_presenter_factory()(detail);
                const auto selected = session.select_provider("virtual-dependency", candidate_set, presenter);
                if(answer == "0\n2\n" || answer == "1\n") {
                    expect(selected.has_value() && selected.value() == candidate_set[answer == "1\n" ? 0 : 1],
                           "detail mode changed selected provider identity or metadata");
                } else {
                    expect(!selected.has_value() && session.was_cancelled("virtual-dependency"),
                           "detail mode changed cancellation semantics");
                }
                if(detail == PresentationDetail::Detailed) {
                    expect(output.str().find(
                               "1) source=repository package=repository-provider repository=extra "
                               "provided=virtual-dependency provided-specification=virtual-dependency=1.2 "
                               "version=1.2.3-1") != std::string::npos,
                           "detail mode lost repository provider metadata");
                    expect(output.str().find(
                               "2) source=AUR package=aur-provider PackageBase=aur-provider-base "
                               "provided=virtual-dependency provided-specification=virtual-dependency>=2.4 "
                               "version=2.4.0-1") != std::string::npos,
                           "detail mode lost AUR provider metadata");
                    expect(output.str().find("1) source=repository") < output.str().find("2) source=AUR"),
                           "detail mode reordered provider candidates");
                } else {
                    expect(output.str().find("1) extra/repository-provider 1.2.3-1 [provides: virtual-dependency=1.2]") != std::string::npos,
                           "Normal lost repository identity/version/capability");
                    expect(output.str().find("2) aur/aur-provider 2.4.0-1 (PackageBase: aur-provider-base) [provides: virtual-dependency>=2.4]") != std::string::npos,
                           "Normal lost AUR identity/PackageBase/capability");
                    expect(output.str().find("1) extra/") < output.str().find("2) aur/"), "Normal reordered candidates");
                    expect(output.str().find("source=") == std::string::npos, "Normal leaked raw candidate fields");
                }
                expect(output.str().find('\033') == std::string::npos, "capture contains ANSI");
                expect(occurrence_count(output.str(), "1) ") == 1 && occurrence_count(output.str(), "2) ") == 1,
                       "candidate count/numbering changed on retry");
                expect(output.str().find(unknown_state ? "[installed state unknown]" : "[installed]") != std::string::npos,
                       "detail mode lost installed state annotation");
                if(unknown_state) {
                    expect(occurrence_count(output.str(), "Warning: installed state is unavailable for provider candidates:") == 1,
                           "detail mode lost or repeated installed state diagnostic");
                } else {
                    stub::require_local_package_query_expectations_consumed();
                }
            }
        }
    }
}

void test_compact_identity_and_style() {
    const auto repository = ProvidedDependency::from_repository(
        "aur", "shared", "cargo", "cargo", "1.0-1");
    const auto aur = ProvidedDependency::from_aur(
        "shared", "shared", "cargo", "cargo=2", "2.0-1");
    std::ostringstream output;
    const auto presenter = make_default_provider_candidate_presenter();
    presenter(output, 9, repository);
    presenter(output, 10, aur);
    expect(output.str() ==
               "9) aur/shared 1.0-1 [repository] [provides: cargo]\n"
               "10) aur/shared 2.0-1 [provides: cargo=2]\n",
           "compact source collision, equal PackageBase, or capability projection drift");
    auto split_repository = repository;
    split_repository.package_base = "toolchain";
    split_repository.provided_dependency_specification = "different-component=3";
    std::ostringstream split_output;
    presenter(split_output, 1, split_repository);
    expect(split_output.str() ==
               "1) aur/shared 1.0-1 [repository] (PackageBase: toolchain) [provides: different-component=3] [component: cargo]\n",
           "repository PackageBase or differing provided component was lost");
    std::ostringstream detailed_output;
    make_default_provider_candidate_presenter(PresentationDetail::Detailed)(detailed_output, 1, split_repository);
    expect(detailed_output.str().find("repository=aur PackageBase=toolchain provided=cargo provided-specification=different-component=3") != std::string::npos,
           "Detailed lost repository PackageBase or capability metadata");
    expect(!package_text_style::enabled_for(output), "capture enabled terminal style");
    std::ostringstream styled;
    package_text_style::identity(styled, "aur", "shared", true);
    styled << ' ';
    package_text_style::version(styled, "2.0-1", true);
    styled << ' ';
    package_text_style::installed(styled, "[installed]", true);
    expect(styled.str() == "\033[1;35maur\033[0m/\033[1mshared\033[0m "
                           "\033[1;32m2.0-1\033[0m \033[1;36m[installed]\033[0m",
           "shared search palette changed");
}

void test_noninteractive_session_does_not_read_or_write() {
    std::istringstream input("2\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, false);

    std::optional<ProvidedDependency> selected =
        session.select_provider("virtual-dependency>=1", candidates());

    expect(!selected.has_value(), "non-interactive session selected a provider");
    expect(input.tellg() == std::streampos(0), "non-interactive session read stdin");
    expect(output.str().empty(), "non-interactive session wrote a prompt");
}

void test_candidate_metadata_and_exact_number_selection() {
    std::istringstream input(" 2 \n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);

    std::optional<ProvidedDependency> selected =
        session.select_provider("virtual-dependency", candidates(),
                                make_default_provider_candidate_presenter(PresentationDetail::Detailed));

    expect(selected.has_value(), "numbered provider was not selected");
    expect(
        same_provider_identity(selected.value(), aur_candidate()),
        "numbered selection did not preserve source-aware identity");

    const std::string presentation = output.str();
    expect(
        presentation.find(
            "1) source=repository package=shared-provider repository=extra "
            "provided=virtual-dependency "
            "provided-specification=virtual-dependency=1.2 "
            "version=1.2.3-1") != std::string::npos,
        "repository candidate metadata was not presented");
    expect(
        presentation.find(
            "2) source=AUR package=shared-provider "
            "PackageBase=shared-provider-base "
            "provided=virtual-dependency "
            "provided-specification=virtual-dependency>=2.4 "
            "version=2.4.0-1") != std::string::npos,
        "AUR candidate metadata was not presented");
    expect(
        presentation.find(
            ":: Select a provider from [1-2], or press Enter / enter "
            "q/quit/cancel to cancel: ") != std::string::npos,
        "translated provider prompt sentence was not presented");
}

void test_installed_state_presentation_preserves_order_and_explicit_selection() {
    reset_metadata_stubs();
    enqueue_valid_database_paths();
    stub::enqueue_local_package_query_present(
        "repository-provider", "repository-provider", "1.2.3-1",
        ALPM_PKG_REASON_EXPLICIT);
    stub::enqueue_local_package_query_absent("aur-provider");

    std::istringstream input("2\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    ProviderInstalledStateLookup lookup;

    const std::vector<ProvidedDependency> candidates = installed_state_candidates();
    std::optional<ProvidedDependency> selected = session.select_provider(
        "virtual-dependency", candidates,
        make_installed_state_presenter(lookup));

    expect(selected.has_value(), "installed-state presentation did not accept explicit input");
    expect(
        same_provider_identity(selected.value(), candidates[1]),
        "installed state changed the selected provider identity");
    const std::string presentation = output.str();
    expect(
        presentation.find(
            "1) extra/repository-provider 1.2.3-1 [provides: virtual-dependency=1.2] [installed]") != std::string::npos,
        "installed repository provider was not annotated");
    expect(
        presentation.find(
            "2) aur/aur-provider 2.4.0-1 (PackageBase: aur-provider-base) [provides: virtual-dependency>=2.4]\n") != std::string::npos,
        "not-installed provider did not preserve its metadata line");
    expect(
        presentation.find("1) extra/") <
            presentation.find("2) aur/"),
        "installed state changed candidate order or numbering");
    expect(stub::package_query_call_count() == 2, "candidate states were not queried once each");
    stub::require_local_package_query_expectations_consumed();
}

void test_multiple_installed_candidates_remain_explicit_choices() {
    reset_metadata_stubs();
    enqueue_valid_database_paths();
    stub::enqueue_local_package_query_present(
        "repository-provider", "repository-provider", "1.2.3-1",
        ALPM_PKG_REASON_EXPLICIT);
    stub::enqueue_local_package_query_present(
        "aur-provider", "aur-provider", "2.4.0-1",
        ALPM_PKG_REASON_EXPLICIT);

    std::istringstream input("2\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    ProviderInstalledStateLookup lookup;
    const std::vector<ProvidedDependency> candidates = installed_state_candidates();

    std::optional<ProvidedDependency> selected = session.select_provider(
        "virtual-dependency", candidates,
        make_installed_state_presenter(lookup));

    expect(selected.has_value(), "multiple installed candidates were not selectable");
    expect(
        same_provider_identity(selected.value(), candidates[1]),
        "multiple installed candidates were auto-selected or reordered");
    expect(
        occurrence_count(output.str(), "[installed]") == 2,
        "multiple installed candidates did not receive independent annotations");
    stub::require_local_package_query_expectations_consumed();
}

void test_unknown_installed_state_stays_selectable_and_reports_once_per_package() {
    reset_metadata_stubs();
    enqueue_valid_database_paths();
    stub::enqueue_local_package_query_failure("repository-provider");
    stub::enqueue_local_package_query_absent("aur-provider");

    std::istringstream input("1\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    ProviderInstalledStateLookup lookup;
    const std::vector<ProvidedDependency> candidates = installed_state_candidates();

    std::optional<ProvidedDependency> selected = session.select_provider(
        "virtual-dependency", candidates,
        make_installed_state_presenter(lookup));

    expect(selected.has_value(), "unknown installed state rejected a valid choice");
    expect(
        same_provider_identity(selected.value(), candidates[0]),
        "unknown installed state changed explicit selection");
    expect(
        output.str().find("[installed state unknown]") != std::string::npos,
        "unknown installed state was indistinguishable from not installed");
    expect(
        occurrence_count(
            output.str(),
            "Warning: installed state is unavailable for provider candidate "
            "repository-provider:") == 1,
        "package-specific installed-state warning was not emitted exactly once");
    stub::require_local_package_query_expectations_consumed();
}

void test_session_installed_state_failure_reports_once_for_all_candidates() {
    reset_metadata_stubs();
    stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND, CapturedCommandResult{"", 127});

    std::istringstream input("1\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    ProviderInstalledStateLookup lookup;

    std::optional<ProvidedDependency> selected = session.select_provider(
        "virtual-dependency", installed_state_candidates(),
        make_installed_state_presenter(lookup));

    expect(selected.has_value(), "session-level installed-state failure rejected a choice");
    expect(
        occurrence_count(output.str(), "[installed state unknown]") == 2,
        "session-level failure did not annotate every unknown candidate");
    expect(
        occurrence_count(
            output.str(),
            "Warning: installed state is unavailable for provider candidates:") == 1,
        "session-level installed-state warning repeated for candidates");
    expect(stub::capture_command_call_count() == 1, "session failure retried metadata initialization");
}

void test_lookup_is_not_started_for_noninteractive_small_reuse_or_cancelled_paths() {
    reset_metadata_stubs();
    ProviderInstalledStateLookup lookup;
    ProviderCandidatePresenter presenter = make_installed_state_presenter(lookup);

    {
        std::istringstream input("2\n");
        std::ostringstream output;
        ProviderSelectionSession session(input, output, false);
        static_cast<void>(session.select_provider(
            "virtual-dependency", installed_state_candidates(), presenter));
    }
    {
        std::istringstream input;
        std::ostringstream output;
        ProviderSelectionSession session(input, output, true);
        const std::vector<ProvidedDependency> single_candidate{
            installed_state_candidates().front()};
        static_cast<void>(session.select_provider(
            "virtual-dependency", single_candidate, presenter));
    }
    expect(stub::capture_command_call_count() == 0, "noninteractive or single candidate started lookup");
    expect(stub::initialize_call_count() == 0, "noninteractive or single candidate initialized libalpm");
    expect(stub::package_query_call_count() == 0, "noninteractive or single candidate queried local DB");

    reset_metadata_stubs();
    enqueue_valid_database_paths();
    stub::enqueue_local_package_query_absent("repository-provider");
    stub::enqueue_local_package_query_absent("aur-provider");
    ProviderInstalledStateLookup interactive_lookup;
    ProviderCandidatePresenter interactive_presenter =
        make_installed_state_presenter(interactive_lookup);
    std::istringstream input("q\n2\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    const std::vector<ProvidedDependency> candidates = installed_state_candidates();
    static_cast<void>(session.select_provider(
        "virtual-dependency", candidates, interactive_presenter));
    const std::size_t queries_after_cancel = stub::package_query_call_count();
    static_cast<void>(session.select_provider(
        "virtual-dependency>=1", candidates, interactive_presenter));
    expect(
        stub::package_query_call_count() == queries_after_cancel,
        "cancelled dependency reuse started an installed-state lookup");
    stub::require_local_package_query_expectations_consumed();

    reset_metadata_stubs();
    enqueue_valid_database_paths();
    stub::enqueue_local_package_query_absent("repository-provider");
    stub::enqueue_local_package_query_absent("aur-provider");
    ProviderInstalledStateLookup reuse_lookup;
    ProviderCandidatePresenter reuse_presenter =
        make_installed_state_presenter(reuse_lookup);
    std::istringstream reuse_input("1\n2\n");
    std::ostringstream reuse_output;
    ProviderSelectionSession reuse_session(reuse_input, reuse_output, true);
    static_cast<void>(reuse_session.select_provider(
        "virtual-dependency", candidates, reuse_presenter));
    const std::size_t queries_after_choice = stub::package_query_call_count();
    static_cast<void>(reuse_session.select_provider(
        "virtual-dependency<9", candidates, reuse_presenter));
    expect(
        stub::package_query_call_count() == queries_after_choice,
        "existing provider choice reuse started an installed-state lookup");
    stub::require_local_package_query_expectations_consumed();
}

void test_invalid_and_out_of_range_input_retries() {
    std::istringstream input("not-a-number\n0\n3\n1\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);

    std::optional<ProvidedDependency> selected =
        session.select_provider("virtual-dependency", candidates());

    expect(selected.has_value(), "valid choice after retries was not selected");
    expect(
        same_provider_identity(selected.value(), repository_candidate()),
        "retry path selected the wrong candidate");
    expect(
        occurrence_count(
            output.str(),
            ":: Invalid choice. Enter a number from [1-2], or press "
            "Enter / enter q/quit/cancel to cancel.") == 3,
        "invalid and out-of-range input did not retry exactly three times");
}

void test_cancel_inputs_return_no_selection() {
    const std::vector<std::string> cancel_inputs{
        "\n", "q\n", "QUIT\n", " cancel \n", ""};
    for(const std::string& cancel_input : cancel_inputs) {
        std::istringstream input(cancel_input);
        std::ostringstream output;
        ProviderSelectionSession session(input, output, true);

        std::optional<ProvidedDependency> selected =
            session.select_provider("virtual-dependency", candidates());
        expect(
            !selected.has_value(),
            "cancel or EOF unexpectedly selected a provider");
        expect(
            output.str().find(":: Invalid choice.") == std::string::npos,
            "cancel or EOF was treated as invalid input");
    }
}

void test_cancelled_dependency_does_not_prompt_or_read_again() {
    std::istringstream input("\n2\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);

    std::optional<ProvidedDependency> first =
        session.select_provider("virtual-dependency>=1", candidates());
    expect(!first.has_value(), "cancel unexpectedly selected a provider");
    expect(
        session.was_cancelled(" virtual-dependency<9 "),
        "constraint-bearing cancellation lookup lost canonical identity");

    const std::string output_after_cancel = output.str();
    std::optional<ProvidedDependency> second =
        session.select_provider("virtual-dependency<9", candidates());
    expect(
        !second.has_value(),
        "cancelled canonical dependency selected on a later request");
    expect(
        output.str() == output_after_cancel,
        "cancelled canonical dependency prompted again");

    std::string unread_input;
    expect(
        static_cast<bool>(std::getline(input, unread_input)) &&
            unread_input == "2",
        "cancelled canonical dependency consumed later input");
}

void test_eof_dependency_does_not_prompt_again() {
    std::istringstream input;
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);

    std::optional<ProvidedDependency> first =
        session.select_provider("virtual-dependency", candidates());
    expect(!first.has_value(), "EOF unexpectedly selected a provider");

    const std::string output_after_eof = output.str();
    std::optional<ProvidedDependency> second =
        session.select_provider("virtual-dependency>=2", candidates());
    expect(
        !second.has_value(),
        "EOF-cancelled canonical dependency selected on a later request");
    expect(
        output.str() == output_after_eof,
        "EOF-cancelled canonical dependency prompted again");
}

void test_canonical_dependency_reuses_source_aware_choice() {
    std::istringstream input("2\n1\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);

    std::optional<ProvidedDependency> first =
        session.select_provider(" virtual-dependency>=2 ", candidates());
    expect(first.has_value(), "initial provider selection failed");

    std::vector<ProvidedDependency> current_candidates{
        repository_candidate("1.2.4-1"), aur_candidate("2.5.0-1")};
    const std::string presentation_before_reuse = output.str();
    std::optional<ProvidedDependency> reused = session.select_provider(
        "virtual-dependency<9", current_candidates);

    expect(reused.has_value(), "canonical dependency choice was not reused");
    expect(
        reused.value() == current_candidates[1],
        "reuse did not return authoritative current candidate metadata");
    expect(
        output.str() == presentation_before_reuse,
        "reused provider choice prompted a second time");

    std::string unread_input;
    expect(
        static_cast<bool>(std::getline(input, unread_input)) &&
            unread_input == "1",
        "reused provider choice consumed another input line");
}

void test_choice_reuse_returns_current_typed_capability_without_reordering() {
    std::istringstream input("2\n1\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    const std::vector<ProvidedDependency> initial_candidates{
        repository_candidate(),
        typed_aur_candidate("virtual-dependency=3", "8.0-1")};

    const std::optional<ProvidedDependency> selected =
        session.select_provider(
            "virtual-dependency>=2", initial_candidates);
    expect(
        selected.has_value() &&
            selected->constraint_metadata.has_value() &&
            selected->constraint_metadata->provided_capability
                    .raw_specification() ==
                "virtual-dependency=3",
        "Initial typed AUR capability was not selected explicitly");

    const std::vector<ProvidedDependency> current_candidates{
        repository_candidate("1.3.0-1"),
        typed_aur_candidate("virtual-dependency=1", "9.0-1")};
    const std::string output_before_reuse = output.str();
    const std::optional<ProvidedDependency> refreshed =
        session.select_provider(
            "virtual-dependency<9", current_candidates);
    expect(
        refreshed.has_value() &&
            refreshed.value() == current_candidates[1] &&
            refreshed->constraint_metadata.has_value() &&
            refreshed->constraint_metadata->provided_capability
                    .raw_specification() ==
                "virtual-dependency=1",
        "Choice reuse did not return the current typed capability");
    expect(
        output.str() == output_before_reuse,
        "Typed capability refresh prompted or changed candidate policy");

    std::string unread_input;
    expect(
        static_cast<bool>(std::getline(input, unread_input)) &&
            unread_input == "1",
        "Typed capability refresh consumed a new selection");
}

void test_missing_previous_identity_throws_typed_conflict() {
    std::istringstream input("2\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    static_cast<void>(
        session.select_provider("virtual-dependency", candidates()));

    bool caught = false;
    try {
        static_cast<void>(session.select_provider(
            "virtual-dependency>=3",
            std::vector<ProvidedDependency>{repository_candidate()}));
    } catch(const ProviderSelectionConflict& error) {
        caught = true;
        expect(
            error.dependency_name() == "virtual-dependency",
            "typed conflict did not expose the canonical dependency name");
    }
    expect(caught, "missing previous provider identity did not throw conflict");
}

void expect_cross_dependency_identity_conflict(
    const ProvidedDependency& first,
    const ProvidedDependency& second,
    const std::string& expected_diagnostic) {
    std::istringstream input("1\n1\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    static_cast<void>(session.select_provider(
        "first-virtual", {first, decoy_candidate("first-virtual")}));

    bool caught = false;
    try {
        static_cast<void>(session.select_provider(
            "second-virtual",
            {second, decoy_candidate("second-virtual")}));
    } catch(const std::runtime_error& error) {
        caught = true;
        expect(
            error.what() == expected_diagnostic,
            "Cross-dependency provider identity diagnostic differs");
    }
    expect(caught, "Cross-dependency provider identity conflict was accepted");
}

void test_cross_dependency_package_identity_conflicts() {
    expect_cross_dependency_identity_conflict(
        repository_identity_candidate("extra", "first-virtual"),
        repository_identity_candidate("core", "second-virtual"),
        "Selected providers use incompatible identities for package "
        "shared-provider: extra/shared-provider and core/shared-provider.");
    expect_cross_dependency_identity_conflict(
        repository_identity_candidate("extra", "first-virtual"),
        aur_identity_candidate(
            "shared-provider-base", "second-virtual"),
        "Selected providers use incompatible identities for package "
        "shared-provider: extra/shared-provider and aur/shared-provider "
        "(PackageBase: shared-provider-base).");
    expect_cross_dependency_identity_conflict(
        aur_identity_candidate("first-provider-base", "first-virtual"),
        aur_identity_candidate("second-provider-base", "second-virtual"),
        "Selected providers use incompatible identities for package "
        "shared-provider: aur/shared-provider (PackageBase: "
        "first-provider-base) and aur/shared-provider (PackageBase: "
        "second-provider-base).");
}

void test_cross_dependency_same_identity_is_allowed() {
    std::istringstream input("1\n1\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    const ProvidedDependency first =
        repository_identity_candidate("extra", "first-virtual");
    const ProvidedDependency second = repository_identity_candidate(
        "extra", "second-virtual", "2.0-1");
    static_cast<void>(session.select_provider(
        "first-virtual", {first, decoy_candidate("first-virtual")}));
    const std::optional<ProvidedDependency> selected =
        session.select_provider(
            "second-virtual",
            {second, decoy_candidate("second-virtual")});
    expect(
        selected.has_value() && selected.value() == second,
        "Same provider identity was rejected across dependency aliases");
}

void test_no_confirm_production_session_is_noninteractive() {
    std::shared_ptr<ProviderSelectionSession> session =
        make_provider_selection_session(true);
    expect(session != nullptr, "production session factory returned null");
    expect(
        !session->is_interactive(),
        "--noconfirm production session remained interactive");
}

} // namespace

int main() {
    try {
        test_compact_identity_and_style();
        test_presentation_modes_preserve_candidates_and_selection();
        test_noninteractive_session_does_not_read_or_write();
        test_candidate_metadata_and_exact_number_selection();
        test_installed_state_presentation_preserves_order_and_explicit_selection();
        test_multiple_installed_candidates_remain_explicit_choices();
        test_unknown_installed_state_stays_selectable_and_reports_once_per_package();
        test_session_installed_state_failure_reports_once_for_all_candidates();
        test_lookup_is_not_started_for_noninteractive_small_reuse_or_cancelled_paths();
        test_invalid_and_out_of_range_input_retries();
        test_cancel_inputs_return_no_selection();
        test_cancelled_dependency_does_not_prompt_or_read_again();
        test_eof_dependency_does_not_prompt_again();
        test_canonical_dependency_reuses_source_aware_choice();
        test_choice_reuse_returns_current_typed_capability_without_reordering();
        test_missing_previous_identity_throws_typed_conflict();
        test_cross_dependency_package_identity_conflicts();
        test_cross_dependency_same_identity_is_allowed();
        test_no_confirm_production_session_is_noninteractive();
        std::cout << "provider selection tests passed" << std::endl;
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
