#include "devel_package_assessment.hpp"
#ifdef MOGUET_TEST_AUR_DEVEL_ROUTING
#include "aur_devel_update.hpp"
#include "aur_rpc.hpp"
#include "system_source_upgrade.hpp"
struct UnifiedPlanProjectionTestAccess {
    static SystemSourceUpgradeProjectionAuthority source_view(const SystemSourceUpgradePreparedSnapshot& snapshot,
                                                              const ProductionSourceBuildWorkItem& work, const std::vector<SystemSourceUpgradeIssue>& issues) {
        std::vector<PreparedSystemSourceWorkReference> refs;
        refs.push_back(PreparedSystemSourceWorkReference(snapshot.registered_sources.front(), work));
        return SystemSourceUpgradeProjectionAuthority(snapshot, nullptr, issues, std::move(refs));
    }
};
namespace route_rpc {
std::string version = "1-1";
std::string name = "assessment-git";
unsigned calls = 0;
} // namespace route_rpc
std::map<std::string, AurPackageInfo> AurClient::info_many(const std::vector<std::string>& names) {
    ++route_rpc::calls;
    std::map<std::string, AurPackageInfo> out;
    for(const auto& n : names) {
        AurPackageInfo p;
        p.Name = n;
        p.PackageBase = "assessment";
        p.Version = route_rpc::version;
        out.emplace(n, std::move(p));
    }
    return out;
}
std::optional<AurPackageInfo> AurClient::info_strict(const std::string& n) {
    return info_many({n}).at(n);
}
#endif

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using Stage = DevelPackageAssessmentStage;
using State = DevelUpdateAssessmentState;
using Check = DevelRequiresCheckReason;

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
template <class Arm, class Value>
const Arm& arm(const Value& value) {
    const auto* found = std::get_if<Arm>(&value);
    require(found, "unexpected result arm");
    return *found;
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    require(!output.fail(), "fixture write failed");
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "fixture read failed");
    return {std::istreambuf_iterator<char>(input), {}};
}
void replace(const fs::path& path, const std::string& from, const std::string& to) {
    auto text = read(path);
    const auto pos = text.find(from);
    require(pos != std::string::npos, "fixture replacement absent");
    text.replace(pos, from.size(), to);
    write(path, text);
}
class Environment final {
public:
    Environment(const char* name, const std::string& value) : name_(name) {
        if(const char* old = std::getenv(name)) old_ = old;
        require(setenv(name, value.c_str(), 1) == 0, "setenv failed");
    }
    ~Environment() {
        if(old_)
            static_cast<void>(setenv(name_, old_->c_str(), 1));
        else
            static_cast<void>(unsetenv(name_));
    }

private:
    const char* name_;
    std::optional<std::string> old_;
};
fs::path temporary() {
    char pattern[] = "/tmp/moguet-assessment-XXXXXX";
    require(mkdtemp(pattern), "mkdtemp failed");
    return pattern;
}

struct Fixture {
    fs::path root = temporary();
    Environment home{"HOME", (root / "home").string()};
    Environment state{"XDG_STATE_HOME", (root / "state").string()};
    fs::path db = root / "db";
    fs::path record = db / "local/assessment-git-1-1";
    PackageBaseIdentity base = PackageBaseIdentity::make(
        PackageSourceIdentity::aur(SourceLocationIdentity::known_git_remote("https://aur.archlinux.org/assessment.git")), "assessment");
    PackageChildIdentity child = PackageChildIdentity::make(base, "assessment-git");
    DevelPackageAssessmentTarget target{base, {child}, true};
    std::string recipe = std::string(40, 'a');
    std::string oid;
    VcsSourceIdentity source;
    InstalledRecordObservationTestHooks record_hooks;
    unsigned char generation = 0x80;
    unsigned sessions = 0;
    unsigned remote_calls = 0;
    std::map<Stage, unsigned> stages;
    std::function<void(Stage)> before_stage;
    std::function<void()> during_remote;
    std::string remote_mode = "same";

