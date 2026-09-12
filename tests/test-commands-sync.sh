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
pty_runner=$repo_root/tests/run-with-pty.py
MOGUET_TEST_REPOSITORY_ROOT=$repo_root
export MOGUET_TEST_REPOSITORY_ROOT
. "$repo_root/tests/test-command-safety.sh"
. "$repo_root/scripts/validation-status.sh"
tmp_dir=$(mktemp -d)

cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

export PATH=$repo_root/tests/stubs/commands-sync:$repo_root/tests/stubs:/usr/bin:/bin
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
require_exact_test_command makepkg "$repo_root/tests/stubs/makepkg"
require_exact_test_command pacman "$repo_root/tests/stubs/commands-sync/pacman"
require_exact_test_command sudo "$repo_root/tests/stubs/commands-sync/sudo"
require_exact_test_command git "$repo_root/tests/stubs/git"

setup_case() {
    case_name=$1
    case_dir=$tmp_dir/cases/$case_name
    command_log=$case_dir/commands.log
    output_file=$case_dir/output
    metadata_log=$case_dir/metadata.log
    config_file=$case_dir/config.toml
    package_metadata_state=$case_dir/package-metadata.state
    repository_metadata_state=$case_dir/repository-metadata.state
    source_preference_dir=$case_dir/xdg-config/moguet/source-build.d

    mkdir -p \
        "$case_dir/home" "$case_dir/xdg-config" \
        "$case_dir/xdg-state" "$case_dir/work" "$case_dir/xdg-cache"
    chmod 0700 "$case_dir/xdg-config"
    : > "$command_log"
    : > "$output_file"
    : > "$metadata_log"
    : > "$package_metadata_state"
    : > "$repository_metadata_state"
    printf '%s\n' 'schema_version = 1' > "$config_file"

    export HOME=$case_dir/home
    export XDG_CONFIG_HOME=$case_dir/xdg-config
    export XDG_STATE_HOME=$case_dir/xdg-state
    export XDG_CACHE_HOME=$case_dir/xdg-cache
    export MOGUET_TEST_COMMAND_LOG=$command_log
    export MOGUET_TEST_CONFIG_FILE=$config_file
    export MOGUET_TEST_PACMAN_MAIN_STATUS=1
    export MOGUET_TEST_SUDO_MAIN_STATUS=0
    export MOGUET_TEST_GIT_CLONE_EXIT_CODE=0
    export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
    MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
    export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST

    unset MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE
    unset MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES
    unset MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE
    unset MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_FAILURE_AT
    unset MOGUET_TEST_PACKAGE_METADATA_STATE_FILE
    unset MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE
    unset MOGUET_TEST_INSPECTION_SCENARIO
    export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$metadata_log
    unset MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE
    unset MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_PACKAGE
    unset MOGUET_TEST_SYNC_CACHE_FAILURE_REPOSITORY
    unset MOGUET_TEST_REPOSITORY_QUERY_FAILURE_PACKAGE
    unset MOGUET_TEST_MAKEPKG_PACKAGE_METADATA_STATE_AFTER_SUCCESS_FILE
    unset MOGUET_TEST_PACMAN_MAIN_COMMAND
    unset MOGUET_TEST_PACMAN_MAIN_OUTPUT
    unset MOGUET_TEST_PACMAN_QM_OUTPUT
    unset MOGUET_TEST_PACMAN_INSTALLED_PACKAGES
    unset MOGUET_TEST_PACMAN_REPO_PACKAGES
    unset MOGUET_TEST_SUDO_MAIN_OUTPUT
    unset MOGUET_TEST_GIT_REMOTE_URL
    unset MOGUET_TEST_GIT_CLONE_FAIL_DESTINATION
    unset MOGUET_TEST_GIT_CLONE_FAIL_DESTINATION_EXIT_CODE
    unset MOGUET_TEST_GIT_CLONE_SYMLINK_TARGET
    unset MOGUET_TEST_GIT_CLONE_FIXTURE_DIR
    unset MOGUET_TEST_GIT_SYMBOLIC_REF
    unset MOGUET_TEST_GIT_SYMBOLIC_REF_EXIT_CODE
    unset MOGUET_TEST_SOURCE_PREFERENCE_EXTERNAL
    unset MOGUET_TEST_PACMAN_U_SUCCESS_LOG
    unset MOGUET_TEST_REPLACE_WORKSPACE_AFTER_PACMAN_U
    unset MOGUET_TEST_ALPM_VERCMP_EXPECTED_LHS
    unset MOGUET_TEST_ALPM_VERCMP_EXPECTED_RHS
    unset MOGUET_TEST_ALPM_VERCMP_RESULT

    export MOGUET_TEST_PACKAGE_METADATA_STATE_FILE=$package_metadata_state
    export MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE=$repository_metadata_state
}

write_source_preference() {
    package=$1
    contents=$2
    mkdir -p "$source_preference_dir"
    chmod 0700 "$source_preference_dir"
    printf '%s\n' "$contents" > "$source_preference_dir/$package"
    chmod 0600 "$source_preference_dir/$package"
}

write_repository_package() {
    package=$1
    printf 'core %s 1 1\n' "$package" >> "$repository_metadata_state"
}

