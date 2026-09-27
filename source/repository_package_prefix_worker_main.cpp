#include "package_metadata.hpp"
#include "logging.hpp"
#include "repository_package_prefix_policy.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <variant>

int main(int argc, char* argv[]) {
    using namespace repository_package_prefix;
    Logger::set_diagnostics_to_stderr();
    if(argc != 2 || std::string(argv[1]).size() > PREFIX_BYTE_LIMIT) return 2;
    try {
        const auto configuration = resolve_pacman_repository_configuration(
            CONFIGURATION_CAPTURE_LIMIT);
        auto session = RepositoryPackageMetadataSession::open(configuration);
        const auto result = session.query_package_name_prefix(
            argv[1], CANDIDATE_LIMIT, OUTPUT_BYTE_LIMIT);
        if(std::holds_alternative<PackageMetadataFailure>(result)) {
            std::cerr << "Local sync package prefix query failed.\n";
            return 1;
        }
        // Publish only after the whole snapshot was validated. The supervisor
        // also buffers until exit success, so no partial failure output leaks.
        for(const auto& name : std::get<RepositoryPackagePrefixSnapshot>(result).names)
            std::cout << name << '\n';
        return std::cout ? 0 : 1;
    } catch(const std::exception&) {
        std::cerr << "Local sync package metadata unavailable.\n";
        return 1;
    }
}
