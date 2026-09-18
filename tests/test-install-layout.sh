#!/bin/sh

set -eu

repo_root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
. "$repo_root/scripts/validation-status.sh"
stage_root=$(mktemp -d)
stage_dir=$stage_root/root
fixture_build_dir=$stage_root/build
fixture_binary=$stage_root/moguet
test_home=$stage_dir/home/test-user
current_package_fixture=$repo_root/tests/fixtures/current-package
current_package_contract=$current_package_fixture/contract.env
install_payload_authority=$current_package_fixture/install-payload.txt

cleanup() {
    rm -rf "$stage_root"
}
trap cleanup EXIT INT TERM

fail() {
    printf 'install-layout-test: %s\n' "$*" >&2
    exit 1
}

for authority_file in "$current_package_contract" "$install_payload_authority"
do
    [ -f "$authority_file" ] && [ ! -L "$authority_file" ] &&
        [ -s "$authority_file" ] ||
        fail "current package authority must be a non-empty regular non-symlink: $authority_file"
done
# shellcheck source=fixtures/current-package/contract.env
. "$current_package_contract"

xdg_config_home=$test_home/.config
xdg_state_home=$test_home/.local/state
xdg_cache_home=$test_home/.cache

[ -f "$repo_root/VERSION" ] && [ ! -L "$repo_root/VERSION" ] ||
    fail 'VERSION must be a regular non-symlink'
current_version=$(tr -d '[:space:]' < "$repo_root/VERSION")
[ -n "$current_version" ] || fail 'VERSION is empty'

run_make() {
    HOME=$test_home \
    XDG_CONFIG_HOME=$xdg_config_home \
    XDG_STATE_HOME=$xdg_state_home \
    XDG_CACHE_HOME=$xdg_cache_home \
        make -C "$repo_root" --no-print-directory \
            BUILD_DIR="$fixture_build_dir" \
            TARGET="$fixture_binary" \
            PREFIX=/usr DESTDIR="$stage_dir" "$@"
}

binary_file=$stage_dir/usr/bin/$COMMAND_NAME
receipt_helper_file=$stage_dir/usr/libexec/moguet/moguet-alpm-receipt-helper
receipt_helper_build=$fixture_build_dir/cmake-production/moguet-alpm-receipt-helper
source_artifact_helper_file=$stage_dir/usr/libexec/moguet/moguet-source-artifact-install-helper
source_artifact_helper_build=$fixture_build_dir/cmake-production/moguet-source-artifact-install-helper
legacy_binary_file=$stage_dir/usr/bin/jpacker
bash_completion_file=$stage_dir/usr/share/bash-completion/completions/$COMMAND_NAME
zsh_completion_file=$stage_dir/usr/share/zsh/site-functions/_$COMMAND_NAME
fish_completion_file=$stage_dir/usr/share/fish/vendor_completions.d/$COMMAND_NAME.fish
english_man_file=$stage_dir/usr/share/man/man1/$COMMAND_NAME.1
japanese_man_file=$stage_dir/usr/share/man/ja/man1/$COMMAND_NAME.1
catalog_file=$stage_dir/usr/share/locale/ja/LC_MESSAGES/$GETTEXT_DOMAIN.mo
built_catalog_file=$fixture_build_dir/cmake-production/locale/ja/LC_MESSAGES/$GETTEXT_DOMAIN.mo
license_dir=$stage_dir/usr/share/licenses/$PACKAGE_NAME
doc_dir=$stage_dir/usr/share/doc/$PACKAGE_NAME
migration_dir=$doc_dir/docs/migration
licensing_file=$doc_dir/docs/LICENSING.md
config_sample_file=$doc_dir/examples/config.toml

assert_installed_file() {
    source_file=$1
    installed_file=$2
    expected_mode=${3:-644}

    [ -f "$installed_file" ] ||
        fail "$installed_file is missing or is not a regular file."
    [ ! -L "$installed_file" ] ||
        fail "$installed_file must not be a symbolic link."

    mode=$(stat -c '%a' "$installed_file")
    [ "$mode" = "$expected_mode" ] ||
        fail "$installed_file has mode $mode; expected $expected_mode."

    cmp -s "$source_file" "$installed_file" ||
        fail "$installed_file differs from $source_file."
}

assert_file_text() {
    text_file=$1
    expected_text=$2

    [ -f "$text_file" ] && [ ! -L "$text_file" ] ||
        fail "$text_file is missing, not regular, or a symbolic link."
    actual_text=$(cat "$text_file")
    [ "$actual_text" = "$expected_text" ] ||
        fail "$text_file content changed unexpectedly."
}

