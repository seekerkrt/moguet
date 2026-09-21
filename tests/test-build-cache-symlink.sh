#!/bin/sh
set -eu

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

snapshot_directory_raw() {
    snapshot_dir=$1
    (
        CDPATH='' cd "$snapshot_dir" || exit $?
        find . -exec "${MOGUET_TEST_SNAPSHOT_STAT_COMMAND:-stat}" --printf \
            'entry type=%F mode=%f uid=%u gid=%g dev=%d ino=%i size=%s mtime=%y ctime=%z path=%n target=%N\n' -- {} + || exit $?
        find . -type f \
            -exec "${MOGUET_TEST_SNAPSHOT_CKSUM_COMMAND:-cksum}" {} + || exit $?
    )
}

if [ -n "${MOGUET_TEST_SNAPSHOT_FAULT_ROOT:-}" ]; then
    if [ -z "${MOGUET_TEST_SNAPSHOT_FAULT_OUTPUT:-}" ]; then
        printf '%s\n' \
            'snapshot fault injection requires an output path' >&2
        exit 1
    fi
    if validation_capture_sorted_output \
        "$MOGUET_TEST_SNAPSHOT_FAULT_OUTPUT.raw" \
        "$MOGUET_TEST_SNAPSHOT_FAULT_OUTPUT" \
        snapshot_directory_raw "$MOGUET_TEST_SNAPSHOT_FAULT_ROOT"; then
        exit 0
    else
        snapshot_status=$?
    fi
    printf 'directory snapshot failed with status %s; raw=%s\n' \
        "$snapshot_status" "$MOGUET_TEST_SNAPSHOT_FAULT_OUTPUT.raw" >&2
    exit 1
fi

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

# script(1) gives cmd_clean() a TTY so the destructive yes branch is exercised
# without adding a test-only prompt override to the production binary.
if ! command -v script >/dev/null 2>&1; then
    echo "script(1) is required for build cache cleanup tests" >&2
    exit 1
fi
ln -s "$test_binary" "$tmp_dir/moguet-test"
test_runner=$tmp_dir/moguet-test

port_file=$tmp_dir/port
python3 "$repo_root/tests/aur_rpc_fixture_server.py" \
    "$repo_root/tests/fixtures/aur-rpc-conflicts-replaces.json" "$port_file" &
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
export MOGUET_TEST_PACMAN_EXIT_CODE=1
export MOGUET_TEST_SUDO_EXIT_CODE=0

fail() {
    echo "$*" >&2
    exit 1
}

setup_case() {
    case_name=$1
    case_dir=$tmp_dir/cases/$case_name
    home_dir=$case_dir/home
    xdg_config_dir=$case_dir/xdg-config
    xdg_state_dir=$case_dir/xdg-state
    xdg_cache_dir=$case_dir/xdg-cache
    outside_dir=$case_dir/outside
    command_log=$case_dir/commands.log
    editor_argv_log=$case_dir/editor-argv.log
    package_metadata_state=$case_dir/package-metadata.state
    repository_metadata_state=$case_dir/repository-metadata.state

    mkdir -p \
        "$home_dir" "$xdg_config_dir" "$xdg_state_dir" \
        "$xdg_cache_dir" "$outside_dir"
    chmod 0700 "$xdg_config_dir"
    : > "$command_log"
    : > "$editor_argv_log"
    : > "$package_metadata_state"
    : > "$repository_metadata_state"
    export HOME=$home_dir
    export XDG_CONFIG_HOME=$xdg_config_dir
    export XDG_STATE_HOME=$xdg_state_dir
    export XDG_CACHE_HOME=$xdg_cache_dir
    export MOGUET_TEST_COMMAND_LOG=$command_log
    export MOGUET_TEST_EDITOR_ARGV_LOG=$editor_argv_log
    export MOGUET_TEST_PACKAGE_METADATA_STATE_FILE=$package_metadata_state
    export MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE=$repository_metadata_state
    export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
    unset MOGUET_TEST_GIT_REMOTE_URL
    unset MOGUET_TEST_GIT_CLONE_EXIT_CODE
    unset MOGUET_TEST_GIT_CLONE_SYMLINK_TARGET
    unset MOGUET_TEST_GIT_CONFIG_REPLACE_PKGBUILD_SYMLINK_TARGET
    unset MOGUET_TEST_GIT_FETCH_REPLACE_PKGBUILD_SYMLINK_TARGET
    unset MOGUET_TEST_GIT_CLONE_FIXTURE_DIR
    unset MOGUET_TEST_GIT_CONFIG_RAW_OUTPUT_FILE
    unset MOGUET_TEST_GIT_CONFIG_RAW_OUTPUT_EXIT_CODE
    unset MOGUET_TEST_GIT_FETCH_ARGV_LOG
    unset MOGUET_TEST_GIT_FETCH_AUTO_MAINTENANCE_DIR
    unset MOGUET_TEST_TRUSTED_GIT_UNSAFE_MARKER
    unset MOGUET_TEST_TRUSTED_GIT_FETCH_MARKER
    unset MOGUET_TEST_TRUSTED_GIT_ENVIRONMENT_LOG
    unset MOGUET_TEST_EDITOR_REPLACE_TARGET
    unset MOGUET_TEST_EDITOR_REMOVE_TARGET
    unset MOGUET_TEST_EDITOR_SYMLINK_TARGET
    unset MOGUET_TEST_EDITOR_EXIT_CODE
    unset MOGUET_TEST_PACMAN_QM_OUTPUT
    unset MOGUET_TEST_PACMAN_REPO_PACKAGES
    unset MOGUET_TEST_MAKEPKG_EXIT_CODE
    unset EDITOR
    unset VISUAL
    unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_OBJECT_DIRECTORY
    unset GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_INDEX_FILE GIT_NAMESPACE
    unset GIT_CONFIG_PARAMETERS GIT_CONFIG_COUNT
    unset GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0
    unset GIT_CONFIG_SYSTEM GIT_CONFIG_GLOBAL GIT_CONFIG_NOSYSTEM
    unset GIT_CONFIG GIT_EXEC_PATH GIT_TEMPLATE_DIR GIT_SSH GIT_SSH_COMMAND
    unset GIT_PROXY_COMMAND GIT_SSL_NO_VERIFY CURL_CA_BUNDLE
    unset LD_PRELOAD LD_LIBRARY_PATH SHELL BASH_ENV ENV CDPATH
    unset http_proxy https_proxy all_proxy no_proxy
    unset HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY
    unset SSL_CERT_FILE SSL_CERT_DIR GIT_SSL_CAINFO GIT_SSL_CAPATH
    unset MOGUET_UNRECOGNIZED_ENVIRONMENT_SENTINEL

    cache_root=$XDG_CACHE_HOME/moguet
    entry_path=$cache_root/clean-root
}

