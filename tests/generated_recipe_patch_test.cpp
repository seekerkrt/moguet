#include "generated_recipe_patch.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using Reason = RecipePatchGenerationFailureReason;

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
const std::string BASELINE = "pkgname=recipe-test\npkgver=1\npkgrel=1\narch=('any')\npkgdesc='before'\npackage() {\n    :\n}\n";

AurReviewedSourceReviewIdentity identity() {
    return AurReviewedSourceReviewIdentity::make(
        PackageBaseIdentity::make(PackageSourceIdentity::aur(SourceLocationIdentity::known_git_remote("https://aur.archlinux.org/recipe-test.git")), "recipe-test"),
        SourceRevisionIdentity::git_commit(std::string(40, 'a')));
}
std::string edited(std::string bytes) {
    const auto position = bytes.find("before");
    require(position != std::string::npos, "fixture description missing");
    bytes.replace(position, 6, "after");
    return bytes;
}
std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "probe read failed");
    return {std::istreambuf_iterator<char>(input), {}};
}
void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(bool(output), "probe write failed");
}

class Fixture final {
    std::map<std::string, std::optional<std::string>> previous_;

public:
    fs::path root;
    explicit Fixture(const fs::path& parent = fs::temp_directory_path()) {
        std::string pattern = (parent / "moguet-generated-recipe-test-XXXXXX").string();
        char* created = ::mkdtemp(pattern.data());
        require(created != nullptr, "fixture mkdtemp failed");
        root = created;
    }
    void set(const std::string& key, const std::string& value) {
        if(!previous_.contains(key)) {
            const char* previous = std::getenv(key.c_str());
            previous_[key] = previous ? std::optional<std::string>(previous) : std::nullopt;
        }
        require(::setenv(key.c_str(), value.c_str(), 1) == 0, "fixture setenv failed");
    }
    ~Fixture() {
        for(const auto& [key, value] : previous_) {
            if(value)
                ::setenv(key.c_str(), value->c_str(), 1);
            else
                ::unsetenv(key.c_str());
        }
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

const GeneratedRecipePatch& success(const RecipePatchGenerationResult& result) {
    const auto* patch = std::get_if<GeneratedRecipePatch>(&result);
    require(patch != nullptr, "expected verified generated patch");
    require(patch->identity() == identity(), "source identity lost");
    require(!patch->bytes().empty() && !validate_local_recipe_patch(patch->bytes()), "verified result has invalid shape");
    return *patch;
}
const RecipePatchGenerationFailure& failure(const RecipePatchGenerationResult& result, Reason reason) {
    const auto* failed = std::get_if<RecipePatchGenerationFailure>(&result);
    require(failed && failed->reason == reason, "wrong typed failure");
    return *failed;
}

void verify_independent_replay(const std::string& baseline, const std::string& accepted,
                               const GeneratedRecipePatch& patch) {
    Fixture fixture;
    write_file(fixture.root / "PKGBUILD", baseline);
    auto before = open_local_source_root(fixture.root, true);
    LocalRecipeCandidateFailure replay{LocalRecipeCandidatePhase::Preflight,
                                       LocalRecipeCandidateFailureReason::InvalidMaterial,
                                       {},
                                       std::nullopt,
                                       std::nullopt,
                                       fixture.root};
    auto after = apply_recipe_patch_series(before, {{patch.bytes()}}, replay);
    require(after.pkgbuild().contents == accepted, "independent replay is not byte exact");
}

void supported_edits() {
    const auto one_line = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE));
    const auto& patch = success(one_line);
    require(patch.bytes().starts_with("diff --git a/PKGBUILD b/PKGBUILD\nindex ") &&
                patch.bytes().find("--- a/PKGBUILD\n+++ b/PKGBUILD\n") != std::string::npos &&
                patch.bytes().find("-pkgdesc='before'\n+pkgdesc='after'\n") != std::string::npos,
            "canonical Git unified path/content mismatch");
    verify_independent_replay(BASELINE, edited(BASELINE), patch);

