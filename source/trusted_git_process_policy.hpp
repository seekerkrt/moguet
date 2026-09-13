#pragma once

#include <string>
#include <vector>

enum class TrustedGitProcessEnvironmentMode {
    ManagedOperation,
    ReadOnlyObservation,
};

// Returns a complete child environment rather than additions to the parent
// environment. Only the fixed Git runtime settings and the existing proxy /
// absolute custom-CA allowlists are admitted. ReadOnlyObservation also sets
// GIT_OPTIONAL_LOCKS=0.
[[nodiscard]] std::vector<std::string> trusted_git_process_environment(
    TrustedGitProcessEnvironmentMode mode);

// Existing managed clone/review operations retain this HTTP-or-HTTPS profile.
// It is intentionally separate from the observer profile below.
[[nodiscard]] std::vector<std::string>
trusted_git_managed_process_arguments();

// Repositoryless observer commands start with this exact HTTPS-only global
// profile. Callers may append only a closed Git subcommand and validated
// operands; raw source metadata is not accepted here.
[[nodiscard]] std::vector<std::string>
trusted_git_observer_process_arguments();

// Same HTTPS-only isolation, bound by the acquisition owner to a fresh repo.
[[nodiscard]] std::vector<std::string>
trusted_git_recipe_acquisition_process_arguments();