run_ok() {
    output_file=$1
    shift
    : > "$command_log"
    if ! "$test_runner" "$@" </dev/null > "$output_file" 2>&1; then
        echo "expected command to succeed: $*" >&2
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_fail() {
    output_file=$1
    shift
    : > "$command_log"
    if ! validation_expect_status build-cache-business-failure 1 \
        "$output_file" "$output_file" "$test_runner" "$@" </dev/null; then
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_clean_tty_ok() {
    output_file=$1
    : > "$command_log"
    if ! printf 'y\n' | script -qec "$test_runner clean" /dev/null > "$output_file" 2>&1; then
        echo "expected interactive clean to succeed" >&2
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_clean_tty_fail() {
    output_file=$1
    : > "$command_log"
    tty_input=$case_dir/clean-tty.input
    printf 'y\n' >"$tty_input"
    if script -qec "$test_runner clean" /dev/null \
        <"$tty_input" >"$output_file" 2>&1; then
        tty_status=0
    else
        tty_status=$?
    fi
    if ! validation_assert_status build-cache-clean-tty-failure 1 \
        "$tty_status" "$output_file" "$output_file" \
        script -qec "$test_runner clean" /dev/null; then
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_build_tty_ok() {
    output_file=$1
    answers=$2
    : > "$command_log"
    if ! printf '%b' "$answers" |
        script -qec "$test_runner --nodiff build clean-root" /dev/null > "$output_file" 2>&1; then
        echo "expected interactive build to succeed" >&2
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_build_tty_fail() {
    output_file=$1
    answers=$2
    : > "$command_log"
    tty_input=$case_dir/build-tty.input
    printf '%b' "$answers" >"$tty_input"
    if script -qec "$test_runner --nodiff build clean-root" /dev/null \
        <"$tty_input" >"$output_file" 2>&1; then
        tty_status=0
    else
        tty_status=$?
    fi
    if ! validation_assert_status build-cache-build-tty-failure 1 \
        "$tty_status" "$output_file" "$output_file" \
        script -qec "$test_runner --nodiff build clean-root" /dev/null; then
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
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

assert_environment_assignment() {
    variable_name=$1
    expected_value=$2
    environment_log=$3
    if ! grep -Fx -- "$variable_name=$expected_value" \
        "$environment_log" >/dev/null; then
        echo "missing trusted Git environment assignment: $variable_name" >&2
        sed -n '1,240p' "$environment_log" >&2
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

assert_command_before() {
    first=$1
    second=$2
    first_line=$(grep -nFx -- "$first" "$command_log" | sed -n '1s/:.*//p')
    second_line=$(grep -nFx -- "$second" "$command_log" | sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] || [ "$first_line" -ge "$second_line" ]; then
        echo "unexpected command order: $first -> $second" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_editor_argv_log() {
    expected=$1
    actual=$(cat "$editor_argv_log")
    if [ "$actual" != "$expected" ]; then
        echo "unexpected editor argv" >&2
        printf 'expected:\n%s\nactual:\n%s\n' "$expected" "$actual" >&2
        exit 1
    fi
}

assert_editor_targets_not_option_like() {
    if grep -E '^target=<-' "$editor_argv_log" >/dev/null; then
        echo "editor review target remained option-like" >&2
        cat "$editor_argv_log" >&2
        exit 1
    fi
}

assert_log_empty() {
    if [ -s "$command_log" ]; then
        echo "external command ran before unsafe cache path was rejected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_no_cache_mutation_commands() {
    if grep -E '^(git|moguet-test-editor|makepkg|sudo) ' "$command_log" >/dev/null; then
        echo "unsafe cache path reached a git/build/install command" >&2
        cat "$command_log" >&2
        exit 1
    fi
    pacman_filter_raw=$case_dir/pacman-commands.raw
    if validation_capture_output "$pacman_filter_raw" \
        grep '^pacman ' "$command_log"; then
        :
    else
        grep_status=$?
        case "$grep_status" in
            1) : >"$pacman_filter_raw" ;;
            *) fail "pacman command filter failed with status $grep_status" ;;
        esac
    fi
    if grep -v '^pacman -Si ' "$pacman_filter_raw" >/dev/null; then
        echo "unsafe cache path reached a pacman mutation command" >&2
        cat "$command_log" >&2
        exit 1
    else
        grep_status=$?
        [ "$grep_status" -eq 1 ] ||
            fail "pacman mutation filter failed with status $grep_status"
    fi
}

assert_no_build_or_install_commands() {
    if grep -E '^(makepkg|sudo) ' "$command_log" >/dev/null; then
        echo "unsafe reviewed artifact reached a build/install command" >&2
        cat "$command_log" >&2
        exit 1
    fi
    pacman_filter_raw=$case_dir/pacman-commands.raw
    if validation_capture_output "$pacman_filter_raw" \
        grep '^pacman ' "$command_log"; then
        :
    else
        grep_status=$?
        case "$grep_status" in
            1) : >"$pacman_filter_raw" ;;
            *) fail "pacman command filter failed with status $grep_status" ;;
        esac
    fi
    if grep -v '^pacman -Si ' "$pacman_filter_raw" >/dev/null; then
        echo "unsafe reviewed artifact reached a pacman mutation command" >&2
        cat "$command_log" >&2
        exit 1
    else
        grep_status=$?
        [ "$grep_status" -eq 1 ] ||
            fail "pacman mutation filter failed with status $grep_status"
    fi
}

assert_only_clone_after_metadata() {
    expected_clone=$1
    assert_command "$expected_clone"
    while IFS= read -r command; do
        case $command in
            pacman-conf\ --verbose\ RootDir\ DBPath)
                ;;
            pacman-conf\ --repo-list)
                ;;
            "$expected_clone")
                ;;
            *)
                echo "unsafe cloned checkout reached an unexpected command" >&2
                cat "$command_log" >&2
                exit 1
                ;;
        esac
    done < "$command_log"
}

assert_symlink() {
    path=$1
    if [ ! -L "$path" ]; then
        fail "expected symlink to remain: $path"
    fi
}

assert_path_absent() {
    path=$1
    if [ -e "$path" ] || [ -L "$path" ]; then
        fail "expected path to be absent: $path"
    fi
}

assert_fetch_argv_log() {
    fetch_argv_log=$1
    expected_fetch_count=$2
    suppression_mode=$3
    expected_fetch_argv_log=$case_dir/expected-fetch-argv.log
    : > "$expected_fetch_argv_log"
    fetch_index=0
    while [ "$fetch_index" -lt "$expected_fetch_count" ]; do
        printf '%s\n' \
            'argv-begin' \
            'arg=<fetch>' >> "$expected_fetch_argv_log"
        if [ "$suppression_mode" = "suppressed" ]; then
            printf '%s\n' 'arg=<--no-auto-maintenance>' >> \
                "$expected_fetch_argv_log"
        fi
        printf '%s\n' \
            'arg=<--no-recurse-submodules>' \
            'arg=<origin>' \
            'argv-end' >> "$expected_fetch_argv_log"
        fetch_index=$((fetch_index + 1))
    done
    if ! cmp -s "$expected_fetch_argv_log" "$fetch_argv_log"; then
        echo "unexpected managed fetch argv" >&2
        diff -u "$expected_fetch_argv_log" "$fetch_argv_log" >&2 || true
        exit 1
    fi
}

wait_for_marker() {
    marker_path=$1
    marker_description=$2
    marker_attempt=0
    while [ ! -e "$marker_path" ]; do
        marker_attempt=$((marker_attempt + 1))
        if [ "$marker_attempt" -gt 500 ]; then
            fail "timed out waiting for $marker_description: $marker_path"
        fi
        sleep 0.01
    done
}

assert_symlink_rejection() {
    output_file=$1
    rejected_path=$2
    assert_contains "symlink" "$output_file"
    # Cache authority diagnostics must not echo the raw XDG cache value or a
    # derived absolute cache path. Filesystem assertions below identify the
    # rejected fixture without weakening that diagnostic boundary.
    assert_not_contains "$rejected_path" "$output_file"
}

assert_descendant_rejection() {
    output_file=$1
    rejected_path=$2
    reason=$3
    case $reason in
        "symlink.") reason="symlink refused" ;;
        "gitfile / redirect.") reason="directory required" ;;
        "non-regular file.") reason="unsupported cache entry type" ;;
    esac
    assert_contains "$reason" "$output_file"
    assert_not_contains "$rejected_path" "$output_file"
}

create_checkout() {
    checkout_dir=$1
    mkdir -p "$checkout_dir/.git"
    printf 'outside marker\n' > "$checkout_dir/marker"
    printf 'uncommitted content\n' > "$checkout_dir/uncommitted-content"
}

create_regular_repo() {
    repo_dir=$1
    mkdir -p "$repo_dir/.git"
    printf 'pkgname=moguet-test-fixture\npkgver=1\npkgrel=1\n' > "$repo_dir/PKGBUILD"
}

