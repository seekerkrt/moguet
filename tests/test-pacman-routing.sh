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

mkdir -p \
    "$tmp_dir/cache" "$tmp_dir/config" "$tmp_dir/home" "$tmp_dir/state"
chmod 0700 "$tmp_dir/config"
command_log=$tmp_dir/commands.log
: > "$command_log"

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
export HOME=$tmp_dir/home
export XDG_CONFIG_HOME=$tmp_dir/config
export XDG_STATE_HOME=$tmp_dir/state
export XDG_CACHE_HOME=$tmp_dir/cache
export PATH=$repo_root/tests/stubs:/usr/bin:/bin
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
require_exact_test_command makepkg "$repo_root/tests/stubs/makepkg"
require_exact_test_command pacman "$repo_root/tests/stubs/pacman"
require_exact_test_command sudo "$repo_root/tests/stubs/sudo"
require_exact_test_command git "$repo_root/tests/stubs/git"
export MOGUET_TEST_AUR_RPC_BASE_URL=http://127.0.0.1:$port/rpc/
export MOGUET_TEST_COMMAND_LOG=$command_log
export MOGUET_TEST_PACMAN_EXIT_CODE=0
export MOGUET_TEST_SUDO_EXIT_CODE=0
unset MOGUET_TEST_PACMAN_QM_OUTPUT
unset MOGUET_TEST_PACMAN_REPO_PACKAGES
unset MOGUET_TEST_MAKEPKG_EXIT_CODE
unset MOGUET_TEST_GIT_CLONE_FIXTURE_DIR

run_ok() {
    output_file=$1
    shift
    : > "$command_log"
    "$test_binary" "$@" </dev/null > "$output_file" 2>&1
}

run_fail() {
    output_file=$1
    shift
    : > "$command_log"
    if ! validation_expect_status pacman-routing-business-failure 1 \
        "$output_file" "$output_file" "$test_binary" "$@" </dev/null; then
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

assert_command() {
    expected=$1
    if ! grep -Fx -- "$expected" "$command_log" >/dev/null; then
        echo "missing expected command: $expected" >&2
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

assert_only_two_commands() {
    first_expected=$1
    second_expected=$2
    first_actual=$(sed -n '1p' "$command_log")
    second_actual=$(sed -n '2p' "$command_log")
    if [ "$first_actual" != "$first_expected" ] || \
       [ "$second_actual" != "$second_expected" ] || \
       [ "$(wc -l < "$command_log")" -ne 2 ]; then
        echo "unexpected command sequence" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_no_sudo() {
    if grep -E '^sudo ' "$command_log" >/dev/null; then
        echo "unexpected sudo command" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_log_empty() {
    if [ -s "$command_log" ]; then
        echo "external command ran before refresh/info guard" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_ok "$tmp_dir/files-refresh.out" -Fy
assert_only_command "sudo pacman -Fy"
run_ok "$tmp_dir/files-force-refresh.out" -Fyy
assert_only_command "sudo pacman -Fyy"
run_ok "$tmp_dir/files-separated-refresh.out" -F -y
assert_only_command "sudo pacman -F -y"
run_ok "$tmp_dir/files-long-refresh.out" -F --refresh
assert_only_command "sudo pacman -F --refresh"

run_ok "$tmp_dir/info-combined-refresh.out" -Siy core/filesystem
assert_only_command "sudo pacman -Siy core/filesystem"
run_ok "$tmp_dir/info-separated-refresh.out" -Si -y core/filesystem
assert_only_command "sudo pacman -Si -y core/filesystem"
run_ok "$tmp_dir/info-long-refresh.out" -Si --refresh core/filesystem
assert_only_command "sudo pacman -Si --refresh core/filesystem"

run_ok "$tmp_dir/search-combined-refresh.out" -Ssy keyword
assert_command "sudo pacman -Ssy keyword"
run_ok "$tmp_dir/search-long-refresh.out" -Ss --refresh keyword
assert_command "sudo pacman -Ss --refresh keyword"

run_ok "$tmp_dir/files-query.out" -F usr/bin/foo
assert_command "pacman -F usr/bin/foo"
assert_no_sudo
run_ok "$tmp_dir/files-list.out" -Fl filesystem
assert_command "pacman -Fl filesystem"
assert_no_sudo
run_ok "$tmp_dir/files-option-value.out" -F --config -y usr/bin/foo
assert_command "pacman -F --config -y usr/bin/foo"
assert_no_sudo
run_ok "$tmp_dir/search.out" -Ss keyword
assert_command "pacman -Ss keyword"
assert_no_sudo
# Auto info now classifies from typed repository metadata before delegation.
repository_metadata_state=$tmp_dir/repository-metadata.state
printf 'core filesystem 1 1\n' > "$repository_metadata_state"
export MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE=$repository_metadata_state
export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
run_ok "$tmp_dir/info.out" -Si filesystem
assert_command "pacman -Si filesystem"
assert_no_sudo

option_index=0
for moguet_option in \
    --edit --noedit --diff --nodiff \
    --build-mode=normal --build-mode=rebuild --build-mode=clean \
    --rebuild --cleanbuild --rmdeps; do
    option_index=$((option_index + 1))
    run_ok "$tmp_dir/moguet-option-$option_index.out" \
        "$moguet_option" -Q filesystem
    assert_only_command "pacman -Q filesystem"
done

# custom upgradeと明示RepoOnly routingを保ち、upgrade-aurはpacmanへ委譲しない。
run_ok "$tmp_dir/custom-upgrade.out" upgrade
assert_only_two_commands \
    "pacman-conf --verbose RootDir DBPath" \
    "sudo pacman -Syu"
run_ok "$tmp_dir/repository-system-upgrade.out" -Syu --repo
assert_only_command "sudo pacman -Syu"
run_fail "$tmp_dir/upgrade-aur-target.out" upgrade-aur unexpected-target
assert_contains "Operation upgrade-aur does not accept target operands." "$tmp_dir/upgrade-aur-target.out"
assert_log_empty

run_fail "$tmp_dir/aur-refresh.out" -Siy risk-root
assert_contains "Cannot combine pacman refresh with AUR info fallback" "$tmp_dir/aur-refresh.out"
assert_log_empty
run_fail "$tmp_dir/aur-long-refresh.out" -Si --refresh risk-root
assert_contains "Cannot combine pacman refresh with AUR info fallback" "$tmp_dir/aur-long-refresh.out"
assert_log_empty
run_fail "$tmp_dir/mixed-refresh.out" -Siy core/filesystem risk-root
assert_contains "Cannot combine pacman refresh with AUR info fallback" "$tmp_dir/mixed-refresh.out"
assert_log_empty
run_fail "$tmp_dir/unqualified-official-refresh.out" -Siy filesystem
assert_contains "Use a repository-qualified target" "$tmp_dir/unqualified-official-refresh.out"
assert_log_empty

echo "pacman routing integration tests: all checks passed"
