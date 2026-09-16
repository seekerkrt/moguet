#!/bin/sh
set -eu

LANG=C
LC_ALL=C
export LANG LC_ALL
unset LANGUAGE

test_binary=$1
repository_test_binary=$2
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
MOGUET_TEST_REPOSITORY_ROOT=$repo_root
export MOGUET_TEST_REPOSITORY_ROOT
. "$repo_root/tests/test-command-safety.sh"
. "$repo_root/scripts/validation-status.sh"

tmp_dir=$(mktemp -d)
server_pid=
mutation_sentinel_pid=

cleanup() {
    if [ -n "$mutation_sentinel_pid" ]; then
        kill "$mutation_sentinel_pid" 2>/dev/null || true
        wait "$mutation_sentinel_pid" 2>/dev/null || true
    fi
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

port_file=$tmp_dir/port
python3 "$repo_root/tests/aur_rpc_fixture_server.py" \
    "$repo_root/tests/fixtures/aur-rpc-conflicts-replaces.json" \
    "$port_file" &
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
PATH=$repo_root/tests/stubs:/usr/bin:/bin
export PATH
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
require_exact_test_command makepkg "$repo_root/tests/stubs/makepkg"
require_exact_test_command pacman "$repo_root/tests/stubs/pacman"
require_exact_test_command sudo "$repo_root/tests/stubs/sudo"
require_exact_test_command git "$repo_root/tests/stubs/git"
MOGUET_TEST_AUR_RPC_BASE_URL=http://127.0.0.1:$port/rpc/
export MOGUET_TEST_AUR_RPC_BASE_URL

snapshot_protected_storage_raw() {
    for snapshot_tree in \
        "$XDG_CONFIG_HOME" \
        "$XDG_STATE_HOME" \
        "$XDG_CACHE_HOME" \
        "$TMPDIR" \
        "$case_work_dir"
    do
        printf 'tree=%s\n' "$snapshot_tree" || return $?
        find "$snapshot_tree" -printf '%p|%y\n' || return $?
        find "$snapshot_tree" -exec \
            stat --format='%n|%F|%d|%i|%u|%g|%a|%s|%Y|%Z' -- {} + || return $?
        find "$snapshot_tree" -type f -exec sha256sum -- {} + || return $?
    done
}

snapshot_protected_storage() {
    snapshot_destination=$1
    if validation_capture_sorted_output \
        "$snapshot_destination.raw" "$snapshot_destination" \
        snapshot_protected_storage_raw; then
        return 0
    else
        snapshot_status=$?
    fi
    printf 'protected storage snapshot producer failed with status %s; raw=%s\n' \
        "$snapshot_status" "$snapshot_destination.raw" >&2
    exit 1
}

prepare_canary_tree() {
    protected_tree=$1
    protected_label=$2
    mkdir -p "$protected_tree/moguet/canary-directory"
    printf '%s\n' "$protected_label root canary" > \
        "$protected_tree/root-canary"
    printf '%s\n' "$protected_label Moguet canary" > \
        "$protected_tree/moguet/canary-directory/content"
    chmod 0600 \
        "$protected_tree/root-canary" \
        "$protected_tree/moguet/canary-directory/content"
    chmod 0700 \
        "$protected_tree/moguet/canary-directory" \
        "$protected_tree/moguet" \
        "$protected_tree"
}

setup_case() {
    case_name=$1
    case_dir=$tmp_dir/cases/$case_name
    command_log=$case_dir/commands.log
    output_file=$case_dir/output
    mkdir -p \
        "$case_dir/home" \
        "$case_dir/xdg-config" \
        "$case_dir/xdg-state" \
        "$case_dir/xdg-cache" \
        "$case_dir/tmp" \
        "$case_dir/work"
    : > "$command_log"

    HOME=$case_dir/home
    XDG_CONFIG_HOME=$case_dir/xdg-config
    XDG_STATE_HOME=$case_dir/xdg-state
    XDG_CACHE_HOME=$case_dir/xdg-cache
    TMPDIR=$case_dir/tmp
    case_work_dir=$case_dir/work
    MOGUET_TEST_COMMAND_LOG=$command_log
    export HOME XDG_CONFIG_HOME XDG_STATE_HOME XDG_CACHE_HOME TMPDIR
    export MOGUET_TEST_COMMAND_LOG

    # Every persistent or temporary boundary is pre-existing and
    # content-addressed. A per-case filesystem event sentinel additionally
    # retains create-then-cleanup attempts that leave the final tree unchanged.
    prepare_canary_tree "$XDG_CONFIG_HOME" config
    prepare_canary_tree "$XDG_STATE_HOME" state
    prepare_canary_tree "$XDG_CACHE_HOME" cache
    prepare_canary_tree "$TMPDIR" temporary
    prepare_canary_tree "$case_work_dir" working
    protected_before_snapshot=$case_dir/protected-before
    protected_after_snapshot=$case_dir/protected-after
    snapshot_protected_storage "$protected_before_snapshot"

    # Mutation-capable stubs leave a marker and return a status outside the
    # dry-run 0/1 contract. The pacman stub accepts only read-only queries and
    # logs before rejecting transaction argv.
    MOGUET_TEST_PACMAN_EXIT_CODE=0
    MOGUET_TEST_SUDO_EXIT_CODE=97
    MOGUET_TEST_MAKEPKG_EXIT_CODE=97
    MOGUET_TEST_GIT_CLONE_EXIT_CODE=97
    MOGUET_TEST_PACMAN_REPO_PACKAGES=official-only
    MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
    VISUAL=sudo
    EDITOR=sudo
    export MOGUET_TEST_PACMAN_EXIT_CODE MOGUET_TEST_SUDO_EXIT_CODE
    export MOGUET_TEST_MAKEPKG_EXIT_CODE MOGUET_TEST_GIT_CLONE_EXIT_CODE
    export MOGUET_TEST_PACMAN_REPO_PACKAGES
    export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST VISUAL EDITOR

    unset MOGUET_TEST_PACMAN_QM_OUTPUT
    unset MOGUET_TEST_PACMAN_Q_OUTPUT
    unset MOGUET_TEST_PACMAN_Q_OUTPUT_FILE
    unset MOGUET_TEST_PACKAGE_METADATA_STATE_FILE
    unset MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG
    unset MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE
    unset MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE
    unset MOGUET_TEST_MAKEPKG_PRINTSRCINFO_EXIT_CODE
    unset MOGUET_TEST_MAKEPKG_SRCINFO_OUTPUT_FILE
    unset MOGUET_TEST_VERCMP_OUTPUT
    unset PKGDEST
}

set_foreign_inventory() {
    inventory_state=$case_dir/foreign-inventory.state
    printf '%s\n' "$1" > "$inventory_state"
    MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$inventory_state
    export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE
}

set_empty_foreign_inventory() {
    inventory_state=$case_dir/foreign-inventory.state
    : > "$inventory_state"
    MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$inventory_state
    export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE
}

assert_text_occurrence_count() {
    expected_count=$1
    text=$2
    checked_file=$3
    actual_count=$(validation_grep_count -Fc -- "$text" "$checked_file")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "expected $expected_count occurrences of '$text', got $actual_count" >&2
        sed -n '1,240p' "$checked_file" >&2
        exit 1
    fi
}

assert_text_before() {
    first_text=$1
    second_text=$2
    checked_file=$3
    first_line=$(grep -n -F -m1 -- "$first_text" "$checked_file" |
        sed -n '1s/:.*//p')
    second_line=$(grep -n -F -m1 -- "$second_text" "$checked_file" |
        sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] ||
       [ "$first_line" -ge "$second_line" ]; then
        echo "expected '$first_text' before '$second_text'" >&2
        sed -n '1,240p' "$checked_file" >&2
        exit 1
    fi
}

start_mutation_sentinel() {
    mutation_sentinel_ready=$case_dir/mutation-sentinel.ready
    mutation_sentinel_stop=$case_dir/mutation-sentinel.stop
    mutation_sentinel_events=$case_dir/mutation-sentinel.events
    mutation_sentinel_error=$case_dir/mutation-sentinel.stderr
    python3 "$repo_root/tests/fs_mutation_sentinel.py" \
        "$mutation_sentinel_ready" \
        "$mutation_sentinel_stop" \
        "$mutation_sentinel_events" \
        "$XDG_CONFIG_HOME" \
        "$XDG_STATE_HOME" \
        "$XDG_CACHE_HOME" \
        "$TMPDIR" \
        "$case_work_dir" \
        "$@" \
        2> "$mutation_sentinel_error" &
    mutation_sentinel_pid=$!

    mutation_sentinel_attempt=0
    while [ ! -s "$mutation_sentinel_ready" ]; do
        if ! kill -0 "$mutation_sentinel_pid" 2>/dev/null; then
            wait "$mutation_sentinel_pid" 2>/dev/null || true
            mutation_sentinel_pid=
            echo "filesystem mutation sentinel did not start" >&2
            cat "$mutation_sentinel_error" >&2
            exit 1
        fi
        mutation_sentinel_attempt=$((mutation_sentinel_attempt + 1))
        if [ "$mutation_sentinel_attempt" -gt 100 ]; then
            echo "filesystem mutation sentinel did not become ready" >&2
            cat "$mutation_sentinel_error" >&2
            exit 1
        fi
        sleep 0.01
    done
}

finish_mutation_sentinel() {
    mutation_sentinel_expectation=${1:-clean}
    : > "$mutation_sentinel_stop"
    if ! wait "$mutation_sentinel_pid"; then
        mutation_sentinel_pid=
        echo "filesystem mutation sentinel failed" >&2
        cat "$mutation_sentinel_error" >&2
        exit 1
    fi
    mutation_sentinel_pid=
    case $mutation_sentinel_expectation in
        clean)
            if [ -s "$mutation_sentinel_events" ]; then
                echo "dry-run crossed a filesystem mutation boundary" >&2
                cat "$mutation_sentinel_events" >&2
                exit 1
            fi
            ;;
        mutated)
            if [ ! -s "$mutation_sentinel_events" ]; then
                echo "filesystem mutation sentinel missed its transient canary" >&2
                exit 1
            fi
            ;;
        *)
            echo "invalid filesystem mutation sentinel expectation" >&2
            exit 1
            ;;
    esac
}