assert_installed_text() {
    installed_file=$1
    expected=$2

    grep -F -- "$expected" "$installed_file" >/dev/null ||
        fail "$installed_file is missing installed-path reference: $expected"
}

assert_binary_contains() {
    binary_path=$1
    expected_text=$2

    LC_ALL=C grep -aF -- "$expected_text" "$binary_path" >/dev/null ||
        fail "$binary_path is missing compiled installed-path authority: $expected_text"
}

assert_absent() {
    path=$1
    if [ -e "$path" ] || [ -L "$path" ]; then
        fail "$path is present; expected it to be absent."
    fi
}

repository_binary=$repo_root/$COMMAND_NAME
repository_cache=$repo_root/build/cmake-production/CMakeCache.txt
repository_binary_before=
repository_cache_before=

if [ -e "$repository_binary" ] || [ -L "$repository_binary" ]; then
    [ -f "$repository_binary" ] && [ ! -L "$repository_binary" ] ||
        fail "repository binary must be a regular non-symlink when present"
    repository_binary_before=$(sha256sum -- "$repository_binary")
fi

if [ -e "$repository_cache" ] || [ -L "$repository_cache" ]; then
    [ -f "$repository_cache" ] && [ ! -L "$repository_cache" ] ||
        fail "canonical production CMake cache must be a regular non-symlink when present"
    repository_cache_before=$(sha256sum -- "$repository_cache")
fi

assert_directory() {
    directory=$1
    [ -d "$directory" ] && [ ! -L "$directory" ] ||
        fail "$directory is missing, not a directory, or a symbolic link."
}

assert_mode() {
    mode_path=$1
    expected_mode=$2
    actual_mode=$(stat -c '%a' "$mode_path")
    [ "$actual_mode" = "$expected_mode" ] ||
        fail "$mode_path has mode $actual_mode; expected $expected_mode."
}

assert_no_symlinks() {
    inspected_tree=${1:-$stage_dir}
    first_symlink=$(find "$inspected_tree" -type l -print -quit)
    [ -z "$first_symlink" ] ||
        fail "staged tree contains a symbolic link: $first_symlink"
}

assert_exact_payload() {
    if expected_payload=$(cat "$install_payload_authority"); then
        :
    else
        payload_status=$?
        fail "install payload authority read failed with status $payload_status"
    fi
    payload_paths_raw=$stage_root/payload-paths.raw
    payload_paths_normalized=$stage_root/payload-paths.normalized
    payload_paths_sorted=$stage_root/payload-paths.sorted
    if validation_capture_output "$payload_paths_raw" \
        find "$stage_dir" -type f -print; then
        :
    else
        payload_status=$?
        fail "payload path producer failed with status $payload_status; raw=$payload_paths_raw"
    fi
    if sed "s|^$stage_dir||" "$payload_paths_raw" \
        >"$payload_paths_normalized"; then
        :
    else
        payload_status=$?
        fail "payload path normalization failed with status $payload_status"
    fi
    if LC_ALL=C sort "$payload_paths_normalized" >"$payload_paths_sorted"; then
        actual_payload=$(cat "$payload_paths_sorted")
    else
        payload_status=$?
        fail "payload path sorting failed with status $payload_status"
    fi
    [ "$actual_payload" = "$expected_payload" ] || {
        printf 'install-layout-test: unexpected payload:\n%s\n' \
            "$actual_payload" >&2
        exit 1
    }
}

assert_installed_markdown_links() {
    if ! python3 - "$doc_dir" <<'PY'
import pathlib
import re
import sys
import urllib.parse

doc_root = pathlib.Path(sys.argv[1]).resolve()
link_pattern = re.compile(r"\[[^]]*\]\(([^)]+)\)")

for markdown_file in sorted(doc_root.rglob("*.md")):
    text = markdown_file.read_text(encoding="utf-8")
    for match in link_pattern.finditer(text):
        raw_target = match.group(1).strip()
        if not raw_target or raw_target.startswith("#"):
            continue
        parsed = urllib.parse.urlparse(raw_target)
        if parsed.scheme or raw_target.startswith("/"):
            continue
        relative_target = urllib.parse.unquote(parsed.path)
        resolved_target = (markdown_file.parent / relative_target).resolve()
        try:
            resolved_target.relative_to(doc_root)
        except ValueError:
            print(
                f"{markdown_file}: relative link leaves packaged doc root: "
                f"{raw_target}",
                file=sys.stderr,
            )
            raise SystemExit(1)
        if not resolved_target.is_file():
            print(
                f"{markdown_file}: missing packaged relative-link target: "
                f"{raw_target}",
                file=sys.stderr,
            )
            raise SystemExit(1)
PY
    then
        fail 'installed Markdown contains an unresolved relative link.'
    fi
}