run_status() {
    expected_status=$1
    shift
    : > "$command_log"
    : > "$output_file"

    actual_status=0
    (cd "$case_dir/work" && "$test_binary" "$@" </dev/null) > "$output_file" 2>&1 || actual_status=$?
    if [ "$actual_status" -ne "$expected_status" ]; then
        echo "unexpected status for case $case_name: $actual_status (expected $expected_status)" >&2
        echo "command: $*" >&2
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_status_pty() {
    expected_status=$1
    input=$2
    shift 2
    : > "$command_log"
    : > "$output_file"

    actual_status=0
    (cd "$case_dir/work" &&
        printf '%b' "$input" |
            python3 "$pty_runner" -- "$test_binary" "$@") \
        > "$output_file" 2>&1 || actual_status=$?
    if [ "$actual_status" -ne "$expected_status" ]; then
        echo "unexpected PTY status for case $case_name: $actual_status (expected $expected_status)" >&2
        echo "command: $*" >&2
        sed -n '1,260p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_contains() {
    expected=$1
    file=$2
    if ! grep -F -- "$expected" "$file" >/dev/null; then
        echo "missing expected text in case $case_name: $expected" >&2
        sed -n '1,260p' "$file" >&2
        exit 1
    fi
}

assert_not_contains() {
    unexpected=$1
    file=$2
    if grep -F -- "$unexpected" "$file" >/dev/null; then
        echo "unexpected text in case $case_name: $unexpected" >&2
        sed -n '1,260p' "$file" >&2
        exit 1
    fi
}

assert_output_count() {
    expected_count=$1
    expected=$2
    actual_count=$(validation_grep_count -Fc -- "$expected" "$output_file")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected output count in case $case_name: $expected" >&2
        echo "actual: $actual_count, expected: $expected_count" >&2
        sed -n '1,260p' "$output_file" >&2
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

assert_event() {
    expected=$1
    if ! grep -Fx -- "$expected" "$command_log" >/dev/null; then
        echo "missing expected event in case $case_name: $expected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_event_absent() {
    unexpected=$1
    if grep -Fx -- "$unexpected" "$command_log" >/dev/null; then
        echo "unexpected event in case $case_name: $unexpected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_event_prefix_absent() {
    unexpected_pattern=$1
    if grep -E -- "$unexpected_pattern" "$command_log" >/dev/null; then
        echo "unexpected event pattern in case $case_name: $unexpected_pattern" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_event_count() {
    expected_count=$1
    expected=$2
    actual_count=$(validation_grep_count -Fxc -- "$expected" "$command_log")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected event count in case $case_name: $expected" >&2
        echo "actual: $actual_count, expected: $expected_count" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_event_pattern() {
    expected_pattern=$1
    if ! grep -E -- "$expected_pattern" "$command_log" >/dev/null; then
        echo "missing expected event pattern in case $case_name: $expected_pattern" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_event_pattern_count() {
    expected_count=$1
    expected_pattern=$2
    actual_count=$(validation_grep_count -Ec -- \
        "$expected_pattern" "$command_log")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected event pattern count in case $case_name: $expected_pattern" >&2
        echo "actual: $actual_count, expected: $expected_count" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_event_at() {
    line_number=$1
    expected=$2
    actual=$(sed -n "${line_number}p" "$command_log")
    if [ "$actual" != "$expected" ]; then
        echo "unexpected event at line $line_number in case $case_name" >&2
        echo "actual: $actual" >&2
        echo "expected: $expected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_event_before() {
    first=$1
    second=$2
    first_line=$(grep -nFx -- "$first" "$command_log" | sed -n '1s/:.*//p')
    second_line=$(grep -nFx -- "$second" "$command_log" | sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] || [ "$first_line" -ge "$second_line" ]; then
        echo "unexpected event order in case $case_name" >&2
        echo "expected before: $first" >&2
        echo "expected after:  $second" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_event_count_before() {
    expected_count=$1
    expected=$2
    boundary=$3
    boundary_line=$(grep -nFx -- "$boundary" "$command_log" | sed -n '1s/:.*//p')
    if [ -z "$boundary_line" ]; then
        echo "missing boundary event in case $case_name: $boundary" >&2
        cat "$command_log" >&2
        exit 1
    fi
    pre_boundary_log=$case_dir/pre-boundary.log
    sed -n "1,$((boundary_line - 1))p" "$command_log" \
        >"$pre_boundary_log" || {
        echo "failed to capture pre-boundary events in case $case_name" >&2
        exit 1
    }
    actual_count=$(validation_grep_count \
        -Fxc -- "$expected" "$pre_boundary_log")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected pre-boundary event count in case $case_name: $expected" >&2
        echo "actual: $actual_count, expected: $expected_count" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_log_empty() {
    if [ -s "$command_log" ]; then
        echo "unexpected external event in case $case_name" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_output_line_before() {
    first=$1
    second=$2
    first_line=$(grep -nF -- "$first" "$output_file" | sed -n '1s/:.*//p')
    second_line=$(grep -nF -- "$second" "$output_file" | sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] || [ "$first_line" -ge "$second_line" ]; then
        echo "unexpected output order in case $case_name" >&2
        echo "expected before: $first" >&2
        echo "expected after:  $second" >&2
        sed -n '1,260p' "$output_file" >&2
        exit 1
    fi
}

assert_one_blank_line_between_output_lines() {
    first=$1
    second=$2
    first_line=$(grep -nF -- "$first" "$output_file" | sed -n '1s/:.*//p')
    second_line=$(grep -nF -- "$second" "$output_file" | sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] ||
       [ "$second_line" -ne $((first_line + 2)) ]; then
        echo "unexpected blank-line separation in case $case_name" >&2
        echo "expected before: $first" >&2
        echo "expected after:  $second" >&2
        sed -n '1,260p' "$output_file" >&2
        exit 1
    fi
    separator=$(sed -n "$((first_line + 1))p" "$output_file")
    if [ -n "$separator" ]; then
        echo "expected one blank line in case $case_name" >&2
        sed -n '1,260p' "$output_file" >&2
        exit 1
    fi
}

assert_two_info_blocks_have_one_blank_line() {
    first_end_line=$(grep -nF -- "Out of Date     :" "$output_file" | sed -n '1s/:.*//p')
    second_start_line=$(grep -nF -- "Repository      : aur" "$output_file" | sed -n '2s/:.*//p')
    if [ -z "$first_end_line" ] || [ -z "$second_start_line" ] ||
       [ "$second_start_line" -ne $((first_end_line + 2)) ]; then
        echo "unexpected blank-line separation between AUR info blocks in case $case_name" >&2
        sed -n '1,260p' "$output_file" >&2
        exit 1
    fi
    separator=$(sed -n "$((first_end_line + 1))p" "$output_file")
    if [ -n "$separator" ]; then
        echo "expected one blank line between AUR info blocks in case $case_name" >&2
        sed -n '1,260p' "$output_file" >&2
        exit 1
    fi
}

assert_no_mutation_events() {
    if grep -E '^(sudo pacman -(S|U)|pacman -U|git (clone|fetch)( |$)|makepkg )' "$command_log" >/dev/null; then
        echo "mutation event occurred before validation/plan barrier in case $case_name" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_cache_root_absent() {
    if [ -e "$XDG_CACHE_HOME/moguet" ] || [ -L "$XDG_CACHE_HOME/moguet" ]; then
        echo "cache root was created before invocation constraint preflight in case $case_name" >&2
        find "$XDG_CACHE_HOME" -maxdepth 3 -print >&2 || true
        exit 1
    fi
}

assert_state_log_absent() {
    state_path=$XDG_STATE_HOME/moguet
    if [ -e "$state_path" ] || [ -L "$state_path" ]; then
        echo "state log path was created before the selection preflight completed in case $case_name: $state_path" >&2
        exit 1
    fi
}

assert_cache_entry_absent() {
    entry=$XDG_CACHE_HOME/moguet/$1
    if [ -e "$entry" ] || [ -L "$entry" ]; then
        echo "unexpected cache entry in case $case_name: $entry" >&2
        exit 1
    fi
}

assert_cache_entry_present() {
    entry=$XDG_CACHE_HOME/moguet/$1
    if [ ! -d "$entry" ]; then
        echo "missing cache entry in case $case_name: $entry" >&2
        find "$XDG_CACHE_HOME" -maxdepth 3 -print >&2 || true
        sed -n '1,200p' "$output_file" >&2
        exit 1
    fi
}

ESC=$(printf '\033')

# P0-1/P0-2: search handler validation, selector behavior, preflight order, continuation, status, presentation.
setup_case search-missing-query
run_status 1 -Ss --aur
assert_contains "Missing search query." "$output_file"
assert_command_log_empty

setup_case aur-search-rejects-needed
run_status 1 -Ss --aur --needed search-hit-a
assert_contains "Unsupported pacman option for AUR search: --needed" "$output_file"
assert_command_log_empty

setup_case aur-search-presentation-no-installed-query
export MOGUET_TEST_PACMAN_QM_OUTPUT='search-presented 1.0-1'
run_status 0 -Ss --aur search-presented
assert_event_at 1 "aur search search-presented"
assert_event_count 1 "aur search search-presented"
assert_event_prefix_absent '^(pacman|sudo) '
assert_contains "Searching AUR..." "$output_file"
assert_contains "${ESC}[1;35maur${ESC}[0m/${ESC}[1msearch-presented${ESC}[0m ${ESC}[1;32m2.0-1${ESC}[0m ${ESC}[1;31m[out-of-date]${ESC}[0m ${ESC}[1;33m[orphaned]${ESC}[0m" "$output_file"
assert_output_line_before "Searching AUR..." "aur${ESC}[0m/${ESC}[1msearch-presented"
assert_not_contains "[installed]" "$output_file"
assert_event_prefix_absent '^pacman-conf '
assert_not_contains "alpm " "$metadata_log"
assert_contains "    search presentation fixture" "$output_file"

setup_case aur-search-empty
run_status 1 -Ss --aur search-empty
assert_event_at 1 "aur search search-empty"
assert_event_prefix_absent '^(pacman|sudo) '

setup_case repo-search-status-and-ordered-args
export MOGUET_TEST_PACMAN_MAIN_COMMAND='-Ss repo-only'
export MOGUET_TEST_PACMAN_MAIN_STATUS=13
run_status 13 -Ss --repo repo-only
assert_event_at 1 "pacman -Ss repo-only"
assert_event_count 1 "pacman -Ss repo-only"
assert_event_prefix_absent '^aur '
assert_event_prefix_absent '^sudo '

setup_case repo-search-refresh-sudo-and-global-option
export MOGUET_TEST_SUDO_MAIN_STATUS=17
run_status 17 --noconfirm -Ss --repo --refresh repo-a --config config-value repo-b
assert_event_at 1 "sudo pacman -Ss --noconfirm --refresh repo-a --config config-value repo-b"
assert_event_count 1 "sudo pacman -Ss --noconfirm --refresh repo-a --config config-value repo-b"
assert_event_prefix_absent '^aur '
assert_event_prefix_absent '^pacman '

setup_case auto-search-pacman-failure-aur-success
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$package_metadata_state
export MOGUET_TEST_PACMAN_MAIN_COMMAND='-Ss search-hit-a'
export MOGUET_TEST_PACMAN_MAIN_OUTPUT='repo search failed output'
export MOGUET_TEST_PACMAN_MAIN_STATUS=9
run_status 0 -Ss search-hit-a
assert_event_at 1 "pacman -Ss search-hit-a"
assert_event_at 2 "pacman-conf --verbose RootDir DBPath"
assert_event_at 3 "pacman-conf --repo-list"
assert_event_at 4 "aur search search-hit-a"
assert_output_line_before "repo search failed output" "Searching AUR..."
assert_output_line_before "Searching AUR..." "aur${ESC}[0m/${ESC}[1msearch-hit-a"

setup_case auto-search-pacman-success-aur-empty
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$package_metadata_state
export MOGUET_TEST_PACMAN_MAIN_COMMAND='-Ss search-empty'
export MOGUET_TEST_PACMAN_MAIN_STATUS=0
run_status 0 -Ss search-empty
assert_event_at 1 "pacman -Ss search-empty"
assert_event_at 2 "pacman-conf --verbose RootDir DBPath"
assert_event_at 3 "pacman-conf --repo-list"
assert_event_at 4 "aur search search-empty"

setup_case auto-search-both-empty-fail
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$package_metadata_state
export MOGUET_TEST_PACMAN_MAIN_COMMAND='-Ss search-empty'
export MOGUET_TEST_PACMAN_MAIN_STATUS=8
run_status 1 -Ss search-empty
assert_event_at 1 "pacman -Ss search-empty"
assert_event_at 2 "pacman-conf --verbose RootDir DBPath"
assert_event_at 3 "pacman-conf --repo-list"
assert_event_at 4 "aur search search-empty"

setup_case auto-search-installed-presentation
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$package_metadata_state
export MOGUET_TEST_PACMAN_MAIN_COMMAND='-Ss search-presented'
export MOGUET_TEST_PACMAN_MAIN_STATUS=0
printf 'search-presented 1.0-1\n' > "$package_metadata_state"
run_status 0 -Ss search-presented
assert_event_at 1 "pacman -Ss search-presented"
assert_event_at 2 "pacman-conf --verbose RootDir DBPath"
assert_event_at 3 "pacman-conf --repo-list"
assert_event_at 4 "aur search search-presented"
assert_contains "${ESC}[1;36m[installed]${ESC}[0m ${ESC}[1;31m[out-of-date]${ESC}[0m ${ESC}[1;33m[orphaned]${ESC}[0m" "$output_file"

setup_case auto-search-refresh-preflight-and-deferred-failure
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$package_metadata_state
export MOGUET_TEST_SUDO_MAIN_OUTPUT='repo refresh search output'
export MOGUET_TEST_SUDO_MAIN_STATUS=7
run_status 0 -Ssy search-deferred search-hit-b -- -skip
assert_event_at 1 "aur search search-deferred"
assert_event_at 2 "aur search search-hit-b"
assert_event_at 3 "sudo pacman -Ssy search-deferred search-hit-b -- -skip"
assert_event_at 4 "pacman-conf --verbose RootDir DBPath"
assert_event_at 5 "pacman-conf --repo-list"
assert_event_at 6 "aur search search-deferred"
assert_event_at 7 "aur search search-hit-b"
assert_event_absent "aur search -skip"
assert_output_line_before "repo refresh search output" "Searching AUR..."

setup_case auto-search-refresh-schema-stop
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$package_metadata_state
run_status 1 -Ssy search-hit-a search-schema
assert_event_at 1 "aur search search-hit-a"
assert_event_at 2 "aur search search-schema"
assert_event_count 1 "aur search search-schema"
assert_event_prefix_absent '^(pacman|sudo) '
assert_contains "fixture search schema failure" "$output_file"

# P0-3/P0-4: info validation, per-target continuation, Installed query, layout, filtering, aggregate status.
setup_case aur-info-rejects-needed
run_status 1 -Si --aur --needed info-a
assert_contains "Unsupported pacman option for AUR info: --needed" "$output_file"
assert_command_log_empty

setup_case aur-info-missing-target
run_status 1 -Si --aur
assert_contains "Missing AUR package target." "$output_file"
assert_command_log_empty

setup_case aur-info-validates-all-targets-before-rpc
run_status 1 -Si --aur info-a core/filesystem info-b
assert_contains "Invalid AUR package target: core/filesystem" "$output_file"
assert_command_log_empty

setup_case aur-info-continuation-installed-and-layout
printf 'info-installed 1.0-1\n' > "$package_metadata_state"
run_status 1 -Si --aur info-installed info-missing info-error info-uninstalled
assert_event_at 1 "aur info info-installed"
assert_event_at 2 "aur info info-missing"
assert_event_at 3 "aur info info-error"
assert_event_at 4 "aur info info-uninstalled"
assert_event_at 5 "pacman-conf --verbose RootDir DBPath"
assert_contains "alpm query info-installed" "$metadata_log"
assert_event_at 6 "pacman-conf --verbose RootDir DBPath"
assert_contains "alpm query info-uninstalled" "$metadata_log"
assert_event_absent "pacman -Q info-missing"
assert_event_absent "pacman -Q info-error"
assert_event_prefix_absent '^pacman -Si( |$)'
assert_event_prefix_absent '^sudo '
assert_contains "AUR package not found: info-missing" "$output_file"
assert_contains "Failed to fetch AUR info for info-error: fixture info failure" "$output_file"
assert_contains "Installed       : ${ESC}[1;36myes${ESC}[0m" "$output_file"
assert_contains "Installed       : no" "$output_file"
assert_output_line_before "Name            : info-installed" "Name            : info-uninstalled"
assert_two_info_blocks_have_one_blank_line

setup_case aur-info-all-success
run_status 0 -Si --aur info-a info-b
assert_event_at 1 "aur info info-a"
assert_event_at 2 "aur info info-b"
assert_event_at 3 "pacman-conf --verbose RootDir DBPath"
assert_contains "alpm query info-a" "$metadata_log"
assert_event_at 4 "pacman-conf --verbose RootDir DBPath"
assert_contains "alpm query info-b" "$metadata_log"
assert_event_prefix_absent '^pacman -Si( |$)'
assert_event_prefix_absent '^sudo '
assert_output_line_before "Name            : info-a" "Name            : info-b"
assert_two_info_blocks_have_one_blank_line

setup_case repo-info-refresh-status
export MOGUET_TEST_SUDO_MAIN_STATUS=19
run_status 19 -Si --repo --refresh core/filesystem
assert_event_at 1 "sudo pacman -Si --refresh core/filesystem"
assert_event_count 1 "sudo pacman -Si --refresh core/filesystem"
assert_event_prefix_absent '^aur '
assert_event_prefix_absent '^pacman '

setup_case auto-info-refresh-barrier
run_status 1 -Siy core/qualified info-a
assert_contains "Cannot combine pacman refresh with AUR info fallback for unqualified target: info-a" "$output_file"
assert_contains "Use a repository-qualified target such as repo/package, or run refresh and -Si separately." "$output_file"
assert_command_log_empty

setup_case auto-info-mixed-continuation-filtering-and-layout
write_repository_package repo-local
export MOGUET_TEST_PACMAN_MAIN_COMMAND='-Si core/qualified --config info-a repo-local'
export MOGUET_TEST_PACMAN_MAIN_OUTPUT='repo info transaction output'
export MOGUET_TEST_PACMAN_MAIN_STATUS=0
run_status 1 -Si core/qualified --config info-a repo-local info-a info-missing info-error info-b
assert_event_count 7 "pacman-conf --verbose RootDir DBPath"
assert_event_count 5 "pacman-conf --repo-list"
assert_contains "alpm sync-query core/repo-local" "$metadata_log"
assert_event_before "aur info info-a" "aur info info-missing"
assert_event_before "aur info info-missing" "aur info info-error"
assert_event_before "aur info info-error" "aur info info-b"
assert_event_before "aur info info-b" "pacman -Si core/qualified --config info-a repo-local"
assert_contains "alpm query info-a" "$metadata_log"
assert_contains "alpm query info-b" "$metadata_log"
assert_event_absent "pacman -Si repo-local"
assert_event_absent "pacman -Si core/qualified --config info-a repo-local info-missing info-error"
assert_contains "Package not found in repos or AUR: info-missing" "$output_file"
assert_contains "Failed to fetch AUR info for info-error: fixture info failure" "$output_file"
assert_output_line_before "repo info transaction output" "Repository      : aur"
assert_one_blank_line_between_output_lines "repo info transaction output" "Repository      : aur"
assert_output_line_before "Name            : info-a" "Name            : info-b"
assert_two_info_blocks_have_one_blank_line

setup_case auto-info-pacman-failure-not-hidden
export MOGUET_TEST_PACMAN_MAIN_COMMAND='-Si core/qualified'
export MOGUET_TEST_PACMAN_MAIN_OUTPUT='repo info failed output'
export MOGUET_TEST_PACMAN_MAIN_STATUS=6
run_status 1 -Si core/qualified info-a
assert_event_at 1 "pacman-conf --verbose RootDir DBPath"
assert_event_at 2 "pacman-conf --repo-list"
assert_event_at 3 "aur info info-a"
assert_event_at 4 "pacman -Si core/qualified"
assert_event_at 5 "pacman-conf --verbose RootDir DBPath"
assert_contains "alpm query info-a" "$metadata_log"
assert_contains "Name            : info-a" "$output_file"
assert_output_line_before "repo info failed output" "Repository      : aur"
assert_one_blank_line_between_output_lines "repo info failed output" "Repository      : aur"

# P0-5/P0-6/P0-7: install transaction boundary, all-root/all-source barriers, ordering and failure stops.
# Issue #512: optional installed metadata preserves useful info, but never says no.
for failure in config open query cache; do
    setup_case "info-installed-metadata-$failure"
    printf 'info-a 1.0-1\n' > "$package_metadata_state"
    case $failure in
        config) export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE=7 ;;
        open) export MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE=1 ;;
        query) export MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_PACKAGE=info-a ;;
        cache) export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$case_dir/missing ;;
    esac
    run_status 0 -Si --aur info-a
    assert_event "aur info info-a"
    assert_contains "Installed       : unavailable" "$output_file"
    assert_contains "Installed state is unavailable for info-a:" "$output_file"
    assert_not_contains "Installed       : no" "$output_file"
    assert_event_prefix_absent '^pacman -Q '
done

for failure in config open cache query malformed; do
    setup_case "auto-info-metadata-$failure"
    case $failure in
        config) export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE=7 ;;
        open) export MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE=1 ;;
        cache) export MOGUET_TEST_SYNC_CACHE_FAILURE_REPOSITORY=core ;;
        query) export MOGUET_TEST_REPOSITORY_QUERY_FAILURE_PACKAGE=info-a ;;
        malformed) printf 'core info-a 1 1 invalid/base\n' > "$repository_metadata_state" ;;
    esac
    run_status 1 -Si info-a
    assert_event_prefix_absent '^aur '
    assert_event_prefix_absent '^pacman '
    assert_contains "Failed to inspect repository metadata for info-a:" "$output_file"
    assert_not_contains "Package not found" "$output_file"
done

setup_case auto-info-metadata-failure-mixed-operands
write_repository_package repo-local
export MOGUET_TEST_REPOSITORY_QUERY_FAILURE_PACKAGE=info-a
export MOGUET_TEST_PACMAN_MAIN_COMMAND='-Si core/qualified --config info-a repo-local'
export MOGUET_TEST_PACMAN_MAIN_STATUS=0
run_status 1 -Si core/qualified --config info-a info-a repo-local info-b
assert_event_absent "aur info info-a"
assert_event "aur info info-b"
assert_event "pacman -Si core/qualified --config info-a repo-local"
assert_event_count 1 "pacman -Si core/qualified --config info-a repo-local"
assert_event_absent "pacman -Si core/qualified --config info-a info-a repo-local"
assert_contains "Failed to inspect repository metadata for info-a:" "$output_file"
assert_contains "Name            : info-b" "$output_file"

setup_case auto-search-confirmed-empty-inventory
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$package_metadata_state
run_status 0 -Ss search-presented
assert_not_contains "[installed]" "$output_file"
assert_not_contains "inventory is unavailable" "$output_file"
assert_contains "alpm sync-cache core" "$metadata_log"

setup_case auto-search-native-not-foreign
printf 'search-presented 1.0-1\n' > "$package_metadata_state"
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$package_metadata_state
write_repository_package search-presented
run_status 0 -Ss search-presented
assert_not_contains "[installed]" "$output_file"
assert_not_contains "inventory is unavailable" "$output_file"
assert_contains "alpm sync-query core/search-presented" "$metadata_log"

for failure in config open cache query partial; do
    setup_case "auto-search-inventory-$failure"
    printf 'search-presented 1.0-1\n' > "$package_metadata_state"
    export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$package_metadata_state
    case $failure in
        config) export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE=7 ;;
        open) export MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE=1 ;;
        cache) export MOGUET_TEST_SYNC_CACHE_FAILURE_REPOSITORY=core ;;
        query) export MOGUET_TEST_REPOSITORY_QUERY_FAILURE_PACKAGE=search-presented ;;
        partial) printf 'invalid/name 1.0-1\n' >> "$package_metadata_state" ;;
    esac
    run_status 0 -Ss search-presented
    assert_event "aur search search-presented"
    assert_not_contains "[installed]" "$output_file"
    assert_contains "Foreign package inventory is unavailable; installed annotations are omitted:" "$output_file"
    assert_event_absent "pacman -Qm"
    if [ "$failure" = partial ]; then
        # The first package was observed before the later malformed entry.
        assert_contains "alpm sync-query core/search-presented" "$metadata_log"
    fi
