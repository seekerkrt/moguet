#pragma once

#include <variant>

class InvocationOwnedSourceBuildContext;
class InvocationOwnedMakepkgEnvironment;
class EvaluatedDevelSourceBuildProof;
struct EvaluatedDevelSourceBuildFailure;

using EvaluatedDevelSourceBuildResult = std::variant<
    EvaluatedDevelSourceBuildProof, EvaluatedDevelSourceBuildFailure>;

// Every header granting this authority friendship must include this complete
// declaration. A narrow consumer cannot define a same-named friend and add a
// raw observation mint. Types stay forward-declared to avoid include cycles.
class EvaluatedDevelSourceBuildAuthority final {
    EvaluatedDevelSourceBuildAuthority() = delete;

    friend EvaluatedDevelSourceBuildResult build_evaluated_devel_source(
        InvocationOwnedSourceBuildContext context,
        InvocationOwnedMakepkgEnvironment environment);

    [[nodiscard]] static EvaluatedDevelSourceBuildResult build(
        InvocationOwnedSourceBuildContext context,
        InvocationOwnedMakepkgEnvironment environment);
};