write_standard_git_config() {
    checkout_dir=$1
    remote_url=${2:-https://aur.archlinux.org/clean-root.git}
    mkdir -p "$checkout_dir/.git"
    printf '%s\n' \
        '[core]' \
        '    repositoryformatversion = 0' \
        '    filemode = true' \
        '    bare = false' \
        '    logallrefupdates = true' \
        '[remote "origin"]' \
        "    url = $remote_url" \
        '    fetch = +refs/heads/*:refs/remotes/origin/*' \
        '[branch "main"]' \
        '    remote = origin' \
        '    merge = refs/heads/main' > "$checkout_dir/.git/config"
}

append_padding_branch_git_config() {
    config_file=$1
    branch_length=$2
    {
        printf '[branch "'
        awk -v count="$branch_length" \
            'BEGIN { for(position = 0; position < count; ++position) printf "a" }'
        printf '"]\n    remote = origin\n    merge = refs/heads/'
        awk -v count="$branch_length" \
            'BEGIN { for(position = 0; position < count; ++position) printf "a" }'
        printf '\n'
    } >> "$config_file"
}

git_config_output_size() {
    config_file=$1
    config_output_raw=$case_dir/git-config-output.raw
    if validation_capture_output "$config_output_raw" \
        /usr/bin/git config --file "$config_file" \
        --no-includes --null --list; then
        wc -c <"$config_output_raw"
        return 0
    else
        config_status=$?
    fi
    fail "git config producer failed with status $config_status; raw=$config_output_raw"
}

create_clone_fixture() {
    clone_fixture=$case_dir/clone-fixture
    mkdir -p "$clone_fixture/.git"
    printf 'pkgname=moguet-test-fixture\npkgver=1\npkgrel=1\n' > "$clone_fixture/PKGBUILD"
    export MOGUET_TEST_GIT_CLONE_FIXTURE_DIR=$clone_fixture
}

snapshot_directory() {
    snapshot_dir=$1
    snapshot_file=$2
    if validation_capture_sorted_output "$snapshot_file.raw" "$snapshot_file" \
        snapshot_directory_raw "$snapshot_dir"; then
        return 0
    else
        snapshot_status=$?
    fi
    fail "directory snapshot failed with status $snapshot_status; raw=$snapshot_file.raw"
}

assert_directory_unchanged() {
    checked_dir=$1
    before_snapshot=$2
    after_snapshot=$case_dir/after.snapshot
    if [ ! -d "$checked_dir" ]; then
        fail "outside fixture directory was removed: $checked_dir"
    fi
    snapshot_directory "$checked_dir" "$after_snapshot"
    if ! cmp -s "$before_snapshot" "$after_snapshot"; then
        echo "outside fixture changed: $checked_dir" >&2
        diff -u "$before_snapshot" "$after_snapshot" >&2 || true
        exit 1
    fi
}

create_legacy_cache_fixture() {
    legacy_cache_root=$XDG_CACHE_HOME/jpacker
    mkdir -p \
        "$legacy_cache_root/existing-checkout/.git" \
        "$legacy_cache_root/.artifact-workspace~-legacy"
    legacy_log=$legacy_cache_root/jpacker.log
    printf 'legacy log sentinel\n' > "$legacy_log"
    printf 'legacy checkout sentinel\n' > \
        "$legacy_cache_root/existing-checkout/PKGBUILD"
    printf 'legacy workspace sentinel\n' > \
        "$legacy_cache_root/.artifact-workspace~-legacy/artifact"
    ln -s jpacker.log "$legacy_cache_root/log-link"
    chmod 0755 "$legacy_cache_root"
    chmod 0640 "$legacy_log"
    snapshot_directory \
        "$legacy_cache_root" "$case_dir/legacy-before.snapshot"
}

assert_output_before() {
    first=$1
    second=$2
    file=$3
    first_line=$(grep -nF -- "$first" "$file" | sed -n '1s/:.*//p')
    second_line=$(grep -nF -- "$second" "$file" | sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] || [ "$first_line" -ge "$second_line" ]; then
        echo "expected output ordering: $first before $second" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

# --- Existing package entry symlinks ---

setup_case fetch-directory-symlink
mkdir -p "$cache_root"
create_checkout "$outside_dir/checkout"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$outside_dir/checkout" "$entry_path"
run_fail "$case_dir/output" fetch clean-root
assert_symlink_rejection "$case_dir/output" "$entry_path"
assert_log_empty
assert_symlink "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-file-symlink
mkdir -p "$cache_root"
printf 'outside regular file\n' > "$outside_dir/package-entry"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$outside_dir/package-entry" "$entry_path"
run_fail "$case_dir/output" fetch clean-root
assert_symlink_rejection "$case_dir/output" "$entry_path"
assert_log_empty
assert_symlink "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-dangling-symlink
mkdir -p "$cache_root"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$outside_dir/missing-package-entry" "$entry_path"
run_fail "$case_dir/output" fetch clean-root
assert_symlink_rejection "$case_dir/output" "$entry_path"
assert_log_empty
assert_symlink "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

# This case is the direct canonical escape reproducer: the lexical package entry
# is below the active Moguet cache component, while its resolved checkout is
# outside the trusted root.
setup_case fetch-canonical-outside
mkdir -p "$cache_root"
create_checkout "$outside_dir/canonical-outside"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$outside_dir/canonical-outside" "$entry_path"
run_fail "$case_dir/output" fetch clean-root
assert_symlink_rejection "$case_dir/output" "$entry_path"
assert_log_empty
assert_symlink "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

# A string-prefix check would incorrectly accept moguet-escape as being below
# the active Moguet cache component. Component-based containment and the
# symlink policy must reject it.
setup_case fetch-prefix-lookalike
mkdir -p "$cache_root"
prefix_sibling=$XDG_CACHE_HOME/moguet-escape
create_checkout "$prefix_sibling/clean-root"
snapshot_directory "$prefix_sibling" "$case_dir/before.snapshot"
ln -s "$prefix_sibling/clean-root" "$entry_path"
run_fail "$case_dir/output" fetch clean-root
assert_symlink_rejection "$case_dir/output" "$entry_path"
assert_log_empty
assert_symlink "$entry_path"
assert_directory_unchanged "$prefix_sibling" "$case_dir/before.snapshot"

# --- Cache root and ancestor symlink boundaries ---

setup_case fetch-cache-root-symlink
root_target=$outside_dir/cache-root
create_checkout "$root_target/clean-root"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$root_target" "$cache_root"
run_fail "$case_dir/output" fetch clean-root
assert_symlink_rejection "$case_dir/output" "$cache_root"
assert_log_empty
assert_symlink "$cache_root"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-cache-ancestor-symlink
ancestor_target=$outside_dir/cache-parent
ancestor_link=$case_dir/cache-parent-link
create_checkout "$ancestor_target/moguet/clean-root"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$ancestor_target" "$ancestor_link"
export XDG_CACHE_HOME=$ancestor_link
cache_root=$XDG_CACHE_HOME/moguet
entry_path=$cache_root/clean-root
run_fail "$case_dir/output" fetch clean-root
assert_symlink_rejection "$case_dir/output" "$cache_root"
assert_log_empty
assert_symlink "$ancestor_link"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

# --- Persistent checkout descendant boundaries ---

setup_case fetch-git-directory-symlink
mkdir -p "$entry_path" "$outside_dir/external.git"
printf 'external git metadata\n' > "$outside_dir/external.git/marker"
printf 'pkgname=clean-root\n' > "$entry_path/PKGBUILD"
ln -s "$outside_dir/external.git" "$entry_path/.git"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" fetch clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/.git" "symlink."
assert_log_empty
assert_symlink "$entry_path/.git"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-git-object-store-symlink
create_regular_repo "$entry_path"
mkdir -p "$outside_dir/object-store"
printf 'external object marker\n' > "$outside_dir/object-store/marker"
ln -s "$outside_dir/object-store" "$entry_path/.git/objects"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" fetch clean-root
assert_descendant_rejection \
    "$case_dir/output" "$entry_path/.git/objects" "symlink."
assert_log_empty
assert_symlink "$entry_path/.git/objects"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-git-metadata-hardlink
create_regular_repo "$entry_path"
printf 'external reflog marker\n' > "$outside_dir/HEAD-log"
ln "$outside_dir/HEAD-log" "$entry_path/.git/FETCH_HEAD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection \
    "$case_dir/output" "$entry_path/.git/FETCH_HEAD" "child escape"
assert_no_cache_mutation_commands
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-git-alternates-redirect
create_regular_repo "$entry_path"
mkdir -p "$entry_path/.git/objects/info"
printf '%s\n' "$outside_dir/object-store" > \
    "$entry_path/.git/objects/info/alternates"
run_fail "$case_dir/output" fetch clean-root
assert_descendant_rejection \
    "$case_dir/output" "$entry_path/.git/objects/info/alternates" \
    "child escape"
assert_not_contains "$outside_dir/object-store" "$case_dir/output"
assert_log_empty

setup_case build-git-dangling-symlink
mkdir -p "$entry_path"
printf 'pkgname=clean-root\n' > "$entry_path/PKGBUILD"
ln -s "$outside_dir/missing.git" "$entry_path/.git"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/.git" "symlink."
assert_no_cache_mutation_commands
assert_symlink "$entry_path/.git"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-absolute-external-gitdir
mkdir -p "$entry_path" "$outside_dir/external.git"
printf 'external git metadata\n' > "$outside_dir/external.git/marker"
printf 'pkgname=clean-root\n' > "$entry_path/PKGBUILD"
printf 'gitdir: %s\n' "$outside_dir/external.git" > "$entry_path/.git"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" fetch clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/.git" "gitfile / redirect."
assert_not_contains "$outside_dir/external.git" "$case_dir/output"
assert_log_empty
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-relative-external-gitdir
mkdir -p "$entry_path" "$outside_dir/external.git"
printf 'external git metadata\n' > "$outside_dir/external.git/marker"
printf 'pkgname=clean-root\n' > "$entry_path/PKGBUILD"
printf 'gitdir: ../../../outside/external.git\n' > "$entry_path/.git"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/.git" "gitfile / redirect."
assert_not_contains "$outside_dir/external.git" "$case_dir/output"
assert_no_cache_mutation_commands
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-pkgbuild-symlink
create_regular_repo "$entry_path"
printf 'external PKGBUILD\n' > "$outside_dir/PKGBUILD"
rm "$entry_path/PKGBUILD"
ln -s "$outside_dir/PKGBUILD" "$entry_path/PKGBUILD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "symlink."
assert_no_cache_mutation_commands
assert_symlink "$entry_path/PKGBUILD"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-pkgbuild-dangling-symlink
create_regular_repo "$entry_path"
rm "$entry_path/PKGBUILD"
ln -s "$outside_dir/missing-PKGBUILD" "$entry_path/PKGBUILD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "symlink."
assert_no_cache_mutation_commands
assert_symlink "$entry_path/PKGBUILD"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-install-symlink
create_regular_repo "$entry_path"
printf 'external install script\n' > "$outside_dir/clean-root.install"
ln -s "$outside_dir/clean-root.install" "$entry_path/clean-root.install"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/clean-root.install" "symlink."
assert_no_cache_mutation_commands
assert_symlink "$entry_path/clean-root.install"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-pkgbuild-directory
create_regular_repo "$entry_path"
rm "$entry_path/PKGBUILD"
mkdir "$entry_path/PKGBUILD"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "non-regular file."
assert_no_cache_mutation_commands

setup_case build-install-fifo
create_regular_repo "$entry_path"
mkfifo "$entry_path/clean-root.install"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/clean-root.install" "non-regular file."
assert_no_cache_mutation_commands

# --- Trusted Git environment and repository binding ---

# The stub models Git's detached automatic-maintenance lifetime directly. The
# ready/release handshake proves that an unsuppressed fetch parent can return
# while its background child still has checkout mutation pending; it does not
# try to force a literal ENOENT into production descendant validation.
setup_case trusted-git-fetch-auto-maintenance-unsuppressed-fixture
create_regular_repo "$entry_path"
fetch_argv_log=$case_dir/fetch-argv.log
maintenance_dir=$case_dir/auto-maintenance
mkdir "$maintenance_dir"
: > "$fetch_argv_log"
if ! env -i \
    PATH=/usr/bin:/bin \
    LC_ALL=C \
    LANG=C \
    GIT_CONFIG_NOSYSTEM=1 \
    GIT_CONFIG_SYSTEM=/dev/null \
    GIT_CONFIG_GLOBAL=/dev/null \
    GIT_TERMINAL_PROMPT=0 \
    GIT_ASKPASS=/bin/false \
    SSH_ASKPASS=/bin/false \
    GIT_PAGER=cat \
    PAGER=cat \
    GIT_ATTR_NOSYSTEM=1 \
    MOGUET_TEST_TRUSTED_GIT_BOUNDARY=1 \
    MOGUET_TEST_TRUSTED_GIT_DISPLAY_COMMAND='git fetch origin' \
    MOGUET_TEST_COMMAND_LOG="$command_log" \
    MOGUET_TEST_TRUSTED_GIT_UNSAFE_MARKER="$outside_dir/unsafe-environment-marker" \
    MOGUET_TEST_GIT_FETCH_ARGV_LOG="$fetch_argv_log" \
    MOGUET_TEST_GIT_FETCH_AUTO_MAINTENANCE_DIR="$maintenance_dir" \
    "$repo_root/tests/stubs/git" \
    --no-pager \
    --git-dir="$entry_path/.git" \
    --work-tree="$entry_path" \
    fetch --no-recurse-submodules origin \
    > "$case_dir/output" 2>&1; then
    fail "unsuppressed managed fetch fixture failed"
fi
assert_fetch_argv_log "$fetch_argv_log" 1 unsuppressed
assert_command "git fetch origin"
assert_path_absent "$outside_dir/unsafe-environment-marker"
if [ ! -f "$maintenance_dir/ready" ] ||
   [ ! -f "$maintenance_dir/active" ]; then
    fail "unsuppressed fetch returned without pending background maintenance"
fi
maintenance_mutation_marker=$entry_path/.git/.moguet-test-auto-maintenance-mutation
assert_path_absent "$maintenance_mutation_marker"
: > "$maintenance_dir/release"
wait_for_marker "$maintenance_dir/done" "background maintenance completion"
assert_path_absent "$maintenance_dir/active"
assert_path_absent "$maintenance_dir/timeout"
if [ ! -f "$maintenance_mutation_marker" ]; then
    fail "background maintenance did not mutate after fetch return"
fi

# Production must pass the canonical suppression option in the operation argv,
# while preserving the concise user-facing display command.
setup_case trusted-git-fetch-auto-maintenance-suppressed
create_regular_repo "$entry_path"
fetch_argv_log=$case_dir/fetch-argv.log
maintenance_dir=$case_dir/auto-maintenance
mkdir "$maintenance_dir"
: > "$fetch_argv_log"
export MOGUET_TEST_GIT_FETCH_ARGV_LOG=$fetch_argv_log
export MOGUET_TEST_GIT_FETCH_AUTO_MAINTENANCE_DIR=$maintenance_dir
run_ok "$case_dir/output" fetch clean-root
assert_command "git fetch origin"
assert_fetch_argv_log "$fetch_argv_log" 1 suppressed
assert_path_absent "$maintenance_dir/ready"
assert_path_absent "$maintenance_dir/active"
assert_path_absent "$maintenance_dir/done"
assert_path_absent "$maintenance_dir/timeout"
assert_path_absent "$entry_path/.git/.moguet-test-auto-maintenance-mutation"

# Reusing the same validated checkout must keep every managed fetch synchronous
# with respect to Git-originated maintenance work.
setup_case trusted-git-repeated-fetch-auto-maintenance-suppressed
create_regular_repo "$entry_path"
fetch_argv_log=$case_dir/fetch-argv.log
maintenance_dir=$case_dir/auto-maintenance
mkdir "$maintenance_dir"
: > "$fetch_argv_log"
export MOGUET_TEST_GIT_FETCH_ARGV_LOG=$fetch_argv_log
export MOGUET_TEST_GIT_FETCH_AUTO_MAINTENANCE_DIR=$maintenance_dir
run_ok "$case_dir/first-output" fetch clean-root
assert_command "git fetch origin"
run_ok "$case_dir/second-output" fetch clean-root
assert_command "git fetch origin"
assert_fetch_argv_log "$fetch_argv_log" 2 suppressed
assert_path_absent "$maintenance_dir/ready"
assert_path_absent "$maintenance_dir/active"
assert_path_absent "$maintenance_dir/done"
assert_path_absent "$maintenance_dir/timeout"
assert_path_absent "$entry_path/.git/.moguet-test-auto-maintenance-mutation"

for environment_case in \
    git-dir git-work-tree git-common-dir git-object-directory \
    git-alternate-object-directories git-index-file git-namespace \
    git-config-parameters git-config-count system-global-config; do
    setup_case "trusted-git-environment-$environment_case"
    create_regular_repo "$entry_path"
    write_standard_git_config "$entry_path"
    mkdir -p "$outside_dir/external.git" "$outside_dir/objects"
    printf 'outside Git sentinel\n' > "$outside_dir/external.git/sentinel"
    printf 'outside object sentinel\n' > "$outside_dir/objects/sentinel"
    printf '%s\n' \
        '[core]' \
        "    worktree = $outside_dir" > "$outside_dir/injected.gitconfig"
    export MOGUET_TEST_TRUSTED_GIT_UNSAFE_MARKER=$outside_dir/unsafe-environment-marker
    export MOGUET_TEST_TRUSTED_GIT_FETCH_MARKER=.trusted-fetch-target
    case $environment_case in
        git-dir)
            export GIT_DIR=$outside_dir/external.git
            ;;
        git-work-tree)
            export GIT_WORK_TREE=$outside_dir
            ;;
        git-common-dir)
            export GIT_COMMON_DIR=$outside_dir/external.git
            ;;
        git-object-directory)
            export GIT_OBJECT_DIRECTORY=$outside_dir/objects
            ;;
        git-alternate-object-directories)
            export GIT_ALTERNATE_OBJECT_DIRECTORIES=$outside_dir/objects
            ;;
        git-index-file)
            export GIT_INDEX_FILE=$outside_dir/index
            ;;
        git-namespace)
            export GIT_NAMESPACE=outside
            ;;
        git-config-parameters)
            export GIT_CONFIG_PARAMETERS="'core.worktree'='$outside_dir'"
            ;;
        git-config-count)
            export GIT_CONFIG_COUNT=1
            export GIT_CONFIG_KEY_0=core.worktree
            export GIT_CONFIG_VALUE_0=$outside_dir
            ;;
        system-global-config)
            export GIT_CONFIG_NOSYSTEM=0
            export GIT_CONFIG_SYSTEM=$outside_dir/injected.gitconfig
            export GIT_CONFIG_GLOBAL=$outside_dir/injected.gitconfig
            ;;
    esac
    snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
    run_ok "$case_dir/output" fetch clean-root
    assert_command "git config --get remote.origin.url"
    assert_command "git fetch origin"
    assert_path_absent "$outside_dir/unsafe-environment-marker"
    if [ ! -f "$entry_path/.trusted-fetch-target" ]; then
        fail "trusted Git fetch did not act on the validated checkout: $environment_case"
    fi
    assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"