done

setup_case repo-install-one-ordered-transaction
write_source_preference repo-a 'CFLAGS=-Oshould-not-load'
export MOGUET_TEST_SUDO_MAIN_STATUS=31
run_status 31 --noconfirm -S --repo repo-a --config config-value repo-b
assert_event_at 1 "sudo pacman -S --noconfirm repo-a --config config-value repo-b"
assert_event_count 1 "sudo pacman -S --noconfirm repo-a --config config-value repo-b"
assert_event_prefix_absent '^aur '
assert_event_prefix_absent '^pacman '
assert_event_prefix_absent '^(git|makepkg) '
assert_not_contains "Loading custom build flags" "$output_file"

setup_case aur-install-missing-target
run_status 1 -S --aur
assert_contains "Missing AUR package target." "$output_file"
assert_command_log_empty

setup_case aur-install-validates-all-targets-before-plan
run_status 1 -S --aur plan-a core/filesystem plan-b
assert_contains "Invalid AUR package target: core/filesystem" "$output_file"
assert_command_log_empty

setup_case aur-install-rejects-option-before-plan
run_status 1 -S --aur plan-a --config config-value plan-b
assert_contains "Error: Unsupported: Unsupported pacman option for AUR/source-build target: --config" "$output_file"
assert_contains "Error: Unsupported: Rerun --aur without this option." "$output_file"
assert_output_count 2 "Error:"
assert_command_log_empty

