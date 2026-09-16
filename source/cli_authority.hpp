#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace cli_authority {

enum class OperationId {
    Build,
    Upgrade,
    UpgradeAur,
    UpgradeAll,
    Clean,
    Deps,
    Plan,
    Fetch,
    AddSource,
    DeleteSource,
    Revert,
    EditSource,
    ListSources,
    Count,
};

struct OperationSpec {
    OperationId id;
    std::string_view token;
    bool rejects_options_before_dispatch;
};

inline constexpr std::array<OperationSpec, static_cast<std::size_t>(OperationId::Count)>
    MOGUET_OPERATIONS = {{
        {OperationId::Build, "build", true},
        {OperationId::Upgrade, "upgrade", true},
        {OperationId::UpgradeAur, "upgrade-aur", true},
        {OperationId::UpgradeAll, "upgrade-all", true},
        {OperationId::Clean, "clean", true},
        {OperationId::Deps, "deps", false},
        {OperationId::Plan, "plan", false},
        {OperationId::Fetch, "fetch", false},
        {OperationId::AddSource, "add-src", true},
        {OperationId::DeleteSource, "del-src", true},
        {OperationId::Revert, "revert", true},
        {OperationId::EditSource, "edit-src", true},
        {OperationId::ListSources, "list-src", true},
    }};

constexpr const OperationSpec& operation_spec(OperationId id) noexcept {
    return MOGUET_OPERATIONS[static_cast<std::size_t>(id)];
}

constexpr const OperationSpec* find_moguet_operation(
    std::string_view token) noexcept {
    for(const OperationSpec& spec : MOGUET_OPERATIONS) {
        if(spec.token == token) return &spec;
    }
    return nullptr;
}

enum class GlobalOptionId {
    Edit,
    NoEdit,
    Diff,
    NoDiff,
    NoConfirm,
    DryRun,
    BuildMode,
    Rebuild,
    CleanBuild,
    RmDeps,
    Select,
    Aur,
    Repo,
    Count,
};

struct GlobalOptionSpec {
    GlobalOptionId id;
    std::string_view token;
    bool accepts_attached_value;
};

inline constexpr std::array<GlobalOptionSpec, static_cast<std::size_t>(GlobalOptionId::Count)>
    MOGUET_GLOBAL_OPTIONS = {{
        {GlobalOptionId::Edit, "--edit", false},
        {GlobalOptionId::NoEdit, "--noedit", false},
        {GlobalOptionId::Diff, "--diff", false},
        {GlobalOptionId::NoDiff, "--nodiff", false},
        {GlobalOptionId::NoConfirm, "--noconfirm", false},
        {GlobalOptionId::DryRun, "--dry-run", false},
        {GlobalOptionId::BuildMode, "--build-mode", true},
        {GlobalOptionId::Rebuild, "--rebuild", false},
        {GlobalOptionId::CleanBuild, "--cleanbuild", false},
        {GlobalOptionId::RmDeps, "--rmdeps", false},
        {GlobalOptionId::Select, "--select", false},
        {GlobalOptionId::Aur, "--aur", false},
        {GlobalOptionId::Repo, "--repo", false},
    }};

constexpr const GlobalOptionSpec& global_option_spec(
    GlobalOptionId id) noexcept {
    return MOGUET_GLOBAL_OPTIONS[static_cast<std::size_t>(id)];
}

constexpr const GlobalOptionSpec* find_moguet_global_option(
    std::string_view argument) noexcept {
    for(const GlobalOptionSpec& spec : MOGUET_GLOBAL_OPTIONS) {
        if(argument == spec.token) return &spec;
        if(spec.accepts_attached_value &&
           argument.size() > spec.token.size() &&
           argument.starts_with(spec.token) &&
           argument[spec.token.size()] == '=') {
            return &spec;
        }
    }
    return nullptr;
}

inline constexpr std::string_view BUILD_MODE_NORMAL = "normal";
inline constexpr std::string_view BUILD_MODE_REBUILD = "rebuild";
inline constexpr std::string_view BUILD_MODE_CLEAN = "clean";
inline constexpr std::string_view BUILD_MODE_REBUILD_OPTION =
    "--build-mode=rebuild";
inline constexpr std::string_view BUILD_MODE_CLEAN_OPTION =
    "--build-mode=clean";

// Operation-local source selector。GlobalOptionSpecへ昇格させず、build routing
// だけが解釈する。
inline constexpr std::string_view LOCAL_SOURCE_OPTION = "--local";

// PKGBUILD exportだけが解釈するoperation-local attached-value option。
// GlobalOptionSpecやCliOverridesへ昇格させない。
inline constexpr std::string_view PKGBUILD_OUTPUT_DIRECTORY_OPTION =
    "--output-dir";

inline constexpr std::string_view HELP_SHORT_OPTION = "-h";
inline constexpr std::string_view HELP_LONG_OPTION = "--help";
inline constexpr std::string_view VERSION_SHORT_OPTION = "-V";
inline constexpr std::string_view VERSION_LONG_OPTION = "--version";

inline constexpr std::string_view PKGBUILD_EXPORT_OPERATION = "-G";
inline constexpr std::string_view PKGBUILD_PRINT_OPERATION = "-Gp";

// Pacman-compatible syntax constants are public grammar identities, not a
// parser allowlist. -Syu / -Su key the exact intercepted forms below; the
// remaining spellings stay open delegated examples.
inline constexpr std::string_view PACMAN_SYNC_INSTALL_SYNTAX = "-S <pkg>";
inline constexpr std::string_view PACMAN_SYSTEM_UPGRADE_SYNTAX = "-Syu";
inline constexpr std::string_view PACMAN_SYSTEM_UPGRADE_NO_REFRESH_SYNTAX = "-Su";
inline constexpr std::string_view PACMAN_SYNC_SEARCH_SYNTAX = "-Ss <query>";
inline constexpr std::string_view PACMAN_SYNC_INFO_SYNTAX = "-Si <pkg>";
inline constexpr std::string_view PACMAN_FOREIGN_UPDATES_SYNTAX = "-Qua";
inline constexpr std::string_view PACMAN_NEEDED_OPTION = "--needed";

// Public token compatibility remains in MOGUET_OPERATIONS and
// MOGUET_GLOBAL_OPTIONS above. The structured contract below is keyed by
// those stable IDs and owns public grammar semantics.
enum class GrammarOwnership {
    MoguetOwned,
    InterceptedPacman,
    DelegatedPacman,
};

enum class OperationSemanticScope {
    SourceBuild,
    SystemAndRegisteredSourceUpgrade,
    AurUpgrade,
    SystemRegisteredAndAurUpgrade,
    SystemAndNormalAurUpgrade,
    RepositorySystemUpgrade,
    SourceMaintenance,
    DependencyInspection,
    SourceFetch,
    Information,
    PackageExport,
    RootPackageSelection,
    PacmanDelegation,
};