done

# Standard proxy and custom-CA variables are the only parent network settings
# admitted to the fresh trusted-Git environment. Values remain single opaque
# envp entries even when they contain shell metacharacters.
setup_case trusted-git-safe-network-environment
create_regular_repo "$entry_path"
write_standard_git_config "$entry_path"
trusted_git_environment_log=$case_dir/trusted-git-environment.log
: > "$trusted_git_environment_log"
export MOGUET_TEST_TRUSTED_GIT_ENVIRONMENT_LOG=$trusted_git_environment_log
export MOGUET_TEST_TRUSTED_GIT_UNSAFE_MARKER=$outside_dir/unsafe-environment-marker
export MOGUET_TEST_TRUSTED_GIT_FETCH_MARKER=.trusted-fetch-target

proxy_execution_marker=$outside_dir/proxy-execution-marker
http_proxy_value="http://lower-http.invalid/a path;touch $proxy_execution_marker;\"quoted\"'single'"
https_proxy_value=http://lower-https.invalid:8443
all_proxy_value=socks5://lower-all.invalid:1080
no_proxy_value=127.0.0.1,localhost
HTTP_PROXY_value=http://upper-http.invalid:8080
HTTPS_PROXY_value=http://upper-https.invalid:9443
ALL_PROXY_value=socks5://upper-all.invalid:1081
NO_PROXY_value=localhost,127.0.0.1
http_proxy=$http_proxy_value
https_proxy=$https_proxy_value
all_proxy=$all_proxy_value
no_proxy=$no_proxy_value
HTTP_PROXY=$HTTP_PROXY_value
HTTPS_PROXY=$HTTPS_PROXY_value
ALL_PROXY=$ALL_PROXY_value
NO_PROXY=$NO_PROXY_value
export http_proxy https_proxy all_proxy no_proxy
export HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY

mkdir -p "$case_dir/ca directory" "$case_dir/git ca directory"
SSL_CERT_FILE=$case_dir/ca\ directory/ssl-cert.pem
SSL_CERT_DIR=$case_dir/ca\ directory
GIT_SSL_CAINFO=$case_dir/git\ ca\ directory/git-ca.pem
GIT_SSL_CAPATH=$case_dir/git\ ca\ directory
: > "$SSL_CERT_FILE"
: > "$GIT_SSL_CAINFO"
export SSL_CERT_FILE SSL_CERT_DIR GIT_SSL_CAINFO GIT_SSL_CAPATH

printf '%s\n' '[core]' "    worktree = $outside_dir" > \
    "$outside_dir/injected.gitconfig"
GIT_CONFIG=$outside_dir/injected.gitconfig
GIT_EXEC_PATH=$outside_dir/git-exec
GIT_TEMPLATE_DIR=$outside_dir/git-template
GIT_SSH=$outside_dir/git-ssh
GIT_SSH_COMMAND="sh -c 'touch $outside_dir/git-ssh-command-marker'"
GIT_PROXY_COMMAND="sh -c 'touch $outside_dir/git-proxy-command-marker'"
XDG_CONFIG_HOME=$outside_dir/xdg-config
mkdir -p "$XDG_CONFIG_HOME"
chmod 0700 "$XDG_CONFIG_HOME"
LD_PRELOAD=
LD_LIBRARY_PATH=
SHELL="sh -c 'touch $outside_dir/shell-marker'"
BASH_ENV=$outside_dir/bash-env
ENV=$outside_dir/env
CDPATH=$outside_dir
GIT_SSL_NO_VERIFY=1
CURL_CA_BUNDLE=$outside_dir/forbidden-ca.pem
GIT_TERMINAL_PROMPT=1
GIT_ASKPASS=$outside_dir/git-askpass
SSH_ASKPASS=$outside_dir/ssh-askpass
MOGUET_UNRECOGNIZED_ENVIRONMENT_SENTINEL=$outside_dir/unknown-environment
export GIT_CONFIG GIT_EXEC_PATH GIT_TEMPLATE_DIR GIT_SSH GIT_SSH_COMMAND
export GIT_PROXY_COMMAND XDG_CONFIG_HOME LD_PRELOAD LD_LIBRARY_PATH
export SHELL BASH_ENV ENV CDPATH GIT_SSL_NO_VERIFY CURL_CA_BUNDLE
export GIT_TERMINAL_PROMPT GIT_ASKPASS SSH_ASKPASS
export MOGUET_UNRECOGNIZED_ENVIRONMENT_SENTINEL

snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_ok "$case_dir/output" fetch clean-root
assert_command "git config --get remote.origin.url"
assert_command "git fetch origin"
assert_path_absent "$outside_dir/unsafe-environment-marker"
assert_path_absent "$proxy_execution_marker"
if [ ! -f "$entry_path/.trusted-fetch-target" ]; then
    fail "trusted Git fetch did not act on the validated checkout"
fi
assert_environment_assignment \
    http_proxy "$http_proxy_value" "$trusted_git_environment_log"
assert_environment_assignment \
    https_proxy "$https_proxy_value" "$trusted_git_environment_log"
assert_environment_assignment \
    all_proxy "$all_proxy_value" "$trusted_git_environment_log"
assert_environment_assignment \
    no_proxy "$no_proxy_value" "$trusted_git_environment_log"
assert_environment_assignment \
    HTTP_PROXY "$HTTP_PROXY_value" "$trusted_git_environment_log"
assert_environment_assignment \
    HTTPS_PROXY "$HTTPS_PROXY_value" "$trusted_git_environment_log"
assert_environment_assignment \
    ALL_PROXY "$ALL_PROXY_value" "$trusted_git_environment_log"
assert_environment_assignment \
    NO_PROXY "$NO_PROXY_value" "$trusted_git_environment_log"
assert_environment_assignment \
    SSL_CERT_FILE "$SSL_CERT_FILE" "$trusted_git_environment_log"
assert_environment_assignment \
    SSL_CERT_DIR "$SSL_CERT_DIR" "$trusted_git_environment_log"
assert_environment_assignment \
    GIT_SSL_CAINFO "$GIT_SSL_CAINFO" "$trusted_git_environment_log"
assert_environment_assignment \
    GIT_SSL_CAPATH "$GIT_SSL_CAPATH" "$trusted_git_environment_log"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

# SSL_CERT_DIR accepts a POSIX ':'-separated list only when every component is
# nonempty and absolute. The original value remains one opaque env assignment.
ssl_cert_dir_case_index=0
for ssl_cert_dir_value in \
    /etc/ssl/certs \
    /etc/ssl/certs:/opt/company-ca \
    /a:/b:/c; do
    ssl_cert_dir_case_index=$((ssl_cert_dir_case_index + 1))
    setup_case "trusted-git-ssl-cert-dir-absolute-list-$ssl_cert_dir_case_index"
    create_regular_repo "$entry_path"
    write_standard_git_config "$entry_path"
    trusted_git_environment_log=$case_dir/trusted-git-environment.log
    : > "$trusted_git_environment_log"
    export MOGUET_TEST_TRUSTED_GIT_ENVIRONMENT_LOG=$trusted_git_environment_log
    export MOGUET_TEST_TRUSTED_GIT_UNSAFE_MARKER=$outside_dir/unsafe-environment-marker
    export MOGUET_TEST_TRUSTED_GIT_FETCH_MARKER=.trusted-fetch-target
    SSL_CERT_DIR=$ssl_cert_dir_value
    export SSL_CERT_DIR
    snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
    run_ok "$case_dir/output" fetch clean-root
    assert_command "git config --get remote.origin.url"
    assert_command "git fetch origin"
    assert_environment_assignment \
        SSL_CERT_DIR "$ssl_cert_dir_value" "$trusted_git_environment_log"
    exact_count=$(validation_grep_count -Fxc -- \
        "SSL_CERT_DIR=$ssl_cert_dir_value" \
        "$trusted_git_environment_log")
    variable_count=$(validation_grep_count -Ec '^SSL_CERT_DIR=' \
        "$trusted_git_environment_log")
    if [ "$exact_count" -ne 3 ] || [ "$variable_count" -ne 3 ]; then
        fail "SSL_CERT_DIR was not forwarded exactly once per trusted Git child"
    fi
    assert_path_absent "$outside_dir/unsafe-environment-marker"
    if [ ! -f "$entry_path/.trusted-fetch-target" ]; then
        fail "trusted Git fetch did not accept SSL_CERT_DIR absolute list"
    fi
    assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"
done

# Empty CA settings are intentionally omitted instead of overriding Git's
# default trust store with an empty value.
setup_case trusted-git-empty-ca-environment
create_regular_repo "$entry_path"
write_standard_git_config "$entry_path"
trusted_git_environment_log=$case_dir/trusted-git-environment.log
: > "$trusted_git_environment_log"
export MOGUET_TEST_TRUSTED_GIT_ENVIRONMENT_LOG=$trusted_git_environment_log
SSL_CERT_FILE=
SSL_CERT_DIR=
GIT_SSL_CAINFO=
GIT_SSL_CAPATH=
export SSL_CERT_FILE SSL_CERT_DIR GIT_SSL_CAINFO GIT_SSL_CAPATH
run_ok "$case_dir/output" fetch clean-root
if grep -E '^(SSL_CERT_FILE|SSL_CERT_DIR|GIT_SSL_CAINFO|GIT_SSL_CAPATH)=' \
    "$trusted_git_environment_log" >/dev/null; then
    fail "empty custom CA environment reached trusted Git"
fi

# Reject every relative or empty SSL_CERT_DIR component before the trusted Git
# child can inspect or mutate the checkout.
for ssl_cert_dir_case in \
    relative absolute-relative relative-absolute trailing-empty \
    middle-empty leading-empty only-separator; do
    case $ssl_cert_dir_case in
        relative) ssl_cert_dir_value=relative ;;
        absolute-relative) ssl_cert_dir_value=/absolute:relative ;;
        relative-absolute) ssl_cert_dir_value=relative:/absolute ;;
        trailing-empty) ssl_cert_dir_value=/absolute: ;;
        middle-empty) ssl_cert_dir_value=/absolute::/other ;;
        leading-empty) ssl_cert_dir_value=:/absolute ;;
        only-separator) ssl_cert_dir_value=: ;;
    esac
    setup_case "trusted-git-ssl-cert-dir-$ssl_cert_dir_case"
    create_regular_repo "$entry_path"
    write_standard_git_config "$entry_path"
    trusted_git_environment_log=$case_dir/trusted-git-environment.log
    : > "$trusted_git_environment_log"
    export MOGUET_TEST_TRUSTED_GIT_ENVIRONMENT_LOG=$trusted_git_environment_log
    export MOGUET_TEST_TRUSTED_GIT_UNSAFE_MARKER=$outside_dir/unsafe-environment-marker
    export MOGUET_TEST_TRUSTED_GIT_FETCH_MARKER=.trusted-fetch-target
    SSL_CERT_DIR=$ssl_cert_dir_value
    export SSL_CERT_DIR
    snapshot_directory "$cache_root" "$case_dir/cache-before.snapshot"
    snapshot_directory "$outside_dir" "$case_dir/outside-before.snapshot"
    run_fail "$case_dir/output" fetch clean-root
    assert_contains "non-absolute custom CA path" "$case_dir/output"
    assert_contains "SSL_CERT_DIR" "$case_dir/output"
    assert_not_contains "/absolute" "$case_dir/output"
    assert_not_contains "/other" "$case_dir/output"
    assert_not_contains "relative" "$case_dir/output"
    assert_command_absent "git config --get remote.origin.url"
    assert_command_absent "git fetch origin"
    assert_path_absent "$outside_dir/unsafe-environment-marker"
    assert_path_absent "$entry_path/.trusted-fetch-target"
    if [ -s "$trusted_git_environment_log" ]; then
        fail "invalid SSL_CERT_DIR reached trusted Git child: $ssl_cert_dir_case"
    fi
    assert_directory_unchanged "$cache_root" "$case_dir/cache-before.snapshot"
    assert_directory_unchanged "$outside_dir" "$case_dir/outside-before.snapshot"