setup_case aur-install-all-root-plan-barrier
run_status 1 --noedit --nodiff --noconfirm -S --aur plan-a plan-missing
assert_event_at 1 "aur info-strict plan-a"
assert_event_at 2 "aur info-strict plan-missing"
assert_event_count 1 "aur info-strict plan-a"
assert_event_count 1 "aur info-strict plan-missing"
assert_no_mutation_events
assert_event_prefix_absent '^sudo '
assert_cache_entry_absent plan-a
assert_cache_entry_absent plan-missing

setup_case aur-install-constraint-preflight-firewall
export MOGUET_TEST_ALPM_VERCMP_EXPECTED_LHS=1.0-1
export MOGUET_TEST_ALPM_VERCMP_EXPECTED_RHS=2.0-1
export MOGUET_TEST_ALPM_VERCMP_RESULT=-1
run_status 1 --noedit --nodiff --noconfirm -S --aur constraint-block-root
assert_contains "dependency constraint-block-leaf>=2.0-1 is Unsatisfied" "$output_file"
assert_no_mutation_events
assert_event_prefix_absent '^sudo '
assert_cache_entry_absent constraint-block-root
assert_cache_entry_absent constraint-block-leaf

setup_case aur-install-partial-provider-firewall
run_status_pty 1 '1\n' --noedit --nodiff -S --aur install-partial-root
assert_contains "source metadata is incomplete" "$output_file"
assert_not_contains ":: provider dependency=" "$output_file"
assert_no_mutation_events
assert_event_prefix_absent '^sudo '
assert_cache_root_absent

setup_case aur-install-cross-target-conflict-before-prompt
export MOGUET_TEST_ALPM_VERCMP_EXPECTED_LHS=2
export MOGUET_TEST_ALPM_VERCMP_EXPECTED_RHS=2
export MOGUET_TEST_ALPM_VERCMP_RESULT=0
run_status_pty 1 '1\n' --noedit --nodiff -S --aur \
    install-conflict-root-a install-conflict-root-b
assert_contains "is Conflicting" "$output_file"
assert_not_contains ":: provider dependency=" "$output_file"
assert_no_mutation_events
assert_event_prefix_absent '^sudo '
assert_cache_root_absent

setup_case aur-install-plan-order-needed-and-preferences-disabled
write_source_preference plan-a 'CFLAGS=-Oaur-only-must-ignore'
write_source_preference plan-b 'CFLAGS=-Oaur-only-must-ignore'
run_status 0 --noedit --nodiff --noconfirm -S --aur --needed plan-a plan-b
assert_event_at 1 "aur info-strict plan-a"
assert_event_at 2 "aur info-strict plan-b"
assert_event_at 3 "aur info-strict plan-a"
assert_event_at 4 "aur info-strict plan-b"
assert_event_at 5 "pacman-conf --verbose RootDir DBPath"
assert_event_at 6 "git clone https://aur.archlinux.org/plan-a.git plan-a"
assert_event_at 7 "git config --get remote.origin.url"
assert_event_at 8 "makepkg --packagelist"
assert_event_at 9 "makepkg -sc --noconfirm"
assert_event_pattern '^pacman -Qp --color never -- .*/plan-a-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_event_pattern '^sudo pacman -U --noconfirm --needed -- .*/plan-a-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_event_at 12 "git clone https://aur.archlinux.org/plan-b.git plan-b"
assert_event_at 13 "git config --get remote.origin.url"
assert_event_at 14 "makepkg --packagelist"
assert_event_at 15 "makepkg -sc --noconfirm"
assert_event_pattern '^pacman -Qp --color never -- .*/plan-b-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_event_pattern '^sudo pacman -U --noconfirm --needed -- .*/plan-b-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_event_count 1 "pacman-conf --verbose RootDir DBPath"
assert_event_count 2 "makepkg --packagelist"
assert_event_count 2 "makepkg -sc --noconfirm"
assert_event_pattern_count 2 '^pacman -Qp --color never '
assert_event_pattern_count 2 '^sudo pacman -U --noconfirm --needed -- '
assert_event_absent "sudo pacman -S --noconfirm --needed plan-a plan-b"
assert_not_contains "Loading custom build flags" "$output_file"
assert_not_contains "Applying custom build flags" "$output_file"
assert_cache_entry_present plan-a
assert_cache_entry_present plan-b

setup_case aur-install-first-execution-failure-stops-later-plan
export MOGUET_TEST_MAKEPKG_EXIT_CODE=42
export MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE=0
run_status 1 --noedit --nodiff --noconfirm -S --aur --needed plan-a plan-b
assert_event_at 1 "aur info-strict plan-a"
assert_event_at 2 "aur info-strict plan-b"
assert_event_at 3 "aur info-strict plan-a"
assert_event_at 4 "aur info-strict plan-b"
assert_event_at 5 "pacman-conf --verbose RootDir DBPath"
assert_event_at 6 "git clone https://aur.archlinux.org/plan-a.git plan-a"
assert_event_at 8 "makepkg --packagelist"
assert_event_at 9 "makepkg -sc --noconfirm"
assert_event_pattern_count 0 '^pacman -Qp --color never '
assert_event_pattern_count 0 '^sudo pacman -U '
assert_event_absent "git clone https://aur.archlinux.org/plan-b.git plan-b"
assert_event_absent "sudo pacman -S --noconfirm --needed plan-a plan-b"
assert_cache_entry_present plan-a
assert_cache_entry_absent plan-b

setup_case aur-install-cleanup-partial-success-stops-later-plan
installed_state=$XDG_CACHE_HOME/installed-state
installed_after_success=$XDG_CACHE_HOME/installed-after-success
install_success_log=$XDG_CACHE_HOME/pacman-u-success.log
: > "$installed_state"
printf 'plan-a 1.0-1\n' > "$installed_after_success"
: > "$install_success_log"
export MOGUET_TEST_PACKAGE_METADATA_STATE_FILE=$installed_state
export MOGUET_TEST_MAKEPKG_PACKAGE_METADATA_STATE_AFTER_SUCCESS_FILE=$installed_after_success
export MOGUET_TEST_PACMAN_U_SUCCESS_LOG=$install_success_log
export MOGUET_TEST_REPLACE_WORKSPACE_AFTER_PACMAN_U=1
run_status 1 --noedit --nodiff --noconfirm -S --aur plan-a plan-b
assert_contains "Package installation succeeded, but artifact workspace cleanup failed:" "$output_file"
assert_not_contains "Build Error:" "$output_file"
assert_not_contains "Failed while building/installing PackageBase" "$output_file"
assert_not_contains "Pacman failed." "$output_file"
assert_not_contains "pacman -U failed" "$output_file"
assert_not_contains "The update failed." "$output_file"
assert_event_pattern_count 1 '^pacman -Qp --color never '
assert_event_pattern_count 1 '^sudo pacman -U --noconfirm -- '
assert_event_absent "git clone https://aur.archlinux.org/plan-b.git plan-b"
assert_cache_entry_absent plan-b
if ! cmp -s "$installed_after_success" "$installed_state"; then
    echo "fake pacman -U did not publish installed state in case $case_name" >&2
    exit 1
fi
assert_cleanup_partial_success_fixture "$install_success_log"

