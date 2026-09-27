#!/bin/sh

set -eu

repo_root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
. "$repo_root/scripts/validation-status.sh"
current_package_fixture=$repo_root/tests/fixtures/current-package
current_package_contract=$current_package_fixture/contract.env
runtime_dependency_authority=$current_package_fixture/runtime-dependencies.txt
build_dependency_authority=$current_package_fixture/build-dependencies.txt
install_payload_authority=$current_package_fixture/install-payload.txt
tmp_dir=$(mktemp -d)

cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

fail() {
    printf 'package-transition-test: %s\n' "$*" >&2
    exit 1
}

for authority_file in \
    "$current_package_contract" \
    "$runtime_dependency_authority" \
    "$build_dependency_authority" \
    "$install_payload_authority"
do
    [ -f "$authority_file" ] && [ ! -L "$authority_file" ] &&
        [ -s "$authority_file" ] ||
        fail "current package authority must be a non-empty regular non-symlink: $authority_file"
done
# shellcheck source=fixtures/current-package/contract.env
. "$current_package_contract"

sha256_file() {
    if checksum_output=$(sha256sum -- "$1"); then
        printf '%s\n' "${checksum_output%% *}"
        return 0
    else
        return $?
    fi
}

count_command_output_lines() {
    count_label=$1
    shift
    count_raw=$tmp_dir/$count_label.raw
    if validation_capture_output "$count_raw" "$@"; then
        wc -l <"$count_raw"
        return 0
    else
        count_status=$?
    fi
    fail "$count_label producer failed with status $count_status; raw=$count_raw"
}

grep_fixed_matches() {
    match_pattern=$1
    match_file=$2
    if grep -Fo -- "$match_pattern" "$match_file"; then
        return 0
    else
        match_status=$?
    fi
    [ "$match_status" -eq 1 ] && return 0
    return "$match_status"
}

assert_source_archive_input() {
    archive_label=$1
    archive_path=$2

    [ -f "$archive_path" ] && [ ! -L "$archive_path" ] ||
        fail "$archive_label source archive is missing, not regular, or a symlink: $archive_path"
    [ -r "$archive_path" ] ||
        fail "$archive_label source archive is not readable: $archive_path"
    bsdtar -tf "$archive_path" >/dev/null 2>&1 ||
        fail "$archive_label source archive is not a readable tar archive: $archive_path"
}

run_logged() {
    run_description=$1
    run_log=$2
    shift 2

    if ! "$@" >"$run_log" 2>&1; then
        printf 'package-transition-test: %s failed; log follows:\n' \
            "$run_description" >&2
        sed -n '1,240p' "$run_log" >&2
        exit 1
    fi
}

assert_absent() {
    absent_path=$1
    if [ -e "$absent_path" ] || [ -L "$absent_path" ]; then
        fail "$absent_path is present; expected it to be absent"
    fi
}

assert_text_file() {
    text_path=$1
    expected_text=$2

    [ -f "$text_path" ] && [ ! -L "$text_path" ] ||
        fail "$text_path is missing or is not a regular file"
    actual_text=$(cat "$text_path")
    [ "$actual_text" = "$expected_text" ] ||
        fail "$text_path changed unexpectedly"
}

assert_mode() {
    mode_path=$1
    expected_mode=$2
    actual_mode=$(stat -c '%a' "$mode_path")
    [ "$actual_mode" = "$expected_mode" ] ||
        fail "$mode_path has mode $actual_mode; expected $expected_mode"
}

write_regular_manifest() {
    manifest_root=$1
    manifest_output=$2
    if validation_capture_sorted_output \
        "$manifest_output.raw" "$manifest_output" \
        write_regular_manifest_raw "$manifest_root"; then
        return 0
    else
        manifest_status=$?
    fi
    fail "regular manifest producer failed with status $manifest_status; raw=$manifest_output.raw"
}

write_regular_manifest_raw() {
    manifest_root=$1
    (
        cd "$manifest_root" || exit $?
        find . -type f ! -path './.*' -print || exit $?
    )
}

write_directory_manifest() {
    manifest_root=$1
    manifest_output=$2
    manifest_raw=$manifest_output.raw
    manifest_temporary=$manifest_output.tmp.$$
    if validation_capture_output "$manifest_raw" \
        write_directory_manifest_raw "$manifest_root"; then
        :
    else
        manifest_status=$?
        fail "directory manifest producer failed with status $manifest_status; raw=$manifest_raw"
    fi
    if LC_ALL=C sort -r "$manifest_raw" >"$manifest_temporary"; then
        mv "$manifest_temporary" "$manifest_output"
    else
        manifest_status=$?
        rm -f "$manifest_temporary" >/dev/null 2>&1 || :
        fail "directory manifest normalization failed with status $manifest_status"
    fi
}

write_directory_manifest_raw() {
    manifest_root=$1
    (
        cd "$manifest_root" || exit $?
        find . -type d ! -path . ! -path './.*' -print || exit $?
    )
}

write_checksum_snapshot() {
    snapshot_root=$1
    snapshot_output=$2
    snapshot_paths_raw=$snapshot_output.paths.raw
    snapshot_paths=$snapshot_output.paths
    snapshot_temporary=$snapshot_output.tmp.$$
    if validation_capture_sorted_output \
        "$snapshot_paths_raw" "$snapshot_paths" \
        write_checksum_paths_raw "$snapshot_root"; then
        :
    else
        snapshot_status=$?
        fail "checksum path producer failed with status $snapshot_status; raw=$snapshot_paths_raw"
    fi
    if (
        cd "$snapshot_root" || exit $?
        while IFS= read -r snapshot_path; do
            sha256sum "$snapshot_path" || exit $?
        done <"$snapshot_paths"
    ) >"$snapshot_temporary"; then
        mv "$snapshot_temporary" "$snapshot_output"
    else
        snapshot_status=$?
        fail "checksum producer failed with status $snapshot_status; partial=$snapshot_temporary"
    fi
}