done

# Other custom CA settings remain single absolute paths. Relative values are
# rejected before the trusted Git child, without exposing the value.
for ca_variable in \
    SSL_CERT_FILE GIT_SSL_CAINFO GIT_SSL_CAPATH; do
    setup_case "trusted-git-relative-ca-$ca_variable"
    create_regular_repo "$entry_path"
    write_standard_git_config "$entry_path"
    trusted_git_environment_log=$case_dir/trusted-git-environment.log
    : > "$trusted_git_environment_log"
    export MOGUET_TEST_TRUSTED_GIT_ENVIRONMENT_LOG=$trusted_git_environment_log
    export MOGUET_TEST_TRUSTED_GIT_UNSAFE_MARKER=$outside_dir/unsafe-environment-marker
    relative_ca_value=relative/secret-ca-value-$ca_variable
    export "$ca_variable=$relative_ca_value"
    run_fail "$case_dir/output" fetch clean-root
    assert_contains "non-absolute custom CA path" "$case_dir/output"
    assert_contains "$ca_variable" "$case_dir/output"
    assert_not_contains "$relative_ca_value" "$case_dir/output"
    assert_command_absent "git config --get remote.origin.url"
    assert_command_absent "git fetch origin"
    assert_path_absent "$outside_dir/unsafe-environment-marker"
    if [ -s "$trusted_git_environment_log" ]; then
        fail "relative custom CA path reached trusted Git child: $ca_variable"
    fi
done

# --- Managed checkout local-config allowlist ---

for config_case in \
    core-worktree include-path include-if filter-process core-fsmonitor \
    core-hookspath credential-helper core-sshcommand url-insteadof; do
    setup_case "trusted-git-local-config-$config_case"
    create_regular_repo "$entry_path"
    write_standard_git_config "$entry_path"
    external_command_marker=$outside_dir/external-command-marker
    case $config_case in
        core-worktree)
            printf '%s\n' '[core]' "    worktree = $outside_dir" >> \
                "$entry_path/.git/config"
            ;;
        include-path)
            printf '%s\n' '[include]' \
                "    path = $outside_dir/included.gitconfig" >> \
                "$entry_path/.git/config"
            ;;
        include-if)
            printf '%s\n' "[includeIf \"gitdir:$outside_dir/**\"]" \
                "    path = $outside_dir/included.gitconfig" >> \
                "$entry_path/.git/config"
            ;;
        filter-process)
            printf '%s\n' '[filter "moguet-test"]' \
                "    process = sh -c 'printf unsafe > $external_command_marker'" >> \
                "$entry_path/.git/config"
            ;;
        core-fsmonitor)
            printf '%s\n' '[core]' \
                "    fsmonitor = sh -c 'printf unsafe > $external_command_marker'" >> \
                "$entry_path/.git/config"
            ;;
        core-hookspath)
            printf '%s\n' '[core]' "    hooksPath = $outside_dir/hooks" >> \
                "$entry_path/.git/config"
            ;;
        credential-helper)
            printf '%s\n' '[credential]' \
                "    helper = !sh -c 'printf unsafe > $external_command_marker'" >> \
                "$entry_path/.git/config"
            ;;
        core-sshcommand)
            printf '%s\n' '[core]' \
                "    sshCommand = sh -c 'printf unsafe > $external_command_marker'" >> \
                "$entry_path/.git/config"
            ;;
        url-insteadof)
            printf '%s\n' '[url "https://example.invalid/"]' \
                '    insteadOf = https://aur.archlinux.org/' >> \
                "$entry_path/.git/config"
            ;;
    esac
    snapshot_directory "$entry_path" "$case_dir/checkout-before.snapshot"
    snapshot_directory "$outside_dir" "$case_dir/outside-before.snapshot"
    run_fail "$case_dir/output" fetch clean-root
    assert_contains "unsafe local Git configuration" "$case_dir/output"
    assert_not_contains "$outside_dir" "$case_dir/output"
    assert_command "git config --get remote.origin.url"
    assert_command_absent "git fetch origin"
    assert_path_absent "$external_command_marker"
    assert_directory_unchanged "$entry_path" "$case_dir/checkout-before.snapshot"
    assert_directory_unchanged "$outside_dir" "$case_dir/outside-before.snapshot"
done

# The local-config parser consumes real `git config --null --list` output.
# Build a valid allowlisted config within two bytes of the 1 MiB policy limit
# without retaining or printing that binary output in the test harness.
max_local_config_output=$((1024 * 1024))
setup_case trusted-git-local-config-near-capture-limit
create_regular_repo "$entry_path"
write_standard_git_config "$entry_path"
base_config_output_size=$(git_config_output_size "$entry_path/.git/config")
append_padding_branch_git_config "$entry_path/.git/config" 1
one_branch_output_size=$(git_config_output_size "$entry_path/.git/config")
one_branch_growth=$((one_branch_output_size - base_config_output_size))
if [ "$one_branch_growth" -le 0 ]; then
    fail "unable to size real Git local-config output"
fi
remaining_output_budget=$((
    max_local_config_output - base_config_output_size - one_branch_growth
))
if [ "$remaining_output_budget" -lt 0 ]; then
    fail "ordinary real Git local-config output exceeds capture policy"
fi
near_limit_branch_length=$((1 + remaining_output_budget / 3))
write_standard_git_config "$entry_path"
append_padding_branch_git_config \
    "$entry_path/.git/config" "$near_limit_branch_length"
near_limit_output_size=$(git_config_output_size "$entry_path/.git/config")
if [ "$near_limit_output_size" -gt "$max_local_config_output" ] ||
   [ $((max_local_config_output - near_limit_output_size)) -gt 2 ]; then
    fail "real Git local-config fixture is not near the capture limit"
fi
run_ok "$case_dir/output" fetch clean-root
assert_command "git config --get remote.origin.url"
assert_command "git fetch origin"

# The same real-Git output path must stop storing at the bounded capture limit,
# reject deterministically, reap the child, and leave the next invocation usable.
setup_case trusted-git-local-config-over-capture-limit
create_regular_repo "$entry_path"
write_standard_git_config "$entry_path"
oversized_branch_length=$((near_limit_branch_length + 1024))
append_padding_branch_git_config \
    "$entry_path/.git/config" "$oversized_branch_length"
oversized_output_size=$(git_config_output_size "$entry_path/.git/config")
if [ "$oversized_output_size" -le "$max_local_config_output" ]; then
    fail "real Git oversized local-config fixture did not cross the limit"
fi
run_fail "$case_dir/output" fetch clean-root
assert_contains "unsafe local Git configuration" "$case_dir/output"
assert_command "git config --get remote.origin.url"
assert_command_absent "git fetch origin"

write_standard_git_config "$entry_path"
run_ok "$case_dir/subsequent-output" fetch clean-root
assert_command "git config --get remote.origin.url"
assert_command "git fetch origin"

# Preserve parser rejection for framing failures independently of the storage
# limit. These fixtures are emitted by the trusted-Git test executable only.
for raw_config_case in truncated invalid-record; do
    setup_case "trusted-git-local-config-$raw_config_case"
    create_regular_repo "$entry_path"
    write_standard_git_config "$entry_path"
    raw_config_output=$case_dir/raw-config-output
    case $raw_config_case in
        truncated)
            printf 'core.repositoryformatversion\n0' > "$raw_config_output"
            ;;
        invalid-record)
            printf 'invalid-record\0' > "$raw_config_output"
            ;;
    esac
    export MOGUET_TEST_GIT_CONFIG_RAW_OUTPUT_FILE=$raw_config_output
    run_fail "$case_dir/output" fetch clean-root
    assert_contains "unsafe local Git configuration" "$case_dir/output"
    assert_command "git config --get remote.origin.url"
    assert_command_absent "git fetch origin"
done

# --- Build/update/reset and re-clone boundaries ---

setup_case build-directory-symlink
mkdir -p "$cache_root"
create_checkout "$outside_dir/checkout"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$outside_dir/checkout" "$entry_path"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_symlink_rejection "$case_dir/output" "$entry_path"
assert_no_cache_mutation_commands
assert_symlink "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-reclone-symlink
mkdir -p "$cache_root"
create_checkout "$outside_dir/checkout"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$outside_dir/checkout" "$entry_path"
export MOGUET_TEST_GIT_REMOTE_URL=https://example.invalid/wrong.git
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_symlink_rejection "$case_dir/output" "$entry_path"
assert_no_cache_mutation_commands
assert_symlink "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

# --- Review-time replacement and successful-clone descendant boundaries ---

