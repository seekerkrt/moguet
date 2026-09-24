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
#include <utility>
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

ProviderCapability metadata_capability(
    const std::string& name, std::optional<std::string> version) {
    const ProviderCapabilityParseResult parsed =
        make_provider_capability_from_metadata(name, std::move(version));
    if(parsed.capability() == nullptr) {
        throw std::runtime_error("typed repository capability did not parse");
    }
    return *parsed.capability();
}

ProvidedDependency typed_repository_candidate(
    const std::string& repository, const std::string& package,
    const ProviderCapability& capability) {
    return ProvidedDependency::from_repository_constraint_metadata(
        repository, package, package, "x86_64",
        ProviderConstraintMetadata{
            capability,
            ObservedVersion::available(
                ObservedVersionSource::RepositoryExactPackage, "1.0-1"),
            ObservedVersion::from_provider_capability(
                ObservedVersionSource::RepositoryProviderCapability,
                capability)});
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

ProvidedDependency named_repository_candidate(
    const std::string& package, const std::string& dependency,
    std::string version = "1.0-1") {
    return ProvidedDependency::from_repository(
        "extra", package, dependency, dependency + "=1", std::move(version));
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

void test_legacy_soname_class_projection() {
    const auto reported_class = [](const std::string& name,
                                   std::optional<std::string> version) {
        return legacy_soname_v1_class(
            metadata_capability(name, std::move(version)));
    };
    expect(reported_class("libjack.so", "0-64") ==
               LegacySonameV1Class::Class64,
           "legacy explicit 64-bit SONAME class was lost");
    expect(reported_class("libasound.so", "2-32") ==
               LegacySonameV1Class::Class32,
           "legacy explicit 32-bit SONAME class was lost");
    expect(reported_class("libexample.so", "1.2-64") ==
               LegacySonameV1Class::Class64,
           "dot-separated numeric interface version was lost");
    expect(reported_class("libexample.so", "libexample.so-64") ==
                   LegacySonameV1Class::Class64 &&
               reported_class("libexample.so", "libexample.so-32") ==
                   LegacySonameV1Class::Class32,
           "legacy unversioned SONAME form was lost");

    expect(!reported_class("foo", "1.2-64").has_value() &&
               !reported_class("foo", "1.2-32").has_value() &&
               !reported_class("libfoo.so", std::nullopt).has_value() &&
               !reported_class("libfoo.so", "1..2-64").has_value() &&
               !reported_class("libfoo.so", "abc-64").has_value() &&
               !reported_class("libfoo.so", "1-128").has_value() &&
               !reported_class("libfoo.so", "libbar.so-64").has_value(),
           "ordinary, bare, ambiguous, or unsupported provide received a SONAME class");
    expect(!legacy_soname_v1_class(
                ProviderCapability("lib:libfoo.so.1", "lib:libfoo.so.1",
                                   std::nullopt))
                   .has_value() &&
               !legacy_soname_v1_class(
                    ProviderCapability("libfoo.so>=1-64", "libfoo.so",
                                       "1-64"))
                    .has_value(),
           "SONAME v2 or non-equality text received a legacy class");
}

void test_soname_annotations_preserve_selection_and_installed_state() {
    const std::vector<ProvidedDependency> offered{
        typed_repository_candidate(
            "extra", "jack2", metadata_capability("libjack.so", "0-64")),
        typed_repository_candidate(
            "multilib", "lib32-jack2",
            metadata_capability("libjack.so", "0-32")),
        typed_repository_candidate(
            "multilib", "lib32-decoy",
            metadata_capability("libjack.so", std::nullopt))};

    for(const PresentationDetail detail :
        {PresentationDetail::Normal, PresentationDetail::Detailed}) {
        reset_metadata_stubs();
        enqueue_valid_database_paths();
        stub::enqueue_local_package_query_present(
            "jack2", "jack2", "1.0-1", ALPM_PKG_REASON_EXPLICIT);
        stub::enqueue_local_package_query_absent("lib32-jack2");
        stub::enqueue_local_package_query_absent("lib32-decoy");
        std::istringstream input("1,2\n");
        std::ostringstream output;
        ProviderSelectionSession session(input, output, true);
        const auto selected = session.select_provider_set(
            "libjack.so", offered,
            make_provider_installed_state_candidate_presenter_factory()(detail));
        expect(selected.has_value() &&
                   selected->members() ==
                       std::vector<ProvidedDependency>{offered[0], offered[1]},
               "SONAME annotation changed the selected set or canonical order");
        const std::string lines = output.str();
        if(detail == PresentationDetail::Normal) {
            expect(lines.find(
                       "1) extra/jack2 1.0-1 [SONAME: 64-bit] "
                       "[provides: libjack.so=0-64] [installed]\n") !=
                           std::string::npos &&
                       lines.find(
                           "2) multilib/lib32-jack2 1.0-1 [SONAME: 32-bit] "
                           "[provides: libjack.so=0-32]\n") !=
                           std::string::npos &&
                       lines.find(
                           "3) multilib/lib32-decoy 1.0-1 "
                           "[provides: libjack.so]\n") !=
                           std::string::npos,
                   "Normal SONAME, capability, or installed annotation drifted");
        } else {
            expect(lines.find(
                       "1) source=repository package=jack2 repository=extra "
                       "provided=libjack.so "
                       "provided-specification=libjack.so=0-64 "
                       "version=1.0-1 soname-class=64-bit [installed]\n") !=
                           std::string::npos &&
                       lines.find(
                           "2) source=repository package=lib32-jack2 "
                           "repository=multilib provided=libjack.so "
                           "provided-specification=libjack.so=0-32 "
                           "version=1.0-1 soname-class=32-bit\n") !=
                           std::string::npos &&
                       lines.find(
                           "3) source=repository package=lib32-decoy "
                           "repository=multilib provided=libjack.so "
                           "provided-specification=libjack.so version=1.0-1\n") !=
                           std::string::npos,
                   "Detailed SONAME or existing metadata drifted");
        }
        expect(lines.find("1) ") < lines.find("2) ") &&
                   lines.find("2) ") < lines.find("3) ") &&
                   occurrence_count(lines, "SONAME: ") ==
                       (detail == PresentationDetail::Normal ? 2U : 0U) &&
                   occurrence_count(lines, "soname-class=") ==
                       (detail == PresentationDetail::Detailed ? 2U : 0U),
               "unknown candidate acquired a class or numbering changed");
        stub::require_local_package_query_expectations_consumed();
    }
}

void test_noninteractive_session_does_not_read_or_write() {
    std::istringstream input("2\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, false);

    std::optional<ProvidedDependency> selected =
        session.select_provider("virtual-dependency>=1", candidates());

    expect(!selected.has_value(), "non-interactive session selected a provider");
    const auto selected_set = session.select_provider_set(
        "virtual-dependency>=1", candidates(), make_default_provider_candidate_presenter());
    expect(!selected_set.has_value(), "non-interactive set path selected a provider");
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
            ":: Select providers from [1-2] (1 2, 1,2, 1-2; exclude ^2):") != std::string::npos,
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
        occurrence_count(output.str(), ":: Invalid provider selection token.") == 1 &&
            occurrence_count(output.str(),
                             ":: Provider selection index is out of range.") == 2,
        "invalid and out-of-range input did not retry");
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

void test_selection_set_retains_multiple_members_in_candidate_order() {
    const std::vector<ProvidedDependency> offered{
        named_repository_candidate("a", "virtual"),
        named_repository_candidate("b", "virtual"),
        named_repository_candidate("c", "virtual"),
        named_repository_candidate("d", "virtual")};
    const ProviderSelectionSet selected =
        ProviderSelectionSet::from_candidate_indices(offered, {4, 2});
    expect(selected.members() == std::vector<ProvidedDependency>{offered[1], offered[3]},
           "Multiple provider selection was lost or reordered by input order");
}

void test_public_selection_expression_for_each_source() {
    const std::vector<std::pair<std::string, std::vector<std::size_t>>> cases{
        {"1", {1}}, {"2", {2}}, {"1 3", {1, 3}}, {"1,3", {1, 3}}, {"1-3", {1, 2, 3}}, {"1-2,4", {1, 2, 4}}, {"1-3 5", {1, 2, 3, 5}}, {"1-3,5", {1, 2, 3, 5}}, {"1 3-5", {1, 3, 4, 5}}, {"1 2,4-6", {1, 2, 4, 5, 6}}, {"^4", {1, 2, 3, 5, 6}}, {"^2-4", {1, 5, 6}}, {"1-5,^3", {1, 2, 4, 5}}, {"1-3 5 ^2", {1, 3, 5}}, {"^2 1-3", {1, 3}}, {"1-3 ^2", {1, 3}}, {"1,1,2", {1, 2}}, {"3,1,2", {1, 2, 3}}};

    for(const bool is_aur : {false, true}) {
        std::vector<ProvidedDependency> offered;
        for(std::size_t index = 1; index <= 6; ++index) {
            const std::string name = "provider-" + std::to_string(index);
            offered.push_back(is_aur
                                  ? ProvidedDependency::from_aur(
                                        name, name, "virtual", "virtual=1", "1.0-1")
                                  : ProvidedDependency::from_repository(
                                        "extra", name, "virtual", "virtual=1", "1.0-1"));
        }
        for(const auto& [expression, indices] : cases) {
            std::istringstream input(expression + "\n");
            std::ostringstream output;
            ProviderSelectionSession session(input, output, true);
            const auto selected = session.select_provider_set(
                "virtual", offered, make_default_provider_candidate_presenter());
            expect(selected.has_value(), "valid public expression was rejected: " + expression);
            std::vector<ProvidedDependency> expected;
            for(const std::size_t index : indices)
                expected.push_back(offered[index - 1]);
            expect(selected->members() == expected,
                   "public expression changed candidate order or source: " + expression);
            expect(occurrence_count(output.str(), ":: Select providers from [1-6]") == 1,
                   "valid expression prompted more than once: " + expression);
        }
    }
}

void test_public_multiple_presentation_modes_preserve_selection() {
    const std::vector<ProvidedDependency> offered{
        named_repository_candidate("a", "virtual"),
        named_repository_candidate("b", "virtual"),
        named_repository_candidate("c", "virtual")};
    for(const PresentationDetail detail : {PresentationDetail::Normal, PresentationDetail::Detailed}) {
        std::istringstream input("3,1\n");
        std::ostringstream output;
        ProviderSelectionSession session(input, output, true);
        const auto selected = session.select_provider_set(
            "virtual", offered, make_default_provider_candidate_presenter(detail));
        expect(selected.has_value() &&
                   selected->members() == std::vector<ProvidedDependency>{offered[0], offered[2]},
               "detail mode changed public multiple selection");
        const std::string first = detail == PresentationDetail::Detailed
                                      ? "1) source=repository package=a"
                                      : "1) extra/a";
        const std::string second = detail == PresentationDetail::Detailed
                                       ? "2) source=repository package=b"
                                       : "2) extra/b";
        const std::string third = detail == PresentationDetail::Detailed
                                      ? "3) source=repository package=c"
                                      : "3) extra/c";
        expect(output.str().find(first) < output.str().find(second) &&
                   output.str().find(second) < output.str().find(third),
               "detail mode changed candidate presentation order");
        expect(occurrence_count(output.str(), ":: Select providers from [1-3]") == 1,
               "detail mode changed prompt or retry behavior");
    }
}

void test_public_invalid_expression_retries_atomically() {
    const std::vector<std::string> invalid{
        "0", "7", "3-1", "^4-2", "1-", "-3", "^", "^^3",
        "^ 3", "foo", "1,,3", ",1", "1,", "1 - 3",
        "1 3 foo", "1,3,99", "^1-6", "1,^1"};
    std::vector<ProvidedDependency> offered;
    for(std::size_t index = 1; index <= 6; ++index) {
        offered.push_back(named_repository_candidate(
            "provider-" + std::to_string(index), "virtual"));
    }
    for(const std::string& expression : invalid) {
        std::istringstream input(expression + "\n2\n");
        std::ostringstream output;
        ProviderSelectionSession session(input, output, true);
        const auto selected = session.select_provider_set(
            "virtual", offered, make_default_provider_candidate_presenter());
        expect(selected.has_value() &&
                   selected->members() == std::vector<ProvidedDependency>{offered[1]},
               "invalid line was partially accepted or failed to retry: " + expression);
        expect(occurrence_count(output.str(), ":: Select providers from [1-6]") == 2,
               "invalid line did not retry exactly once: " + expression);
        if(expression == "^1-6" || expression == "1,^1") {
            expect(output.str().find("Provider selection is empty after exclusions.") !=
                       std::string::npos,
                   "empty result was not a typed invalid selection: " + expression);
        }
    }
}

void test_public_multiple_reuse_refresh_and_cancel() {
    const std::vector<ProvidedDependency> initial{
        named_repository_candidate("a", "virtual"),
        named_repository_candidate("b", "virtual"),
        named_repository_candidate("c", "virtual")};
    std::istringstream input("1 3\n2\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    const auto first = session.select_provider_set(
        "virtual>=1", initial, make_default_provider_candidate_presenter());
    expect(first.has_value() &&
               first->members() == std::vector<ProvidedDependency>{initial[0], initial[2]},
           "public multiple selection lost a member");
    const std::vector<ProvidedDependency> refreshed{
        named_repository_candidate("c", "virtual", "3.0-1"),
        named_repository_candidate("b", "virtual", "2.0-1"),
        named_repository_candidate("a", "virtual", "4.0-1")};
    const auto second = session.select_provider_set(
        "virtual<9", refreshed, make_default_provider_candidate_presenter());
    expect(second.has_value() &&
               second->members() == std::vector<ProvidedDependency>{refreshed[0], refreshed[2]},
           "public cached set lost current order or metadata");
    expect(occurrence_count(output.str(), ":: Choose a provider for virtual:") == 1,
           "cached public set prompted again");
    std::string unread;
    expect(static_cast<bool>(std::getline(input, unread)) && unread == "2",
           "cached public set consumed input");
    bool conflict = false;
    try {
        static_cast<void>(session.select_provider_set(
            "virtual", {refreshed[2], refreshed[1]},
            make_default_provider_candidate_presenter()));
    } catch(const ProviderSelectionConflict&) {
        conflict = true;
    }
    expect(conflict, "public cached set shrank after member disappearance");

    for(const std::string& cancel : {std::string("\n"), std::string("q\n"),
                                     std::string("QUIT\n"), std::string("cancel\n"),
                                     std::string()}) {
        std::istringstream cancel_input(cancel);
        std::ostringstream cancel_output;
        ProviderSelectionSession cancel_session(cancel_input, cancel_output, true);
        const auto selected = cancel_session.select_provider_set(
            "virtual", initial, make_default_provider_candidate_presenter());
        expect(!selected.has_value() && cancel_session.was_cancelled("virtual"),
               "public cancel became an empty set or a choice");
        expect(!cancel_session.select_provider_set(
                                  "virtual", initial, make_default_provider_candidate_presenter())
                    .has_value(),
               "cancelled public dependency was read again");
        expect(occurrence_count(cancel_output.str(), ":: Choose a provider for virtual:") == 1,
               "cancelled public dependency prompted again");
    }
}

void test_selection_set_deduplicates_identity_and_rejects_empty() {
    const ProvidedDependency first = named_repository_candidate("a", "virtual");
    const ProvidedDependency duplicate = named_repository_candidate(
        "a", "virtual", "2.0-1");
    ProviderSelectionSet selected =
        ProviderSelectionSet::from_candidate_indices(
            {first, duplicate}, {2, 1, 2});
    expect(selected.members() == std::vector<ProvidedDependency>{first},
           "Duplicate provider identity was preserved or preferred stale candidate order");

    bool empty_rejected = false;
    try {
        static_cast<void>(ProviderSelectionSet::from_candidate_indices({first}, {}));
    } catch(const std::invalid_argument&) {
        empty_rejected = true;
    }
    expect(empty_rejected, "Empty provider set was accepted");

    ProviderSelectionSet copied_from_rvalue = std::move(selected);
    expect(!selected.members().empty() && !copied_from_rvalue.members().empty(),
           "Moving a provider set left a valid empty selection");
}

void test_selection_set_reuse_refreshes_all_members_and_fails_on_partial_loss() {
    std::istringstream input("1\n");
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    const std::vector<ProvidedDependency> initial{
        named_repository_candidate("a", "virtual"),
        named_repository_candidate("b", "virtual"),
        named_repository_candidate("c", "virtual")};
    const ProviderSelectionSet selected = session.record_provider_selection(
        "virtual>=1", initial, {3, 1});
    expect(selected.members() == std::vector<ProvidedDependency>{initial[0], initial[2]},
           "Selection cache did not retain all explicit members");

    const std::vector<ProvidedDependency> refreshed{
        named_repository_candidate("c", "virtual", "3.0-1"),
        named_repository_candidate("b", "virtual", "2.0-1"),
        named_repository_candidate("a", "virtual", "4.0-1")};
    const auto reused = session.reuse_provider_selection("virtual<9", refreshed);
    expect(reused.has_value() &&
               reused->members() == std::vector<ProvidedDependency>{refreshed[0], refreshed[2]},
           "Cached set did not refresh all metadata in current candidate order");
    const auto callback_selection = session.select_provider_set(
        "virtual<9", refreshed, make_default_provider_candidate_presenter());
    expect(callback_selection.has_value() &&
               callback_selection->members() == reused->members(),
           "Production adapter flattened a cached provider set");
    expect(output.str().empty(), "Selection-set reuse unexpectedly presented candidates");
    std::string unread;
    expect(static_cast<bool>(std::getline(input, unread)) && unread == "1",
           "Selection-set reuse read input");

    bool conflict = false;
    try {
        static_cast<void>(session.reuse_provider_selection(
            "virtual", {refreshed[2], refreshed[1]}));
    } catch(const ProviderSelectionConflict& error) {
        conflict = error.dependency_name() == "virtual";
    }
    expect(conflict, "Partial disappearance silently shrank the cached set");
}

void test_selection_set_rejects_within_and_cross_dependency_identity_conflicts() {
    const ProvidedDependency extra =
        repository_identity_candidate("extra", "first-virtual");
    const ProvidedDependency core =
        repository_identity_candidate("core", "first-virtual");
    bool within_conflict = false;
    try {
        static_cast<void>(ProviderSelectionSet::from_candidate_indices(
            {extra, core}, {1, 2}));
    } catch(const std::runtime_error&) {
        within_conflict = true;
    }
    expect(within_conflict, "Incompatible identity within one set was accepted");

    std::istringstream input;
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    const ProvidedDependency unrelated =
        named_repository_candidate("unrelated", "first-virtual");
    static_cast<void>(session.record_provider_selection(
        "first-virtual", {unrelated, extra}, {1, 2}));
    const ProvidedDependency conflicting =
        repository_identity_candidate("core", "second-virtual");
    bool cross_conflict = false;
    try {
        static_cast<void>(session.record_provider_selection(
            "second-virtual",
            {named_repository_candidate("other", "second-virtual"), conflicting},
            {1, 2}));
    } catch(const std::runtime_error&) {
        cross_conflict = true;
    }
    expect(cross_conflict, "Cross-dependency guard skipped a set member");

    const ProvidedDependency same =
        repository_identity_candidate("extra", "second-virtual", "8.0-1");
    const ProviderSelectionSet allowed = session.record_provider_selection(
        "second-virtual", {same}, {1});
    expect(allowed.members() == std::vector<ProvidedDependency>{same},
           "Same provider identity across dependencies was rejected");
}

void test_legacy_adapter_rejects_cached_multiple_selection() {
    std::istringstream input;
    std::ostringstream output;
    ProviderSelectionSession session(input, output, true);
    const std::vector<ProvidedDependency> offered{
        named_repository_candidate("a", "virtual"),
        named_repository_candidate("b", "virtual")};
    static_cast<void>(session.record_provider_selection("virtual", offered, {1, 2}));
    bool rejected = false;
    try {
        static_cast<void>(session.select_provider("virtual", offered));
    } catch(const std::logic_error&) {
        rejected = true;
    }
    expect(rejected, "Legacy adapter flattened a multiple provider set");
}

void test_no_confirm_production_session_is_noninteractive() {
    std::shared_ptr<ProviderSelectionSession> session =
        make_provider_selection_session(true);
    expect(session != nullptr, "production session factory returned null");
    expect(
        !session->is_interactive(),
        "--noconfirm production session remained interactive");
    std::ostringstream output;
    const auto selected = session->select_provider_set(
        "virtual", {named_repository_candidate("a", "virtual"), named_repository_candidate("b", "virtual")},
        [&output](std::ostream&, std::size_t, const ProvidedDependency&) {
            output << "presented";
        });
    expect(!selected.has_value() && output.str().empty(),
           "--noconfirm public set path presented or selected a candidate");
}

} // namespace

int main() {
    try {
        test_compact_identity_and_style();
        test_legacy_soname_class_projection();
        test_soname_annotations_preserve_selection_and_installed_state();
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
        test_selection_set_retains_multiple_members_in_candidate_order();
        test_public_selection_expression_for_each_source();
        test_public_multiple_presentation_modes_preserve_selection();
        test_public_invalid_expression_retries_atomically();
        test_public_multiple_reuse_refresh_and_cancel();
        test_selection_set_deduplicates_identity_and_rejects_empty();
        test_selection_set_reuse_refreshes_all_members_and_fails_on_partial_loss();
        test_selection_set_rejects_within_and_cross_dependency_identity_conflicts();
        test_legacy_adapter_rejects_cached_multiple_selection();
        test_no_confirm_production_session_is_noninteractive();
        std::cout << "provider selection tests passed" << std::endl;
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