assert_protected_storage_unchanged() {
    snapshot_protected_storage "$protected_after_snapshot"
    finish_mutation_sentinel
    if ! cmp -s \
        "$protected_before_snapshot" "$protected_after_snapshot"
    then
        echo "dry-run changed protected config, state, cache, temporary, or working storage" >&2
        diff -u \
            "$protected_before_snapshot" "$protected_after_snapshot" \
            >&2 || true
        exit 1
    fi
}

assert_read_only_commands() {
    while IFS= read -r command; do
        case $command in
            '') ;;
            'pacman-conf --verbose RootDir DBPath'|\
            'pacman-conf --repo-list'|\
            'pacman -Si clean-root'|\
            'pacman -Qm'|\
            'vercmp 1.0-1 0.9-1'|\
            'vercmp 1.0-1 1.0-1'|\
            'alpm initialize'|\
            'alpm sync-register core'|\
            'alpm sync-valid core'|\
            'alpm sync-cache core'|\
            'alpm sync-query core/clean-root'|\
            'alpm sync-query core/foo'|\
            'alpm sync-query core/foo-git'|\
            'alpm sync-query core/required-devel-git'|\
            'alpm sync-query core/required-devel-root'|\
            'alpm sync-query core/repository-root'|\
            'alpm sync-query core/moguet-fixture-rc-one-git'|\
            'alpm sync-query core/moguet-fixture-rc-two-git'|\
            'alpm sync-query core/moguet-fixture-rc-three-git'|\
            'alpm release') ;;
            *)
                echo "dry-run crossed a forbidden or unapproved process boundary" >&2
                echo "$command" >&2
                cat "$command_log" >&2
                exit 1
                ;;
        esac
    done < "$command_log"
}