enum class DryRunSupport {
    Supported,
    Unsupported,
    NotApplicable,
};

enum class OperandKind {
    None,
    Package,
    Directory,
    Query,
    SourcePreferenceItem,
    EnvironmentAssignment,
    DelegatedPacmanArgument,
};

enum class OperandOrderingRule {
    None,
    PreserveInputOrder,
    PrimaryThenEnvironmentAssignments,
    PackageIntroducesFollowingAssignmentScope,
    Delegated,
};

enum class TargetPolicy {
    None,
    ExactlyOne,
    OneOrMore,
    OrderedItems,
    Delegated,
};

inline constexpr std::size_t UNBOUNDED_OPERAND_COUNT =
    std::numeric_limits<std::size_t>::max();

struct OperandTermSpec {
    OperandKind kind = OperandKind::None;
    std::size_t min_count = 0;
    std::size_t max_count = 0;
};

struct OperandContract {
    std::array<OperandTermSpec, 2> terms{};
    std::size_t term_count = 0;
    OperandOrderingRule ordering = OperandOrderingRule::None;
};

constexpr OperandContract no_operands() noexcept {
    return {};
}

constexpr OperandContract one_operand_term(
    OperandKind kind, std::size_t min_count, std::size_t max_count,
    OperandOrderingRule ordering) noexcept {
    return OperandContract{{OperandTermSpec{kind, min_count, max_count}, {}},
                           1,
                           ordering};
}

constexpr OperandContract operand_with_trailing_assignments(
    OperandKind primary_kind) noexcept {
    return OperandContract{{OperandTermSpec{primary_kind, 1, 1},
                            OperandTermSpec{
                                OperandKind::EnvironmentAssignment,
                                0,
                                UNBOUNDED_OPERAND_COUNT}},
                           2,
                           OperandOrderingRule::
                               PrimaryThenEnvironmentAssignments};
}

enum class OptionId {
    Edit,
    NoEdit,
    Diff,
    NoDiff,
    NoConfirm,
    DryRun,
    BuildMode,
    Rebuild,
    CleanBuild,
    RmDeps,
    Select,
    Aur,
    Repo,
    Help,
    Version,
    LocalSource,
    PkgbuildOutputDirectory,
    Recursive,
    Needed,
    EndOfOptions,
    Count,
};

static_assert(
    static_cast<std::size_t>(GlobalOptionId::Count) ==
    static_cast<std::size_t>(OptionId::Help));
static_assert(
    static_cast<std::size_t>(GlobalOptionId::Edit) ==
        static_cast<std::size_t>(OptionId::Edit) &&
    static_cast<std::size_t>(GlobalOptionId::NoEdit) ==
        static_cast<std::size_t>(OptionId::NoEdit) &&
    static_cast<std::size_t>(GlobalOptionId::Diff) ==
        static_cast<std::size_t>(OptionId::Diff) &&
    static_cast<std::size_t>(GlobalOptionId::NoDiff) ==
        static_cast<std::size_t>(OptionId::NoDiff) &&
    static_cast<std::size_t>(GlobalOptionId::NoConfirm) ==
        static_cast<std::size_t>(OptionId::NoConfirm) &&
    static_cast<std::size_t>(GlobalOptionId::DryRun) ==
        static_cast<std::size_t>(OptionId::DryRun) &&
    static_cast<std::size_t>(GlobalOptionId::BuildMode) ==
        static_cast<std::size_t>(OptionId::BuildMode) &&
    static_cast<std::size_t>(GlobalOptionId::Rebuild) ==
        static_cast<std::size_t>(OptionId::Rebuild) &&
    static_cast<std::size_t>(GlobalOptionId::CleanBuild) ==
        static_cast<std::size_t>(OptionId::CleanBuild) &&
    static_cast<std::size_t>(GlobalOptionId::RmDeps) ==
        static_cast<std::size_t>(OptionId::RmDeps) &&
    static_cast<std::size_t>(GlobalOptionId::Select) ==
        static_cast<std::size_t>(OptionId::Select) &&
    static_cast<std::size_t>(GlobalOptionId::Aur) ==
        static_cast<std::size_t>(OptionId::Aur) &&
    static_cast<std::size_t>(GlobalOptionId::Repo) ==
        static_cast<std::size_t>(OptionId::Repo));

constexpr OptionId option_id(GlobalOptionId id) noexcept {
    return static_cast<OptionId>(id);
}

struct TokenAliasSet {
    std::array<std::string_view, 2> values{};
    std::size_t count = 0;

    constexpr bool contains(std::string_view token) const noexcept {
        for(std::size_t index = 0; index < count; ++index) {
            if(values[index] == token) return true;
        }
        return false;
    }
};

constexpr TokenAliasSet no_token_aliases() noexcept {
    return {};
}

constexpr TokenAliasSet token_alias(std::string_view alias) noexcept {
    return TokenAliasSet{{alias, {}}, 1};
}

enum class OptionValueKind {
    None,
    AttachedEnum,
    AttachedValue,
    Marker,
};

struct OptionValueContract {
    OptionValueKind kind = OptionValueKind::None;
    std::array<std::string_view, 3> allowed_values{};
    std::size_t allowed_value_count = 0;
};

constexpr OptionValueContract no_option_value() noexcept {
    return {};
}

constexpr OptionValueContract build_mode_value() noexcept {
    return OptionValueContract{
        OptionValueKind::AttachedEnum,
        {BUILD_MODE_NORMAL, BUILD_MODE_REBUILD, BUILD_MODE_CLEAN},
        3};
}

constexpr OptionValueContract output_directory_value() noexcept {
    return OptionValueContract{
        OptionValueKind::AttachedValue, {"DIR", {}, {}}, 1};
}

enum class OptionOccurrence {
    Once,
    RepeatIdempotent,
    RepeatSameValue,
    Delegated,
};

enum class OptionConflictRule {
    None,
    MutuallyExclusive,
    FinalValueMustAgree,
};

struct OptionConflictSet {
    std::array<OptionId, 4> values{};
    std::size_t count = 0;
    OptionConflictRule rule = OptionConflictRule::None;
    std::string_view value_identity;

    constexpr bool contains(OptionId id) const noexcept {
        for(std::size_t index = 0; index < count; ++index) {
            if(values[index] == id) return true;
        }
        return false;
    }
};

constexpr OptionConflictSet no_option_conflicts() noexcept {
    return {};
}

constexpr OptionConflictSet option_conflicts(OptionId first) noexcept {
    return OptionConflictSet{{first, OptionId::Edit, OptionId::Edit,
                              OptionId::Edit},
                             1,
                             OptionConflictRule::MutuallyExclusive,
                             {}};
}