    explicit Fixture(unsigned oid_length = 40, std::string url = "https://example.invalid/upstream.git",
                     VcsSelector selector = VcsSelector::default_head())
        : oid(oid_length, 'b'), source(VcsSourceIdentity::make(VcsKind::Git, std::move(url), std::move(selector))) {
        fs::create_directory(root / "home");
        fs::create_directory(root / "state");
        fs::create_directories(record);
        write(db / "local/ALPM_DB_VERSION", "9\n");
        write(record / "desc", "%NAME%\nassessment-git\n\n%BASE%\nassessment\n\n%VERSION%\n1-1\n\n%ARCH%\nany\n\n%REASON%\n0\n\n");
        write(record / "files", "%FILES%\nusr/share/assessment\n\n");
        write(record / "mtree", "raw-mtree");
        record_hooks.database_path = db.string();
        record_hooks.expected_owner = geteuid();
        record_hooks.statfs = [](int, struct statfs* value) { *value = {}; value->f_type = 0xef53; return 0; };
        record_hooks.name_to_handle = [&](int fd, const char* path, struct file_handle* handle, int* mount, int flags) {
            require(fd >= 0 && path[0] == '\0' && flags == AT_EMPTY_PATH, "generation did not use retained descriptor");
            *mount = 1;
            if(handle->handle_bytes == 0) {
                handle->handle_bytes = 1;
                errno = EOVERFLOW;
                return -1;
            }
            handle->handle_type = 1;
            handle->f_handle[0] = generation;
            return 0;
        };
        record_hooks.event = [&](InstalledRecordObservationTestEvent event) {
            if(event == InstalledRecordObservationTestEvent::BeforeSessionOpen) ++sessions;
        };
        set_installed_record_observation_test_hooks(record_hooks);
        publish_reviewed(recipe);
        publish_provenance();
        sessions = 0;
        set_devel_package_assessment_test_hooks({[&](Stage stage) { ++stages[stage]; if(before_stage) before_stage(stage); },
                                                 [&](const ValidatedGitRemoteRevisionRequest& request) {
                                                     ++remote_calls;
                                                     require(stages[Stage::Provenance] == 1 && stages[Stage::Installed] == 1 && stages[Stage::Reviewed] == 1,
                                                             "remote preceded local authority");
                                                     if(during_remote) during_remote();
                                                     if(remote_mode == "source") {
                                                         const auto other_source = VcsSourceIdentity::make(VcsKind::Git, "https://example.invalid/other.git", VcsSelector::default_head());
                                                         const auto other = ValidatedGitRemoteRevisionRequest::make(
                                                             make_authority_approved_git_source_identity_fixture_for_test(other_source),
                                                             GitRemoteRevisionObservationKey::make(ValidatedHttpsGitRemote::make(other_source.source_location()), ValidatedGitRemoteSelector::default_head()));
                                                         return parse_git_remote_revision_observation(other, 0, oid + "\tHEAD\n");
                                                     }
                                                     std::string output = (remote_mode == "different" ? std::string(oid.size(), '0') : remote_mode == "format" ? std::string(oid.size() == 40 ? 64 : 40, 'b')
                                                                                                                                                               : oid);
                                                     output += "\t";
                                                     output += request.key().selector().kind() == ValidatedGitRemoteSelectorKind::DefaultHead
                                                                   ? "HEAD"
                                                                   : "refs/heads/" + request.key().selector().exact_branch()->name();
                                                     output += '\n';
                                                     std::string executable = "/usr/bin/printf";
                                                     std::vector<std::string> args{"%s", output};
                                                     if(remote_mode == "ref" || remote_mode == "exit") {
                                                         executable = "/bin/sh";
                                                         args = {"-c", remote_mode == "ref" ? "exit 2" : "exit 128"};
                                                     }
                                                     if(remote_mode == "timeout") {
                                                         executable = "/bin/sh";
                                                         args = {"-c", "sleep 5"};
                                                     }
                                                     if(remote_mode == "process") executable = "/no-such-assessment-executable";
                                                     if(remote_mode == "capture") args = {"%s", std::string(GIT_REMOTE_OBSERVER_STDOUT_CAPTURE_LIMIT + 1, 'x')};
                                                     if(remote_mode == "malformed") args = {"%s", "bad\n"};
                                                     if(remote_mode == "ambiguous") args = {"%s", output + output};
                                                     return observe_git_remote_revision_process_fixture_for_test(
                                                         request, executable, args, std::chrono::milliseconds(remote_mode == "timeout" ? 30 : 2000), std::chrono::milliseconds(30));
                                                 }});
    }
    Fixture(const Fixture&) = delete;
    ~Fixture() {
        set_devel_package_assessment_test_hooks({});
        set_installed_record_observation_test_hooks({});
        reset_xdg_generation_store_test_hooks();
        std::error_code error;
        fs::remove_all(root, error);
    }
    void publish_reviewed(const std::string& revision) {
        const auto current = read_reviewed_source_state(base);
        const auto& before = arm<ReviewedSourceStateStoreRead>(current);
        const auto publication = publish_reviewed_source_state(ReviewedSourceState::make(base, SourceRevisionIdentity::git_commit(revision)), before.observed);
        static_cast<void>(arm<ReviewedSourceStateStorePublished>(publication));
    }
    void publish_provenance() {
        const auto current = observe_current_installed_artifact_binding(child);
        const auto binding = arm<CurrentInstalledArtifactBindingObserved>(current).binding();
        const auto reviewed = read_reviewed_source_state(base);
        const auto& r = arm<ReviewedSourceStateStoreRead>(reviewed);
        const auto& loaded = arm<ReviewedSourceStateLoaded>(r.observation);
        const auto proof = prove_actual_built_git_revision(make_makepkg_git_workspace_revision_observation_fixture_for_test(
            UpstreamGitRevision::git_commit(source, oid), UpstreamGitRevision::git_commit(source, oid)));
        const auto value = make_devel_build_provenance(base,
                                                       make_reviewed_source_state_record_binding_fixture_for_test(base, AurRecipeRevision::git_commit(*loaded.state.reviewed_revision().git_commit()),
                                                                                                                  r.observed->generation, ReviewedSourceStateDocumentSha256Digest::make(xdg_generation_store_raw_contents_sha256(r.observed->raw_contents))),
                                                       source, arm<ActualBuiltGitRevision>(proof),
                                                       BuiltPackageArtifactEvidence{ArtifactPackageIdentity{child.package_name(), "1-1", ArtifactPackageBaseIdentity::known(base.package_base()),
                                                                                                            ArtifactPackageArchitectureIdentity::known("any")},
                                                                                    PackageArchiveSha256Digest::make(std::string(64, 'c')), binding.mtree_digest()},
                                                       binding);
        const auto existing = read_devel_build_provenance(base);
        std::optional<DevelBuildProvenanceStoreObservedRecord> predecessor;
        if(const auto* old = std::get_if<DevelBuildProvenanceStoreLoaded>(&existing)) predecessor = old->observed;
        const auto publication = publish_devel_build_provenance(arm<DevelBuildProvenance>(value), predecessor);
        static_cast<void>(arm<DevelBuildProvenanceStorePublished>(publication));
    }
    fs::path p_file() const {
        return devel_build_provenance_store_entry_path(base) / "1.toml";
    }
    fs::path r_file() const {
        return reviewed_source_state_store_entry_path(base) / "1.toml";
    }
    DevelPackageAssessment assess() {
        return assess_current_devel_package(target);
    }
};