# Issue #505: exact targetless -Syu is a sequential repository + normal AUR
# update, while --repo remains the full repository-only escape hatch.
setup_case system-aur-update-unsupported-option-before-mutation
run_status 1 -Syu --config custom.conf
assert_contains "A pacman option is not supported for the combined -Syu route." "$output_file"
assert_contains "moguet -Syu --repo" "$output_file"
assert_command_log_empty
assert_state_log_absent

setup_case system-aur-update-rmdeps-before-mutation
run_status 1 -Syu --rmdeps
assert_contains "--rmdeps" "$output_file"
assert_command_log_empty
assert_state_log_absent

setup_case system-aur-update-aur-selector-rejected
run_status 1 -Syu --aur
assert_contains "Cannot combine --aur with pacman refresh for operation -Syu." "$output_file"
assert_command_log_empty
assert_state_log_absent

setup_case system-repository-update-full-pass-through
write_source_preference system-update-a 'INVALID PREFERENCE'
export MOGUET_TEST_SUDO_MAIN_STATUS=17
run_status 17 --noconfirm -Syu --repo --config custom.conf
assert_event_at 1 "sudo pacman -Syu --noconfirm --config custom.conf"
assert_event_count 1 "sudo pacman -Syu --noconfirm --config custom.conf"
assert_event_prefix_absent '^aur '
assert_event_prefix_absent '^pacman-conf '
assert_event_prefix_absent '^alpm '
assert_event_prefix_absent '^(git|makepkg) '
assert_not_contains "Loading custom build flags" "$output_file"

setup_case noncanonical-system-update-routes-stay-existing
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 -Sy
assert_event_at 1 "sudo pacman -Sy"
assert_event_count 1 "sudo pacman -Sy"
assert_event_prefix_absent '^aur info-many'

setup_case noncanonical-sysupgrade-only-stays-existing
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 -Su
assert_event_at 1 "sudo pacman -Su"
assert_event_count 1 "sudo pacman -Su"
assert_event_prefix_absent '^aur info-many'

setup_case noncanonical-modifier-order-stays-existing
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 -Suy
assert_event_at 1 "sudo pacman -Suy"
assert_event_count 1 "sudo pacman -Suy"
assert_event_prefix_absent '^aur info-many'

setup_case noncanonical-separated-modifiers-stay-existing
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 -S -y -u
assert_event_at 1 "sudo pacman -S -y -u"
assert_event_count 1 "sudo pacman -S -y -u"
assert_event_prefix_absent '^aur info-many'

setup_case noncanonical-separated-long-modifiers-stay-existing
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 -S --refresh --sysupgrade
assert_event_at 1 "sudo pacman -S --refresh --sysupgrade"
assert_event_count 1 "sudo pacman -S --refresh --sysupgrade"
assert_event_prefix_absent '^aur info-many'

setup_case unknown-system-modifier-stays-delegated
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 -Syux
assert_event_at 1 "sudo pacman -Syux"
assert_event_count 1 "sudo pacman -Syux"
assert_event_prefix_absent '^aur info-many'

setup_case system-aur-update-repository-failure-stops-fresh-authority
foreign_inventory=$case_dir/foreign-inventory.state
printf 'system-update-a 0.9-1 explicit\n' > "$foreign_inventory"
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_SUDO_MAIN_STATUS=42
run_status 1 --noedit --nodiff --noconfirm -Syu
assert_event_at 1 "sudo pacman -Syu --noconfirm"
assert_event_count 1 "sudo pacman -Syu --noconfirm"
assert_event_prefix_absent '^aur '
assert_event_prefix_absent '^pacman-conf '
assert_event_prefix_absent '^alpm '
assert_event_prefix_absent '^(git|makepkg) '
assert_contains "The repository system upgrade failed." "$output_file"
assert_contains "The AUR update was not attempted." "$output_file"

setup_case system-aur-update-fresh-configuration-failure-reports-cause
export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE=37
export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_FAILURE_AT=1
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 1 --noedit --nodiff --noconfirm -Syu
repository_update='sudo pacman -Syu --noconfirm'
assert_event_at 1 "$repository_update"
assert_event_count 1 "$repository_update"
assert_event_at 2 "pacman-conf --verbose RootDir DBPath"
assert_event_prefix_absent '^aur '
assert_event_prefix_absent '^(git|makepkg) '
assert_event_pattern_count 0 '^sudo pacman -(R|U) '
assert_contains "The repository system upgrade completed." "$output_file"
assert_contains \
    "The repository system upgrade completed, but the fresh installed-package inventory for AUR could not be obtained." \
    "$output_file"
assert_contains \
    "Query failure: pacman-conf failed with exit code 37. [source=pacman]" \
    "$output_file"
assert_output_count 1 "pacman-conf failed with exit code 37."
assert_contains "The AUR update was not attempted." "$output_file"
assert_contains \
    "The completed repository system upgrade was not rolled back." \
    "$output_file"

setup_case system-aur-update-fatal-query-reports-safe-cause
foreign_inventory=$case_dir/foreign-inventory.state
printf 'system-query-fatal 0.9-1 explicit\n' > "$foreign_inventory"
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 1 --noedit --nodiff --noconfirm -Syu
repository_update='sudo pacman -Syu --noconfirm'
assert_event_at 1 "$repository_update"
assert_event_before "$repository_update" "aur info-many system-query-fatal"
assert_event_prefix_absent '^(git|makepkg) '
assert_event_pattern_count 0 '^sudo pacman -(R|U) '
assert_contains "The repository system upgrade completed." "$output_file"
assert_contains \
    "The repository system upgrade completed, but the fresh AUR update query failed." \
    "$output_file"
assert_contains \
    "Query failure: fixture fatal AUR schema failure\\x0A\\x1Bunsafe\\x5Cdetail [source=aur]" \
    "$output_file"
assert_output_count 1 "fixture fatal AUR schema failure"
assert_contains "The AUR update was not attempted." "$output_file"
assert_not_contains "AUR update:" "$output_file"
assert_contains \
    "The completed repository system upgrade was not rolled back." \
    "$output_file"

setup_case system-aur-update-no-updates-is-not-whole-noop
foreign_inventory=$case_dir/foreign-inventory.state
: > "$foreign_inventory"
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 --noedit --nodiff --noconfirm -Syu
assert_event_at 1 "sudo pacman -Syu --noconfirm"
assert_event_before "sudo pacman -Syu --noconfirm" "pacman-conf --verbose RootDir DBPath"
assert_event_prefix_absent '^aur '
assert_event_prefix_absent '^(git|makepkg) '
assert_contains "The repository system upgrade completed." "$output_file"
assert_contains "AUR update: no updates" "$output_file"
assert_not_contains "No changes are required." "$output_file"
assert_not_contains "[source=aur]" "$output_file"
assert_not_contains "[source=pacman]" "$output_file"
assert_not_contains "Query failure:" "$output_file"
assert_not_contains "Execution failure:" "$output_file"
assert_not_contains "Blocked:" "$output_file"
assert_not_contains "Internal inconsistency:" "$output_file"

setup_case system-aur-update-success-is-fresh-and-ignores-preference
foreign_inventory=$case_dir/foreign-inventory.state
printf 'system-update-a 0.9-1 explicit\n' > "$foreign_inventory"
write_source_preference system-update-a 'INVALID PREFERENCE'
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 --noedit --nodiff --noconfirm -Syu --needed
repository_update='sudo pacman -Syu --noconfirm --needed'
assert_event_at 1 "$repository_update"
assert_event_before "$repository_update" "pacman-conf --verbose RootDir DBPath"
assert_event_before "pacman-conf --verbose RootDir DBPath" "aur info-many system-update-a"
assert_event_before "aur info-many system-update-a" "git clone https://aur.archlinux.org/system-update-a.git system-update-a"
assert_event_pattern '^sudo pacman -U --noconfirm -- .*/system-update-a-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_event_pattern_count 0 '^sudo pacman -U --noconfirm --needed '
assert_not_contains "Loading custom build flags" "$output_file"
assert_not_contains "Applying custom build flags" "$output_file"
assert_contains "The repository system upgrade completed." "$output_file"
assert_contains "AUR update: completed" "$output_file"
assert_contains "The repository system upgrade and normal AUR update completed." "$output_file"
assert_not_contains "[source=aur]" "$output_file"
assert_not_contains "[source=pacman]" "$output_file"
assert_not_contains "Query failure:" "$output_file"
assert_not_contains "Execution failure:" "$output_file"
assert_not_contains "Blocked:" "$output_file"
assert_not_contains "Internal inconsistency:" "$output_file"

setup_case system-aur-update-independent-requires-check-is-attention
foreign_inventory=$case_dir/foreign-inventory.state
printf '%s\n' \
    'tree-sitter-cli-git 1.0-1 explicit' \
    'wezterm-git 1.0-1 explicit' \
    'xpadneo-dkms-git 1.0-1 explicit' > "$foreign_inventory"
write_source_preference tree-sitter-cli-git 'INVALID PREFERENCE'
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 --noedit --nodiff --noconfirm -Syu
assert_event_at 1 "sudo pacman -Syu --noconfirm"
assert_event_before \
    "sudo pacman -Syu --noconfirm" \
    "aur info-many tree-sitter-cli-git wezterm-git xpadneo-dkms-git"
assert_event_prefix_absent '^(git|makepkg) '
assert_event_pattern_count 0 '^sudo pacman -U '
assert_contains "The repository system upgrade completed." "$output_file"
assert_contains "AUR update: no updates" "$output_file"
assert_contains "The repository system upgrade and normal AUR update completed." "$output_file"
assert_output_count 3 \
    "skipped: devel update requires check: suffix candidate only; not automatically updated because authoritative build provenance is unavailable"
