#include "local_recipe_candidate.hpp"
#include "process.hpp"
#include "shell_words.hpp"
#include "stubs/local-dependency-plan/query_stub.hpp"
#include "trusted_cache_test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
namespace query = local_dependency_plan_query_stub;

void expect(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
std::string read_file(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path);
    file << bytes;
    file.close();
    expect(bool(file), "fixture write failed");
    expect(::chmod(path.c_str(), 0600) == 0, "fixture chmod failed");
}

const std::string RECIPE = R"(pkgbase=recipe-test
pkgname=('recipe-test' 'recipe-test-extra')
pkgver=1
pkgrel=1
arch=('any')
pkgdesc='before'
depends=()
printf '%s:%s:%s\n' "$PWD" "$pkgdesc" "$RECIPE_PROBE" >> "$RECIPE_LOG"
package_recipe-test() {
    install -Dm644 /dev/null "$pkgdir/usr/share/recipe-test/probe"
    printf '%s:%s' "$pkgdesc" "$RECIPE_PROBE" > "$pkgdir/usr/share/recipe-test/probe"
}
package_recipe-test-extra() {
    install -Dm644 /dev/null "$pkgdir/usr/share/recipe-test/extra"
}
)";
const std::string STALE = "pkgbase = stale-base\n\tpkgver = 9\n\tpkgrel = 9\n\tarch = any\npkgname = stale-child\n";

class Fixture final {
    std::map<std::string, std::optional<std::string>> previous_;

public:
    fs::path root, source, cache, log;
    Fixture() {
        std::string pattern = (fs::temp_directory_path() / "moguet-recipe-test-XXXXXX").string();
        char* path = ::mkdtemp(pattern.data());
        expect(path != nullptr, "mkdtemp failed");
        root = path;
        source = root / "original";
        cache = root / "cache";
        log = root / "evaluations";
        for(const auto& directory : {source, cache, root / "home"}) {
            fs::create_directory(directory);
            fs::permissions(directory, fs::perms::owner_all);
        }
        for(const char* key : {"HOME", "XDG_CACHE_HOME", "PKGDEST", "CARCH", "PATH",
                               "LC_ALL", "SRCDEST", "BUILDDIR", "LOGDEST", "MAKEPKG_CONF"}) {
            const char* value = std::getenv(key);
            previous_[key] = value ? std::optional<std::string>(value) : std::nullopt;
            ::unsetenv(key);
        }
        ::setenv("HOME", (root / "home").c_str(), 1);
        ::setenv("XDG_CACHE_HOME", cache.c_str(), 1);
        ::setenv("PATH", "/usr/bin:/bin", 1);
        ::setenv("LC_ALL", "C", 1);
        write_file(source / "PKGBUILD", RECIPE);
        write_file(source / ".SRCINFO", STALE);
        write_file(source / "payload", "original payload\n");
        write_file(root / "makepkg.conf", R"(CARCH=x86_64
CHOST=x86_64-unknown-linux-gnu
CFLAGS=''
CXXFLAGS=''
CPPFLAGS=''
LDFLAGS=''
LTOFLAGS=''
DEBUG_CFLAGS=''
DEBUG_CXXFLAGS=''
MAKEFLAGS=''
BUILDENV=(!distcc !color !ccache !check !sign)
OPTIONS=(!strip !docs !libtool !staticlibs !emptydirs !zipman !purge !debug !lto !autodeps)
INTEGRITY_CHECK=(sha256)
PKGEXT='.pkg.tar'
SRCEXT='.src.tar'
PACMAN=/usr/bin/pacman
)");
        query::reset_repository_stub();
        query::reset_aur_stub();
    }
    ~Fixture() {
        set_local_source_workspace_test_hook({});
        for(const auto& [key, value] : previous_) {
            if(value)
                ::setenv(key.c_str(), value->c_str(), 1);
            else
                ::unsetenv(key.c_str());
        }
        std::error_code error;
        fs::remove_all(root, error);
    }
    SourceBuildEnvironment environment() const {
        return {{{"MAKEPKG_CONF", (root / "makepkg.conf").string()},
                 {"RECIPE_LOG", log.string()},
                 {"RECIPE_PROBE", "first"},
                 {"RECIPE_PROBE", "effective"},
                 {"RECIPE_EMPTY", ""}}};
    }
    PreparedLocalRecipeBuild prepare(std::vector<LocalRecipePatch> patches) const {
        return prepare_local_recipe_build(
            open_local_source_root(source), prepare_test_trusted_cache_root(),
            environment(), std::move(patches),
            {.no_confirm = true, .rebuild = true, .clean_build = true});
    }
    void require_original() const {
        expect(read_file(source / "PKGBUILD") == RECIPE, "original PKGBUILD changed");
        expect(read_file(source / ".SRCINFO") == STALE, "original SRCINFO changed");
        expect(read_file(source / "payload") == "original payload\n", "original payload changed");
        expect(std::distance(fs::directory_iterator(source), fs::directory_iterator{}) == 3,
               "original tree acquired generated files");
    }
};