void check(const DevelPackageAssessment& result, Check reason) {
    require(result.assessment.state() == State::RequiresCheck && result.assessment.requires_check_reason() &&
                *result.assessment.requires_check_reason() == reason,
            "wrong RequiresCheck reason");
}
void counts(Fixture& fixture, unsigned p, unsigned i, unsigned r, unsigned remote, unsigned post) {
    require(fixture.stages[Stage::Provenance] == p && fixture.stages[Stage::Installed] == i &&
                fixture.stages[Stage::Reviewed] == r && fixture.remote_calls == remote &&
                fixture.stages[Stage::PostProvenance] == post && fixture.stages[Stage::PostInstalled] == post &&
                fixture.stages[Stage::PostReviewed] == post,
            "I/O count/retry mismatch");
}
void positive_matrix() {
    for(unsigned length : {40U, 64U})
        for(bool branch : {false, true})
            for(const std::string mode : {"same", "different", "format", "source"}) {
                Fixture fixture(length, "https://example.invalid/upstream.git", branch ? VcsSelector::branch("exact/branch") : VcsSelector::default_head());
                fixture.remote_mode = mode;
                const auto result = fixture.assess();
                counts(fixture, 1, 1, 1, 1, 1);
                require(fixture.sessions == 2 && result.post_check == DevelPackagePostCheck::Validated, "post-check did not use fresh session");
                if(mode == "same" || mode == "different") {
                    require(result.assessment.state() == (mode == "same" ? State::UpToDate : State::UpdateAvailable), "wrong positive");
                    require((mode == "different") == result.update_basis.has_value(), "Git update basis lost");
                } else {
                    check(result, Check::SourceIdentityChanged);
                    require(result.revision_comparison == (mode == "format" ? DevelGitRevisionComparison::ObjectFormatMismatch : DevelGitRevisionComparison::SourceMismatch), "comparison detail lost");
                }
                require(branch == result.branch_validation.has_value(), "branch gate skipped");
                std::cout << "S7B revision " << length << '/' << branch << '/' << mode << " P2/I2/R2/remote1 PASS\n";
            }
}

void remote_failures() {
    const std::array<std::pair<const char*, DevelUnknownReason>, 7> cases{{{"ref", DevelUnknownReason::RemoteRefNotFound}, {"timeout", DevelUnknownReason::RemoteObservationTimedOut}, {"process", DevelUnknownReason::RemoteObservationFailed}, {"exit", DevelUnknownReason::RemoteObservationFailed}, {"capture", DevelUnknownReason::RemoteObservationFailed}, {"malformed", DevelUnknownReason::RemoteResultMalformed}, {"ambiguous", DevelUnknownReason::RemoteResultAmbiguous}}};
    for(std::size_t index = 0; index < cases.size(); ++index) {
        Fixture fixture;
        fixture.remote_mode = cases[index].first;
        const auto result = fixture.assess();
        counts(fixture, 1, 1, 1, 1, 0);
        require(result.assessment.state() == State::Unknown && *result.assessment.unknown_reason() == cases[index].second, "remote failure flattened");
        require(result.remote->index() == index + 1, "original remote arm lost");
        require(result.post_check == DevelPackagePostCheck::NotAttempted && fixture.sessions == 1, "failure retried local I/O");
        std::cout << "S7B remote " << cases[index].first << " original-arm/P1/I1/R1/remote1 PASS\n";
    }
}

