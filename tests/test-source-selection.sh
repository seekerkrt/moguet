#!/bin/sh
set -eu
umask 077

# Assertions target the canonical untranslated CLI output.
# Do not inherit locale settings from the invoking environment.
LANG=C
LC_ALL=C
export LANG LC_ALL
unset LANGUAGE

test_binary=$1
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
MOGUET_TEST_REPOSITORY_ROOT=$repo_root
export MOGUET_TEST_REPOSITORY_ROOT
. "$repo_root/tests/test-command-safety.sh"
. "$repo_root/scripts/validation-status.sh"
tmp_dir=$(mktemp -d)
server_pid=

cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

port_file=$tmp_dir/port
request_log=$tmp_dir/aur-requests.log
: > "$request_log"
python3 "$repo_root/tests/aur_rpc_fixture_server.py" \
    "$repo_root/tests/fixtures/aur-rpc-conflicts-replaces.json" \
    "$port_file" "$request_log" &
server_pid=$!

attempt=0
while [ ! -s "$port_file" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -gt 100 ]; then
        echo "fixture server did not start" >&2
        exit 1
    fi
    sleep 0.05
done

port=$(cat "$port_file")
export PATH=$repo_root/tests/stubs:/usr/bin:/bin
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
require_exact_test_command makepkg "$repo_root/tests/stubs/makepkg"
require_exact_test_command pacman "$repo_root/tests/stubs/pacman"
require_exact_test_command sudo "$repo_root/tests/stubs/sudo"
require_exact_test_command git "$repo_root/tests/stubs/git"
export MOGUET_TEST_AUR_RPC_BASE_URL=http://127.0.0.1:$port/rpc/

setup_case() {
    case_name=$1
    case_dir=$tmp_dir/cases/$case_name
    command_log=$case_dir/commands.log
    output_file=$case_dir/output
    package_metadata_state=$case_dir/package-metadata.state
    repository_metadata_state=$case_dir/repository-metadata.state

    mkdir -p \
        "$case_dir/home" "$case_dir/xdg-config" \
        "$case_dir/xdg-state" "$case_dir/xdg-cache"
    : > "$command_log"
    : > "$package_metadata_state"
    : > "$repository_metadata_state"
    : > "$request_log"
    export HOME=$case_dir/home
    export XDG_CONFIG_HOME=$case_dir/xdg-config
    export XDG_STATE_HOME=$case_dir/xdg-state
    export XDG_CACHE_HOME=$case_dir/xdg-cache
    preference_dir=$XDG_CONFIG_HOME/moguet/source-build.d
    export MOGUET_TEST_COMMAND_LOG=$command_log
    export MOGUET_TEST_PACKAGE_METADATA_STATE_FILE=$package_metadata_state
    export MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE=$repository_metadata_state
    export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
    export MOGUET_TEST_PACMAN_EXIT_CODE=1
    export MOGUET_TEST_SUDO_EXIT_CODE=0
    unset MOGUET_TEST_PACMAN_QM_OUTPUT
    unset MOGUET_TEST_PACMAN_REPO_PACKAGES
    unset MOGUET_TEST_GIT_REMOTE_URL
    unset MOGUET_TEST_GIT_CLONE_EXIT_CODE
    unset MOGUET_TEST_GIT_CLONE_SYMLINK_TARGET
    unset MOGUET_TEST_GIT_CLONE_FIXTURE_DIR
    unset MOGUET_TEST_MAKEPKG_EXIT_CODE
    unset MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE
    unset MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES
    unset MOGUET_TEST_MAKEPKG_ENV_LOG
    unset MOGUET_TEST_MAKEPKG_ENV_KEYS
    unset MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE
    unset MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_FAILURE_AT
    unset MOGUET_TEST_SOURCE_PREFERENCE_EXTERNAL
    unset MOGUET_TEST_EDITOR_REPLACE_TARGET
    unset MOGUET_TEST_EDITOR_SYMLINK_TARGET
    unset MOGUET_TEST_EDITOR_REMOVE_TARGET
    unset DUP
    unset EMPTY
    unset UNDEFINED
    unset EDITOR
    unset VISUAL
}

prepare_preference_store() {
    mkdir -p "$preference_dir"
    chmod 700 "$XDG_CONFIG_HOME/moguet" "$preference_dir"
}