write_checksum_paths_raw() {
    snapshot_root=$1
    (
        cd "$snapshot_root" || exit $?
        find . -type f -print || exit $?
    )
}

assert_snapshot_matches() {
    snapshot_root=$1
    expected_snapshot=$2

    if ! (cd "$snapshot_root" && sha256sum -c "$expected_snapshot" >/dev/null); then
        fail "$snapshot_root no longer matches $expected_snapshot"
    fi
}

assert_snapshot_exact() {
    snapshot_root=$1
    expected_snapshot=$2
    actual_snapshot=$tmp_dir/actual.snapshot

    write_checksum_snapshot "$snapshot_root" "$actual_snapshot"
    cmp -s "$expected_snapshot" "$actual_snapshot" ||
        fail "$snapshot_root has unexpected added, removed, or changed files"
}

assert_manifest_files_match() {
    source_root=$1
    installed_root=$2
    expected_manifest=$3

    while IFS= read -r relative_path; do
        [ -n "$relative_path" ] || continue
        source_path=$source_root/$relative_path
        installed_path=$installed_root/$relative_path
        [ -f "$installed_path" ] && [ ! -L "$installed_path" ] ||
            fail "$installed_path is missing or is not a regular file"
        cmp -s "$source_path" "$installed_path" ||
            fail "$installed_path differs from its archived payload"
    done <"$expected_manifest"
}

assert_manifest_absent() {
    installed_root=$1
    removed_manifest=$2

    while IFS= read -r relative_path; do
        [ -n "$relative_path" ] || continue
        assert_absent "$installed_root/$relative_path"
    done <"$removed_manifest"
}

remove_package_payload() {
    installed_root=$1
    removed_manifest=$2
    removed_directories=$3

    while IFS= read -r relative_path; do
        [ -n "$relative_path" ] || continue
        case "$relative_path" in
            ./*) ;;
            *) fail "unsafe package manifest path: $relative_path" ;;
        esac
        rm -f "$installed_root/$relative_path"
    done <"$removed_manifest"

    while IFS= read -r relative_path; do
        [ -n "$relative_path" ] || continue
        case "$relative_path" in
            ./*) ;;
            *) fail "unsafe package directory path: $relative_path" ;;
        esac
        rmdir "$installed_root/$relative_path" 2>/dev/null || :
    done <"$removed_directories"
}

assert_no_symlinks() {
    symlink_root=$1
    first_symlink=$(find "$symlink_root" -type l -print -quit)
    [ -z "$first_symlink" ] ||
        fail "$symlink_root contains unexpected symlink $first_symlink"
}

metadata_values() {
    metadata_file=$1
    metadata_key=$2
    awk -v key="$metadata_key" \
        '$1 == key && $2 == "=" { $1 = ""; $2 = ""; sub(/^  */, ""); print }' \
        "$metadata_file"
}

assert_metadata_single() {
    metadata_file=$1
    metadata_key=$2
    expected_value=$3
    actual_value=$(metadata_values "$metadata_file" "$metadata_key")
    [ "$actual_value" = "$expected_value" ] ||
        fail "$metadata_key mismatch: expected '$expected_value', got '$actual_value'"
}

assert_metadata_set() {
    metadata_file=$1
    metadata_key=$2
    expected_values=$3
    metadata_raw=$tmp_dir/metadata-$metadata_key.raw
    metadata_sorted=$tmp_dir/metadata-$metadata_key.sorted
    if validation_capture_sorted_output "$metadata_raw" "$metadata_sorted" \
        metadata_values "$metadata_file" "$metadata_key"; then
        actual_values=$(cat "$metadata_sorted")
    else
        metadata_status=$?
        fail "$metadata_key producer failed with status $metadata_status; raw=$metadata_raw"
    fi
    [ "$actual_values" = "$expected_values" ] || {
        printf 'package-transition-test: %s mismatch\nexpected:\n%s\nactual:\n%s\n' \
            "$metadata_key" "$expected_values" "$actual_values" >&2
        exit 1
    }
}

assert_metadata_absent() {
    metadata_file=$1
    metadata_key=$2
    actual_values=$(metadata_values "$metadata_file" "$metadata_key")
    [ -z "$actual_values" ] ||
        fail "$metadata_key must be absent; found: $actual_values"
}

initialize_fixture_repository() {
    source_root=$1
    source_tag=$2

    git -C "$source_root" init -q
    git -C "$source_root" config core.hooksPath /dev/null
    git -C "$source_root" config commit.gpgSign false
    git -C "$source_root" config tag.gpgSign false
    git -C "$source_root" config user.name \
        'jpacker package transition fixture'
    git -C "$source_root" config user.email \
        'fixture.invalid@example.invalid'
    git -C "$source_root" add -- .
    git -C "$source_root" commit -q -m "fixture $source_tag"
    git -C "$source_root" tag "$source_tag"
}