// Literal Git patches are inputs, not a fixture implementation of apply.
LocalRecipePatch description_patch(const std::string& before, const std::string& after) {
    return {"diff --git a/PKGBUILD b/PKGBUILD\n--- a/PKGBUILD\n+++ b/PKGBUILD\n"
            "@@ -5,3 +5,3 @@\n arch=('any')\n-pkgdesc='" +
            before +
            "'\n+pkgdesc='" + after + "'\n depends=()\n"};
}
LocalRecipePatch dependency_patch() {
    return {"diff --git a/PKGBUILD b/PKGBUILD\n--- a/PKGBUILD\n+++ b/PKGBUILD\n"
            "@@ -6,3 +6,3 @@\n pkgdesc='before'\n-depends=()\n+depends=('postpatch-dep')\n"
            " printf '%s:%s:%s\\n' \"$PWD\" \"$pkgdesc\" \"$RECIPE_PROBE\" >> \"$RECIPE_LOG\"\n"};
}

void test_order_and_build() {
    Fixture fixture;
    fs::path candidate;
    {
        auto prepared = fixture.prepare({description_patch("before", "first"), description_patch("first", "second")});
        candidate = prepared.modified_candidate().path.parent_path();
        expect(candidate != fixture.source, "candidate aliases original");
        expect(*prepared.source_identity().source().location().value() == fs::canonical(fixture.source).string(),
               "semantic identity became candidate path");
        expect(prepared.source_identity().package_base() == "recipe-test", "stale SRCINFO used as identity");
        expect(prepared.prepatch_candidate().contents == RECIPE, "baseline mismatch");
        expect(prepared.modified_candidate().contents.find("pkgdesc='second'") != std::string::npos,
               "series order lost");
        expect(prepared.metadata().provenance() == LocalSourceBuildMetadataProvenance::EvaluatedPkgbuild,
               "metadata was not evaluated");
        expect(materialize_source_build_environment_assignment_words(prepared.metadata().source_environment(), SourceEnvironmentEmptyValuePolicy::Forward) ==
                   materialize_source_build_environment_assignment_words(fixture.environment(), SourceEnvironmentEmptyValuePolicy::Forward),
               "environment order/empty changed");
        expect(prepared.plan().local_metadata() == prepared.metadata().metadata(), "plan used other metadata");
        expect(read_file(fixture.log) == candidate.string() + ":before:effective\n" +
                                             candidate.string() + ":second:effective\n",
               "evaluations did not use the same candidate/environment");
        auto moved = std::move(prepared);
        auto result = execute_local_recipe_build(std::move(moved));
        expect(result.selected_artifacts().size() == 2, "split artifacts lost");
        expect(!fs::exists(candidate), "successful source cleanup failed");
        bool observed_archive = false;
        for(const auto& entry : fs::recursive_directory_iterator(fixture.cache)) {
            if(entry.path().filename() != "recipe-test-1-1-any.pkg.tar") continue;
            const std::string command = shell_words::join(
                {"/usr/bin/bsdtar", "-xOf", entry.path().string(), "usr/share/recipe-test/probe"});
            const auto actual = capture_command_output_raw(command.c_str());
            expect(actual.exit_code == 0 && actual.output == "second:effective",
                   "artifact did not come from modified candidate/effective environment");
            observed_archive = true;
        }
        expect(observed_archive, "built archive was not observed");
        result.cleanup_artifacts();
    }
    fixture.require_original();
}