assert_expected_observation() {
    observation_expected_status=$1
    observation_expected_root_count=$2
    observation_expected_identity=$3
    observation_exit_status=$4
    observation_context=$5

    case $observation_expected_status in
        Ready|NoOp)
            observation_expected_exit_status=0
            ;;
        Blocked)
            observation_expected_exit_status=1
            ;;
        *)
            echo "invalid expected dry-run status: $observation_expected_status" >&2
            exit 1
            ;;
    esac
    if [ "$observation_exit_status" -ne "$observation_expected_exit_status" ]; then
        echo "dry-run returned $observation_exit_status instead of $observation_expected_exit_status: $observation_context" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
    if ! grep -Fx -- \
        "  Status: $observation_expected_status" "$output_file" >/dev/null
    then
        echo "dry-run did not render expected $observation_expected_status status: $observation_context" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
    observation_root_count=$(
        awk '/^  [0-9]+\. Identity: / { count++ }
             END { print count + 0 }' "$output_file"
    )
    if [ "$observation_root_count" -ne "$observation_expected_root_count" ]; then
        echo "dry-run rendered $observation_root_count roots instead of $observation_expected_root_count: $observation_context" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
    if ! grep -F -- "$observation_expected_identity" \
        "$output_file" >/dev/null
    then
        echo "dry-run lost its expected typed identity: $observation_context" >&2
        echo "expected: $observation_expected_identity" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
}

run_supported() {
    case_name=$1
    expected_status=$2
    expected_root_count=$3
    expected_identity=$4
    shift 4
    setup_case "$case_name"
    : > "$command_log"
    start_mutation_sentinel
    if (cd "$case_work_dir" && "$test_binary" "$@") \
        </dev/null > "$output_file" 2>&1
    then
        status=0
    else
        status=$?
    fi
    if ! grep -F -- "Unified plan:" "$output_file" >/dev/null; then
        echo "supported dry-run did not render a unified observation: $*" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
    assert_expected_observation \
        "$expected_status" "$expected_root_count" "$expected_identity" \
        "$status" "$*"
    assert_protected_storage_unchanged
    assert_read_only_commands
}

snapshot_local_tree() {
    source_tree=$1
    destination=$2
    if validation_capture_sorted_output "$destination.raw" "$destination" \
        snapshot_local_tree_raw "$source_tree"; then
        return 0
    else
        snapshot_status=$?
    fi
    printf 'local tree snapshot producer failed with status %s; raw=%s\n' \
        "$snapshot_status" "$destination.raw" >&2
    exit 1
}

