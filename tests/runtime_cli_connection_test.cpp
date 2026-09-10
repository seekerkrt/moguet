#include "cli_runtime_contract.hpp"
#include "cli_routing.hpp"
#include "runtime_diagnostic.hpp"
#include "terminal_safe_text.hpp"
#include "terminal_safe_text_cases.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

ParsedCliArguments invocation(
    std::string operation, std::vector<std::string> operands = {},
    bool local_source = false, bool root_selection = false) {
    ParsedCliArguments parsed;
    parsed.operation = std::move(operation);
    parsed.root_package_selection_requested = root_selection;
    parsed.tokens.push_back(
        ParsedCliToken{parsed.operation, 1, CliTokenRole::Operation});
    if(local_source) {
        parsed.tokens.push_back(ParsedCliToken{
            std::string(cli_authority::LOCAL_SOURCE_OPTION), 2,
            CliTokenRole::PacmanOption});
    }
    for(std::size_t index = 0; index < operands.size(); ++index) {
        parsed.tokens.push_back(ParsedCliToken{
            operands[index], index + 2,
            CliTokenRole::Target});
        parsed.target_token_indices.push_back(parsed.tokens.size() - 1);
    }
    parsed.targets = std::move(operands);
    return parsed;
}

std::optional<ParsedCliArguments> parse_invocation(
    const std::vector<std::string>& arguments) {
    std::vector<std::string> argv{"moguet"};
    argv.insert(argv.end(), arguments.begin(), arguments.end());

    std::vector<char*> raw_argv;
    raw_argv.reserve(argv.size());
    for(std::string& argument : argv)
        raw_argv.push_back(argument.data());

    return parse_cli_arguments(
        static_cast<int>(raw_argv.size()), raw_argv.data());
}

ParsedCliArguments require_parsed_invocation(
    const std::vector<std::string>& arguments,
    const std::string& context) {
    std::optional<ParsedCliArguments> parsed =
        parse_invocation(arguments);
    expect(parsed.has_value(), context + ": parser rejected invocation");
    return std::move(parsed.value());
}

ParsedCliArguments with_pacman_option(
    ParsedCliArguments parsed, std::string option) {
    parsed.tokens.push_back(ParsedCliToken{
        std::move(option), parsed.tokens.size() + 1,
        CliTokenRole::PacmanOption});
    return parsed;
}

void expect_valid(
    const ParsedCliArguments& parsed, const std::string& context) {
    const CliInvocationValidation validation =
        validate_cli_invocation_contract(parsed);
    expect(validation.is_valid(), context + ": invocation was rejected");
}

void expect_issue(
    const ParsedCliArguments& parsed, CliInvocationIssueKind expected_kind,
    DiagnosticClass expected_class, const std::string& context) {
    const CliInvocationValidation validation =
        validate_cli_invocation_contract(parsed);
    expect(!validation.is_valid(), context + ": invocation was accepted");
    expect(
        validation.diagnostic->reason.kind == expected_kind &&
            validation.diagnostic->classification == expected_class,
        context + ": typed diagnostic differs");
}