prepare_test_pkgbuild() {
    production_pkgbuild=$1
    source_repository=$2
    package_work=$3
    production_source=$4

    fixture_source=git+file://$source_repository
    mkdir -p "$package_work"
    cp "$production_pkgbuild" "$package_work/PKGBUILD"
    cp "$(dirname "$production_pkgbuild")/VERSION" "$package_work/VERSION"

    source_occurrences=$(count_command_output_lines production-source-occurrences \
        grep_fixed_matches "$production_source" "$package_work/PKGBUILD")
    [ "$source_occurrences" = 1 ] ||
        fail "$production_pkgbuild must contain exactly one production source URL"
    sed -i "s|$production_source|$fixture_source|" \
        "$package_work/PKGBUILD"

    restored_pkgbuild=$package_work/PKGBUILD.restored
    sed "s|$fixture_source|$production_source|" \
        "$package_work/PKGBUILD" >"$restored_pkgbuild"
    cmp -s "$production_pkgbuild" "$restored_pkgbuild" ||
        fail "test PKGBUILD differs from production beyond its source URL"
    rm -f "$restored_pkgbuild"
}

run_makepkg_fixture() {
    package_work=$1
    build_root=$2
    source_cache=$3
    package_destination=$4
    source_package_destination=$5
    log_destination=$6
    xdg_cache=$7
    fixture_home=$build_root/home
    fixture_xdg_config=$build_root/xdg-config
    fixture_gnupg_home=$build_root/gnupg
    fixture_makepkg_config=$build_root/makepkg.conf

    mkdir -p "$fixture_home" "$fixture_xdg_config" "$xdg_cache"
    install -d -m700 "$fixture_gnupg_home"
    cp /etc/makepkg.conf "$fixture_makepkg_config"
    # Rolling Arch enables split debug packages by default. This fixture
    # validates the two production package archives, so disable only that
    # additional artifact while retaining the system compiler/tool settings.
    printf '\nOPTIONS+=(!debug)\n' >>"$fixture_makepkg_config"

    (
        cd "$package_work"
        env -u MAKEFLAGS -u MFLAGS \
            HOME="$fixture_home" \
            XDG_CONFIG_HOME="$fixture_xdg_config" \
            XDG_CACHE_HOME="$xdg_cache" \
            GNUPGHOME="$fixture_gnupg_home" \
            BUILDDIR="$build_root" \
            SRCDEST="$source_cache" \
            PKGDEST="$package_destination" \
            SRCPKGDEST="$source_package_destination" \
            LOGDEST="$log_destination" \
            CCACHE_DIR="$build_root/ccache" \
            CCACHE_TEMPDIR="$build_root/ccache-tmp" \
            makepkg --config "$fixture_makepkg_config" \
                --cleanbuild --force --noconfirm
    )
}

assert_archive_layout() {
    archive_path=$1
    archive_listing=$2
    executable_path=$3
    internal_executable_path=${4:-}
    second_internal_executable_path=${5:-}
    third_internal_executable_path=${6:-}
    fourth_internal_executable_path=${7:-}

    LC_ALL=C bsdtar --numeric-owner -tvf "$archive_path" >"$archive_listing"
    if ! awk '
        $3 != "0" || $4 != "0" {
            print "non-root archive entry: " $0
            bad = 1
        }
        END { exit bad }
    ' "$archive_listing" >"$tmp_dir/archive-owner-errors.txt"; then
        sed -n '1,120p' "$tmp_dir/archive-owner-errors.txt" >&2
        fail "$archive_path contains a non-root-owned entry"
    fi

    if ! awk \
        -v executable_path="$executable_path" \
        -v internal_executable_path="$internal_executable_path" \
        -v second_internal_executable_path="$second_internal_executable_path" \
        -v third_internal_executable_path="$third_internal_executable_path" \
        -v fourth_internal_executable_path="$fourth_internal_executable_path" '
        {
            entry_type = substr($1, 1, 1)
            entry_path = $NF
            if (entry_type == "d") {
                expected_mode = "drwxr-xr-x"
            } else if (entry_type == "-") {
                expected_mode = (entry_path == executable_path || \
                                 entry_path == internal_executable_path || \
                                 entry_path == second_internal_executable_path || \
                                 entry_path == third_internal_executable_path || \
                                 entry_path == fourth_internal_executable_path) \
                    ? "-rwxr-xr-x" : "-rw-r--r--"
            } else {
                print "unexpected archive entry type: " $0
                bad = 1
                next
            }
            if ($1 != expected_mode) {
                print "unexpected archive mode: " $0 \
                    " (expected " expected_mode ")"
                bad = 1
            }
        }
        END { exit bad }
    ' "$archive_listing" >"$tmp_dir/archive-mode-errors.txt"; then
        sed -n '1,120p' "$tmp_dir/archive-mode-errors.txt" >&2
        fail "$archive_path contains an entry with an unexpected type or mode"
    fi
}

for required_command in bsdtar git makepkg sha256sum
do
    command -v "$required_command" >/dev/null 2>&1 ||
        fail "$required_command is required"
done

legacy_source_archive_input=${MOGUET_TEST_LEGACY_SOURCE_ARCHIVE-}
current_source_archive_input=${MOGUET_TEST_CURRENT_SOURCE_ARCHIVE-}
if [ -n "$legacy_source_archive_input" ]; then
    assert_source_archive_input legacy "$legacy_source_archive_input"
fi
if [ -n "$current_source_archive_input" ]; then
    assert_source_archive_input current "$current_source_archive_input"
fi

[ -f "$repo_root/VERSION" ] && [ ! -L "$repo_root/VERSION" ] ||
    fail 'current VERSION must be a regular non-symlink'