snapshot_local_tree_raw() {
    source_tree=$1
    {
        find "$source_tree" -mindepth 1 -printf '%P\n' || return $?
        find "$source_tree" -mindepth 1 -exec \
            stat --format='%n|%F|%d|%i|%u|%g|%a|%s|%Y|%Z' -- {} + || return $?
        find "$source_tree" -type f -exec sha256sum -- {} + || return $?
    }
}

assert_no_raw_local_terminal_payload() {
    local_output_hex_raw=$case_dir/local-output.hex.raw
    if validation_capture_output "$local_output_hex_raw" \
        od -An -v -tx1 "$output_file"; then
        :
    else
        hex_status=$?
        printf 'terminal payload producer failed with status %s; raw=%s\n' \
            "$hex_status" "$local_output_hex_raw" >&2
        exit 1
    fi
    if local_output_hex=$(tr '\n' ' ' <"$local_output_hex_raw"); then
        :
    else
        hex_status=$?
        printf 'terminal payload normalization failed with status %s\n' \
            "$hex_status" >&2
        exit 1
    fi
    for local_raw_sequence in \
        '1b' \
        '07' \
        'c2 85' \
        'e2 80 a8'
    do
        case " $local_output_hex " in
            *" $local_raw_sequence "*)
                echo "local metadata blocker reflected raw terminal bytes: $local_raw_sequence" >&2
                sed -n '1,240p' "$output_file" >&2
                exit 1
                ;;
        esac
    done
    if grep -E '^M4-RAW-NEXT-LINE' "$output_file" >/dev/null; then
        echo "local metadata blocker reflected a raw newline" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
}

# Prove that the event seam retains a create-then-cleanup mutation even though
# a final filesystem snapshot would be identical.
setup_case mutation-sentinel-self-check
start_mutation_sentinel
printf '%s\n' transient > "$TMPDIR/transient-canary"
rm "$TMPDIR/transient-canary"
finish_mutation_sentinel mutated

# Both supported sync forms share the same production classifier, while the
# remaining route cases exercise each route-specific authority adapter.
run_supported \
    sync-install Ready 1 \
    "Identity: AUR/clean-root (PackageBase: clean-root)" \
    --dry-run -S clean-root

for sync_operation in -Syu -Su; do
    setup_case "sync-system-aur-update-$sync_operation"
    set_foreign_inventory 'clean-root 0.9-1 explicit'
    MOGUET_TEST_VERCMP_OUTPUT=1
    MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
    export MOGUET_TEST_VERCMP_OUTPUT MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG
    : > "$command_log"
    start_mutation_sentinel
    if (cd "$case_work_dir" &&
            "$repository_test_binary" --dry-run "$sync_operation") \
        </dev/null > "$output_file" 2>&1
    then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne 0 ]; then
        echo "combined system/AUR dry-run returned $status" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
    grep -Fx -- "System + normal AUR update plan:" "$output_file" >/dev/null
    grep -Fx -- "  Status: Ready" "$output_file" >/dev/null
    grep -Fx -- "Repository system phase:" "$output_file" >/dev/null
    grep -Fx -- "Current-state normal AUR phase:" "$output_file" >/dev/null
    grep -Fx -- "  AUR assessment is based on the current installed state." "$output_file" >/dev/null
    grep -Fx -- "  Actual execution re-evaluates AUR state after the repository upgrade succeeds." "$output_file" >/dev/null
    grep -Fx -- "  The repository system transaction and later normal AUR transactions are separate intents." "$output_file" >/dev/null
    assert_text_before \
        "Repository system phase:" \
        "Current-state normal AUR phase:" \
        "$output_file"
    assert_protected_storage_unchanged
    assert_read_only_commands

    setup_case "sync-system-aur-no-updates-not-whole-noop-$sync_operation"
    set_foreign_inventory 'clean-root 1.0-1 explicit'
    MOGUET_TEST_VERCMP_OUTPUT=0
    MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
    export MOGUET_TEST_VERCMP_OUTPUT MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG
    : > "$command_log"
    start_mutation_sentinel
    if (cd "$case_work_dir" &&
            "$repository_test_binary" --dry-run "$sync_operation") \
        </dev/null > "$output_file" 2>&1
    then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne 0 ]; then
        echo "combined no-update dry-run returned $status" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
    grep -Fx -- "System + normal AUR update plan:" "$output_file" >/dev/null
    grep -Fx -- "  Status: Ready" "$output_file" >/dev/null
    grep -Fx -- "Current-state normal AUR phase:" "$output_file" >/dev/null
    assert_text_occurrence_count 1 "  Status: NoOp" "$output_file"
    assert_protected_storage_unchanged
    assert_read_only_commands
