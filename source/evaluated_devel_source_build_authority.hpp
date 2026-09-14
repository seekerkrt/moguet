#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <variant>
#include <vector>

class InvocationOwnedSourceBuildContext;
class InvocationOwnedMakepkgEnvironment;
class EvaluatedDevelSourceBuildProof;
class EvaluatedDevelSourceSelection;
struct EvaluatedDevelSourceBuildFailure;
struct EvaluatedDevelSourceSelectionStateData;
enum class EvaluatedDevelSourceBuildStage;
enum class EvaluatedDevelSourceBuildProcess;

using EvaluatedDevelSourceBuildResult = std::variant<
    EvaluatedDevelSourceBuildProof, EvaluatedDevelSourceBuildFailure>;
using EvaluatedDevelSourceSelectionResult = std::variant<
    EvaluatedDevelSourceSelection, EvaluatedDevelSourceBuildFailure>;

// Every header granting this authority friendship must include this complete
// declaration. A narrow consumer cannot define a same-named friend and add a
// raw observation mint. Types stay forward-declared to avoid include cycles.
class EvaluatedDevelSourceBuildAuthority final {
    EvaluatedDevelSourceBuildAuthority() = delete;

    friend class EvaluatedDevelSourceSelection;
    friend EvaluatedDevelSourceSelectionResult select_evaluated_devel_source(
        InvocationOwnedSourceBuildContext context,
        InvocationOwnedMakepkgEnvironment environment);
    friend EvaluatedDevelSourceBuildResult resume_evaluated_devel_source(
        EvaluatedDevelSourceSelection selection);

    // Privileged nested classes must also be complete here: a forward-only
    // declaration would let another TU define members with our friend access.
    // The opaque backing below has no friendship or construction privileges.
    struct SelectionState final {
        SelectionState(InvocationOwnedSourceBuildContext&& context,
                       InvocationOwnedMakepkgEnvironment&& environment);
        ~SelectionState() noexcept;
        void revalidate(EvaluatedDevelSourceBuildStage stage) const;
        std::string run_makepkg(std::vector<std::string> arguments,
                                EvaluatedDevelSourceBuildStage stage,
                                EvaluatedDevelSourceBuildProcess process,
                                std::chrono::milliseconds timeout, std::size_t capture_limit) const;
        void cleanup_after_failure(EvaluatedDevelSourceBuildFailure& failure, bool package_build_started = false);
        void prepare_cleanup() noexcept;
        std::unique_ptr<EvaluatedDevelSourceSelectionStateData> data;
    };
    [[nodiscard]] static EvaluatedDevelSourceSelectionResult select(
        InvocationOwnedSourceBuildContext context,
        InvocationOwnedMakepkgEnvironment environment);
    [[nodiscard]] static EvaluatedDevelSourceBuildResult resume(
        EvaluatedDevelSourceSelection selection);
};
