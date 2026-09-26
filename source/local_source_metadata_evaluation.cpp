#include "local_source_metadata_evaluation.hpp"

#include "localization.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "shell_words.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/utsname.h>
#include <utility>
#include <vector>

extern char** environ;

struct LocalSourceMetadataEvaluationAccess final {
    static int directory_descriptor(
        const LocalSourceRoot& source_root) noexcept {
        return source_root.directory_descriptor_;
    }
};

namespace {

constexpr std::size_t MAX_EVALUATED_SRCINFO_BYTES = 8U * 1024U * 1024U;

bool is_valid_architecture(std::string_view architecture) noexcept {
    return !architecture.empty() &&
           std::all_of(
               architecture.begin(), architecture.end(),
               [](unsigned char character) {
                   return (character >= 'A' && character <= 'Z') ||
                          (character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') ||
                          character == '_';
               });
}

std::string require_valid_architecture(std::string architecture) {
    if(!is_valid_architecture(architecture)) {
        throw std::runtime_error(localization::translate_message(
            "The effective local build architecture is invalid."));
    }
    return architecture;
}

std::vector<std::string> inherited_process_environment() {
    std::vector<std::string> environment;
    for(char** current = ::environ;
        current != nullptr && *current != nullptr; ++current) {
        std::string assignment(*current);
        const std::size_t separator = assignment.find('=');
        if(separator == std::string::npos || separator == 0) continue;
        environment.push_back(std::move(assignment));
    }
    return environment;
}

} // namespace

std::string resolve_local_source_effective_architecture(
    const SourceBuildEnvironment& source_environment) {
    for(auto assignment = source_environment.ordered_assignments.rbegin();
        assignment != source_environment.ordered_assignments.rend();
        ++assignment) {
        if(assignment->key == "CARCH") {
            return require_valid_architecture(assignment->value);
        }
    }

    const char* inherited_architecture = std::getenv("CARCH");
    if(inherited_architecture != nullptr) {
        return require_valid_architecture(inherited_architecture);
    }

    struct utsname host{};
    if(::uname(&host) != 0) {
        throw std::runtime_error(localization::translate_message(
            "Failed to determine the effective local build architecture."));
    }
    return require_valid_architecture(host.machine);
}

LocalSourceBuildMetadata evaluate_local_source_metadata(
    const LocalSourceRoot& source_root,
    SourceBuildEnvironment source_environment,
    std::string effective_architecture) {
    return evaluate_recipe_metadata(source_root, std::move(source_environment),
                                    std::move(effective_architecture), SourceEnvironmentEmptyValuePolicy::Forward);
}

LocalSourceBuildMetadata evaluate_recipe_metadata(
    const LocalSourceRoot& source_root, SourceBuildEnvironment source_environment,
    std::string effective_architecture, SourceEnvironmentEmptyValuePolicy empty_policy) {
    require_unclaimed_artifact_pkgdest(source_environment);
    source_root.require_unchanged_identity();

    std::vector<std::string> command_words{"makepkg", "--printsrcinfo"};
    const std::vector<std::string> assignment_words =
        materialize_source_build_environment_assignment_words(
            source_environment,
            empty_policy);
    command_words.insert(
        command_words.end(), assignment_words.begin(),
        assignment_words.end());
    const std::string command =
        serialize_source_build_environment(
            source_environment,
            empty_policy) +
        shell_words::join(command_words);
    Logger::raw_cmd(command);
    ExplicitProcessInvocation invocation{
        "/bin/sh", {"-c", command}, inherited_process_environment(), MAX_EVALUATED_SRCINFO_BYTES, LocalSourceMetadataEvaluationAccess::directory_descriptor(source_root)};
    CapturedCommandResult result =
        capture_explicit_process_output_raw(invocation);
    source_root.require_unchanged_identity();
    if(result.stdout_capture_limit_exceeded) {
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal makepkg option.
            "{} output exceeded the local metadata size limit.",
            "makepkg --printsrcinfo"));
    }
    if(result.exit_code != 0) {
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: The first placeholder is the literal makepkg
            // option; the second is its exit code.
            "{} failed with exit code {}.",
            "makepkg --printsrcinfo", result.exit_code));
    }

    LocalPackageMetadataParseResult parsed =
        parse_local_package_metadata(result.output);
    if(!parsed.is_success() || parsed.metadata() == nullptr ||
       parsed.failure() != nullptr) {
        const std::size_t line = parsed.failure() == nullptr
                                     ? 0
                                     : parsed.failure()->line;
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: The first placeholder is the literal makepkg
            // option; the second is a line number in generated metadata.
            "{} returned invalid local package metadata at line {}.",
            "makepkg --printsrcinfo", line));
    }

    return bind_evaluated_local_source_metadata(
        source_root, std::move(source_environment),
        std::move(effective_architecture), result.output);
}