void local_matrix() {
    for(const std::string mode : {"p-missing", "p-invalid", "p-corrupt", "p-future", "p-unsafe", "p-authority", "p-read", "p-source", "p-base", "p-tag", "p-ssh", "p-arch",
                                  "i-absent", "i-generation", "i-mtree", "i-database", "i-version", "i-arch", "i-base", "i-child", "i-load",
                                  "r-missing", "r-recipe", "r-generation", "r-digest", "r-source", "r-base", "r-invalid", "r-corrupt", "r-future", "r-unsafe", "r-authority", "r-read", "history"}) {
        Fixture fixture;
        const bool p = mode.starts_with("p-");
        const bool installed = mode.starts_with("i-") || mode == "history";
        Check expected = Check::BuildSourceProofUnavailable;
        if(mode == "p-missing" || mode == "r-missing") {
            fs::rename((p ? fixture.p_file() : fixture.r_file()).parent_path(), fixture.root / "detached-store");
            expected = p ? Check::ProvenanceMissing : Check::NoAuthoritativeBuildProvenance;
        }
        if(mode == "p-invalid" || mode == "r-invalid") {
            write(p ? fixture.p_file() : fixture.r_file(), "schema_version = 1\n");
            if(p) expected = Check::ProvenanceInvalid;
        }
        if(mode == "p-corrupt" || mode == "r-corrupt") {
            write(p ? fixture.p_file() : fixture.r_file(), "[");
            if(p) expected = Check::ProvenanceCorrupted;
        }
        if(mode == "p-future" || mode == "r-future") {
            replace(p ? fixture.p_file() : fixture.r_file(), "schema_version = 1", "schema_version = 99");
            if(p) expected = Check::ProvenanceFutureSchema;
        }
        if(mode == "p-unsafe" || mode == "r-unsafe") write((p ? fixture.p_file() : fixture.r_file()).parent_path() / "unexpected", "unsafe");
        if(mode == "p-authority" || mode == "r-authority") {
            fixture.before_stage = [&, p](Stage stage) {
                if(stage == (p ? Stage::Provenance : Stage::Reviewed)) require(setenv("XDG_STATE_HOME", "relative", 1) == 0, "setenv failed");
            };
        }
        if(mode == "p-read" || mode == "r-read") fixture.before_stage = [p](Stage stage) {
            if(stage == (p ? Stage::Provenance : Stage::Reviewed)) fail_next_xdg_generation_store_operation_for_test(XdgGenerationStoreTestFailurePoint::Read);
        };
        if(mode == "p-source" || mode == "r-source") {
            replace(p ? fixture.p_file() : fixture.r_file(), "https://aur.archlinux.org/assessment.git", "https://aur.archlinux.org/other.git");
            expected = Check::SourceIdentityChanged;
        }
        if(mode == "p-base" || mode == "r-base") {
            replace(p ? fixture.p_file() : fixture.r_file(), "package_base = \"assessment\"", "package_base = \"other\"");
            if(p) {
                replace(fixture.p_file(), "artifact_package_base = \"assessment\"", "artifact_package_base = \"other\"");
                replace(fixture.p_file(), "installed_package_base = \"assessment\"", "installed_package_base = \"other\"");
            }
            expected = Check::SourceIdentityChanged;
        }
        if(mode == "p-tag") {
            replace(fixture.p_file(), "default-head", "tag");
            expected = Check::ProvenanceInvalid;
        }
        if(mode == "p-ssh") {
            replace(fixture.p_file(), "https://example.invalid", "ssh://example.invalid");
            expected = Check::ProvenanceInvalid;
        }
        if(mode == "p-arch") {
            replace(fixture.p_file(), "independent", "x86_64");
            expected = Check::ProvenanceInvalid;
        }
        if(installed) expected = Check::InstalledArtifactDrift;
        if(mode == "i-absent") fs::rename(fixture.record, fixture.root / "absent");
        if(mode == "i-generation") fixture.generation = 1;
        if(mode == "i-mtree") write(fixture.record / "mtree", "different mtree");
        if(mode == "i-database") write(fixture.record / "desc", read(fixture.record / "desc") + "\n");
        if(mode == "i-version") {
            replace(fixture.record / "desc", "%VERSION%\n1-1", "%VERSION%\n2-1");
            fs::rename(fixture.record, fixture.db / "local/assessment-git-2-1");
        }
        if(mode == "i-arch") replace(fixture.record / "desc", "%ARCH%\nany", "%ARCH%\nx86_64");
        if(mode == "i-base") {
            replace(fixture.record / "desc", "%BASE%\nassessment", "%BASE%\nother");
            expected = Check::BuildSourceProofUnavailable;
        }
        if(mode == "i-child") {
            replace(fixture.record / "desc", "%NAME%\nassessment-git", "%NAME%\nother-git");
            fs::rename(fixture.record, fixture.db / "local/other-git-1-1");
            fixture.target.installed_children = {PackageChildIdentity::make(fixture.base, "other-git")};
        }
        if(mode == "i-load") {
            fixture.record_hooks.fail_database_load = true;
            set_installed_record_observation_test_hooks(fixture.record_hooks);
            expected = Check::BuildSourceProofUnavailable;
        }
        if(mode == "r-recipe") {
            fixture.publish_reviewed(std::string(40, 'd'));
            expected = Check::AurRecipeAdvanced;
        }
        if(mode == "r-generation") {
            fixture.publish_reviewed(fixture.recipe);
            expected = Check::NoAuthoritativeBuildProvenance;
        }
        if(mode == "r-digest") {
            write(fixture.r_file(), read(fixture.r_file()) + "\n");
            expected = Check::NoAuthoritativeBuildProvenance;
        }
        if(mode == "history") {
            fixture.generation = 1;
            fixture.publish_provenance();
            fixture.generation = 0x80;
            fixture.sessions = 0;
        }
        const auto result = fixture.assess();
        check(result, expected);
        counts(fixture, 1, p ? 0 : 1, p || installed ? 0 : 1, 0, 0);
        if(mode == "p-missing") static_cast<void>(arm<DevelBuildProvenanceStoreMissing>(*result.before.provenance));
        if(mode == "p-invalid") static_cast<void>(arm<DevelBuildProvenanceStoreInvalidDocument>(*result.before.provenance));
        if(mode == "p-corrupt") static_cast<void>(arm<DevelBuildProvenanceStoreCorruptRecord>(*result.before.provenance));
        if(mode == "p-future") static_cast<void>(arm<DevelBuildProvenanceStoreFutureSchema>(*result.before.provenance));
        if(mode == "p-unsafe") static_cast<void>(arm<DevelBuildProvenanceStoreUnsafeHistory>(*result.before.provenance));
        if(mode == "p-authority") static_cast<void>(arm<DevelBuildProvenanceStoreAuthorityUnavailable>(*result.before.provenance));
        if(mode == "p-read") require(arm<DevelBuildProvenanceStoreFailure>(*result.before.provenance).store_failure.kind == XdgGenerationStoreFailureKind::ReadFailed, "P read issue lost");
        if(mode == "p-source") static_cast<void>(arm<DevelBuildProvenanceStoreSourceMismatch>(*result.before.provenance));
        if(mode == "p-base") static_cast<void>(arm<DevelBuildProvenanceStorePackageBaseMismatch>(*result.before.provenance));
        if(mode == "r-unsafe") static_cast<void>(arm<ReviewedSourceStateStoreUnsafeHistory>(*result.before.reviewed));
        if(mode == "r-authority" || mode == "r-read") require(arm<ReviewedSourceStateStoreFailure>(*result.before.reviewed).kind ==
                                                                  (mode == "r-read" ? XdgGenerationStoreFailureKind::ReadFailed : XdgGenerationStoreFailureKind::AuthorityUnavailable),
                                                              "R outer issue lost");
        if(mode == "i-child") require(arm<InstalledArtifactBindingMismatch>(*result.before.installed_comparison).reason == InstalledArtifactBindingMismatchReason::PackageIdentityMismatch, "installed child mismatch lost");
        if(mode == "history") require(arm<DevelBuildProvenanceStoreLoaded>(*result.before.provenance).observed.generation == 2, "adopted old matching history");
        if(mode == "r-generation") require(arm<ReviewedSourceStateRecordBindingMismatch>(*result.before.reviewed_comparison).reason == ReviewedSourceStateRecordBindingMismatchReason::ReviewedStateGenerationMismatch, "generation drift mislabeled");
        if(mode == "r-digest") require(arm<ReviewedSourceStateRecordBindingMismatch>(*result.before.reviewed_comparison).reason == ReviewedSourceStateRecordBindingMismatchReason::ReviewedStateDocumentDigestMismatch, "digest drift mislabeled");
        std::cout << "S7B local " << mode << " remote0 PASS\n";
    }
}

