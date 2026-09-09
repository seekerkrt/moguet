#include "process.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view PACKAGE_NAME =
    "moguet-devel-phase-fixture";
constexpr std::size_t PROCESS_CAPTURE_LIMIT = 4U * 1024U * 1024U;

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string path_template =
            "/tmp/moguet-makepkg-devel-phase-XXXXXX";
        std::vector<char> writable(
            path_template.begin(), path_template.end());
        writable.push_back('\0');
        char* created = mkdtemp(writable.data());
        if(created == nullptr) {
            throw std::runtime_error(
                "Failed to create makepkg characterization root.");
        }
        path_ = created;
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

class DirectoryDescriptor final {
public:
    explicit DirectoryDescriptor(const fs::path& path)
        : descriptor_(open(
              path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
        if(descriptor_ < 0) {
            throw std::runtime_error(
                "Failed to open fixture working directory: " +
                path.string());
        }
    }

    DirectoryDescriptor(const DirectoryDescriptor&) = delete;
    DirectoryDescriptor& operator=(const DirectoryDescriptor&) = delete;

    ~DirectoryDescriptor() noexcept {
        if(descriptor_ >= 0) {
            static_cast<void>(close(descriptor_));
        }
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

void write_file(const fs::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error(
            "Failed to open fixture file: " + path.string());
    }
    output.write(
        contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if(!output) {
        throw std::runtime_error(
            "Failed to finish fixture file: " + path.string());
    }
}

std::vector<std::string> base_environment(const fs::path& home) {
    return {
        "HOME=" + home.string(),
        "PATH=/usr/bin:/bin",
        "LANG=C",
        "LC_ALL=C",
        "GIT_CONFIG_NOSYSTEM=1",
        "GIT_CONFIG_GLOBAL=/dev/null",
        "GIT_TERMINAL_PROMPT=0",
    };
}

std::vector<std::string> commit_environment(
    const fs::path& home, std::string_view date) {
    std::vector<std::string> environment = base_environment(home);
    environment.insert(
        environment.end(),
        {"GIT_AUTHOR_NAME=Moguet Fixture",
         "GIT_AUTHOR_EMAIL=fixture@example.invalid",
         "GIT_COMMITTER_NAME=Moguet Fixture",
         "GIT_COMMITTER_EMAIL=fixture@example.invalid",
         "GIT_AUTHOR_DATE=" + std::string(date),
         "GIT_COMMITTER_DATE=" + std::string(date)});
    return environment;
}

std::vector<std::string> makepkg_environment(
    const fs::path& home, const fs::path& pkgdest,
    const fs::path& builddir, const fs::path& srcdest,
    const fs::path& root) {
    std::vector<std::string> environment = base_environment(home);
    environment.insert(
        environment.end(),
        {"PKGDEST=" + pkgdest.string(),
         "BUILDDIR=" + builddir.string(),
         "SRCDEST=" + srcdest.string(),
         "SRCPKGDEST=" + (root / "srcpkg").string(),
         "LOGDEST=" + (root / "logs").string(),
         "MAKEPKG_LIBRARY=/usr/share/makepkg"});
    return environment;
}

CapturedCommandResult capture_process(
    const std::string& executable,
    std::vector<std::string> arguments,
    const std::vector<std::string>& environment,
    const fs::path* working_directory = nullptr) {
    std::optional<DirectoryDescriptor> directory;
    if(working_directory != nullptr) {
        directory.emplace(*working_directory);
    }

    ExplicitProcessInvocation invocation{
        executable,
        std::move(arguments),
        environment,
        PROCESS_CAPTURE_LIMIT};
    if(directory.has_value()) {
        invocation.working_directory_fd = directory->get();
    }
    return capture_explicit_process_output_raw(invocation, false);
}

std::string require_successful_output(
    const std::string& context, const std::string& executable,
    std::vector<std::string> arguments,
    const std::vector<std::string>& environment,
    const fs::path* working_directory = nullptr) {
    CapturedCommandResult result = capture_process(
        executable, std::move(arguments), environment,
        working_directory);
    if(result.exit_code != 0 || result.stdout_capture_limit_exceeded) {
        throw std::runtime_error(
            context + " failed with exit code " +
            std::to_string(result.exit_code) + ": " + result.output);
    }
    return result.output;
}

void require_success(
    const std::string& context, const std::string& executable,
    std::vector<std::string> arguments,
    const std::vector<std::string>& environment,
    const fs::path* working_directory = nullptr) {
    static_cast<void>(require_successful_output(
        context, executable, std::move(arguments), environment,
        working_directory));
}

std::string single_output_line(
    std::string output, const std::string& context) {
    require(
        !output.empty() && output.back() == '\n',
        context + " did not end with a newline.");
    output.pop_back();
    require(
        !output.empty() && output.find('\n') == std::string::npos,
        context + " did not return exactly one line.");
    return output;
}

std::string git_output(
    const fs::path& repository,
    std::vector<std::string> arguments,
    const std::vector<std::string>& environment,
    const std::string& context) {
    std::vector<std::string> complete{
        "-C", repository.string()};
    complete.insert(
        complete.end(), arguments.begin(), arguments.end());
    return single_output_line(
        require_successful_output(
            context, "/usr/bin/git", std::move(complete),
            environment),
        context);
}

void run_git(
    const fs::path& repository,
    std::vector<std::string> arguments,
    const std::vector<std::string>& environment,
    const std::string& context) {
    std::vector<std::string> complete{
        "-C", repository.string()};
    complete.insert(
        complete.end(), arguments.begin(), arguments.end());
    require_success(
        context, "/usr/bin/git", std::move(complete), environment);
}

std::string makepkg_packagelist(
    const fs::path& recipe,
    const std::vector<std::string>& environment) {
    return single_output_line(
        require_successful_output(
            "makepkg --packagelist", "/usr/bin/makepkg",
            {"--packagelist"}, environment, &recipe),
        "makepkg --packagelist");
}

std::vector<fs::path> regular_file_inventory(
    const fs::path& directory) {
    std::vector<fs::path> inventory;
    for(const fs::directory_entry& entry :
        fs::directory_iterator(directory)) {
        if(entry.is_regular_file()) inventory.push_back(entry.path());
    }
    std::sort(inventory.begin(), inventory.end());
    return inventory;
}

std::map<std::string, std::string> parse_pkginfo(
    const std::string& contents) {
    std::map<std::string, std::string> metadata;
    std::size_t offset = 0;
    while(offset < contents.size()) {
        const std::size_t end = contents.find('\n', offset);
        const std::string_view line(
            contents.data() + offset,
            (end == std::string::npos ? contents.size() : end) -
                offset);
        const std::size_t separator = line.find(" = ");
        if(separator != std::string_view::npos) {
            const std::string key(line.substr(0, separator));
            if(key == "pkgname" || key == "pkgbase" ||
               key == "pkgver" || key == "arch" ||
               key == "builddate" || key == "packager" ||
               key == "size") {
                const bool inserted = metadata.emplace(
                                                  key, line.substr(separator + 3))
                                          .second;
                require(inserted, "Archive metadata key was duplicated: " + key);
            }
        }
        if(end == std::string::npos) break;
        offset = end + 1;
    }
    return metadata;
}

const std::string& require_metadata(
    const std::map<std::string, std::string>& metadata,
    const std::string& key) {
    const auto found = metadata.find(key);
    require(found != metadata.end(), "Archive metadata is missing: " + key);
    return found->second;
}

std::string sha256_file(
    const fs::path& path,
    const std::vector<std::string>& environment) {
    const std::string output = single_output_line(
        require_successful_output(
            "sha256sum", "/usr/bin/sha256sum", {path.string()},
            environment),
        "sha256sum");
    require(
        output.size() >= 66 && output[64] == ' ',
        "sha256sum output was malformed.");
    const std::string digest = output.substr(0, 64);
    require(
        std::all_of(
            digest.begin(), digest.end(), [](char character) {
                return (character >= '0' && character <= '9') ||
                       (character >= 'a' && character <= 'f');
            }),
        "sha256sum returned a non-canonical digest.");
    return digest;
}

void characterize_makepkg_phases() {
    TemporaryDirectory temporary;
    const fs::path root = temporary.path();
    const fs::path recipe = root / "recipe";
    const fs::path pkgdest = root / "pkgdest";
    const fs::path builddir = root / "build";
    const fs::path srcdest = root / "sources";
    const fs::path home = root / "home";
    const fs::path remote = root / "upstream.git";
    const fs::path upstream_work = root / "upstream-work";
    for(const fs::path& directory :
        {recipe, pkgdest, builddir, srcdest, home}) {
        fs::create_directory(directory);
    }

    const std::vector<std::string> environment = base_environment(home);
    require_success(
        "bare Git remote initialization", "/usr/bin/git",
        {"init", "--bare", "--initial-branch=main", remote.string()},
        environment);
    require_success(
        "upstream worktree initialization", "/usr/bin/git",
        {"init", "--initial-branch=main", upstream_work.string()},
        environment);

    write_file(upstream_work / "payload.txt", "revision-one\n");
    const std::vector<std::string> first_commit_environment =
        commit_environment(home, "2001-01-01T00:00:00+0000");
    run_git(
        upstream_work, {"add", "--", "payload.txt"},
        first_commit_environment, "first Git add");
    run_git(
        upstream_work, {"commit", "-q", "-m", "fixture revision one"},
        first_commit_environment, "first Git commit");
    const std::string first_oid = git_output(
        upstream_work, {"rev-parse", "HEAD"}, environment,
        "first Git OID");
    run_git(
        upstream_work, {"remote", "add", "origin", remote.string()},
        environment, "Git remote setup");
    run_git(
        upstream_work, {"push", "-u", "origin", "main"},
        environment, "first Git push");

    const std::string pkgbuild =
        "pkgname=" + std::string(PACKAGE_NAME) + "\n"
                                                 "pkgver=0\n"
                                                 "options=('!debug')\n"
                                                 "pkgrel=1\n"
                                                 "pkgdesc='Moguet makepkg phase characterization fixture'\n"
                                                 "arch=('any')\n"
                                                 "license=('GPL')\n"
                                                 "source=(\"$pkgname::git+file://" +
        remote.string() + "\")\n"
                          "sha256sums=('SKIP')\n\n"
                          "pkgver() {\n"
                          "    cd \"$srcdir/$pkgname\"\n"
                          "    printf '1.r%s.g%s' \"$(git rev-list --count HEAD)\" \"$(git rev-parse --short=12 HEAD)\"\n"
                          "}\n\n"
                          "package() {\n"
                          "    install -Dm644 \"$srcdir/$pkgname/payload.txt\" \"$pkgdir/usr/share/$pkgname/payload.txt\"\n"
                          "}\n";
    write_file(recipe / "PKGBUILD", pkgbuild);

    const std::vector<std::string> makepkg_env = makepkg_environment(
        home, pkgdest, builddir, srcdest, root);
    const std::string pre_build_packagelist =
        makepkg_packagelist(recipe, makepkg_env);

    write_file(upstream_work / "payload.txt", "revision-two\n");
    const std::vector<std::string> second_commit_environment =
        commit_environment(home, "2001-01-02T00:00:00+0000");
    run_git(
        upstream_work, {"add", "--", "payload.txt"},
        second_commit_environment, "second Git add");
    run_git(
        upstream_work, {"commit", "-q", "-m", "fixture revision two"},
        second_commit_environment, "second Git commit");
    const std::string remote_oid = git_output(
        upstream_work, {"rev-parse", "HEAD"}, environment,
        "second Git OID");
    run_git(
        upstream_work, {"push", "origin", "main"}, environment,
        "second Git push");

    require_success(
        "makepkg --nobuild", "/usr/bin/makepkg",
        {"--nobuild", "--nodeps", "--noconfirm"}, makepkg_env,
        &recipe);

    const fs::path mirror = srcdest / std::string(PACKAGE_NAME);
    const fs::path workspace =
        builddir / std::string(PACKAGE_NAME) / "src" /
        std::string(PACKAGE_NAME);
    require(
        fs::is_directory(mirror),
        "Private SRCDEST Git mirror was not created.");
    require(
        fs::is_directory(workspace),
        "Private BUILDDIR Git workspace was not created.");

    const std::string prepared_oid = git_output(
        workspace, {"rev-parse", "HEAD"}, environment,
        "prepared workspace OID");
    const std::string prepared_remote_oid = git_output(
        workspace, {"rev-parse", "refs/remotes/origin/main"},
        environment, "prepared workspace origin/main");
    const std::string mirror_remote = git_output(
        mirror, {"remote", "get-url", "origin"}, environment,
        "private mirror origin");
    const std::string remote_head = git_output(
        remote, {"symbolic-ref", "HEAD"}, environment,
        "fixture remote HEAD selector");
    const std::string prepared_status =
        require_successful_output(
            "prepared workspace status", "/usr/bin/git",
            {"-C", workspace.string(), "status", "--porcelain=v1"},
            environment);
    const std::string post_preparation_packagelist =
        makepkg_packagelist(recipe, makepkg_env);

    require(
        prepared_oid == remote_oid && prepared_remote_oid == remote_oid,
        "Prepared workspace did not match the selected remote branch.");
    require(
        mirror_remote == "file://" + remote.string(),
        "Private source mirror lost the original local remote.");
    require(
        remote_head == "refs/heads/main",
        "Fixture did not retain the default-HEAD selector relation.");
    require(
        prepared_status.empty(),
        "Prepared makepkg-managed Git workspace was not clean.");
    require(
        regular_file_inventory(pkgdest).empty(),
        "PKGDEST was not fresh before the build phase.");

    require_success(
        "makepkg --noextract", "/usr/bin/makepkg",
        {"--noextract", "--nodeps", "--noconfirm"}, makepkg_env,
        &recipe);

    const std::string post_build_oid = git_output(
        workspace, {"rev-parse", "HEAD"}, environment,
        "post-build workspace OID");
    const std::string post_build_status =
        require_successful_output(
            "post-build workspace status", "/usr/bin/git",
            {"-C", workspace.string(), "status", "--porcelain=v1"},
            environment);
    require(
        post_build_oid == prepared_oid,
        "Post-build Git revision differs from prepared revision.");
    require(
        post_build_status.empty(),
        "Post-build makepkg-managed Git workspace was not clean.");

    const std::vector<fs::path> artifacts =
        regular_file_inventory(pkgdest);
    require(
        artifacts.size() == 1,
        "Fresh PKGDEST inventory was not exactly one artifact.");
    const fs::path& artifact_path = artifacts.front();
    require(
        fs::path(post_preparation_packagelist) == artifact_path,
        "Post-preparation packagelist did not match the actual artifact.");
    require(
        fs::path(pre_build_packagelist).filename() !=
            artifact_path.filename(),
        "Pre-preparation packagelist did not become stale.");

    const std::string pkginfo = require_successful_output(
        "actual archive metadata", "/usr/bin/bsdtar",
        {"-xOf", artifact_path.string(), ".PKGINFO"}, environment);
    const std::map<std::string, std::string> metadata =
        parse_pkginfo(pkginfo);
    const std::string expected_version =
        "1.r2.g" + remote_oid.substr(0, 12) + "-1";
    require(
        require_metadata(metadata, "pkgname") == PACKAGE_NAME &&
            require_metadata(metadata, "pkgbase") == PACKAGE_NAME &&
            require_metadata(metadata, "pkgver") == expected_version &&
            require_metadata(metadata, "arch") == "any",
        "Actual archive identity did not match the dynamic build context.");

    const std::string archive_digest =
        sha256_file(artifact_path, environment);
    const std::string mtree_bytes = require_successful_output(
        "actual archive ALPM-MTREE", "/usr/bin/bsdtar",
        {"-xOf", artifact_path.string(), ".MTREE"}, environment);
    const fs::path mtree_path = root / "actual-archive-mtree";
    write_file(mtree_path, mtree_bytes);
    const std::string mtree_digest = sha256_file(mtree_path, environment);

    std::cout
        << "makepkg characterization evidence\n"
        << "prepared Git OID: " << prepared_oid << '\n'
        << "post-build Git OID: " << post_build_oid << '\n'
        << "selected remote Git OID: " << remote_oid << '\n'
        << "first remote Git OID: " << first_oid << '\n'
        << "remote selector: " << remote_head << '\n'
        << "pre-build packagelist: " << pre_build_packagelist << '\n'
        << "post-preparation packagelist: "
        << post_preparation_packagelist << '\n'
        << "fresh PKGDEST inventory: "
        << artifact_path.filename().string() << '\n'
        << "actual archive metadata: pkgname="
        << require_metadata(metadata, "pkgname")
        << " PackageBase=" << require_metadata(metadata, "pkgbase")
        << " version=" << require_metadata(metadata, "pkgver")
        << " architecture=" << require_metadata(metadata, "arch")
        << " builddate=" << require_metadata(metadata, "builddate")
        << " packager=" << require_metadata(metadata, "packager")
        << " installed-size=" << require_metadata(metadata, "size")
        << '\n'
        << "actual archive SHA-256: " << archive_digest << '\n'
        << "actual archive ALPM-MTREE SHA-256: " << mtree_digest << '\n'
        << "dynamic version: " << expected_version << '\n'
        << "pre-build packagelist stale: yes\n"
        << "makepkg --noextract without -c: PASS\n"
        << "makepkg phase feasibility: PASS\n";
}

} // namespace

int main() {
    try {
        characterize_makepkg_phases();
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "makepkg devel phase characterization failed: "
                  << error.what() << '\n';
        return 1;
    }
}