assert_contains "package=tree-sitter-cli-git" "$output_file"
assert_contains "package=wezterm-git" "$output_file"
assert_contains "package=xpadneo-dkms-git" "$output_file"
assert_not_contains "preflight issue: devel update requires check" "$output_file"
assert_not_contains "The completed repository system upgrade was not rolled back." "$output_file"
assert_not_contains "Loading custom build flags" "$output_file"

# The existing authoritative query stub refines system-current to
# RequiresCheck(ProvenanceMissing), without suffix candidate evidence.
for scenario in attention-only mixed-update; do
    setup_case "system-aur-update-authoritative-requires-check-$scenario"
    foreign_inventory=$case_dir/foreign-inventory.state
    printf '%s\n' 'system-current 1.0-1 explicit' > "$foreign_inventory"
    if [ "$scenario" = mixed-update ]; then
        printf '%s\n' 'system-update-a 0.9-1 explicit' >> "$foreign_inventory"
    fi
    export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
    export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
    export MOGUET_TEST_INSPECTION_SCENARIO=foreign-authoritative-requires-check
    run_status 0 --noedit --nodiff --noconfirm -Syu
    assert_event_at 1 "sudo pacman -Syu --noconfirm"
    assert_event_before "sudo pacman -Syu --noconfirm" "pacman-conf --verbose RootDir DBPath"
    assert_output_count 1 \
        "skipped: devel update requires check; local authority is unavailable or has changed"
    assert_contains "Warning: Requires check" "$output_file"
    assert_contains "package=system-current" "$output_file"
    assert_contains "The repository system upgrade completed." "$output_file"
    assert_contains "The repository system upgrade and normal AUR update completed." "$output_file"
    assert_not_contains "Unknown devel RequiresCheck attention reason." "$output_file"
    assert_not_contains "suffix candidate only" "$output_file"
    assert_not_contains "system-current: skipped: up to date" "$output_file"
    assert_not_contains "preflight issue: devel update requires check" "$output_file"
    assert_not_contains "The completed repository system upgrade was not rolled back." "$output_file"
    assert_event_pattern_count 0 '^git clone .* system-current$'
    assert_event_pattern_count 0 '^sudo pacman -U .*system-current-'
    if [ "$scenario" = mixed-update ]; then
        assert_event_pattern '^sudo pacman -U --noconfirm -- .*/system-update-a-1\.0-1-x86_64\.pkg\.tar\.zst$'
        assert_contains "system-update-a: updated" "$output_file"
        assert_contains "AUR update: completed" "$output_file"
    else
        assert_event_prefix_absent '^(git|makepkg) '
        assert_event_pattern_count 0 '^sudo pacman -U '
        assert_contains "AUR update: no updates" "$output_file"
    fi
done

setup_case system-aur-update-mixed-independent-requires-check
foreign_inventory=$case_dir/foreign-inventory.state
printf '%s\n' \
    'system-update-a 0.9-1 explicit' \
    'system-devel-git 1.0-1 explicit' \
    'system-current 1.0-1 explicit' > "$foreign_inventory"
write_source_preference system-devel-git 'INVALID PREFERENCE'
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 --noedit --nodiff --noconfirm -Syu
assert_event_before \
    "aur info-many system-update-a system-devel-git system-current" \
    "git clone https://aur.archlinux.org/system-update-a.git system-update-a"
assert_event_pattern '^sudo pacman -U --noconfirm -- .*/system-update-a-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_contains "AUR update: completed" "$output_file"
assert_contains "system-update-a: updated" "$output_file"
assert_contains "system-current: skipped: up to date" "$output_file"
assert_contains "package=system-devel-git" "$output_file"
assert_output_count 1 \
    "skipped: devel update requires check: suffix candidate only; not automatically updated because authoritative build provenance is unavailable"
assert_contains "The repository system upgrade and normal AUR update completed." "$output_file"
assert_not_contains "The completed repository system upgrade was not rolled back." "$output_file"
assert_not_contains "Loading custom build flags" "$output_file"

setup_case system-aur-update-required-requires-check-is-partial
foreign_inventory=$case_dir/foreign-inventory.state
printf '%s\n' \
    'system-required-root 0.9-1 explicit' \
    'system-required-git 1.0-1 dependency' > "$foreign_inventory"
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 1 --noedit --nodiff --noconfirm -Syu
assert_event_at 1 "sudo pacman -Syu --noconfirm"
assert_event_prefix_absent '^(git|makepkg) '
assert_event_pattern_count 0 '^sudo pacman -U '
assert_contains "The repository system upgrade completed." "$output_file"
assert_contains "system-required-root: incomplete: devel update requires check" "$output_file"
assert_contains "system-required-git: skipped: devel update requires check: suffix candidate only" "$output_file"
assert_contains "The repository system upgrade completed, but the AUR update was blocked before execution." "$output_file"
assert_contains "The completed repository system upgrade was not rolled back." "$output_file"
assert_not_contains "Warning: Requires check" "$output_file"

setup_case system-aur-update-work-failure-is-partial
foreign_inventory=$case_dir/foreign-inventory.state
printf 'system-update-a 0.9-1 explicit\n' > "$foreign_inventory"
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_SUDO_MAIN_STATUS=0
export MOGUET_TEST_MAKEPKG_EXIT_CODE=42
export MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE=0
run_status 1 --noedit --nodiff --noconfirm -Syu
assert_event_at 1 "sudo pacman -Syu --noconfirm"
assert_event "makepkg -sc --noconfirm"
assert_event_pattern_count 0 '^sudo pacman -U '
assert_contains "The repository system upgrade completed." "$output_file"
assert_contains "The repository system upgrade completed, but the AUR update failed." "$output_file"
assert_contains "The completed repository system upgrade was not rolled back." "$output_file"

setup_case system-aur-update-cleanup-failure-is-distinct
foreign_inventory=$case_dir/foreign-inventory.state
installed_state=$XDG_CACHE_HOME/installed-state
installed_after_success=$XDG_CACHE_HOME/installed-after-success
install_success_log=$XDG_CACHE_HOME/pacman-u-success.log
printf 'system-update-a 0.9-1 explicit\n' > "$foreign_inventory"
: > "$installed_state"
printf 'system-update-a 1.0-1\n' > "$installed_after_success"
: > "$install_success_log"
export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_PACKAGE_METADATA_STATE_FILE=$installed_state
export MOGUET_TEST_MAKEPKG_PACKAGE_METADATA_STATE_AFTER_SUCCESS_FILE=$installed_after_success
export MOGUET_TEST_PACMAN_U_SUCCESS_LOG=$install_success_log
export MOGUET_TEST_REPLACE_WORKSPACE_AFTER_PACMAN_U=1
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 1 --noedit --nodiff --noconfirm -Syu
assert_event_at 1 "sudo pacman -Syu --noconfirm"
assert_event_pattern_count 1 '^sudo pacman -U --noconfirm -- '
assert_contains "updated, but cleanup failed" "$output_file"
assert_contains "AUR update cleanup failed after a package transaction." "$output_file"
assert_contains "The repository system upgrade completed, but AUR cleanup failed after a package transaction." "$output_file"
assert_contains "The completed repository system upgrade was not rolled back." "$output_file"
assert_cleanup_partial_success_fixture "$install_success_log"

setup_case auto-install-targetless-pacman-pass-through
export MOGUET_TEST_SUDO_MAIN_STATUS=23
run_status 23 --noconfirm -S
assert_event_at 1 "sudo pacman -S --noconfirm"
assert_event_count 1 "sudo pacman -S --noconfirm"
assert_event_prefix_absent '^aur '
assert_event_prefix_absent '^pacman '
assert_event_prefix_absent '^(git|makepkg) '

setup_case auto-install-validates-all-targets-before-classification
run_status 1 -S source-a core/filesystem source-b
assert_event_prefix_absent '^pacman '
assert_event_prefix_absent '^aur '
assert_no_mutation_events
assert_contains "Invalid package name: core/filesystem" "$output_file"

setup_case auto-install-unsupported-option-before-source-guard
run_status 1 -S source-a --config config-value
assert_event_at 1 "pacman-conf --verbose RootDir DBPath"
assert_event_at 2 "pacman-conf --repo-list"
assert_event_prefix_absent '^aur '
assert_no_mutation_events
assert_contains "Error: Unsupported: Unsupported pacman option for AUR/source-build target: --config" "$output_file"
assert_contains "Error: Unsupported: Split official repository and AUR/source-build targets, or rerun without this option." "$output_file"
assert_output_count 2 "Error:"

setup_case auto-install-all-source-guard-before-pacman
write_repository_package official-a
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a'
run_status 1 --noedit --nodiff --noconfirm -S official-a source-a plan-missing
assert_event_count 3 "pacman-conf --verbose RootDir DBPath"
assert_event_count 3 "pacman-conf --repo-list"
assert_event_at 7 "aur info-strict source-a"
assert_event_at 8 "aur info-strict plan-missing"
assert_event_prefix_absent '^sudo pacman -S( |$)'
assert_event_prefix_absent '^(git|makepkg) '
assert_cache_entry_absent source-a
assert_cache_entry_absent plan-missing

setup_case auto-install-later-source-pkgdest-before-official-transaction
write_repository_package official-a
write_source_preference source-b 'PKGDEST='
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a'
mkdir -p "$XDG_CACHE_HOME/moguet/preflight-sentinel"
printf 'stable auto preflight fixture\n' > \
    "$XDG_CACHE_HOME/moguet/preflight-sentinel/state"
auto_preflight_checksum=$(cksum \
    "$XDG_CACHE_HOME/moguet/preflight-sentinel/state")