void test_operand_contract_connection() {
    for(const char* operation : {
            "deps", "plan", "fetch", "edit-src", "del-src",
            "revert"}) {
        expect_valid(
            invocation(operation, {"one", "two"}),
            std::string{operation} + " multi-target");
        expect_issue(
            invocation(operation), CliInvocationIssueKind::MissingOperand,
            DiagnosticClass::Invalid,
            std::string{operation} + " missing target");
    }

    expect_valid(invocation("build", {"pkg"}), "remote build");
    expect_valid(
        invocation("build", {"pkg", "CFLAGS=-O2", "JOBS=4"}),
        "remote build assignments");
    expect_issue(
        invocation("build", {"pkg", "extra"}),
        CliInvocationIssueKind::ExtraOperand,
        DiagnosticClass::Invalid, "remote build extra bare operand");
    expect_valid(
        invocation("build", {"/source", "JOBS=4"}, true),
        "local build assignments");
    expect_issue(
        invocation("build", {"/source", "extra"}, true),
        CliInvocationIssueKind::ExtraOperand,
        DiagnosticClass::Invalid, "local build extra bare operand");

    for(const char* operation : {
            "upgrade", "upgrade-aur", "upgrade-all", "clean",
            "list-src"}) {
        expect_valid(
            invocation(operation),
            std::string{operation} + " targetless");
        expect_issue(
            invocation(operation, {"ignored-before-slice-3"}),
            CliInvocationIssueKind::ExtraOperand,
            DiagnosticClass::Invalid,
            std::string{operation} + " extra operand");
    }

    expect_valid(
        invocation(
            "add-src",
            {"first", "CFLAGS=-O2", "second", "JOBS=4"}),
        "add-src ordered items");
    expect_issue(
        invocation("add-src", {"CFLAGS=-O2", "first"}),
        CliInvocationIssueKind::InvalidOperandOrdering,
        DiagnosticClass::Invalid, "add-src assignment before package");

    expect_valid(invocation("-G", {"pkg"}), "-G exactly one");
    expect_valid(
        with_pacman_option(
            invocation("-G", {"pkg"}),
            "--output-dir=./exports"),
        "-G attached output directory");
    expect_valid(invocation("-Gp", {"pkg"}), "-Gp exactly one");
    expect_issue(
        invocation("-G", {"one", "two"}),
        CliInvocationIssueKind::ExtraOperand,
        DiagnosticClass::Invalid, "-G extra operand");
    expect_issue(
        with_pacman_option(
            invocation("-Gp", {"pkg"}),
            "--output-dir=./exports"),
        CliInvocationIssueKind::
            MisplacedPkgbuildOutputDirectoryOption,
        DiagnosticClass::Unsupported,
        "-Gp output directory scope");
    expect_issue(
        with_pacman_option(
            invocation("plan", {"pkg"}),
            "--output-dir=./exports"),
        CliInvocationIssueKind::
            MisplacedPkgbuildOutputDirectoryOption,
        DiagnosticClass::Unsupported,
        "other operation output directory scope");
    expect_issue(
        invocation("--output-dir=./exports", {"pkg"}),
        CliInvocationIssueKind::
            MisplacedPkgbuildOutputDirectoryOption,
        DiagnosticClass::Unsupported,
        "global-position output directory scope");
    expect_valid(
        invocation("-S", {"query"}, false, true),
        "-S --select exactly one");
    expect_issue(
        invocation("-S", {"one", "two"}, false, true),
        CliInvocationIssueKind::ExtraOperand,
        DiagnosticClass::Invalid, "-S --select extra operand");

    // Delegated pacman remains an open grammar and is not narrowed to the
    // Moguet-owned cardinality rules.
    expect_valid(
        invocation("-Q", {"one", "two", "--foreign"}),
        "delegated pacman open grammar");
    expect_valid(
        invocation("-S", {"one", "two"}),
        "plain delegated sync grammar");
    expect_issue(
        invocation("unknown-operation"),
        CliInvocationIssueKind::UnknownOperation,
        DiagnosticClass::Unsupported, "unknown Moguet operation");
}

void test_runtime_help_connection() {
    using cli_authority::OperationId;
    const std::array expected = {
        std::pair{OperationId::Build,
                  std::string{
                      "build <pkg> [V=K...] | build --local <directory> [V=K...]"}},
        std::pair{OperationId::Upgrade, std::string{"upgrade"}},
        std::pair{OperationId::Clean, std::string{"clean"}},
        std::pair{OperationId::Deps,
                  std::string{"deps [--recursive] <pkg>..."}},
        std::pair{OperationId::Plan, std::string{"plan <pkg>..."}},
        std::pair{OperationId::Fetch, std::string{"fetch <pkg>..."}},
        std::pair{OperationId::AddSource,
                  std::string{"add-src <item>..."}},
        std::pair{OperationId::EditSource,
                  std::string{"edit-src <pkg>..."}},
        std::pair{OperationId::ListSources,
                  std::string{"list-src"}},
        std::pair{OperationId::DeleteSource,
                  std::string{"del-src <pkg>..."}},
        std::pair{OperationId::Revert,
                  std::string{"revert <pkg>..."}}};
    for(const auto& [operation, syntax] : expected) {
        expect(
            cli_operation_syntax(operation) == syntax,
            "Runtime syntax did not derive canonical cardinality: " +
                syntax);
    }

    const std::vector<std::string> canonical = {
        "build <pkg> [V=K...]",
        "build --local <directory> [V=K...]",
        "upgrade",
        "upgrade-aur",
        "upgrade-all",
        "clean",
        "deps [--recursive] <pkg>...",
        "plan <pkg>...",
        "fetch <pkg>...",
        "add-src <item>...",
        "edit-src <pkg>...",
        "list-src",
        "del-src <pkg>...",
        "revert <pkg>...",
        "-G <pkg> [--output-dir=DIR]",
        "-Gp <pkg>",
        "-S --select [--needed] <query>",
        "-Syu [--needed]",
        "-Syu --repo [--needed]",
    };
    expect(
        cli_canonical_grammar() == canonical,
        "Canonical public grammar projection differs");
}