constexpr OptionConflictSet option_conflicts(
    OptionId first, OptionId second) noexcept {
    return OptionConflictSet{{first, second, OptionId::Edit, OptionId::Edit},
                             2,
                             OptionConflictRule::MutuallyExclusive,
                             {}};
}

constexpr OptionConflictSet final_value_conflicts(
    std::string_view value_identity, OptionId first) noexcept {
    return OptionConflictSet{{first, OptionId::Edit, OptionId::Edit,
                              OptionId::Edit},
                             1,
                             OptionConflictRule::FinalValueMustAgree,
                             value_identity};
}

constexpr OptionConflictSet final_value_conflicts(
    std::string_view value_identity, OptionId first,
    OptionId second) noexcept {
    return OptionConflictSet{{first, second, OptionId::Edit, OptionId::Edit},
                             2,
                             OptionConflictRule::FinalValueMustAgree,
                             value_identity};
}

enum class OptionLexicalPlacement {
    ParserGlobalNormalPosition,
    FirstNonGlobalToken,
    OperationLocal,
    PacmanGrammar,
    EndOfOptionsMarker,
};

enum class OptionSemanticScope : std::uint32_t {
    Information = 1U << 0,
    SourceBuildReview = 1U << 1,
    SourceCheckoutReview = 1U << 2,
    SourceBuild = 1U << 3,
    DryRunRouting = 1U << 4,
    RootPackageSelection = 1U << 5,
    SourceSelection = 1U << 6,
    LocalSourceBuild = 1U << 7,
    DependencyInspection = 1U << 8,
    FinalPackageInstall = 1U << 9,
    PacmanDelegation = 1U << 10,
    ParserBoundary = 1U << 11,
    DependencyCleanup = 1U << 12,
    PackageExport = 1U << 13,
};

using OptionSemanticScopeMask = std::uint32_t;

constexpr OptionSemanticScopeMask option_scope(
    OptionSemanticScope scope) noexcept {
    return static_cast<OptionSemanticScopeMask>(scope);
}

constexpr bool has_option_scope(
    OptionSemanticScopeMask scopes, OptionSemanticScope scope) noexcept {
    return (scopes & option_scope(scope)) != 0;
}

enum class OptionPublicDefinitionRole {
    Definition,
    SyntaxOnly,
    SchemaOnly,
};

enum class OptionCompletionVisibility {
    SuggestedAndDescribed,
    Hidden,
};

struct OptionContract {
    OptionId id;
    std::string_view canonical_token;
    TokenAliasSet aliases;
    OptionValueContract value;
    OptionOccurrence default_occurrence;
    OptionConflictSet conflicts;
    OptionLexicalPlacement lexical_placement;
    OptionSemanticScopeMask semantic_scopes;
    GrammarOwnership owner;
    OptionPublicDefinitionRole public_definition_role;
    OptionCompletionVisibility completion_visibility;
    std::string_view related_contract_identity;
};

// OptionContract describes lexical/default metadata. Once an operation route
// is known, OptionRelationContract is the sole occurrence, semantic-effect,
// and forwarding authority.