run_status 1 --noedit --nodiff --noconfirm -S official-a source-a source-b
assert_contains "Source environment PKGDEST conflicts with the invocation-owned artifact workspace." "$output_file"
# Three strict root repository reads complete; source invocation DB-path
# preparation must not add a fourth read after the PKGDEST guard fails.
assert_event_count 3 "pacman-conf --verbose RootDir DBPath"
assert_event_prefix_absent '^sudo '
assert_event_prefix_absent '^(git|makepkg) '
assert_event_pattern_count 0 '^pacman -U '
assert_cache_entry_absent source-a
assert_cache_entry_absent source-b
auto_preflight_after=$(cksum \
    "$XDG_CACHE_HOME/moguet/preflight-sentinel/state")
auto_preflight_entries_raw=$case_dir/auto-preflight-entries.raw
if validation_capture_output "$auto_preflight_entries_raw" \
    find "$XDG_CACHE_HOME/moguet" \
    -mindepth 1 -maxdepth 1 -print; then
    auto_preflight_entry_count=$(wc -l <"$auto_preflight_entries_raw")
else
    preflight_status=$?
    echo "Auto mixed PKGDEST cache producer failed with status $preflight_status" >&2
    exit 1
fi
if [ "$auto_preflight_after" != "$auto_preflight_checksum" ] ||
   [ "$auto_preflight_entry_count" -ne 1 ]; then
    echo "Auto mixed PKGDEST preflight mutated the cache tree" >&2
    exit 1
fi

setup_case auto-install-mixed-order-filtering-and-source-asymmetry
write_repository_package official-a
write_repository_package forced-official
write_source_preference forced-official 'CFLAGS=-Oforced-official'
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a forced-official'
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 --noedit --nodiff --noconfirm -S official-a --needed source-a forced-official source-b
official_transaction='sudo pacman -S --noconfirm official-a --needed'
assert_event "$official_transaction"
assert_event_count 1 "$official_transaction"
assert_event_count_before 2 "aur info-strict source-a" "$official_transaction"
assert_event_count_before 2 "aur info-strict source-b" "$official_transaction"
assert_event_count_before 4 "pacman-conf --repo-list" "$official_transaction"
assert_event_before "pacman-conf --verbose RootDir DBPath" "$official_transaction"
assert_event_before "$official_transaction" "git clone https://aur.archlinux.org/source-a.git source-a"
assert_event_before "git clone https://aur.archlinux.org/source-a.git source-a" "git clone https://gitlab.archlinux.org/archlinux/packaging/packages/forced-official.git forced-official"
assert_event_before "git clone https://gitlab.archlinux.org/archlinux/packaging/packages/forced-official.git forced-official" "git clone https://aur.archlinux.org/source-b.git source-b"
assert_event_count 5 "pacman-conf --verbose RootDir DBPath"
assert_event_count 3 "makepkg --packagelist"
assert_event_count 3 "makepkg -sc --noconfirm"
assert_event_pattern_count 3 '^pacman -Qp --color never '
assert_event_pattern_count 3 '^sudo pacman -U --noconfirm --needed -- '
assert_event_absent "sudo pacman -S --noconfirm official-a --needed source-a forced-official source-b"
assert_event_absent "sudo pacman -S --noconfirm source-a"
assert_event_absent "sudo pacman -S --noconfirm forced-official"
assert_event_absent "sudo pacman -S --noconfirm source-b"
assert_contains "Loading custom build flags from $source_preference_dir/forced-official." "$output_file"
assert_cache_entry_present source-a
assert_cache_entry_present forced-official
assert_cache_entry_present source-b

setup_case auto-install-cache-activation-failure-stops-official-transaction
write_repository_package official-a
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a'
printf '%s\n' 'cache root obstruction' > "$XDG_CACHE_HOME/moguet"
cache_obstruction_checksum=$(cksum "$XDG_CACHE_HOME/moguet")
run_status 1 --noedit --nodiff --noconfirm -S official-a source-a
assert_event_prefix_absent '^sudo '
assert_event_prefix_absent '^(git|makepkg) '
assert_event_pattern_count 0 '^pacman -U '
if [ ! -f "$XDG_CACHE_HOME/moguet" ] ||
   [ "$(cksum "$XDG_CACHE_HOME/moguet")" != "$cache_obstruction_checksum" ]; then
    echo "Sync cache activation failure changed its obstruction fixture" >&2
    exit 1
fi

setup_case auto-install-dry-run-duplicate-repository-source-correlation
write_repository_package duplicate-root
write_source_preference duplicate-root 'CFLAGS=-Oduplicate-root'
export MOGUET_TEST_PACMAN_REPO_PACKAGES='duplicate-root'
run_status 0 --dry-run --noedit --nodiff --noconfirm -S duplicate-root duplicate-root
assert_output_count 1 "Request: duplicate-root (invocation index: 0)"
assert_output_count 1 "Request: duplicate-root (invocation index: 1)"
assert_output_count 2 "Identity: duplicate-root (PackageBase: duplicate-root; source key: repository:duplicate-root)"
assert_event_prefix_absent '^(sudo|git|makepkg) '
assert_event_pattern_count 0 '^pacman -U '

setup_case auto-install-pacman-failure-stops-source-execution
write_repository_package official-a
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a'
export MOGUET_TEST_SUDO_MAIN_STATUS=42
run_status 1 --noedit --nodiff --noconfirm -S official-a source-a source-b
failed_transaction='sudo pacman -S --noconfirm official-a'
assert_event "$failed_transaction"
assert_event_count_before 2 "aur info-strict source-a" "$failed_transaction"
assert_event_count_before 2 "aur info-strict source-b" "$failed_transaction"
assert_event_prefix_absent '^(git|makepkg) '
assert_contains "Pacman failed." "$output_file"
assert_cache_entry_absent source-a
assert_cache_entry_absent source-b

setup_case auto-install-first-source-failure-stops-later-target
write_repository_package official-a
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a'
export MOGUET_TEST_SUDO_MAIN_STATUS=0
export MOGUET_TEST_MAKEPKG_EXIT_CODE=42
export MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE=0
run_status 1 --noedit --nodiff --noconfirm -S official-a source-a source-b
assert_event "sudo pacman -S --noconfirm official-a"
assert_event_before "sudo pacman -S --noconfirm official-a" "git clone https://aur.archlinux.org/source-a.git source-a"
assert_event "makepkg --packagelist"
assert_event "makepkg -sc --noconfirm"
assert_event_pattern_count 0 '^pacman -Qp --color never '
assert_event_pattern_count 0 '^sudo pacman -U '
assert_event_absent "git clone https://aur.archlinux.org/source-b.git source-b"
assert_cache_entry_present source-a
assert_cache_entry_absent source-b

setup_case auto-install-sysupgrade-without-official-target
export MOGUET_TEST_SUDO_MAIN_STATUS=0
run_status 0 --noedit --nodiff --noconfirm -Syu source-a
assert_event "sudo pacman -Syu --noconfirm"
assert_event_count_before 2 "aur info-strict source-a" "sudo pacman -Syu --noconfirm"
assert_event_before "pacman-conf --verbose RootDir DBPath" "sudo pacman -Syu --noconfirm"
assert_event_before "sudo pacman -Syu --noconfirm" "git clone https://aur.archlinux.org/source-a.git source-a"
assert_event "makepkg --packagelist"
assert_event "makepkg -sc --noconfirm"
assert_event_pattern '^pacman -Qp --color never -- .*/source-a-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_event_pattern '^sudo pacman -U --noconfirm -- .*/source-a-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_event_absent "sudo pacman -Syu --noconfirm source-a"

# #553 is confined to the exact target-less aggregate. A foreign-inventory
# sentinel would fail if any target-bearing form accidentally started a sweep.
for sync_operation in -Syu -Su; do
    setup_case "issue553-target-bearing-$sync_operation"
    write_repository_package official-a
    write_repository_package official-b
    export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a official-b'
    foreign_inventory=$case_dir/foreign-inventory.state
    printf 'system-query-fatal 0.9-1 explicit\n' > "$foreign_inventory"
    export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory
    run_status 0 --noconfirm "$sync_operation" official-a
    assert_event "sudo pacman $sync_operation --noconfirm official-a"
    assert_event_prefix_absent '^aur '
    assert_not_contains 'devel tracking baseline is missing' "$output_file"
    run_status 0 --noconfirm "$sync_operation" official-a official-b
    assert_event "sudo pacman $sync_operation --noconfirm official-a official-b"
    assert_event_prefix_absent '^aur '
    run_status 0 --noconfirm "$sync_operation" -- official-a
    assert_event "sudo pacman $sync_operation --noconfirm -- official-a"
    assert_event_prefix_absent '^aur '
    assert_not_contains 'devel tracking baseline is missing' "$output_file"
done

setup_case issue553-target-bearing-su-explicit-source
run_status 0 --noedit --nodiff --noconfirm -Su source-a
assert_event "sudo pacman -Su --noconfirm"
assert_event "git clone https://aur.archlinux.org/source-a.git source-a"
assert_event_pattern '^sudo pacman -U --noconfirm -- .*/source-a-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_event_absent 'aur info-many system-query-fatal'
assert_not_contains 'devel tracking baseline is missing' "$output_file"

# P0-8/P0-9: Issue #217 production root search/selection route and phase barrier.
setup_case select-nontty-gate-before-query
run_status 1 -S --select select-scope
assert_contains \
    "Error: Unavailable: Interactive package selection requires a TTY on standard input." \
    "$output_file"
assert_event_prefix_absent '^root search '
assert_command_log_empty
assert_state_log_absent

setup_case select-noconfirm-gate-before-query
run_status 1 --noconfirm -S --select select-scope
assert_contains \
    "Error: Unavailable: Interactive package selection is not available with --noconfirm." \
    "$output_file"
