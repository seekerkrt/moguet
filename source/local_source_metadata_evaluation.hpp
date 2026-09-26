#pragma once

#include "local_source_build.hpp"

#include <string>

// one-off CARCH、inherited CARCH、host architectureの順でinvocation内の
// effective architecture snapshotを固定する。
std::string resolve_local_source_effective_architecture(
    const SourceBuildEnvironment& source_environment);

// Callerがno-default consentを得た後だけ呼ぶmutation-capable metadata
// evaluation boundary。retained root descriptorをchild cwdに使い、stdoutを
// environment/source identityへ束ねる。
LocalSourceBuildMetadata evaluate_local_source_metadata(
    const LocalSourceRoot& source_root,
    SourceBuildEnvironment source_environment,
    std::string effective_architecture);

// Remote recipe candidates retain their existing Omit/Forward environment
// policy. Evaluation does not select or load saved preferences.
LocalSourceBuildMetadata evaluate_recipe_metadata(
    const LocalSourceRoot& source_root, SourceBuildEnvironment source_environment,
    std::string effective_architecture, SourceEnvironmentEmptyValuePolicy empty_policy);