    std::string many = BASELINE;
    for(int i = 0; i < 40; ++i)
        many += "# context " + std::to_string(i) + "\n";
    auto changed = edited(many);
    changed.replace(changed.find("context 35"), 10, "changed 35");
    const auto multiple = generate_recipe_patch_for_test(identity(), many, changed);
    const auto& hunks = success(multiple).bytes();
    require(hunks.find("\n@@ ") != hunks.rfind("\n@@ "), "fixture did not produce multiple hunks");
    for(int i = 0; i < 3; ++i) {
        const auto repeated = generate_recipe_patch_for_test(identity(), many, changed);
        require(success(repeated).bytes() == hunks, "repeat generation changed patch bytes");
    }
    verify_independent_replay(many, changed, success(multiple));

    auto no_newline = BASELINE;
    no_newline.pop_back();
    std::string crlf;
    for(char byte : BASELINE) {
        if(byte == '\n') crlf += '\r';
        crlf += byte;
    }
    for(const auto& [before, after] : std::vector<std::pair<std::string, std::string>>{
            {no_newline, edited(no_newline)}, {no_newline, BASELINE}, {BASELINE, no_newline}, {crlf, edited(crlf)}, {crlf + "# mixed LF\n", edited(crlf + "# mixed LF\n")}}) {
        const auto result = generate_recipe_patch_for_test(identity(), before, after);
        verify_independent_replay(before, after, success(result));
    }
}

void unchanged_and_unsupported() {
    int calls = 0;
    const auto forbidden = [&](const auto&, const auto&) -> BoundedCapturedProcessResult {
        ++calls;
        throw std::runtime_error("unexpected diff call");
    };
    for(const auto& bytes : {BASELINE, std::string{}}) {
        const auto result = generate_recipe_patch_for_test(identity(), bytes, bytes, forbidden);
        require(std::holds_alternative<RecipePatchNoChange>(result), "unchanged became empty patch success");
    }
    failure(generate_recipe_patch_for_test(identity(), BASELINE, BASELINE + std::string("\0payload", 8), forbidden), Reason::UnsupportedEdit);
    require(calls == 0, "unchanged/binary input reached Git");
    for(const auto& [before, after] : std::vector<std::pair<std::string, std::string>>{
            {"before\n", "after\n"}, {"", BASELINE}, {BASELINE, ""}, {BASELINE, "entire replacement\n"}}) {
        failure(generate_recipe_patch_for_test(identity(), before, after), Reason::PatchRejected);
    }
}

void process_policy_and_contamination() {
    Fixture fixture;
    write_file(fixture.root / "gitconfig", "[diff]\n algorithm = histogram\n external = /bin/false\n noprefix = false\n[core]\n autocrlf = true\n attributesFile = " + (fixture.root / "attributes").string() + "\n[color]\n ui = always\n");
    write_file(fixture.root / "attributes", "* diff=poison text eol=crlf\n");
    const auto clean = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE));
    fixture.set("HOME", fixture.root.string());
    fixture.set("XDG_CONFIG_HOME", fixture.root.string());
    fixture.set("GIT_CONFIG_GLOBAL", (fixture.root / "gitconfig").string());
    fixture.set("GIT_CONFIG_SYSTEM", (fixture.root / "gitconfig").string());
    fixture.set("GIT_CONFIG_COUNT", "1");
    fixture.set("GIT_CONFIG_KEY_0", "diff.external");
    fixture.set("GIT_CONFIG_VALUE_0", "/bin/false");
    fixture.set("GIT_EXTERNAL_DIFF", "/bin/false");
    fixture.set("GIT_DIFF_OPTS", "--unified=0");
    fixture.set("GIT_DIR", fixture.root.string());
    fixture.set("LC_ALL", "ja_JP.UTF-8");
    fixture.set("PATH", fixture.root.string());
    fs::path temporary;
    const auto result = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                       [&](const ExplicitProcessInvocation& call, const BoundedProcessPolicy& policy) {
                                                           require(call.executable == "/usr/bin/git" && call.working_directory_fd && call.standard_input_fd, "diff invocation is not explicit");
                                                           require(policy.stdout_capture_limit == LOCAL_RECIPE_PATCH_MAX_BYTES && policy.capture_standard_error && policy.hard_timeout.count() > 0, "diff output/lifetime not bounded");
                                                           require(std::find(call.environment.begin(), call.environment.end(), "LC_ALL=C") != call.environment.end(), "locale authority inherited");
                                                           for(const auto& setting : call.environment)
                                                               require(!setting.starts_with("HOME=") && !setting.starts_with("XDG_") && !setting.starts_with("GIT_CONFIG_COUNT=") && !setting.starts_with("GIT_EXTERNAL_DIFF="), "parent Git authority inherited");
                                                           temporary = fs::canonical("/proc/self/fd/" + std::to_string(*call.working_directory_fd));
                                                           require(temporary.parent_path() == "/ramdisk" || temporary.parent_path() == "/tmp", "unapproved temporary parent");
                                                           require(read_file(temporary / "a/PKGBUILD") == BASELINE && read_file(temporary / "b/PKGBUILD") == edited(BASELINE), "frozen diff bytes changed");
                                                           const auto diff = capture_bounded_explicit_process_output_raw(call, policy);
                                                           require(std::get<BoundedProcessExited>(diff.outcome).exit_code == 1, "Git difference exit 1 lost");
                                                           return diff;
                                                       });
    require(success(result).bytes() == success(clean).bytes(), "parent config/environment changed patch");
    require(!fs::exists(temporary), "invocation temp directory leaked after success");
}

