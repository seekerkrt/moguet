#include "package_identifier.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "repository_package_prefix_policy.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>

namespace {
struct Descriptor {
    int value;
    ~Descriptor() noexcept {
        if(value >= 0) ::close(value);
    }
};

bool is_valid_protocol(const std::string& output) {
    if(!output.empty() && output.back() != '\n') return false;
    std::istringstream stream(output);
    std::string name;
    std::string previous;
    std::size_t count = 0;
    while(std::getline(stream, name)) {
        if(!is_valid_package_name(name) || (!previous.empty() && name <= previous) ||
           ++count > repository_package_prefix::CANDIDATE_LIMIT) return false;
        previous = name;
    }
    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    using namespace repository_package_prefix;
    Logger::set_diagnostics_to_stderr();
    if(argc != 2 || std::string(argv[1]).size() > PREFIX_BYTE_LIMIT) return 2;
    try {
        // The two private executables live together in the existing libexec
        // directory. Resolve our actual location, never PATH/user configuration.
        const auto worker = std::filesystem::read_symlink("/proc/self/exe").parent_path() /
                            "moguet-repository-prefix-worker";
        Descriptor cwd{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        Descriptor input{::open("/dev/null", O_RDONLY | O_CLOEXEC)};
        if(cwd.value < 0 || input.value < 0) return 1;
        ExplicitProcessInvocation invocation{
            worker.string(), {argv[1]}, {"LC_ALL=C"}};
        invocation.working_directory_fd = cwd.value;
        invocation.standard_input_fd = input.value;
        // One outer deadline includes configuration subprocesses, libalpm open,
        // all cache reads and projection. No nested process group is created by
        // the worker's shell-free pacman-conf captures.
        const auto result = capture_bounded_explicit_process_output_raw(
            invocation, BoundedProcessPolicy{
                            QUERY_TIMEOUT, TERMINATION_GRACE, OUTPUT_BYTE_LIMIT});
        const auto* exited = std::get_if<BoundedProcessExited>(&result.outcome);
        if(result.cancellation_signal || exited == nullptr || exited->exit_code != 0 ||
           !is_valid_protocol(result.output)) {
            std::cerr << "Local sync package prefix provider failed or exceeded its budget.\n";
            return 1;
        }
        std::cout << result.output;
        return std::cout ? 0 : 1;
    } catch(const std::exception&) {
        std::cerr << "Local sync package prefix provider unavailable.\n";
        return 1;
    }
}