current_version=$(tr -d '[:space:]' <"$repo_root/VERSION")
[ -n "$current_version" ] || fail 'current VERSION is empty'

v1_source=$tmp_dir/jpacker-v1.16.0-source
v1_makepkg_work=$tmp_dir/jpacker-v1.16.0-makepkg
v1_package_destination=$tmp_dir/jpacker-v1.16.0-packages
v1_archive_root=$tmp_dir/jpacker-v1.16.0-archive-root
v1_manifest=$tmp_dir/jpacker-v1.16.0-files.txt
v1_directory_manifest=$tmp_dir/jpacker-v1.16.0-directories.txt
v1_removable_manifest=$tmp_dir/jpacker-v1.16.0-removable-files.txt

v2_source=$tmp_dir/$PACKAGE_NAME-v$current_version-source
v2_source_manifest=$tmp_dir/$PACKAGE_NAME-v$current_version-source-files.txt
v2_makepkg_work=$tmp_dir/$PACKAGE_NAME-v$current_version-makepkg
v2_package_destination=$tmp_dir/$PACKAGE_NAME-v$current_version-packages
v2_archive_root=$tmp_dir/$PACKAGE_NAME-v$current_version-archive-root
v2_manifest=$tmp_dir/$PACKAGE_NAME-v$current_version-files.txt
v2_directory_manifest=$tmp_dir/$PACKAGE_NAME-v$current_version-directories.txt

coinstall_root=$tmp_dir/coinstall-root
transition_root=$tmp_dir/transition-root

if [ -n "$legacy_source_archive_input" ]; then
    v1_source_archive=$legacy_source_archive_input
else
    v1_source_archive=$tmp_dir/jpacker-v1.16.0-source.tar
    git -C "$repo_root" rev-parse --verify 'refs/tags/v1.16.0^{commit}' \
        >/dev/null || fail 'local tag v1.16.0 is unavailable'
    git -C "$repo_root" archive --format=tar \
        --output="$v1_source_archive" v1.16.0
fi
mkdir -p "$v1_source"
bsdtar -xf "$v1_source_archive" -C "$v1_source"

legacy_version=$(tr -d '[:space:]' <"$v1_source/VERSION")
[ "$legacy_version" = 1.16.0 ] ||
    fail "v1.16.0 archive reports VERSION=$legacy_version"
initialize_fixture_repository "$v1_source" v1.16.0
prepare_test_pkgbuild "$v1_source/PKGBUILD" "$v1_source" \
    "$v1_makepkg_work" \
    'git+https://github.com/seekerkrt/jpacker.git'

mkdir -p "$v2_source"
if [ -n "$current_source_archive_input" ]; then
    bsdtar -xf "$current_source_archive_input" -C "$v2_source"