assert_package_artifacts_installed() {
    assert_installed_file "$fixture_binary" "$binary_file" 755
    assert_installed_file "$receipt_helper_build" "$receipt_helper_file" 755
    assert_installed_file "$source_artifact_helper_build" \
        "$source_artifact_helper_file" 755
    assert_directory "$(dirname "$receipt_helper_file")"
    assert_mode "$(dirname "$receipt_helper_file")" 755
    assert_binary_contains \
        "$binary_file" \
        /usr/libexec/moguet/moguet-alpm-receipt-helper
    assert_binary_contains \
        "$receipt_helper_file" \
        /usr/libexec/moguet/moguet-alpm-receipt-helper
    assert_binary_contains \
        "$binary_file" \
        /usr/libexec/moguet/moguet-source-artifact-install-helper
    assert_binary_contains \
        "$source_artifact_helper_file" \
        /usr/libexec/moguet/moguet-source-artifact-install-helper
    assert_absent "$legacy_binary_file"
    assert_installed_file "$repo_root/completions/$COMMAND_NAME.bash" \
        "$bash_completion_file"
    assert_installed_file "$repo_root/completions/_$COMMAND_NAME" \
        "$zsh_completion_file"
    assert_installed_file "$repo_root/completions/$COMMAND_NAME.fish" \
        "$fish_completion_file"
    assert_installed_file "$repo_root/man/$COMMAND_NAME.1" "$english_man_file"
    assert_installed_file "$repo_root/man/ja/$COMMAND_NAME.1" "$japanese_man_file"
    assert_installed_file "$built_catalog_file" "$catalog_file"

    assert_installed_file "$repo_root/LICENSE" "$license_dir/LICENSE"
    assert_installed_file "$repo_root/LICENSES/jpacker-MIT-legacy.txt" \
        "$license_dir/jpacker-MIT-legacy.txt"
    assert_installed_file "$repo_root/LICENSES/curl.txt" \
        "$license_dir/curl.txt"
    assert_installed_file "$repo_root/LICENSES/nlohmann-json-MIT.txt" \
        "$license_dir/nlohmann-json-MIT.txt"
    assert_installed_file "$repo_root/LICENSES/tomlplusplus-MIT.txt" \
        "$license_dir/tomlplusplus-MIT.txt"
    assert_installed_file "$repo_root/LICENSES/bjoern-hoehrmann-utf8-MIT.txt" \
        "$license_dir/bjoern-hoehrmann-utf8-MIT.txt"

    assert_installed_file "$repo_root/README.md" "$doc_dir/README.md"
    assert_installed_file "$repo_root/README.ja.md" "$doc_dir/README.ja.md"
    assert_installed_file "$repo_root/THIRD_PARTY_NOTICES.md" \
        "$doc_dir/THIRD_PARTY_NOTICES.md"
    assert_installed_file "$repo_root/sample/config.toml" \
        "$config_sample_file"
    assert_installed_file "$repo_root/docs/LICENSING.md" \
        "$licensing_file"
    assert_installed_file "$repo_root/docs/migration/v1-to-v2.md" \
        "$migration_dir/v1-to-v2.md"
    assert_installed_file "$repo_root/docs/migration/v1-to-v2.ja.md" \
        "$migration_dir/v1-to-v2.ja.md"

    assert_installed_text "$licensing_file" \
        "/usr/share/licenses/$PACKAGE_NAME/LICENSE"
    assert_installed_text "$doc_dir/THIRD_PARTY_NOTICES.md" \
        "/usr/share/licenses/$PACKAGE_NAME/tomlplusplus-MIT.txt"
    assert_installed_text "$doc_dir/THIRD_PARTY_NOTICES.md" \
        "/usr/share/doc/$PACKAGE_NAME/docs/LICENSING.md"
    assert_installed_text "$doc_dir/README.md" "sample/config.toml"
    assert_installed_text "$doc_dir/README.md" \
        "/usr/share/doc/$PACKAGE_NAME/examples/config.toml"
    assert_installed_text "$doc_dir/README.ja.md" "sample/config.toml"
    assert_installed_text "$doc_dir/README.ja.md" \
        "/usr/share/doc/$PACKAGE_NAME/examples/config.toml"
    assert_installed_markdown_links
    assert_no_symlinks
}