void post_matrix() {
    for(const std::string mode : {"p-tip", "p-missing", "i-generation", "i-replace", "i-mtree", "i-absent", "r-recipe", "r-generation", "r-digest", "r-missing", "r-read"}) {
        Fixture fixture;
        fixture.during_remote = [&] {
            if(mode == "p-tip") {
                const auto read = read_devel_build_provenance(fixture.base);
                const auto& old = arm<DevelBuildProvenanceStoreLoaded>(read);
                const auto pub = publish_devel_build_provenance(old.provenance, old.observed);
                static_cast<void>(arm<DevelBuildProvenanceStorePublished>(pub));
            }
            if(mode == "p-missing") fs::rename(fixture.p_file().parent_path(), fixture.root / "old-p");
            if(mode == "i-generation") fixture.generation = 1;
            if(mode == "i-replace") {
                fs::rename(fixture.record, fixture.root / "old-record");
                fs::create_directory(fixture.record);
                for(const char* leaf : {"desc", "files", "mtree"})
                    write(fixture.record / leaf, read(fixture.root / "old-record" / leaf));
                fixture.generation = 1;
            }
            if(mode == "i-mtree") write(fixture.record / "mtree", "changed during remote");
            if(mode == "i-absent") fs::rename(fixture.record, fixture.root / "old-i");
            if(mode == "r-recipe") fixture.publish_reviewed(std::string(40, 'e'));
            if(mode == "r-generation") fixture.publish_reviewed(fixture.recipe);
            if(mode == "r-digest") write(fixture.r_file(), read(fixture.r_file()) + "\n");
            if(mode == "r-missing") fs::rename(fixture.r_file().parent_path(), fixture.root / "old-r");
        };
        if(mode == "r-read") fixture.before_stage = [](Stage stage) {
            if(stage == Stage::PostReviewed) fail_next_xdg_generation_store_operation_for_test(XdgGenerationStoreTestFailurePoint::Read);
        };
        const auto result = fixture.assess();
        require(result.assessment.state() == State::RequiresCheck && result.post_check == DevelPackagePostCheck::Rejected && !result.revision_comparison,
                "post-remote drift became positive");
        counts(fixture, 1, 1, 1, 1, 1);
        require(result.before.installed && result.after.installed && result.before.reviewed && result.after.reviewed, "post evidence lost");
        if(mode == "p-tip") require(result.issue == DevelPackageAssessmentIssue::ProvenanceTipChanged, "tip change detail lost");
        if(mode.starts_with("i-")) check(result, Check::InstalledArtifactDrift);
        if(mode == "r-recipe") check(result, Check::AurRecipeAdvanced);
        if(mode == "r-generation" || mode == "r-digest" || mode == "r-missing") check(result, Check::NoAuthoritativeBuildProvenance);
        if(mode == "r-read") check(result, Check::BuildSourceProofUnavailable);
        if(mode == "p-missing") check(result, Check::ProvenanceMissing);
        std::cout << "S7B post " << mode << " P2/I2/R2/remote1/no-retry PASS\n";
    }
}

