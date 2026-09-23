#pragma once

#include "root_package_search.hpp"

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <string>
#include <variant>
#include <vector>

enum class RootPackageSelectionCancellationReason {
    EmptyInput,
    CancelToken,
    EndOfInput
};

struct CancelledRootPackageSelection {
    RootPackageSelectionCancellationReason reason;

    bool operator==(const CancelledRootPackageSelection&) const = default;
};

struct MalformedRootPackageSelectionToken {
    std::string token;

    bool operator==(
        const MalformedRootPackageSelectionToken&) const = default;
};

struct RootPackageSelectionIndexOutOfRange {
    std::string token;
    std::size_t candidate_count;

    bool operator==(
        const RootPackageSelectionIndexOutOfRange&) const = default;
};

struct DescendingRootPackageSelectionRange {
    std::string token;

    bool operator==(
        const DescendingRootPackageSelectionRange&) const = default;
};

struct EmptyRootPackageSelectionCommaField {
    bool operator==(
        const EmptyRootPackageSelectionCommaField&) const = default;
};

struct EmptyRootPackageSelectionResult {
    bool operator==(const EmptyRootPackageSelectionResult&) const = default;
};

struct UnknownRootPackageSelectionGroup {
    std::string group_name;

    bool operator==(
        const UnknownRootPackageSelectionGroup&) const = default;
};

struct MixedRootPackageSelectionCancellationToken {
    std::string token;

    bool operator==(
        const MixedRootPackageSelectionCancellationToken&) const =
        default;
};

struct ConflictingRootPackageSelectionAlternatives {
    std::string package_name;
    std::vector<RootPackageIdentity> identities;

    bool operator==(
        const ConflictingRootPackageSelectionAlternatives&) const =
        default;
};

using RootPackageSelectionIssue = std::variant<
    MalformedRootPackageSelectionToken,
    RootPackageSelectionIndexOutOfRange,
    DescendingRootPackageSelectionRange,
    EmptyRootPackageSelectionCommaField,
    EmptyRootPackageSelectionResult,
    UnknownRootPackageSelectionGroup,
    MixedRootPackageSelectionCancellationToken,
    ConflictingRootPackageSelectionAlternatives>;

struct InvalidRootPackageSelection {
    std::vector<RootPackageSelectionIssue> issues;

    bool operator==(const InvalidRootPackageSelection&) const = default;
};

// parserだけが生成し、emptyやalternative-source conflictのないselectionを
// route projectionへ渡す。
class RootPackageSelection final {
public:
    RootPackageSelection(const RootPackageSelection&) = default;
    RootPackageSelection(RootPackageSelection&&) noexcept = default;
    RootPackageSelection& operator=(const RootPackageSelection&) = default;
    RootPackageSelection& operator=(RootPackageSelection&&) noexcept = default;
    ~RootPackageSelection() = default;

    [[nodiscard]] const std::vector<SelectedRootPackageTarget>& targets()
        const noexcept;

    bool operator==(const RootPackageSelection&) const = default;

private:
    explicit RootPackageSelection(
        std::vector<SelectedRootPackageTarget> targets) noexcept;

    std::vector<SelectedRootPackageTarget> targets_;

    friend std::variant<
        RootPackageSelection,
        CancelledRootPackageSelection,
        InvalidRootPackageSelection>
    parse_root_package_selection(
        std::string input,
        const RootPackageSearchSnapshot& snapshot);
};

using RootPackageSelectionExpressionResult = std::variant<
    RootPackageSelection,
    CancelledRootPackageSelection,
    InvalidRootPackageSelection>;

RootPackageSelectionExpressionResult parse_root_package_selection(
    std::string input,
    const RootPackageSearchSnapshot& snapshot);

enum class RootPackageSelectionUnavailableReason {
    NonInteractiveInput,
    NoConfirm,
    NoCandidates
};

struct UnavailableRootPackageSelection {
    RootPackageSelectionUnavailableReason reason;

    bool operator==(const UnavailableRootPackageSelection&) const = default;
};

using RootPackageSelectionSessionResult = std::variant<
    RootPackageSelection,
    CancelledRootPackageSelection,
    UnavailableRootPackageSelection>;

struct PresentRootPackageSelectionCandidates {
    bool operator==(
        const PresentRootPackageSelectionCandidates&) const = default;
};

struct PromptForRootPackageSelection {
    bool operator==(const PromptForRootPackageSelection&) const = default;
};

struct InvalidRootPackageSelectionAttempt {
    InvalidRootPackageSelection selection;

    bool operator==(
        const InvalidRootPackageSelectionAttempt&) const = default;
};

using RootPackageSelectionInteractionEvent = std::variant<
    PresentRootPackageSelectionCandidates,
    PromptForRootPackageSelection,
    InvalidRootPackageSelectionAttempt>;

// Slice 5のlocalized public surfaceへ候補表示、prompt、invalid診断を委ね、
// Slice 4のinteraction ownerはtyped eventと入力状態だけを扱う。
using RootPackageSelectionInteractionCallback = std::function<void(
    const RootPackageSelectionInteractionEvent& event,
    const RootPackageSearchSnapshot& snapshot)>;

enum class RootPackageSelectionInputGate {
    Interactive,
    NonTty,
    NoConfirm
};

// provider selectionのexactly-one choice cacheとは分離し、root固有の
// multiple / range / group expressionを同じcandidate snapshotへ適用する。
class RootPackageSelectionSession final {
public:
    RootPackageSelectionSession(
        std::istream& input,
        RootPackageSelectionInteractionCallback interaction,
        RootPackageSelectionInputGate input_gate);

    RootPackageSelectionSession(const RootPackageSelectionSession&) = delete;
    RootPackageSelectionSession(RootPackageSelectionSession&&) noexcept =
        default;
    RootPackageSelectionSession& operator=(
        const RootPackageSelectionSession&) = delete;
    RootPackageSelectionSession& operator=(
        RootPackageSelectionSession&&) noexcept = default;
    ~RootPackageSelectionSession() = default;

    [[nodiscard]] RootPackageSelectionSessionResult select(
        const RootPackageSearchSnapshot& snapshot);
    [[nodiscard]] bool is_interactive() const noexcept;
    [[nodiscard]] RootPackageSelectionInputGate input_gate() const noexcept;

private:
    std::istream* input_;
    RootPackageSelectionInteractionCallback interaction_;
    RootPackageSelectionInputGate input_gate_;
};

// production factoryもevent wordingを持たず、stdin gateだけを確定する。
RootPackageSelectionSession make_root_package_selection_session(
    RootPackageSelectionInteractionCallback interaction,
    bool no_confirm);