assert_package_artifacts_absent() {
    for path in \
        "$binary_file" \
        "$receipt_helper_file" \
        "$source_artifact_helper_file" \
        "$legacy_binary_file" \
        "$bash_completion_file" \
        "$zsh_completion_file" \
        "$fish_completion_file" \
        "$english_man_file" \
        "$japanese_man_file" \
        "$catalog_file" \
        "$license_dir/LICENSE" \
        "$license_dir/jpacker-MIT-legacy.txt" \
        "$license_dir/curl.txt" \
        "$license_dir/nlohmann-json-MIT.txt" \
        "$license_dir/tomlplusplus-MIT.txt" \
        "$license_dir/bjoern-hoehrmann-utf8-MIT.txt" \
        "$doc_dir/README.md" \
        "$doc_dir/README.ja.md" \
        "$doc_dir/THIRD_PARTY_NOTICES.md" \
        "$config_sample_file" \
        "$licensing_file" \
        "$migration_dir/v1-to-v2.md" \
        "$migration_dir/v1-to-v2.ja.md"
    do
        assert_absent "$path"
    done
}

# Fresh install: exact Moguet payload only, no legacy alias, /etc content, or
# user XDG data. Version execution is also read-only with respect to XDG.
run_make install
assert_package_artifacts_installed
assert_exact_payload
assert_absent "$stage_dir/etc"
assert_absent "$xdg_config_home/$XDG_IDENTITY"
assert_absent "$xdg_state_home/$XDG_IDENTITY"
assert_absent "$xdg_cache_home/$XDG_IDENTITY"
version_output=$(LC_ALL=C HOME=$test_home \
    XDG_CONFIG_HOME=$xdg_config_home \
    XDG_STATE_HOME=$xdg_state_home \
    XDG_CACHE_HOME=$xdg_cache_home \
    "$binary_file" --version)
[ "$version_output" = "$PROJECT_NAME v$current_version" ] ||
    fail "staged binary version mismatch: $version_output"
assert_absent "$xdg_config_home/$XDG_IDENTITY"
assert_absent "$xdg_state_home/$XDG_IDENTITY"
assert_absent "$xdg_cache_home/$XDG_IDENTITY"

# Reinstall refreshes only package-owned files and leaves foreign/user/legacy
# data untouched. These sentinels also cover uninstall preservation.
legacy_config=$stage_dir/etc/jpacker/jpacker.conf
legacy_preference=$stage_dir/etc/jpacker/package.build/fastfetch
foreign_system_file=$stage_dir/etc/$XDG_IDENTITY/foreign-admin-file
user_config=$xdg_config_home/$XDG_IDENTITY/config.toml
canonical_preference_dir=$xdg_config_home/$XDG_IDENTITY/source-build.d
canonical_preference=$canonical_preference_dir/fastfetch
user_state=$xdg_state_home/$XDG_IDENTITY/$COMMAND_NAME.log
user_cache=$xdg_cache_home/$XDG_IDENTITY/cache-entry
foreign_doc=$doc_dir/foreign-file.keep
foreign_license=$license_dir/foreign-file.keep
foreign_completion=$(dirname "$bash_completion_file")/foreign-command
foreign_catalog=$(dirname "$catalog_file")/foreign-domain.mo

install -Dm644 /dev/null "$legacy_config"
printf '%s\n' 'NOEDIT=true' > "$legacy_config"
install -Dm644 /dev/null "$legacy_preference"
printf '%s\n' 'CFLAGS=-O3 -march=native' > "$legacy_preference"
install -Dm644 /dev/null "$foreign_system_file"
printf '%s\n' 'foreign admin data' > "$foreign_system_file"
install -Dm600 /dev/null "$user_config"
printf '%s\n' 'schema_version = 1' > "$user_config"
install -d -m700 "$canonical_preference_dir"
install -m600 /dev/null "$canonical_preference"
printf '%s\n' 'CFLAGS=-O2 -pipe' > "$canonical_preference"
install -Dm600 /dev/null "$user_state"
printf '%s\n' 'user state' > "$user_state"
install -Dm600 /dev/null "$user_cache"
printf '%s\n' 'user cache' > "$user_cache"
printf '%s\n' 'foreign documentation' > "$foreign_doc"
printf '%s\n' 'foreign license' > "$foreign_license"
printf '%s\n' 'foreign completion' > "$foreign_completion"
printf '%s\n' 'foreign catalog' > "$foreign_catalog"

run_make install
assert_package_artifacts_installed
assert_file_text "$legacy_config" 'NOEDIT=true'
assert_file_text "$legacy_preference" 'CFLAGS=-O3 -march=native'
assert_file_text "$foreign_system_file" 'foreign admin data'
assert_file_text "$user_config" 'schema_version = 1'
assert_file_text "$canonical_preference" 'CFLAGS=-O2 -pipe'
assert_mode "$canonical_preference_dir" 700
assert_mode "$canonical_preference" 600
assert_file_text "$user_state" 'user state'
assert_file_text "$user_cache" 'user cache'

