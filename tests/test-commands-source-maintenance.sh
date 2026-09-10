#!/bin/sh
set -eu
umask 077

LC_ALL=C
export LC_ALL

test_binary=$1
source_install_test_binary=$2
upgrade_metadata_test_binary=$3
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
MOGUET_TEST_REPOSITORY_ROOT=$repo_root
export MOGUET_TEST_REPOSITORY_ROOT
. "$repo_root/tests/test-command-safety.sh"
. "$repo_root/scripts/validation-status.sh"

snapshot_source_tree_raw() {
    checked_source_root=$1
    (
        CDPATH='' cd "$checked_source_root" || exit $?
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
        snapshot_source_tree_raw "$MOGUET_TEST_SNAPSHOT_FAULT_ROOT"; then
        exit 0
    else
        snapshot_status=$?
    fi
    printf 'source tree snapshot failed with status %s; raw=%s\n' \
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

count_command_output_lines() {
    count_label=$1
    shift
    count_raw=$case_dir/$count_label.raw
    if validation_capture_output "$count_raw" "$@"; then
        wc -l <"$count_raw"
        return 0
    else
        count_status=$?
    fi
    echo "$count_label producer failed with status $count_status; raw=$count_raw" >&2
    return "$count_status"
}

if ! command -v script >/dev/null 2>&1; then
    echo "script(1) is required for source/maintenance command tests" >&2
    exit 1
fi

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
export PATH=$repo_root/tests/stubs/source-maintenance:$repo_root/tests/stubs:/usr/bin:/bin
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
require_exact_test_command makepkg "$repo_root/tests/stubs/makepkg"
require_exact_test_command pacman "$repo_root/tests/stubs/pacman"
require_exact_test_command sudo "$repo_root/tests/stubs/source-maintenance/sudo"
require_exact_test_command git "$repo_root/tests/stubs/git"
require_exact_test_command moguet-test-editor \
    "$repo_root/tests/stubs/source-maintenance/moguet-test-editor"
upgrade_metadata_path=$repo_root/tests/stubs/upgrade-baseline-metadata:$PATH
(
    PATH=$upgrade_metadata_path
    export PATH
    require_exact_test_command pacman-conf \
        "$repo_root/tests/stubs/upgrade-baseline-metadata/pacman-conf"
)
export MOGUET_TEST_AUR_RPC_BASE_URL=http://127.0.0.1:$port/rpc/

setup_case() {
    case_name=$1
    case_dir=$tmp_dir/cases/$case_name
    command_log=$case_dir/commands.log
    output_file=$case_dir/output
    stdout_file=$case_dir/stdout
    stderr_file=$case_dir/stderr
    preference_dir=$case_dir/xdg-config/moguet/source-build.d
    cache_root=$case_dir/xdg-cache/moguet
    sudo_failures=$case_dir/sudo-failures
    config_file=$case_dir/config.toml
    package_metadata_state=$case_dir/package-metadata-state
    repository_metadata_state=$case_dir/repository-metadata-state

    mkdir -p \
        "$case_dir/home" "$case_dir/xdg-config" "$case_dir/xdg-cache" \
        "$preference_dir"
    chmod 700 "$case_dir/xdg-config" \
        "$case_dir/xdg-config/moguet" "$preference_dir"
    : > "$command_log"
    : > "$request_log"
    : > "$sudo_failures"
    : > "$package_metadata_state"
    : > "$repository_metadata_state"
    printf '%s\n' 'schema_version = 1' > "$config_file"

    export HOME=$case_dir/home
    export XDG_CONFIG_HOME=$case_dir/xdg-config
    export XDG_CACHE_HOME=$case_dir/xdg-cache
    export MOGUET_TEST_COMMAND_LOG=$command_log
    export MOGUET_TEST_CONFIG_FILE=$config_file
    export MOGUET_TEST_SOURCE_MAINTENANCE_FAIL_EXACT_FILE=$sudo_failures
    export MOGUET_TEST_PACMAN_EXIT_CODE=1
    export MOGUET_TEST_SUDO_EXIT_CODE=0
    export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
    export MOGUET_TEST_PACKAGE_METADATA_STATE_FILE=$package_metadata_state
    export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
    export MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE=$repository_metadata_state
    MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
    export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST

    unset MOGUET_TEST_PACMAN_REPO_PACKAGES
    unset MOGUET_TEST_PACMAN_Q_OUTPUT
    unset MOGUET_TEST_PACMAN_Q_OUTPUT_FILE
    unset MOGUET_TEST_PACMAN_Q_EXIT_CODE
    unset MOGUET_TEST_PACMAN_QM_OUTPUT
    unset MOGUET_TEST_APP_CONFIG_CASE
    unset MOGUET_TEST_RELEASE_STATE_LOG_BEFORE_DISPATCH
    unset MOGUET_TEST_GIT_REMOTE_URL
    unset MOGUET_TEST_GIT_CLONE_EXIT_CODE
    unset MOGUET_TEST_GIT_CLONE_FAIL_DESTINATION
    unset MOGUET_TEST_GIT_CLONE_FAIL_DESTINATION_EXIT_CODE
    unset MOGUET_TEST_GIT_CLONE_SYMLINK_TARGET
    unset MOGUET_TEST_GIT_CLONE_FIXTURE_DIR
    unset MOGUET_TEST_GIT_RESET_SRCINFO_FIXTURE
    unset MOGUET_TEST_GIT_SYMBOLIC_REF
    unset MOGUET_TEST_GIT_SYMBOLIC_REF_EXIT_CODE
    unset MOGUET_TEST_GIT_MAIN_REF_EXIT_CODE
    unset MOGUET_TEST_GIT_MASTER_REF_EXIT_CODE
    unset MOGUET_TEST_VERCMP_OUTPUT
    unset MOGUET_TEST_VERCMP_EXIT_CODE
    unset MOGUET_TEST_VERCMP_ARGV_LOG
    unset MOGUET_TEST_MAKEPKG_ARGV_LOG
    unset MOGUET_TEST_MAKEPKG_CWD_LOG
    unset MOGUET_TEST_MAKEPKG_ENV_LOG
    unset MOGUET_TEST_MAKEPKG_ENV_KEYS
    unset MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE
    unset MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES
    unset MOGUET_TEST_MAKEPKG_PACKAGE_BASE
    unset MOGUET_TEST_MAKEPKG_LOCAL_PACKAGE_BASE
    unset MOGUET_TEST_MAKEPKG_SRCINFO_OUTPUT_FILE
    unset MOGUET_TEST_MAKEPKG_PRINTSRCINFO_EXIT_CODE
    unset MOGUET_TEST_SOURCE_MAINTENANCE_FAIL_SUBSTRING
    unset MOGUET_TEST_SOURCE_MAINTENANCE_PACMAN_SYU_Q_OUTPUT_FILE
    unset MOGUET_TEST_SOURCE_MAINTENANCE_PACMAN_SC_RACE_PATH
    unset MOGUET_TEST_SOURCE_MAINTENANCE_PACMAN_SC_RACE_TARGET
    unset MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_SNAPSHOT_FILE
    unset MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_APPEND_LINE
    unset MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_FAIL_SUBSTRING
    unset MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND
    unset MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_SUBSTRING
    unset MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_TARGET
    unset MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_SOURCE
    unset MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_CONCURRENT_DESTINATION
    unset MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_CONCURRENT_CONTENTS
    unset MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE
    unset MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE_AT
    unset MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_PACKAGE
    unset MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_AT
    unset MOGUET_TEST_PACKAGE_METADATA_UNKNOWN_REASON_PACKAGE
    unset MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE
    unset MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_FAILURE_AT
    unset MOGUET_TEST_ALPM_VERCMP_EXPECTED_LHS
    unset MOGUET_TEST_ALPM_VERCMP_EXPECTED_RHS
    unset MOGUET_TEST_ALPM_VERCMP_RESULT
    unset MOGUET_TEST_MAKEPKG_PACKAGE_METADATA_STATE_AFTER_SUCCESS_FILE
    unset MOGUET_TEST_REPOSITORY_PACKAGE_BASE
    unset MOGUET_TEST_REPOSITORY_QUERY_FAILURE_PACKAGE
    unset MOGUET_TEST_SYNC_CACHE_FAILURE_REPOSITORY
    unset MOGUET_TEST_PACMAN_U_SUCCESS_LOG
    unset MOGUET_TEST_REPLACE_WORKSPACE_AFTER_PACMAN_U
    unset MOGUET_TEST_APPEND_LOCAL_PKGBUILD_AFTER_ASDEPS
    unset XDG_STATE_HOME
    unset EMPTY
    unset PKGDEST
    unset EDITOR
    unset VISUAL
}

refresh_repository_metadata_fixture() {
    : > "$repository_metadata_state"
    for package in ${MOGUET_TEST_PACMAN_REPO_PACKAGES:-}; do
        if [ -n "${MOGUET_TEST_REPOSITORY_PACKAGE_BASE:-}" ]; then
            printf 'core %s 1 1 %s\n' \
                "$package" "$MOGUET_TEST_REPOSITORY_PACKAGE_BASE" \
                >> "$repository_metadata_state"
        else
            printf 'core %s 1 1\n' "$package" \
                >> "$repository_metadata_state"
        fi
    done
}

run_ok() {
    refresh_repository_metadata_fixture
    : > "$command_log"
    : > "$request_log"
    if ! "$test_binary" "$@" </dev/null > "$output_file" 2>&1; then
        echo "expected command to succeed: $*" >&2
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_fail() {
    refresh_repository_metadata_fixture
    : > "$command_log"
    : > "$request_log"
    if ! validation_expect_status source-maintenance-business-failure 1 \
        "$output_file" "$output_file" "$test_binary" "$@" </dev/null; then
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_clean_low_nofile_fail() {
    soft_limit=$1
    : > "$command_log"
    : > "$request_log"
    if (
        ulimit -n "$soft_limit"
        "$test_binary" --noconfirm clean </dev/null > "$output_file" 2>&1
    ); then
        exit_code=0
    else
        exit_code=$?
    fi
    if ! validation_assert_status clean-low-nofile-business-failure 1 \
        "$exit_code" "$output_file" "$output_file" \
        "$test_binary" --noconfirm clean; then
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_upgrade_ok() {
    refresh_repository_metadata_fixture
    : > "$command_log"
    : > "$request_log"
    if ! PATH=$upgrade_metadata_path "$upgrade_metadata_test_binary" "$@" </dev/null > "$output_file" 2>&1; then
        echo "expected upgrade metadata command to succeed: $*" >&2
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
    # POLICY(#152): upgrade targetのinstalled stateはlibalpm snapshotだけから取得する。
    assert_command_content_absent "pacman -Q "
}

run_upgrade_fail() {
    refresh_repository_metadata_fixture
    : > "$command_log"
    : > "$request_log"
    exit_code=0
    if PATH=$upgrade_metadata_path "$upgrade_metadata_test_binary" "$@" </dev/null > "$output_file" 2>&1; then
        echo "expected upgrade metadata command to fail: $*" >&2
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    else
        exit_code=$?
    fi
    if [ "$exit_code" -ne 1 ]; then
        echo "unexpected upgrade metadata exit code: $exit_code (expected 1)" >&2
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
    # failure経路でもlegacy package-specific queryへfallbackしない。
    assert_command_content_absent "pacman -Q "
}

run_upgrade_split_fail() {
    refresh_repository_metadata_fixture
    : > "$command_log"
    : > "$request_log"
    exit_code=0
    if PATH=$upgrade_metadata_path "$upgrade_metadata_test_binary" "$@" </dev/null \
        > "$stdout_file" 2> "$stderr_file"; then
        echo "expected upgrade metadata command to fail: $*" >&2
        sed -n '1,260p' "$stdout_file" >&2
        sed -n '1,260p' "$stderr_file" >&2
        cat "$command_log" >&2
        exit 1
    else
        exit_code=$?
    fi
    if [ "$exit_code" -ne 1 ]; then
        echo "unexpected upgrade metadata exit code: $exit_code (expected 1)" >&2
        sed -n '1,260p' "$stdout_file" >&2
        sed -n '1,260p' "$stderr_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
    assert_command_content_absent "pacman -Q "
}

run_ok_stdin_closed() {
    : > "$command_log"
    : > "$request_log"
    if ! "$test_binary" "$@" <&- > "$output_file" 2>&1; then
        echo "expected command with closed stdin to succeed: $*" >&2
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_fail_nonblocking() {
    : > "$command_log"
    : > "$request_log"
    exit_code=0
    if timeout 5 "$test_binary" "$@" > "$output_file" 2>&1; then
        exit_code=0
    else
        exit_code=$?
    fi
    if ! validation_assert_status source-maintenance-nonblocking-failure 1 \
        "$exit_code" "$output_file" "$output_file" \
        timeout 5 "$test_binary" "$@"; then
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_clean_tty_ok() {
    answer=$1
    : > "$command_log"
    if ! printf '%s\n' "$answer" |
        script -qec "$test_binary clean" /dev/null > "$output_file" 2>&1; then
        echo "expected interactive clean to succeed with answer: $answer" >&2
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_clean_tty_fail() {
    answer=$1
    : > "$command_log"
    clean_input=$case_dir/clean-tty.input
    printf '%s\n' "$answer" >"$clean_input"
    if script -qec "$test_binary clean" /dev/null \
        <"$clean_input" >"$output_file" 2>&1; then
        tty_status=0
    else
        tty_status=$?
    fi
    if ! validation_assert_status clean-tty-business-failure 1 \
        "$tty_status" "$output_file" "$output_file" \
        script -qec "$test_binary clean" /dev/null; then
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_build_local_tty_fail() {
    answers=$1
    shift
    : > "$command_log"
    local_input=$case_dir/build-local-tty.input
    printf '%b' "$answers" >"$local_input"
    if timeout 5 script -qec \
        "$test_binary --noedit build --local $local_root $*" \
        /dev/null <"$local_input" >"$output_file" 2>&1; then
        tty_status=0
    else
        tty_status=$?
    fi
    if ! validation_assert_status build-local-tty-failure 1 \
        "$tty_status" "$output_file" "$output_file" timeout 5 \
        script -qec \
        "$test_binary --noedit build --local $local_root $*" \
        /dev/null; then
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_source_ok() {
    refresh_repository_metadata_fixture
    : > "$command_log"
    if ! "$source_install_test_binary" "$@" </dev/null > "$output_file" 2>&1; then
        echo "expected source-install scenario to succeed: $*" >&2
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_source_fail() {
    refresh_repository_metadata_fixture
    : > "$command_log"
    if ! validation_expect_status source-install-business-failure 1 \
        "$output_file" "$output_file" \
        "$source_install_test_binary" "$@" </dev/null; then
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_contains() {
    pattern=$1
    file=$2
    if ! grep -F -- "$pattern" "$file" >/dev/null; then
        echo "missing expected output: $pattern" >&2
        sed -n '1,260p' "$file" >&2
        exit 1
    fi
}

assert_not_contains() {
    pattern=$1
    file=$2
    if grep -F -- "$pattern" "$file" >/dev/null; then
        echo "unexpected output: $pattern" >&2
        sed -n '1,260p' "$file" >&2
        exit 1
    fi
}

assert_cleanup_partial_success_fixture() {
    success_log=$1
    if [ ! -s "$success_log" ]; then
        echo "fake pacman -U did not record a successful install in case $case_name" >&2
        exit 1
    fi
    installed_artifact=$(sed -n '1p' "$success_log")
    if [ -z "$installed_artifact" ] ||
       [ "$(wc -l < "$success_log")" -ne 1 ]; then
        echo "unexpected fake pacman -U success log in case $case_name" >&2
        cat "$success_log" >&2
        exit 1
    fi
    workspace_path=${installed_artifact%/*}
    displaced_workspace=${workspace_path}.installed-before-cleanup
    artifact_name=${installed_artifact##*/}
    if [ ! -d "$workspace_path" ] ||
       [ ! -f "$displaced_workspace/$artifact_name" ]; then
        echo "cleanup partial-success fixture was not retained in case $case_name" >&2
        exit 1
    fi
}

assert_output_line_count() {
    pattern=$1
    expected_count=$2
    file=$3
    actual_count=$(validation_grep_count -Fc -- "$pattern" "$file")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected output count for: $pattern" >&2
        echo "expected $expected_count, got $actual_count" >&2
        sed -n '1,260p' "$file" >&2
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

assert_command_content_absent() {
    unexpected=$1
    if grep -F -- "$unexpected" "$command_log" >/dev/null; then
        echo "unexpected command content: $unexpected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_separated_source_commands() {
    expected_count=$1
    assert_command_count "makepkg --packagelist" "$expected_count"
    assert_command_prefix_count "pacman -Qp --color never " "$expected_count"
    assert_command_prefix_count "sudo pacman -U " "$expected_count"
    assert_command_content_absent "pacman -D"
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
    expected_count=$2
    actual_count=$(validation_grep_count -Fxc -- "$expected" "$command_log")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected command count for: $expected" >&2
        echo "expected $expected_count, got $actual_count" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_prefix_count() {
    expected_prefix=$1
    expected_count=$2
    actual_count=$(awk -v prefix="$expected_prefix" '
        index($0, prefix) == 1 { count++ }
        END { print count + 0 }
    ' "$command_log")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected command prefix count for: $expected_prefix" >&2
        echo "expected $expected_count, got $actual_count" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_pattern_count() {
    expected_pattern=$1
    expected_count=$2
    actual_count=$(validation_grep_count -Ec -- \
        "$expected_pattern" "$command_log")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected command pattern count: $actual_count (expected $expected_count)" >&2
        echo "pattern: $expected_pattern" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_pattern_absent() {
    unexpected_pattern=$1
    if grep -E -- "$unexpected_pattern" "$command_log" >/dev/null; then
        echo "unexpected command pattern: $unexpected_pattern" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_pattern_before() {
    first_pattern=$1
    second_pattern=$2
    first_line=$(grep -nE -- "$first_pattern" "$command_log" |
        sed -n '1s/:.*//p')
    second_line=$(grep -nE -- "$second_pattern" "$command_log" |
        sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] ||
       [ "$first_line" -ge "$second_line" ]; then
        echo "unexpected command pattern order: $first_pattern -> $second_pattern" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_total_command_count() {
    expected=$1
    actual=$(wc -l < "$command_log")
    if [ "$actual" -ne "$expected" ]; then
        echo "unexpected command count: $actual (expected $expected)" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_request_log_empty() {
    if [ -s "$request_log" ]; then
        echo "unexpected AUR RPC request before package validation completed" >&2
        cat "$request_log" >&2
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

assert_command_occurrence_before() {
    first=$1
    first_occurrence=$2
    second=$3
    second_occurrence=$4
    first_line=$(grep -nFx -- "$first" "$command_log" | sed -n "${first_occurrence}s/:.*//p")
    second_line=$(grep -nFx -- "$second" "$command_log" | sed -n "${second_occurrence}s/:.*//p")
    if [ -z "$first_line" ] || [ -z "$second_line" ] || [ "$first_line" -ge "$second_line" ]; then
        echo "unexpected command order: $first (#$first_occurrence) -> $second (#$second_occurrence)" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_single_package_metadata_snapshots_around_syu() {
    package_name=$1
    expected_metadata_session_count=${2:-3}
    expected_total_session_count=$((expected_metadata_session_count + 1))
    assert_command_count "pacman-conf --verbose RootDir DBPath" 2
    assert_command_count "pacman-conf --repo-list" 1
    assert_command_count "alpm initialize" "$expected_total_session_count"
    assert_command_count "alpm sync-query core/$package_name" 1
    assert_command_count "alpm query $package_name" "$expected_metadata_session_count"
    assert_command_count "alpm release" "$expected_total_session_count"
    assert_command_absent "pacman -Si $package_name"
    assert_command_occurrence_before "alpm sync-query core/$package_name" 1 "alpm release" 1
    assert_command_occurrence_before "alpm query $package_name" 1 "alpm release" 2
    assert_command_occurrence_before "alpm release" 2 "sudo pacman -Syu --noconfirm" 1
    assert_command_occurrence_before "pacman-conf --verbose RootDir DBPath" 1 "alpm initialize" 1
    assert_command_occurrence_before "pacman-conf --verbose RootDir DBPath" 2 "alpm initialize" 2
    assert_command_occurrence_before "sudo pacman -Syu --noconfirm" 1 "alpm initialize" 3
    assert_command_occurrence_before "alpm initialize" 3 "alpm query $package_name" 2
    assert_command_occurrence_before "alpm query $package_name" 2 "alpm release" 3
}

assert_output_before() {
    first=$1
    second=$2
    file=$3
    first_line=$(grep -nF -- "$first" "$file" | sed -n '1s/:.*//p')
    second_line=$(grep -nF -- "$second" "$file" | sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] || [ "$first_line" -ge "$second_line" ]; then
        echo "unexpected output order: $first -> $second" >&2
        sed -n '1,260p' "$file" >&2
        exit 1
    fi
}

assert_path_absent() {
    path=$1
    if [ -e "$path" ] || [ -L "$path" ]; then
        echo "expected path to be absent: $path" >&2
        exit 1
    fi
}

prepare_fresh_pre_log_storage_case() {
    XDG_STATE_HOME=$case_dir/xdg-state
    export XDG_STATE_HOME
    mkdir -m 700 "$XDG_STATE_HOME"
    rmdir "$preference_dir"
    rmdir "$XDG_CONFIG_HOME/moguet"
}

assert_pre_log_storage_absent() {
    assert_path_absent "$XDG_CONFIG_HOME/moguet"
    assert_path_absent "$XDG_STATE_HOME/moguet"
    assert_path_absent "$HOME/.config/moguet"
    assert_path_absent "$HOME/.local/state/moguet"
    assert_not_contains "Started Moguet v" "$output_file"
}

assert_file_equals() {
    expected_file=$1
    actual_file=$2
    if ! cmp -s "$expected_file" "$actual_file"; then
        echo "unexpected file content: $actual_file" >&2
        diff -u "$expected_file" "$actual_file" >&2 || true
        exit 1
    fi
}

assert_file_empty() {
    file=$1
    if [ -s "$file" ]; then
        echo "expected file to be empty: $file" >&2
        sed -n '1,260p' "$file" >&2
        exit 1
    fi
}

snapshot_source_tree() {
    checked_source_root=$1
    source_tree_snapshot=$2
    if validation_capture_sorted_output \
        "$source_tree_snapshot.raw" "$source_tree_snapshot" \
        snapshot_source_tree_raw "$checked_source_root"; then
        return 0
    else
        snapshot_status=$?
    fi
    printf 'source tree snapshot failed with status %s; raw=%s\n' \
        "$snapshot_status" "$source_tree_snapshot.raw" >&2
    exit 1
}

assert_source_tree_unchanged() {
    checked_source_root=$1
    before_snapshot=$2
    after_snapshot=$case_dir/source-tree-after.snapshot
    snapshot_source_tree "$checked_source_root" "$after_snapshot"
    if ! cmp -s "$before_snapshot" "$after_snapshot"; then
        echo "local source tree changed: $checked_source_root" >&2
        diff -u "$before_snapshot" "$after_snapshot" >&2 || true
        exit 1
    fi
}

assert_argv_log() {
    actual_file=$1
    expected=$2
    expected_file=$case_dir/expected-argv.log
    printf '%s\n' "$expected" > "$expected_file"
    assert_file_equals "$expected_file" "$actual_file"
}

assert_makepkg_argv_log() {
    actual_file=$1
    expected=$2
    normalized_file=$case_dir/normalized-makepkg-argv.log
    sed 's#^\(arg\[[0-9][0-9]*\]=<PKGDEST=\).*>$#\1<owned>>#' \
        "$actual_file" > "$normalized_file"
    assert_argv_log "$normalized_file" "$expected"
}

write_upgrade_srcinfo() {
    srcinfo_file=$1
    srcinfo_version=$2
    srcinfo_release=$3
    {
        printf 'pkgbase = clean-root\n'
        printf 'pkgver = %s\n' "$srcinfo_version"
        printf 'pkgrel = %s\n' "$srcinfo_release"
        if [ "$#" -ge 4 ]; then
            printf 'epoch = %s\n' "$4"
        fi
        printf 'pkgname = clean-root\n'
    } > "$srcinfo_file"
}

prepare_local_source_root() {
    local_root=$1
    local_package_base=$2
    local_children=$3
    local_dependency=${4:-}

    mkdir -m 700 "$local_root"
    {
        printf 'pkgbase=%s\n' "$local_package_base"
        printf 'pkgver=1.0\n'
        printf 'pkgrel=1\n'
    } > "$local_root/PKGBUILD"
    {
        printf 'pkgbase = %s\n' "$local_package_base"
        printf 'pkgver = 1.0\n'
        printf 'pkgrel = 1\n'
        printf 'arch = any\n'
        if [ -n "$local_dependency" ]; then
            printf 'depends = %s\n' "$local_dependency"
        fi
        for local_child in $local_children; do
            printf 'pkgname = %s\n' "$local_child"
        done
    } > "$local_root/.SRCINFO"
    chmod 600 "$local_root/PKGBUILD" "$local_root/.SRCINFO"
    touch -t 202001010000 "$local_root/PKGBUILD"
    touch -t 202001010001 "$local_root/.SRCINFO"
}

prepare_upgrade_source_checkout() {
    source_package=$1
    source_version=${2:-2.0}
    source_checkout=$cache_root/$source_package

    mkdir -p "$source_checkout/.git"
    {
        printf 'pkgbase = %s\n' "$source_package"
        printf 'pkgver = %s\n' "$source_version"
        printf 'pkgrel = 1\n'
        printf 'pkgname = %s\n' "$source_package"
    } > "$source_checkout/.SRCINFO"
    printf 'pkgname=%s\npkgver=%s\npkgrel=1\n' \
        "$source_package" "$source_version" > "$source_checkout/PKGBUILD"
    printf 'https://gitlab.archlinux.org/archlinux/packaging/packages/%s.git\n' \
        "$source_package" > "$source_checkout/.git/.moguet-test-remote-url"
}

capture_two_source_preference_order() {
    preference_order_file=$case_dir/preference-order
    find "$preference_dir" -mindepth 1 -maxdepth 1 -type f \
        -printf '%f\n' > "$preference_order_file"
    if [ "$(wc -l < "$preference_order_file")" -ne 2 ]; then
        echo "expected exactly two source preferences in $case_name" >&2
        cat "$preference_order_file" >&2
        exit 1
    fi
    preference_first=$(sed -n '1p' "$preference_order_file")
    preference_second=$(sed -n '2p' "$preference_order_file")
}

setup_upgrade_transition_case() {
    upgrade_case_name=$1
    installed_version_before_syu=$2
    installed_version_after_syu=$3
    source_preference_state=$4
    source_git_url=$5

    setup_case "$upgrade_case_name"

    upgrade_package=clean-root
    installed_version_state=$case_dir/installed-version-state
    installed_version_before=$case_dir/installed-version-before
    installed_version_after=$case_dir/installed-version-after-syu
    initial_srcinfo=$case_dir/initial.SRCINFO
    remote_srcinfo=$case_dir/remote.SRCINFO
    checkout_dir=$cache_root/$upgrade_package
    vercmp_argv_log=$case_dir/vercmp-argv.log
    makepkg_argv_log=$case_dir/makepkg-argv.log

    : > "$installed_version_before"
    if [ "$installed_version_before_syu" != not-installed ]; then
        printf '%s %s\n' "$upgrade_package" "$installed_version_before_syu" > "$installed_version_before"
    fi
    : > "$installed_version_after"
    if [ "$installed_version_after_syu" != not-installed ]; then
        printf '%s %s\n' "$upgrade_package" "$installed_version_after_syu" > "$installed_version_after"
    fi
    cp "$installed_version_before" "$installed_version_state"

    write_upgrade_srcinfo "$initial_srcinfo" 1.0 1
    write_upgrade_srcinfo "$remote_srcinfo" 2.0 1
    mkdir -p "$checkout_dir/.git"
    cp "$initial_srcinfo" "$checkout_dir/.SRCINFO"
    printf 'pkgname=%s\npkgver=1.0\npkgrel=1\n' "$upgrade_package" > "$checkout_dir/PKGBUILD"
    printf '%s\n' "$source_git_url" > "$checkout_dir/.git/.moguet-test-remote-url"

    if [ "$source_preference_state" = enabled ]; then
        : > "$preference_dir/$upgrade_package"
    fi

    : > "$vercmp_argv_log"
    : > "$makepkg_argv_log"
    export MOGUET_TEST_PACMAN_Q_OUTPUT_FILE=$installed_version_state
    export MOGUET_TEST_PACKAGE_METADATA_STATE_FILE=$installed_version_state
    export MOGUET_TEST_SOURCE_MAINTENANCE_PACMAN_SYU_Q_OUTPUT_FILE=$installed_version_after
    export MOGUET_TEST_GIT_RESET_SRCINFO_FIXTURE=$remote_srcinfo
    export MOGUET_TEST_VERCMP_ARGV_LOG=$vercmp_argv_log
    export MOGUET_TEST_MAKEPKG_ARGV_LOG=$makepkg_argv_log
}

# P0-1: build handler parsing, validation, catch boundary, and source request mapping.
setup_case build-missing-argument
run_fail build
assert_contains \
    "Usage: moguet build <pkg> [V=K...] | build --local <directory> [V=K...]" \
    "$output_file"
assert_total_command_count 0

setup_case build-environment-without-package
run_fail build FIRST=one SECOND=two
assert_contains \
    "Invalid: Environment assignment requires a preceding package: FIRST=one" \
    "$output_file"
assert_total_command_count 0

setup_case build-invalid-environment
run_fail build 1INVALID=value clean-root
assert_contains "Invalid environment assignment: 1INVALID=value" "$output_file"
assert_not_contains "Build Error:" "$output_file"
assert_total_command_count 0

setup_case build-environment-order
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
run_fail --noedit --nodiff --noconfirm build \
    clean-root FIRST=one "SECOND=two words" FIRST=last EMPTY= \
    PKGDEST=first-path PKGDEST=
assert_contains "Source environment PKGDEST conflicts with the invocation-owned artifact workspace." "$output_file"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "pacman-conf --repo-list"
assert_command "alpm sync-query core/clean-root"
assert_command_absent "pacman -Si clean-root"
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"

setup_case build-extra-bare-operand-rejected
run_fail build clean-root ignored
assert_contains \
    "Invalid: Operation build requires exactly one <pkg> operand." \
    "$output_file"
assert_total_command_count 0

setup_case build-inherited-pkgdest
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
export PKGDEST=
run_fail --noedit --nodiff --noconfirm build clean-root
assert_contains "Inherited PKGDEST conflicts with the invocation-owned artifact workspace." "$output_file"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "pacman-conf --repo-list"
assert_command "alpm sync-query core/clean-root"
assert_command_absent "pacman -Si clean-root"
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"

setup_case build-rmdeps-rejected
run_fail --rmdeps --noedit --nodiff --noconfirm build clean-root
assert_contains "Separated build/install does not support --rmdeps." "$output_file"
assert_total_command_count 0
assert_request_log_empty

setup_case build-resolve-failure
run_fail build missing-source-package
assert_contains "Build Error: Package not found in repos or AUR: missing-source-package" "$output_file"
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"

setup_case build-constraint-preflight-firewall
export MOGUET_TEST_ALPM_VERCMP_EXPECTED_LHS=1.0-1
export MOGUET_TEST_ALPM_VERCMP_EXPECTED_RHS=2.0-1
export MOGUET_TEST_ALPM_VERCMP_RESULT=-1
run_fail --noedit --nodiff --noconfirm build constraint-block-root
assert_contains "dependency constraint-block-leaf>=2.0-1 is Unsatisfied" "$output_file"
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"
assert_command_content_absent "sudo "

setup_case build-split-child-selected-only
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='split-base|split-sibling|3.1-4
split-base|split-child|3.1-4
split-base|split-child-debug|3.1-4'
run_ok --noedit --nodiff build split-child
assert_command "git clone https://aur.archlinux.org/split-base.git split-base"
assert_command_pattern_count \
    '^sudo pacman -U -- .*/split-child-3\.1-4-x86_64\.pkg\.tar\.zst$' 1
assert_command_pattern_absent '^sudo pacman -U .*split-sibling'
assert_command_pattern_absent '^sudo pacman -U .*split-child-debug'
assert_contains "PackageBase result: split-base" "$output_file"
assert_contains "  required child: split-child -> split-child 3.1-4 (explicit): installed" "$output_file"
assert_output_before \
    "  produced artifact: split-sibling 3.1-4 (not selected; not installed)" \
    "  produced artifact: split-child-debug 3.1-4 (not selected; not installed)" \
    "$output_file"

setup_case build-direct-split-priority
run_fail build split-metadata-ambiguous-root
assert_contains "ambiguous providers" "$output_file"
assert_not_contains "conflicts/replaces metadata" "$output_file"

setup_case sync-plan-no-match-reaches-split
run_fail -S --aur split-metadata-root
assert_contains \
    "PackageBase artifact selection failed before package transaction." \
    "$output_file"
assert_not_contains \
    "package relation assessment requires manual review" "$output_file"

setup_case sync-plan-provider-before-metadata-split
run_fail -S --aur split-metadata-ambiguous-root
assert_contains "ambiguous providers" "$output_file"
assert_not_contains \
    "package relation assessment requires manual review" "$output_file"

setup_case build-execution-failure
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
export MOGUET_TEST_MAKEPKG_EXIT_CODE=42
export MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE=0
run_fail --noedit --nodiff build clean-root
assert_contains "Build Error: The build-only makepkg command failed with exit code 42." "$output_file"
assert_command "makepkg --packagelist"
assert_command "makepkg -sc"
assert_command_content_absent "sudo pacman -U"

setup_case build-install-failure-keeps-staged-outcome
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
export MOGUET_TEST_SOURCE_MAINTENANCE_FAIL_SUBSTRING='pacman -U'
run_fail --noedit --nodiff --noconfirm build clean-root
assert_contains \
    "Build outcome for PackageBase clean-root: succeeded." \
    "$output_file"
assert_contains \
    "Install outcome for PackageBase clean-root: failed." \
    "$output_file"
assert_contains "Install Error: pacman -U failed with exit code 1." "$output_file"
assert_not_contains "Build Error: pacman -U" "$output_file"
assert_command_prefix_count "sudo pacman -U --noconfirm -- " 1

setup_case build-cleanup-partial-success-keeps-cli-contract
installed_after_success=$case_dir/installed-after-success
install_success_log=$XDG_CACHE_HOME/pacman-u-success.log
printf 'clean-root 1.0-1\n' > "$installed_after_success"
: > "$install_success_log"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
export MOGUET_TEST_MAKEPKG_PACKAGE_METADATA_STATE_AFTER_SUCCESS_FILE=$installed_after_success
export MOGUET_TEST_PACMAN_U_SUCCESS_LOG=$install_success_log
export MOGUET_TEST_REPLACE_WORKSPACE_AFTER_PACMAN_U=1
run_fail --noedit --nodiff --noconfirm build clean-root
assert_contains "Package installation succeeded, but artifact workspace cleanup failed:" "$output_file"
assert_not_contains "Build Error:" "$output_file"
assert_not_contains "Failed while building/installing PackageBase" "$output_file"
assert_not_contains "pacman -U failed" "$output_file"
assert_command_prefix_count "sudo pacman -U --noconfirm -- " 1
assert_file_equals "$installed_after_success" "$package_metadata_state"
assert_cleanup_partial_success_fixture "$install_success_log"

echo "  ok: P0-1 cmd_build"

# #271 Slice 5: formal local route builds an invocation-owned snapshot and
# installs every declared local child explicitly without touching the source.
setup_case build-local-split-success
local_root=$case_dir/local-source
prepare_local_source_root \
    "$local_root" local-suite 'local-cli local-libs'
cp "$local_root/PKGBUILD" "$case_dir/expected-PKGBUILD"
cp "$local_root/.SRCINFO" "$case_dir/expected-SRCINFO"
export MOGUET_TEST_MAKEPKG_LOCAL_PACKAGE_BASE=local-suite
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='local-suite|local-cli|1.0-1
local-suite|local-libs|1.0-1
local-suite|local-debug|1.0-1'
run_ok --noedit --noconfirm build --local "$local_root"
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg --printsrcinfo"
assert_command_count "makepkg --packagelist" 1
assert_command_count "makepkg -sc --noconfirm" 1
assert_command_pattern_count \
    '^sudo pacman -U --noconfirm -- .*/local-cli-1\.0-1-x86_64\.pkg\.tar\.zst .*/local-libs-1\.0-1-x86_64\.pkg\.tar\.zst$' 1
assert_command_pattern_absent '^sudo pacman -U .*local-debug'
assert_contains "Local PackageBase result: local-suite" "$output_file"
assert_contains "  required child: local-cli 1.0-1 (explicit): installed" "$output_file"
assert_contains "  required child: local-libs 1.0-1 (explicit): installed" "$output_file"
assert_contains \
    "  produced artifact: local-debug 1.0-1 (not selected; not installed)" \
    "$output_file"
assert_file_equals "$case_dir/expected-PKGBUILD" "$local_root/PKGBUILD"
assert_file_equals "$case_dir/expected-SRCINFO" "$local_root/.SRCINFO"
assert_path_absent "$local_root/src"
assert_path_absent "$local_root/pkg"
assert_request_log_empty

# A failed local makepkg invocation retains only its invocation-owned artifact
# workspace for diagnosis and keeps internal phase tokens out of the CLI.
setup_case build-local-execution-failure-retains-artifact-workspace
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-failure local-failure
source_tree_before=$case_dir/source-tree-before.snapshot
snapshot_source_tree "$local_root" "$source_tree_before"
export MOGUET_TEST_MAKEPKG_LOCAL_PACKAGE_BASE=local-failure
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='local-failure|local-failure|1.0-1'
export MOGUET_TEST_MAKEPKG_EXIT_CODE=42
export MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE=0
run_fail --noedit --noconfirm build --local "$local_root"
assert_contains \
    "The build-only makepkg command failed with exit code 42." \
    "$output_file"
assert_contains "Retained artifact workspace:" "$output_file"
assert_not_contains \
    "local-source-build-command-returned-nonzero" "$output_file"
assert_command_count "makepkg --packagelist" 1
assert_command_count "makepkg -sc --noconfirm" 1
assert_command_content_absent "sudo pacman"
retained_workspace=$(sed -n \
    's|^.*Retained artifact workspace: \(.*\)$|\1|p' "$output_file")
if [ -z "$retained_workspace" ] || [ ! -d "$retained_workspace" ] ||
   [ -L "$retained_workspace" ]; then
    echo "displayed local artifact workspace was not retained" >&2
    sed -n '1,260p' "$output_file" >&2
    exit 1
fi
assert_source_tree_unchanged "$local_root" "$source_tree_before"

# State/cache authority must be rejected by retained identity before a
# managed directory or the fixed state log can mutate the selected source.
setup_case build-local-state-under-source-missing-rejected
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-state local-state
export XDG_STATE_HOME=$local_root
run_fail --noedit --noconfirm build --local "$local_root"
assert_contains \
    "Moguet state and cache directories must be outside the local source tree." \
    "$output_file"
assert_path_absent "$local_root/moguet"
assert_total_command_count 0
assert_request_log_empty

setup_case build-local-state-under-source-existing-rejected-before-log
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-state local-state
mkdir -m 700 "$local_root/moguet"
export XDG_STATE_HOME=$local_root
run_fail --noedit --noconfirm build --local "$local_root"
assert_contains \
    "Moguet state and cache directories must be outside the local source tree." \
    "$output_file"
assert_path_absent "$local_root/moguet/moguet.log"
assert_total_command_count 0
assert_request_log_empty

setup_case build-local-cache-under-source-missing-rejected
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-cache local-cache
export XDG_CACHE_HOME=$local_root
run_fail --noedit --noconfirm build --local "$local_root"
assert_contains \
    "Moguet state and cache directories must be outside the local source tree." \
    "$output_file"
assert_path_absent "$local_root/moguet"
assert_command_content_absent "makepkg"
assert_command_content_absent "sudo pacman"
assert_request_log_empty

setup_case build-local-cache-under-source-existing-rejected
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-cache local-cache
mkdir -m 700 "$local_root/moguet"
export XDG_CACHE_HOME=$local_root
run_fail --noedit --noconfirm build --local "$local_root"
assert_contains \
    "Moguet state and cache directories must be outside the local source tree." \
    "$output_file"
local_cache_entries_raw=$case_dir/local-cache-entries.raw
if validation_capture_output "$local_cache_entries_raw" \
    find "$local_root/moguet" -mindepth 1 -print -quit; then
    :
else
    local_cache_status=$?
    echo "local cache producer failed with status $local_cache_status" >&2
    exit 1
fi
if [ -s "$local_cache_entries_raw" ]; then
    echo "existing source-local cache directory was mutated" >&2
    find "$local_root/moguet" -mindepth 1 -maxdepth 1 -print >&2
    exit 1
fi
assert_command_content_absent "makepkg"
assert_command_content_absent "sudo pacman"
assert_request_log_empty

# The local install adapter owns transaction failure retention and successful
# transaction/failed cleanup presentation at the production CLI boundary.
setup_case build-local-install-failure-retains-artifacts
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-failure local-failure
source_tree_before=$case_dir/source-tree-before.snapshot
snapshot_source_tree "$local_root" "$source_tree_before"
export MOGUET_TEST_MAKEPKG_LOCAL_PACKAGE_BASE=local-failure
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='local-failure|local-failure|1.0-1'
export MOGUET_TEST_SOURCE_MAINTENANCE_FAIL_SUBSTRING='pacman -U'
run_fail --noedit --noconfirm build --local "$local_root"
assert_contains "Build Error: pacman -U failed with exit code 1." "$output_file"
assert_not_contains "Local PackageBase result:" "$output_file"
assert_command_prefix_count "sudo pacman -U --noconfirm -- " 1
failed_artifact=$(sed -n \
    's|^sudo pacman -U --noconfirm -- \([^ ]*\)$|\1|p' "$command_log")
failed_workspace=${failed_artifact%/*}
if [ -z "$failed_artifact" ] || [ ! -f "$failed_artifact" ] ||
   [ "${failed_workspace%/*}" != "$cache_root" ]; then
    echo "failed local install did not retain its artifact workspace" >&2
    cat "$command_log" >&2
    exit 1
fi
case ${failed_workspace##*/} in
    .artifact-workspace~-??????) ;;
    *)
        echo "failed local install retained an unexpected workspace: $failed_workspace" >&2
        exit 1
        ;;
esac
assert_source_tree_unchanged "$local_root" "$source_tree_before"

setup_case build-local-cleanup-partial-success
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-cleanup local-cleanup
source_tree_before=$case_dir/source-tree-before.snapshot
snapshot_source_tree "$local_root" "$source_tree_before"
install_success_log=$XDG_CACHE_HOME/pacman-u-success.log
: > "$install_success_log"
export MOGUET_TEST_MAKEPKG_LOCAL_PACKAGE_BASE=local-cleanup
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='local-cleanup|local-cleanup|1.0-1'
export MOGUET_TEST_PACMAN_U_SUCCESS_LOG=$install_success_log
export MOGUET_TEST_REPLACE_WORKSPACE_AFTER_PACMAN_U=1
run_fail --noedit --noconfirm build --local "$local_root"
assert_contains \
    "Package installation succeeded, but artifact workspace cleanup failed:" \
    "$output_file"
assert_not_contains "Build Error:" "$output_file"
assert_not_contains "pacman -U failed" "$output_file"
assert_output_line_count "Local PackageBase result: local-cleanup" 1 "$output_file"
assert_output_line_count \
    "  required child: local-cleanup 1.0-1 (explicit): installed" 1 \
    "$output_file"
assert_command_prefix_count "sudo pacman -U --noconfirm -- " 1
assert_cleanup_partial_success_fixture "$install_success_log"
assert_source_tree_unchanged "$local_root" "$source_tree_before"

# A fatal reviewed-state observation for a remote AUR dependency must happen
# after the read-only local plan, but before the first persistent cache
# mutation owned by the top-level local route.
setup_case build-local-aur-dependency-fatal-before-cache
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-app local-app clean-root
XDG_STATE_HOME=$case_dir/xdg-state
export XDG_STATE_HOME
reviewed_state_package_dir=$XDG_STATE_HOME/moguet/reviewed-sources/aur/clean-root
mkdir -p "$reviewed_state_package_dir"
chmod 700 \
    "$XDG_STATE_HOME" \
    "$XDG_STATE_HOME/moguet" \
    "$XDG_STATE_HOME/moguet/reviewed-sources" \
    "$XDG_STATE_HOME/moguet/reviewed-sources/aur" \
    "$reviewed_state_package_dir"
reviewed_state_file=$reviewed_state_package_dir/1.toml
printf '%s\n' 'schema_version = 2' > "$reviewed_state_file"
chmod 600 "$reviewed_state_file"
reviewed_state_before=$case_dir/reviewed-state-before
cp "$reviewed_state_file" "$reviewed_state_before"
source_tree_before=$case_dir/source-tree-before.snapshot
snapshot_source_tree "$local_root" "$source_tree_before"
assert_path_absent "$cache_root"

run_fail --noedit --noconfirm build --local "$local_root"

assert_contains \
    "Reviewed source state uses an unsupported future schema; the build was not started." \
    "$output_file"
assert_path_absent "$cache_root"
assert_file_equals "$reviewed_state_before" "$reviewed_state_file"
assert_source_tree_unchanged "$local_root" "$source_tree_before"
reviewed_state_file_count=$(find "$reviewed_state_package_dir" \
    -mindepth 1 -maxdepth 1 -type f | wc -l)
if [ "$reviewed_state_file_count" -ne 1 ]; then
    echo "fatal local dependency preflight changed reviewed state" >&2
    find "$reviewed_state_package_dir" -mindepth 1 -maxdepth 1 -print >&2
    exit 1
fi
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"
assert_command_content_absent "sudo "
assert_command_content_absent "moguet-test-editor"
assert_contains "clean-root" "$request_log"

# A non-fatal remote AUR dependency continues through cache activation,
# dependency installation, and then the local root build.
setup_case build-local-aur-dependency-before-root
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-app local-app clean-root
export MOGUET_TEST_MAKEPKG_LOCAL_PACKAGE_BASE=local-app
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='clean-root|clean-root|1.0-1
local-app|local-app|1.0-1'
run_ok --noedit --noconfirm build --local "$local_root"
if [ ! -d "$cache_root" ] || [ -L "$cache_root" ]; then
    echo "non-fatal local dependency route did not activate its cache root" >&2
    exit 1
fi
assert_command "git clone https://aur.archlinux.org/clean-root.git clean-root"
assert_command_pattern_count \
    '^sudo pacman -U --noconfirm --asdeps -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$' 1
assert_command_pattern_count \
    '^sudo pacman -U --noconfirm -- .*/local-app-1\.0-1-x86_64\.pkg\.tar\.zst$' 1
assert_command_pattern_before \
    '^sudo pacman -U .*clean-root-1\.0-1-' \
    '^sudo pacman -U .*local-app-1\.0-1-'
assert_not_contains "local-app" "$request_log"
assert_contains "clean-root" "$request_log"

# A successful remote dependency transaction is not rolled back, but any
# subsequent local source mutation must stop before local build/install.
setup_case build-local-source-change-after-aur-dependency
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-app local-app clean-root
export MOGUET_TEST_MAKEPKG_LOCAL_PACKAGE_BASE=local-app
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='clean-root|clean-root|1.0-1
local-app|local-app|1.0-1'
export MOGUET_TEST_APPEND_LOCAL_PKGBUILD_AFTER_ASDEPS=$local_root/PKGBUILD
run_fail --noedit --noconfirm build --local "$local_root"
assert_contains \
    "Local source entry content changed during the operation: $local_root/PKGBUILD" \
    "$output_file"
assert_not_contains "local-source-workspace-failed" "$output_file"
assert_not_contains "local source root failure" "$output_file"
assert_command_pattern_count \
    '^sudo pacman -U --noconfirm --asdeps -- .*/clean-root-1\.0-1-x86_64\.pkg\.tar\.zst$' 1
assert_command_pattern_absent \
    '^sudo pacman -U --noconfirm -- .*/local-app-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_command_count "makepkg --packagelist" 1
assert_command_count "makepkg -sc --noconfirm" 1

# Missing metadata requires a no-default consent. --noconfirm must stop before
# makepkg evaluation, while an explicit TTY yes binds the exact environment to
# evaluation, packagelist, and build.
setup_case build-local-metadata-noconfirm-rejected
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-eval local-eval
rm "$local_root/.SRCINFO"
run_fail --noedit --noconfirm build --local "$local_root"
assert_contains "Unavailable:" "$output_file"
assert_contains "--noconfirm" "$output_file"
assert_not_contains "Build Error:" "$output_file"
assert_command_content_absent "makepkg"
assert_command_content_absent "sudo pacman"
assert_path_absent "$local_root/.SRCINFO"

setup_case build-local-metadata-noninteractive-rejected
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-eval local-eval
rm "$local_root/.SRCINFO"
source_tree_before=$case_dir/source-tree-before.snapshot
snapshot_source_tree "$local_root" "$source_tree_before"
run_fail_nonblocking --noedit build --local "$local_root" < /dev/null
assert_contains \
    "Unavailable: The required confirmation is unavailable because standard input is non-interactive." \
    "$output_file"
assert_not_contains "Build Error:" "$output_file"
assert_command_content_absent "makepkg"
assert_command_content_absent "sudo pacman"
assert_path_absent "$local_root/.SRCINFO"
assert_source_tree_unchanged "$local_root" "$source_tree_before"

for metadata_decline in n no; do
    setup_case "build-local-metadata-tty-$metadata_decline-declined"
    local_root=$case_dir/local-source
    prepare_local_source_root "$local_root" local-eval local-eval
    rm "$local_root/.SRCINFO"
    source_tree_before=$case_dir/source-tree-before.snapshot
    snapshot_source_tree "$local_root" "$source_tree_before"
    run_build_local_tty_fail "$metadata_decline\n"
    assert_contains "Declined:" "$output_file"
    assert_not_contains "Build Error:" "$output_file"
    assert_command_content_absent "makepkg"
    assert_command_content_absent "sudo pacman"
    assert_path_absent "$local_root/.SRCINFO"
    assert_source_tree_unchanged "$local_root" "$source_tree_before"
done

for metadata_cancel in q quit cancel; do
    setup_case "build-local-metadata-tty-$metadata_cancel-cancelled"
    local_root=$case_dir/local-source
    prepare_local_source_root "$local_root" local-eval local-eval
    rm "$local_root/.SRCINFO"
    source_tree_before=$case_dir/source-tree-before.snapshot
    snapshot_source_tree "$local_root" "$source_tree_before"
    run_build_local_tty_fail "$metadata_cancel\n"
    assert_contains "Cancelled:" "$output_file"
    assert_not_contains "Build Error:" "$output_file"
    assert_command_content_absent "makepkg"
    assert_command_content_absent "sudo pacman"
    assert_path_absent "$local_root/.SRCINFO"
    assert_source_tree_unchanged "$local_root" "$source_tree_before"
done

for metadata_retry in empty invalid; do
    setup_case "build-local-metadata-tty-$metadata_retry-then-n"
    local_root=$case_dir/local-source
    prepare_local_source_root "$local_root" local-eval local-eval
    rm "$local_root/.SRCINFO"
    source_tree_before=$case_dir/source-tree-before.snapshot
    snapshot_source_tree "$local_root" "$source_tree_before"
    case $metadata_retry in
        empty) metadata_input='\nn\n' ;;
        invalid) metadata_input='later\nn\n' ;;
    esac
    run_build_local_tty_fail "$metadata_input"
    assert_contains "Please answer yes, no, or cancel." "$output_file"
    assert_contains "Declined:" "$output_file"
    assert_not_contains "Build Error:" "$output_file"
    assert_command_content_absent "makepkg"
    assert_command_content_absent "sudo pacman"
    assert_path_absent "$local_root/.SRCINFO"
    assert_source_tree_unchanged "$local_root" "$source_tree_before"
done

setup_case build-local-metadata-tty-eof-cancelled
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-eval local-eval
rm "$local_root/.SRCINFO"
source_tree_before=$case_dir/source-tree-before.snapshot
snapshot_source_tree "$local_root" "$source_tree_before"
run_build_local_tty_fail ''
assert_contains "Cancelled:" "$output_file"
assert_contains "interactive input ended" "$output_file"
assert_not_contains "Build Error:" "$output_file"
assert_command_content_absent "makepkg"
assert_command_content_absent "sudo pacman"
assert_path_absent "$local_root/.SRCINFO"
assert_source_tree_unchanged "$local_root" "$source_tree_before"

for metadata_accept in y yes; do
setup_case "build-local-metadata-explicit-consent-$metadata_accept"
local_root=$case_dir/local-source
prepare_local_source_root "$local_root" local-eval local-eval
metadata_output=$case_dir/evaluated.SRCINFO
cp "$local_root/.SRCINFO" "$metadata_output"
rm "$local_root/.SRCINFO"
makepkg_env_log=$case_dir/makepkg-env.log
makepkg_cwd_log=$case_dir/makepkg-cwd.log
makepkg_argv_log=$case_dir/makepkg-argv.log
: > "$makepkg_env_log"
: > "$makepkg_cwd_log"
: > "$makepkg_argv_log"
export MOGUET_TEST_MAKEPKG_LOCAL_PACKAGE_BASE=local-eval
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='local-eval|local-eval|1.0-1'
export MOGUET_TEST_MAKEPKG_SRCINFO_OUTPUT_FILE=$metadata_output
export MOGUET_TEST_MAKEPKG_ENV_LOG=$makepkg_env_log
export MOGUET_TEST_MAKEPKG_ENV_KEYS='FIRST EMPTY'
export MOGUET_TEST_MAKEPKG_CWD_LOG=$makepkg_cwd_log
export MOGUET_TEST_MAKEPKG_ARGV_LOG=$makepkg_argv_log
if ! printf '%s\n' "$metadata_accept" |
    script -qec \
        "$test_binary --noedit build --local $local_root FIRST=one EMPTY= FIRST=last" \
        /dev/null > "$output_file" 2>&1; then
    echo "expected explicit local metadata evaluation consent to succeed" >&2
    sed -n '1,260p' "$output_file" >&2
    cat "$command_log" >&2
    exit 1
fi
assert_command_count "makepkg --printsrcinfo" 1
assert_command_count "makepkg --packagelist" 1
assert_command_count "makepkg -sc" 1
assert_contains "FIRST='one'" "$output_file"
assert_contains "FIRST='last'" "$output_file"
assert_contains "EMPTY=''" "$output_file"
assert_output_line_count "env[FIRST]=<last>" 3 "$makepkg_env_log"
assert_output_line_count "env[EMPTY]=<>" 3 "$makepkg_env_log"
assert_makepkg_argv_log "$makepkg_argv_log" 'argv-begin
arg[0]=<--printsrcinfo>
arg[1]=<FIRST=one>
arg[2]=<EMPTY=>
arg[3]=<FIRST=last>
argv-end
argv-begin
arg[0]=<--packagelist>
arg[1]=<FIRST=one>
arg[2]=<EMPTY=>
arg[3]=<FIRST=last>
arg[4]=<PKGDEST=<owned>>
argv-end
argv-begin
arg[0]=<-sc>
arg[1]=<FIRST=one>
arg[2]=<EMPTY=>
arg[3]=<FIRST=last>
arg[4]=<PKGDEST=<owned>>
argv-end'
assert_path_absent "$local_root/.SRCINFO"
first_makepkg_cwd=$(sed -n '1p' "$makepkg_cwd_log")
if [ "$first_makepkg_cwd" != "$local_root" ]; then
    echo "metadata evaluation did not run in the retained local root" >&2
    cat "$makepkg_cwd_log" >&2
    exit 1
fi
if sed -n '2,$p' "$makepkg_cwd_log" | grep -Fx -- "$local_root" >/dev/null; then
    echo "local packagelist/build unexpectedly ran in the user source root" >&2
    cat "$makepkg_cwd_log" >&2
    exit 1
fi
done

echo "  ok: #271 Slice 5 local build/install route"

# dot path componentは、state logやsource preferenceの作成前にpackage validationで拒否する。
for dot_target in . ..; do
    case $dot_target in
        .) dot_target_label=dot ;;
        ..) dot_target_label=dot-dot ;;
    esac
    for operation in add-src edit-src del-src revert; do
        setup_case "$operation-reject-$dot_target_label"
        prepare_fresh_pre_log_storage_case

        run_fail "$operation" "$dot_target"

        assert_contains "Invalid package name: $dot_target" "$output_file"
        assert_total_command_count 0
        assert_request_log_empty
        assert_pre_log_storage_absent
    done
done

echo "  ok: dot package names stop before state and source preference creation"

while IFS='|' read -r invalid_label invalid_target use_marker; do
    for operation in add-src edit-src del-src revert; do
        setup_case "$operation-reject-$invalid_label"
        prepare_fresh_pre_log_storage_case

        if [ "$use_marker" = yes ]; then
            run_fail "$operation" -- "$invalid_target"
        else
            run_fail "$operation" "$invalid_target"
        fi

        assert_contains "Invalid package name: $invalid_target" "$output_file"
        assert_total_command_count 0
        assert_request_log_empty
        assert_pre_log_storage_absent
    done
done <<'INVALID_PACKAGE_TARGETS'
slash|invalid/name|no
absolute|/absolute-name|no
leading-hyphen|-leading-name|yes
INVALID_PACKAGE_TARGETS

echo "  ok: unsafe package paths stop before state, source preference, editor, and sudo boundaries"

# P0-2: add-src is a native streaming mutation with cumulative assignment scope.
setup_case add-src-streaming
rmdir "$preference_dir"
run_ok add-src alpha beta FIRST=one gamma SECOND=two
assert_total_command_count 0
printf 'FIRST=one\nSECOND=two\n' > "$case_dir/alpha.expected"
printf 'FIRST=one\nSECOND=two\n' > "$case_dir/beta.expected"
printf 'SECOND=two\n' > "$case_dir/gamma.expected"
assert_file_equals "$case_dir/alpha.expected" "$preference_dir/alpha"
assert_file_equals "$case_dir/beta.expected" "$preference_dir/beta"
assert_file_equals "$case_dir/gamma.expected" "$preference_dir/gamma"
entry_count=$(count_command_output_lines add-src-preference-entries \
    find "$preference_dir" -maxdepth 1 -type f)
if [ "$entry_count" -ne 3 ]; then
    echo "add-src did not preserve one preference file per package" >&2
    exit 1
fi
if [ "$(stat -c '%a' "$preference_dir")" != "700" ]; then
    echo "add-src did not create the canonical directory with mode 0700" >&2
    exit 1
fi
for package in alpha beta gamma; do
    if [ "$(stat -c '%a' "$preference_dir/$package")" != "600" ]; then
        echo "add-src did not create $package with mode 0600" >&2
        exit 1
    fi
done

setup_case add-src-leading-assignment
run_fail add-src FIRST=one alpha SECOND=two
assert_contains "Environment assignment requires a preceding package: FIRST=one" "$output_file"
assert_total_command_count 0
if [ -e "$preference_dir/alpha" ]; then
    echo "add-src mutated preferences after ordered grammar rejection" >&2
    exit 1
fi

setup_case add-src-package-failure
printf 'UNSAFE=yes\n' > "$preference_dir/failed"
chmod 644 "$preference_dir/failed"
run_fail add-src alpha failed beta FLAGS=value
assert_contains "Source preference entry permissions are not private mode 0600: $preference_dir/failed" "$output_file"
assert_total_command_count 0
printf 'FLAGS=value\n' > "$case_dir/alpha.expected"
assert_file_equals "$case_dir/alpha.expected" "$preference_dir/alpha"
assert_file_equals "$case_dir/alpha.expected" "$preference_dir/beta"

echo "  ok: P0-2 cmd_add_src"

# P0-3: edit-src pins the user-opened source fd and publishes through native IO.
setup_case edit-src-editor-arguments
printf 'CFLAGS=-Oexisting\n' > "$preference_dir/alpha"
printf 'CFLAGS=-Oexisting\n' > "$case_dir/existing.expected"
export EDITOR='moguet-test-editor --environment'
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_SNAPSHOT_FILE=$case_dir/editor.snapshot
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_APPEND_LINE='LDFLAGS=-Wl,--as-needed'
run_ok edit-src alpha
editor_command=$(sed -n '1p' "$command_log")
case $editor_command in
    "moguet-test-editor --environment /tmp/moguet-edit-src-alpha."??????)
        ;;
    *)
        echo "unexpected editor command: $editor_command" >&2
        cat "$command_log" >&2
        exit 1
        ;;
esac
edit_temp_path=${editor_command##* }
assert_total_command_count 1
assert_file_equals "$case_dir/existing.expected" "$case_dir/editor.snapshot"
printf 'CFLAGS=-Oexisting\nLDFLAGS=-Wl,--as-needed\n' > "$case_dir/edited.expected"
assert_file_equals "$case_dir/edited.expected" "$preference_dir/alpha"
if [ "$(stat -c '%a' "$preference_dir/alpha")" != "600" ]; then
    echo "edit-src destination mode is not 0600" >&2
    exit 1
fi
assert_path_absent "$edit_temp_path"

setup_case edit-src-create-missing-preference
rmdir "$preference_dir"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_APPEND_LINE='CREATED=yes'
run_ok edit-src alpha
editor_command=$(sed -n '1p' "$command_log")
edit_temp_path=${editor_command##* }
printf 'CREATED=yes\n' > "$case_dir/created.expected"
assert_file_equals "$case_dir/created.expected" "$preference_dir/alpha"
assert_total_command_count 1
if [ "$(stat -c '%a' "$preference_dir")" != "700" ] ||
   [ "$(stat -c '%a' "$preference_dir/alpha")" != "600" ]; then
    echo "edit-src did not use the private creation boundary" >&2
    exit 1
fi
assert_path_absent "$edit_temp_path"

setup_case edit-src-empty-content
: > "$case_dir/replacement-content"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND=regular
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_SOURCE=$case_dir/replacement-content
run_ok edit-src alpha
editor_command=$(sed -n '1p' "$command_log")
edit_temp_path=${editor_command##* }
assert_file_equals "$case_dir/replacement-content" "$preference_dir/alpha"
assert_path_absent "$edit_temp_path"

setup_case edit-src-no-final-newline
printf 'NO_FINAL_NEWLINE' > "$case_dir/replacement-content"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND=regular
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_SOURCE=$case_dir/replacement-content
run_ok edit-src alpha
assert_file_equals "$case_dir/replacement-content" "$preference_dir/alpha"

setup_case edit-src-binary-regular-replacement
printf 'BINARY\000PAYLOAD\n' > "$case_dir/replacement-content"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND=regular
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_SOURCE=$case_dir/replacement-content
run_ok edit-src alpha
assert_file_equals "$case_dir/replacement-content" "$preference_dir/alpha"

setup_case edit-src-editor-failure
printf 'CFLAGS=-Oexisting\n' > "$preference_dir/alpha"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_FAIL_SUBSTRING=moguet-edit-src-alpha.
run_fail edit-src alpha
editor_command=$(sed -n '1p' "$command_log")
edit_temp_path=${editor_command##* }
assert_contains "Editor failed for $preference_dir/alpha" "$output_file"
if [ ! -f "$edit_temp_path" ]; then
    echo "edit-src discarded the retained temporary file after editor failure" >&2
    exit 1
fi
assert_file_equals "$preference_dir/alpha" "$edit_temp_path"
assert_total_command_count 1
rm -f "$edit_temp_path"

setup_case edit-src-reject-symlink
printf 'ORIGINAL=yes\n' > "$preference_dir/alpha"
printf 'ORIGINAL=yes\n' > "$case_dir/original.expected"
printf 'UNREVIEWED=yes\n' > "$case_dir/symlink-target"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND=symlink
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_TARGET=$case_dir/symlink-target
run_fail edit-src alpha
editor_command=$(sed -n '1p' "$command_log")
edit_temp_path=${editor_command##* }
assert_contains "Failed to open edited temporary file $edit_temp_path" "$output_file"
assert_total_command_count 1
assert_file_equals "$case_dir/original.expected" "$preference_dir/alpha"
if [ ! -f "$case_dir/symlink-target" ] ||
   [ "$(cat "$case_dir/symlink-target")" != "UNREVIEWED=yes" ]; then
    echo "edit-src changed the external symlink target" >&2
    exit 1
fi
rm -f "$edit_temp_path"

setup_case edit-src-reject-directory
printf 'ORIGINAL=yes\n' > "$preference_dir/alpha"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND=directory
run_fail edit-src alpha
editor_command=$(sed -n '1p' "$command_log")
edit_temp_path=${editor_command##* }
assert_contains "Edited temporary file is not a regular file: $edit_temp_path" "$output_file"
assert_total_command_count 1
assert_contains "ORIGINAL=yes" "$preference_dir/alpha"
rm -rf "$edit_temp_path"

setup_case edit-src-reject-fifo-without-blocking
printf 'ORIGINAL=yes\n' > "$preference_dir/alpha"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND=fifo
run_fail_nonblocking edit-src alpha
editor_command=$(sed -n '1p' "$command_log")
edit_temp_path=${editor_command##* }
assert_contains "Edited temporary file is not a regular file: $edit_temp_path" "$output_file"
assert_total_command_count 1
assert_contains "ORIGINAL=yes" "$preference_dir/alpha"
rm -f "$edit_temp_path"

setup_case edit-src-reject-missing-source
printf 'ORIGINAL=yes\n' > "$preference_dir/alpha"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND=missing
run_fail edit-src alpha
editor_command=$(sed -n '1p' "$command_log")
edit_temp_path=${editor_command##* }
assert_contains "Failed to open edited temporary file $edit_temp_path" "$output_file"
assert_total_command_count 1
assert_path_absent "$edit_temp_path"

setup_case edit-src-concurrent-destination-replacement
printf 'CFLAGS=-Oexisting\n' > "$preference_dir/alpha"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_APPEND_LINE='EDITED=yes'
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_CONCURRENT_DESTINATION=$preference_dir/alpha
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_CONCURRENT_CONTENTS='CONCURRENT=yes'
run_fail edit-src alpha
editor_command=$(sed -n '1p' "$command_log")
edit_temp_path=${editor_command##* }
assert_total_command_count 1
assert_contains "Source preference entry changed before publication: $preference_dir/alpha" "$output_file"
assert_contains "edited file was kept at $edit_temp_path" "$output_file"
if [ ! -f "$edit_temp_path" ]; then
    echo "edit-src removed the retained file after install failure" >&2
    exit 1
fi
assert_contains "EDITED=yes" "$edit_temp_path"
printf 'CONCURRENT=yes\n' > "$case_dir/concurrent.expected"
assert_file_equals "$case_dir/concurrent.expected" "$preference_dir/alpha"
rm -f "$edit_temp_path"

setup_case edit-src-editor-failure-continues
printf 'FIRST=original\n' > "$preference_dir/first"
printf 'SECOND=original\n' > "$preference_dir/second"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_APPEND_LINE='EDITED=yes'
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_FAIL_SUBSTRING=moguet-edit-src-first.
run_fail edit-src first second
first_editor_command=$(sed -n '1p' "$command_log")
second_editor_command=$(sed -n '2p' "$command_log")
first_temp_path=${first_editor_command##* }
second_temp_path=${second_editor_command##* }
case $second_editor_command in
    "moguet-test-editor /tmp/moguet-edit-src-second."??????)
        ;;
    *)
        echo "edit-src did not continue after editor failure" >&2
        cat "$command_log" >&2
        exit 1
        ;;
esac
assert_total_command_count 2
if [ ! -f "$first_temp_path" ]; then
    echo "edit-src discarded the first retained temporary file" >&2
    exit 1
fi
assert_path_absent "$second_temp_path"
assert_contains "EDITED=yes" "$preference_dir/second"
rm -f "$first_temp_path"

setup_case edit-src-source-validation-continues
printf 'FIRST=original\n' > "$preference_dir/first"
printf 'SECOND=original\n' > "$preference_dir/second"
printf 'UNREVIEWED=yes\n' > "$case_dir/symlink-target"
export EDITOR=moguet-test-editor
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_APPEND_LINE='EDITED=yes'
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND=symlink
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_SUBSTRING=moguet-edit-src-first.
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_TARGET=$case_dir/symlink-target
run_fail edit-src first second
first_editor_command=$(sed -n '1p' "$command_log")
second_editor_command=$(sed -n '2p' "$command_log")
first_temp_path=${first_editor_command##* }
second_temp_path=${second_editor_command##* }
assert_contains "Failed to open edited temporary file $first_temp_path" "$output_file"
assert_total_command_count 2
assert_path_absent "$second_temp_path"
assert_contains "EDITED=yes" "$preference_dir/second"
rm -f "$first_temp_path"

setup_case edit-src-source-fd-zero
export EDITOR=moguet-test-editor
export MOGUET_TEST_RELEASE_STATE_LOG_BEFORE_DISPATCH=1
printf 'FD_ZERO\000PAYLOAD' > "$case_dir/replacement-content"
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_KIND=regular
export MOGUET_TEST_SOURCE_MAINTENANCE_EDITOR_REPLACEMENT_SOURCE=$case_dir/replacement-content
run_ok_stdin_closed edit-src alpha
assert_total_command_count 1
assert_file_equals "$case_dir/replacement-content" "$preference_dir/alpha"

echo "  ok: P0-3 cmd_edit_src"

# Existing list-src characterization remains owned by test-source-selection.

# P0-4: del-src and revert continue per target and preserve error priority.
setup_case del-src-missing-store-does-not-create
rmdir "$preference_dir"
run_ok del-src absent-source
assert_path_absent "$preference_dir"
assert_total_command_count 0

setup_case revert-missing-store-does-not-create
rmdir "$preference_dir"
run_ok revert absent-source
assert_path_absent "$preference_dir"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "pacman-conf --repo-list"
assert_command "alpm sync-query core/absent-source"
assert_command_content_absent "pacman -Si "

setup_case del-src-partial-failure
for package in alpha beta gamma; do
    printf 'SOURCE=yes\n' > "$preference_dir/$package"
done
chmod 644 "$preference_dir/beta"
run_fail del-src alpha beta gamma
assert_total_command_count 0
assert_path_absent "$preference_dir/alpha"
if [ ! -f "$preference_dir/beta" ]; then
    echo "failed del-src target was unexpectedly removed" >&2
    exit 1
fi
assert_path_absent "$preference_dir/gamma"
assert_contains "Source preference entry permissions are not private mode 0600: $preference_dir/beta" "$output_file"

setup_case revert-grouped-reinstall
for package in official-a remove-fail aur-a; do
    printf 'SOURCE=yes\n' > "$preference_dir/$package"
done
chmod 644 "$preference_dir/remove-fail"
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a official-b remove-fail'
run_fail --noconfirm revert official-a remove-fail aur-a official-b
assert_command_count "pacman-conf --verbose RootDir DBPath" 4
assert_command_count "pacman-conf --repo-list" 4
assert_command "alpm sync-query core/remove-fail"
assert_command_before "alpm sync-query core/official-b" "sudo pacman -S --noconfirm official-a official-b"
assert_command_content_absent "pacman -Si "
assert_command_count "sudo pacman -S --noconfirm official-a official-b" 1
assert_contains "official-b was not marked." "$output_file"
assert_contains "aur-a is likely an AUR package. Config removed only." "$output_file"
assert_contains "Failed to revert one or more packages." "$output_file"
assert_path_absent "$preference_dir/official-a"
assert_path_absent "$preference_dir/aur-a"
if [ ! -f "$preference_dir/remove-fail" ]; then
    echo "failed revert target was unexpectedly removed" >&2
    exit 1
fi

setup_case revert-reinstall-error-priority
printf 'SOURCE=yes\n' > "$preference_dir/delete-fail"
printf 'SOURCE=yes\n' > "$preference_dir/official-a"
chmod 644 "$preference_dir/delete-fail"
printf 'pacman -S official-a\n' > "$sudo_failures"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=official-a
run_fail revert delete-fail official-a
assert_contains "Failed to reinstall binaries." "$output_file"
assert_not_contains "Failed to revert one or more packages." "$output_file"

setup_case revert-delete-aggregate
printf 'SOURCE=yes\n' > "$preference_dir/delete-fail"
chmod 644 "$preference_dir/delete-fail"
run_fail revert delete-fail
assert_contains "Failed to revert one or more packages." "$output_file"
assert_command_content_absent "sudo pacman -S"

# Issue #512: metadata failure is resolved before this target's deletion.
for failure in config open cache query malformed; do
    setup_case "revert-metadata-$failure"
    printf 'CFLAGS=-O2\n' > "$preference_dir/official-a"
    expected_preference=$case_dir/expected-preference
    cp "$preference_dir/official-a" "$expected_preference"
    export MOGUET_TEST_PACMAN_REPO_PACKAGES=official-a
    case $failure in
        config) export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE=7 ;;
        open) export MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE=1 ;;
        cache) export MOGUET_TEST_SYNC_CACHE_FAILURE_REPOSITORY=core ;;
        query) export MOGUET_TEST_REPOSITORY_QUERY_FAILURE_PACKAGE=official-a ;;
        malformed) export MOGUET_TEST_REPOSITORY_PACKAGE_BASE='invalid/base' ;;
    esac
    run_fail --noconfirm revert official-a
    assert_file_equals "$expected_preference" "$preference_dir/official-a"
    assert_command_content_absent "sudo pacman"
    assert_not_contains "Unmarking source-build" "$output_file"
    assert_not_contains "Config removed only" "$output_file"
    assert_contains "Failed to inspect repository metadata for official-a:" "$output_file"
    assert_contains "Failed to revert one or more packages." "$output_file"
done

setup_case revert-metadata-mixed-targets
for package in official-a metadata-fail aur-a remove-fail official-b; do
    printf 'CFLAGS=-O2\n' > "$preference_dir/$package"
done
chmod 644 "$preference_dir/remove-fail"
cp "$preference_dir/metadata-fail" "$case_dir/expected-preference"
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a metadata-fail remove-fail official-b'
export MOGUET_TEST_REPOSITORY_QUERY_FAILURE_PACKAGE=metadata-fail
run_fail --noconfirm revert official-a metadata-fail aur-a remove-fail official-b
assert_file_equals "$case_dir/expected-preference" "$preference_dir/metadata-fail"
assert_file_equals "$case_dir/expected-preference" "$preference_dir/remove-fail"
assert_path_absent "$preference_dir/official-a"
assert_path_absent "$preference_dir/aur-a"
assert_path_absent "$preference_dir/official-b"
assert_command_count "sudo pacman -S --noconfirm official-a official-b" 1
assert_command_absent "sudo pacman -S --noconfirm official-a metadata-fail official-b"
assert_contains "Failed to inspect repository metadata for metadata-fail:" "$output_file"
assert_contains "Failed to revert one or more packages." "$output_file"

setup_case revert-metadata-reinstall-error-priority
printf 'CFLAGS=-O2\n' > "$preference_dir/metadata-fail"
cp "$preference_dir/metadata-fail" "$case_dir/expected-preference"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=official-a
export MOGUET_TEST_REPOSITORY_QUERY_FAILURE_PACKAGE=metadata-fail
printf 'pacman -S official-a\n' > "$sudo_failures"
run_fail revert metadata-fail official-a
assert_file_equals "$case_dir/expected-preference" "$preference_dir/metadata-fail"
assert_contains "Failed to reinstall binaries." "$output_file"
assert_not_contains "Failed to revert one or more packages." "$output_file"

setup_case revert-missing-store-metadata-failure
rmdir "$preference_dir"
export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE=7
run_fail revert absent-source
assert_path_absent "$preference_dir"
assert_command_content_absent "sudo pacman"

echo "  ok: P0-4 cmd_del_src/cmd_revert"

# P0-5: clean preflights every target, then preserves pacman/prompt/cleanup continuation.
setup_case clean-unsafe-preflight
mkdir -p "$cache_root/preflighted-safe" "$case_dir/outside"
ln -s "$case_dir/outside" "$cache_root/unsafe"
run_fail clean
assert_contains "Trusted Moguet cache cleanup preflight failed: symlink refused." "$output_file"
assert_total_command_count 0
if [ ! -d "$cache_root/preflighted-safe" ]; then
    echo "clean mutated a safe target before all cache targets passed preflight" >&2
    exit 1
fi
if [ ! -L "$cache_root/unsafe" ]; then
    echo "clean mutated the unsafe target after preflight failed" >&2
    exit 1
fi

setup_case clean-fd-exhaustion-before-pacman
mkdir -p "$cache_root"
fd_entry=0
while [ "$fd_entry" -lt 64 ]; do
    mkdir "$cache_root/entry-$fd_entry"
    fd_entry=$((fd_entry + 1))
done
run_clean_low_nofile_fail 24
assert_contains "Too many open files" "$output_file"
assert_not_contains "Cleaning package caches..." "$output_file"
assert_total_command_count 0
fd_cache_entry_count=$(count_command_output_lines fd-cache-entries \
    find "$cache_root" -mindepth 1 -maxdepth 1 -type d)
if [ "$fd_cache_entry_count" -ne 64 ]; then
    echo "low-RLIMIT cleanup preflight mutated cache entries" >&2
    exit 1
fi

setup_case clean-success
mkdir -p "$cache_root/alpha" "$cache_root/beta"
run_clean_tty_ok y
assert_command "sudo pacman -Sc"
assert_path_absent "$cache_root/alpha"
assert_path_absent "$cache_root/beta"
assert_contains "Moguet cache cleaned." "$output_file"

setup_case clean-pacman-failure-continues
mkdir -p "$cache_root/alpha" "$cache_root/beta"
printf 'pacman -Sc\n' > "$sudo_failures"
run_clean_tty_fail y
assert_contains "Pacman clean failed or cancelled." "$output_file"
assert_contains "Moguet cache cleaned." "$output_file"
assert_path_absent "$cache_root/alpha"
assert_path_absent "$cache_root/beta"

setup_case clean-concurrent-replacement
mkdir -p "$cache_root/alpha" "$cache_root/beta" "$case_dir/outside"
export MOGUET_TEST_SOURCE_MAINTENANCE_PACMAN_SC_RACE_PATH=$cache_root/alpha
export MOGUET_TEST_SOURCE_MAINTENANCE_PACMAN_SC_RACE_TARGET=$case_dir/outside
run_clean_tty_fail y
if [ ! -L "$cache_root/alpha" ]; then
    echo "clean race fixture did not replace the first target" >&2
    exit 1
fi
if [ ! -d "$cache_root/beta" ]; then
    echo "clean continued deleting after concurrent replacement" >&2
    exit 1
fi
assert_contains "Failed to clean the Moguet cache" "$output_file"
assert_contains "Moguet cache cleanup was incomplete." "$output_file"

setup_case clean-prompt-no
mkdir -p "$cache_root/alpha"
run_clean_tty_ok n
if [ ! -d "$cache_root/alpha" ]; then
    echo "clean removed cache after a no answer" >&2
    exit 1
fi
assert_contains "Skipped Moguet cache cleaning." "$output_file"

setup_case clean-prompt-default-no
mkdir -p "$cache_root/alpha"
run_clean_tty_ok ''
if [ ! -d "$cache_root/alpha" ]; then
    echo "clean removed cache after the default No answer" >&2
    exit 1
fi
assert_contains "Skipped Moguet cache cleaning." "$output_file"

for clean_cancel in q quit cancel; do
    setup_case "clean-prompt-$clean_cancel-cancelled"
    mkdir -p "$cache_root/alpha"
    run_clean_tty_fail "$clean_cancel"
    assert_command "sudo pacman -Sc"
    assert_contains "Cancelled:" "$output_file"
    assert_contains "earlier completed phases remain unchanged" "$output_file"
    assert_not_contains "Build Error:" "$output_file"
    assert_not_contains "Skipped Moguet cache cleaning." "$output_file"
    if [ ! -d "$cache_root/alpha" ]; then
        echo "clean removed cache after cancellation" >&2
        exit 1
    fi
done

setup_case clean-empty-cache
mkdir -p "$cache_root"
run_ok clean
assert_command "sudo pacman -Sc"
assert_contains "Moguet cache is empty." "$output_file"

setup_case clean-missing-cache-root-and-repeat
assert_path_absent "$cache_root"
run_ok clean
assert_command "sudo pacman -Sc"
assert_contains "Moguet cache is empty." "$output_file"
if [ ! -d "$cache_root" ] || [ "$(stat -c '%a' "$cache_root")" != 700 ]; then
    echo "clean did not lazily prepare a 0700 Moguet cache root" >&2
    exit 1
fi
run_ok clean
assert_command "sudo pacman -Sc"
assert_contains "Moguet cache is empty." "$output_file"
if [ "$(stat -c '%a' "$cache_root")" != 700 ]; then
    echo "repeated clean changed the existing safe cache mode" >&2
    exit 1
fi

setup_case clean-no-confirm-default
mkdir -p "$cache_root/alpha"
run_ok --noconfirm clean
assert_command "sudo pacman -Sc --noconfirm"
assert_contains "Skipping prompt (--noconfirm): Clean Moguet build cache ($cache_root)? -> no" "$output_file"
assert_contains "Skipped Moguet cache cleaning." "$output_file"
if [ ! -d "$cache_root/alpha" ]; then
    echo "--noconfirm changed the clean prompt default" >&2
    exit 1
fi

echo "  ok: P0-5 cmd_clean"

# P0-6: upgrade keeps metadata preflight -> pacman -> source execution and catch hierarchy.
setup_case upgrade-metadata-no-target
run_upgrade_ok --noconfirm upgrade
assert_command_at 1 "pacman-conf --verbose RootDir DBPath"
assert_command_at 2 "alpm initialize"
assert_command_at 3 "alpm release"
assert_command_at 4 "sudo pacman -Syu --noconfirm"
assert_command_at 5 "alpm initialize"
assert_command_at 6 "alpm release"
assert_command "sudo pacman -Syu --noconfirm"
assert_total_command_count 6

setup_case upgrade-positional-operand-is-rejected
run_upgrade_fail --noconfirm upgrade ignored-target
assert_contains \
    "Invalid: Operation upgrade does not accept target operands." \
    "$output_file"
assert_total_command_count 0

# 現行契約: --rmdepsはregularかつvalidなsource preferenceがある場合だけ検証する。
# sourceが空のupgradeは--rmdepsを無視してsystem Syuを実行する。
setup_case upgrade-rmdeps-without-source-is-ignored
run_upgrade_ok --rmdeps --noconfirm upgrade
assert_command "sudo pacman -Syu --noconfirm"
assert_command_count "alpm initialize" 2
assert_command_count "alpm release" 2
assert_total_command_count 6

setup_case upgrade-rmdeps-with-source-stops-before-mutation
: > "$preference_dir/clean-root"
run_upgrade_fail --rmdeps --noconfirm upgrade
assert_contains "Separated build/install does not support --rmdeps." "$output_file"
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "pacman -Si"
assert_total_command_count 0

setup_case upgrade-constraint-preflight-firewall
: > "$preference_dir/constraint-block-root"
export MOGUET_TEST_ALPM_VERCMP_EXPECTED_LHS=1.0-1
export MOGUET_TEST_ALPM_VERCMP_EXPECTED_RHS=2.0-1
export MOGUET_TEST_ALPM_VERCMP_RESULT=-1
run_fail --noedit --nodiff --noconfirm upgrade
assert_contains "dependency constraint-block-leaf>=2.0-1 is Unsatisfied" "$output_file"
assert_command_content_absent "sudo "
assert_command_content_absent "git clone"
assert_command_content_absent "git fetch"
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"

setup_case upgrade-invalid-name-is-hard-error
: > "$preference_dir/bad name"
run_upgrade_split_fail --noconfirm upgrade
assert_contains "Invalid source preference entry name in $preference_dir: bad name" "$stderr_file"
assert_not_contains "Ignoring invalid source-build preference filename" "$stdout_file"
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_total_command_count 0

setup_case upgrade-failure-diagnostic-remains-on-stderr
printf 'pacman -Syu --noconfirm\n' > "$sudo_failures"
run_upgrade_split_fail --noconfirm upgrade
assert_contains "System upgrade..." "$stdout_file"
assert_contains "Running: sudo pacman '-Syu' '--noconfirm'" "$stdout_file"
assert_not_contains "The update failed." "$stdout_file"
assert_contains "The update failed." "$stderr_file"
assert_command "sudo pacman -Syu --noconfirm"

setup_case upgrade-metadata-no-preference-root
rmdir "$preference_dir"
run_upgrade_ok --noconfirm upgrade
assert_command "sudo pacman -Syu --noconfirm"
assert_command_count "pacman-conf --verbose RootDir DBPath" 1
assert_command_count "alpm initialize" 2
assert_command_count "alpm release" 2
assert_total_command_count 6

setup_case upgrade-metadata-nonregular-only
mkdir "$preference_dir/alpha"
run_upgrade_fail --noconfirm upgrade
assert_contains "Source preference entry is not a regular file: $preference_dir/alpha" "$output_file"
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "pacman-conf "
assert_command_content_absent "alpm "
assert_total_command_count 0

setup_case upgrade-metadata-invalid-preference-only
: > "$preference_dir/bad name"
run_upgrade_fail --noconfirm upgrade
assert_contains "Invalid source preference entry name in $preference_dir: bad name" "$output_file"
assert_output_line_count "Invalid source preference entry name in $preference_dir: bad name" 1 "$output_file"
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "pacman-conf "
assert_command_content_absent "alpm "
assert_command_absent "pacman -Si bad name"
assert_command_content_absent "git "
assert_command_content_absent "makepkg "
assert_request_log_empty

setup_case upgrade-multi-source-pkgdest-before-syu
: > "$preference_dir/alpha"
printf 'PKGDEST=\n' > "$preference_dir/beta"
export MOGUET_TEST_PACMAN_REPO_PACKAGES='alpha beta'
mkdir -p "$cache_root/preflight-sentinel"
printf 'stable upgrade preflight fixture\n' > \
    "$cache_root/preflight-sentinel/state"
upgrade_preflight_checksum=$(cksum \
    "$cache_root/preflight-sentinel/state")
run_upgrade_fail --noedit --nodiff --noconfirm upgrade
assert_contains "Source environment PKGDEST conflicts with the invocation-owned artifact workspace." "$output_file"
assert_command_count "pacman-conf --verbose RootDir DBPath" 2
assert_command_count "pacman-conf --repo-list" 2
assert_command_count "alpm initialize" 2
assert_command_count "alpm sync-query core/alpha" 1
assert_command_count "alpm sync-query core/beta" 1
assert_command_count "alpm release" 2
assert_command_content_absent "pacman -Si "
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "git "
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"
if [ -e "$cache_root/alpha" ] || [ -L "$cache_root/alpha" ] ||
   [ -e "$cache_root/beta" ] || [ -L "$cache_root/beta" ]; then
    echo "upgrade PKGDEST preflight created a checkout" >&2
    exit 1
fi
upgrade_preflight_after=$(cksum \
    "$cache_root/preflight-sentinel/state")
upgrade_preflight_entry_count=$(count_command_output_lines \
    upgrade-preflight-cache-entries find "$cache_root" \
    -mindepth 1 -maxdepth 1 -print)
if [ "$upgrade_preflight_after" != "$upgrade_preflight_checksum" ] ||
   [ "$upgrade_preflight_entry_count" -ne 1 ]; then
    echo "upgrade PKGDEST preflight mutated the cache tree" >&2
    exit 1
fi

setup_case upgrade-metadata-resolver-failure
: > "$preference_dir/alpha"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=alpha
export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE=42
export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_FAILURE_AT=2
run_upgrade_fail --noconfirm upgrade
assert_contains "pacman-conf failed with exit code 42." "$output_file"
assert_command_count "pacman-conf --verbose RootDir DBPath" 2
assert_command_count "pacman-conf --repo-list" 1
assert_command_count "alpm initialize" 1
assert_command_count "alpm sync-query core/alpha" 1
assert_command_count "alpm release" 1
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "git "
assert_command_content_absent "makepkg "

setup_case upgrade-metadata-session-open-failure
: > "$preference_dir/alpha"
printf 'alpha 1.0-1\n' > "$package_metadata_state"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=alpha
export MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE_AT=2
run_upgrade_fail --noconfirm upgrade
assert_contains "Failed to initialize package metadata session: system error." "$output_file"
assert_command_count "pacman-conf --verbose RootDir DBPath" 2
assert_command_count "pacman-conf --repo-list" 1
assert_command_count "alpm initialize" 2
assert_command_count "alpm sync-query core/alpha" 1
assert_command_content_absent "alpm query "
assert_command_count "alpm release" 1
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "git "
assert_command_content_absent "makepkg "

setup_case upgrade-metadata-query-failure
: > "$preference_dir/alpha"
printf 'alpha 1.0-1\n' > "$package_metadata_state"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=alpha
export MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_AT=1
run_upgrade_fail --noconfirm upgrade
assert_contains "Failed to query installed package metadata for alpha: Installed package query failed: database open failed." "$output_file"
assert_command_count "pacman-conf --verbose RootDir DBPath" 2
assert_command_count "pacman-conf --repo-list" 1
assert_command_count "alpm initialize" 2
assert_command_count "alpm sync-query core/alpha" 1
assert_command_count "alpm query alpha" 1
assert_command_count "alpm release" 2
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "git "
assert_command_content_absent "makepkg "

setup_case upgrade-metadata-multi-target-failure
for package in alpha beta gamma; do
    : > "$preference_dir/$package"
    printf '%s 1.0-1\n' "$package" >> "$package_metadata_state"
done
export MOGUET_TEST_PACMAN_REPO_PACKAGES='alpha beta gamma'
export MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_AT=2
run_upgrade_fail --noconfirm upgrade
assert_contains "Failed to query installed package metadata for " "$output_file"
assert_command_count "pacman-conf --verbose RootDir DBPath" 4
assert_command_count "pacman-conf --repo-list" 3
assert_command_count "alpm initialize" 4
assert_command_prefix_count "alpm query " 2
assert_command_count "alpm release" 4
for package in alpha beta gamma; do
    assert_command_count "alpm sync-query core/$package" 1
done
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "git "
assert_command_content_absent "makepkg "

setup_case upgrade-database-paths-resolved-once
: > "$preference_dir/alpha"
printf 'alpha 1.0-1\n' > "$package_metadata_state"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=alpha
export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE=42
export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_FAILURE_AT=3
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_command_count "pacman-conf --verbose RootDir DBPath" 2
assert_command_count "pacman-conf --repo-list" 1
assert_command_count "alpm initialize" 3
assert_command_count "alpm sync-query core/alpha" 1
assert_command_count "alpm query alpha" 2
assert_command_count "alpm release" 3
assert_command "sudo pacman -Syu --noconfirm"
assert_command_absent "pacman -Si alpha"
assert_command_occurrence_before "alpm release" 2 "sudo pacman -Syu --noconfirm" 1
assert_command_occurrence_before "sudo pacman -Syu --noconfirm" 1 "alpm initialize" 3
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"

setup_case upgrade-post-metadata-session-open-failure
: > "$preference_dir/alpha"
printf 'alpha 1.0-1\n' > "$package_metadata_state"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=alpha
export MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE_AT=3
run_upgrade_fail --noconfirm upgrade
post_snapshot_failure_prefix="The system upgrade completed, but the post-upgrade package metadata snapshot failed: "
post_initialize_failure_diagnostic="${post_snapshot_failure_prefix}Failed to initialize package metadata session: system error."
assert_contains "$post_initialize_failure_diagnostic" "$output_file"
assert_output_line_count "$post_initialize_failure_diagnostic" 1 "$output_file"
assert_not_contains "${post_snapshot_failure_prefix}${post_snapshot_failure_prefix}" "$output_file"
assert_command_count "pacman-conf --verbose RootDir DBPath" 2
assert_command_count "pacman-conf --repo-list" 1
assert_command_count "alpm initialize" 3
assert_command_count "alpm sync-query core/alpha" 1
assert_command_count "alpm query alpha" 1
assert_command_count "alpm release" 2
assert_command "sudo pacman -Syu --noconfirm"
assert_command_absent "pacman -Si alpha"
assert_command_occurrence_before "sudo pacman -Syu --noconfirm" 1 "alpm initialize" 3
assert_command_content_absent "git "
assert_command_content_absent "makepkg "

setup_case upgrade-post-metadata-query-failure
: > "$preference_dir/alpha"
printf 'alpha 1.0-1\n' > "$package_metadata_state"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=alpha
export MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_AT=2
run_upgrade_fail --noedit --nodiff --noconfirm upgrade
post_query_failure_diagnostic="The system upgrade completed, but the post-upgrade package metadata query failed for alpha: Installed package query failed: database open failed. Source processing did not start."
assert_contains "$post_query_failure_diagnostic" "$output_file"
assert_output_line_count "$post_query_failure_diagnostic" 1 "$output_file"
assert_command_count "pacman-conf --verbose RootDir DBPath" 2
assert_command_count "pacman-conf --repo-list" 1
assert_command_count "alpm initialize" 3
assert_command_count "alpm sync-query core/alpha" 1
assert_command_count "alpm query alpha" 2
assert_command_count "alpm release" 3
assert_command "sudo pacman -Syu --noconfirm"
assert_command_absent "pacman -Si alpha"
assert_command_occurrence_before "alpm query alpha" 2 "alpm release" 3
assert_command_content_absent "git "
assert_command_content_absent "vercmp "
assert_command_content_absent "makepkg "

setup_case upgrade-post-metadata-multi-target-query-failure
multi_checkout_fixture=$case_dir/multi-checkout-fixture
mkdir -p "$multi_checkout_fixture/.git"
write_upgrade_srcinfo "$multi_checkout_fixture/.SRCINFO" 2.0 1
printf 'pkgname=fixture\npkgver=2.0\npkgrel=1\n' > "$multi_checkout_fixture/PKGBUILD"
for package in alpha beta gamma; do
    : > "$preference_dir/$package"
    printf '%s 1.0-1\n' "$package" >> "$package_metadata_state"
done
export MOGUET_TEST_PACMAN_REPO_PACKAGES='alpha beta gamma'
export MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_AT=5
export MOGUET_TEST_GIT_CLONE_FIXTURE_DIR=$multi_checkout_fixture
export MOGUET_TEST_VERCMP_OUTPUT=1
run_upgrade_fail --noedit --nodiff --noconfirm upgrade
failed_package=$(awk '
    $1 == "alpm" && $2 == "query" {
        query_count++
        if(query_count == 5) print $3
    }
' "$command_log")
if [ -z "$failed_package" ]; then
    echo "failed to identify the post-upgrade query failure package" >&2
    cat "$command_log" >&2
    exit 1
fi
assert_command_count "pacman-conf --verbose RootDir DBPath" 4
assert_command_count "pacman-conf --repo-list" 3
assert_command_count "alpm initialize" 5
assert_command_prefix_count "alpm query " 6
assert_command_count "alpm release" 5
assert_command_content_absent "makepkg"
assert_command_content_absent "vercmp"
assert_command_content_absent "git clone"
assert_command_content_absent "pacman -U"
for package in alpha beta gamma; do
    assert_command_count "alpm query $package" 2
    assert_command_occurrence_before "alpm query $package" 2 "alpm release" 5
    assert_command_count "alpm sync-query core/$package" 1
    assert_command_absent "pacman -Si $package"
done
multi_query_failure_diagnostic="The system upgrade completed, but the post-upgrade package metadata query failed for $failed_package: Installed package query failed: database open failed. Source processing did not start."
assert_contains "$multi_query_failure_diagnostic" "$output_file"
assert_output_line_count "$multi_query_failure_diagnostic" 1 "$output_file"

setup_case upgrade-ordinary-preflight-errors
: > "$preference_dir/missing-upgrade-a"
: > "$preference_dir/missing-upgrade-b"
run_upgrade_fail --noconfirm upgrade
assert_command_count "pacman-conf --verbose RootDir DBPath" 1
assert_command_count "pacman-conf --repo-list" 1
assert_command_count "alpm initialize" 1
assert_command_prefix_count "alpm sync-query core/missing-upgrade-" 1
assert_command_count "alpm release" 1
assert_command_content_absent "pacman -Si missing-upgrade-"
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_contains "Package not found in repos or AUR: missing-upgrade-" "$output_file"
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"

setup_case upgrade-pacman-failure
: > "$preference_dir/clean-root"
printf 'pacman -Syu --noconfirm\n' > "$sudo_failures"
run_upgrade_fail --noedit --nodiff --noconfirm upgrade
assert_command_absent "pacman -Si clean-root"
assert_command_count "pacman-conf --verbose RootDir DBPath" 2
assert_command_count "pacman-conf --repo-list" 1
assert_command_count "alpm initialize" 2
assert_command_count "alpm sync-query core/clean-root" 1
assert_command_count "alpm query clean-root" 1
assert_command_count "alpm release" 2
assert_command_absent "pacman -Q clean-root"
assert_command "sudo pacman -Syu --noconfirm"
assert_command_occurrence_before "alpm release" 2 "sudo pacman -Syu --noconfirm" 1
assert_contains "The update failed." "$output_file"
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"

setup_case upgrade-static-source-failure-stops-all-packages
: > "$preference_dir/clean-root"
: > "$preference_dir/missing-upgrade"
: > "$preference_dir/bad name"
run_upgrade_fail --noedit --nodiff --noconfirm upgrade
assert_contains "Invalid source preference entry name in $preference_dir: bad name" "$output_file"
assert_command_absent "pacman -Si missing-upgrade"
assert_command_count "pacman-conf --verbose RootDir DBPath" 0
assert_command_content_absent "alpm "
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"

setup_case upgrade-runtime-source-order-follows-preference-enumeration
for package in beta alpha; do
    : > "$preference_dir/$package"
    printf '%s 1.0-1\n' "$package" >> "$package_metadata_state"
    prepare_upgrade_source_checkout "$package"
done
capture_two_source_preference_order
makepkg_cwd_log=$case_dir/makepkg-cwd.log
: > "$makepkg_cwd_log"
export MOGUET_TEST_MAKEPKG_CWD_LOG=$makepkg_cwd_log
export MOGUET_TEST_PACMAN_REPO_PACKAGES='alpha beta'
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_output_before "System upgrade..." "Processing $preference_first..." "$output_file"
assert_output_before "Processing $preference_first..." "Processing $preference_second..." "$output_file"
assert_command_before \
    "alpm sync-query core/$preference_first" \
    "alpm sync-query core/$preference_second"
assert_command_content_absent "pacman -Si "
assert_command_occurrence_before "sudo pacman -Syu --noconfirm" 1 "git fetch origin" 1
assert_command_count "makepkg --packagelist" 2
assert_command_count "makepkg -sc --noconfirm" 2
{
    printf '%s\n' "$cache_root/$preference_first"
    printf '%s\n' "$cache_root/$preference_first"
    printf '%s\n' "$cache_root/$preference_second"
    printf '%s\n' "$cache_root/$preference_second"
} > "$case_dir/expected-makepkg-cwd.log"
assert_file_equals "$case_dir/expected-makepkg-cwd.log" "$makepkg_cwd_log"
assert_not_contains "PackageBase result:" "$output_file"

setup_case upgrade-registered-packagebase-selected-only
registered_child=registered-child
registered_base=registered-suite
registered_checkout=$cache_root/$registered_base
makepkg_cwd_log=$case_dir/makepkg-cwd.log
makepkg_env_log=$case_dir/makepkg-environment.log
printf 'REGISTERED_FIRST=one\nREGISTERED_EMPTY=\nREGISTERED_SECOND="two words"\n' \
    > "$preference_dir/$registered_child"
printf '%s 1.0-1\n' "$registered_child" > "$package_metadata_state"
mkdir -p "$registered_checkout/.git"
{
    printf 'pkgbase = %s\n' "$registered_base"
    printf 'pkgver = 4.2\n'
    printf 'pkgrel = 3\n'
    printf 'pkgname = %s\n' "$registered_child"
    printf 'pkgname = registered-sibling\n'
} > "$registered_checkout/.SRCINFO"
printf 'pkgbase=%s\npkgname=(%s registered-sibling)\npkgver=4.2\npkgrel=3\n' \
    "$registered_base" "$registered_child" \
    > "$registered_checkout/PKGBUILD"
printf 'https://gitlab.archlinux.org/archlinux/packaging/packages/%s.git\n' \
    "$registered_base" \
    > "$registered_checkout/.git/.moguet-test-remote-url"
: > "$makepkg_cwd_log"
: > "$makepkg_env_log"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$registered_child
export MOGUET_TEST_REPOSITORY_PACKAGE_BASE=$registered_base
export MOGUET_TEST_MAKEPKG_CWD_LOG=$makepkg_cwd_log
export MOGUET_TEST_MAKEPKG_ENV_LOG=$makepkg_env_log
export MOGUET_TEST_MAKEPKG_ENV_KEYS='REGISTERED_FIRST REGISTERED_EMPTY REGISTERED_SECOND'
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='registered-suite|registered-sibling|4.2-3
registered-suite|registered-child|4.2-3
registered-suite|registered-child-debug|4.2-3'
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_command_count "alpm sync-query core/$registered_child" 1
assert_command_absent "alpm sync-query core/$registered_base"
assert_command_count "git fetch origin" 1
assert_command_count "makepkg --packagelist" 1
assert_command_count "makepkg -sc --noconfirm" 1
assert_output_line_count "$registered_checkout" 2 "$makepkg_cwd_log"
assert_output_line_count 'env[REGISTERED_FIRST]=<one>' 2 "$makepkg_env_log"
assert_output_line_count 'env[REGISTERED_EMPTY]=<unset>' 2 "$makepkg_env_log"
assert_output_line_count \
    'env[REGISTERED_SECOND]=<two words>' 2 "$makepkg_env_log"
assert_contains \
    "Applying custom build flags: REGISTERED_FIRST='one' REGISTERED_SECOND='two words' " \
    "$output_file"
assert_command_prefix_count "pacman -Qp --color never " 3
assert_command_pattern_count \
    '^sudo pacman -U --noconfirm -- .*/registered-child-4\.2-3-x86_64\.pkg\.tar\.zst$' 1
assert_command_pattern_absent '^sudo pacman -U .*registered-sibling'
assert_command_pattern_absent '^sudo pacman -U .*registered-child-debug'
assert_request_log_empty
assert_contains "PackageBase result: $registered_base" "$output_file"
assert_contains \
    "  required child: $registered_child -> $registered_child 4.2-3 (explicit): installed" \
    "$output_file"
assert_output_before \
    "  produced artifact: registered-sibling 4.2-3 (not selected; not installed)" \
    "  produced artifact: registered-child-debug 4.2-3 (not selected; not installed)" \
    "$output_file"

setup_case upgrade-first-runtime-source-failure-stops-later-source
for package in beta alpha; do
    : > "$preference_dir/$package"
    printf '%s 1.0-1\n' "$package" >> "$package_metadata_state"
done
capture_two_source_preference_order
export MOGUET_TEST_PACMAN_REPO_PACKAGES='alpha beta'
export MOGUET_TEST_GIT_CLONE_FAIL_DESTINATION=$preference_first
run_upgrade_fail --noedit --nodiff --noconfirm upgrade
assert_command "sudo pacman -Syu --noconfirm"
assert_command "git clone https://gitlab.archlinux.org/archlinux/packaging/packages/$preference_first.git $preference_first"
assert_command_absent "git clone https://gitlab.archlinux.org/archlinux/packaging/packages/$preference_second.git $preference_second"
assert_contains "Failed while building/installing PackageBase $preference_first ($preference_first): Failed to clone $preference_first" "$output_file"
assert_not_contains "Processing $preference_second..." "$output_file"
assert_command_occurrence_before "sudo pacman -Syu --noconfirm" 1 \
    "git clone https://gitlab.archlinux.org/archlinux/packaging/packages/$preference_first.git $preference_first" 1
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"

setup_case upgrade-first-source-cleanup-failure-stops-later-source
for package in beta alpha; do
    : > "$preference_dir/$package"
    printf '%s 1.0-1\n' "$package" >> "$package_metadata_state"
    prepare_upgrade_source_checkout "$package"
done
capture_two_source_preference_order
install_success_log=$XDG_CACHE_HOME/pacman-u-success.log
: > "$install_success_log"
export MOGUET_TEST_PACMAN_REPO_PACKAGES='alpha beta'
export MOGUET_TEST_PACMAN_U_SUCCESS_LOG=$install_success_log
export MOGUET_TEST_REPLACE_WORKSPACE_AFTER_PACMAN_U=1
run_upgrade_fail --noedit --nodiff --noconfirm upgrade
assert_command "sudo pacman -Syu --noconfirm"
assert_contains "Processing $preference_first..." "$output_file"
assert_not_contains "Processing $preference_second..." "$output_file"
assert_contains "Package installation succeeded, but artifact workspace cleanup failed:" "$output_file"
assert_command_count "makepkg --packagelist" 1
assert_command_count "makepkg -sc --noconfirm" 1
assert_command_prefix_count "sudo pacman -U --noconfirm -- " 1
assert_cleanup_partial_success_fixture "$install_success_log"

# PR2 contract: strict readerはread不能なregistered preferenceをempty
# environmentへ丸めず、system/source mutation前にtyped preparation failureとする。
setup_upgrade_transition_case \
    upgrade-unreadable-preference-stops-before-mutation \
    1.0-1 1.0-1 enabled \
    https://gitlab.archlinux.org/archlinux/packaging/packages/clean-root.git
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
chmod 000 "$preference_dir/$upgrade_package"
run_upgrade_fail --noedit --nodiff --noconfirm upgrade
chmod 600 "$preference_dir/$upgrade_package"
assert_contains "Source preference entry permissions are not private mode 0600: $preference_dir/$upgrade_package" "$output_file"
assert_not_contains "Loading custom build flags from $preference_dir/$upgrade_package." "$output_file"
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "pacman-conf "
assert_command_content_absent "alpm "
assert_command_content_absent "git "
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"
assert_total_command_count 0

setup_upgrade_transition_case \
    upgrade-rebuild-option-propagation \
    1.0-1 1.0-1 enabled \
    https://gitlab.archlinux.org/archlinux/packaging/packages/clean-root.git
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
run_upgrade_ok --noedit --nodiff --noconfirm \
    --build-mode=rebuild --rebuild upgrade
assert_command "sudo pacman -Syu --noconfirm"
assert_command_count "makepkg -sc --noconfirm -f" 1
assert_command_absent "sudo pacman -Syu --noconfirm -f"
assert_contains "Skipping PKGBUILD/.install review (--noedit)." "$output_file"
assert_command_content_absent "git diff"

# Issue #215 regression: system transactionでofficial binaryへ置換された場合も、
# source-build preferenceを実際のinstalled packageへ反映する。
official_source_url=https://gitlab.archlinux.org/archlinux/packaging/packages/clean-root.git
aur_source_url=https://aur.archlinux.org/clean-root.git

setup_upgrade_transition_case \
    issue-215-case-1-rebuild-after-official-binary-replacement \
    1.0-1 2.0-1 enabled "$official_source_url"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
assert_file_equals "$installed_version_before" "$installed_version_state"
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_equals "$installed_version_after" "$installed_version_state"
assert_file_equals "$remote_srcinfo" "$checkout_dir/.SRCINFO"
assert_contains "Loading custom build flags from $preference_dir/$upgrade_package." "$output_file"
assert_contains "$upgrade_package was updated by the system transaction (1.0-1 -> 2.0-1); rebuilding the preferred source package." "$output_file"
assert_not_contains "$upgrade_package is up to date (2.0-1). Skipping." "$output_file"
assert_command_absent "pacman -Si $upgrade_package"
assert_command "sudo pacman -Syu --noconfirm"
assert_single_package_metadata_snapshots_around_syu "$upgrade_package"
assert_command "git fetch origin"
assert_command "git reset --hard origin/main"
assert_command_absent "pacman -Q $upgrade_package"
assert_command "vercmp 2.0-1 2.0-1"
assert_command_count "makepkg -sc --noconfirm" 1
assert_separated_source_commands 1
assert_command_content_absent "git clone"
assert_command_occurrence_before "alpm release" 3 "git fetch origin" 1
assert_command_occurrence_before "git fetch origin" 1 "git reset --hard origin/main" 1
assert_command_occurrence_before "git reset --hard origin/main" 1 "vercmp 2.0-1 2.0-1" 1
assert_command_occurrence_before "vercmp 2.0-1 2.0-1" 1 "makepkg --packagelist" 1
assert_argv_log "$vercmp_argv_log" 'argv-begin
arg[0]=<2.0-1>
arg[1]=<2.0-1>
argv-end'
assert_makepkg_argv_log "$makepkg_argv_log" 'argv-begin
arg[0]=<--packagelist>
arg[1]=<PKGDEST=<owned>>
argv-end
argv-begin
arg[0]=<-sc>
arg[1]=<--noconfirm>
arg[2]=<PKGDEST=<owned>>
argv-end'
assert_request_log_empty

# F-537-01: positive epochを含むsource/post-installedの同一versionでも、
# system前から更新済みなら#215のsource preference再適用へ進む。
setup_upgrade_transition_case \
    f537-01-positive-epoch-rebuild-after-official-binary-replacement \
    1:1.0-1 1:2.0-1 enabled "$official_source_url"
write_upgrade_srcinfo "$initial_srcinfo" 1.0 1 1
write_upgrade_srcinfo "$remote_srcinfo" 2.0 1 1
cp "$initial_srcinfo" "$checkout_dir/.SRCINFO"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
assert_file_equals "$installed_version_before" "$installed_version_state"
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_equals "$installed_version_after" "$installed_version_state"
assert_file_equals "$remote_srcinfo" "$checkout_dir/.SRCINFO"
assert_contains "Loading custom build flags from $preference_dir/$upgrade_package." "$output_file"
assert_contains "$upgrade_package was updated by the system transaction (1:1.0-1 -> 1:2.0-1); rebuilding the preferred source package." "$output_file"
assert_not_contains "$upgrade_package is up to date (1:2.0-1). Skipping." "$output_file"
assert_command "sudo pacman -Syu --noconfirm"
assert_single_package_metadata_snapshots_around_syu "$upgrade_package"
assert_command "vercmp 1:2.0-1 1:2.0-1"
assert_command_count "makepkg -sc --noconfirm" 1
assert_separated_source_commands 1
assert_command_occurrence_before "alpm release" 3 "git fetch origin" 1
assert_command_occurrence_before "git fetch origin" 1 "git reset --hard origin/main" 1
assert_command_occurrence_before "git reset --hard origin/main" 1 "vercmp 1:2.0-1 1:2.0-1" 1
assert_command_occurrence_before "vercmp 1:2.0-1 1:2.0-1" 1 "makepkg --packagelist" 1
assert_argv_log "$vercmp_argv_log" 'argv-begin
arg[0]=<1:2.0-1>
arg[1]=<1:2.0-1>
argv-end'
assert_request_log_empty

# Case 1とのfixture差は、fake pacman transaction後のinstalled versionだけ。
setup_upgrade_transition_case \
    issue-215-case-2-official-version-unchanged \
    1.0-1 1.0-1 enabled "$official_source_url"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
assert_file_equals "$installed_version_before" "$installed_version_state"
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_equals "$installed_version_after" "$installed_version_state"
assert_file_equals "$remote_srcinfo" "$checkout_dir/.SRCINFO"
assert_contains "Loading custom build flags from $preference_dir/$upgrade_package." "$output_file"
assert_not_contains "rebuilding the preferred source package." "$output_file"
assert_command_absent "pacman -Si $upgrade_package"
assert_command "sudo pacman -Syu --noconfirm"
assert_single_package_metadata_snapshots_around_syu "$upgrade_package"
assert_command "git fetch origin"
assert_command "git reset --hard origin/main"
assert_command_absent "pacman -Q $upgrade_package"
assert_command "vercmp 2.0-1 1.0-1"
assert_command_count "makepkg -sc --noconfirm" 1
assert_separated_source_commands 1
assert_command_content_absent "git clone"
assert_command_occurrence_before "alpm release" 3 "git fetch origin" 1
assert_command_occurrence_before "git reset --hard origin/main" 1 "vercmp 2.0-1 1.0-1" 1
assert_command_occurrence_before "vercmp 2.0-1 1.0-1" 1 "makepkg --packagelist" 1
assert_argv_log "$vercmp_argv_log" 'argv-begin
arg[0]=<2.0-1>
arg[1]=<1.0-1>
argv-end'
assert_makepkg_argv_log "$makepkg_argv_log" 'argv-begin
arg[0]=<--packagelist>
arg[1]=<PKGDEST=<owned>>
argv-end
argv-begin
arg[0]=<-sc>
arg[1]=<--noconfirm>
arg[2]=<PKGDEST=<owned>>
argv-end'
assert_request_log_empty

setup_upgrade_transition_case \
    issue-215-case-3-no-source-preference \
    1.0-1 2.0-1 disabled "$official_source_url"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
assert_file_equals "$installed_version_before" "$installed_version_state"
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_equals "$installed_version_after" "$installed_version_state"
assert_file_equals "$initial_srcinfo" "$checkout_dir/.SRCINFO"
assert_not_contains "rebuilding the preferred source package." "$output_file"
assert_command "sudo pacman -Syu --noconfirm"
assert_command_count "pacman-conf --verbose RootDir DBPath" 1
assert_command_count "alpm initialize" 2
assert_command_count "alpm release" 2
assert_command_absent "pacman -Si $upgrade_package"
assert_command_content_absent "git "
assert_command_absent "pacman -Q $upgrade_package"
assert_command_content_absent "vercmp"
assert_command_content_absent "makepkg"
assert_total_command_count 6
assert_file_empty "$vercmp_argv_log"
assert_file_empty "$makepkg_argv_log"
assert_request_log_empty

setup_upgrade_transition_case \
    issue-215-case-4-aur-build-after-system-upgrade \
    1.0-1 1.0-1 enabled "$aur_source_url"
assert_file_equals "$installed_version_before" "$installed_version_state"
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_equals "$installed_version_after" "$installed_version_state"
assert_file_equals "$remote_srcinfo" "$checkout_dir/.SRCINFO"
assert_contains "Loading custom build flags from $preference_dir/$upgrade_package." "$output_file"
assert_not_contains "rebuilding the preferred source package." "$output_file"
assert_command_absent "pacman -Si $upgrade_package"
assert_command "sudo pacman -Syu --noconfirm"
assert_single_package_metadata_snapshots_around_syu "$upgrade_package"
assert_command "git fetch origin"
assert_command "git reset --hard origin/main"
assert_command_absent "pacman -Q $upgrade_package"
assert_command "vercmp 2.0-1 1.0-1"
assert_command_count "makepkg -sc --noconfirm" 1
assert_separated_source_commands 1
assert_command_content_absent "git clone"
assert_command_occurrence_before "alpm sync-query core/$upgrade_package" 1 "alpm query $upgrade_package" 1
assert_command_occurrence_before "alpm release" 3 "git fetch origin" 1
assert_command_occurrence_before "git reset --hard origin/main" 1 "vercmp 2.0-1 1.0-1" 1
assert_command_occurrence_before "vercmp 2.0-1 1.0-1" 1 "makepkg --packagelist" 1
assert_argv_log "$vercmp_argv_log" 'argv-begin
arg[0]=<2.0-1>
arg[1]=<1.0-1>
argv-end'
assert_makepkg_argv_log "$makepkg_argv_log" 'argv-begin
arg[0]=<--packagelist>
arg[1]=<PKGDEST=<owned>>
argv-end
argv-begin
arg[0]=<-sc>
arg[1]=<--noconfirm>
arg[2]=<PKGDEST=<owned>>
argv-end'
if [ ! -s "$request_log" ]; then
    echo "expected AUR RPC request for $upgrade_package" >&2
    exit 1
fi

setup_upgrade_transition_case \
    issue-215-case-5-equal-version-without-transaction-change \
    2.0-1 2.0-1 enabled "$official_source_url"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
assert_file_equals "$installed_version_before" "$installed_version_state"
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_equals "$installed_version_after" "$installed_version_state"
assert_file_equals "$remote_srcinfo" "$checkout_dir/.SRCINFO"
assert_contains "$upgrade_package is up to date (2.0-1). Skipping." "$output_file"
assert_not_contains "rebuilding the preferred source package." "$output_file"
assert_command_absent "pacman -Si $upgrade_package"
assert_command_absent "pacman -Q $upgrade_package"
assert_command "sudo pacman -Syu --noconfirm"
assert_single_package_metadata_snapshots_around_syu "$upgrade_package" 2
assert_command "git reset --hard origin/main"
assert_command "vercmp 2.0-1 2.0-1"
assert_command_content_absent "makepkg"
assert_command_occurrence_before "git reset --hard origin/main" 1 "vercmp 2.0-1 2.0-1" 1
assert_argv_log "$vercmp_argv_log" 'argv-begin
arg[0]=<2.0-1>
arg[1]=<2.0-1>
argv-end'
assert_file_empty "$makepkg_argv_log"
assert_request_log_empty

setup_upgrade_transition_case \
    issue-215-case-6-older-source-does-not-downgrade \
    1.0-1 3.0-1 enabled "$official_source_url"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
assert_file_equals "$installed_version_before" "$installed_version_state"
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_equals "$installed_version_after" "$installed_version_state"
assert_file_equals "$remote_srcinfo" "$checkout_dir/.SRCINFO"
assert_contains "$upgrade_package is up to date (3.0-1). Skipping." "$output_file"
assert_not_contains "rebuilding the preferred source package." "$output_file"
assert_command_absent "pacman -Si $upgrade_package"
assert_command_absent "pacman -Q $upgrade_package"
assert_command "sudo pacman -Syu --noconfirm"
assert_single_package_metadata_snapshots_around_syu "$upgrade_package" 2
assert_command "git reset --hard origin/main"
assert_command "vercmp 2.0-1 3.0-1"
assert_command_content_absent "makepkg"
assert_command_occurrence_before "git reset --hard origin/main" 1 "vercmp 2.0-1 3.0-1" 1
assert_argv_log "$vercmp_argv_log" 'argv-begin
arg[0]=<2.0-1>
arg[1]=<3.0-1>
argv-end'
assert_file_empty "$makepkg_argv_log"
assert_request_log_empty

setup_upgrade_transition_case \
    issue-215-case-7-installed-by-system-transaction \
    not-installed 2.0-1 enabled "$official_source_url"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
assert_file_empty "$installed_version_state"
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_equals "$installed_version_after" "$installed_version_state"
assert_file_equals "$remote_srcinfo" "$checkout_dir/.SRCINFO"
assert_contains "$upgrade_package was installed by the system transaction at version 2.0-1; rebuilding the preferred source package." "$output_file"
assert_not_contains "$upgrade_package is up to date (2.0-1). Skipping." "$output_file"
assert_command_absent "pacman -Si $upgrade_package"
assert_command_absent "pacman -Q $upgrade_package"
assert_command "sudo pacman -Syu --noconfirm"
assert_single_package_metadata_snapshots_around_syu "$upgrade_package"
assert_command "git reset --hard origin/main"
assert_command "vercmp 2.0-1 2.0-1"
assert_command_count "makepkg -sc --noconfirm" 1
assert_separated_source_commands 1
assert_command_occurrence_before "alpm release" 3 "git reset --hard origin/main" 1
assert_command_occurrence_before "git reset --hard origin/main" 1 "vercmp 2.0-1 2.0-1" 1
assert_command_occurrence_before "vercmp 2.0-1 2.0-1" 1 "makepkg --packagelist" 1
assert_argv_log "$vercmp_argv_log" 'argv-begin
arg[0]=<2.0-1>
arg[1]=<2.0-1>
argv-end'
assert_makepkg_argv_log "$makepkg_argv_log" 'argv-begin
arg[0]=<--packagelist>
arg[1]=<PKGDEST=<owned>>
argv-end
argv-begin
arg[0]=<-sc>
arg[1]=<--noconfirm>
arg[2]=<PKGDEST=<owned>>
argv-end'
assert_request_log_empty

setup_upgrade_transition_case \
    issue-152-post-upgrade-package-absent \
    1.0-1 not-installed enabled "$official_source_url"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_empty "$installed_version_state"
assert_single_package_metadata_snapshots_around_syu "$upgrade_package"
assert_command_absent "pacman -Si $upgrade_package"
assert_command "git fetch origin"
assert_command "git reset --hard origin/main"
assert_command_absent "pacman -Q $upgrade_package"
assert_command_content_absent "vercmp "
assert_command_count "makepkg -sc --noconfirm" 1
assert_separated_source_commands 1
assert_command_occurrence_before "alpm release" 3 "git reset --hard origin/main" 1
assert_command_occurrence_before "alpm release" 3 "git fetch origin" 1
assert_command_occurrence_before "git reset --hard origin/main" 1 "makepkg --packagelist" 1

setup_upgrade_transition_case \
    issue-152-post-upgrade-unknown-install-reason \
    2.0-1 2.0-1 enabled "$official_source_url"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
export MOGUET_TEST_PACKAGE_METADATA_UNKNOWN_REASON_PACKAGE=$upgrade_package
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_contains "$upgrade_package is up to date (2.0-1). Skipping." "$output_file"
assert_single_package_metadata_snapshots_around_syu "$upgrade_package" 2
assert_command "vercmp 2.0-1 2.0-1"
assert_command_absent "pacman -Q $upgrade_package"
assert_command_content_absent "makepkg"

setup_upgrade_transition_case \
    issue-152-mixed-valid-and-invalid-post-entries \
    2.0-1 2.0-1 enabled "$official_source_url"
: > "$preference_dir/bad name"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=$upgrade_package
run_upgrade_fail --noedit --nodiff --noconfirm upgrade
assert_contains "Invalid source preference entry name in $preference_dir: bad name" "$output_file"
assert_output_line_count "Invalid source preference entry name in $preference_dir: bad name" 1 "$output_file"
assert_not_contains "$upgrade_package is up to date (2.0-1). Skipping." "$output_file"
assert_command_absent "sudo pacman -Syu --noconfirm"
assert_command_content_absent "alpm "
assert_command_absent "alpm query bad name"
assert_command_absent "pacman -Si bad name"
assert_command_absent "pacman -Si $upgrade_package"
assert_command_absent "vercmp 2.0-1 2.0-1"
assert_command_absent "pacman -Q $upgrade_package"
assert_command_content_absent "makepkg"
assert_request_log_empty

setup_case issue-152-coherent-post-upgrade-snapshot
post_syu_metadata_state=$case_dir/post-syu-metadata-state
live_metadata_after_first_makepkg=$case_dir/live-metadata-after-first-makepkg
for package in alpha beta; do
    : > "$preference_dir/$package"
    printf '%s 1.0-1\n' "$package" >> "$package_metadata_state"
    printf '%s 1.0-1\n' "$package" >> "$post_syu_metadata_state"
    printf '%s 9.0-1\n' "$package" >> "$live_metadata_after_first_makepkg"
    package_checkout=$cache_root/$package
    mkdir -p "$package_checkout/.git"
    write_upgrade_srcinfo "$package_checkout/.SRCINFO" 2.0 1
    printf 'pkgname=%s\npkgver=2.0\npkgrel=1\n' "$package" > "$package_checkout/PKGBUILD"
    printf 'https://gitlab.archlinux.org/archlinux/packaging/packages/%s.git\n' "$package" > "$package_checkout/.git/.moguet-test-remote-url"
done
export MOGUET_TEST_PACMAN_REPO_PACKAGES='alpha beta'
export MOGUET_TEST_PACMAN_Q_OUTPUT_FILE=$package_metadata_state
export MOGUET_TEST_SOURCE_MAINTENANCE_PACMAN_SYU_Q_OUTPUT_FILE=$post_syu_metadata_state
export MOGUET_TEST_MAKEPKG_PACKAGE_METADATA_STATE_AFTER_SUCCESS_FILE=$live_metadata_after_first_makepkg
run_upgrade_ok --noedit --nodiff --noconfirm upgrade
assert_file_equals "$live_metadata_after_first_makepkg" "$package_metadata_state"
assert_command_count "pacman-conf --verbose RootDir DBPath" 3
assert_command_count "pacman-conf --repo-list" 2
assert_command_count "alpm initialize" 6
assert_command_count "alpm release" 6
assert_command_count "git fetch origin" 2
assert_command_count "git reset --hard origin/main" 2
assert_command_count "vercmp 2.0-1 1.0-1" 2
assert_command_absent "vercmp 2.0-1 9.0-1"
assert_command_count "makepkg -sc --noconfirm" 2
assert_separated_source_commands 2
for package in alpha beta; do
    assert_command_count "alpm query $package" 3
    assert_command_count "alpm sync-query core/$package" 1
    assert_command_absent "pacman -Si $package"
    assert_command_absent "pacman -Q $package"
    assert_command_occurrence_before "alpm query $package" 2 "alpm release" 4
    assert_command_occurrence_before "alpm release" 4 "git fetch origin" 1
done

echo "  ok: P0-6 cmd_upgrade"

# Schema-fatal upgrade cases remain owned by test-aur-rpc-validation.

# P0-7: direct production work-list orchestration and separated lifecycle composition.
setup_case source-plan-order
run_source_ok plan-success
assert_contains "Building AUR PackageBase: dep-target" "$output_file"
assert_contains "Target package(s): dep-target" "$output_file"
assert_contains "Building AUR PackageBase: root-target" "$output_file"
assert_contains "Target package(s): root-target" "$output_file"
assert_command_before \
    "git clone https://aur.archlinux.org/dep-target.git dep-target" \
    "git clone https://aur.archlinux.org/root-target.git root-target"
assert_command_count "pacman-conf --verbose RootDir DBPath" 1
assert_command_count "makepkg -sc --noconfirm" 2
assert_command_prefix_count "sudo pacman -U --noconfirm --needed --asdeps -- " 1
assert_command_prefix_count "sudo pacman -U --noconfirm --needed -- " 1
assert_separated_source_commands 2

setup_case source-plan-failure-context
export MOGUET_TEST_MAKEPKG_EXIT_CODE=42
export MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE=0
run_source_fail plan-failure
assert_contains "The build-only makepkg command failed with exit code 42." "$output_file"
assert_command "git clone https://aur.archlinux.org/dep-target.git dep-target"
assert_command_count "makepkg --packagelist" 1
assert_command_count "makepkg -sc --noconfirm" 1
assert_command_absent "git clone https://aur.archlinux.org/root-target.git root-target"
assert_command_content_absent "sudo pacman -U"

setup_case source-preference-pkgdest-conflict
printf 'FALLBACK=base-value\nPKGDEST=owned-elsewhere\n' > "$preference_dir/base-target"
run_source_fail fallback
assert_contains "Loading custom build flags from $preference_dir/base-target." "$output_file"
assert_contains "Source environment PKGDEST conflicts with the invocation-owned artifact workspace." "$output_file"
assert_command_count "pacman-conf --verbose RootDir DBPath" 0
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"

setup_case source-preference-forwarded
printf 'REQUESTED=requested-value\n' > "$preference_dir/base-target"
run_source_ok fallback
assert_contains "Loading custom build flags from $preference_dir/base-target." "$output_file"
assert_contains "Applying custom build flags: REQUESTED='requested-value' " "$output_file"
assert_command "makepkg -sc --noconfirm"
assert_separated_source_commands 1

setup_case source-preference-empty-pkgdest-conflict
printf 'PKGDEST=\nEMPTY=""\n' > "$preference_dir/base-target"
run_source_fail fallback
assert_contains "Loading custom build flags from $preference_dir/base-target." "$output_file"
assert_contains "Source environment PKGDEST conflicts with the invocation-owned artifact workspace." "$output_file"
assert_command_count "pacman-conf --verbose RootDir DBPath" 0
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"

setup_case source-preference-invalid-assignment-ignored
printf '9INVALID=value\nignored without equals\n' > "$preference_dir/base-target"
run_source_ok fallback
assert_contains "Loading custom build flags from $preference_dir/base-target." "$output_file"
assert_contains "Ignoring invalid environment assignment: 9INVALID=value" "$output_file"
assert_command_count "makepkg -sc --noconfirm" 1
assert_separated_source_commands 1

setup_case smart-source-order
printf 'SMART=preference\n' > "$preference_dir/clean-root"
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
run_source_ok smart-source
assert_output_before \
    "Loading custom build flags from $preference_dir/clean-root." \
    "Processing clean-root..." "$output_file"
assert_command_before \
    "alpm sync-query core/clean-root" \
    "git clone https://gitlab.archlinux.org/archlinux/packaging/packages/clean-root.git clean-root"
assert_command_absent "pacman -Si clean-root"
assert_contains "Applying custom build flags: SMART='preference' " "$output_file"
assert_command_count "makepkg -sc --noconfirm" 1
assert_separated_source_commands 1

setup_case smart-source-missing-post-snapshot
export MOGUET_TEST_PACMAN_REPO_PACKAGES=clean-root
run_source_fail smart-source-missing-post-snapshot
assert_contains "No authoritative installed package snapshot was supplied for clean-root." "$output_file"
assert_command_absent "pacman -Si clean-root"
assert_command_count "pacman-conf --verbose RootDir DBPath" 2
assert_command_count "pacman-conf --repo-list" 1
assert_command_count "alpm initialize" 1
assert_command_count "alpm sync-query core/clean-root" 1
assert_command_count "alpm release" 1
assert_command_content_absent "git clone"
assert_command_content_absent "makepkg"
assert_command_content_absent "pacman -U"
assert_request_log_empty

echo "  ok: P0-7 shared source-install orchestration"

echo "source/maintenance command characterization tests: all checks passed"