void test_system_update_runtime_authority() {
    using cli_authority::DelegatedPacmanTailPolicy;
    using cli_authority::GrammarOwnership;
    using cli_authority::SpecialOperationId;

    const ParsedCliArguments automatic =
        require_parsed_invocation({"-Syu"}, "automatic system update");
    const ResolvedCliRuntimeContract automatic_contract =
        resolve_cli_runtime_contract(automatic);
    expect(
        automatic_contract.special_operation ==
                &cli_authority::special_operation_spec(
                    SpecialOperationId::SystemAurUpdate) &&
            automatic_contract.owner == GrammarOwnership::InterceptedPacman &&
            automatic_contract.special_operation->dry_run_support ==
                cli_authority::DryRunSupport::Supported &&
            automatic_contract.special_operation
                    ->delegated_pacman_tail_policy ==
                DelegatedPacmanTailPolicy::None,
        "Automatic -Syu runtime authority differs");
    expect_valid(automatic, "automatic system update runtime contract");

    const ParsedCliArguments repo_only = require_parsed_invocation(
        {"-Syu", "--repo", "--config", "custom.conf"},
        "repository-only system update");
    const ResolvedCliRuntimeContract repo_contract =
        resolve_cli_runtime_contract(repo_only);
    expect(
        repo_contract.special_operation ==
                &cli_authority::special_operation_spec(
                    SpecialOperationId::SystemRepositoryUpdate) &&
            repo_contract.owner == GrammarOwnership::InterceptedPacman &&
            repo_contract.special_operation
                    ->delegated_pacman_tail_policy ==
                DelegatedPacmanTailPolicy::RepositoryOnly,
        "Repository-only -Syu runtime authority differs");
    expect_valid(repo_only, "repository-only delegated tail");
    expect(
        !validate_source_selection_operation(repo_only).has_value(),
        "Repository-only exact -Syu selector was rejected");

    const ParsedCliArguments unsupported_option =
        require_parsed_invocation(
            {"-Syu", "--config", "custom.conf"},
            "unsupported automatic option");
    expect_issue(
        unsupported_option,
        CliInvocationIssueKind::UnsupportedAutoSystemUpdateOption,
        DiagnosticClass::Unsupported,
        "unsupported automatic option gate");
    const std::string unsupported_message = cli_invocation_issue_message(
        validate_cli_invocation_contract(unsupported_option)
            .diagnostic->reason);
    expect(
        unsupported_message.find("moguet -Syu --repo") !=
                std::string::npos &&
            unsupported_message.find("custom.conf") == std::string::npos,
        "Unsupported automatic option diagnostic is unsafe or lacks migration guidance");

    const ParsedCliArguments unsupported_argument =
        require_parsed_invocation(
            {"-Syu", "--"}, "unsupported automatic argument form");
    expect_issue(
        unsupported_argument,
        CliInvocationIssueKind::
            UnsupportedAutoSystemUpdateArgumentForm,
        DiagnosticClass::Unsupported,
        "unsupported automatic argument-form gate");

    const ParsedCliArguments target_bearing =
        require_parsed_invocation(
            {"-Syu", "package"}, "target-bearing delegated update");
    const ResolvedCliRuntimeContract delegated_contract =
        resolve_cli_runtime_contract(target_bearing);
    expect(
        delegated_contract.special_operation ==
                &cli_authority::special_operation_spec(
                    SpecialOperationId::DelegatedPacmanGrammar) &&
            delegated_contract.is_delegated(),
        "Target-bearing -Syu was promoted into the composite route");

    const ParsedCliArguments aur_only =
        require_parsed_invocation({"-Syu", "--aur"}, "invalid AUR-only update");
    expect(
        validate_source_selection_operation(aur_only).has_value(),
        "-Syu --aur became a public AUR-only route");
}