done

setup_case sync-system-aur-requires-check
set_foreign_inventory 'moguet-fixture-rc-one-git 1.0-1 explicit
moguet-fixture-rc-two-git 1.0-1 explicit
moguet-fixture-rc-three-git 1.0-1 explicit'
MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG
: > "$command_log"
start_mutation_sentinel
if (cd "$case_work_dir" &&
        "$repository_test_binary" --dry-run --noconfirm -Syu) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
if [ "$status" -ne 0 ]; then
    echo "combined RequiresCheck dry-run returned $status" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
grep -Fx -- "System + normal AUR update plan:" "$output_file" >/dev/null
grep -Fx -- "  Status: Ready" "$output_file" >/dev/null
assert_text_occurrence_count 1 "  Status: NoOp" "$output_file"
assert_text_occurrence_count 1 "Attention-required details:" "$output_file"
assert_text_occurrence_count 3 \
    "skipped: devel update requires check: suffix candidate only; not automatically updated because authoritative build provenance is unavailable" \
    "$output_file"
grep -F -- "moguet-fixture-rc-one-git (PackageBase: moguet-fixture-rc-one-git)" "$output_file" >/dev/null
grep -F -- "moguet-fixture-rc-two-git (PackageBase: moguet-fixture-rc-two-git)" "$output_file" >/dev/null
grep -F -- "moguet-fixture-rc-three-git (PackageBase: moguet-fixture-rc-three-git)" "$output_file" >/dev/null
if grep -F -- "  Status: Blocked" "$output_file" >/dev/null; then
    echo "independent RequiresCheck blocked the combined dry-run" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
grep -Fx -- "  AUR assessment is based on the current installed state." "$output_file" >/dev/null
assert_protected_storage_unchanged
assert_read_only_commands

setup_case sync-system-aur-update-plus-requires-check
set_foreign_inventory 'clean-root 0.9-1 explicit
foo-git 1.0-1 explicit
foo 1.0-1 explicit'
MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG
: > "$command_log"
start_mutation_sentinel
if (cd "$case_work_dir" &&
        "$repository_test_binary" --dry-run --noconfirm -Syu) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
if [ "$status" -ne 0 ]; then
    echo "combined mixed RequiresCheck dry-run returned $status" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
grep -Fx -- "System + normal AUR update plan:" "$output_file" >/dev/null
assert_text_occurrence_count 3 "  Status: Ready" "$output_file"
assert_text_occurrence_count 1 "Attention-required details:" "$output_file"
assert_text_occurrence_count 1 \
    "foo-git (PackageBase: foo-git): skipped: devel update requires check: suffix candidate only; not automatically updated because authoritative build provenance is unavailable" \
    "$output_file"
assert_protected_storage_unchanged
assert_read_only_commands

setup_case sync-system-aur-required-requires-check
set_foreign_inventory 'required-devel-root 0.9-1 explicit
required-devel-git 1.0-1 dependency'
MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG
: > "$command_log"
start_mutation_sentinel
if (cd "$case_work_dir" &&
        "$repository_test_binary" --dry-run --noconfirm -Syu) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
if [ "$status" -ne 1 ]; then
    echo "combined required RequiresCheck dry-run returned $status" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
if ! grep -Fx -- "System + normal AUR update plan:" "$output_file" >/dev/null; then
    echo "required RequiresCheck dry-run lost the combined plan" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_text_occurrence_count 2 "  Status: Blocked" "$output_file"
grep -F -- "AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck" "$output_file" >/dev/null
if grep -F -- "Attention-required details:" "$output_file" >/dev/null; then
    echo "required RequiresCheck was downgraded to attention" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_protected_storage_unchanged
assert_read_only_commands

for sync_operation in -Syu -Su; do
    setup_case "sync-system-repository-only-$sync_operation"
    MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
    export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG
    : > "$command_log"
    start_mutation_sentinel
    if (cd "$case_work_dir" &&
            "$repository_test_binary" --dry-run "$sync_operation" --repo \
                --config custom.conf) \
        </dev/null > "$output_file" 2>&1
    then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne 0 ]; then
        echo "repository-only system dry-run returned $status" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
    grep -Fx -- "Repository system update plan:" "$output_file" >/dev/null
    grep -Fx -- "  Status: Ready" "$output_file" >/dev/null
    grep -Fx -- "Repository update phase:" "$output_file" >/dev/null
    grep -Fx -- "Repository system phase:" "$output_file" >/dev/null
    grep -F -- "Repository system transaction intent" "$output_file" >/dev/null
    # A successful RepoOnly projection with this value-taking tail preserves the
    # delegated --config argument form without executing a repository query.
    if grep -F -- "System + normal AUR update plan:" "$output_file" >/dev/null ||
       grep -F -- "Combined update phases:" "$output_file" >/dev/null ||
       grep -F -- "Current-state normal AUR phase:" "$output_file" >/dev/null ||
       grep -F -- "AUR assessment is based on" "$output_file" >/dev/null ||
       grep -F -- "Attention-required details:" "$output_file" >/dev/null ||
       grep -F -- "later normal AUR" "$output_file" >/dev/null
    then
        echo "repository-only dry-run exposed AUR observation semantics" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
    if [ -s "$command_log" ]; then
        echo "repository-only dry-run performed a read-only AUR/repository query" >&2
        cat "$command_log" >&2
        exit 1
    fi
    assert_protected_storage_unchanged
    assert_read_only_commands