setup_case build-review-pkgbuild-replaced
create_regular_repo "$entry_path"
printf 'external reviewed artifact\n' > "$outside_dir/PKGBUILD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export EDITOR=$repo_root/tests/stubs/moguet-test-editor
export MOGUET_TEST_EDITOR_REPLACE_TARGET=./PKGBUILD
export MOGUET_TEST_EDITOR_SYMLINK_TARGET=$outside_dir/PKGBUILD
run_build_tty_fail "$case_dir/output" 'y\n'
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "symlink."
assert_command "moguet-test-editor ./PKGBUILD"
assert_command_before "git reset --hard origin/main" "moguet-test-editor ./PKGBUILD"
assert_no_build_or_install_commands
assert_symlink "$entry_path/PKGBUILD"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-review-install-removed
create_regular_repo "$entry_path"
printf 'post_install() { :; }\n' > "$entry_path/clean-root.install"
export EDITOR=$repo_root/tests/stubs/moguet-test-editor
export MOGUET_TEST_EDITOR_REMOVE_TARGET=clean-root.install
run_build_tty_fail "$case_dir/output" 'y\n'
assert_descendant_rejection "$case_dir/output" "$entry_path/clean-root.install" "non-regular file."
assert_command "moguet-test-editor ./PKGBUILD"
assert_command_absent "moguet-test-editor ./clean-root.install"
assert_no_build_or_install_commands
assert_path_absent "$entry_path/clean-root.install"

setup_case fetch-clone-unsafe-git-symlink
create_clone_fixture
rmdir "$clone_fixture/.git"
mkdir -p "$outside_dir/external.git"
printf 'external git metadata\n' > "$outside_dir/external.git/marker"
ln -s "$outside_dir/external.git" "$clone_fixture/.git"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" fetch clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/.git" "symlink."
assert_only_clone_after_metadata "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-clone-unsafe-pkgbuild-symlink
create_clone_fixture
printf 'external PKGBUILD\n' > "$outside_dir/PKGBUILD"
rm "$clone_fixture/PKGBUILD"
ln -s "$outside_dir/PKGBUILD" "$clone_fixture/PKGBUILD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" fetch clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "symlink."
assert_only_clone_after_metadata "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-clone-unsafe-install-symlink
create_clone_fixture
printf 'external install script\n' > "$outside_dir/clean-root.install"
ln -s "$outside_dir/clean-root.install" "$clone_fixture/clean-root.install"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" fetch clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/clean-root.install" "symlink."
assert_only_clone_after_metadata "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-clone-unsafe-git-symlink
create_clone_fixture
rmdir "$clone_fixture/.git"
mkdir -p "$outside_dir/external.git"
printf 'external git metadata\n' > "$outside_dir/external.git/marker"
ln -s "$outside_dir/external.git" "$clone_fixture/.git"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/.git" "symlink."
assert_only_clone_after_metadata "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-clone-unsafe-pkgbuild-symlink
create_clone_fixture
printf 'external PKGBUILD\n' > "$outside_dir/PKGBUILD"
rm "$clone_fixture/PKGBUILD"
ln -s "$outside_dir/PKGBUILD" "$clone_fixture/PKGBUILD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "symlink."
assert_only_clone_after_metadata "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-clone-unsafe-install-symlink
create_clone_fixture
printf 'external install script\n' > "$outside_dir/clean-root.install"
ln -s "$outside_dir/clean-root.install" "$clone_fixture/clean-root.install"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_descendant_rejection "$case_dir/output" "$entry_path/clean-root.install" "symlink."
assert_only_clone_after_metadata "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-clone-remote-mismatch-rollback
create_clone_fixture
printf 'outside sibling marker\n' > "$outside_dir/marker"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export MOGUET_TEST_GIT_REMOTE_URL=https://example.invalid/wrong.git
run_fail "$case_dir/output" fetch clean-root
assert_contains "Remote URL mismatch for clean-root." "$case_dir/output"
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command "git config --get remote.origin.url"
assert_command_before "git clone https://aur.archlinux.org/clean-root.git clean-root" "git config --get remote.origin.url"
assert_command_absent "git fetch origin"
assert_command_absent "git reset --hard origin/main"
assert_command_absent "moguet-test-editor ./PKGBUILD"
assert_no_build_or_install_commands
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-clone-remote-mismatch-rollback
create_clone_fixture
printf 'outside sibling marker\n' > "$outside_dir/marker"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export MOGUET_TEST_GIT_REMOTE_URL=https://example.invalid/wrong.git
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_contains "Remote URL mismatch for clean-root" "$case_dir/output"
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command "git config --get remote.origin.url"
assert_command_before "git clone https://aur.archlinux.org/clean-root.git clean-root" "git config --get remote.origin.url"
assert_command_absent "git fetch origin"
assert_command_absent "git reset --hard origin/main"
assert_command_absent "moguet-test-editor ./PKGBUILD"
assert_no_build_or_install_commands
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-clone-valid-descendants
create_clone_fixture
printf 'post_install() { :; }\n' > "$clone_fixture/clean-root.install"
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok "$case_dir/output" --noedit --nodiff build clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command "git config --get remote.origin.url"
assert_command "makepkg --packagelist"
assert_command "makepkg -sc"
assert_command_before "git clone https://aur.archlinux.org/clean-root.git clean-root" "git config --get remote.origin.url"
assert_command_before "git config --get remote.origin.url" "makepkg --packagelist"
if [ ! -d "$entry_path/.git" ] || [ -L "$entry_path/.git" ] ||
   [ ! -f "$entry_path/PKGBUILD" ] || [ -L "$entry_path/PKGBUILD" ] ||
   [ ! -f "$entry_path/clean-root.install" ] || [ -L "$entry_path/clean-root.install" ]; then
    fail "valid clone descendants were not retained as regular entries: $entry_path"
fi

# --- Regular paths retain fetch/build/fresh-clone behavior ---

setup_case regular-existing-fetch
create_regular_repo "$entry_path"
run_ok "$case_dir/output" fetch clean-root
assert_command "git config --get remote.origin.url"
assert_command "git fetch origin"
assert_command_before "git config --get remote.origin.url" "git fetch origin"
assert_command_absent "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_absent "git reset --hard origin/main"
assert_command_absent "moguet-test-editor ./PKGBUILD"
assert_no_build_or_install_commands

setup_case regular-missing-fetch
run_ok "$case_dir/output" fetch clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command "git config --get remote.origin.url"
assert_command_before "git clone https://aur.archlinux.org/clean-root.git clean-root" "git config --get remote.origin.url"
assert_command_absent "git fetch origin"
assert_command_absent "git reset --hard origin/main"
assert_command_absent "moguet-test-editor ./PKGBUILD"
assert_no_build_or_install_commands
if [ ! -d "$entry_path/.git" ] || [ -L "$entry_path/.git" ] ||
   [ ! -f "$entry_path/PKGBUILD" ] || [ -L "$entry_path/PKGBUILD" ]; then
    fail "regular missing fetch did not create a repository: $entry_path"
fi

setup_case legacy-cache-preserved-by-fetch
create_legacy_cache_fixture
run_ok "$case_dir/output" fetch clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_directory_unchanged \
    "$legacy_cache_root" "$case_dir/legacy-before.snapshot"
if [ ! -d "$entry_path/.git" ]; then
    fail "fetch did not create checkout below the Moguet cache root"
fi

setup_case regular-existing-build
create_regular_repo "$entry_path"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_command "git config --get remote.origin.url"
assert_command "git fetch origin"
assert_command "git reset --hard origin/main"
assert_command "makepkg --packagelist"
assert_command_before "git config --get remote.origin.url" "git fetch origin"
assert_command_before "git fetch origin" "git reset --hard origin/main"
assert_command_before "git reset --hard origin/main" "makepkg --packagelist"

setup_case legacy-cache-preserved-by-build
create_clone_fixture
create_legacy_cache_fixture
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_ok "$case_dir/output" --noedit --nodiff build clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command "makepkg --packagelist"
assert_directory_unchanged \
    "$legacy_cache_root" "$case_dir/legacy-before.snapshot"
if [ ! -d "$entry_path/.git" ]; then
    fail "build did not create checkout below the Moguet cache root"
fi

setup_case regular-existing-review
create_regular_repo "$entry_path"
printf 'post_install() { :; }\n' > "$entry_path/clean-root.install"
export EDITOR=$repo_root/tests/stubs/moguet-test-editor
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_build_tty_ok "$case_dir/output" 'y\ny\ny\n'
assert_contains "Review target: PKGBUILD" "$case_dir/output"
assert_contains "clean-root.install" "$case_dir/output"
assert_command "moguet-test-editor ./PKGBUILD"
assert_command "moguet-test-editor ./clean-root.install"
assert_command "makepkg --packagelist"
assert_command "makepkg -sc"
assert_command_before "git reset --hard origin/main" "moguet-test-editor ./PKGBUILD"
assert_command_before "moguet-test-editor ./PKGBUILD" "moguet-test-editor ./clean-root.install"
assert_command_before "moguet-test-editor ./clean-root.install" "makepkg --packagelist"
assert_editor_argv_log 'argv-begin
arg[0]=<./PKGBUILD>
target=<./PKGBUILD>
argv-end
argv-begin
arg[0]=<./clean-root.install>
target=<./clean-root.install>
argv-end'
assert_editor_targets_not_option_like