void test_sync_invocation_route_classification() {
    struct RouteCase {
        std::string context;
        std::vector<std::string> arguments;
        SyncInvocationRouteClassification expected;
    };

    const std::vector<RouteCase> operation_cases = {
        {"canonical auto",
         {"-Syu"},
         AutoSystemUpdateRouteCandidate{
             CompatibleAutoSystemUpdatePacmanArguments{}, {"-Syu"}, false}},
        {"refresh only", {"-Sy"}, OtherSyncRoute{}},
        {"sysupgrade only", {"-Su"}, OtherSyncRoute{}},
        {"modifier order variation", {"-Suy"}, OtherSyncRoute{}},
        {"separated short modifiers",
         {"-S", "-y", "-u"},
         OtherSyncRoute{}},
        {"separated long modifiers",
         {"-S", "--refresh", "--sysupgrade"},
         OtherSyncRoute{}},
        {"target-bearing canonical form",
         {"-Syu", "package"},
         OtherSyncRoute{}},
        {"unknown modifier", {"-Syux"}, OtherSyncRoute{}},
    };

    const std::vector<RouteCase> selector_and_option_cases = {
        {"repo-only candidate",
         {"-Syu", "--repo"},
         RepoOnlySystemUpdateRouteCandidate{{"-Syu"}, false}},
        {"repo-only full ordered pass-through",
         {"-Syu", "--repo", "--config", "custom.conf"},
         RepoOnlySystemUpdateRouteCandidate{
             {"-Syu", "--config", "custom.conf"}, false}},
        {"invalid AUR-only route",
         {"-Syu", "--aur"},
         InvalidAurOnlySystemUpdateRoute{}},
        {"compatible needed option",
         {"-Syu", "--needed"},
         AutoSystemUpdateRouteCandidate{
             CompatibleAutoSystemUpdatePacmanArguments{},
             {"-Syu", "--needed"},
             true}},
        {"unsupported value-taking option",
         {"-Syu", "--config", "custom.conf"},
         AutoSystemUpdateRouteCandidate{
             IncompatibleAutoSystemUpdatePacmanArguments{
                 AutoSystemUpdatePacmanIncompatibilityKind::
                     UnsupportedOption,
                 "--config"},
             {"-Syu", "--config", "custom.conf"},
             false}},
        {"unsupported operand marker",
         {"-Syu", "--"},
         AutoSystemUpdateRouteCandidate{
             IncompatibleAutoSystemUpdatePacmanArguments{
                 AutoSystemUpdatePacmanIncompatibilityKind::
                     UnsupportedArgumentForm,
                 "--"},
             {"-Syu", "--"},
             false}},
        {"opaque operand is target-bearing",
         {"-Syu", "--", "--repo"},
         OtherSyncRoute{}},
        {"selector spelling as option value",
         {"-Syu", "--config", "--repo"},
         AutoSystemUpdateRouteCandidate{
             IncompatibleAutoSystemUpdatePacmanArguments{
                 AutoSystemUpdatePacmanIncompatibilityKind::
                     UnsupportedOption,
                 "--config"},
             {"-Syu", "--config", "--repo"},
             false}},
        {"AUR selector spelling as option value",
         {"-Syu", "--config", "--aur"},
         AutoSystemUpdateRouteCandidate{
             IncompatibleAutoSystemUpdatePacmanArguments{
                 AutoSystemUpdatePacmanIncompatibilityKind::
                     UnsupportedOption,
                 "--config"},
             {"-Syu", "--config", "--aur"},
             false}},
        {"leading semantic selector is not forwarded",
         {"--repo", "-Syu", "--needed"},
         RepoOnlySystemUpdateRouteCandidate{
             {"-Syu", "--needed"}, true}},
    };

    for(const RouteCase& route_case : operation_cases) {
        const ParsedCliArguments parsed = require_parsed_invocation(
            route_case.arguments, route_case.context);
        expect(
            classify_sync_invocation_route(parsed) ==
                route_case.expected,
            route_case.context + ": route classification differs");
    }
    for(const RouteCase& route_case : selector_and_option_cases) {
        const ParsedCliArguments parsed = require_parsed_invocation(
            route_case.arguments, route_case.context);
        expect(
            classify_sync_invocation_route(parsed) ==
                route_case.expected,
            route_case.context + ": route classification differs");
    }

    const ParsedCliArguments opaque_selector =
        require_parsed_invocation(
            {"-Syu", "--", "--repo"},
            "opaque selector lexical priority");
    expect(
        opaque_selector.source_selection ==
                PackageSourceSelection::Auto &&
            opaque_selector.tokens.back().role ==
                CliTokenRole::OpaqueOperand &&
            opaque_selector.ordered_pacman_args ==
                std::vector<std::string>{"-Syu", "--", "--repo"},
        "opaque selector spelling changed parser semantic state");

    const ParsedCliArguments semantic_aur =
        require_parsed_invocation(
            {"-Syu", "--aur"},
            "semantic AUR selector non-forwarding");
    expect(
        semantic_aur.source_selection ==
                PackageSourceSelection::AurOnly &&
            semantic_aur.ordered_pacman_args ==
                std::vector<std::string>{"-Syu"},
        "semantic AUR selector leaked into ordered pacman arguments");

    std::ostringstream parse_diagnostic;
    std::streambuf* previous_stderr =
        std::cerr.rdbuf(parse_diagnostic.rdbuf());
    const std::optional<ParsedCliArguments> conflicting =
        parse_invocation({"-Syu", "--aur", "--repo"});
    std::cerr.rdbuf(previous_stderr);
    expect(
        !conflicting.has_value(),
        "conflicting source selectors were parsed for classification");
}