run_make uninstall
assert_package_artifacts_absent
assert_file_text "$legacy_config" 'NOEDIT=true'
assert_file_text "$legacy_preference" 'CFLAGS=-O3 -march=native'
assert_file_text "$foreign_system_file" 'foreign admin data'
assert_file_text "$user_config" 'schema_version = 1'
assert_file_text "$canonical_preference" 'CFLAGS=-O2 -pipe'
assert_mode "$canonical_preference_dir" 700
assert_mode "$canonical_preference" 600
assert_file_text "$user_state" 'user state'
assert_file_text "$user_cache" 'user cache'
assert_file_text "$foreign_doc" 'foreign documentation'
assert_file_text "$foreign_license" 'foreign license'
assert_file_text "$foreign_completion" 'foreign completion'
assert_file_text "$foreign_catalog" 'foreign catalog'
assert_directory "$stage_dir/etc/jpacker/package.build"
assert_directory "$xdg_config_home/$XDG_IDENTITY"
assert_directory "$xdg_state_home/$XDG_IDENTITY"
assert_directory "$xdg_cache_home/$XDG_IDENTITY"
assert_no_symlinks

# A final reinstall/uninstall cycle proves that retained foreign entries do not
# prevent package-owned files from being restored or removed exactly.
run_make install
assert_package_artifacts_installed
run_make uninstall
assert_package_artifacts_absent
assert_file_text "$legacy_preference" 'CFLAGS=-O3 -march=native'
assert_file_text "$user_config" 'schema_version = 1'
assert_file_text "$canonical_preference" 'CFLAGS=-O2 -pipe'
assert_mode "$canonical_preference_dir" 700
assert_mode "$canonical_preference" 600
assert_file_text "$foreign_doc" 'foreign documentation'
assert_file_text "$foreign_license" 'foreign license'
assert_no_symlinks

# Every supported Make destination override is projected into the same CMake
# install graph.  A second DESTDIR proves the custom layout without touching
# the host, and the CMake manifest is then the sole uninstall path authority.
custom_stage_dir=$stage_root/custom-root
custom_prefix=/opt/moguet-prefix
custom_bindir=/custom/bin
custom_libexecdir=/custom/libexec/moguet
custom_compdir=/custom/completions/bash
custom_zshcompdir=/custom/completions/zsh
custom_fishcompdir=/custom/completions/fish
custom_mandir=/custom/man/en/man1
custom_jamandir=/custom/man/ja/man1
custom_licensedir=/custom/licenses/moguet
custom_docdir=/custom/doc/moguet
custom_localedir=/custom/locale

run_custom_make() {
    HOME=$test_home \
    XDG_CONFIG_HOME=$xdg_config_home \
    XDG_STATE_HOME=$xdg_state_home \
    XDG_CACHE_HOME=$xdg_cache_home \
        make -C "$repo_root" --no-print-directory \
            BUILD_DIR="$fixture_build_dir" \
            TARGET="$fixture_binary" \
            PREFIX="$custom_prefix" \
            DESTDIR="$custom_stage_dir" \
            BINDIR="$custom_bindir" \
            LIBEXECDIR="$custom_libexecdir" \
            COMPDIR="$custom_compdir" \
            ZSHCOMPDIR="$custom_zshcompdir" \
            FISHCOMPDIR="$custom_fishcompdir" \
            MANDIR="$custom_mandir" \
            JAMANDIR="$custom_jamandir" \
            LICENSEDIR="$custom_licensedir" \
            DOCDIR="$custom_docdir" \
            LOCALEDIR="$custom_localedir" \
            "$@"
}

custom_binary=$custom_stage_dir$custom_bindir/$COMMAND_NAME
custom_receipt_helper=$custom_stage_dir$custom_libexecdir/moguet-alpm-receipt-helper
custom_source_artifact_helper=$custom_stage_dir$custom_libexecdir/moguet-source-artifact-install-helper
custom_bash_completion=$custom_stage_dir$custom_compdir/$COMMAND_NAME
custom_zsh_completion=$custom_stage_dir$custom_zshcompdir/_$COMMAND_NAME
custom_fish_completion=$custom_stage_dir$custom_fishcompdir/$COMMAND_NAME.fish
custom_english_man=$custom_stage_dir$custom_mandir/$COMMAND_NAME.1
custom_japanese_man=$custom_stage_dir$custom_jamandir/$COMMAND_NAME.1
custom_catalog=$custom_stage_dir$custom_localedir/ja/LC_MESSAGES/$GETTEXT_DOMAIN.mo
custom_license_dir=$custom_stage_dir$custom_licensedir
custom_doc_dir=$custom_stage_dir$custom_docdir
custom_migration_dir=$custom_doc_dir/docs/migration
custom_config_sample=$custom_doc_dir/examples/config.toml