void failures_and_tampering() {
    const auto valid = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE));
    const std::string patch = success(valid).bytes();
    for(const auto outcome : std::vector<BoundedProcessOutcome>{
            BoundedProcessExited{0}, BoundedProcessExited{2}, BoundedProcessExited{127},
            BoundedProcessLaunchOrSetupFailure{BoundedProcessLaunchStage::Execve, ENOENT},
            BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::StandardOutputRead, EIO},
            BoundedProcessTimedOut{}, BoundedProcessSignaled{15}}) {
        const auto result = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                           [&](const auto&, const auto&) { return BoundedCapturedProcessResult{patch, outcome}; });
        require(failure(result, Reason::DiffToolFailure).diff_process.has_value(), "typed tool detail lost");
    }
    failure(generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                           [](const auto&, const auto&) { return BoundedCapturedProcessResult{{}, BoundedProcessExited{1}}; }),
            Reason::DiffToolFailure);
    failure(generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                           [&](const auto&, const auto&) { return BoundedCapturedProcessResult{patch, BoundedProcessExited{1}, 15}; }),
            Reason::DiffToolFailure);

    fs::path temporary;
    const auto truncated = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                          [&](const auto& call, auto policy) {
                                                              temporary = fs::canonical("/proc/self/fd/" + std::to_string(*call.working_directory_fd));
                                                              policy.stdout_capture_limit = 4;
                                                              return capture_bounded_explicit_process_output_raw(call, policy);
                                                          });
    failure(truncated, Reason::DiffOutputLimit);
    require(!fs::exists(temporary), "invocation temp directory leaked after failure");
    failure(generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                           [](const auto&, const auto&) { return BoundedCapturedProcessResult{std::string(LOCAL_RECIPE_PATCH_MAX_BYTES + 1, 'x'), BoundedProcessExited{1}}; }),
            Reason::DiffOutputLimit);
    std::vector<std::string> unsupported{
        patch + "warning: unexpected stderr\n",
        "warning: unexpected stderr\n" + patch,
        patch + patch,
        "diff --git a/PKGBUILD b/PKGBUILD\nold mode 100644\nnew mode 100755\n" + patch.substr(patch.find("index ")),
        "diff --git a/PKGBUILD b/PKGBUILD\nGIT binary patch\nliteral 1\nA\n",
        patch.substr(0, patch.size() - 1),
    };
    auto other_path = patch;
    for(std::size_t position = 0; (position = other_path.find("PKGBUILD", position)) != std::string::npos;) {
        other_path.replace(position, 8, "OTHER");
        position += 5;
    }
    unsupported.push_back(other_path);
    for(const auto& bytes : unsupported) {
        failure(generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                               [&](const auto&, const auto&) { return BoundedCapturedProcessResult{bytes, BoundedProcessExited{1}}; }),
                Reason::PatchRejected);
    }
    auto tampered = patch;
    tampered.replace(tampered.find("+pkgdesc='after'"), 16, "+pkgdesc='other'");
    require(!validate_local_recipe_patch(tampered), "tamper fixture unexpectedly invalid");
    const auto mismatch = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                         [&](const auto&, const auto&) { return BoundedCapturedProcessResult{tampered, BoundedProcessExited{1}}; });
    require(failure(mismatch, Reason::ReproductionMismatch).replay->patches == std::vector{LocalRecipePatchOutcome::Applied}, "exact mismatch lost successful apply detail");
    auto bad_context = patch;
    bad_context.replace(bad_context.find(" arch=('any')"), 13, " arch=('bad')");
    const auto not_applicable = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                               [&](const auto&, const auto&) { return BoundedCapturedProcessResult{bad_context, BoundedProcessExited{1}}; });
    require(failure(not_applicable, Reason::ApplyFailed).replay->tool_exit_code.has_value(), "apply failure flattened");
    const auto missing = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                        [](auto call, const auto& policy) {
                                                            call.executable = "/nonexistent/moguet-test-git";
                                                            return capture_bounded_explicit_process_output_raw(call, policy);
                                                        });
    require(std::holds_alternative<BoundedProcessLaunchOrSetupFailure>(failure(missing, Reason::DiffToolFailure).diff_process->outcome), "actual missing Git not typed");

    std::optional<Fixture> cleanup_fixture;
    fs::path replaced;
    const auto changed_root = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                             [&](const auto& call, const auto& policy) {
                                                                 auto diff = capture_bounded_explicit_process_output_raw(call, policy);
                                                                 replaced = fs::canonical("/proc/self/fd/" + std::to_string(*call.working_directory_fd));
                                                                 // Keep this rename on the producer's filesystem,
                                                                 // independently of the test's ambient TMPDIR.
                                                                 cleanup_fixture.emplace(replaced.parent_path());
                                                                 fs::rename(replaced, cleanup_fixture->root / "retained");
                                                                 fs::create_directory(replaced);
                                                                 write_file(replaced / "sentinel", "foreign replacement\n");
                                                                 return diff;
                                                             });
    const auto& cleanup_failure = failure(changed_root, Reason::TemporaryIoFailure);
    require(cleanup_failure.recipe_snapshot_failure && !cleanup_failure.replay &&
                cleanup_failure.cleanup_error && cleanup_failure.abandoned_directory == replaced &&
                read_file(replaced / "sentinel") == "foreign replacement\n",
            "cleanup retried or deleted a replaced invocation root");
    // This replacement belongs to the test, not the producer's expired root.
    fs::remove_all(replaced);

    const auto missing_recipe = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                               [](const auto& call, const auto& policy) {
                                                                   auto diff = capture_bounded_explicit_process_output_raw(call, policy);
                                                                   const auto root = fs::canonical("/proc/self/fd/" + std::to_string(*call.working_directory_fd));
                                                                   fs::remove(root / "replay/PKGBUILD");
                                                                   return diff;
                                                               });
    const auto& io_failure = failure(missing_recipe, Reason::TemporaryIoFailure);
    require(io_failure.recipe_snapshot_failure && io_failure.recipe_snapshot_failure->code == LocalSourceRootErrorCode::Missing && !io_failure.replay,
            "private recipe I/O failure became invalid apply material");
    const auto baseline_mismatch = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                                  [](const auto& call, const auto& policy) {
                                                                      auto diff = capture_bounded_explicit_process_output_raw(call, policy);
                                                                      const auto root = fs::canonical("/proc/self/fd/" + std::to_string(*call.working_directory_fd));
                                                                      write_file(root / "replay/PKGBUILD", "different baseline\n");
                                                                      return diff;
                                                                  });
    require(!failure(baseline_mismatch, Reason::ReproductionMismatch).replay, "baseline mismatch reached apply");
    const auto internal = generate_recipe_patch_for_test(identity(), BASELINE, edited(BASELINE),
                                                         [](const auto&, const auto&) -> BoundedCapturedProcessResult { throw std::logic_error("injected internal error"); });
    const auto& internal_failure = failure(internal, Reason::InternalFailure);
    require(internal_failure.exception && !internal_failure.diff_process, "unknown exception became a Git execution failure");
    try {
        std::rethrow_exception(internal_failure.exception);
    } catch(const std::logic_error& error) {
        require(std::string(error.what()) == "injected internal error", "internal cause lost");
    }
}
} // namespace

int main() {
    try {
        supported_edits();
        unchanged_and_unsupported();
        process_policy_and_contamination();
        failures_and_tampering();
        std::cout << "generated recipe patch tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