inline constexpr std::array<OptionContract,
                            static_cast<std::size_t>(OptionId::Count)>
    MOGUET_OPTION_CONTRACTS = {{
        {OptionId::Edit,
         global_option_spec(GlobalOptionId::Edit).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         final_value_conflicts("review.pkgbuild", OptionId::NoEdit),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceBuildReview),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.source-review"},
        {OptionId::NoEdit,
         global_option_spec(GlobalOptionId::NoEdit).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         final_value_conflicts("review.pkgbuild", OptionId::Edit),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceBuildReview),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.source-review"},
        {OptionId::Diff,
         global_option_spec(GlobalOptionId::Diff).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         final_value_conflicts("review.diff", OptionId::NoDiff),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceCheckoutReview),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.source-diff"},
        {OptionId::NoDiff,
         global_option_spec(GlobalOptionId::NoDiff).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         final_value_conflicts("review.diff", OptionId::Diff),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceCheckoutReview),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.source-diff"},
        {OptionId::NoConfirm,
         global_option_spec(GlobalOptionId::NoConfirm).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         no_option_conflicts(),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceBuild) |
             option_scope(OptionSemanticScope::RootPackageSelection) |
             option_scope(OptionSemanticScope::PacmanDelegation),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.no-confirm"},
        {OptionId::DryRun,
         global_option_spec(GlobalOptionId::DryRun).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         no_option_conflicts(),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::DryRunRouting),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.dry-run"},
        {OptionId::BuildMode,
         global_option_spec(GlobalOptionId::BuildMode).token,
         no_token_aliases(),
         build_mode_value(),
         OptionOccurrence::RepeatSameValue,
         final_value_conflicts(
             "build.mode", OptionId::Rebuild,
             OptionId::CleanBuild),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceBuild),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.build-mode"},
        {OptionId::Rebuild,
         global_option_spec(GlobalOptionId::Rebuild).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         final_value_conflicts(
             "build.mode", OptionId::BuildMode,
             OptionId::CleanBuild),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceBuild),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.build-mode.rebuild"},
        {OptionId::CleanBuild,
         global_option_spec(GlobalOptionId::CleanBuild).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         final_value_conflicts(
             "build.mode", OptionId::BuildMode,
             OptionId::Rebuild),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceBuild),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.build-mode.clean"},
        {OptionId::RmDeps,
         global_option_spec(GlobalOptionId::RmDeps).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         option_conflicts(OptionId::Select),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::DependencyCleanup) |
             option_scope(OptionSemanticScope::PacmanDelegation),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.remove-dependencies"},
        {OptionId::Select,
         global_option_spec(GlobalOptionId::Select).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         option_conflicts(OptionId::RmDeps),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::RootPackageSelection),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.root-selection"},
        {OptionId::Aur,
         global_option_spec(GlobalOptionId::Aur).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         final_value_conflicts("source.selection", OptionId::Repo),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceSelection),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.source-selection"},
        {OptionId::Repo,
         global_option_spec(GlobalOptionId::Repo).token,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         final_value_conflicts("source.selection", OptionId::Aur),
         OptionLexicalPlacement::ParserGlobalNormalPosition,
         option_scope(OptionSemanticScope::SourceSelection),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.option.source-selection"},
        {OptionId::Help,
         HELP_LONG_OPTION,
         token_alias(HELP_SHORT_OPTION),
         no_option_value(),
         OptionOccurrence::Once,
         no_option_conflicts(),
         OptionLexicalPlacement::FirstNonGlobalToken,
         option_scope(OptionSemanticScope::Information),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.information.help"},
        {OptionId::Version,
         VERSION_LONG_OPTION,
         token_alias(VERSION_SHORT_OPTION),
         no_option_value(),
         OptionOccurrence::Once,
         no_option_conflicts(),
         OptionLexicalPlacement::FirstNonGlobalToken,
         option_scope(OptionSemanticScope::Information),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.information.version"},
        {OptionId::LocalSource,
         LOCAL_SOURCE_OPTION,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::Once,
         no_option_conflicts(),
         OptionLexicalPlacement::OperationLocal,
         option_scope(OptionSemanticScope::LocalSourceBuild),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::SyntaxOnly,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.build.local"},
        {OptionId::PkgbuildOutputDirectory,
         PKGBUILD_OUTPUT_DIRECTORY_OPTION,
         no_token_aliases(),
         output_directory_value(),
         OptionOccurrence::Once,
         no_option_conflicts(),
         OptionLexicalPlacement::OperationLocal,
         option_scope(OptionSemanticScope::PackageExport),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.pkgbuild.output-directory"},
        {OptionId::Recursive,
         "--recursive",
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::RepeatIdempotent,
         no_option_conflicts(),
         OptionLexicalPlacement::OperationLocal,
         option_scope(OptionSemanticScope::DependencyInspection),
         GrammarOwnership::MoguetOwned,
         OptionPublicDefinitionRole::SyntaxOnly,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.deps.recursive"},
        {OptionId::Needed,
         PACMAN_NEEDED_OPTION,
         no_token_aliases(),
         no_option_value(),
         OptionOccurrence::Delegated,
         no_option_conflicts(),
         OptionLexicalPlacement::PacmanGrammar,
         option_scope(OptionSemanticScope::FinalPackageInstall) |
             option_scope(OptionSemanticScope::RootPackageSelection) |
             option_scope(OptionSemanticScope::PacmanDelegation),
         GrammarOwnership::InterceptedPacman,
         OptionPublicDefinitionRole::Definition,
         OptionCompletionVisibility::SuggestedAndDescribed,
         "cli.pacman.needed"},
        {OptionId::EndOfOptions,
         "--",
         no_token_aliases(),
         OptionValueContract{OptionValueKind::Marker, {}, 0},
         OptionOccurrence::Once,
         no_option_conflicts(),
         OptionLexicalPlacement::EndOfOptionsMarker,
         option_scope(OptionSemanticScope::ParserBoundary) |
             option_scope(OptionSemanticScope::PacmanDelegation),
         GrammarOwnership::InterceptedPacman,
         OptionPublicDefinitionRole::SchemaOnly,
         OptionCompletionVisibility::Hidden,
         "cli.lexical.end-of-options"},
    }};

constexpr const OptionContract& option_contract(OptionId id) noexcept {
    return MOGUET_OPTION_CONTRACTS[static_cast<std::size_t>(id)];
}

constexpr const OptionContract* find_option_contract(
    std::string_view token) noexcept {
    for(const OptionContract& contract : MOGUET_OPTION_CONTRACTS) {
        if(token == contract.canonical_token || contract.aliases.contains(token)) {
            return &contract;
        }
        if((contract.value.kind == OptionValueKind::AttachedEnum ||
            contract.value.kind == OptionValueKind::AttachedValue) &&
           token.size() > contract.canonical_token.size() &&
           token.starts_with(contract.canonical_token) &&
           token[contract.canonical_token.size()] == '=') {
            return &contract;
        }
    }
    return nullptr;
}

enum class OptionRelationRequirement {
    Optional,
    Required,
};

enum class OptionPublicSyntax {
    Hidden,
    Optional,
    Required,
};

enum class OptionSemanticEffect : std::uint32_t {
    None = 0,
    MoguetControl = 1U << 0,
    UpstreamArgument = 1U << 1,
    FinalInstallSemantic = 1U << 2,
    ParserBoundary = 1U << 3,
};

using OptionSemanticEffectMask = std::uint32_t;

constexpr OptionSemanticEffectMask option_effect(
    OptionSemanticEffect effect) noexcept {
    return static_cast<OptionSemanticEffectMask>(effect);
}

constexpr bool has_option_effect(
    OptionSemanticEffectMask effects,
    OptionSemanticEffect effect) noexcept {
    return (effects & option_effect(effect)) != 0;
}

enum class OptionForwardingTarget : std::uint32_t {
    None = 0,
    Pacman = 1U << 0,
    Makepkg = 1U << 1,
    FinalInstallPacman = 1U << 2,
};

using OptionForwardingTargetMask = std::uint32_t;

constexpr OptionForwardingTargetMask option_forwarding_target(
    OptionForwardingTarget target) noexcept {
    return static_cast<OptionForwardingTargetMask>(target);
}

constexpr bool has_option_forwarding_target(
    OptionForwardingTargetMask targets,
    OptionForwardingTarget target) noexcept {
    return (targets & option_forwarding_target(target)) != 0;
}

enum class OptionForwardingOccurrence {
    None,
    PreserveAll,
    ConsolidateSingle,
};

struct OptionRelationContract {
    OptionId option = OptionId::Edit;
    OptionRelationRequirement requirement =
        OptionRelationRequirement::Optional;
    OptionOccurrence occurrence = OptionOccurrence::Once;
    OptionSemanticEffectMask semantic_effects =
        option_effect(OptionSemanticEffect::None);
    OptionForwardingTargetMask forwarding_targets =
        option_forwarding_target(OptionForwardingTarget::None);
    OptionForwardingOccurrence forwarding_occurrence =
        OptionForwardingOccurrence::None;
    OptionPublicSyntax public_syntax =
        OptionPublicSyntax::Hidden;
};

constexpr OptionRelationContract consumed_option_relation(
    OptionId option,
    OptionRelationRequirement requirement =
        OptionRelationRequirement::Optional) noexcept {
    return OptionRelationContract{
        option,
        requirement,
        option_contract(option).default_occurrence,
        option_effect(OptionSemanticEffect::MoguetControl),
        option_forwarding_target(OptionForwardingTarget::None),
        OptionForwardingOccurrence::None};
}

constexpr OptionRelationContract source_no_confirm_option_relation() noexcept {
    return OptionRelationContract{
        OptionId::NoConfirm,
        OptionRelationRequirement::Optional,
        OptionOccurrence::RepeatIdempotent,
        option_effect(OptionSemanticEffect::MoguetControl) |
            option_effect(OptionSemanticEffect::UpstreamArgument),
        option_forwarding_target(OptionForwardingTarget::Pacman) |
            option_forwarding_target(OptionForwardingTarget::Makepkg) |
            option_forwarding_target(
                OptionForwardingTarget::FinalInstallPacman),
        OptionForwardingOccurrence::ConsolidateSingle};
}

constexpr OptionRelationContract source_needed_option_relation() noexcept {
    return OptionRelationContract{
        OptionId::Needed,
        OptionRelationRequirement::Optional,
        OptionOccurrence::RepeatIdempotent,
        option_effect(OptionSemanticEffect::FinalInstallSemantic),
        option_forwarding_target(
            OptionForwardingTarget::FinalInstallPacman),
        OptionForwardingOccurrence::ConsolidateSingle};
}

constexpr OptionRelationContract sync_select_no_confirm_option_relation() noexcept {
    return OptionRelationContract{
        OptionId::NoConfirm,
        OptionRelationRequirement::Optional,
        OptionOccurrence::RepeatIdempotent,
        option_effect(OptionSemanticEffect::MoguetControl),
        option_forwarding_target(OptionForwardingTarget::None),
        OptionForwardingOccurrence::None};
}

constexpr OptionRelationContract sync_select_needed_option_relation() noexcept {
    return OptionRelationContract{
        OptionId::Needed,
        OptionRelationRequirement::Optional,
        OptionOccurrence::RepeatIdempotent,
        option_effect(OptionSemanticEffect::UpstreamArgument) |
            option_effect(
                OptionSemanticEffect::FinalInstallSemantic),
        option_forwarding_target(OptionForwardingTarget::Pacman) |
            option_forwarding_target(
                OptionForwardingTarget::FinalInstallPacman),
        OptionForwardingOccurrence::ConsolidateSingle};
}

constexpr OptionRelationContract delegated_needed_option_relation() noexcept {
    return OptionRelationContract{
        OptionId::Needed,
        OptionRelationRequirement::Optional,
        OptionOccurrence::Delegated,
        option_effect(OptionSemanticEffect::UpstreamArgument),
        option_forwarding_target(OptionForwardingTarget::Pacman),
        OptionForwardingOccurrence::PreserveAll};
}

// Exact supported targetless system-update forms keep --needed in the repository transaction only.
// The later normal-AUR transaction deliberately does not inherit it.
constexpr OptionRelationContract system_aur_needed_option_relation() noexcept {
    return OptionRelationContract{
        OptionId::Needed,
        OptionRelationRequirement::Optional,
        OptionOccurrence::RepeatIdempotent,
        option_effect(OptionSemanticEffect::UpstreamArgument),
        option_forwarding_target(OptionForwardingTarget::Pacman),
        OptionForwardingOccurrence::PreserveAll};
}

constexpr OptionRelationContract delegated_end_of_options_relation() noexcept {
    return OptionRelationContract{
        OptionId::EndOfOptions,
        OptionRelationRequirement::Optional,
        OptionOccurrence::Once,
        option_effect(OptionSemanticEffect::ParserBoundary) |
            option_effect(OptionSemanticEffect::UpstreamArgument),
        option_forwarding_target(OptionForwardingTarget::Pacman),
        OptionForwardingOccurrence::PreserveAll};
}

constexpr OptionRelationContract pacman_no_confirm_option_relation() noexcept {
    return OptionRelationContract{
        OptionId::NoConfirm,
        OptionRelationRequirement::Optional,
        OptionOccurrence::RepeatIdempotent,
        option_effect(OptionSemanticEffect::MoguetControl) |
            option_effect(OptionSemanticEffect::UpstreamArgument),
        option_forwarding_target(OptionForwardingTarget::Pacman),
        OptionForwardingOccurrence::ConsolidateSingle};
}

constexpr OptionRelationContract relation_contract(
    OptionId option) noexcept {
    if(option == OptionId::NoConfirm) {
        return consumed_option_relation(option);
    }
    if(option == OptionId::Needed) {
        return source_needed_option_relation();
    }
    if(option == OptionId::EndOfOptions) {
        return OptionRelationContract{
            option,
            OptionRelationRequirement::Optional,
            OptionOccurrence::Once,
            option_effect(OptionSemanticEffect::ParserBoundary),
            option_forwarding_target(OptionForwardingTarget::None),
            OptionForwardingOccurrence::None};
    }
    return consumed_option_relation(option);
}

constexpr OptionRelationContract relation_contract(
    OptionRelationContract relation) noexcept {
    return relation;
}

constexpr OptionRelationContract public_syntax_option_relation(
    OptionId option, OptionPublicSyntax public_syntax) noexcept {
    OptionRelationContract relation = consumed_option_relation(option);
    relation.public_syntax = public_syntax;
    return relation;
}

constexpr OptionRelationContract public_syntax_option_relation(
    OptionRelationContract relation,
    OptionPublicSyntax public_syntax) noexcept {
    relation.public_syntax = public_syntax;
    return relation;
}

struct OperationOptionRelationSet {
    std::array<OptionRelationContract, 13> values{};
    std::size_t count = 0;

    constexpr bool contains(OptionId id) const noexcept {
        for(std::size_t index = 0; index < count; ++index) {
            if(values[index].option == id) return true;
        }
        return false;
    }

    constexpr const OptionRelationContract* find(
        OptionId id) const noexcept {
        for(std::size_t index = 0; index < count; ++index) {
            if(values[index].option == id) return &values[index];
        }
        return nullptr;
    }
};

constexpr OperationOptionRelationSet no_operation_option_relations() noexcept {
    return {};
}

template <typename... Ids>
constexpr OperationOptionRelationSet operation_option_relations(
    Ids... ids) noexcept {
    static_assert(sizeof...(Ids) <= 13);
    OperationOptionRelationSet relations;
    ((relations.values[relations.count++] = relation_contract(ids)), ...);
    return relations;
}

constexpr OperationOptionRelationSet operation_option_relation(
    OptionId id) noexcept {
    return operation_option_relations(id);
}

struct OperationFormSpec {
    OperationId operation;
    std::string_view related_contract_identity;
    OperandContract operands;
    TargetPolicy target_policy;
    OperationOptionRelationSet option_relations;
};

// option_relations lists semantic effects for the form. Parser-global lexical
// acceptance is deliberately separate and remains owned by GlobalOptionSpec.
inline constexpr std::array<OperationFormSpec, 14> MOGUET_OPERATION_FORMS = {{
    {OperationId::Build,
     "cli.build.remote",
     operand_with_trailing_assignments(OperandKind::Package),
     TargetPolicy::ExactlyOne,
     operation_option_relations(
         OptionId::Edit, OptionId::NoEdit,
         OptionId::Diff, OptionId::NoDiff,
         source_no_confirm_option_relation(), OptionId::DryRun,
         OptionId::BuildMode, OptionId::Rebuild,
         OptionId::CleanBuild)},
    {OperationId::Build,
     "cli.build.local",
     operand_with_trailing_assignments(OperandKind::Directory),
     TargetPolicy::ExactlyOne,
     operation_option_relations(
         OptionId::Edit, OptionId::NoEdit,
         source_no_confirm_option_relation(), OptionId::DryRun,
         OptionId::BuildMode, OptionId::Rebuild,
         OptionId::CleanBuild,
         public_syntax_option_relation(
             OptionId::LocalSource,
             OptionPublicSyntax::Required))},
    {OperationId::Upgrade,
     "cli.upgrade.registered-and-system",
     no_operands(),
     TargetPolicy::None,
     operation_option_relations(
         OptionId::Edit, OptionId::NoEdit,
         OptionId::Diff, OptionId::NoDiff,
         source_no_confirm_option_relation(), OptionId::DryRun,
         OptionId::BuildMode, OptionId::Rebuild,
         OptionId::CleanBuild)},
    {OperationId::UpgradeAur,
     "cli.upgrade.aur",
     no_operands(),
     TargetPolicy::None,
     operation_option_relations(
         OptionId::Edit, OptionId::NoEdit,
         OptionId::Diff, OptionId::NoDiff,
         source_no_confirm_option_relation(), OptionId::DryRun,
         OptionId::BuildMode, OptionId::Rebuild,
         OptionId::CleanBuild)},
    {OperationId::UpgradeAll,
     "cli.upgrade.all",
     no_operands(),
     TargetPolicy::None,
     operation_option_relations(
         OptionId::Edit, OptionId::NoEdit,
         OptionId::Diff, OptionId::NoDiff,
         source_no_confirm_option_relation(), OptionId::DryRun,
         OptionId::BuildMode, OptionId::Rebuild,
         OptionId::CleanBuild)},
    {OperationId::Clean,
     "cli.maintenance.clean",
     no_operands(),
     TargetPolicy::None,
     operation_option_relations(
         pacman_no_confirm_option_relation())},
    {OperationId::Deps,
     "cli.inspect.dependencies",
     one_operand_term(OperandKind::Package, 1, UNBOUNDED_OPERAND_COUNT,
                      OperandOrderingRule::PreserveInputOrder),
     TargetPolicy::OneOrMore,
     operation_option_relations(
         consumed_option_relation(OptionId::NoConfirm),
         public_syntax_option_relation(
             OptionId::Recursive,
             OptionPublicSyntax::Optional))},
    {OperationId::Plan,
     "cli.inspect.plan",
     one_operand_term(OperandKind::Package, 1, UNBOUNDED_OPERAND_COUNT,
                      OperandOrderingRule::PreserveInputOrder),
     TargetPolicy::OneOrMore,
     operation_option_relations(
         consumed_option_relation(OptionId::NoConfirm))},
    {OperationId::Fetch,
     "cli.fetch.sources",
     one_operand_term(OperandKind::Package, 1, UNBOUNDED_OPERAND_COUNT,
                      OperandOrderingRule::PreserveInputOrder),
     TargetPolicy::OneOrMore,
     operation_option_relations(
         consumed_option_relation(OptionId::NoConfirm),
         OptionId::DryRun)},
    {OperationId::AddSource,
     "cli.source-preference.add",
     one_operand_term(
         OperandKind::SourcePreferenceItem, 1,
         UNBOUNDED_OPERAND_COUNT,
         OperandOrderingRule::
             PackageIntroducesFollowingAssignmentScope),
     TargetPolicy::OrderedItems,
     no_operation_option_relations()},
    {OperationId::DeleteSource,
     "cli.source-preference.delete",
     one_operand_term(OperandKind::Package, 1, UNBOUNDED_OPERAND_COUNT,
                      OperandOrderingRule::PreserveInputOrder),
     TargetPolicy::OneOrMore,
     no_operation_option_relations()},
    {OperationId::Revert,
     "cli.source-preference.revert",
     one_operand_term(OperandKind::Package, 1, UNBOUNDED_OPERAND_COUNT,
                      OperandOrderingRule::PreserveInputOrder),
     TargetPolicy::OneOrMore,
     operation_option_relations(
         pacman_no_confirm_option_relation())},
    {OperationId::EditSource,
     "cli.source-preference.edit",
     one_operand_term(OperandKind::Package, 1, UNBOUNDED_OPERAND_COUNT,
                      OperandOrderingRule::PreserveInputOrder),
     TargetPolicy::OneOrMore,
     no_operation_option_relations()},
    {OperationId::ListSources,
     "cli.source-preference.list",
     no_operands(),
     TargetPolicy::None,
     no_operation_option_relations()},
}};

struct OperationMetadata {
    OperationId id;
    std::string_view canonical_token;
    TokenAliasSet aliases;
    GrammarOwnership owner;
    OperationSemanticScope semantic_scope;
    DryRunSupport dry_run_support;
    std::size_t first_form;
    std::size_t form_count;
    std::string_view exit_contract_identity;
    std::string_view related_contract_identity;
};

inline constexpr std::array<OperationMetadata,
                            static_cast<std::size_t>(OperationId::Count)>
    MOGUET_OPERATION_METADATA = {{
        {OperationId::Build,
         operation_spec(OperationId::Build).token, no_token_aliases(),
         GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SourceBuild,
         DryRunSupport::Supported, 0, 2, "exit.mutation",
         "cli.build"},
        {OperationId::Upgrade,
         operation_spec(OperationId::Upgrade).token,
         no_token_aliases(), GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SystemAndRegisteredSourceUpgrade,
         DryRunSupport::Supported, 2, 1, "exit.partial-mutation",
         "cli.upgrade"},
        {OperationId::UpgradeAur,
         operation_spec(OperationId::UpgradeAur).token,
         no_token_aliases(), GrammarOwnership::MoguetOwned,
         OperationSemanticScope::AurUpgrade,
         DryRunSupport::Supported, 3, 1, "exit.partial-mutation",
         "cli.upgrade-aur"},
        {OperationId::UpgradeAll,
         operation_spec(OperationId::UpgradeAll).token,
         no_token_aliases(), GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SystemRegisteredAndAurUpgrade,
         DryRunSupport::Supported, 4, 1, "exit.partial-mutation",
         "cli.upgrade-all"},
        {OperationId::Clean,
         operation_spec(OperationId::Clean).token, no_token_aliases(),
         GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SourceMaintenance,
         DryRunSupport::Unsupported, 5, 1, "exit.mutation",
         "cli.clean"},
        {OperationId::Deps,
         operation_spec(OperationId::Deps).token, no_token_aliases(),
         GrammarOwnership::MoguetOwned,
         OperationSemanticScope::DependencyInspection,
         DryRunSupport::Unsupported, 6, 1, "exit.read-only-plan",
         "cli.deps"},
        {OperationId::Plan,
         operation_spec(OperationId::Plan).token, no_token_aliases(),
         GrammarOwnership::MoguetOwned,
         OperationSemanticScope::DependencyInspection,
         DryRunSupport::Unsupported, 7, 1, "exit.read-only-plan",
         "cli.plan"},
        {OperationId::Fetch,
         operation_spec(OperationId::Fetch).token, no_token_aliases(),
         GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SourceFetch,
         DryRunSupport::Supported, 8, 1, "exit.mutation",
         "cli.fetch"},
        {OperationId::AddSource,
         operation_spec(OperationId::AddSource).token,
         no_token_aliases(), GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SourceMaintenance,
         DryRunSupport::Unsupported, 9, 1, "exit.mutation",
         "cli.add-source"},
        {OperationId::DeleteSource,
         operation_spec(OperationId::DeleteSource).token,
         no_token_aliases(), GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SourceMaintenance,
         DryRunSupport::Unsupported, 10, 1, "exit.mutation",
         "cli.delete-source"},
        {OperationId::Revert,
         operation_spec(OperationId::Revert).token,
         no_token_aliases(), GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SourceMaintenance,
         DryRunSupport::Unsupported, 11, 1, "exit.partial-mutation",
         "cli.revert"},
        {OperationId::EditSource,
         operation_spec(OperationId::EditSource).token,
         no_token_aliases(), GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SourceMaintenance,
         DryRunSupport::Unsupported, 12, 1, "exit.mutation",
         "cli.edit-source"},
        {OperationId::ListSources,
         operation_spec(OperationId::ListSources).token,
         no_token_aliases(), GrammarOwnership::MoguetOwned,
         OperationSemanticScope::SourceMaintenance,
         DryRunSupport::Unsupported, 13, 1, "exit.read-only-query",
         "cli.list-sources"},
    }};

constexpr const OperationMetadata& operation_metadata(
    OperationId id) noexcept {
    return MOGUET_OPERATION_METADATA[static_cast<std::size_t>(id)];
}

constexpr const OperationFormSpec& operation_form(
    const OperationMetadata& metadata, std::size_t form_index) noexcept {
    return MOGUET_OPERATION_FORMS[metadata.first_form + form_index];
}

enum class SpecialOperationId {
    Help,
    Version,
    PkgbuildExport,
    PkgbuildPrint,
    SyncSelect,
    SystemRepositoryUpdate,
    SystemAurUpdate,
    SystemRepositoryUpdateNoRefresh,
    SystemAurUpdateNoRefresh,
    DelegatedPacmanGrammar,
    Count,
};

enum class DelegatedPacmanTailPolicy {
    None,
    RepositoryOnly,
};

struct SpecialOperationSpec {
    SpecialOperationId id;
    std::string_view canonical_token;
    TokenAliasSet aliases;
    GrammarOwnership owner;
    bool is_open_grammar;
    OperationSemanticScope semantic_scope;
    DryRunSupport dry_run_support;
    OperandContract operands;
    TargetPolicy target_policy;
    OperationOptionRelationSet option_relations;
    std::string_view exit_contract_identity;
    std::string_view related_contract_identity;
    DelegatedPacmanTailPolicy delegated_pacman_tail_policy =
        DelegatedPacmanTailPolicy::None;
};

inline constexpr std::array<SpecialOperationSpec,
                            static_cast<std::size_t>(SpecialOperationId::Count)>
    MOGUET_SPECIAL_OPERATIONS = {{
        {SpecialOperationId::Help, HELP_LONG_OPTION,
         token_alias(HELP_SHORT_OPTION),
         GrammarOwnership::MoguetOwned, false,
         OperationSemanticScope::Information,
         DryRunSupport::NotApplicable, no_operands(),
         TargetPolicy::None,
         operation_option_relation(OptionId::Help),
         "exit.information", "cli.information.help"},
        {SpecialOperationId::Version, VERSION_LONG_OPTION,
         token_alias(VERSION_SHORT_OPTION),
         GrammarOwnership::MoguetOwned, false,
         OperationSemanticScope::Information,
         DryRunSupport::NotApplicable, no_operands(),
         TargetPolicy::None,
         operation_option_relation(OptionId::Version),
         "exit.information", "cli.information.version"},
        {SpecialOperationId::PkgbuildExport,
         PKGBUILD_EXPORT_OPERATION, no_token_aliases(),
         GrammarOwnership::MoguetOwned, false,
         OperationSemanticScope::PackageExport,
         DryRunSupport::Unsupported,
         one_operand_term(OperandKind::Package, 1, 1,
                          OperandOrderingRule::PreserveInputOrder),
         TargetPolicy::ExactlyOne,
         operation_option_relations(
             public_syntax_option_relation(
                 OptionId::PkgbuildOutputDirectory,
                 OptionPublicSyntax::Optional)),
         "exit.mutation",
         "cli.pkgbuild.export"},
        {SpecialOperationId::PkgbuildPrint,
         PKGBUILD_PRINT_OPERATION, no_token_aliases(),
         GrammarOwnership::MoguetOwned, false,
         OperationSemanticScope::PackageExport,
         DryRunSupport::Unsupported,
         one_operand_term(OperandKind::Package, 1, 1,
                          OperandOrderingRule::PreserveInputOrder),
         TargetPolicy::ExactlyOne,
         no_operation_option_relations(), "exit.read-only-query",
         "cli.pkgbuild.print"},
        {SpecialOperationId::SyncSelect, "-S", no_token_aliases(),
         GrammarOwnership::InterceptedPacman, false,
         OperationSemanticScope::RootPackageSelection,
         DryRunSupport::Supported,
         one_operand_term(OperandKind::Query, 1, 1,
                          OperandOrderingRule::PreserveInputOrder),
         TargetPolicy::ExactlyOne,
         operation_option_relations(
             public_syntax_option_relation(
                 consumed_option_relation(
                     OptionId::Select,
                     OptionRelationRequirement::Required),
                 OptionPublicSyntax::Required),
             public_syntax_option_relation(
                 sync_select_needed_option_relation(),
                 OptionPublicSyntax::Optional),
             OptionId::Edit, OptionId::NoEdit,
             OptionId::Diff, OptionId::NoDiff,
             sync_select_no_confirm_option_relation(),
             OptionId::DryRun,
             OptionId::BuildMode, OptionId::Rebuild,
             OptionId::CleanBuild, OptionId::Aur,
             OptionId::Repo),
         "exit.root-selection", "cli.pacman.sync-select"},
        {SpecialOperationId::SystemRepositoryUpdate,
         PACMAN_SYSTEM_UPGRADE_SYNTAX, no_token_aliases(),
         GrammarOwnership::InterceptedPacman, false,
         OperationSemanticScope::RepositorySystemUpgrade,
         DryRunSupport::Supported, no_operands(),
         TargetPolicy::None,
         operation_option_relations(
             public_syntax_option_relation(
                 consumed_option_relation(
                     OptionId::Repo,
                     OptionRelationRequirement::Required),
                 OptionPublicSyntax::Required),
             public_syntax_option_relation(
                 system_aur_needed_option_relation(),
                 OptionPublicSyntax::Optional),
             pacman_no_confirm_option_relation(), OptionId::DryRun),
         "exit.delegated-pacman",
         "cli.pacman.system-repository-update",
         DelegatedPacmanTailPolicy::RepositoryOnly},
        {SpecialOperationId::SystemAurUpdate,
         PACMAN_SYSTEM_UPGRADE_SYNTAX, no_token_aliases(),
         GrammarOwnership::InterceptedPacman, false,
         OperationSemanticScope::SystemAndNormalAurUpgrade,
         DryRunSupport::Supported, no_operands(),
         TargetPolicy::None,
         operation_option_relations(
             OptionId::Edit, OptionId::NoEdit,
             OptionId::Diff, OptionId::NoDiff,
             source_no_confirm_option_relation(), OptionId::DryRun,
             OptionId::BuildMode, OptionId::Rebuild,
             OptionId::CleanBuild,
             public_syntax_option_relation(
                 system_aur_needed_option_relation(),
                 OptionPublicSyntax::Optional)),
         "exit.partial-mutation", "cli.pacman.system-aur-update"},
        {SpecialOperationId::SystemRepositoryUpdateNoRefresh,
         PACMAN_SYSTEM_UPGRADE_NO_REFRESH_SYNTAX, no_token_aliases(),
         GrammarOwnership::InterceptedPacman, false,
         OperationSemanticScope::RepositorySystemUpgrade,
         DryRunSupport::Supported, no_operands(),
         TargetPolicy::None,
         operation_option_relations(
             public_syntax_option_relation(
                 consumed_option_relation(
                     OptionId::Repo,
                     OptionRelationRequirement::Required),
                 OptionPublicSyntax::Required),
             public_syntax_option_relation(
                 system_aur_needed_option_relation(),
                 OptionPublicSyntax::Optional),
             pacman_no_confirm_option_relation(), OptionId::DryRun),
         "exit.delegated-pacman",
         "cli.pacman.system-repository-update",
         DelegatedPacmanTailPolicy::RepositoryOnly},
        {SpecialOperationId::SystemAurUpdateNoRefresh,
         PACMAN_SYSTEM_UPGRADE_NO_REFRESH_SYNTAX, no_token_aliases(),
         GrammarOwnership::InterceptedPacman, false,
         OperationSemanticScope::SystemAndNormalAurUpgrade,
         DryRunSupport::Supported, no_operands(),
         TargetPolicy::None,
         operation_option_relations(
             OptionId::Edit, OptionId::NoEdit,
             OptionId::Diff, OptionId::NoDiff,
             source_no_confirm_option_relation(), OptionId::DryRun,
             OptionId::BuildMode, OptionId::Rebuild,
             OptionId::CleanBuild,
             public_syntax_option_relation(
                 system_aur_needed_option_relation(),
                 OptionPublicSyntax::Optional)),
         "exit.partial-mutation", "cli.pacman.system-aur-update"},
        {SpecialOperationId::DelegatedPacmanGrammar, {}, no_token_aliases(), GrammarOwnership::DelegatedPacman, true, OperationSemanticScope::PacmanDelegation, DryRunSupport::Unsupported, one_operand_term(OperandKind::DelegatedPacmanArgument, 0, UNBOUNDED_OPERAND_COUNT, OperandOrderingRule::Delegated), TargetPolicy::Delegated, operation_option_relations(delegated_needed_option_relation(), delegated_end_of_options_relation(), pacman_no_confirm_option_relation()), "exit.delegated-pacman", "cli.pacman.open-grammar"},
    }};

constexpr const SpecialOperationSpec& special_operation_spec(
    SpecialOperationId id) noexcept {
    return MOGUET_SPECIAL_OPERATIONS[static_cast<std::size_t>(id)];
}

constexpr const SpecialOperationSpec* find_special_operation(
    std::string_view token,
    std::span<const OptionId> present_options) noexcept {
    for(const SpecialOperationSpec& operation : MOGUET_SPECIAL_OPERATIONS) {
        if(operation.is_open_grammar) continue;
        if(token == operation.canonical_token ||
           operation.aliases.contains(token)) {
            bool has_all_required_relations = true;
            for(std::size_t index = 0;
                index < operation.option_relations.count; ++index) {
                const OptionRelationContract& relation =
                    operation.option_relations.values[index];
                if(relation.requirement !=
                   OptionRelationRequirement::Required) {
                    continue;
                }
                bool is_present = false;
                for(OptionId present : present_options) {
                    if(present == relation.option) {
                        is_present = true;
                        break;
                    }
                }
                if(!is_present) {
                    has_all_required_relations = false;
                    break;
                }
            }
            if(!has_all_required_relations) continue;
            return &operation;
        }
    }
    return nullptr;
}

constexpr const SpecialOperationSpec* find_special_operation(
    std::string_view token) noexcept {
    return find_special_operation(token, std::span<const OptionId>{});
}

constexpr bool rich_cli_metadata_is_index_aligned() noexcept {
    for(std::size_t index = 0; index < MOGUET_OPERATION_METADATA.size();
        ++index) {
        if(static_cast<std::size_t>(MOGUET_OPERATION_METADATA[index].id) !=
               index ||
           MOGUET_OPERATION_METADATA[index].canonical_token !=
               MOGUET_OPERATIONS[index].token ||
           MOGUET_OPERATION_METADATA[index].first_form +
                   MOGUET_OPERATION_METADATA[index].form_count >
               MOGUET_OPERATION_FORMS.size()) {
            return false;
        }
        for(std::size_t form_index = 0;
            form_index < MOGUET_OPERATION_METADATA[index].form_count;
            ++form_index) {
            if(operation_form(
                   MOGUET_OPERATION_METADATA[index], form_index)
                   .operation != MOGUET_OPERATION_METADATA[index].id) {
                return false;
            }
        }
    }
    for(std::size_t index = 0; index < MOGUET_OPTION_CONTRACTS.size();
        ++index) {
        if(static_cast<std::size_t>(MOGUET_OPTION_CONTRACTS[index].id) !=
           index) {
            return false;
        }
    }
    for(std::size_t index = 0; index < MOGUET_SPECIAL_OPERATIONS.size();
        ++index) {
        if(static_cast<std::size_t>(MOGUET_SPECIAL_OPERATIONS[index].id) !=
           index) {
            return false;
        }
    }
    return true;
}

static_assert(rich_cli_metadata_is_index_aligned());

} // namespace cli_authority