run_custom_make install
assert_installed_file "$fixture_binary" "$custom_binary" 755
assert_installed_file "$receipt_helper_build" "$custom_receipt_helper" 755
assert_installed_file "$source_artifact_helper_build" \
    "$custom_source_artifact_helper" 755
assert_directory "$(dirname "$custom_receipt_helper")"
assert_mode "$(dirname "$custom_receipt_helper")" 755
assert_binary_contains \
    "$custom_binary" \
    "$custom_libexecdir/moguet-alpm-receipt-helper"
assert_binary_contains \
    "$custom_receipt_helper" \
    "$custom_libexecdir/moguet-alpm-receipt-helper"
assert_binary_contains \
    "$custom_binary" \
    "$custom_libexecdir/moguet-source-artifact-install-helper"
assert_binary_contains \
    "$custom_source_artifact_helper" \
    "$custom_libexecdir/moguet-source-artifact-install-helper"
assert_installed_file "$repo_root/completions/$COMMAND_NAME.bash" \
    "$custom_bash_completion"
assert_installed_file "$repo_root/completions/_$COMMAND_NAME" \
    "$custom_zsh_completion"
assert_installed_file "$repo_root/completions/$COMMAND_NAME.fish" \
    "$custom_fish_completion"
assert_installed_file "$repo_root/man/$COMMAND_NAME.1" "$custom_english_man"
assert_installed_file "$repo_root/man/ja/$COMMAND_NAME.1" "$custom_japanese_man"
assert_installed_file "$built_catalog_file" "$custom_catalog"
assert_installed_file "$repo_root/LICENSE" "$custom_license_dir/LICENSE"
assert_installed_file "$repo_root/LICENSES/jpacker-MIT-legacy.txt" \
    "$custom_license_dir/jpacker-MIT-legacy.txt"
assert_installed_file "$repo_root/LICENSES/curl.txt" \
    "$custom_license_dir/curl.txt"
assert_installed_file "$repo_root/LICENSES/nlohmann-json-MIT.txt" \
    "$custom_license_dir/nlohmann-json-MIT.txt"
assert_installed_file "$repo_root/LICENSES/tomlplusplus-MIT.txt" \
    "$custom_license_dir/tomlplusplus-MIT.txt"
assert_installed_file "$repo_root/LICENSES/bjoern-hoehrmann-utf8-MIT.txt" \
    "$custom_license_dir/bjoern-hoehrmann-utf8-MIT.txt"
assert_installed_file "$repo_root/README.md" "$custom_doc_dir/README.md"
assert_installed_file "$repo_root/README.ja.md" "$custom_doc_dir/README.ja.md"
assert_installed_file "$repo_root/THIRD_PARTY_NOTICES.md" \
    "$custom_doc_dir/THIRD_PARTY_NOTICES.md"
assert_installed_file "$repo_root/sample/config.toml" \
    "$custom_config_sample"
assert_installed_file "$repo_root/docs/LICENSING.md" \
    "$custom_doc_dir/docs/LICENSING.md"
assert_installed_file "$repo_root/docs/migration/v1-to-v2.md" \
    "$custom_migration_dir/v1-to-v2.md"
assert_installed_file "$repo_root/docs/migration/v1-to-v2.ja.md" \
    "$custom_migration_dir/v1-to-v2.ja.md"

assert_absent "$custom_stage_dir/usr/bin/$COMMAND_NAME"
assert_absent "$custom_stage_dir/usr/libexec/moguet/moguet-alpm-receipt-helper"
assert_absent "$custom_stage_dir/usr/libexec/moguet/moguet-source-artifact-install-helper"
assert_absent "$custom_stage_dir/usr/share/bash-completion/completions/$COMMAND_NAME"
assert_absent "$custom_stage_dir$custom_prefix/bin/$COMMAND_NAME"
assert_absent "$custom_stage_dir$custom_prefix/share/man/man1/$COMMAND_NAME.1"
assert_absent "$custom_stage_dir$custom_prefix/share/licenses/$PACKAGE_NAME/LICENSE"
assert_absent "$custom_stage_dir$custom_prefix/share/doc/$PACKAGE_NAME/README.md"
assert_no_symlinks "$custom_stage_dir"

custom_foreign_file=$custom_doc_dir/foreign-file.keep
printf '%s\n' 'foreign custom-layout documentation' > "$custom_foreign_file"
run_custom_make uninstall