done

run_supported \
    fetch Ready 1 \
    "Identity: AUR/clean-root (PackageBase: clean-root)" \
    fetch clean-root --dry-run
run_supported \
    remote-build Ready 1 \
    "Identity: AUR/clean-root (PackageBase: clean-root)" \
    --dry-run build clean-root --dry-run

# Issue #406 Slice 3: strict repository metadata selects the standalone
# PackageBase-set projection. Ready therefore proves the route-specific Set
# canary accepted it, while the sentinel proves observation stayed read-only.
setup_case remote-repository-build
repository_metadata_state=$case_dir/repository-metadata-state
printf 'core repository-root 1 1\n' > "$repository_metadata_state"
export MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE=$repository_metadata_state
export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
MOGUET_TEST_PACMAN_REPO_PACKAGES=repository-root
export MOGUET_TEST_PACMAN_REPO_PACKAGES
: > "$command_log"
start_mutation_sentinel
if (cd "$case_work_dir" &&
        "$repository_test_binary" --dry-run build repository-root) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
if ! grep -F -- "Unified plan:" "$output_file" >/dev/null; then
    echo "standalone repository dry-run did not render a unified observation" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_expected_observation \
    Ready 1 \
    "repository source key repository:repository-root (requested package: repository-root; checkout PackageBase: repository-root)" \
    "$status" "standalone repository PackageBase-set build"
assert_protected_storage_unchanged
assert_read_only_commands

setup_case local-build
local_source=$case_dir/local-source
cp -a "$repo_root/tests/fixtures/unified-plan-local-blocked" "$local_source"
touch "$local_source/.SRCINFO"
before_snapshot=$case_dir/local-before
after_snapshot=$case_dir/local-after
snapshot_local_tree "$local_source" "$before_snapshot"
start_mutation_sentinel "$local_source"
if (cd "$case_work_dir" &&
        "$test_binary" build --local "$local_source" --dry-run) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
if ! grep -F -- "Unified plan:" "$output_file" >/dev/null; then
    echo "local dry-run did not render a unified observation" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_expected_observation \
    Ready 1 \
    "Request: unified-plan-local-blocked (invocation index: 0)" \
    "$status" "local build with reusable metadata"
if ! grep -F -- "     Source: local" "$output_file" >/dev/null; then
    echo "local dry-run lost its typed source kind" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
snapshot_local_tree "$local_source" "$after_snapshot"
if ! cmp -s "$before_snapshot" "$after_snapshot"; then
    echo "local dry-run changed source file list, content, or identity" >&2
    diff -u "$before_snapshot" "$after_snapshot" >&2 || true
    exit 1
fi
assert_protected_storage_unchanged
assert_read_only_commands

setup_case local-build-relation-no-match
local_source=$case_dir/local-source
cp -a "$repo_root/tests/fixtures/unified-plan-local-blocked" "$local_source"
touch "$local_source/.SRCINFO"
printf '\tconflicts = absent-local-component\n' >> "$local_source/.SRCINFO"
before_snapshot=$case_dir/local-before
after_snapshot=$case_dir/local-after
snapshot_local_tree "$local_source" "$before_snapshot"
start_mutation_sentinel "$local_source"
if (cd "$case_work_dir" &&
        "$repository_test_binary" build --local "$local_source" --dry-run) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
assert_expected_observation \
    Ready 1 \
    "Request: unified-plan-local-blocked (invocation index: 0)" \
    "$status" "local build with a complete no-match relation assessment"
if ! grep -F -- "     Source: local" "$output_file" >/dev/null ||
   ! grep -F -- \
       "Confirmed no matching current or planned target" \
       "$output_file" >/dev/null ||
   ! grep -F -- "source: local source" "$output_file" >/dev/null ||
   ! grep -F -- "this relation does not block build/install" \
       "$output_file" >/dev/null
then
    echo "local dry-run lost its typed package-relation result" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
snapshot_local_tree "$local_source" "$after_snapshot"
if ! cmp -s "$before_snapshot" "$after_snapshot"; then
    echo "local relation dry-run changed source file list, content, or identity" >&2
    diff -u "$before_snapshot" "$after_snapshot" >&2 || true
    exit 1
fi
assert_protected_storage_unchanged
assert_read_only_commands