void target_and_request() {
    for(const std::string mode : {"empty", "split", "source", "ordinary", "branch-invalid", "url-invalid", "exception"}) {
        Fixture fixture(40, mode == "url-invalid" ? "https://user@example.invalid/upstream.git" : "https://example.invalid/upstream.git",
                        mode == "branch-invalid" ? VcsSelector::branch("invalid..branch") : VcsSelector::default_head());
        if(mode == "empty") fixture.target.installed_children.clear();
        if(mode == "split") fixture.target.installed_children.push_back(PackageChildIdentity::make(fixture.base, "second"));
        if(mode == "source") fixture.target.package_base = PackageBaseIdentity::make(PackageSourceIdentity::aur(SourceLocationIdentity::known_git_remote("https://aur.archlinux.org/other.git")), "assessment");
        if(mode == "ordinary") {
            fixture.target.known_devel_context = false;
            fixture.target.installed_children = {PackageChildIdentity::make(fixture.base, "ordinary")};
            fs::rename(fixture.p_file().parent_path(), fixture.root / "no-p");
        }
        if(mode == "exception") fixture.before_stage = [](Stage stage) { if(stage == Stage::Remote) throw std::bad_alloc(); };
        if(mode == "exception") {
            bool caught = false;
            try {
                static_cast<void>(fixture.assess());
            } catch(const std::bad_alloc&) {
                caught = true;
            }
            require(caught && fixture.remote_calls == 0, "exception converted to domain success/failure");
        } else {
            const auto result = fixture.assess();
            require(result.assessment.state() == (mode == "ordinary" ? State::NotApplicable : State::RequiresCheck) && fixture.remote_calls == 0, "target/request gate failed");
            if(mode == "branch-invalid") require(std::holds_alternative<InvalidExactGitBranch>(*result.branch_validation), "branch failure detail lost");
            if(mode == "url-invalid") require(result.issue == DevelPackageAssessmentIssue::InvalidHttpsRemote, "URL failure detail lost");
        }
        std::cout << "S7B target " << mode << " remote0 PASS\n";
    }
}

