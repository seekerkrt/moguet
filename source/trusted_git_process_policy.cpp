#include "trusted_git_process_policy.hpp"

#include "localization.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace {

constexpr std::array<const char*, 8> TRUSTED_PROXY_ENVIRONMENT_VARIABLES{
    "http_proxy",
    "https_proxy",
    "all_proxy",
    "no_proxy",
    "HTTP_PROXY",
    "HTTPS_PROXY",
    "ALL_PROXY",
    "NO_PROXY",
};

constexpr std::array<const char*, 4> TRUSTED_CA_ENVIRONMENT_VARIABLES{
    "SSL_CERT_FILE",
    "SSL_CERT_DIR",
    "GIT_SSL_CAINFO",
    "GIT_SSL_CAPATH",
};

class TrustedGitEnvironmentError final : public std::runtime_error {
public:
    explicit TrustedGitEnvironmentError(const std::string& variable_name)
        // TRANSLATORS: The placeholders are the Git program identity and an environment variable name. Its value is intentionally not exposed.
        : std::runtime_error(localization::format_translated_message(
              "Refusing a non-absolute custom CA path in the trusted {} environment variable {}.",
              "Git", variable_name)) {
    }
};

bool is_absolute_ssl_cert_directory_list(std::string_view value) {
    // OpenSSL treats SSL_CERT_DIR as a POSIX ':'-separated directory list.
    // Validate each component without changing the raw value passed to envp.
    while(true) {
        const std::size_t separator = value.find(':');
        const std::string_view component = value.substr(0, separator);
        if(component.empty() || !fs::path(component).is_absolute()) {
            return false;
        }
        if(separator == std::string_view::npos) return true;
        value.remove_prefix(separator + 1);
    }
}

} // namespace

std::vector<std::string> trusted_git_process_environment(
    TrustedGitProcessEnvironmentMode mode) {
    std::vector<std::string> environment{
        "PATH=/usr/bin:/bin",
        "LC_ALL=C",
        "LANG=C",
        "GIT_CONFIG_NOSYSTEM=1",
        "GIT_CONFIG_SYSTEM=/dev/null",
        "GIT_CONFIG_GLOBAL=/dev/null",
        "GIT_TERMINAL_PROMPT=0",
        "GIT_ASKPASS=/bin/false",
        "SSH_ASKPASS=/bin/false",
        "GIT_PAGER=cat",
        "PAGER=cat",
        "GIT_ATTR_NOSYSTEM=1",
    };
    if(mode == TrustedGitProcessEnvironmentMode::ReadOnlyObservation) {
        environment.push_back("GIT_OPTIONAL_LOCKS=0");
    }

    // Proxy values are the existing trusted routing exception. They remain
    // opaque, may contain proxy credentials, and do not authorize remote Git
    // credential state from HOME, XDG, helpers, or askpass.
    for(const char* variable_name : TRUSTED_PROXY_ENVIRONMENT_VARIABLES) {
        const char* value = std::getenv(variable_name);
        if(value != nullptr) {
            environment.emplace_back(
                std::string(variable_name) + "=" + value);
        }
    }
    for(const char* variable_name : TRUSTED_CA_ENVIRONMENT_VARIABLES) {
        const char* value = std::getenv(variable_name);
        if(value == nullptr || value[0] == '\0') continue;
        const bool is_absolute =
            std::string_view(variable_name) == "SSL_CERT_DIR"
                ? is_absolute_ssl_cert_directory_list(value)
                : fs::path(value).is_absolute();
        if(!is_absolute) {
            // Do not expose the path value in a user-facing diagnostic.
            throw TrustedGitEnvironmentError(variable_name);
        }
        environment.emplace_back(std::string(variable_name) + "=" + value);
    }
    return environment;
}

std::vector<std::string> trusted_git_managed_process_arguments() {
    return {
        "--no-pager",
        "-c",
        "core.hooksPath=/dev/null",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.sshCommand=/bin/false",
        "-c",
        "credential.helper=",
        "-c",
        "core.askPass=/bin/false",
        "-c",
        "core.pager=cat",
        "-c",
        "pager.diff=false",
        "-c",
        "diff.external=",
        "-c",
        "protocol.allow=never",
        "-c",
        "protocol.http.allow=always",
        "-c",
        "protocol.https.allow=always",
        "-c",
        "submodule.recurse=false",
    };
}

std::vector<std::string> trusted_git_observer_process_arguments() {
    return {
        "--no-pager",
        "--git-dir=/dev/null",
        "-c",
        "core.hooksPath=/dev/null",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.sshCommand=/bin/false",
        "-c",
        "credential.helper=",
        "-c",
        "credential.interactive=false",
        "-c",
        "credential.username=",
        "-c",
        "core.askPass=/bin/false",
        "-c",
        "http.emptyAuth=false",
        "-c",
        "http.proactiveAuth=none",
        "-c",
        "http.delegation=none",
        "-c",
        "http.extraHeader=",
        "-c",
        "http.cookieFile=",
        "-c",
        "http.saveCookies=false",
        "-c",
        "http.followRedirects=false",
        "-c",
        "http.sslVerify=true",
        "-c",
        "protocol.allow=never",
        "-c",
        "protocol.https.allow=always",
        "-c",
        "protocol.http.allow=never",
        "-c",
        "protocol.file.allow=never",
        "-c",
        "protocol.ext.allow=never",
        "-c",
        "protocol.ssh.allow=never",
        "-c",
        "protocol.git.allow=never",
        "-c",
        "submodule.recurse=false",
    };
}

std::vector<std::string> trusted_git_recipe_acquisition_process_arguments() {
    auto arguments = trusted_git_observer_process_arguments();
    // The repositoryless profile has exactly this binding at index 1.
    // All transport/config restrictions remain shared with the observer.
    arguments.erase(arguments.begin() + 1);
    arguments.insert(arguments.end(), {"--no-replace-objects", "-c", "gc.auto=0", "-c", "maintenance.auto=false",
                                       "-c", "fetch.fsckObjects=true", "-c", "transfer.fsckObjects=true"});
    return arguments;
}