void test_postpatch_plan() {
    Fixture fixture;
    query::set_repository_package_response("postpatch-dep", "core");
    fs::path candidate;
    {
        auto prepared = fixture.prepare({dependency_patch()});
        candidate = prepared.modified_candidate().path.parent_path();
        expect(query::repository_query_count(query::RepositoryQueryKind::StrictPackage, "postpatch-dep") > 0,
               "postpatch dependency did not reach planner");
        expect(prepared.plan().local_metadata() == prepared.metadata().metadata(), "plan metadata mismatch");
        const auto& edges = prepared.plan().build_plan().dependency_edges;
        expect(std::any_of(edges.begin(), edges.end(), [](const auto& edge) {
                   return edge.dependency_spec == "postpatch-dep" &&
                          edge.resolved_package_name == "postpatch-dep";
               }),
               "postpatch dependency missing");
        fixture.require_original();
    }
    expect(!fs::exists(candidate), "abandoned prepared candidate was not cleaned");
}

void expect_failure(Fixture& fixture, std::vector<LocalRecipePatch> patches,
                    LocalRecipeCandidatePhase phase, std::vector<LocalRecipePatchOutcome> outcomes,
                    bool cleanup_failure = false) {
    try {
        static_cast<void>(fixture.prepare(std::move(patches)));
    } catch(const LocalRecipeCandidateError& error) {
        expect(error.failure().phase == phase, "wrong failure phase");
        expect(error.failure().patches == outcomes, "partial series outcome lost");
        if(phase == LocalRecipeCandidatePhase::Apply && error.failure().tool_exit_code)
            expect(error.failure().reason == LocalRecipeCandidateFailureReason::ToolFailure,
                   "nonzero tool exit was reclassified as a conflict");
        expect(error.failure().cleanup_failure.has_value() == cleanup_failure, "cleanup result lost");
        if(!cleanup_failure && !error.failure().candidate_path.empty())
            expect(!fs::exists(error.failure().candidate_path), "failed candidate leaked");
        expect(query::repository_query_history().empty() && query::aur_query_history().empty(),
               "failure reached dependency planning");
        fixture.require_original();
        return;
    }
    throw std::runtime_error("candidate unexpectedly succeeded");
}

void test_failures() {
    using Phase = LocalRecipeCandidatePhase;
    using Outcome = LocalRecipePatchOutcome;
    {
        Fixture fixture;
        expect_failure(fixture, {description_patch("before", "first"), description_patch("wrong", "bad"), description_patch("bad", "third")}, Phase::Apply,
                       {Outcome::Applied, Outcome::Failed, Outcome::NotAttempted});
        expect(read_file(fixture.log).find(":third:") == std::string::npos, "later evaluation ran");
    }
    for(const auto& [old_line, new_line] : std::vector<std::pair<std::string, std::string>>{
            {"pkgbase=recipe-test", "pkgbase=other-base"},
            {"pkgname=('recipe-test' 'recipe-test-extra')", "pkgname=('recipe-test-extra' 'recipe-test')"},
            {"pkgname=('recipe-test' 'recipe-test-extra')", "pkgname=('recipe-test')"}}) {
        Fixture fixture;
        const bool base = old_line.starts_with("pkgbase");
        std::string patch = "diff --git a/PKGBUILD b/PKGBUILD\n--- a/PKGBUILD\n+++ b/PKGBUILD\n@@ -1,3 +1,3 @@\n";
        patch += base ? "-" + old_line + "\n+" + new_line + "\n pkgname=('recipe-test' 'recipe-test-extra')\n"
                      : " pkgbase=recipe-test\n-" + old_line + "\n+" + new_line + "\n";
        patch += " pkgver=1\n";
        expect_failure(fixture, {{patch}}, Phase::Identity, {Outcome::Applied});
    }
    for(const bool fail_cleanup : {false, true}) {
        Fixture fixture;
        if(fail_cleanup) set_local_source_workspace_test_hook([](auto event, const fs::path&) {
            if(event == LocalSourceWorkspaceTestEvent::BeforeCleanupRemoval)
                throw std::runtime_error("injected cleanup refusal");
        });
        auto patch = description_patch("before", "after");
        // Valid textual patch, deliberately invalid shell after apply.
        patch.bytes.replace(patch.bytes.find("+pkgdesc='after'"), 16, "+pkgdesc='unterminated");
        expect_failure(fixture, {patch}, Phase::PostpatchMetadata, {Outcome::Applied}, fail_cleanup);
    }
    for(const std::string target : {"other", "../PKGBUILD", "src/PKGBUILD"}) {
        Fixture fixture;
        auto patch = description_patch("before", "after");
        std::size_t pos = 0;
        while((pos = patch.bytes.find("PKGBUILD", pos)) != std::string::npos) {
            patch.bytes.replace(pos, 8, target);
            pos += target.size();
        }
        expect_failure(fixture, {patch}, Phase::Preflight, {Outcome::NotAttempted});
        expect(!fs::exists(fixture.log), "unsupported material evaluated PKGBUILD");
    }
    {
        Fixture fixture;
        auto patch = description_patch("before", "after");
        patch.bytes += "--- a/payload\n+++ b/payload\n@@ -1 +1 @@\n-original payload\n+oops\n";
        expect_failure(fixture, {patch}, Phase::Preflight, {Outcome::NotAttempted});
    }
    for(const auto& bytes : std::vector<std::string>{"", "diff --git a/PKGBUILD b/PKGBUILD\nold mode 100644\nnew mode 100755\n"}) {
        Fixture fixture;
        expect_failure(fixture, {description_patch("before", "first"), {bytes}},
                       Phase::Preflight, {Outcome::NotAttempted, Outcome::NotAttempted});
        expect(!fs::exists(fixture.log), "invalid later entry evaluated PKGBUILD");
        const fs::path managed = fixture.cache / "moguet";
        expect(fs::is_empty(managed), "material preflight created a candidate");
    }
}