run_ok() {
    : > "$command_log"
    : > "$request_log"
    if ! "$test_binary" "$@" </dev/null > "$output_file" 2>&1; then
        echo "expected command to succeed: $*" >&2
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_status() {
    expected_status=$1
    shift
    : > "$command_log"
    : > "$request_log"
    if ! validation_expect_status source-selection-expected-failure \
        "$expected_status" \
        "$output_file" "$output_file" "$test_binary" "$@" </dev/null; then
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_fail() {
    run_status 1 "$@"
}

assert_contains() {
    pattern=$1
    file=$2
    if ! grep -F -- "$pattern" "$file" >/dev/null; then
        echo "missing expected output: $pattern" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_line() {
    expected=$1
    file=$2
    if ! grep -Fx -- "$expected" "$file" >/dev/null; then
        echo "missing expected line: $expected" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_file_content() {
    actual_file=$1
    expected=$2
    expected_file=$case_dir/expected-file-content
    printf '%s\n' "$expected" > "$expected_file"
    if ! cmp -s "$expected_file" "$actual_file"; then
        echo "unexpected file content: $actual_file" >&2
        diff -u "$expected_file" "$actual_file" >&2 || true
        exit 1
    fi
}

assert_line_before() {
    first=$1
    second=$2
    file=$3
    assert_line "$first" "$file"
    assert_line "$second" "$file"
    first_line_number=$(grep -nFx -- "$first" "$file" | sed -n '1s/:.*//p')
    second_line_number=$(grep -nFx -- "$second" "$file" | sed -n '1s/:.*//p')
    if [ "$first_line_number" -ge "$second_line_number" ]; then
        echo "unexpected line order: $first" >&2
        echo "must appear before: $second" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_output_before() {
    first=$1
    second=$2
    file=$3
    assert_contains "$first" "$file"
    assert_contains "$second" "$file"
    first_line_number=$(grep -nF -- "$first" "$file" | sed -n '1s/:.*//p')
    second_line_number=$(grep -nF -- "$second" "$file" | sed -n '1s/:.*//p')
    if [ "$first_line_number" -ge "$second_line_number" ]; then
        echo "unexpected output order: $first" >&2
        echo "must appear before: $second" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_not_contains() {
    pattern=$1
    file=$2
    if grep -F -- "$pattern" "$file" >/dev/null; then
        echo "unexpected output: $pattern" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_command() {
    expected=$1
    if ! grep -Fx -- "$expected" "$command_log" >/dev/null; then
        echo "missing expected command: $expected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_absent() {
    unexpected=$1
    if grep -Fx -- "$unexpected" "$command_log" >/dev/null; then
        echo "unexpected command: $unexpected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_pattern() {
    expected_pattern=$1
    if ! grep -E -- "$expected_pattern" "$command_log" >/dev/null; then
        echo "missing expected command pattern: $expected_pattern" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_pattern_count() {
    expected_count=$1
    expected_pattern=$2
    actual_count=$(validation_grep_count -Ec -- \
        "$expected_pattern" "$command_log")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected command pattern count: $actual_count (expected $expected_count)" >&2
        echo "pattern: $expected_pattern" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_repository_read_counts() {
    strict_repository_reads=$1
    additional_database_path_reads=$2

    assert_command_pattern_count \
        "$((strict_repository_reads + additional_database_path_reads))" \
        '^pacman-conf --verbose RootDir DBPath$'
    assert_command_pattern_count \
        "$strict_repository_reads" \
        '^pacman-conf --repo-list$'
}

assert_command_pattern_absent() {
    unexpected_pattern=$1
    if grep -E -- "$unexpected_pattern" "$command_log" >/dev/null; then
        echo "unexpected command pattern: $unexpected_pattern" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_only_command() {
    expected=$1
    assert_command "$expected"
    if [ "$(wc -l < "$command_log")" -ne 1 ]; then
        echo "unexpected additional command(s)" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_at() {
    line_number=$1
    expected=$2
    actual=$(sed -n "${line_number}p" "$command_log")
    if [ "$actual" != "$expected" ]; then
        echo "unexpected command at line $line_number: $actual" >&2
        echo "expected: $expected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_count() {
    expected=$1
    actual=$(wc -l < "$command_log")
    if [ "$actual" -ne "$expected" ]; then
        echo "unexpected command count: $actual (expected $expected)" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_log_empty() {
    if [ -s "$command_log" ]; then
        echo "external command ran before source selection validation completed" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_request_log_empty() {
    if [ -s "$request_log" ]; then
        echo "unexpected AUR RPC request" >&2
        cat "$request_log" >&2
        exit 1
    fi
}

assert_request_log_nonempty() {
    if [ ! -s "$request_log" ]; then
        echo "expected an AUR RPC request" >&2
        exit 1
    fi
}

assert_cache_root_absent() {
    if [ -e "$XDG_CACHE_HOME/moguet" ] || [ -L "$XDG_CACHE_HOME/moguet" ]; then
        echo "Moguet cache root was created before source selection validation completed" >&2
        exit 1
    fi
}

assert_no_mutation_commands() {
    if grep -E '^(sudo|git|makepkg) |^pacman -S( |$)' "$command_log" >/dev/null; then
        echo "source selection preflight allowed an external mutation command" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_no_repo_search_command() {
    if grep -E '^(pacman|sudo pacman) -Ss?( |$)' "$command_log" >/dev/null; then
        echo "AUR-only search reached pacman search" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_no_repo_info_command() {
    if grep -E '^(pacman|sudo pacman) -Si( |$)' "$command_log" >/dev/null; then
        echo "AUR-only info reached pacman -Si" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_no_source_build_commands() {
    if grep -E '^(git|makepkg) ' "$command_log" >/dev/null; then
        echo "repository-only route reached source build command" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_cache_entry_absent() {
    entry=$XDG_CACHE_HOME/moguet/$1
    if [ -e "$entry" ] || [ -L "$entry" ]; then
        echo "cache entry was created before all targets passed preflight: $entry" >&2
        exit 1
    fi
}

prepare_source_preference() {
    package=$1
    prepare_preference_store
    printf 'CFLAGS=-Osource-selection-test\n' > "$preference_dir/$package"
    chmod 600 "$preference_dir/$package"
}

write_repository_package() {
    package=$1
    printf 'core %s 1 1\n' "$package" >> "$repository_metadata_state"
}

run_exact() {
    case_name=$1
    expected=$2
    shift 2
    setup_case "$case_name"
    export MOGUET_TEST_PACMAN_EXIT_CODE=0
    run_ok "$@"
    assert_only_command "$expected"
    assert_request_log_empty
}

# Source preferenceのpath/read契約。列挙順はfilesystem依存なので固定しない。
setup_case list-src-missing-root
LC_ALL=C LANGUAGE= run_ok list-src
assert_contains "No source-build packages registered." "$output_file"
assert_command_log_empty
assert_request_log_empty
if [ -e "$preference_dir" ] || [ -L "$preference_dir" ]; then
    echo "list-src created the missing source preference root" >&2
    exit 1
fi

setup_case list-src-regular-entries
prepare_preference_store
cat > "$preference_dir/alpha" <<'SOURCE_PREFERENCE'
  # comment-only line
    CFLAGS = "-O2 # kept quoted"   # removed trailing comment
    raw value without equals        # removed raw comment

SOURCE_PREFERENCE
printf '%s\n' "LDFLAGS='-Wl,--as-needed'" > "$preference_dir/zeta"
chmod 600 "$preference_dir/alpha" "$preference_dir/zeta"
LC_ALL=C LANGUAGE= run_ok list-src
assert_contains "Registered Source Packages:" "$output_file"
assert_contains "alpha" "$output_file"
assert_line_before '    CFLAGS = "-O2 # kept quoted"' \
    "    raw value without equals" "$output_file"
assert_contains "zeta" "$output_file"
assert_line "    LDFLAGS='-Wl,--as-needed'" "$output_file"
assert_not_contains "removed trailing comment" "$output_file"
assert_not_contains "removed raw comment" "$output_file"
assert_command_log_empty
assert_request_log_empty

setup_case list-src-nonregular-is-atomic-hard-error
prepare_preference_store
printf 'VISIBLE=must-not-print\n' > "$preference_dir/alpha"
chmod 600 "$preference_dir/alpha"
mkdir "$preference_dir/invalid-directory"
LC_ALL=C LANGUAGE= run_fail list-src
assert_contains "not a regular file" "$output_file"
assert_not_contains "VISIBLE=must-not-print" "$output_file"
assert_not_contains "Registered Source Packages:" "$output_file"
assert_command_log_empty
assert_request_log_empty

setup_case list-src-empty-root
prepare_preference_store
LC_ALL=C LANGUAGE= run_ok list-src
assert_contains "Registered Source Packages:" "$output_file"
assert_contains "  (none)" "$output_file"
assert_command_log_empty
assert_request_log_empty

# Source preference mutationはnative filesystem APIだけを使う。
setup_case add-src-handler
add_src_path=$preference_dir/clean-root
run_ok add-src clean-root CFLAGS=-O2
assert_contains "Added clean-root to source-build list." "$output_file"
assert_contains "Appending CFLAGS=-O2 to $add_src_path" "$output_file"
assert_command_log_empty
if [ ! -f "$add_src_path" ] || [ -L "$add_src_path" ]; then
    echo "add-src did not create a regular canonical entry" >&2
    exit 1
fi
assert_file_content "$add_src_path" 'CFLAGS=-O2'
if [ "$(stat -c '%a' "$preference_dir")" != "700" ] ||
   [ "$(stat -c '%a' "$add_src_path")" != "600" ]; then
    echo "add-src created unsafe source preference modes" >&2
    exit 1
fi

setup_case edit-src-handler
prepare_preference_store
edit_src_path=$preference_dir/clean-root
printf 'CFLAGS=-Oexisting\n' > "$edit_src_path"
chmod 600 "$edit_src_path"
export EDITOR="moguet-test-editor --wait"
run_ok edit-src clean-root
editor_command=$(sed -n '1p' "$command_log")
editor_prefix="moguet-test-editor --wait /tmp/moguet-edit-src-clean-root."
case $editor_command in
    "$editor_prefix"??????)
        ;;
    *)
        echo "unexpected edit-src editor command: $editor_command" >&2
        cat "$command_log" >&2
        exit 1
        ;;
esac
edit_temp_path=${editor_command#moguet-test-editor --wait }
assert_command_count 1
if [ -e "$edit_temp_path" ] || [ -L "$edit_temp_path" ]; then
    echo "edit-src did not remove its temporary file after successful install" >&2
    exit 1
fi
assert_file_content "$edit_src_path" 'CFLAGS=-Oexisting'
if [ "$(stat -c '%a' "$edit_src_path")" != "600" ]; then
    echo "edit-src did not preserve private entry mode" >&2
    exit 1
fi

setup_case del-src-handler
prepare_preference_store
del_src_path=$preference_dir/clean-root
printf 'CFLAGS=-Oexisting\n' > "$del_src_path"
chmod 600 "$del_src_path"
run_ok del-src clean-root
assert_contains "Removing clean-root from list..." "$output_file"
assert_command_log_empty
if [ -e "$del_src_path" ] || [ -L "$del_src_path" ]; then
    echo "del-src did not remove the canonical entry" >&2
    exit 1
fi

setup_case revert-handler
prepare_preference_store
revert_src_path=$preference_dir/official-a
printf 'CFLAGS=-Oexisting\n' > "$revert_src_path"
chmod 600 "$revert_src_path"
write_repository_package official-a
run_ok revert official-a
assert_contains "Unmarking source-build for official-a" "$output_file"
assert_contains "official-a exists in official repos. Will reinstall binary." "$output_file"
assert_contains "Reinstalling binaries: 'official-a'" "$output_file"
assert_command_at 1 "pacman-conf --verbose RootDir DBPath"
assert_command_at 2 "pacman-conf --repo-list"
assert_command_at 3 "sudo pacman -S official-a"
assert_command_count 3
if [ -e "$revert_src_path" ] || [ -L "$revert_src_path" ]; then
    echo "revert did not remove the canonical entry" >&2
    exit 1
fi

# Auto source selectionはpreference lookupやrepository/AUR probeより先にpackage名を検証する。
for dot_target in . ..; do
    case $dot_target in
        .) dot_target_label=dot ;;
        ..) dot_target_label=dot-dot ;;
    esac
    setup_case "auto-install-reject-$dot_target_label"
    prepare_preference_store
    printf 'SOURCE_PREFERENCE_GUARD=yes\n' > "$preference_dir/root-guard"
    chmod 600 "$preference_dir/root-guard"
    preference_checksum=$(cksum "$preference_dir/root-guard")

    run_fail -S "$dot_target"

    assert_contains "Invalid package name: $dot_target" "$output_file"
    assert_command_log_empty
    assert_request_log_empty
    if [ "$(cksum "$preference_dir/root-guard")" != "$preference_checksum" ]; then
        echo "source selection changed the source preference fixture for $dot_target" >&2
        exit 1
    fi
done

# Matrix A: selector parse、option value、opaque operand、conflict。
run_exact option-value-aur \
    "pacman -Q --root --aur filesystem" \
    -Q --root --aur filesystem
run_exact option-value-repo \
    "pacman -Q --config --repo filesystem" \
    -Q --config --repo filesystem

setup_case opaque-aur
run_ok -U -- --aur
assert_only_command "sudo pacman -U -- --aur"
assert_request_log_empty

setup_case opaque-repo
run_ok -U -- --repo
assert_only_command "sudo pacman -U -- --repo"
assert_request_log_empty

setup_case opaque-sync-target
run_fail -S -- --aur
assert_contains "Invalid package name: --aur" "$output_file"
assert_command_log_empty
assert_request_log_empty

setup_case selector-before-operation
run_ok --repo -S official-a
assert_only_command "sudo pacman -S official-a"
assert_request_log_empty

setup_case selector-after-operation
run_ok -S --repo official-a
assert_only_command "sudo pacman -S official-a"
assert_request_log_empty

setup_case aur-selector-before-operation
run_ok --aur -Ss virtual-dep-150
assert_contains "provider-a" "$output_file"
assert_command_log_empty
assert_request_log_nonempty

setup_case duplicate-selector
run_ok --repo -S --repo official-a
assert_only_command "sudo pacman -S official-a"
assert_request_log_empty

setup_case duplicate-aur-selector
run_ok --aur -Ss --aur virtual-dep-150
assert_contains "provider-a" "$output_file"
assert_command_log_empty
assert_request_log_nonempty

# selector付きでもseparated operation modifierをcanonical -Ss/-Siと同じrouteへ送る。
setup_case separated-aur-search-short
run_ok -S -s --aur virtual-dep-150
assert_contains "provider-a" "$output_file"
assert_command_log_empty
assert_request_log_nonempty

setup_case separated-repo-search-long
export MOGUET_TEST_PACMAN_EXIT_CODE=0
run_ok -S --search --repo keyword
assert_only_command "pacman -S --search keyword"
assert_request_log_empty

setup_case separated-aur-info-long
run_ok -S --info --aur clean-root
assert_contains "Name            : clean-root" "$output_file"
assert_no_repo_info_command
assert_request_log_nonempty

setup_case separated-repo-info-short
export MOGUET_TEST_PACMAN_EXIT_CODE=0
run_ok -S -i --repo filesystem
assert_only_command "pacman -S -i filesystem"
assert_request_log_empty

# POLICY: Autoはoperation文字列だけでsearch/infoを分類し、separated modifierをinstall routeで扱う。
# selector routeとの現行非対称を共通化しないため、official targetでcall pathを固定する。
setup_case separated-auto-search-short
write_repository_package official-a
run_ok -S -s official-a
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "pacman-conf --repo-list"
assert_command "sudo pacman -S -s official-a"
assert_repository_read_counts 1 0
assert_command_count 3
assert_request_log_empty

setup_case separated-auto-info-short
write_repository_package official-a
run_ok -S -i official-a
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "pacman-conf --repo-list"
assert_command "sudo pacman -S -i official-a"
assert_repository_read_counts 1 0
assert_command_count 3
assert_request_log_empty

conflict_index=0
for conflict_order in \
    "--aur --repo -S conflict-target" \
    "--repo --aur -S conflict-target" \
    "-S --aur --repo conflict-target" \
    "-S --repo --aur conflict-target"
do
    conflict_index=$((conflict_index + 1))
    setup_case conflict-$conflict_index
    # The matrix contains no whitespace-bearing operand, so field splitting is intentional here.
    # shellcheck disable=SC2086
    run_fail $conflict_order
    assert_contains "Cannot combine --aur and --repo" "$output_file"
    assert_command_log_empty
    assert_request_log_empty
    assert_cache_root_absent
done

# Matrix F: 初期scope外のoperationはselectorを黙って無視しない。
assert_unsupported_operation() {
    selector=$1
    operation=$2
    shift 2
    unsupported_index=$((unsupported_index + 1))
    setup_case unsupported-$unsupported_index
    run_fail "$operation" "$selector" "$@"
    assert_contains "$selector is not supported for operation" "$output_file"
    assert_contains "$operation" "$output_file"
    assert_command_log_empty
    assert_request_log_empty
    assert_cache_root_absent
}

unsupported_index=0
for selector in --aur --repo; do
    assert_unsupported_operation "$selector" upgrade
    assert_unsupported_operation "$selector" upgrade-aur
    assert_unsupported_operation "$selector" -Su
    assert_unsupported_operation "$selector" -S -u
    if [ "$selector" = "--aur" ]; then
        unsupported_index=$((unsupported_index + 1))
        setup_case unsupported-$unsupported_index
        run_fail -Syu --aur
        assert_contains "Cannot combine --aur with pacman refresh" "$output_file"
        assert_contains "-Syu" "$output_file"
        assert_command_log_empty
        assert_request_log_empty

        unsupported_index=$((unsupported_index + 1))
        setup_case unsupported-$unsupported_index
        run_fail -Sy --aur sync-target
        assert_contains "Cannot combine --aur with pacman refresh" "$output_file"
        assert_contains "-Sy" "$output_file"
        assert_command_log_empty
        assert_request_log_empty
    else
        unsupported_index=$((unsupported_index + 1))
        setup_case repository-system-update-$unsupported_index
        prepare_preference_store
        printf '%s\n' 'INVALID PREFERENCE' > \
            "$preference_dir/clean-root"
        chmod 600 "$preference_dir/clean-root"
        run_ok --noconfirm -Syu --repo --config custom.conf
        assert_command "sudo pacman -Syu --noconfirm --config custom.conf"
        assert_command_count 1
        assert_request_log_empty
        assert_cache_root_absent
        assert_not_contains "Loading custom build flags" "$output_file"
        assert_unsupported_operation "$selector" -Sy sync-target
    fi
    assert_unsupported_operation "$selector" -Sc
    assert_unsupported_operation "$selector" -S -c
    assert_unsupported_operation "$selector" -S --clean
    assert_unsupported_operation "$selector" -S --sysupgrade
    assert_unsupported_operation "$selector" -Qua
    assert_unsupported_operation "$selector" -Q filesystem
    assert_unsupported_operation "$selector" -F usr/bin/foo
    assert_unsupported_operation "$selector" -U package.pkg.tar.zst
    assert_unsupported_operation "$selector" build clean-root
    assert_unsupported_operation "$selector" fetch clean-root
    assert_unsupported_operation "$selector" deps clean-root
    assert_unsupported_operation "$selector" plan clean-root
done

# Matrix B: AurOnly install。root targetはrepo probe/preferenceを見ず、全件preflight後だけmutationする。
setup_case aur-install-success
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok --noedit --nodiff --noconfirm -S --aur clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "makepkg --packagelist"
assert_command "makepkg -sc --noconfirm"
assert_command_pattern '^pacman -Qp --color never -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_command_pattern '^sudo pacman -U --noconfirm -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_command_pattern_count 1 '^pacman-conf --verbose RootDir DBPath$'
assert_command_pattern_count 0 '^pacman-conf --repo-list$'
assert_command_pattern_count 1 '^makepkg --packagelist$'
assert_command_pattern_count 1 '^makepkg -sc --noconfirm$'
assert_command_pattern_count 1 '^pacman -Qp --color never '
assert_command_pattern_count 1 '^sudo pacman -U --noconfirm -- '
assert_command_absent "pacman -Si clean-root"
assert_request_log_nonempty

setup_case aur-install-official-same-name
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok --noedit --nodiff --noconfirm -S --aur clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_absent "pacman -Si clean-root"
assert_request_log_nonempty

setup_case aur-install-ignores-preference
prepare_source_preference clean-root
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok --noedit --nodiff --noconfirm -S --aur clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_absent "git clone https://gitlab.archlinux.org/archlinux/packaging/packages/clean-root.git clean-root"
assert_command_absent "pacman -Si clean-root"
assert_request_log_nonempty
if [ ! -f "$preference_dir/clean-root" ]; then
    echo "--aur changed the source-build preference file" >&2
    exit 1
fi

setup_case aur-install-missing
run_fail -S --aur missing-aur-package
assert_contains "AUR package not found" "$output_file"
assert_no_mutation_commands
assert_request_log_nonempty
assert_cache_entry_absent missing-aur-package

setup_case aur-install-qualified-target
run_fail -S --aur core/filesystem
assert_contains "Invalid AUR package target" "$output_file"
assert_command_log_empty
assert_request_log_empty

setup_case aur-install-multiple-preflight
run_fail --noedit --noconfirm -S --aur clean-root missing-aur-package
assert_contains "AUR package not found" "$output_file"
assert_no_mutation_commands
assert_request_log_nonempty
assert_cache_entry_absent clean-root
assert_cache_entry_absent missing-aur-package

while IFS='|' read -r case_name target expected; do
    setup_case "aur-guard-$case_name"
    run_fail --noedit --noconfirm -S --aur "$target"
    assert_contains "$expected" "$output_file"
    assert_no_mutation_commands
    assert_request_log_nonempty
    assert_cache_entry_absent "$target"
done <<'AUR_GUARDS'
relation-assessment|planned-conflict-root|Planned-target conflict confirmed
ambiguous-provider|ambiguous-root|ambiguous providers
unresolved-dependency|unresolved-root|unresolved dependencies
cyclic-plan|cycle-root|cyclic dependencies
AUR_GUARDS

# Requested split childはPackageBase buildからmetadata identityで一件だけを
# selectし、sibling/debug outputをtransactionへ渡さない。
setup_case aur-install-split-child
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='split-base|split-sibling|2.4-1
split-base|split-child|2.4-1
split-base|split-child-debug|2.4-1'
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok --noedit --nodiff --noconfirm -S --aur split-child
assert_command "git clone https://aur.archlinux.org/split-base.git split-base"
assert_command_pattern_count 1 '^sudo pacman -U --noconfirm -- .*/split-child-2\.4-1-x86_64\.pkg\.tar\.zst$'
assert_command_pattern_absent '^sudo pacman -U .*split-sibling'
assert_command_pattern_absent '^sudo pacman -U .*split-child-debug'
assert_contains "PackageBase result: split-base" "$output_file"
assert_contains "  required child: split-child -> split-child 2.4-1 (explicit): installed" "$output_file"
assert_output_before \
    "  produced artifact: split-sibling 2.4-1 (not selected; not installed)" \
    "  produced artifact: split-child-debug 2.4-1 (not selected; not installed)" \
    "$output_file"

# Auto routeはrequested child preferenceを先に読み、空ならPackageBaseへ
# fallbackしたうえで同じselected-only lifecycleを使う。
setup_case auto-install-split-child-package-base-preference
prepare_source_preference split-base
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='split-base|split-child|2.5-2
split-base|split-sibling|2.5-2'
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok --noedit --nodiff --noconfirm -S split-child
assert_command_absent "pacman -Si split-child"
assert_command "pacman-conf --repo-list"
assert_repository_read_counts 1 1
assert_command "git clone https://aur.archlinux.org/split-base.git split-base"
assert_contains "Loading custom build flags from $preference_dir/split-base." "$output_file"
assert_command_pattern_count 1 '^sudo pacman -U --noconfirm -- .*/split-child-2\.5-2-x86_64\.pkg\.tar\.zst$'
assert_command_pattern_absent '^sudo pacman -U .*split-sibling'

# 同じPackageBaseのdependency childrenはBuildPlan順で一つのtransactionへ
# 入り、root packageとは別work itemになる。
setup_case aur-install-same-package-base-dependency-children
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='split-suite|split-runtime|2.0-3
split-suite|split-tools|2.0-3
split-suite|split-suite-debug|2.0-3
split-suite-root|split-suite-root|2.0-3'
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok --noedit --nodiff --noconfirm -S --aur split-suite-root
assert_command_pattern_count 1 '^sudo pacman -U --noconfirm --asdeps -- .*/split-runtime-2\.0-3-x86_64\.pkg\.tar\.zst .*/split-tools-2\.0-3-x86_64\.pkg\.tar\.zst$'
assert_command_pattern_absent '^sudo pacman -U .*split-suite-debug'
assert_output_before \
    "  required child: split-runtime -> split-runtime 2.0-3 (dependency): installed" \
    "  required child: split-tools -> split-tools 2.0-3 (dependency): installed" \
    "$output_file"
assert_contains "  produced artifact: split-suite-debug 2.0-3 (not selected; not installed)" "$output_file"

setup_case aur-install-unsupported-option
run_fail -S --aur clean-root --config custom.conf
assert_contains "Unsupported pacman option for AUR/source-build target: --config" "$output_file"
assert_no_mutation_commands
assert_cache_entry_absent clean-root

setup_case aur-install-rejects-rmdeps-before-resolution
run_fail --noedit --nodiff --noconfirm --rmdeps -S --aur clean-root
assert_contains "Separated build/install does not support --rmdeps." "$output_file"
assert_command_log_empty
assert_request_log_empty
assert_cache_entry_absent clean-root

# Matrix C: RepoOnly install。ordered argvを一度のbinary repository transactionへ渡す。
setup_case repo-install-success
run_ok -S --repo official-a
assert_only_command "sudo pacman -S official-a"
assert_no_source_build_commands
assert_request_log_empty

setup_case repo-install-overrides-preference
prepare_source_preference official-a
preference_checksum=$(cksum "$preference_dir/official-a")
export MOGUET_TEST_PACMAN_REPO_PACKAGES=official-a
run_ok -S --repo official-a
assert_only_command "sudo pacman -S official-a"
assert_no_source_build_commands
assert_request_log_empty
if [ ! -f "$preference_dir/official-a" ]; then
    echo "--repo removed or changed the source-build preference" >&2
    exit 1
fi
if [ "$(cksum "$preference_dir/official-a")" != "$preference_checksum" ]; then
    echo "--repo changed the source-build preference content" >&2
    exit 1
fi

setup_case repo-install-missing
export MOGUET_TEST_SUDO_EXIT_CODE=42
run_status 42 -S --repo clean-root
assert_only_command "sudo pacman -S clean-root"
assert_no_source_build_commands
assert_request_log_empty

setup_case repo-install-qualified
run_ok -S --repo core/filesystem
assert_only_command "sudo pacman -S core/filesystem"
assert_request_log_empty

setup_case repo-install-ordered-argv
run_ok -S --repo target-a --config custom.conf target-b
assert_only_command "sudo pacman -S target-a --config custom.conf target-b"
assert_request_log_empty

setup_case repo-install-noconfirm
run_ok --noconfirm -S --repo target-a
assert_only_command "sudo pacman -S --noconfirm target-a"
assert_request_log_empty

setup_case repo-install-rmdeps-does-not-leak
run_ok -S --repo target-a --rmdeps
assert_only_command "sudo pacman -S target-a"
assert_request_log_empty

# Auto install regression: selector未指定時のrepo/source-build/AUR分類と横断preflightを維持する。
setup_case auto-install-official
write_repository_package official-a
run_ok -S official-a
assert_command_absent "pacman -Si official-a"
assert_command "pacman-conf --repo-list"
assert_command "sudo pacman -S official-a"
assert_repository_read_counts 1 0
assert_command_count 3
assert_no_source_build_commands
assert_request_log_empty

setup_case auto-install-preferred-official
prepare_preference_store
cat > "$preference_dir/clean-root" <<'SOURCE_PREFERENCE'
  # whole-line comment
  FIRST = "alpha value" # stripped comment
  QUOTED = 'quoted # value' # stripped after the quoted hash
  DUP=first
  DUP = second
  BRACED = ${FIRST}/brace
  SIMPLE = $DUP/simple
  UNDEFINED = $MOGUET_TEST_SOURCE_PREFERENCE_EXTERNAL
  EMPTY = ''
  9INVALID=value
  ignored without equals
SOURCE_PREFERENCE
chmod 600 "$preference_dir/clean-root"
export MOGUET_TEST_SOURCE_PREFERENCE_EXTERNAL=from-process-environment
write_repository_package clean-root
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
makepkg_env_log=$case_dir/makepkg-env.log
: > "$makepkg_env_log"
export MOGUET_TEST_MAKEPKG_ENV_LOG=$makepkg_env_log
export MOGUET_TEST_MAKEPKG_ENV_KEYS='DUP EMPTY UNDEFINED'
run_ok --noedit --nodiff --noconfirm -S clean-root
assert_command "git clone https://gitlab.archlinux.org/archlinux/packaging/packages/clean-root.git clean-root"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "makepkg --packagelist"
assert_command "makepkg -sc --noconfirm"
assert_command_pattern '^pacman -Qp --color never -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_command_pattern '^sudo pacman -U --noconfirm -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_repository_read_counts 1 1
assert_contains "Loading custom build flags from $preference_dir/clean-root." "$output_file"
assert_contains "Applying custom build flags: FIRST='alpha value' QUOTED='quoted # value' DUP='first' DUP='second' BRACED='alpha value/brace' SIMPLE='second/simple'" "$output_file"
assert_contains "Ignoring invalid environment assignment: 9INVALID=value" "$output_file"
assert_not_contains "UNDEFINED=" "$output_file"
assert_not_contains "EMPTY=" "$output_file"
assert_not_contains "from-process-environment" "$output_file"
assert_not_contains "ignored without equals" "$output_file"
assert_file_content "$makepkg_env_log" 'env-begin
env[DUP]=<second>
env[EMPTY]=<unset>
env[UNDEFINED]=<unset>
env-end
env-begin
env[DUP]=<second>
env[EMPTY]=<unset>
env[UNDEFINED]=<unset>
env-end'
assert_request_log_empty

setup_case auto-install-aur
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok --noedit --nodiff --noconfirm -S clean-root
assert_command_absent "pacman -Si clean-root"
assert_command "pacman-conf --repo-list"
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "makepkg --packagelist"
assert_command "makepkg -sc --noconfirm"
assert_command_pattern '^pacman -Qp --color never -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_command_pattern '^sudo pacman -U --noconfirm -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_repository_read_counts 1 1
assert_request_log_nonempty

setup_case auto-install-mixed-preflight
write_repository_package official-a
run_fail --noedit --noconfirm -S official-a missing-aur-package
assert_contains "not found" "$output_file"
assert_no_mutation_commands
assert_request_log_nonempty
assert_cache_entry_absent missing-aur-package
assert_repository_read_counts 2 0

setup_case auto-install-mixed-success
write_repository_package official-a
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok --noedit --nodiff --noconfirm -S official-a clean-root
assert_command "sudo pacman -S --noconfirm official-a"
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "makepkg --packagelist"
assert_command "makepkg -sc --noconfirm"
assert_command_pattern '^pacman -Qp --color never -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_command_pattern '^sudo pacman -U --noconfirm -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_repository_read_counts 2 1
assert_request_log_nonempty

setup_case auto-install-unsupported-option
write_repository_package official-a
run_fail -S official-a --config custom.conf clean-root
assert_contains "Unsupported pacman option for AUR/source-build target: --config" "$output_file"
assert_no_mutation_commands
assert_request_log_empty
assert_repository_read_counts 2 0

# Matrix D: search。AurOnlyはAURのみ、RepoOnlyはpacmanのみ、Autoは従来の統合表示。
setup_case aur-search
run_ok -Ss --aur virtual-dep-150
assert_contains "provider-a" "$output_file"
assert_no_repo_search_command
assert_command_log_empty
assert_request_log_nonempty

setup_case aur-search-missing
run_fail -Ss --aur missing-search-query
assert_no_repo_search_command
assert_command_log_empty
assert_request_log_nonempty

refresh_index=0
for refresh_command in \
    "-Ssy --aur virtual-dep-150" \
    "-Ss --refresh --aur virtual-dep-150" \
    "-Ss -y --aur virtual-dep-150"
do
    refresh_index=$((refresh_index + 1))
    setup_case aur-search-refresh-$refresh_index
    # shellcheck disable=SC2086
    run_fail $refresh_command
    assert_contains "Cannot combine --aur with pacman refresh" "$output_file"
    assert_command_log_empty
    assert_request_log_empty
done

setup_case repo-search
export MOGUET_TEST_PACMAN_EXIT_CODE=0
run_ok -Ss --repo keyword
assert_only_command "pacman -Ss keyword"
assert_request_log_empty

setup_case repo-search-missing
export MOGUET_TEST_PACMAN_EXIT_CODE=7
run_status 7 -Ss --repo keyword
assert_only_command "pacman -Ss keyword"
assert_request_log_empty

setup_case repo-search-refresh
run_ok -Ss --repo --refresh keyword
assert_only_command "sudo pacman -Ss --refresh keyword"
assert_request_log_empty

setup_case auto-search
export MOGUET_TEST_PACMAN_EXIT_CODE=0
run_ok -Ss virtual-dep-150
assert_command "pacman -Ss virtual-dep-150"
assert_contains "provider-a" "$output_file"
assert_request_log_nonempty

setup_case auto-search-refresh
run_ok -Ss --refresh virtual-dep-150
assert_command "sudo pacman -Ss --refresh virtual-dep-150"
assert_request_log_nonempty

# Matrix E: info。source限定中はfallbackせず、RepoOnly refreshはunqualified targetも許可する。
setup_case aur-info
run_ok -Si --aur clean-root
assert_contains "Name            : clean-root" "$output_file"
assert_no_repo_info_command
assert_request_log_nonempty

setup_case aur-info-official-same-name
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
run_ok -Si --aur clean-root
assert_contains "Name            : clean-root" "$output_file"
assert_no_repo_info_command
assert_request_log_nonempty

setup_case aur-info-missing
run_fail -Si --aur missing-aur-package
assert_contains "AUR package not found" "$output_file"
assert_no_repo_info_command
assert_request_log_nonempty

setup_case aur-info-qualified
run_fail -Si --aur core/filesystem
assert_contains "Invalid AUR package target" "$output_file"
assert_command_log_empty
assert_request_log_empty

info_refresh_index=0
for refresh_command in \
    "-Siy --aur clean-root" \
    "-Si --aur --refresh clean-root"
do
    info_refresh_index=$((info_refresh_index + 1))
    setup_case aur-info-refresh-$info_refresh_index
    # shellcheck disable=SC2086
    run_fail $refresh_command
    assert_contains "Cannot combine --aur with pacman refresh" "$output_file"
    assert_command_log_empty
    assert_request_log_empty
done

setup_case aur-info-multiple
run_ok -Si --aur clean-root risk-root
assert_contains "Name            : clean-root" "$output_file"
assert_contains "Name            : risk-root" "$output_file"
assert_no_repo_info_command
assert_request_log_nonempty

# partial output契約: valid targetは表示するが、1件でもmissingならinvocationはnon-zero。
setup_case aur-info-multiple-partial
run_fail -Si --aur clean-root missing-aur-package
assert_contains "Name            : clean-root" "$output_file"
assert_contains "AUR package not found" "$output_file"
assert_no_repo_info_command
assert_request_log_nonempty

setup_case repo-info
export MOGUET_TEST_PACMAN_EXIT_CODE=0
run_ok -Si --repo filesystem
assert_only_command "pacman -Si filesystem"
assert_request_log_empty

setup_case repo-info-missing
run_fail -Si --repo clean-root
assert_only_command "pacman -Si clean-root"
assert_request_log_empty

setup_case repo-info-qualified
export MOGUET_TEST_PACMAN_EXIT_CODE=0
run_ok -Si --repo core/filesystem
assert_only_command "pacman -Si core/filesystem"
assert_request_log_empty

setup_case repo-info-refresh-unqualified
run_ok -Si --repo --refresh filesystem
assert_only_command "sudo pacman -Si --refresh filesystem"
assert_request_log_empty

setup_case repo-info-multiple
export MOGUET_TEST_PACMAN_EXIT_CODE=0
run_ok -Si --repo target-a target-b
assert_only_command "pacman -Si target-a target-b"
assert_request_log_empty

setup_case auto-info-official
export MOGUET_TEST_PACMAN_REPO_PACKAGES=filesystem
write_repository_package filesystem
run_ok -Si filesystem
assert_command "pacman -Si filesystem"
assert_request_log_empty

setup_case auto-info-qualified
export MOGUET_TEST_PACMAN_EXIT_CODE=0
run_ok -Si core/filesystem
assert_only_command "pacman -Si core/filesystem"
assert_request_log_empty

setup_case auto-info-aur-fallback
run_ok -Si clean-root
assert_command "pacman-conf --repo-list"
assert_command_absent "pacman -Si clean-root"
assert_contains "Name            : clean-root" "$output_file"
assert_request_log_nonempty

setup_case auto-info-refresh-guard
run_fail -Si --refresh filesystem
assert_contains "Cannot combine pacman refresh with AUR info fallback" "$output_file"
assert_command_log_empty
assert_request_log_empty

echo "source selection integration tests: all checks passed"