setup_case local-build-metadata-required
unsafe_local_component=$(printf \
    'local-source-before\nM4-RAW-NEXT-LINE\\literal\033]0;LOCAL-OSC\007\302\205\342\200\250')
local_source=$case_dir/$unsafe_local_component
cp -a "$repo_root/tests/fixtures/unified-plan-local-blocked" "$local_source"
rm "$local_source/.SRCINFO"
before_snapshot=$case_dir/local-before
after_snapshot=$case_dir/local-after
snapshot_local_tree "$local_source" "$before_snapshot"
start_mutation_sentinel "$local_source"
if (cd "$case_work_dir" &&
        "$test_binary" build --local "$local_source" --dry-run) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
assert_expected_observation \
    Blocked 0 "local metadata evaluation required" \
    "$status" "local build requiring metadata evaluation"
expected_escaped_local_path='local-source-before\x0AM4-RAW-NEXT-LINE\x5Cliteral\x1B]0;LOCAL-OSC\x07\xC2\x85\xE2\x80\xA8'
if ! grep -F -- "$expected_escaped_local_path" \
    "$output_file" >/dev/null
then
    echo "local metadata blocker did not terminal-escape its source path" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_no_raw_local_terminal_payload
snapshot_local_tree "$local_source" "$after_snapshot"
if ! cmp -s "$before_snapshot" "$after_snapshot"; then
    echo "blocked local dry-run changed source file list, content, or identity" >&2
    diff -u "$before_snapshot" "$after_snapshot" >&2 || true
    exit 1
fi
assert_protected_storage_unchanged
assert_read_only_commands

run_supported \
    upgrade Ready 0 "     - system upgrade" \
    upgrade --dry-run

# No-op actual AUR update retains the pre-log zero-mutation boundary.
setup_case upgrade-aur-noop
set_empty_foreign_inventory
: > "$command_log"
start_mutation_sentinel
if (cd "$case_work_dir" &&
        "$repository_test_binary" --noconfirm upgrade-aur) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
if [ "$status" -ne 0 ] ||
   ! grep -F -- "AUR update: no updates" "$output_file" >/dev/null
then
    echo "actual no-op AUR update changed its result" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_protected_storage_unchanged
assert_read_only_commands

# A non-empty no-op still owns query diagnostics, but replay remains console
# only and does not initialize persistent logging.
setup_case upgrade-aur-current-noop
set_foreign_inventory 'clean-root 1.0-1 explicit'
MOGUET_TEST_VERCMP_OUTPUT=0
export MOGUET_TEST_VERCMP_OUTPUT
: > "$command_log"
start_mutation_sentinel
if (cd "$case_work_dir" &&
        "$repository_test_binary" --noconfirm upgrade-aur) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
if [ "$status" -ne 0 ] ||
   ! grep -F -- "AUR update: no updates" "$output_file" >/dev/null
then
    echo "actual current-package AUR no-op changed its result" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_text_occurrence_count 1 \
    'Checking AUR updates for 1 foreign packages...' "$output_file"
assert_text_occurrence_count 1 \
    'Fetching AUR info for packages 1-1 of 1...' "$output_file"
assert_protected_storage_unchanged
assert_read_only_commands

# A normal authoritative update crosses the execution capability boundary,
# but its external mutation is stopped by the Git test double. Captured
# pre-log diagnostics must follow Started in both console and persistent log.
setup_case upgrade-aur-normal-executable-log
set_foreign_inventory 'clean-root 0.9-1 explicit'
MOGUET_TEST_VERCMP_OUTPUT=1
export MOGUET_TEST_VERCMP_OUTPUT
: > "$command_log"
if (cd "$case_work_dir" &&
        "$repository_test_binary" --noedit --nodiff --noconfirm upgrade-aur) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
if [ "$status" -ne 1 ]; then
    echo "normal executable AUR fixture returned $status instead of 1" >&2
    sed -n '1,240p' "$output_file" >&2
    cat "$command_log" >&2
    exit 1
fi
started_text='Started Moguet v'
checking_text='Checking AUR updates for 1 foreign packages...'
fetching_text='Fetching AUR info for packages 1-1 of 1...'
assert_text_before "$started_text" "$checking_text" "$output_file"
assert_text_before "$checking_text" "$fetching_text" "$output_file"
assert_text_occurrence_count 1 "$started_text" "$output_file"
assert_text_occurrence_count 1 "$checking_text" "$output_file"
assert_text_occurrence_count 1 "$fetching_text" "$output_file"

state_log_file=$XDG_STATE_HOME/moguet/moguet.log
if [ ! -f "$state_log_file" ]; then
    echo "normal executable AUR update did not create its state log" >&2
    exit 1
fi
assert_text_before '[INFO] Started Moguet v' \
    "[INFO] $checking_text" "$state_log_file"