void read_only_snapshot() {
    Fixture fixture;
    std::map<fs::path, std::string> before;
    for(const auto& entry : fs::recursive_directory_iterator(fixture.root))
        before.emplace(entry.path(), entry.is_regular_file() ? read(entry.path()) : "directory");
    const auto result = fixture.assess();
    require(result.assessment.state() == State::UpToDate, "read-only positive failed");
    std::map<fs::path, std::string> after;
    for(const auto& entry : fs::recursive_directory_iterator(fixture.root))
        after.emplace(entry.path(), entry.is_regular_file() ? read(entry.path()) : "directory");
    require(before == after, "assessment wrote or created local state");
    const auto copy = result;
    require(copy.assessment == result.assessment && fixture.remote_calls == 1 && fixture.sessions == 2, "snapshot copy performed I/O");
    std::cout << "S7B read-only inventory/bytes and snapshot-copy PASS\n";
}
#ifdef MOGUET_TEST_AUR_DEVEL_ROUTING
void normal_route_matrix() {
    for(const std::string mode : {"same", "different", "sha256", "format", "timeout", "missing", "recipe", "generation", "split", "unknown-base", "ordinary-same", "ordinary-newer", "newer-same", "newer-timeout", "older-different", "registered-different", "registered-same", "db-context"}) {
        Fixture f(mode == "sha256" ? 64 : 40);
        struct Reset {
            ~Reset() {
                set_aur_devel_update_database_paths_for_test(std::nullopt);
            }
        } reset;
        set_aur_devel_update_database_paths_for_test(PacmanDatabasePaths{"/", f.db});
        if(mode == "db-context") set_aur_devel_update_database_paths_for_test(PacmanDatabasePaths{"/", f.root / "different-db"});
        route_rpc::version = mode.starts_with("newer") || mode == "ordinary-newer" ? "2-1" : mode == "older-different" ? "0-1"
                                                                                                                       : "1-1";
        route_rpc::calls = 0;
        std::string name = f.child.package_name();
        if(mode.starts_with("ordinary") || mode == "missing") fs::rename(f.p_file().parent_path(), f.root / "detached-p");
        if(mode.starts_with("ordinary")) {
            name = "ordinary";
            replace(f.record / "desc", "assessment-git", "ordinary");
            fs::rename(f.record, f.db / "local/ordinary-1-1");
        }
        if(mode == "recipe") f.publish_reviewed(std::string(40, 'c'));
        if(mode == "generation") f.generation = 1;
        if(mode == "split" || mode == "unknown-base") {
            const auto sibling = f.db / "local/sibling-1-1";
            fs::create_directory(sibling);
            write(sibling / "desc", "%NAME%\nsibling\n\n%VERSION%\n1-1\n\n%ARCH%\nany\n\n%REASON%\n0\n\n" + (mode == "split" ? std::string("%BASE%\nassessment\n\n") : std::string()));
            write(sibling / "files", "%FILES%\n\n");
            write(sibling / "mtree", "sibling");
        }
        f.remote_mode = mode == "timeout" || mode == "newer-timeout" ? "timeout" : mode == "format"                                                                                     ? "format"
                                                                               : mode == "different" || mode == "sha256" || mode == "older-different" || mode == "registered-different" ? "different"
                                                                                                                                                                                        : "same";
        ForeignPackageInventory supplied_inventory;
        supplied_inventory.push_back(InstalledPackageMetadata{name, "1-1", InstalledPackageReason::Explicit});
        AurUpdateQueryResult q = mode.starts_with("registered") ? query_registered_aur_devel_update(f.base, name) : query_aur_updates_for_foreign_inventory(std::move(supplied_inventory));
        require(q.plan.entries.size() == 1, "normal route lost target");
        const auto& entry = q.plan.entries.front();
        const auto actual = project_aur_update_effective_state(entry);
        const bool version = route_rpc::version == "2-1";
        const bool git = mode == "different" || mode == "sha256" || mode == "older-different" || mode == "registered-different";
        const bool local = mode == "missing" || mode == "recipe" || mode == "generation" || mode == "split" || mode == "unknown-base" || mode == "db-context";
        const auto expected = version || git ? AurUpdateEffectiveState::UpdateAvailable : mode == "timeout"       ? AurUpdateEffectiveState::Unknown
                                                                                      : local || mode == "format" ? AurUpdateEffectiveState::RequiresCheck
                                                                                                                  : AurUpdateEffectiveState::UpToDate;
        require(actual == expected, "normal state/precedence mismatch");
        require(aur_update_basis(entry) == (version ? std::optional{AurUpdateBasis::Version} : git ? std::optional{AurUpdateBasis::GitRevision}
                                                                                                   : std::nullopt),
                "normal update basis flattened");
        const unsigned remote = version || local || mode == "ordinary-same" ? 0 : 1;
        require(f.remote_calls == remote, "normal remote query/retry count");
        require(route_rpc::calls == 1, "normal RPC repeated");
        if(version)
            require(q.devel_observations.empty() && f.stages.empty(), "RPC-newer entered coordinator");
        else
            require(q.devel_observations.size() == 1 && q.devel_observations.front().evidence->assessment == entry.devel_assessment, "normal diagnostic evidence lost");
        std::cout << "S7D query " << mode << " state=" << static_cast<int>(actual) << " remote=" << f.remote_calls << " PASS\n";
    }
}
void registered_observation_parity() {
    for(const std::string mode : {"same", "different", "missing", "unknown", "split", "repo-only", "absent"}) {
        Fixture f;
        struct Reset {
            ~Reset() {
                set_aur_devel_update_database_paths_for_test(std::nullopt);
            }
        } reset;
        set_aur_devel_update_database_paths_for_test(PacmanDatabasePaths{"/", f.db});
        route_rpc::version = "1-1";
        route_rpc::calls = 0;
        f.remote_mode = mode == "different" ? "different" : mode == "unknown" ? "timeout"
                                                                              : "same";
        if(mode == "missing") fs::rename(f.p_file().parent_path(), f.root / "detached-p");
        if(mode == "absent") fs::rename(f.record, f.root / "detached-record");
        if(mode == "split") {
            const auto sibling = f.db / "local/sibling-1-1";
            fs::create_directory(sibling);
            write(sibling / "desc", "%NAME%\nsibling\n\n%BASE%\nassessment\n\n%VERSION%\n1-1\n\n%ARCH%\nany\n\n%REASON%\n0\n\n");
            write(sibling / "files", "%FILES%\n\n");
            write(sibling / "mtree", "sibling");
        }
        SystemSourceUpgradePreparedSnapshot snapshot;
        RegisteredSourcePreferenceSnapshot source;
        source.original_preference_index = 7;
        source.preference_package_name = f.child.package_name();
        source.resolved_package_base = f.base.package_base();
        source.source_kind = mode == "repo-only" ? SourceBuildSourceKind::Repository : SourceBuildSourceKind::Aur;
        snapshot.registered_sources.push_back(source);
        ProductionSourceBuildWorkItem work;
        work.request.package_name = f.child.package_name();
        work.request.checkout_name = f.base.package_base();
        work.request.aur_review_identity = f.base;
        work.request.only_if_updated = true;
        work.required_targets.push_back({f.base.package_base(), f.child.package_name(), DesiredInstallReason::Explicit});
        work.required_target_provenance = RequiredTargetProvenance::AurBuildPlanProjection;
        work.artifact_lifecycle_intent = ArtifactLifecycleIntent::SingularCompatibility;
        const std::vector<SystemSourceUpgradeIssue> issues;
        const auto view = UnifiedPlanProjectionTestAccess::source_view(snapshot, work, issues);
        if(mode == "repo-only" || mode == "absent") {
            const auto result = observe_registered_aur_devel_updates(view);
            require(result.empty() && f.remote_calls == 0 && route_rpc::calls == 0, "unrelated/absent registered source queried Git");
        } else {
            const auto actual = query_registered_aur_devel_update(f.base, f.child.package_name());
            const auto expected = project_aur_update_effective_state(actual.plan.entries.front());
            f.stages.clear();
            f.remote_calls = 0;
            f.sessions = 0;
            route_rpc::calls = 0;
            const auto observed = observe_registered_aur_devel_updates(view);
            require(observed.size() == 1 && observed.front().preference_index == 7, "registered observation lost source attribution");
            require(project_aur_update_effective_state(observed.front().query.plan.entries.front()) == expected &&
                        aur_update_basis(observed.front().query.plan.entries.front()) == aur_update_basis(actual.plan.entries.front()),
                    "registered actual/dry-run state or basis differs");
            const bool blocked = expected == AurUpdateEffectiveState::RequiresCheck || expected == AurUpdateEffectiveState::Unknown;
            require(observed.front().issues.empty() != blocked, "registered blocker mapping differs");
            require(f.remote_calls == (mode == "missing" || mode == "split" ? 0U : 1U) && route_rpc::calls == 1, "registered observation query/retry count");
            if(mode == "unknown") require(observed.front().issues.front().reason == AurUpdateExecutionReason::DevelObservationUnknown, "remote failure became local RequiresCheck");
        }
        std::cout << "S7D registered observation parity " << mode << " / read-only / no execution PASS\n";
    }
}

#endif

} // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    try {
#ifdef MOGUET_TEST_AUR_DEVEL_ROUTING
        if(argc == 2 && std::string(argv[1]) == "--normal-route") {
            normal_route_matrix();
            registered_observation_parity();
            return 0;
        }
#endif
        positive_matrix();
        remote_failures();
        local_matrix();
        post_matrix();
        target_and_request();
        read_only_snapshot();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