void test_typed_runtime_diagnostic_connection() {
    const std::array classifications = {
        DiagnosticClass::Invalid,
        DiagnosticClass::Unsupported,
        DiagnosticClass::Ambiguous,
        DiagnosticClass::Declined,
        DiagnosticClass::Cancelled,
        DiagnosticClass::Unavailable,
        DiagnosticClass::InputFailure,
        DiagnosticClass::QueryFailure,
        DiagnosticClass::MetadataFailure,
        DiagnosticClass::RequiresCheck,
        DiagnosticClass::Blocked,
        DiagnosticClass::PartialFailure,
        DiagnosticClass::ExecutionFailure,
        DiagnosticClass::InternalInconsistency};
    const std::array labels = {
        "Invalid", "Unsupported", "Ambiguous", "Declined",
        "Cancelled", "Unavailable", "Input failure",
        "Query failure", "Metadata failure",
        "Requires check", "Blocked", "Partial failure",
        "Execution failure", "Internal inconsistency"};
    for(std::size_t index = 0; index < classifications.size(); ++index) {
        expect(
            diagnostic_class_label(classifications[index]) ==
                labels[index],
            "Runtime diagnostic taxonomy label differs");
    }

    NormalizedDiagnostic<std::string> diagnostic{
        DiagnosticClass::Ambiguous,
        DiagnosticSeverity::Warning,
        DiagnosticOperation::RootPackageSelection,
        DiagnosticPhase::Selection,
        DiagnosticIdentity{
            DiagnosticSourceKind::RepositorySource,
            std::string{"extra"},
            std::string{"requested-child"},
            std::string{"selected-base"},
            std::string{"repository-source:extra/selected-base"},
            std::nullopt},
        "typed-reason",
        DiagnosticRequiredAction::SelectCandidate,
        DiagnosticBlockingDecision::BlocksCurrentOperation,
        DiagnosticExitStatusEffect::Failure,
        std::nullopt};
    const RuntimeDiagnosticPresentation presentation =
        present_runtime_diagnostic(
            diagnostic,
            "raw detail says Cancelled and InternalInconsistency");
    expect(
        presentation.severity == DiagnosticSeverity::Warning,
        "Runtime presentation inferred severity from class");
    expect(
        presentation.message.starts_with("Ambiguous: ") &&
            presentation.message.find("source=repository-source") !=
                std::string::npos &&
            presentation.message.find("repository=extra") !=
                std::string::npos &&
            presentation.message.find("package=requested-child") !=
                std::string::npos &&
            presentation.message.find("PackageBase=selected-base") !=
                std::string::npos,
        "Runtime presentation lost typed source/PackageBase identity");

    diagnostic.classification = DiagnosticClass::Cancelled;
    const RuntimeDiagnosticPresentation cancelled =
        present_runtime_diagnostic(
            diagnostic, "raw detail says Ambiguous");
    expect(
        cancelled.message.starts_with("Cancelled: "),
        "Runtime presentation classified a localized/raw string");

    const std::string unsafe_detail =
        std::string{"日本語\\path\n"} +
        std::string{"\x1b", 1} +
        "escape" +
        std::string{"\xe2\x80\xae", 3} +
        std::string{"\xef\xbb\xbf", 3} +
        std::string{"\xff", 1};
    expect(
        terminal_safe_runtime_diagnostic_detail(unsafe_detail) ==
            "日本語\\x5Cpath\\x0A\\x1Bescape"
            "\\xE2\\x80\\xAE\\xEF\\xBB\\xBF\\xFF",
        "Runtime diagnostic detail did not escape terminal controls, bidi/BOM, or invalid UTF-8");
}