setup_case regular-existing-leading-hyphen-review
create_regular_repo "$entry_path"
printf 'post_install() { :; }\n' > "$entry_path/-option.install"
export EDITOR=$repo_root/tests/stubs/moguet-test-editor
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_build_tty_ok "$case_dir/output" 'y\ny\ny\n'
assert_contains "-option.install" "$case_dir/output"
assert_command "moguet-test-editor ./PKGBUILD"
assert_command "moguet-test-editor ./-option.install"
assert_command "makepkg --packagelist"
assert_command "makepkg -sc"
assert_command_before "moguet-test-editor ./PKGBUILD" "moguet-test-editor ./-option.install"
assert_command_before "moguet-test-editor ./-option.install" "makepkg --packagelist"
assert_editor_argv_log 'argv-begin
arg[0]=<./PKGBUILD>
target=<./PKGBUILD>
argv-end
argv-begin
arg[0]=<./-option.install>
target=<./-option.install>
argv-end'
assert_editor_targets_not_option_like

setup_case regular-existing-leading-hyphen-editor-failure
create_regular_repo "$entry_path"
printf 'post_install() { :; }\n' > "$entry_path/-option.install"
export EDITOR=$repo_root/tests/stubs/moguet-test-editor
export MOGUET_TEST_EDITOR_EXIT_CODE=42
run_build_tty_fail "$case_dir/output" 'n\ny\n'
assert_contains "Editor failed." "$case_dir/output"
assert_command "moguet-test-editor ./-option.install"
assert_no_build_or_install_commands
assert_editor_argv_log 'argv-begin
arg[0]=<./-option.install>
target=<./-option.install>
argv-end'
assert_editor_targets_not_option_like

setup_case fetch-existing-remote-mismatch
create_regular_repo "$entry_path"
printf 'old clone marker\n' > "$entry_path/old-marker"
printf 'https://example.invalid/wrong.git\n' > "$entry_path/.git/.moguet-test-remote-url"
run_fail "$case_dir/output" fetch clean-root
assert_contains "Remote URL mismatch for clean-root." "$case_dir/output"
assert_command "git config --get remote.origin.url"
assert_command_absent "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_absent "git fetch origin"
assert_command_absent "git reset --hard origin/main"
assert_command_absent "moguet-test-editor ./PKGBUILD"
assert_no_build_or_install_commands
assert_contains "old clone marker" "$entry_path/old-marker"
if [ ! -d "$entry_path/.git" ]; then
    fail "fetch remote mismatch removed the existing checkout"
fi

# Existing derived state is preserved on failure, including Git config and
# filesystem identity. Recovery must be a separate explicit operation.
setup_case build-existing-remote-mismatch
create_regular_repo "$entry_path"
printf 'old clone marker\n' > "$entry_path/old-marker"
write_standard_git_config "$entry_path" https://example.invalid/wrong.git
snapshot_directory "$entry_path" "$case_dir/checkout-before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_contains "Remote URL mismatch for existing cache checkout clean-root." "$case_dir/output"
assert_contains "Build stopped; cache preserved. Check the configured source and cache before retrying." "$case_dir/output"
assert_command "git config --get remote.origin.url"
assert_command_absent "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_absent "git fetch origin"
assert_command_absent "git reset --hard origin/main"
assert_no_build_or_install_commands
assert_directory_unchanged "$entry_path" "$case_dir/checkout-before.snapshot"

setup_case build-existing-nonrepository
mkdir -p "$entry_path"
printf 'existing cache contents\n' > "$entry_path/sentinel"
snapshot_directory "$entry_path" "$case_dir/checkout-before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_contains "Missing .git directory for existing cache checkout clean-root." "$case_dir/output"
assert_contains "Build stopped; cache preserved. Check the configured source and cache before retrying." "$case_dir/output"
assert_command_absent "git config --get remote.origin.url"
assert_command_absent "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_absent "git fetch origin"
assert_command_absent "git reset --hard origin/main"
assert_no_build_or_install_commands
assert_directory_unchanged "$entry_path" "$case_dir/checkout-before.snapshot"

setup_case build-existing-regular-file
mkdir -p "$cache_root"
printf 'existing cache file\n' > "$entry_path"
snapshot_directory "$cache_root" "$case_dir/cache-before.snapshot"
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_contains "directory required" "$case_dir/output"
assert_command_absent "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_absent "git fetch origin"
assert_command_absent "git reset --hard origin/main"
assert_no_build_or_install_commands
assert_directory_unchanged "$cache_root" "$case_dir/cache-before.snapshot"

# --- Clone failure rollback for both fetch and build call sites ---

setup_case fetch-post-fetch-descendant-revalidation
create_regular_repo "$entry_path"
printf 'outside PKGBUILD\n' > "$outside_dir/PKGBUILD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export MOGUET_TEST_GIT_FETCH_REPLACE_PKGBUILD_SYMLINK_TARGET=$outside_dir/PKGBUILD
run_fail "$case_dir/output" fetch clean-root
assert_command "git fetch origin"
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "symlink."
assert_symlink "$entry_path/PKGBUILD"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-post-fetch-descendant-revalidation
create_regular_repo "$entry_path"
printf 'outside PKGBUILD\n' > "$outside_dir/PKGBUILD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export MOGUET_TEST_GIT_FETCH_REPLACE_PKGBUILD_SYMLINK_TARGET=$outside_dir/PKGBUILD
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_command "git fetch origin"
assert_command_absent "git symbolic-ref --short HEAD"
assert_command_absent "git show-ref --verify --quiet refs/remotes/origin/main"
assert_command_absent "git show-ref --verify --quiet refs/remotes/origin/master"
assert_command_absent "git reset --hard origin/main"
assert_command_absent "makepkg --packagelist"
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "symlink."
assert_symlink "$entry_path/PKGBUILD"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-post-config-clone-rollback
printf 'outside PKGBUILD\n' > "$outside_dir/PKGBUILD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export MOGUET_TEST_GIT_CONFIG_REPLACE_PKGBUILD_SYMLINK_TARGET=$outside_dir/PKGBUILD
run_fail "$case_dir/output" fetch clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "symlink."
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-post-config-clone-rollback
printf 'outside PKGBUILD\n' > "$outside_dir/PKGBUILD"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export MOGUET_TEST_GIT_CONFIG_REPLACE_PKGBUILD_SYMLINK_TARGET=$outside_dir/PKGBUILD
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_absent "makepkg --packagelist"
assert_descendant_rejection "$case_dir/output" "$entry_path/PKGBUILD" "symlink."
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case fetch-clone-failure-rollback
printf 'outside sibling marker\n' > "$outside_dir/marker"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export MOGUET_TEST_GIT_CLONE_EXIT_CODE=42
run_fail "$case_dir/output" fetch clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case build-clone-failure-rollback
printf 'outside sibling marker\n' > "$outside_dir/marker"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export MOGUET_TEST_GIT_CLONE_EXIT_CODE=42
run_fail "$case_dir/output" --noedit --nodiff build clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_absent "makepkg --packagelist"
assert_command_absent "makepkg -sc"
assert_path_absent "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

# clone failureがsymlinkを残しても、destructorは再検証でoutside cleanupを拒否する。
setup_case fetch-clone-symlink-rollback-refusal
create_checkout "$outside_dir/checkout"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
export MOGUET_TEST_GIT_CLONE_SYMLINK_TARGET=$outside_dir/checkout
export MOGUET_TEST_GIT_CLONE_EXIT_CODE=42
run_fail "$case_dir/output" fetch clean-root
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_contains "Refusing unsafe clone rollback" "$case_dir/output"
assert_symlink "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

# --- Cache cleanup preflight and safe-path ordering ---

setup_case clean-entry-symlink
mkdir -p "$cache_root"
create_checkout "$outside_dir/checkout"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$outside_dir/checkout" "$entry_path"
run_clean_tty_fail "$case_dir/output"
assert_symlink_rejection "$case_dir/output" "$entry_path"
assert_log_empty
assert_symlink "$entry_path"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case clean-cache-root-symlink
root_target=$outside_dir/cache-root
create_checkout "$root_target/clean-root"
snapshot_directory "$outside_dir" "$case_dir/before.snapshot"
ln -s "$root_target" "$cache_root"
run_clean_tty_fail "$case_dir/output"
assert_symlink_rejection "$case_dir/output" "$cache_root"
assert_log_empty
assert_symlink "$cache_root"
assert_directory_unchanged "$outside_dir" "$case_dir/before.snapshot"

setup_case regular-clean
mkdir -p "$cache_root/regular-entry"
printf 'cached build content\n' > "$cache_root/regular-entry/artifact"
new_legacy_named_entry=$cache_root/jpacker.log
printf 'ordinary cache content\n' > "$new_legacy_named_entry"

create_legacy_cache_fixture
run_clean_tty_ok "$case_dir/output"
assert_only_command "sudo pacman -Sc"
assert_path_absent "$cache_root/regular-entry"
assert_path_absent "$new_legacy_named_entry"
assert_directory_unchanged \
    "$legacy_cache_root" "$case_dir/legacy-before.snapshot"
assert_contains "Moguet cache cleaned." "$case_dir/output"
assert_output_before "Running: sudo pacman" "Clean Moguet build cache" "$case_dir/output"

echo "build cache symlink integration tests: all checks passed"