for custom_owned_file in \
    "$custom_binary" \
    "$custom_receipt_helper" \
    "$custom_source_artifact_helper" \
    "$custom_bash_completion" \
    "$custom_zsh_completion" \
    "$custom_fish_completion" \
    "$custom_english_man" \
    "$custom_japanese_man" \
    "$custom_catalog" \
    "$custom_license_dir/LICENSE" \
    "$custom_license_dir/jpacker-MIT-legacy.txt" \
    "$custom_license_dir/curl.txt" \
    "$custom_license_dir/nlohmann-json-MIT.txt" \
    "$custom_license_dir/tomlplusplus-MIT.txt" \
    "$custom_license_dir/bjoern-hoehrmann-utf8-MIT.txt" \
    "$custom_doc_dir/README.md" \
    "$custom_doc_dir/README.ja.md" \
    "$custom_doc_dir/THIRD_PARTY_NOTICES.md" \
    "$custom_config_sample" \
    "$custom_doc_dir/docs/LICENSING.md" \
    "$custom_migration_dir/v1-to-v2.md" \
    "$custom_migration_dir/v1-to-v2.ja.md"
do
    assert_absent "$custom_owned_file"
done
assert_file_text "$custom_foreign_file" 'foreign custom-layout documentation'
assert_no_symlinks "$custom_stage_dir"

# The native uninstall helper validates the whole manifest before unlinking and
# anchors deletion below a nofollow-opened root.  Keep adversarial fixtures
# separate from the canonical install tree so a failed case cannot corrupt the
# normal-layout authority used above.
uninstall_helper=$fixture_build_dir/cmake-production/moguet-uninstall-helper
[ -x "$uninstall_helper" ] ||
    fail "CMake uninstall helper is missing or not executable: $uninstall_helper"
uninstall_safety_root=$stage_root/uninstall-safety

run_uninstall_helper() {
    helper_destdir=$1
    helper_manifest=$2
    shift 2
    DESTDIR="$helper_destdir" "$uninstall_helper" \
        --manifest "$helper_manifest" "$@"
}

expect_uninstall_failure() {
    failure_label=$1
    shift
    if "$@"; then
        fail "$failure_label unexpectedly succeeded"
    fi
}

# An ancestor symlink must fail before the earlier valid manifest entry is
# removed, and it must never reach the foreign target outside DESTDIR.
ancestor_stage=$uninstall_safety_root/ancestor-stage
ancestor_foreign=$uninstall_safety_root/ancestor-foreign
ancestor_manifest=$uninstall_safety_root/ancestor-manifest.txt
ancestor_owned=$ancestor_stage/custom/bin/moguet
ancestor_foreign_file=$ancestor_foreign/moguet/README.md
install -Dm644 /dev/null "$ancestor_owned"
printf '%s\n' 'owned before ancestor failure' > "$ancestor_owned"
install -Dm644 /dev/null "$ancestor_foreign_file"
printf '%s\n' 'foreign ancestor target' > "$ancestor_foreign_file"
install -d "$ancestor_stage/custom"
ln -s "$ancestor_foreign" "$ancestor_stage/custom/doc"
printf '%s\n' \
    /custom/bin/moguet \
    /custom/doc/moguet/README.md \
    > "$ancestor_manifest"
expect_uninstall_failure \
    'ancestor symlink uninstall' \
    run_uninstall_helper \
    "$ancestor_stage" \
    "$ancestor_manifest" \
    --allowed-root /custom/bin \
    --allowed-root /custom/doc/moguet
assert_file_text "$ancestor_owned" 'owned before ancestor failure'
assert_file_text "$ancestor_foreign_file" 'foreign ancestor target'

# The manifest itself is authority only when it is a regular non-symlink.
manifest_link_stage=$uninstall_safety_root/manifest-link-stage
manifest_link_real=$uninstall_safety_root/manifest-link-real.txt
manifest_link=$uninstall_safety_root/manifest-link.txt
manifest_link_owned=$manifest_link_stage/custom/bin/moguet
install -Dm644 /dev/null "$manifest_link_owned"
printf '%s\n' 'owned before manifest-link failure' > "$manifest_link_owned"
printf '%s\n' /custom/bin/moguet > "$manifest_link_real"
ln -s "$manifest_link_real" "$manifest_link"
expect_uninstall_failure \
    'manifest symlink uninstall' \
    run_uninstall_helper \
    "$manifest_link_stage" \
    "$manifest_link" \
    --allowed-root /custom/bin
assert_file_text "$manifest_link_owned" 'owned before manifest-link failure'

# An empty regular manifest is not a valid uninstall authority.
empty_manifest_stage=$uninstall_safety_root/empty-manifest-stage
empty_manifest=$uninstall_safety_root/empty-manifest.txt
empty_manifest_owned=$empty_manifest_stage/custom/bin/moguet
install -Dm644 /dev/null "$empty_manifest_owned"
printf '%s\n' 'owned before empty-manifest failure' > "$empty_manifest_owned"
install -m644 /dev/null "$empty_manifest"
expect_uninstall_failure \
    'empty manifest uninstall' \
    run_uninstall_helper \
    "$empty_manifest_stage" \
    "$empty_manifest" \
    --allowed-root /custom/bin
