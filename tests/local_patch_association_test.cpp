#include "local_patch_association.hpp"
#include "trusted_cache_test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using Kind = PatchAssociationFailureKind;
using Point = PatchAssociationTestPoint;

void expect(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}
void write(const fs::path& path, const std::string& bytes, mode_t mode = 0644) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    file.close();
    expect(bool(file), "fixture write failed");
    expect(::chmod(path.c_str(), mode) == 0, "fixture chmod failed");
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(bool(file), "fixture read failed");
    return {std::istreambuf_iterator<char>(file), {}};
}
std::string patch(const std::string& before, const std::string& after) {
    return "diff --git a/PKGBUILD b/PKGBUILD\n--- a/PKGBUILD\n+++ b/PKGBUILD\n"
           "@@ -5,3 +5,3 @@\n arch=('any')\n-pkgdesc='" +
           before + "'\n+pkgdesc='" + after + "'\n package() { :; }\n";
}
const std::string RECIPE = "pkgbase=association-base\npkgname=('association-child')\npkgver=1\npkgrel=1\narch=('any')\npkgdesc='before'\npackage() { :; }\n";

template <typename T, typename Variant>
T take(Variant result) {
    if(auto* failure = std::get_if<PatchAssociationFailure>(&result))
        throw std::runtime_error("unexpected association failure kind=" + std::to_string(static_cast<int>(failure->kind)) + " path=" + failure->path.string());
    expect(std::holds_alternative<T>(result), "wrong result arm");
    return std::get<T>(std::move(result));
}
template <typename Variant>
PatchAssociationFailure expect_failure(Variant result, Kind kind) {
    const auto* failure = std::get_if<PatchAssociationFailure>(&result);
    if(!failure || failure->kind != kind) throw std::runtime_error("expected failure " + std::to_string(static_cast<int>(kind)) + " got " + (failure ? std::to_string(static_cast<int>(failure->kind)) : "success"));
    return *failure;
}
class Fixture final {
    std::map<std::string, std::optional<std::string>> previous_;

public:
    fs::path root, source, material, config, cache;
    Fixture() {
        std::string pattern = (fs::temp_directory_path() / "moguet-patch-association-XXXXXX").string();
        char* created = ::mkdtemp(pattern.data());
        expect(created != nullptr, "mkdtemp failed");
        root = created;
        source = root / "source";
        material = root / "material";
        config = root / "config";
        cache = root / "cache";
        for(const auto& path : {source, material, config, cache, root / "home"}) {
            fs::create_directory(path);
            fs::permissions(path, fs::perms::owner_all);
        }
        ::chmod(material.c_str(), 0755);
        for(const char* key : {"HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "PKGDEST", "CARCH", "PATH", "MAKEPKG_CONF"}) {
            const char* value = std::getenv(key);
            previous_[key] = value ? std::optional<std::string>(value) : std::nullopt;
            ::unsetenv(key);
        }
        ::setenv("HOME", (root / "home").c_str(), 1);
        ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
        ::setenv("XDG_CACHE_HOME", cache.c_str(), 1);
        ::setenv("PATH", "/usr/bin:/bin", 1);
        write(source / "PKGBUILD", RECIPE);
        write(source / ".SRCINFO", "pkgbase = stale\n\tpkgver = 9\n\tpkgrel = 9\n\tarch = any\npkgname = stale\n");
        write(material / "one.patch", patch("before", "first"));
        write(material / "two.patch", patch("first", "second"));
        write(root / "makepkg.conf", "CARCH=x86_64\nCHOST=x86_64-unknown-linux-gnu\nPKGEXT='.pkg.tar'\nSRCEXT='.src.tar'\n");
    }
    ~Fixture() {
        set_patch_association_test_hook({});
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
        return {{{"MAKEPKG_CONF", (root / "makepkg.conf").string()}}};
    }
    ObservedLocalPatchSource observe(const fs::path& path = {}) const {
        return take<ObservedLocalPatchSource>(observe_local_patch_source(
            open_local_source_root(path.empty() ? source : path), prepare_test_trusted_cache_root(), environment()));
    }
    LoadedPatchAssociation registration(const ObservedLocalPatchSource& identity) const {
        return take<LoadedPatchAssociation>(register_local_patch_association(identity, material, {"one.patch", "two.patch"}));
    }
    void original_unchanged() const {
        expect(read(source / "PKGBUILD") == RECIPE, "registration changed original recipe");
        expect(std::distance(fs::directory_iterator(source), fs::directory_iterator{}) == 2, "source acquired generated files");
    }
};

void test_lifecycle() {
    Fixture f;
    auto source = f.observe();
    expect(source.identity().package_base() == "association-base", "stale/child identity adopted");
    expect(*source.identity().source().location().value() == fs::canonical(f.source).string(), "canonical path lost");
    expect(std::holds_alternative<PatchAssociationAbsent>(read_local_patch_association(source.identity())), "initial read not absent");
    expect(!fs::exists(f.config / "moguet"), "absence read created config");
    auto registered = f.registration(source);
    auto loaded = take<LoadedPatchAssociation>(read_local_patch_association(source.identity()));
    expect(loaded.entries() == registered.entries() && loaded.entries()[0].file == "one.patch" && loaded.entries()[1].file == "two.patch", "order/digest not retained");
    expect(loaded.entries()[0].sha256.size() == 64 && loaded.entries()[0].sha256 != loaded.entries()[1].sha256, "digest missing");
    const auto path = local_patch_association_record_path(source.identity());
    struct stat status{};
    expect(::stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600, "record mode incorrect");
    expect(::stat(path.parent_path().c_str(), &status) == 0 && (status.st_mode & 0777) == 0700, "namespace mode incorrect");
    const auto raw = read(path);
    expect(raw.find("before") == std::string::npos, "patch bytes persisted into config");
    expect_failure(register_local_patch_association(source, f.material, {"one.patch"}), Kind::AlreadyExists);
    auto acquired = take<AcquiredLocalRecipeSeries>(acquire_local_patch_series(loaded));
    expect(acquired.patches()[0].bytes == patch("before", "first") && acquired.patches()[1].bytes == patch("first", "second"), "ordered acquisition changed bytes");
    write(f.material / "one.patch", patch("before", "updated"));
    expect_failure(acquire_local_patch_series(loaded), Kind::Changed);
    expect(read(path) == raw, "reader followed new digest automatically");
    auto updated = take<LoadedPatchAssociation>(update_local_patch_association(source, loaded, f.material, {"two.patch", "one.patch"}));
    expect(updated.entries()[0].file == "two.patch" && updated.entries()[1].sha256 != loaded.entries()[0].sha256, "explicit update lost order/digest");
    expect_failure(update_local_patch_association(source, loaded, f.material, {"one.patch"}), Kind::ConcurrentChange);
    expect_failure(acquire_local_patch_series(loaded), Kind::ConcurrentChange);
    static_cast<void>(take<PatchAssociationForgotten>(forget_local_patch_association(updated)));
    expect(std::holds_alternative<PatchAssociationAbsent>(read_local_patch_association(source.identity())), "forget did not remove record");
    expect(read(f.material / "one.patch") == patch("before", "updated") && read(f.material / "two.patch") == patch("first", "second"), "lifecycle changed user material");
    f.original_unchanged();
}

void test_identity_and_strict_record() {
    Fixture f;
    auto source = f.observe();
    auto loaded = f.registration(source);
    const auto path = local_patch_association_record_path(source.identity());
    const auto good = read(path);
    const auto known = [&](std::string root, std::string base) { return PackageBaseIdentity::make(PackageSourceIdentity::local(SourceLocationIdentity::known_local_path(std::move(root))), std::move(base)); };
    for(const auto& identity : {known((f.root / "other").string(), "association-base"), known(f.source.string(), "other-base"), known(f.source.string(), "association-child")}) {
        expect(std::holds_alternative<PatchAssociationAbsent>(read_local_patch_association(identity)), "name-only association lookup matched");
        const auto wrong_path = local_patch_association_record_path(identity);
        write(wrong_path, good, 0600);
        expect_failure(read_local_patch_association(identity), Kind::AssociationMismatch);
        fs::remove(wrong_path);
    }
    const auto unknown = PackageBaseIdentity::make(PackageSourceIdentity::local(SourceLocationIdentity::unknown(SourceLocationKind::LocalPath)), "association-base");
    expect_failure(read_local_patch_association(unknown), Kind::InvalidIdentity);
    for(const auto& bad : std::vector<std::string>{"", good.substr(0, good.size() / 2), good + "\n[unexpected]\nx=1\n", good + "\n[[patches]]\nfile='one.patch'\nsha256='bad'\n", "schema_version=1\nschema_version=1\n"}) {
        write(path, bad, 0600);
        expect_failure(read_local_patch_association(source.identity()), Kind::Corrupt);
    }
    std::string future = good;
    const auto version = future.find("schema_version = 1");
    expect(version != std::string::npos, "schema formatter missing");
    for(const auto& wrong_type : {"true", "false", "1.0", "\"1\""}) {
        auto malformed = good;
        malformed.replace(version, 18, std::string("schema_version = ") + wrong_type);
        write(path, malformed, 0600);
        expect_failure(read_local_patch_association(source.identity()), Kind::Corrupt);
    }
    future.replace(version, 18, "schema_version = 9");
    write(path, future, 0600);
    expect_failure(read_local_patch_association(source.identity()), Kind::Unsupported);
    write(path, good, 0600);
    ::chmod(path.c_str(), 0644);
    expect_failure(read_local_patch_association(source.identity()), Kind::Unsafe);
    ::chmod(path.c_str(), 0600);
    fs::rename(path, path.string() + ".saved");
    fs::create_symlink(f.material / "one.patch", path);
    expect_failure(read_local_patch_association(source.identity()), Kind::Unsafe);
    expect_failure(forget_local_patch_association(loaded), Kind::Unsafe);
    expect(read(f.material / "one.patch") == patch("before", "first"), "unsafe forget touched material");
}

void test_material_failures() {
    Fixture f;
    auto source = f.observe();
    auto loaded = f.registration(source);
    const auto record = read(local_patch_association_record_path(source.identity()));
    fs::rename(f.material / "two.patch", f.material / "saved");
    expect_failure(acquire_local_patch_series(loaded), Kind::Missing);
    expect_failure(update_local_patch_association(source, loaded, f.material, {"one.patch", "two.patch"}), Kind::Missing);
    expect(read(local_patch_association_record_path(source.identity())) == record, "failed material update changed old record");
    fs::create_symlink("saved", f.material / "two.patch");
    expect_failure(acquire_local_patch_series(loaded), Kind::Unsafe);
    fs::remove(f.material / "two.patch");
    fs::rename(f.material / "saved", f.material / "two.patch");
    ::chmod((f.material / "one.patch").c_str(), 0664);
    expect_failure(acquire_local_patch_series(loaded), Kind::Unsafe);
    ::chmod((f.material / "one.patch").c_str(), 0644);
    ::chmod(f.material.c_str(), 0777);
    expect_failure(acquire_local_patch_series(loaded), Kind::Unsafe);
    ::chmod(f.material.c_str(), 0755);
    fail_patch_association_operation_for_test(Point::WrongMaterialOwner);
    expect_failure(acquire_local_patch_series(loaded), Kind::Unsafe);
    fail_patch_association_operation_for_test(Point::PartialRead);
    expect_failure(acquire_local_patch_series(loaded), Kind::IoFailure);
    for(const auto& names : std::vector<std::vector<std::string>>{{}, {"one.patch", "one.patch"}, {"../one.patch"}, {"./one.patch"}, {"/one.patch"}, {"nested/one.patch"}})
        expect_failure(update_local_patch_association(source, loaded, f.material, names), Kind::InvalidMaterial);
    fs::create_hard_link(f.material / "one.patch", f.material / "alias.patch");
    expect_failure(update_local_patch_association(source, loaded, f.material, {"one.patch", "alias.patch"}), Kind::InvalidMaterial);
    write(f.material / "invalid.patch", "not a textual recipe patch\n");
    expect_failure(update_local_patch_association(source, loaded, f.material, {"invalid.patch"}), Kind::InvalidMaterial);
    fs::create_directory_symlink(f.material, f.root / "material-link");
    expect_failure(update_local_patch_association(source, loaded, f.root / "material-link", {"one.patch"}), Kind::Unsafe);
    expect(read(local_patch_association_record_path(source.identity())) == record, "invalid updates changed old record");
    f.original_unchanged();
}

void test_publication() {
    Fixture f;
    auto source = f.observe();
    auto loaded = f.registration(source);
    const auto path = local_patch_association_record_path(source.identity());
    const auto old = read(path);
    write(f.material / "one.patch", patch("before", "explicit-update"));
    for(const auto point : {Point::BeforeWrite, Point::BeforeFileSync, Point::BeforePublication}) {
        fail_patch_association_operation_for_test(point);
        expect_failure(update_local_patch_association(source, loaded, f.material, {"one.patch"}), Kind::IoFailure);
        expect(read(path) == old, "failed publication lost old record");
        static_cast<void>(take<LoadedPatchAssociation>(read_local_patch_association(source.identity())));
        expect(read(f.material / "one.patch") == patch("before", "explicit-update"), "publication wrote material");
    }
    fail_patch_association_operation_for_test(Point::BeforeDirectorySync);
    expect_failure(update_local_patch_association(source, loaded, f.material, {"one.patch"}), Kind::PublicationUncertain);
    auto published = take<LoadedPatchAssociation>(read_local_patch_association(source.identity()));
    expect(published.entries().size() == 1, "uncertain publication rolled back or published partial record");
    fail_patch_association_operation_for_test(Point::BeforePublication);
    expect_failure(forget_local_patch_association(published), Kind::IoFailure);
    static_cast<void>(take<LoadedPatchAssociation>(read_local_patch_association(source.identity())));
    fail_patch_association_operation_for_test(Point::AfterPublication);
    auto uncertain = expect_failure(forget_local_patch_association(published), Kind::PublicationUncertain);
    expect(uncertain.leftover && fs::exists(*uncertain.leftover), "forget lost retained artifact diagnostic");
    expect_failure(read_local_patch_association(source.identity()), Kind::Unsafe);
    expect(read(f.material / "one.patch") == patch("before", "explicit-update"), "forget removed material");
}

void test_races_and_same_bytes() {
    Fixture f;
    auto source = f.observe();
    auto loaded = f.registration(source);
    const auto replace = [&](const fs::path& path) {
        fs::rename(path, path.string() + ".old");
        write(path, patch("before", "replaced"));
    };
    for(const auto point : {Point::AfterMaterialOpen, Point::AfterMaterialRead, Point::AfterSeriesRead}) {
        bool ran = false;
        set_patch_association_test_hook([&](Point at, const fs::path& path) {
            if(!ran && at == point) {
                ran = true;
                replace(at == Point::AfterSeriesRead ? f.material / "one.patch" : path);
            }
        });
        expect_failure(acquire_local_patch_series(loaded), Kind::ConcurrentChange);
        set_patch_association_test_hook({});
        expect(ran, "race hook not reached");
        fs::remove(f.material / "one.patch");
        fs::rename(f.material / "one.patch.old", f.material / "one.patch");
    }
    auto acquired = take<AcquiredLocalRecipeSeries>(acquire_local_patch_series(loaded));
    const auto expected = acquired.identity();
    replace(f.material / "one.patch");
    expect(acquired.patches()[0].bytes == patch("before", "first"), "owned bytes followed later replacement");
    {
        auto candidate = prepare_local_recipe_build(open_local_source_root(f.source), prepare_test_trusted_cache_root(),
                                                    f.environment(), std::move(acquired).take_patches(), {}, {}, expected);
        expect(candidate.modified_candidate().contents.find("pkgdesc='second'") != std::string::npos, "consumer reopened material instead of using verified bytes");
    }
    f.original_unchanged();
    // The fresh consumer guard rejects an expected base that is not the actual
    // recipe even though both are valid #355 values.
    const auto wrong = PackageBaseIdentity::make(expected.source(), "other-base");
    try {
        static_cast<void>(prepare_local_recipe_build(open_local_source_root(f.source), prepare_test_trusted_cache_root(),
                                                     f.environment(), {{patch("before", "first")}}, {}, {}, wrong));
    } catch(const LocalRecipeCandidateError& error) {
        expect(error.failure().phase == LocalRecipeCandidatePhase::Identity && error.failure().patches[0] == LocalRecipePatchOutcome::NotAttempted, "association mismatch reached apply");
        return;
    }
    throw std::runtime_error("pure identity equality authorized the wrong association");
}

void test_publication_replacement() {
    Fixture f;
    auto source = f.observe();
    auto loaded = f.registration(source);
    const auto path = local_patch_association_record_path(source.identity());
    set_patch_association_test_hook([&](Point at, const fs::path&) {
        if(at == Point::BeforePublication) {
            fs::rename(path, path.string() + ".user-saved");
            fs::create_symlink(f.material / "one.patch", path);
        }
    });
    expect_failure(update_local_patch_association(source, loaded, f.material, {"one.patch"}), Kind::ConcurrentChange);
    set_patch_association_test_hook({});
    expect(fs::is_symlink(path), "writer removed/replaced unknown named replacement");
    expect(read(f.material / "one.patch") == patch("before", "first"), "writer touched external original");
}

void test_xdg_boundaries() {
    Fixture f;
    auto source = f.observe();
    ::setenv("XDG_CONFIG_HOME", "relative", 1);
    expect_failure(read_local_patch_association(source.identity()), Kind::Unsafe);
    ::setenv("XDG_CONFIG_HOME", (f.root / "missing-base").c_str(), 1);
    expect_failure(register_local_patch_association(source, f.material, {"one.patch"}), Kind::Unsafe);
    expect(!fs::exists(f.root / "missing-base"), "explicit base was created");
    ::setenv("XDG_CONFIG_HOME", "", 1);
    auto loaded = take<LoadedPatchAssociation>(register_local_patch_association(source, f.material, {"one.patch"}));
    expect(fs::exists(f.root / "home/.config/moguet/patches.d"), "HOME fallback not used");
    static_cast<void>(take<PatchAssociationForgotten>(forget_local_patch_association(loaded)));
}

void test_search_only_material_ancestor() {
    Fixture f;
    auto source = f.observe();
    const auto ancestor = f.root / "search-only";
    fs::create_directory(ancestor);
    fs::rename(f.material, ancestor / "material");
    ::chmod(ancestor.c_str(), 0100);
    try {
        auto loaded = take<LoadedPatchAssociation>(register_local_patch_association(
            source, ancestor / "material", {"one.patch"}));
        auto acquired = take<AcquiredLocalRecipeSeries>(acquire_local_patch_series(loaded));
        expect(acquired.patches()[0].bytes == patch("before", "first"),
               "search-only ancestor was treated as unsafe or required directory listing");
    } catch(...) {
        ::chmod(ancestor.c_str(), 0700);
        throw;
    }
    ::chmod(ancestor.c_str(), 0700);
}

void test_config_does_not_enter_originals() {
    for(const int mode : {0, 1, 2, 3}) {
        Fixture f;
        if(mode == 1) fs::create_directory(f.source / "config-parent");
        auto source = f.observe();
        const auto source_entries = std::distance(fs::directory_iterator(f.source), fs::directory_iterator{});
        if(mode == 0) ::setenv("XDG_CONFIG_HOME", f.source.c_str(), 1);
        if(mode == 1) ::setenv("XDG_CONFIG_HOME", (f.source / "config-parent").c_str(), 1);
        if(mode == 2) {
            ::unsetenv("XDG_CONFIG_HOME");
            ::setenv("HOME", f.source.c_str(), 1);
        }
        if(mode == 3) ::setenv("XDG_CONFIG_HOME", f.material.c_str(), 1);
        expect_failure(register_local_patch_association(source, f.material, {"one.patch"}), Kind::Unsafe);
        expect(std::distance(fs::directory_iterator(f.source), fs::directory_iterator{}) == source_entries,
               "config writer modified original tree");
        expect(!fs::exists(f.source / "config-parent/moguet") && !fs::exists(f.material / "moguet"),
               "config writer created managed directories in an input");
    }
}

void test_postcommit_lineage_refuses_cleanup() {
    for(const bool forget : {false, true}) {
        Fixture f;
        auto source = f.observe();
        auto loaded = f.registration(source);
        const auto path = local_patch_association_record_path(source.identity());
        const auto moved = path.parent_path().string() + "-moved";
        set_patch_association_test_hook([&](Point at, const fs::path&) {
            if(at == Point::AfterPublication) {
                fs::rename(path.parent_path(), moved);
                fs::create_directory(path.parent_path());
                fs::permissions(path.parent_path(), fs::perms::owner_all);
            }
        });
        if(forget)
            expect_failure(forget_local_patch_association(loaded), Kind::PublicationUncertain);
        else
            expect_failure(update_local_patch_association(source, loaded, f.material, {"one.patch"}), Kind::PublicationUncertain);
        set_patch_association_test_hook({});
        bool preserved_old = false;
        for(const auto& entry : fs::directory_iterator(moved))
            if(entry.path().filename().string().starts_with("." + path.filename().string() + "-")) preserved_old = true;
        expect(preserved_old, "postcommit cleanup deleted through detached lineage");
        expect(read(f.material / "one.patch") == patch("before", "first"), "lineage failure touched material");
    }
}
} // namespace

int main() {
    try {
        test_lifecycle();
        test_identity_and_strict_record();
        test_material_failures();
        test_publication();
        test_races_and_same_bytes();
        test_publication_replacement();
        test_xdg_boundaries();
        test_config_does_not_enter_originals();
        test_postcommit_lineage_refuses_cleanup();
        test_search_only_material_ancestor();
        std::cout << "local patch association tests passed (strict persistence, atomic outcomes, identity, digest, races, same bytes)\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "local patch association test failed: " << error.what() << '\n';
        return 1;
    }
}