void test_changed_candidate_blocks_build() {
    Fixture fixture;
    auto prepared = fixture.prepare({description_patch("before", "after")});
    const fs::path candidate = prepared.modified_candidate().path.parent_path();
    write_file(candidate / "PKGBUILD", RECIPE);
    try {
        static_cast<void>(execute_local_recipe_build(std::move(prepared)));
    } catch(const LocalSourceBuildPhaseError& error) {
        expect(error.phase() == LocalSourceBuildFailurePhase::Preflight, "changed candidate wrong phase");
        expect(!fs::exists(candidate), "changed candidate cleanup failed");
        fixture.require_original();
        return;
    }
    throw std::runtime_error("changed candidate reached build");
}

void test_build_failure_keeps_existing_outcome() {
    Fixture fixture;
    LocalRecipePatch patch{
        "diff --git a/PKGBUILD b/PKGBUILD\n--- a/PKGBUILD\n+++ b/PKGBUILD\n"
        "@@ -9,3 +9,3 @@\n package_recipe-test() {\n"
        "-    install -Dm644 /dev/null \"$pkgdir/usr/share/recipe-test/probe\"\n"
        "+    return 23\n"
        "     printf '%s:%s' \"$pkgdesc\" \"$RECIPE_PROBE\" > \"$pkgdir/usr/share/recipe-test/probe\"\n"};
    auto prepared = fixture.prepare({patch});
    const fs::path candidate = prepared.modified_candidate().path.parent_path();
    try {
        static_cast<void>(execute_local_recipe_build(std::move(prepared)));
    } catch(const LocalSourceBuildPhaseError& error) {
        expect(error.phase() == LocalSourceBuildFailurePhase::Build &&
                   error.build_exit_code().has_value() && *error.build_exit_code() != 0,
               "actual build failure lost phase/status");
        expect(!fs::exists(candidate), "build failure leaked source candidate");
        expect(error.retained_artifact_workspace() != nullptr &&
                   fs::exists(*error.retained_artifact_workspace()),
               "build failure lost diagnostic artifact ownership");
        fixture.require_original();
        return;
    }
    throw std::runtime_error("failing package function produced build success");
}
} // namespace

int main() {
    try {
        test_order_and_build();
        test_postpatch_plan();
        test_failures();
        test_changed_candidate_blocks_build();
        test_build_failure_keeps_existing_outcome();
        std::cout << "local recipe candidate tests passed (real Git/makepkg, postpatch plan, original preservation, failures/cleanup)\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "local recipe candidate test failed: " << error.what() << '\n';
        return 1;
    }
}