assert_file_text "$empty_manifest_owned" 'owned before empty-manifest failure'

# A later directory entry invalidates the complete manifest before any valid
# entry is removed.
late_invalid_stage=$uninstall_safety_root/late-invalid-stage
late_invalid_manifest=$uninstall_safety_root/late-invalid-manifest.txt
late_invalid_owned=$late_invalid_stage/custom/bin/moguet
late_invalid_directory=$late_invalid_stage/custom/doc/moguet/docs
install -Dm644 /dev/null "$late_invalid_owned"
printf '%s\n' 'owned before later invalid entry' > "$late_invalid_owned"
install -d "$late_invalid_directory"
printf '%s\n' \
    /custom/bin/moguet \
    /custom/doc/moguet/docs \
    > "$late_invalid_manifest"
expect_uninstall_failure \
    'later invalid entry uninstall' \
    run_uninstall_helper \
    "$late_invalid_stage" \
    "$late_invalid_manifest" \
    --allowed-root /custom/bin \
    --allowed-root /custom/doc/moguet
assert_file_text "$late_invalid_owned" 'owned before later invalid entry'
assert_directory "$late_invalid_directory"

# Lexical traversal is rejected instead of normalized into a different path.
traversal_stage=$uninstall_safety_root/traversal-stage
traversal_manifest=$uninstall_safety_root/traversal-manifest.txt
traversal_owned=$traversal_stage/custom/bin/moguet
traversal_foreign=$traversal_stage/custom/foreign.keep
install -Dm644 /dev/null "$traversal_owned"
printf '%s\n' 'owned before traversal failure' > "$traversal_owned"
install -Dm644 /dev/null "$traversal_foreign"
printf '%s\n' 'foreign traversal target' > "$traversal_foreign"
printf '%s\n' \
    /custom/bin/moguet \
    /custom/bin/../foreign.keep \
    > "$traversal_manifest"
expect_uninstall_failure \
    'manifest traversal uninstall' \
    run_uninstall_helper \
    "$traversal_stage" \
    "$traversal_manifest" \
    --allowed-root /custom/bin
assert_file_text "$traversal_owned" 'owned before traversal failure'
assert_file_text "$traversal_foreign" 'foreign traversal target'

# A final symlink is itself the payload entry. unlinkat removes that link while
# the target outside DESTDIR remains untouched.
final_link_stage=$uninstall_safety_root/final-link-stage
final_link_manifest=$uninstall_safety_root/final-link-manifest.txt
final_link_target=$uninstall_safety_root/final-link-target.keep
final_link=$final_link_stage/custom/bin/moguet
install -Dm644 /dev/null "$final_link_target"
printf '%s\n' 'foreign final-link target' > "$final_link_target"
install -d "$(dirname "$final_link")"
ln -s "$final_link_target" "$final_link"
printf '%s\n' /custom/bin/moguet > "$final_link_manifest"
run_uninstall_helper \
    "$final_link_stage" \
    "$final_link_manifest" \
    --allowed-root /custom/bin
assert_absent "$final_link"
assert_file_text "$final_link_target" 'foreign final-link target'

# Without DESTDIR, the helper still anchors traversal at / and restricts the
# manifest to configured install roots. Use a temporary absolute root so this
# exercises live-root semantics without touching the host payload.
root_mode_dir=$uninstall_safety_root/root-mode
root_mode_allowed=$root_mode_dir/allowed
root_mode_file=$root_mode_allowed/moguet
root_mode_manifest=$uninstall_safety_root/root-mode-manifest.txt
install -Dm644 /dev/null "$root_mode_file"
printf '%s\n' 'owned root-mode payload' > "$root_mode_file"
printf '%s\n' "$root_mode_file" > "$root_mode_manifest"
DESTDIR='' "$uninstall_helper" \
    --manifest "$root_mode_manifest" \
    --allowed-root "$root_mode_allowed"
assert_absent "$root_mode_file"

if [ -n "$repository_binary_before" ]; then
    [ "$(sha256sum -- "$repository_binary")" = "$repository_binary_before" ] ||
        fail "install-layout fixture rewrote the repository binary"
else
    assert_absent "$repository_binary"
fi

if [ -n "$repository_cache_before" ]; then
    [ "$(sha256sum -- "$repository_cache")" = "$repository_cache_before" ] ||
        fail "install-layout fixture rewrote the canonical production CMake cache"
else
    assert_absent "$repository_cache"
fi

printf 'install-layout-test: all checks passed\n'