else
    # Preserve the issue branch's dirty edits and tracked deletions while
    # keeping .git, ignored build output, binaries, and package artifacts out.
    v2_source_paths_raw=$v2_source_manifest.raw
    v2_source_manifest_temporary=$v2_source_manifest.tmp
    if validation_capture_output "$v2_source_paths_raw" \
        git -C "$repo_root" ls-files \
        --cached --others --exclude-standard; then
        :
    else
        source_status=$?
        fail "current source path producer failed with status $source_status; raw=$v2_source_paths_raw"
    fi
    if while IFS= read -r source_path; do
            case "$source_path" in
                .git|.git/*|build|build/*|"$COMMAND_NAME"|*.pkg.tar.*|*.src.tar.*)
                    continue
                    ;;
            esac
            if [ -f "$repo_root/$source_path" ] ||
                [ -L "$repo_root/$source_path" ]; then
                printf '%s\n' "$source_path" || exit $?
            fi
        done <"$v2_source_paths_raw" >"$v2_source_manifest_temporary"; then
        mv "$v2_source_manifest_temporary" "$v2_source_manifest"
    else
        source_status=$?
        fail "current source manifest normalization failed with status $source_status; partial=$v2_source_manifest_temporary"
    fi
    [ -s "$v2_source_manifest" ] || fail 'current source manifest is empty'
    while IFS= read -r source_path; do
        mkdir -p "$v2_source/$(dirname "$source_path")"
        cp -a "$repo_root/$source_path" "$v2_source/$source_path"
    done <"$v2_source_manifest"
fi

assert_absent "$v2_source/.git"
assert_absent "$v2_source/build"
assert_absent "$v2_source/$COMMAND_NAME"
assert_absent "$v2_source/config/jpacker.conf"
cmp -s "$repo_root/tests/test-package-transition.sh" \
    "$v2_source/tests/test-package-transition.sh" ||
    fail 'dirty working-tree transition test was not copied into v2 source'
first_package_artifact=$(find "$v2_source" -type f \
    \( -name '*.pkg.tar.*' -o -name '*.src.tar.*' \) -print -quit)
[ -z "$first_package_artifact" ] ||
    fail "source fixture contains package artifact $first_package_artifact"
v2_source_version=$(tr -d '[:space:]' < "$v2_source/VERSION")
[ "$v2_source_version" = "$current_version" ] ||
    fail "current source VERSION is $v2_source_version; expected $current_version"

initialize_fixture_repository "$v2_source" "v$current_version"
prepare_test_pkgbuild "$repo_root/PKGBUILD" "$v2_source" \
    "$v2_makepkg_work" \
    "git+$PROJECT_REPOSITORY_URL.git"

mkdir -p \
    "$v1_package_destination" \
    "$v2_package_destination" \
    "$v1_archive_root" \
    "$v2_archive_root"
run_logged 'jpacker v1.16.0 clean package build' "$tmp_dir/v1-makepkg.log" \
    run_makepkg_fixture \
        "$v1_makepkg_work" \
        "$tmp_dir/v1-build" \
        "$tmp_dir/v1-sources" \
        "$v1_package_destination" \
        "$tmp_dir/v1-source-packages" \
        "$tmp_dir/v1-logs" \
        "$tmp_dir/v1-xdg-cache"
run_logged "$PROJECT_NAME v$current_version clean package build" "$tmp_dir/v2-makepkg.log" \
    run_makepkg_fixture \
        "$v2_makepkg_work" \
        "$tmp_dir/v2-build" \
        "$tmp_dir/v2-sources" \
        "$v2_package_destination" \
        "$tmp_dir/v2-source-packages" \
        "$tmp_dir/v2-logs" \
        "$tmp_dir/v2-xdg-cache"

v1_package_archive=$v1_package_destination/jpacker-1.16.0-1-x86_64.pkg.tar.zst
v2_package_archive=$v2_package_destination/$PACKAGE_NAME-$current_version-
v2_package_archive=$v2_package_archive$PACKAGE_RELEASE-$PACKAGE_ARCHITECTURE.pkg.tar.zst
[ -f "$v1_package_archive" ] ||
    fail "expected package archive is missing: $v1_package_archive"
[ -f "$v2_package_archive" ] ||
    fail "expected package archive is missing: $v2_package_archive"
[ "$(count_command_output_lines v1-package-archives \
    find "$v1_package_destination" -maxdepth 1 -type f \
    -name '*.pkg.tar.*')" = 1 ] ||
    fail 'v1 package build produced an unexpected archive set'
[ "$(count_command_output_lines v2-package-archives \
    find "$v2_package_destination" -maxdepth 1 -type f \
    -name '*.pkg.tar.*')" = 1 ] ||
    fail 'v2 package build produced an unexpected archive set'
v1_package_hash=$(sha256_file "$v1_package_archive") ||
    fail 'v1 package checksum producer failed'
v2_package_hash=$(sha256_file "$v2_package_archive") ||
    fail 'v2 package checksum producer failed'
printf '%s  %s\n' "$v1_package_hash" \
    "$(basename "$v1_package_archive")" >"$tmp_dir/v1-package.sha256"
printf '%s  %s\n' "$v2_package_hash" \
    "$(basename "$v2_package_archive")" >"$tmp_dir/v2-package.sha256"

bsdtar -xOf "$v1_package_archive" .PKGINFO >"$tmp_dir/v1.PKGINFO"
bsdtar -xOf "$v2_package_archive" .PKGINFO >"$tmp_dir/v2.PKGINFO"
assert_metadata_single "$tmp_dir/v1.PKGINFO" pkgname jpacker
assert_metadata_single "$tmp_dir/v1.PKGINFO" pkgver 1.16.0-1
assert_metadata_single "$tmp_dir/v1.PKGINFO" arch x86_64
assert_metadata_single "$tmp_dir/v1.PKGINFO" license GPL-3.0-or-later
assert_metadata_single "$tmp_dir/v1.PKGINFO" backup \
    etc/jpacker/jpacker.conf

assert_metadata_single "$tmp_dir/v2.PKGINFO" pkgname "$PACKAGE_NAME"
assert_metadata_single "$tmp_dir/v2.PKGINFO" pkgver "$current_version-$PACKAGE_RELEASE"
assert_metadata_single "$tmp_dir/v2.PKGINFO" arch "$PACKAGE_ARCHITECTURE"
assert_metadata_single "$tmp_dir/v2.PKGINFO" license "$PACKAGE_LICENSE"
for transition_key in backup conflict conflicts provides replaces
do
    assert_metadata_absent "$tmp_dir/v2.PKGINFO" "$transition_key"
done

v2_dependencies_raw=$tmp_dir/v2-dependencies.raw
if validation_capture_output "$v2_dependencies_raw" \
    metadata_values "$tmp_dir/v2.PKGINFO" depend; then
    :
else
    dependency_status=$?
    fail "v2 dependency producer failed with status $dependency_status; raw=$v2_dependencies_raw"
fi

libalpm_dependencies=$tmp_dir/v2-libalpm-dependencies.raw
if grep -E '^libalpm\.so(=[0-9]+-[0-9]+)?$' \
    "$v2_dependencies_raw" >"$libalpm_dependencies"; then
    :
else
    grep_status=$?
    case "$grep_status" in
        1) : >"$libalpm_dependencies" ;;
        *) fail "libalpm dependency filter failed with status $grep_status" ;;
    esac
fi
if libalpm_dependency_count=$(wc -l <"$libalpm_dependencies"); then
    :
else
    count_status=$?
    fail "libalpm dependency count failed with status $count_status"
fi
[ "$libalpm_dependency_count" = 1 ] ||
    fail 'v2 package has an invalid libalpm soname dependency'

normalized_dependencies_raw=$tmp_dir/v2-dependencies.normalized.raw
normalized_dependencies_sorted=$tmp_dir/v2-dependencies.normalized.sorted
if sed -E 's/^libalpm\.so(=[0-9]+-[0-9]+)?$/libalpm.so/' \
    "$v2_dependencies_raw" >"$normalized_dependencies_raw"; then
    :
else
    normalize_status=$?
    fail "dependency normalization failed with status $normalize_status"
fi
if LC_ALL=C sort "$normalized_dependencies_raw" \
    >"$normalized_dependencies_sorted"; then
    normalized_dependencies=$(cat "$normalized_dependencies_sorted")
else
    sort_status=$?
    fail "dependency sorting failed with status $sort_status"
fi
if expected_dependencies=$(cat "$runtime_dependency_authority"); then
    :
else
    dependency_status=$?
    fail "runtime dependency authority read failed with status $dependency_status"
fi
[ "$normalized_dependencies" = "$expected_dependencies" ] || {
    printf 'package-transition-test: depend mismatch\nexpected:\n%s\nactual:\n%s\n' \
        "$expected_dependencies" "$normalized_dependencies" >&2
    exit 1
}
if expected_makedepends=$(cat "$build_dependency_authority"); then
    :
else
    dependency_status=$?
    fail "build dependency authority read failed with status $dependency_status"
fi
assert_metadata_set "$tmp_dir/v2.PKGINFO" makedepend "$expected_makedepends"

assert_archive_layout "$v1_package_archive" \
    "$tmp_dir/v1-archive-listing.txt" usr/bin/jpacker
assert_archive_layout "$v2_package_archive" \
    "$tmp_dir/v2-archive-listing.txt" \
    "usr/bin/$COMMAND_NAME" \
    usr/libexec/moguet/moguet-alpm-receipt-helper \
    usr/libexec/moguet/moguet-source-artifact-install-helper \
    usr/libexec/moguet/moguet-repository-prefix-helper \
    usr/libexec/moguet/moguet-repository-prefix-worker
bsdtar -xf "$v1_package_archive" -C "$v1_archive_root"
bsdtar -xf "$v2_package_archive" -C "$v2_archive_root"
assert_no_symlinks "$v1_archive_root"
assert_no_symlinks "$v2_archive_root"
write_regular_manifest "$v1_archive_root" "$v1_manifest"
write_directory_manifest "$v1_archive_root" "$v1_directory_manifest"
write_regular_manifest "$v2_archive_root" "$v2_manifest"
write_directory_manifest "$v2_archive_root" "$v2_directory_manifest"

expected_v2_payload=$tmp_dir/expected-v2-package-payload.txt
if sed -e 's|^/|./|' -e '/\/share\/man\// s/\.1$/.1.gz/' \
    "$install_payload_authority" >"$expected_v2_payload"; then
    :
else
    payload_status=$?
    fail "archive payload projection failed with status $payload_status"
fi
if ! cmp -s "$expected_v2_payload" "$v2_manifest"; then
    printf 'package-transition-test: package payload mismatch:\n%s\n' \
        "$(cat "$v2_manifest")" >&2
    exit 1
fi

archive_binary=$v2_archive_root/usr/bin/$COMMAND_NAME
archive_binary_mode=$(stat -c '%a' "$archive_binary")
[ "$archive_binary_mode" = 755 ] ||
    fail "archived $PROJECT_NAME binary mode is $archive_binary_mode; expected 755"
archive_home=$tmp_dir/archive-home
archive_config_home=$tmp_dir/archive-xdg/config
archive_state_home=$tmp_dir/archive-xdg/state
archive_cache_home=$tmp_dir/archive-xdg/cache
archive_version=$(LC_ALL=C \
    HOME="$archive_home" \
    XDG_CONFIG_HOME="$archive_config_home" \
    XDG_STATE_HOME="$archive_state_home" \
    XDG_CACHE_HOME="$archive_cache_home" \
    "$archive_binary" --version)
[ "$archive_version" = "$PROJECT_NAME v$current_version" ] ||
    fail "archived $PROJECT_NAME version mismatch: $archive_version"
assert_absent "$archive_config_home"
assert_absent "$archive_state_home"
assert_absent "$archive_cache_home"

file_conflicts=$tmp_dir/regular-file-conflicts.txt
LC_ALL=C comm -12 "$v1_manifest" "$v2_manifest" >"$file_conflicts"
[ ! -s "$file_conflicts" ] || {
    printf 'package-transition-test: v1/v2 regular-file conflicts:\n' >&2
    sed -n '1,240p' "$file_conflicts" >&2
    exit 1
}
type_conflicts=$tmp_dir/file-directory-conflicts.txt
v1_directories_sorted=$tmp_dir/v1-directories-sorted.txt
v2_directories_sorted=$tmp_dir/v2-directories-sorted.txt
LC_ALL=C sort "$v1_directory_manifest" >"$v1_directories_sorted"
LC_ALL=C sort "$v2_directory_manifest" >"$v2_directories_sorted"
{
    LC_ALL=C comm -12 "$v1_manifest" "$v2_directories_sorted"
    LC_ALL=C comm -12 "$v2_manifest" "$v1_directories_sorted"
} >"$type_conflicts"
[ ! -s "$type_conflicts" ] || {
    printf 'package-transition-test: v1/v2 file-directory conflicts:\n' >&2
    sed -n '1,240p' "$type_conflicts" >&2
    exit 1
}

# Moguet provides no legacy command alias and owns no legacy/system config.
assert_absent "$v2_archive_root/usr/bin/jpacker"
assert_absent "$v2_archive_root/usr/share/bash-completion/completions/jpacker"
assert_absent "$v2_archive_root/usr/share/zsh/site-functions/_jpacker"
assert_absent "$v2_archive_root/usr/share/fish/vendor_completions.d/jpacker.fish"
assert_absent "$v2_archive_root/usr/share/man/man8/jpacker.8"
assert_absent "$v2_archive_root/usr/share/man/man8/jpacker.8.gz"
assert_absent "$v2_archive_root/etc"
assert_absent "$v2_archive_root/home"

# The legacy config is user-modifiable package data. package.build entries and
# paths outside either manifest model runtime-managed and foreign data.
mkdir -p "$coinstall_root"
bsdtar -xf "$v1_package_archive" -C "$coinstall_root" etc usr
modified_legacy_config='NOEDIT=true
NODIFF=true'
legacy_preference='CFLAGS=-O3 -march=native'
coinstall_config_dir=$coinstall_root/user-home/.config/$XDG_IDENTITY
coinstall_source_preference_dir=$coinstall_config_dir/source-build.d
coinstall_source_preference=$coinstall_source_preference_dir/fastfetch
mkdir -p \
    "$coinstall_root/etc/jpacker/package.build" \
    "$coinstall_config_dir" \
    "$coinstall_root/user-home/.local/state/$XDG_IDENTITY" \
    "$coinstall_root/user-home/.cache/$XDG_IDENTITY" \
    "$coinstall_root/usr/share/doc/$PACKAGE_NAME" \
    "$coinstall_root/usr/share/locale/ja/LC_MESSAGES"
printf '%s\n' "$modified_legacy_config" \
    >"$coinstall_root/etc/jpacker/jpacker.conf"
printf '%s\n' "$legacy_preference" \
    >"$coinstall_root/etc/jpacker/package.build/fastfetch"
printf '%s\n' 'legacy foreign data' \
    >"$coinstall_root/etc/jpacker/foreign-file.keep"
printf '%s\n' 'schema_version = 1' \
    >"$coinstall_config_dir/config.toml"
install -d -m700 "$coinstall_source_preference_dir"
install -m600 /dev/null "$coinstall_source_preference"
printf '%s\n' 'CFLAGS=-O2 -pipe' >"$coinstall_source_preference"
printf '%s\n' 'persistent state' \
    >"$coinstall_root/user-home/.local/state/$XDG_IDENTITY/state.keep"
printf '%s\n' 'reproducible cache fixture' \
    >"$coinstall_root/user-home/.cache/$XDG_IDENTITY/cache.keep"
printf '%s\n' 'foreign documentation' \
    >"$coinstall_root/usr/share/doc/$PACKAGE_NAME/foreign-file.keep"
printf '%s\n' 'foreign locale catalog' \
    >"$coinstall_root/usr/share/locale/ja/LC_MESSAGES/foreign-domain.mo"

coinstall_baseline=$tmp_dir/coinstall-baseline.sha256
write_checksum_snapshot "$coinstall_root" "$coinstall_baseline"

# The two actual package archives coinstall without overwriting v1 or user
# data. No host pacman database, /etc tree, user XDG tree, or sudo is involved.
bsdtar -xf "$v2_package_archive" -C "$coinstall_root" usr
assert_snapshot_matches "$coinstall_root" "$coinstall_baseline"
assert_manifest_files_match "$v2_archive_root" "$coinstall_root" "$v2_manifest"

remove_package_payload "$coinstall_root" "$v2_manifest" \
    "$v2_directory_manifest"
assert_manifest_absent "$coinstall_root" "$v2_manifest"
assert_snapshot_exact "$coinstall_root" "$coinstall_baseline"
assert_text_file "$coinstall_root/etc/jpacker/jpacker.conf" \
    "$modified_legacy_config"
assert_text_file "$coinstall_root/etc/jpacker/package.build/fastfetch" \
    "$legacy_preference"
assert_text_file "$coinstall_source_preference" 'CFLAGS=-O2 -pipe'
assert_mode "$coinstall_source_preference_dir" 700
assert_mode "$coinstall_source_preference" 600

# Reinstalling the same archive restores only its exact payload and preserves
# the v1 package, legacy data, user XDG data, and foreign files byte-for-byte.
bsdtar -xf "$v2_package_archive" -C "$coinstall_root" usr
assert_snapshot_matches "$coinstall_root" "$coinstall_baseline"
assert_manifest_files_match "$v2_archive_root" "$coinstall_root" "$v2_manifest"
assert_mode "$coinstall_source_preference_dir" 700
assert_mode "$coinstall_source_preference" 600

# Removing v1 follows its .PKGINFO backup contract: a modified package-owned
# config becomes .pacsave, while runtime-managed and foreign legacy data stays.
mkdir -p "$transition_root"
bsdtar -xf "$v1_package_archive" -C "$transition_root" etc usr
transition_config_dir=$transition_root/user-home/.config/$XDG_IDENTITY
transition_source_preference_dir=$transition_config_dir/source-build.d
transition_source_preference=$transition_source_preference_dir/fastfetch
mkdir -p \
    "$transition_root/etc/jpacker/package.build" \
    "$transition_config_dir" \
    "$transition_root/user-home/.local/state/$XDG_IDENTITY" \
    "$transition_root/user-home/.cache/$XDG_IDENTITY"
printf '%s\n' "$modified_legacy_config" \
    >"$transition_root/etc/jpacker/jpacker.conf"
printf '%s\n' "$legacy_preference" \
    >"$transition_root/etc/jpacker/package.build/fastfetch"
printf '%s\n' 'legacy transition sentinel' \
    >"$transition_root/etc/jpacker/foreign-file.keep"
printf '%s\n' 'schema_version = 1' \
    >"$transition_config_dir/config.toml"
install -d -m700 "$transition_source_preference_dir"
install -m600 /dev/null "$transition_source_preference"
printf '%s\n' 'CFLAGS=-O2 -pipe' >"$transition_source_preference"
printf '%s\n' 'persistent transition state' \
    >"$transition_root/user-home/.local/state/$XDG_IDENTITY/state.keep"
printf '%s\n' 'transition cache fixture' \
    >"$transition_root/user-home/.cache/$XDG_IDENTITY/cache.keep"

expected_legacy_after_removal=$tmp_dir/expected-legacy-after-removal
mkdir -p "$expected_legacy_after_removal"
cp -a "$transition_root/etc/jpacker/." "$expected_legacy_after_removal/"
mv "$expected_legacy_after_removal/jpacker.conf" \
    "$expected_legacy_after_removal/jpacker.conf.pacsave"
legacy_transition_snapshot=$tmp_dir/legacy-transition.sha256
xdg_transition_snapshot=$tmp_dir/xdg-transition.sha256
write_checksum_snapshot "$expected_legacy_after_removal" \
    "$legacy_transition_snapshot"
write_checksum_snapshot "$transition_root/user-home" "$xdg_transition_snapshot"
grep -Fvx './etc/jpacker/jpacker.conf' "$v1_manifest" \
    >"$v1_removable_manifest"

mv "$transition_root/etc/jpacker/jpacker.conf" \
    "$transition_root/etc/jpacker/jpacker.conf.pacsave"
remove_package_payload "$transition_root" "$v1_manifest" \
    "$v1_directory_manifest"
assert_manifest_absent "$transition_root" "$v1_manifest"
assert_snapshot_exact "$transition_root/etc/jpacker" \
    "$legacy_transition_snapshot"
assert_snapshot_exact "$transition_root/user-home" "$xdg_transition_snapshot"
assert_text_file "$transition_root/etc/jpacker/jpacker.conf.pacsave" \
    "$modified_legacy_config"
assert_text_file "$transition_source_preference" 'CFLAGS=-O2 -pipe'
assert_mode "$transition_source_preference_dir" 700
assert_mode "$transition_source_preference" 600

bsdtar -xf "$v2_package_archive" -C "$transition_root" usr
assert_manifest_files_match "$v2_archive_root" "$transition_root" "$v2_manifest"
assert_snapshot_exact "$transition_root/etc/jpacker" \
    "$legacy_transition_snapshot"
assert_snapshot_exact "$transition_root/user-home" "$xdg_transition_snapshot"
assert_mode "$transition_source_preference_dir" 700
assert_mode "$transition_source_preference" 600

# Rollback removes only archive-owned Moguet files, reinstalls the trusted v1
# archive, then manually restores its pacsaved config over the package default.
remove_package_payload "$transition_root" "$v2_manifest" \
    "$v2_directory_manifest"
assert_manifest_absent "$transition_root" "$v2_manifest"
bsdtar -xf "$v1_package_archive" -C "$transition_root" etc usr
cp "$transition_root/etc/jpacker/jpacker.conf.pacsave" \
    "$transition_root/etc/jpacker/jpacker.conf"

assert_manifest_files_match "$v1_archive_root" "$transition_root" \
    "$v1_removable_manifest"
assert_manifest_absent "$transition_root" "$v2_manifest"
assert_snapshot_exact "$transition_root/user-home" "$xdg_transition_snapshot"
assert_text_file "$transition_root/etc/jpacker/jpacker.conf" \
    "$modified_legacy_config"
assert_text_file "$transition_root/etc/jpacker/jpacker.conf.pacsave" \
    "$modified_legacy_config"
assert_text_file "$transition_root/etc/jpacker/package.build/fastfetch" \
    "$legacy_preference"
assert_text_file "$transition_root/etc/jpacker/foreign-file.keep" \
    'legacy transition sentinel'
assert_text_file "$transition_source_preference" 'CFLAGS=-O2 -pipe'
assert_mode "$transition_source_preference_dir" 700
assert_mode "$transition_source_preference" 600

rollback_version=$(LC_ALL=C \
    HOME="$tmp_dir/rollback-home" \
    "$transition_root/usr/bin/jpacker" --version)
[ "$rollback_version" = 'jpacker v1.16.0' ] ||
    fail "rollback jpacker version mismatch: $rollback_version"

(cd "$v1_package_destination" && \
    sha256sum -c "$tmp_dir/v1-package.sha256" >/dev/null) ||
    fail 'v1 package archive changed during transition validation'
(cd "$v2_package_destination" && \
    sha256sum -c "$tmp_dir/v2-package.sha256" >/dev/null) ||
    fail 'v2 package archive changed during transition validation'

if v2_payload_count=$(wc -l <"$v2_manifest"); then
    :
else
    payload_count_status=$?
    fail "verified payload count failed with status $payload_count_status"
fi
case "$v2_payload_count" in
    ''|*[!0-9]*) fail "verified payload count is invalid: $v2_payload_count" ;;
esac

printf 'package-transition-test: actual v1/v2 package archives have 0 path conflicts\n'
printf 'package-transition-test: v2 .PKGINFO identity/dependencies and transition metadata passed\n'
printf 'package-transition-test: archive owners, modes, and entry types passed\n'
printf 'package-transition-test: %s archive payload (%s regular files):\n' \
    "$PROJECT_NAME" "$v2_payload_count"
sed 's|^\./|/|' "$v2_manifest"
printf 'package-transition-test: all checks passed\n'