assert_event_prefix_absent '^root search '
assert_command_log_empty
assert_state_log_absent

setup_case select-no-candidates-without-prompt
run_status_pty 1 '' -S --select select-empty
assert_event_at 1 "root search all select-empty"
assert_contains "Error: Unavailable: No package candidates were found." \
    "$output_file"
assert_not_contains "Package candidates:" "$output_file"
assert_not_contains "Select package numbers" "$output_file"
assert_event_prefix_absent '^(sudo|pacman|pacman-conf|git|makepkg|aur) '
assert_state_log_absent

setup_case select-typed-search-failure-without-prompt
run_status_pty 1 '' -S --select select-search-failure
assert_event_at 1 "root search all select-search-failure"
assert_contains "AUR package search failed: fixture selected root search failure" "$output_file"
assert_not_contains "Package candidates:" "$output_file"
assert_not_contains "Select package numbers" "$output_file"
assert_event_prefix_absent '^(sudo|pacman|pacman-conf|git|makepkg|aur) '
assert_state_log_absent

setup_case select-typed-repository-search-failure-without-prompt
run_status_pty 1 '' -S --select --repo select-repository-search-failure
assert_event_at 1 "root search repository select-repository-search-failure"
assert_contains "Repository package search failed: fixture selected repository search failure" "$output_file"
assert_not_contains "Package candidates:" "$output_file"
assert_not_contains "Select package numbers" "$output_file"
assert_event_prefix_absent '^(sudo|pacman|pacman-conf|git|makepkg|aur) '
assert_state_log_absent

setup_case select-presentation-invalid-retry-cancel
run_status_pty 1 '0\nq\n' -S --select select-presentation
assert_event_at 1 "root search all select-presentation"
assert_event_count 1 "root search all select-presentation"
assert_contains "Package candidates:" "$output_file"
assert_contains "1) source=repository repository=aur package=repo-presented version=3.0-1 groups=@desktop" "$output_file"
assert_contains "    repository presentation fixture" "$output_file"
assert_contains "2) source=AUR package=aur-presented PackageBase=aur-presented version=4.0-1" "$output_file"
assert_contains "    AUR presentation fixture" "$output_file"
assert_contains \
    "Invalid: Package selection index is outside the displayed range 1-2." \
    "$output_file"
assert_output_count 2 "Select package numbers, ascending ranges, or displayed @group; press Enter or enter q/quit/cancel to cancel:"
assert_contains "Cancelled: Package selection was cancelled." "$output_file"
assert_event_prefix_absent '^(sudo|pacman|pacman-conf|git|makepkg|aur) '
assert_state_log_absent

setup_case select-ambiguous-alternative-retry-cancel
run_status_pty 1 '1-2\nq\n' -S --select select-alternative-conflict
assert_event_at 1 "root search all select-alternative-conflict"
assert_contains \
    "Ambiguous: Package shared-alternative was selected from more than one source; select exactly one source. [package=shared-alternative]" \
    "$output_file"
assert_contains "Cancelled: Package selection was cancelled." "$output_file"
assert_event_prefix_absent '^(sudo|pacman|pacman-conf|git|makepkg|aur) '
assert_state_log_absent

setup_case select-repository-scope
run_status_pty 1 'q\n' -S --select --repo select-scope
assert_event_at 1 "root search repository select-scope"
assert_contains "source=repository repository=core package=scope-repo" "$output_file"
assert_not_contains "source=AUR package=scope-aur" "$output_file"
assert_contains "Cancelled: Package selection was cancelled." "$output_file"
assert_event_prefix_absent '^(sudo|pacman|pacman-conf|git|makepkg|aur) '
assert_state_log_absent

setup_case select-aur-scope
run_status_pty 1 'q\n' -S --select --aur select-scope
assert_event_at 1 "root search aur select-scope"
assert_not_contains "source=repository repository=core package=scope-repo" "$output_file"
assert_contains "source=AUR package=scope-aur PackageBase=scope-aur" "$output_file"
assert_contains "Cancelled: Package selection was cancelled." "$output_file"
assert_event_prefix_absent '^(sudo|pacman|pacman-conf|git|makepkg|aur) '
assert_state_log_absent

setup_case select-repository-range-needed-one-transaction
run_status_pty 0 '1-2\n' -S --select --repo --needed select-repository
repository_range_transaction='sudo pacman -S --needed -- core/repo-one extra/repo-two'
assert_event_at 1 "root search repository select-repository"
assert_event_at 2 "$repository_range_transaction"
assert_event_count 1 "$repository_range_transaction"
assert_event_pattern_count 1 '^sudo pacman -S '
assert_event_prefix_absent '^(pacman|pacman-conf|git|makepkg|aur) '

setup_case select-repository-group-one-transaction
run_status_pty 0 '@repo-group\n' -S --select --repo select-repository
repository_group_transaction='sudo pacman -S -- core/repo-one extra/repo-two'
assert_event_at 1 "root search repository select-repository"
assert_event_at 2 "$repository_group_transaction"
assert_event_count 1 "$repository_group_transaction"
assert_event_pattern_count 1 '^sudo pacman -S '
assert_event_prefix_absent '^(pacman|pacman-conf|git|makepkg|aur) '

setup_case select-mixed-repository-before-aur
run_status_pty 0 '1 2\n' --noedit --nodiff -S --select --needed select-mixed
mixed_repository_transaction='sudo pacman -S --needed -- extra/mixed-repo'
assert_event "root search all select-mixed"
assert_event "$mixed_repository_transaction"
assert_event_count 1 "$mixed_repository_transaction"
assert_event_before "$mixed_repository_transaction" "git clone https://aur.archlinux.org/mixed-aur.git mixed-aur"
assert_event_count 1 "git clone https://aur.archlinux.org/mixed-aur.git mixed-aur"
assert_event_count 1 "makepkg --packagelist"
assert_event_count 1 "makepkg -sc"
assert_event_pattern_count 1 '^sudo pacman -U --needed -- .*/mixed-aur-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_event_absent "sudo pacman -S --needed -- extra/mixed-repo mixed-aur"
assert_cache_entry_present mixed-aur

setup_case select-repository-failure-stops-aur-and-cache
export MOGUET_TEST_SUDO_MAIN_STATUS=42
run_status_pty 42 '1 2\n' --noedit --nodiff -S --select select-mixed
failed_selected_repository_transaction='sudo pacman -S -- extra/mixed-repo'
assert_event "$failed_selected_repository_transaction"
assert_event_count 1 "$failed_selected_repository_transaction"
assert_event_count 1 "pacman-conf --verbose RootDir DBPath"
assert_event_before "pacman-conf --verbose RootDir DBPath" "$failed_selected_repository_transaction"
assert_event_prefix_absent '^(git|makepkg) '
assert_event_pattern_count 0 '^sudo pacman -U '
assert_contains "The selected repository package transaction failed; selected AUR packages were not executed." "$output_file"
assert_cache_entry_absent mixed-aur

setup_case select-aur-failure-keeps-completed-repository-transaction
export MOGUET_TEST_MAKEPKG_EXIT_CODE=47
export MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE=0
run_status_pty 1 '1 2\n' --noedit --nodiff -S --select select-mixed
completed_selected_repository_transaction='sudo pacman -S -- extra/mixed-repo'
assert_event_count 1 "$completed_selected_repository_transaction"
assert_event_before "pacman-conf --verbose RootDir DBPath" "$completed_selected_repository_transaction"
assert_event_before "$completed_selected_repository_transaction" "makepkg -sc"
assert_event_count 1 "makepkg -sc"
assert_event_pattern_count 1 '^sudo pacman -S '
assert_event_pattern_count 0 '^sudo pacman -U '
assert_event_prefix_absent '^sudo pacman -R'
assert_contains "The repository package transaction completed before the AUR route failed; it was not rolled back." "$output_file"
assert_cache_entry_present mixed-aur

setup_case select-package-base-mismatch-before-mutation
run_status_pty 1 '1\n' --noedit --nodiff -S --select --aur select-package-base-mismatch
assert_event_at 1 "root search aur select-package-base-mismatch"
assert_contains "The PackageBase for selected AUR package mismatch-child changed from selected-base to resolved-base; rerun package selection." "$output_file"
assert_event_prefix_absent '^(sudo|pacman|pacman-conf|git|makepkg) '
assert_cache_entry_absent selected-base
assert_cache_entry_absent resolved-base
assert_state_log_absent

setup_case select-same-package-base-aggregates-one-build
MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES='split-suite|split-one|1.0-1
split-suite|split-two|1.0-1'
export MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES
run_status_pty 0 '1 2\n' --noedit --nodiff -S --select --aur select-same-base
assert_event "root search aur select-same-base"
assert_event_count 1 "git clone https://aur.archlinux.org/split-suite.git split-suite"
assert_event_count 1 "makepkg --packagelist"
assert_event_count 1 "makepkg -sc"
assert_event_pattern_count 1 '^sudo pacman -U -- .*/split-one-1\.0-1-x86_64\.pkg\.tar\.zst .*/split-two-1\.0-1-x86_64\.pkg\.tar\.zst$'
assert_cache_entry_present split-suite
assert_cache_entry_absent split-one
assert_cache_entry_absent split-two

setup_case select-repository-only-rejects-source-override
run_status_pty 1 '1\n' --noedit -S --select --repo select-repository
assert_event_at 1 "root search repository select-repository"
assert_contains "Source-build review and build-mode options require at least one selected AUR package." "$output_file"
assert_event_prefix_absent '^(sudo|pacman|pacman-conf|git|makepkg|aur) '
assert_cache_entry_absent repo-one
assert_state_log_absent

echo "sync command characterization tests: all checks passed"