assert_text_before "[INFO] $checking_text" \
    "[INFO] $fetching_text" "$state_log_file"
assert_text_before "[INFO] $fetching_text" '[EXEC] git clone ' \
    "$state_log_file"
assert_text_occurrence_count 1 '[INFO] Started Moguet v' "$state_log_file"
assert_text_occurrence_count 1 "[INFO] $checking_text" "$state_log_file"
assert_text_occurrence_count 1 "[INFO] $fetching_text" "$state_log_file"
assert_text_occurrence_count 1 '[EXEC] git clone ' "$state_log_file"
if ! grep -F -- \
        'git clone https://aur.archlinux.org/clean-root.git clean-root' \
        "$command_log" >/dev/null ||
   grep -E '^(makepkg|sudo)( |$)' "$command_log" >/dev/null
then
    echo "normal executable fixture did not stop at its Git test double" >&2
    cat "$command_log" >&2
    exit 1
fi

# The same production query/preflight path must block actual and dry-run AUR
# execution without a transient filesystem mutation. non-TTY/--noconfirm does
# not turn RequiresCheck into a rebuild approval.
setup_case upgrade-aur-devel-requires-check
set_foreign_inventory 'foo-git 1.0-1 explicit'
MOGUET_TEST_VERCMP_OUTPUT=0
export MOGUET_TEST_VERCMP_OUTPUT
: > "$command_log"
start_mutation_sentinel
if (cd "$case_work_dir" &&
        "$repository_test_binary" --noconfirm upgrade-aur) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
if [ "$status" -ne 1 ]; then
    echo "actual RequiresCheck returned $status instead of 1" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
if ! grep -F -- "AUR update: blocked before execution" \
    "$output_file" >/dev/null ||
   ! grep -F -- \
       "foo-git: devel update requires check: suffix candidate only" \
       "$output_file" >/dev/null ||
   ! grep -F -- \
       "authoritative build provenance is unavailable" \
       "$output_file" >/dev/null
then
    echo "actual RequiresCheck lost its public reason" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
if grep -F -- "Warning: Requires check" "$output_file" >/dev/null; then
    echo "strict upgrade-aur RequiresCheck was downgraded to a warning" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_text_occurrence_count 1 \
    'Checking AUR updates for 1 foreign packages...' "$output_file"
assert_text_occurrence_count 1 \
    'Fetching AUR info for packages 1-1 of 1...' "$output_file"
assert_protected_storage_unchanged
assert_read_only_commands

setup_case dry-run-upgrade-aur-devel-requires-check
set_foreign_inventory 'foo-git 1.0-1 explicit'
MOGUET_TEST_VERCMP_OUTPUT=0
export MOGUET_TEST_VERCMP_OUTPUT
: > "$command_log"
start_mutation_sentinel
if (cd "$case_work_dir" &&
        "$repository_test_binary" --noconfirm upgrade-aur --dry-run) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
assert_expected_observation \
    Blocked 1 "AurUpdateExecutionReason::DevelRequiresCheck" \
    "$status" "dry-run devel RequiresCheck"
if ! grep -F -- \
    "authoritative build provenance is unavailable" \
    "$output_file" >/dev/null
then
    echo "dry-run RequiresCheck lost its blocker reason" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
if grep -F -- "Attention-required details:" "$output_file" >/dev/null; then
    echo "strict upgrade-aur dry-run RequiresCheck became attention" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_protected_storage_unchanged
assert_read_only_commands

setup_case dry-run-upgrade-all-devel-requires-check
set_foreign_inventory 'foo-git 1.0-1 explicit'
MOGUET_TEST_VERCMP_OUTPUT=0
export MOGUET_TEST_VERCMP_OUTPUT
: > "$command_log"
start_mutation_sentinel
if (cd "$case_work_dir" &&
        "$repository_test_binary" --noconfirm upgrade-all --dry-run) \
    </dev/null > "$output_file" 2>&1
then
    status=0
else
    status=$?
fi
assert_expected_observation \
    Blocked 1 "AurUpdateExecutionReason::DevelRequiresCheck" \
    "$status" "dry-run upgrade-all devel RequiresCheck"
if ! grep -F -- \
    "authoritative build provenance is unavailable" \
    "$output_file" >/dev/null
then
    echo "upgrade-all dry-run RequiresCheck lost its blocker reason" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
if grep -F -- "Attention-required details:" "$output_file" >/dev/null; then
    echo "strict upgrade-all dry-run RequiresCheck became attention" >&2
    sed -n '1,240p' "$output_file" >&2
    exit 1
fi
assert_protected_storage_unchanged
assert_read_only_commands

run_supported \
    upgrade-aur NoOp 0 "External owner: AUR RPC" \
    upgrade-aur --dry-run
run_supported \
    upgrade-all Ready 0 "     - system upgrade" \
    upgrade-all --dry-run

echo "dry-run command sentinel regression passed"
