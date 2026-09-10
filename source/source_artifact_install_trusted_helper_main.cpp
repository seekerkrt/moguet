#include "source_artifact_install_trusted_helper_state.hpp"
#include "source_artifact_install_trusted_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

bool write_stdout(const std::string& output) noexcept {
    std::size_t offset = 0;
    while(offset < output.size()) {
        const ssize_t written = write(
            STDOUT_FILENO, output.data() + offset,
            output.size() - offset);
        if(written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if(written == -1 && errno == EINTR) continue;
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for(int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const SourceArtifactInstallTrustedHelperInvocationResult parsed =
        parse_source_artifact_install_trusted_helper_arguments(arguments);
    const auto* invocation =
        std::get_if<SourceArtifactInstallTrustedHelperInvocation>(&parsed);
    if(invocation == nullptr) {
        std::cerr << "moguet-source-artifact-install-helper: invalid fixed protocol invocation\n";
        return 2;
    }
    if(geteuid() != 0) {
        std::cerr << "moguet-source-artifact-install-helper: root execution is required\n";
        return 1;
    }

    int runtime_fd = open(
        "/run", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(runtime_fd == -1) {
        std::cerr << "moguet-source-artifact-install-helper: unable to open /run: "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    try {
        SourceArtifactInstallTrustedStateStore store =
            SourceArtifactInstallTrustedStateStore::
                open_below_runtime_parent(runtime_fd, 0);
        static_cast<void>(close(runtime_fd));
        runtime_fd = -1;
        switch(invocation->command) {
            case SourceArtifactInstallTrustedHelperCommand::InstallLegacy: {
                // Only canonical /proc/PID/fd/FD is accepted by the parser.
                // Open once, then require write seals, exact size and digests;
                // this never reopens the original workspace pathname. stdin
                // remains the caller's terminal/pipe for pacman's confirmation.
                const int input = open(invocation->legacy_input_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
                if(input < 0) throw std::runtime_error("unable to open sealed legacy artifact input");
                SourceArtifactInstallRootPrepareRequest request{
                    invocation->transaction_token, invocation->package_base,
                    invocation->directive, invocation->needed, invocation->no_confirm,
                    invocation->artifacts, SourceArtifactInstallTrustedPurpose::LegacyArtifactInstall};
                try {
                    const int result = store.install_legacy(request, input);
                    close(input);
                    return result;
                } catch(...) {
                    close(input);
                    throw;
                }
            }
            case SourceArtifactInstallTrustedHelperCommand::Prepare:
            case SourceArtifactInstallTrustedHelperCommand::PrepareExact: {
                SourceArtifactInstallRootPrepareRequest request{
                    invocation->transaction_token,
                    invocation->package_base,
                    invocation->directive,
                    invocation->needed,
                    invocation->no_confirm,
                    invocation->artifacts};
                if(invocation->command == SourceArtifactInstallTrustedHelperCommand::PrepareExact)
                    request.purpose = SourceArtifactInstallTrustedPurpose::ExactInstalledBinding;
                const SourceArtifactInstallRootPrepareResponse response =
                    store.prepare(request, STDIN_FILENO);
                const std::string protocol =
                    serialize_source_artifact_install_root_prepare_response(
                        response, request);
                if(!write_stdout(protocol)) {
                    try {
                        store.abort(invocation->transaction_token);
                    } catch(const std::exception& cleanup_error) {
                        std::cerr
                            << "moguet-source-artifact-install-helper: unable to publish prepare response and exact abort failed: "
                            << cleanup_error.what() << '\n';
                        return 1;
                    }
                    std::cerr << "moguet-source-artifact-install-helper: unable to publish prepare response; exact state was aborted\n";
                    return 1;
                }
                break;
            }
            case SourceArtifactInstallTrustedHelperCommand::Record:
                store.record(invocation->transaction_token, STDIN_FILENO);
                break;
            case SourceArtifactInstallTrustedHelperCommand::RecordInstall:
                store.record_install(invocation->transaction_token, STDIN_FILENO);
                break;
            case SourceArtifactInstallTrustedHelperCommand::RecordUpgrade:
                store.record_upgrade(invocation->transaction_token, STDIN_FILENO);
                break;
            case SourceArtifactInstallTrustedHelperCommand::ConsumeExact:
                if(!write_stdout(store.consume_exact(invocation->transaction_token))) return 1;
                break;
            case SourceArtifactInstallTrustedHelperCommand::Execute:
                return store.execute(invocation->transaction_token);
            case SourceArtifactInstallTrustedHelperCommand::ExecutionStatus:
                if(!write_stdout(serialize_source_artifact_install_execution_observation(
                       store.execution_status(invocation->transaction_token)))) return 1;
                break;
            case SourceArtifactInstallTrustedHelperCommand::ObserveExecution:
                store.observe_execution(invocation->transaction_token);
                break;
            case SourceArtifactInstallTrustedHelperCommand::Consume: {
                const std::string response =
                    store.consume(invocation->transaction_token);
                if(!write_stdout(response)) {
                    std::cerr << "moguet-source-artifact-install-helper: unable to publish receipt response\n";
                    return 1;
                }
                break;
            }
            case SourceArtifactInstallTrustedHelperCommand::Abort:
                store.abort(invocation->transaction_token);
                break;
        }
    } catch(const SourceArtifactInstallTrustedStateError& error) {
        if(runtime_fd >= 0) static_cast<void>(close(runtime_fd));
        std::cerr << "moguet-source-artifact-install-helper: " << error.what() << '\n';
        if(invocation->command == SourceArtifactInstallTrustedHelperCommand::Prepare ||
           invocation->command == SourceArtifactInstallTrustedHelperCommand::PrepareExact ||
           invocation->command == SourceArtifactInstallTrustedHelperCommand::Consume ||
           invocation->command == SourceArtifactInstallTrustedHelperCommand::ExecutionStatus) {
            static_cast<void>(write_stdout(serialize_source_artifact_install_execution_observation(
                {invocation->transaction_token, false, error.refusal()})));
        }
        return 1;
    } catch(const std::exception& error) {
        if(runtime_fd >= 0) static_cast<void>(close(runtime_fd));
        std::cerr << "moguet-source-artifact-install-helper: "
                  << error.what() << '\n';
        return 1;
    }
    return 0;
}
