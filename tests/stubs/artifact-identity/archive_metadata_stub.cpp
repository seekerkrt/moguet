#include "artifact_archive_metadata.hpp"

#include "localization.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "shell_words.hpp"

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace artifact_archive_metadata {
namespace {

[[noreturn]] void throw_malformed_artifact_identity() {
    throw std::runtime_error(localization::format_translated_message(
        "{} returned malformed package artifact identity.", "pacman"));
}

bool is_ascii_control(unsigned char character) noexcept {
    return character <= 0x1f || character == 0x7f;
}

ArtifactPackageIdentity parse_artifact_package_identity(
    const std::string& raw_output) {
    if(raw_output.empty()) {
        throw std::runtime_error(localization::format_translated_message(
            "{} returned no package artifact identity.", "pacman"));
    }
    if(raw_output.find('\r') != std::string::npos) {
        throw_malformed_artifact_identity();
    }

    std::string record = raw_output;
    if(record.back() == '\n') record.pop_back();
    if(record.empty() || record.find('\n') != std::string::npos) {
        throw_malformed_artifact_identity();
    }

    const std::size_t delimiter = record.find(' ');
    if(delimiter == std::string::npos ||
       record.find(' ', delimiter + 1) != std::string::npos) {
        throw_malformed_artifact_identity();
    }

    std::string package_name = record.substr(0, delimiter);
    std::string full_version = record.substr(delimiter + 1);
    if(package_name.empty() || full_version.empty()) {
        throw_malformed_artifact_identity();
    }
    for(std::size_t index = 0; index < record.size(); ++index) {
        if(index == delimiter) continue;
        const unsigned char character =
            static_cast<unsigned char>(record[index]);
        if(is_ascii_control(character)) throw_malformed_artifact_identity();
    }
    if(!is_valid_package_name(package_name)) {
        throw std::runtime_error(localization::format_translated_message(
            "{} returned an invalid package name for the artifact.",
            "pacman"));
    }

    // The legacy process stub does not invent PackageBase/architecture.
    // Tests requiring complete actual metadata use query_with_libalpm().
    return ArtifactPackageIdentity{
        std::move(package_name), std::move(full_version)};
}

} // namespace

ArtifactPackageIdentity query_with_test_stub(
    const std::filesystem::path& artifact_path) {
    const std::vector<std::string> arguments = {
        "pacman", "-Qp", "--color", "never", "--",
        artifact_path.string()};
    const std::string command = "LC_ALL=C " + shell_words::join(arguments);
    Logger::raw_cmd(command);
    const CapturedCommandResult result =
        capture_command_output_raw(command.c_str());
    if(result.exit_code != 0) {
        throw std::runtime_error(localization::format_translated_message(
            "{} failed to read package artifact identity with exit code {}.",
            "pacman", result.exit_code));
    }
    return parse_artifact_package_identity(result.output);
}

#ifndef MOGUET_ENABLE_REAL_RETAINED_DESCRIPTOR_ARCHIVE_QUERY
RetainedDescriptorQueryAuthority::RetainedDescriptorQueryAuthority(
    int descriptor)
    : descriptor_(descriptor) {
    throw std::logic_error(
        "Slice 4 retained-descriptor archive query reached a legacy stubbed route.");
}

void RetainedDescriptorQueryAuthority::require_validity() const {
    throw std::logic_error(
        "Slice 4 retained-descriptor archive query reached a legacy stubbed route.");
}

std::filesystem::path
RetainedDescriptorQueryAuthority::proc_descriptor_path() const {
    throw std::logic_error(
        "Slice 4 retained-descriptor archive query reached a legacy stubbed route.");
}

ArtifactPackageIdentity query_with_libalpm(
    const RetainedDescriptorQueryAuthority&) {
    throw std::logic_error(
        "Slice 4 retained-descriptor archive query reached a legacy stubbed route.");
}
#endif

} // namespace artifact_archive_metadata