void test_terminal_safe_policy_and_runtime_boundary() {
    using terminal_safe_text_test_cases::bytes;

    for(const auto& test_case : terminal_safe_text_test_cases::cases()) {
        expect(
            terminal_safe_text::escape_utf8(test_case.input) ==
                test_case.expected,
            std::string("Shared terminal-safe policy differs for ") +
                std::string(test_case.label));
        expect(
            terminal_safe_runtime_diagnostic_detail(test_case.input) ==
                test_case.expected,
            std::string("Runtime wrapper differs for ") +
                std::string(test_case.label));
    }

    const std::string bidi = bytes({0xe2, 0x80, 0xae});
    const std::string bom = bytes({0xef, 0xbb, 0xbf});
    const std::string invalid = bytes({0xff});
    NormalizedDiagnostic<std::string> diagnostic{
        DiagnosticClass::QueryFailure,
        DiagnosticSeverity::Warning,
        DiagnosticOperation::Build,
        DiagnosticPhase::Query,
        DiagnosticIdentity{
            DiagnosticSourceKind::Local,
            std::string{"repo"} + bidi,
            std::string{"package"} + bom,
            std::string{"base\\value"},
            std::string{"source"} + invalid,
            std::filesystem::path(
                std::string{"/work/root\n"} + bytes({0x1b}) + bidi + bom)},
        "typed-reason",
        DiagnosticRequiredAction::RetryQuery,
        DiagnosticBlockingDecision::BlocksCurrentOperation,
        DiagnosticExitStatusEffect::Failure,
        std::nullopt};
    const std::string reason =
        std::string{"理由 日本語\n"} + bytes({0x1b}) +
        " literal \\x41 " + bidi + bom + invalid;
    const RuntimeDiagnosticPresentation presentation =
        present_runtime_diagnostic(diagnostic, reason);

    expect(
        presentation.severity == DiagnosticSeverity::Warning,
        "Runtime terminal-safe projection changed typed severity");
    expect(
        presentation.message.find(
            "Query failure: 理由 日本語\\x0A\\x1B literal \\x5Cx41 ") !=
            std::string::npos,
        "Runtime reason was not encoded exactly once");
    expect(
        presentation.message.find("repository=repo\\xE2\\x80\\xAE") !=
                std::string::npos &&
            presentation.message.find(
                "package=package\\xEF\\xBB\\xBF") !=
                std::string::npos &&
            presentation.message.find("PackageBase=base\\x5Cvalue") !=
                std::string::npos &&
            presentation.message.find("source identity=source\\xFF") !=
                std::string::npos &&
            presentation.message.find(
                "local root=/work/root\\x0A\\x1B\\xE2\\x80\\xAE\\xEF\\xBB\\xBF") !=
                std::string::npos,
        "Runtime diagnostic identity fields bypassed terminal-safe encoding");
    expect(
        presentation.message.find("\\x5Cx5Cx41") == std::string::npos,
        "Runtime reason was encoded more than once");
    expect(
        presentation.message.find('\n') == std::string::npos &&
            presentation.message.find('\x1b') == std::string::npos &&
            presentation.message.find(bidi) == std::string::npos &&
            presentation.message.find(bom) == std::string::npos &&
            presentation.message.find(static_cast<char>(0xff)) ==
                std::string::npos,
        "Runtime presentation retained raw unsafe bytes");
}

} // namespace

int main() {
    try {
        test_operand_contract_connection();
        std::cout << "  ok: runtime operand contract connection\n";
        test_runtime_help_connection();
        std::cout << "  ok: runtime help metadata connection\n";
        test_sync_invocation_route_classification();
        std::cout << "  ok: sync invocation route classification\n";
        test_system_update_runtime_authority();
        std::cout << "  ok: system update runtime authority\n";
        test_typed_runtime_diagnostic_connection();
        std::cout << "  ok: typed runtime diagnostic connection\n";
        test_terminal_safe_policy_and_runtime_boundary();
        std::cout << "  ok: terminal-safe policy/runtime boundary\n";
        std::cout << "Runtime CLI connection tests: all checks passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "runtime_cli_connection_test: " << error.what()
                  << std::endl;
        return 1;
    }
}
