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
. "$repo_root/scripts/validation-status.sh"
MOGUET_TEST_REPOSITORY_ROOT=$repo_root
export MOGUET_TEST_REPOSITORY_ROOT
. "$repo_root/tests/test-command-safety.sh"

tmp_dir=$(mktemp -d)
case_count=0

cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

export PATH=$repo_root/tests/stubs:/usr/bin:/bin
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
require_exact_test_command makepkg "$repo_root/tests/stubs/makepkg"
require_exact_test_command pacman "$repo_root/tests/stubs/pacman"
require_exact_test_command sudo "$repo_root/tests/stubs/sudo"
require_exact_test_command git "$repo_root/tests/stubs/git"
require_exact_test_command vercmp "$repo_root/tests/stubs/vercmp"

setup_case() {
    case_name=$1
    scenario_name=$2
    case_dir=$tmp_dir/cases/$case_name
    stdout_file=$case_dir/stdout
    stderr_file=$case_dir/stderr
    command_log=$case_dir/events.log
    config_file=$case_dir/config.toml
    foreign_inventory_file=$case_dir/foreign-packages

    mkdir -p \
        "$case_dir/home" "$case_dir/xdg-config" \
        "$case_dir/xdg-state" "$case_dir/xdg-cache" \
        "$case_dir/work"
    chmod 0700 "$case_dir/xdg-config"
    : > "$stdout_file"
    : > "$stderr_file"
    : > "$command_log"
    printf '%s\n' 'schema_version = 1' > "$config_file"
    : > "$foreign_inventory_file"

    export HOME=$case_dir/home
    export XDG_CONFIG_HOME=$case_dir/xdg-config
    export XDG_STATE_HOME=$case_dir/xdg-state
    export XDG_CACHE_HOME=$case_dir/xdg-cache
    export MOGUET_TEST_CONFIG_FILE=$config_file
    export MOGUET_TEST_COMMAND_LOG=$command_log
    export MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG=$command_log
    export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory_file
    export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
    export MOGUET_TEST_UPGRADE_ALL_SCENARIO=$scenario_name
    export MOGUET_TEST_AUR_UPDATE_SCENARIO=no-installed-foreign
    export MOGUET_TEST_PACMAN_EXIT_CODE=91
    export MOGUET_TEST_SUDO_EXIT_CODE=92
    case_count=$((case_count + 1))
}

show_case_diagnostics() {
    echo "--- stdout ---" >&2
    sed -n '1,260p' "$stdout_file" >&2
    echo "--- stderr ---" >&2
    sed -n '1,260p' "$stderr_file" >&2
    echo "--- event log ---" >&2
    sed -n '1,320p' "$command_log" >&2
}

fail_case() {
    echo "$1" >&2
    show_case_diagnostics
    exit 1
}

run_status() {
    expected_status=$1
    shift
    actual_status=0
    (cd "$case_dir/work" && "$test_binary" "$@" </dev/null) \
        > "$stdout_file" 2> "$stderr_file" || actual_status=$?
    if [ "$actual_status" -ne "$expected_status" ]; then
        fail_case "unexpected status $actual_status (expected $expected_status): $*"
    fi
}

assert_exact_line() {
    expected=$1
    file=$2
    if ! grep -Fx -- "$expected" "$file" >/dev/null; then
        fail_case "missing exact line: $expected"
    fi
}

assert_contains() {
    expected=$1
    file=$2
    if ! grep -F -- "$expected" "$file" >/dev/null; then
        fail_case "missing expected text: $expected"
    fi
}

assert_not_contains() {
    unexpected=$1
    file=$2
    if grep -F -- "$unexpected" "$file" >/dev/null; then
        fail_case "unexpected text: $unexpected"
    fi
}

assert_primary_system_failure_preserved() {
    assert_exact_line "  operation outcome: Failed" "$stdout_file"
    assert_exact_line "  - source: pacman" "$stdout_file"
    assert_exact_line "    diagnostic: Execution failure" "$stdout_file"
    assert_contains "fixture system upgrade failed" "$stderr_file"
    assert_not_contains "Unexpected upgrade-all command failure" "$stderr_file"
}

assert_no_version_lock_output() {
    assert_not_contains \
        "Possible repository/AUR cross-source version-lock candidate" \
        "$stdout_file"
    assert_not_contains "observed repository candidate" "$stdout_file"
    assert_not_contains "installed foreign package" "$stdout_file"
}

assert_version_lock_authority_safe() {
    for forbidden in \
        "confirmed blocker" \
        "failure cause identified" \
        "actual transaction target" \
        "safe to update" \
        "automatic recovery" \
        "dependency safety guaranteed"
    do
        assert_not_contains "$forbidden" "$stdout_file"
        assert_not_contains "$forbidden" "$stderr_file"
    done
}

assert_not_exact_line() {
    unexpected=$1
    file=$2
    if grep -Fx -- "$unexpected" "$file" >/dev/null; then
        fail_case "unexpected exact line: $unexpected"
    fi
}

extract_attention_section() {
    source_file=$1
    section_file=$2
    if ! awk '
        $0 == "Attention-required details:" {
            in_section = 1
            found_section = 1
            next
        }
        in_section && $0 !~ /^ / { exit }
        in_section { print }
        END {
            if (!found_section) exit 1
        }
    ' "$source_file" > "$section_file"; then
        fail_case "attention-required details section is absent"
    fi
}

assert_occurrence_count() {
    expected_count=$1
    expected=$2
    file=$3
    actual_count=$(validation_grep_count -Fc -- "$expected" "$file")
    if [ "$actual_count" -ne "$expected_count" ]; then
        fail_case "occurrence count for '$expected': $actual_count (expected $expected_count)"
    fi
}

assert_successful_snapshot_unavailable() {
    expected_reason=$1
    expected_metadata_diagnostic=$2
    state_log_file=$XDG_STATE_HOME/moguet/moguet.log

    assert_exact_line "  operation outcome: Succeeded" "$stdout_file"
    assert_exact_line \
        "  package state observation: Unverified" "$stdout_file"
    assert_exact_line "    system/source: Unverified" "$stdout_file"
    assert_exact_line "    AUR: Verified unchanged" "$stdout_file"
    assert_immediately_before \
        "  package state observation: Unverified" \
        "    system/source: Unverified" "$stdout_file"
    assert_immediately_before \
        "    system/source: Unverified" \
        "    AUR: Verified unchanged" "$stdout_file"
    assert_exact_line \
        "    observation reason: $expected_reason" "$stdout_file"
    assert_occurrence_count 1 "$expected_reason" "$stdout_file"
    assert_exact_line "    diagnostic: Requires check" "$stdout_file"
    assert_exact_line "    requires check" "$stdout_file"
    assert_not_contains "    blocking" "$stdout_file"
    assert_contains \
        "Warning: system/source issue: system package snapshot unavailable (observability only, system)" \
        "$stdout_file"
    assert_contains "[package query failed]" "$stdout_file"
    assert_occurrence_count 1 "$expected_metadata_diagnostic" "$stdout_file"
    if [ -s "$stderr_file" ]; then
        fail_case "successful unverified snapshot case wrote to stderr"
    fi
    assert_contains "[WARN] system/source issue:" "$state_log_file"
    assert_not_contains "[ERROR]" "$state_log_file"
}

assert_line_before() {
    first=$1
    second=$2
    file=$3
    first_line=$(grep -nFx -- "$first" "$file" | sed -n '1s/:.*//p')
    second_line=$(grep -nFx -- "$second" "$file" | sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] ||
       [ "$first_line" -ge "$second_line" ]; then
        fail_case "expected '$first' before '$second'"
    fi
}

assert_immediately_before() {
    first=$1
    second=$2
    file=$3
    first_line=$(grep -nFx -- "$first" "$file" | sed -n '1s/:.*//p')
    second_line=$(grep -nFx -- "$second" "$file" | sed -n '1s/:.*//p')
    if [ -z "$first_line" ] || [ -z "$second_line" ] ||
       [ "$second_line" -ne $((first_line + 1)) ]; then
        fail_case "expected '$first' immediately before '$second'"
    fi
}

assert_event_count() {
    expected_count=$1
    expected_event=$2
    actual_count=$(validation_grep_count -Fxc -- \
        "$expected_event" "$command_log")
    if [ "$actual_count" -ne "$expected_count" ]; then
        fail_case "event count for '$expected_event': $actual_count (expected $expected_count)"
    fi
}

assert_cache_absent() {
    if [ -e "$XDG_CACHE_HOME/moguet" ]; then
        fail_case "invalid upgrade-all invocation initialized the cache"
    fi
}

assert_event_log_empty() {
    if [ -s "$command_log" ]; then
        fail_case "invalid upgrade-all invocation performed observable work"
    fi
}

assert_aggregate_absent() {
    if grep -E '^upgrade-all (prepare|execute)( |$)' "$command_log" >/dev/null; then
        fail_case "unrelated route entered the aggregate operation"
    fi
}

assert_no_external_mutation() {
    if grep -E '^(git|makepkg|pacman|sudo|external)( |$)' \
        "$command_log" >/dev/null; then
        fail_case "aggregate command fixture reached an external mutation"
    fi
}

assert_validation_rejected_without_work() {
    expected_diagnostic=$1
    assert_contains "$expected_diagnostic" "$stderr_file"
    assert_event_log_empty
    assert_cache_absent
}

run_matrix_case() {
    matrix_kind=$1
    matrix_index=$2
    expected_matrix_status=$3
    expected_stdout_fragment=$4
    expected_stderr_fragment=$5

    setup_case \
        "matrix-$matrix_kind-$matrix_index" \
        aur-presentation-matrix
    export MOGUET_TEST_UPGRADE_ALL_MATRIX_KIND=$matrix_kind
    export MOGUET_TEST_UPGRADE_ALL_MATRIX_INDEX=$matrix_index
    run_status "$expected_matrix_status" upgrade-all

    if [ "$expected_stdout_fragment" != "-" ]; then
        assert_contains "$expected_stdout_fragment" "$stdout_file"
        assert_not_contains "$expected_stdout_fragment" "$stderr_file"
    fi
    if [ "$expected_stderr_fragment" != "-" ]; then
        assert_contains "$expected_stderr_fragment" "$stderr_file"
        assert_not_contains "$expected_stderr_fragment" "$stdout_file"
    elif [ -s "$stderr_file" ]; then
        fail_case "matrix case unexpectedly wrote to stderr"
    fi
}

run_matrix_table() {
    matrix_table_kind=$1
    expected_matrix_count=$2
    matrix_table_index=0
    while IFS='|' read -r \
        expected_matrix_status \
        expected_stdout_fragment \
        expected_stderr_fragment; do
        if [ -z "$expected_matrix_status" ]; then
            continue
        fi
        run_matrix_case \
            "$matrix_table_kind" \
            "$matrix_table_index" \
            "$expected_matrix_status" \
            "$expected_stdout_fragment" \
            "$expected_stderr_fragment"
        matrix_table_index=$((matrix_table_index + 1))
    done
    if [ "$matrix_table_index" -ne "$expected_matrix_count" ]; then
        fail_case \
            "$matrix_table_kind matrix row count drifted: $matrix_table_index (expected $expected_matrix_count)"
    fi
}

default_snapshot='noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=false rmdeps=false'

# 1: exact custom route. Production parser/routing/presentation are linked;
# only the aggregate API is deterministic.
setup_case exact-route no-updates
run_status 0 upgrade-all
assert_exact_line "upgrade-all prepare $default_snapshot" "$command_log"
assert_exact_line "upgrade-all execute $default_snapshot" "$command_log"
assert_line_before \
    "upgrade-all prepare $default_snapshot" \
    "upgrade-all execute $default_snapshot" "$command_log"
assert_exact_line "upgrade-all summary:" "$stdout_file"
assert_exact_line "  operation outcome: No operation needed" "$stdout_file"
assert_exact_line \
    "  package state observation: Verified unchanged" "$stdout_file"
assert_exact_line \
    "    system/source: Verified unchanged" "$stdout_file"
assert_exact_line "    AUR: Verified unchanged" "$stdout_file"
assert_immediately_before \
    "  package state observation: Verified unchanged" \
    "    system/source: Verified unchanged" "$stdout_file"
assert_immediately_before \
    "    system/source: Verified unchanged" \
    "    AUR: Verified unchanged" "$stdout_file"
assert_exact_line "  no-op basis: No relevant work" "$stdout_file"
assert_exact_line \
    "  items: 0 total, 0 normal, 0 attention-required" "$stdout_file"
assert_not_contains "pacman upgrade-all" "$command_log"
assert_no_external_mutation

# 2-5: existing route compatibility.
setup_case legacy-upgrade no-updates
export MOGUET_TEST_SUDO_EXIT_CODE=0
run_status 0 upgrade
assert_exact_line "sudo pacman -Syu" "$command_log"
assert_aggregate_absent

setup_case legacy-upgrade-aur no-updates
run_status 0 upgrade-aur
assert_exact_line "AUR update: no updates" "$stdout_file"
assert_aggregate_absent

setup_case repository-syu no-updates
export MOGUET_TEST_SUDO_EXIT_CODE=0
run_status 0 -Syu --repo
assert_exact_line "sudo pacman -Syu" "$command_log"
assert_aggregate_absent

setup_case legacy-qua no-updates
run_status 0 -Qua
assert_contains "No foreign packages found." "$stdout_file"
assert_aggregate_absent

# 6-11: semantic misuse must stop before log/cache, metadata, preparation, or
# any package command. The shared event file remaining empty proves all four.
setup_case reject-target no-updates
run_status 1 upgrade-all unexpected-target
assert_validation_rejected_without_work \
    "Invalid: Operation upgrade-all does not accept target operands."

setup_case reject-opaque no-updates
run_status 1 upgrade-all -- opaque-target
assert_validation_rejected_without_work \
    "Invalid: Operation upgrade-all does not accept target operands."

setup_case reject-rmdeps no-updates
run_status 1 upgrade-all --rmdeps
assert_validation_rejected_without_work \
    "Unsupported upgrade-all option: --rmdeps"

setup_case reject-needed no-updates
run_status 1 upgrade-all --needed
assert_validation_rejected_without_work \
    "Unsupported upgrade-all option or operand: --needed"

setup_case reject-aur no-updates
run_status 1 upgrade-all --aur
assert_validation_rejected_without_work \
    "Unsupported upgrade-all option: --aur"

setup_case reject-repo no-updates
run_status 1 upgrade-all --repo
assert_validation_rejected_without_work \
    "Unsupported upgrade-all option: --repo"

# 12-21: supported typed and invocation-only options are independently propagated to both aggregate
# boundaries without CLI-side reinterpretation.
setup_case option-edit no-updates
run_status 0 upgrade-all --edit
assert_event_count 1 \
    "upgrade-all prepare noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=false rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=false rmdeps=false"

setup_case option-noedit no-updates
run_status 0 upgrade-all --noedit
assert_event_count 1 \
    "upgrade-all prepare noedit=true nodiff=false noconfirm=false rebuild=false cleanbuild=false rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=true nodiff=false noconfirm=false rebuild=false cleanbuild=false rmdeps=false"

setup_case option-diff no-updates
run_status 0 upgrade-all --diff
assert_event_count 1 \
    "upgrade-all prepare noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=false rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=false rmdeps=false"

setup_case option-nodiff no-updates
run_status 0 upgrade-all --nodiff
assert_event_count 1 \
    "upgrade-all prepare noedit=false nodiff=true noconfirm=false rebuild=false cleanbuild=false rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=false nodiff=true noconfirm=false rebuild=false cleanbuild=false rmdeps=false"

setup_case option-noconfirm no-updates
run_status 0 upgrade-all --noconfirm
assert_event_count 1 \
    "upgrade-all prepare noedit=false nodiff=false noconfirm=true rebuild=false cleanbuild=false rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=false nodiff=false noconfirm=true rebuild=false cleanbuild=false rmdeps=false"

setup_case option-build-mode-normal no-updates
run_status 0 upgrade-all --build-mode=normal
assert_event_count 1 \
    "upgrade-all prepare noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=false rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=false rmdeps=false"

setup_case option-build-mode-rebuild no-updates
run_status 0 upgrade-all --build-mode=rebuild
assert_event_count 1 \
    "upgrade-all prepare noedit=false nodiff=false noconfirm=false rebuild=true cleanbuild=false rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=false nodiff=false noconfirm=false rebuild=true cleanbuild=false rmdeps=false"

setup_case option-rebuild no-updates
run_status 0 upgrade-all --rebuild
assert_event_count 1 \
    "upgrade-all prepare noedit=false nodiff=false noconfirm=false rebuild=true cleanbuild=false rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=false nodiff=false noconfirm=false rebuild=true cleanbuild=false rmdeps=false"

setup_case option-build-mode-clean no-updates
run_status 0 upgrade-all --build-mode=clean
assert_event_count 1 \
    "upgrade-all prepare noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=true rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=true rmdeps=false"

setup_case option-cleanbuild no-updates
run_status 0 upgrade-all --cleanbuild
assert_event_count 1 \
    "upgrade-all prepare noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=true rmdeps=false"
assert_event_count 1 \
    "upgrade-all execute noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=true rmdeps=false"

# Issue #455: aggregate Changed remains authoritative while the phase rows
# identify which phase supplied the change evidence.
setup_case issue-455-exact issue-455-system-changed-aur-up-to-date
run_status 0 upgrade-all
assert_event_count 1 "upgrade-all prepare $default_snapshot"
assert_event_count 1 "upgrade-all execute $default_snapshot"
assert_exact_line "  operation outcome: Succeeded" "$stdout_file"
assert_exact_line "  package state observation: Changed" "$stdout_file"
assert_exact_line "    system/source: Changed" "$stdout_file"
assert_exact_line "    AUR: Verified unchanged" "$stdout_file"
assert_immediately_before \
    "  package state observation: Changed" \
    "    system/source: Changed" "$stdout_file"
assert_immediately_before \
    "    system/source: Changed" \
    "    AUR: Verified unchanged" "$stdout_file"
assert_exact_line \
    "  items: 1 total, 1 normal, 0 attention-required" "$stdout_file"
assert_exact_line \
    "  update candidates: 0, blockers: 0, requires-check: 0, failures: 0" \
    "$stdout_file"
assert_not_contains "ttf-noto-sans-mono-cjk-vf" "$stdout_file"
assert_no_external_mutation
if [ -s "$stderr_file" ]; then
    fail_case "Issue #455 exact case wrote to stderr"
fi

setup_case issue-455-reverse issue-455-aur-changed
run_status 0 upgrade-all
assert_event_count 1 "upgrade-all prepare $default_snapshot"
assert_event_count 1 "upgrade-all execute $default_snapshot"
assert_exact_line "  operation outcome: Succeeded" "$stdout_file"
assert_exact_line "  package state observation: Changed" "$stdout_file"
assert_exact_line \
    "    system/source: Verified unchanged" "$stdout_file"
assert_exact_line "    AUR: Changed" "$stdout_file"
assert_immediately_before \
    "  package state observation: Changed" \
    "    system/source: Verified unchanged" "$stdout_file"
assert_immediately_before \
    "    system/source: Verified unchanged" \
    "    AUR: Changed" "$stdout_file"
assert_exact_line \
    "  items: 1 total, 0 normal, 1 attention-required" "$stdout_file"
assert_exact_line \
    "  update candidates: 1, blockers: 0, requires-check: 0, failures: 0" \
    "$stdout_file"
assert_exact_line "  - package: issue-455-aur-updated" "$stdout_file"
assert_no_external_mutation
if [ -s "$stderr_file" ]; then
    fail_case "Issue #455 reverse case wrote to stderr"
fi

# Success matrix and normal detailed presentation.
setup_case completed-changed completed-changed
run_status 0 upgrade-all
assert_exact_line "  operation outcome: Succeeded" "$stdout_file"
assert_exact_line "  package state observation: Changed" "$stdout_file"
assert_line_before \
    "upgrade-all summary:" \
    "Attention-required details:" "$stdout_file"
assert_exact_line "  - package: source-updated" "$stdout_file"
assert_exact_line "  - package: source-no-change" "$stdout_file"
assert_exact_line "  - package: aur-updated" "$stdout_file"
assert_exact_line "  - package: aur-no-change" "$stdout_file"
assert_exact_line \
    "    canonical source identity: repository:https://sources.example/source-updated" \
    "$stdout_file"
assert_contains "fixture registered source warning" "$stdout_file"
assert_contains "fixture AUR preparation warning" "$stdout_file"
assert_not_contains "fixture registered source warning" "$stderr_file"

setup_case registered-packagebase-result registered-packagebase-result
run_status 0 upgrade-all
assert_exact_line \
    "  - package: registered-child" "$stdout_file"
assert_exact_line "    PackageBase: registered-suite" "$stdout_file"
assert_exact_line \
    "    selected artifact: registered-child 4.2-3" \
    "$stdout_file"
assert_exact_line \
    "    unselected artifact: registered-sibling 4.2-3" \
    "$stdout_file"
assert_exact_line \
    "    unselected artifact: registered-child-debug 4.2-3" \
    "$stdout_file"
assert_line_before \
    "  - package: registered-child" \
    "    PackageBase: registered-suite" "$stdout_file"
assert_line_before \
    "    selected artifact: registered-child 4.2-3" \
    "    unselected artifact: registered-sibling 4.2-3" \
    "$stdout_file"
assert_line_before \
    "    unselected artifact: registered-sibling 4.2-3" \
    "    unselected artifact: registered-child-debug 4.2-3" \
    "$stdout_file"
assert_line_before \
    "    unselected artifact: registered-child-debug 4.2-3" \
    "  - package: registered-ordinary" "$stdout_file"
assert_not_contains \
    "PackageBase: registered-ordinary" "$stdout_file"
assert_occurrence_count 1 "    PackageBase:" "$stdout_file"

setup_case aur-split-multiple aur-split-multiple
run_status 0 upgrade-all
assert_exact_line "  - package: source-updated" "$stdout_file"
assert_exact_line "  - package: aur-updated" "$stdout_file"
assert_exact_line "  - package: aur-split-main" "$stdout_file"
assert_exact_line "    PackageBase: aur-split-suite" "$stdout_file"
assert_exact_line \
    "    selected artifact: aur-split-main 7.0.1-2" \
    "$stdout_file"
assert_exact_line \
    "    selected artifact: aur-split-dependency 7.0.1-2" \
    "$stdout_file"
assert_line_before \
    "    selected artifact: aur-split-main 7.0.1-2" \
    "    selected artifact: aur-split-dependency 7.0.1-2" \
    "$stdout_file"
assert_not_contains "selected artifact: aur-split-sibling" "$stdout_file"
assert_not_contains "selected artifact: aur-split-suite-debug" "$stdout_file"

# Issue #449: the authoritative UpToDate split child traverses the production
# projection/renderer as one normal item, while the existing generic split
# Updated target remains in the attention section.
setup_case issue-449-split-up-to-date issue-449-split-up-to-date
run_status 0 upgrade-all
assert_exact_line \
    "  items: 6 total, 1 normal, 5 attention-required" "$stdout_file"
assert_exact_line \
    "  update candidates: 4, blockers: 0, requires-check: 0, failures: 0" \
    "$stdout_file"
attention_section_file=$case_dir/attention-section
extract_attention_section "$stdout_file" "$attention_section_file"
assert_exact_line "  - package: aur-split-main" "$attention_section_file"
assert_exact_line "    PackageBase: aur-split-suite" "$attention_section_file"
assert_not_contains \
    "ttf-noto-sans-mono-cjk-vf" "$attention_section_file"

setup_case completed-no-change completed-no-change
run_status 0 upgrade-all
assert_exact_line "  operation outcome: Succeeded" "$stdout_file"
assert_exact_line \
    "  package state observation: Verified unchanged" "$stdout_file"

setup_case before-snapshot-unavailable before-snapshot-unavailable
run_status 0 upgrade-all
assert_successful_snapshot_unavailable \
    "Before snapshot unavailable" \
    "fixture before snapshot unavailable"

setup_case after-snapshot-unavailable after-snapshot-unavailable
run_status 0 upgrade-all
assert_successful_snapshot_unavailable \
    "After snapshot unavailable" \
    "fixture after snapshot unavailable"

setup_case completed-unknown completed-unknown
run_status 0 upgrade-all
assert_exact_line "  operation outcome: Succeeded" "$stdout_file"
assert_exact_line "  package state observation: Unverified" "$stdout_file"
assert_exact_line "    system/source: Unverified" "$stdout_file"
assert_exact_line "    AUR: Verified unchanged" "$stdout_file"
assert_immediately_before \
    "  package state observation: Unverified" \
    "    system/source: Unverified" "$stdout_file"
assert_immediately_before \
    "    system/source: Unverified" \
    "    AUR: Verified unchanged" "$stdout_file"
assert_exact_line \
    "    observation reason: Observation not prepared" "$stdout_file"
assert_occurrence_count 1 "Observation not prepared" "$stdout_file"
assert_exact_line "    diagnostic: Requires check" "$stdout_file"
assert_not_contains "  operation outcome: Failed" "$stdout_file"

setup_case normal-aur-skips aur-skips
run_status 0 upgrade-all
assert_exact_line \
    "  items: 2 total, 2 normal, 0 attention-required" "$stdout_file"
assert_not_contains "aur-up-to-date" "$stdout_file"
assert_not_contains "non-aur-foreign" "$stdout_file"

setup_case many-current-one-attention many-current-one-attention
run_status 1 upgrade-all
assert_exact_line \
    "  items: 51 total, 50 normal, 1 attention-required" "$stdout_file"
assert_exact_line "  - package: attention-package" "$stdout_file"
assert_exact_line "    PackageBase: attention-suite" "$stdout_file"
assert_exact_line "    diagnostic: Unsupported" "$stdout_file"
assert_not_contains "current-package-0" "$stdout_file"
assert_not_contains "current-package-47" "$stdout_file"

setup_case duplicate-exclusions duplicate-exclusions
run_status 0 upgrade-all
assert_exact_line \
    "excluded from AUR update: duplicate-by-name" "$stdout_file"
assert_exact_line \
    "reason: package name handled by explicit source preference" "$stdout_file"
assert_exact_line \
    "matched explicit source package: duplicate-by-name" "$stdout_file"
assert_exact_line \
    "excluded from AUR update: duplicate-by-base-child" "$stdout_file"
assert_exact_line \
    "reason: PackageBase handled by explicit source preference" "$stdout_file"
assert_exact_line "matched PackageBase: shared-base" "$stdout_file"
assert_exact_line "  operation outcome: Succeeded" "$stdout_file"

setup_case external-satisfaction external-satisfaction
run_status 0 upgrade-all
assert_exact_line \
    "AUR build unit externally satisfied: external-base" "$stdout_file"
assert_contains \
    "provided by explicit source preference: repository:https://sources.example/explicit-provider" \
    "$stdout_file"
assert_exact_line \
    "affected AUR root: aur-root (build dependency)" "$stdout_file"
assert_exact_line "  operation outcome: Succeeded" "$stdout_file"

# 23-33: every aggregate failure status, source/AUR result category, phase
# NotAttempted reason, partial completion, and cleanup summary.
setup_case blocked-before-mutation blocked-before-mutation
run_status 1 upgrade-all
assert_event_count 1 "upgrade-all prepare $default_snapshot"
assert_event_count 0 "upgrade-all execute $default_snapshot"
assert_exact_line "  operation outcome: Blocked" "$stdout_file"
assert_exact_line "  package state observation: Not observed" "$stdout_file"
assert_exact_line "    system/source: Not observed" "$stdout_file"
assert_exact_line "    AUR: Not observed" "$stdout_file"
assert_immediately_before \
    "  package state observation: Not observed" \
    "    system/source: Not observed" "$stdout_file"
assert_immediately_before \
    "    system/source: Not observed" \
    "    AUR: Not observed" "$stdout_file"
assert_exact_line \
    "  - package: source-unsupported" \
    "$stdout_file"
assert_exact_line \
    "    diagnostic: Unsupported" "$stdout_file"
assert_exact_line \
    "  - package: source-incomplete" \
    "$stdout_file"
assert_exact_line \
    "    diagnostic: Requires check" "$stdout_file"
assert_contains "reason [preparation]: explicit source adapter invalid" \
    "$stdout_file"
assert_contains "fixture aggregate preparation blocked" "$stderr_file"
assert_contains "Error: system/source issue:" "$stderr_file"
assert_contains "fixture blocking system/source issue" "$stderr_file"
assert_contains \
    'fixture blocking system/source issue\x0Aunsafe-after\x1B\xE2\x80\xAE\xEF\xBB\xBF\xFF' \
    "$stderr_file"
raw_unsafe_payload=$(printf 'unsafe-after\033\342\200\256\357\273\277')
raw_bidi=$(printf '\342\200\256')
raw_bom=$(printf '\357\273\277')
assert_not_contains "$raw_unsafe_payload" "$stderr_file"
assert_not_contains "$raw_bidi" "$stderr_file"
assert_not_contains "$raw_bom" "$stderr_file"
assert_contains \
    "[ERROR] system/source issue:" \
    "$XDG_STATE_HOME/moguet/moguet.log"

setup_case stopped-system stopped-on-system-failure
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Failed" "$stdout_file"
assert_exact_line "  package state observation: Not observed" "$stdout_file"
assert_exact_line \
    "  - package: source-after-system" \
    "$stdout_file"
assert_exact_line \
    "    reason [registered source]: registered source not attempted" \
    "$stdout_file"
assert_exact_line "  - source: pacman" "$stdout_file"
assert_exact_line "    diagnostic: Execution failure" "$stdout_file"
assert_contains "fixture system upgrade failed" "$stderr_file"
assert_no_version_lock_output

setup_case stopped-system-version-lock-complete-zero \
    stopped-system-version-lock-complete-zero
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_no_version_lock_output

setup_case stopped-system-version-lock-partial-zero \
    stopped-system-version-lock-partial-zero
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_no_version_lock_output

setup_case stopped-system-version-lock-failed-zero \
    stopped-system-version-lock-failed-zero
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_no_version_lock_output

setup_case stopped-system-version-lock-correlation-failure \
    stopped-system-version-lock-correlation-failure
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_no_version_lock_output

setup_case stopped-system-version-lock-compatible \
    stopped-system-version-lock-compatible
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_exact_line \
    "Possible repository/AUR cross-source version-lock candidate: 1" \
    "$stdout_file"
assert_line_before \
    "  - source: pacman" \
    "Possible repository/AUR cross-source version-lock candidate: 1" \
    "$stdout_file"
assert_exact_line "  - repository package: virtualbox" "$stdout_file"
assert_exact_line "    installed version: 7.2.14-1" "$stdout_file"
assert_exact_line \
    "    observed repository candidate: virtualbox 7.2.16-1 (repository: extra)" \
    "$stdout_file"
assert_exact_line \
    "    installed foreign package: virtualbox-ext-oracle 7.2.14-1" \
    "$stdout_file"
assert_exact_line \
    "    installed requirement: virtualbox=7.2.14" "$stdout_file"
assert_exact_line \
    "    observed AUR replacement candidate: virtualbox-ext-oracle 7.2.16-1" \
    "$stdout_file"
assert_exact_line \
    "    replacement requirement: virtualbox=7.2.16" "$stdout_file"
assert_exact_line \
    "    replacement metadata: the direct runtime requirement matches the observed repository candidate" \
    "$stdout_file"
assert_exact_line \
    "The observed repository candidate is metadata evidence only; this correlation does not identify the cause of the system update failure." \
    "$stdout_file"
assert_exact_line \
    "Moguet did not perform a coordinated repository/AUR update; review the displayed versions and dependency constraints manually." \
    "$stdout_file"
assert_not_contains "supplemental candidate observation was incomplete" \
    "$stdout_file"
assert_version_lock_authority_safe

setup_case stopped-system-version-lock-incompatible \
    stopped-system-version-lock-incompatible
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_exact_line \
    "    replacement requirement: virtualbox=7.2.15" \
    "$stdout_file"
assert_exact_line \
    "    replacement metadata: the direct runtime requirement does not match the observed repository candidate" \
    "$stdout_file"
assert_not_contains \
    "replacement metadata: the direct runtime requirement matches" \
    "$stdout_file"
assert_version_lock_authority_safe

setup_case stopped-system-version-lock-missing \
    stopped-system-version-lock-missing
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_exact_line \
    "    observed AUR replacement: a matching candidate was not found" \
    "$stdout_file"
assert_not_contains "metadata could not be queried" "$stdout_file"
assert_version_lock_authority_safe

setup_case stopped-system-version-lock-unknown \
    stopped-system-version-lock-unknown
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_exact_line \
    "    replacement metadata: compatibility could not be determined" \
    "$stdout_file"
assert_version_lock_authority_safe

setup_case stopped-system-version-lock-query-failure \
    stopped-system-version-lock-query-failure
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_exact_line \
    "    observed AUR replacement: metadata could not be queried" \
    "$stdout_file"
assert_not_contains "a matching candidate was not found" "$stdout_file"
assert_not_contains "fixture replacement query failure" "$stdout_file"
assert_not_contains "fixture replacement query failure" "$stderr_file"
assert_version_lock_authority_safe

setup_case stopped-system-version-lock-ambiguous \
    stopped-system-version-lock-ambiguous
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_exact_line \
    "    observed AUR replacement: evidence is ambiguous" \
    "$stdout_file"
assert_not_contains "7.2.16-1.ambiguous" "$stdout_file"
assert_version_lock_authority_safe

setup_case stopped-system-version-lock-partial-compatible \
    stopped-system-version-lock-partial-compatible
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_exact_line "  - repository package: virtualbox" "$stdout_file"
assert_exact_line \
    "  The supplemental candidate observation was incomplete." \
    "$stdout_file"
assert_version_lock_authority_safe

setup_case stopped-system-version-lock-multiple \
    stopped-system-version-lock-multiple
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_exact_line \
    "Possible repository/AUR cross-source version-lock candidates: 2" \
    "$stdout_file"
assert_occurrence_count 2 "  - repository package:" "$stdout_file"
assert_exact_line "  - repository package: virtualbox" "$stdout_file"
assert_exact_line "  - repository package: example-api" "$stdout_file"
assert_exact_line \
    "    installed foreign package: example-addon 1.4.0-2" \
    "$stdout_file"
assert_occurrence_count 1 \
    "Moguet did not perform a coordinated repository/AUR update" \
    "$stdout_file"
assert_version_lock_authority_safe

setup_case stopped-system-version-lock-renderer-exception \
    stopped-system-version-lock-invalid-index
run_status 1 upgrade-all
assert_primary_system_failure_preserved
assert_no_version_lock_output
assert_version_lock_authority_safe

setup_case stopped-source stopped-on-source-failure
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Partial failure" "$stdout_file"
assert_exact_line "  package state observation: Changed" "$stdout_file"
assert_exact_line \
    "  - package: source-failed" \
    "$stdout_file"
assert_exact_line \
    "    diagnostic: Execution failure" "$stdout_file"
assert_exact_line \
    "    reason [registered source]: registered source failed" \
    "$stdout_file"
assert_contains "fixture source build or install failed" "$stderr_file"

setup_case stopped-source-cleanup stopped-after-source-cleanup-failure
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Partial failure" "$stdout_file"
assert_exact_line "  package state observation: Changed" "$stdout_file"
assert_exact_line \
    "  - package: source-cleanup-failed" \
    "$stdout_file"
assert_exact_line \
    "    diagnostic: Partial failure" "$stdout_file"
assert_exact_line \
    "    reason [registered source]: registered source updated; cleanup failed" \
    "$stdout_file"
assert_contains "fixture source cleanup failed" "$stderr_file"

setup_case stopped-source-no-change-cleanup \
    stopped-after-source-no-change-cleanup-failure
run_status 1 upgrade-all
assert_exact_line \
    "  - package: source-no-change-cleanup-failed" \
    "$stdout_file"
assert_exact_line \
    "  package state observation: Not observed" "$stdout_file"
assert_exact_line \
    "    reason [registered source]: registered source unchanged; cleanup failed" \
    "$stdout_file"

setup_case stopped-before-aur stopped-before-aur-execution
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Partial failure" "$stdout_file"
assert_exact_line "  package state observation: Unverified" "$stdout_file"
assert_exact_line \
    "  - package: aur-preflight-blocked" \
    "$stdout_file"
assert_exact_line \
    "    reason [AUR execution]: AUR target unsupported" \
    "$stdout_file"
assert_exact_line \
    "  - package: aur-preparation-blocked" \
    "$stdout_file"
assert_contains "fixture AUR preflight blocker" "$stderr_file"
assert_contains "fixture AUR preparation blocker" "$stderr_file"

setup_case stopped-aur stopped-on-aur-failure
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Partial failure" "$stdout_file"
assert_exact_line "  package state observation: Changed" "$stdout_file"
assert_exact_line "  - package: aur-first-updated" "$stdout_file"
assert_exact_line \
    "  - package: aur-failed" \
    "$stdout_file"
assert_exact_line \
    "    diagnostic: Execution failure" "$stdout_file"
assert_exact_line \
    "  - package: aur-later" \
    "$stdout_file"
assert_contains \
    "execution failure for PackageBase aur-failed: package transaction failed (exit code 86)" \
    "$stderr_file"
assert_contains \
    "transaction attempt: aur-failed 4.2.0-1 (explicit)" "$stderr_file"
assert_not_contains "/private/workspace/upgrade-all-secret" "$stdout_file"
assert_not_contains "/private/workspace/upgrade-all-secret" "$stderr_file"
assert_not_contains "/private/artifacts/" "$stdout_file"
assert_not_contains "/private/artifacts/" "$stderr_file"

setup_case stopped-aur-cleanup stopped-after-aur-cleanup-failure
run_status 1 upgrade-all
assert_exact_line \
    "  - package: aur-cleanup-failed" \
    "$stdout_file"
assert_exact_line \
    "    reason [AUR execution]: AUR target updated; cleanup failed" \
    "$stdout_file"
assert_contains \
    "execution failure for PackageBase aur-cleanup-failed: cleanup failure after successful package transaction" \
    "$stderr_file"

setup_case stopped-aur-no-change-cleanup \
    stopped-after-aur-no-change-cleanup-failure
run_status 1 upgrade-all
assert_exact_line \
    "  - package: aur-no-change-cleanup-failed" \
    "$stdout_file"
assert_exact_line \
    "    reason [AUR execution]: AUR target unchanged; cleanup failed" \
    "$stdout_file"

setup_case stopped-aur-mixed-cleanup \
    stopped-after-aur-mixed-cleanup-failure
run_status 1 upgrade-all
assert_exact_line \
    "  - package: aur-cleanup-main" "$stdout_file"
assert_exact_line \
    "  - package: aur-cleanup-later" \
    "$stdout_file"
assert_exact_line "    PackageBase: aur-cleanup-suite" "$stdout_file"
assert_exact_line \
    "    selected artifact: aur-cleanup-main 8.3.0-5" \
    "$stdout_file"
assert_exact_line \
    "    selected artifact: aur-cleanup-dependency 8.3.0-5" \
    "$stdout_file"
assert_contains \
    "execution failure for PackageBase aur-cleanup-suite: cleanup failure after successful package transaction" \
    "$stderr_file"
assert_not_contains "/private/workspace/upgrade-all-secret" "$stdout_file"
assert_not_contains "/private/workspace/upgrade-all-secret" "$stderr_file"
assert_exact_line "  operation outcome: Partial failure" "$stdout_file"

setup_case foreign-inventory-failure foreign-inventory-failure
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Partial failure" "$stdout_file"
assert_exact_line "  package state observation: Not observed" "$stdout_file"
assert_exact_line \
    "    diagnostic: Query failure" "$stdout_file"
assert_exact_line \
    "    reason [foreign inventory]: foreign inventory failed" \
    "$stdout_file"
assert_contains "foreign inventory read failed" "$stderr_file"
assert_contains \
    "foreign inventory failure [package query failed]: fixture foreign inventory read failed" \
    "$stderr_file"
assert_occurrence_count 1 "fixture foreign inventory read failed" "$stderr_file"

# Direct inventory fields are authoritative even when a synthetic result keeps
# Completed and contains no aggregate issue copy.
setup_case defensive-direct-foreign-inventory \
    completed-direct-foreign-inventory-failure
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Partial failure" "$stdout_file"
assert_exact_line \
    "  package state observation: Verified unchanged" "$stdout_file"
assert_contains \
    "foreign inventory failure [local database unavailable]: fixture direct foreign inventory failure" \
    "$stderr_file"
assert_occurrence_count 1 \
    "fixture direct foreign inventory failure" "$stderr_file"
assert_contains \
    "failure details despite a successful aggregate status" "$stderr_file"

setup_case aggregate-inconsistent inconsistent-result
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Inconsistent" "$stdout_file"
assert_exact_line "  package state observation: Unverified" "$stdout_file"
assert_exact_line \
    "    diagnostic: Internal inconsistency" "$stdout_file"
assert_contains "fixture aggregate correlation inconsistency" "$stderr_file"

setup_case nested-system-unavailable nested-system-unavailable
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Inconsistent" "$stdout_file"
assert_exact_line "  - source: pacman" "$stdout_file"
assert_exact_line "    diagnostic: Execution failure" "$stdout_file"
assert_contains \
    "The system result is unavailable because an unexpected exception occurred after the phase started." \
    "$stderr_file"

setup_case nested-source-preserved nested-source-preserved
run_status 1 upgrade-all
assert_exact_line "  operation outcome: Inconsistent" "$stdout_file"
assert_exact_line \
    "  - package: source-recorded-before-exception" \
    "$stdout_file"
assert_exact_line \
    "  - package: source-unavailable-after-start" \
    "$stdout_file"
assert_exact_line "    diagnostic: Requires check" "$stdout_file"
assert_exact_line \
    "  - package: source-not-started" \
    "$stdout_file"
assert_contains \
    "The registered source result is unavailable because an unexpected exception occurred after the phase started." \
    "$stderr_file"

# A synthetically successful aggregate status must never mask typed
# query/planning/mapping/reduction/inconsistency/cleanup/NotAttempted details.
setup_case defensive-query completed-query-failure
run_status 1 upgrade-all
assert_contains "AUR query failure for query-broken, query-also-broken" \
    "$stderr_file"
assert_exact_line "  operation outcome: Partial failure" "$stdout_file"
assert_exact_line "    diagnostic: Query failure" "$stdout_file"
assert_contains \
    "failure details despite a successful aggregate status" "$stderr_file"

setup_case defensive-planning completed-planning-issue
run_status 1 upgrade-all
assert_contains "AUR planner issue: conflicting explicit PackageBase" \
    "$stderr_file"
assert_contains "AUR mapping issue: target/planner mapping inconsistent" \
    "$stderr_file"
assert_exact_line "  operation outcome: Inconsistent" "$stdout_file"
assert_exact_line "    diagnostic: Blocked" "$stdout_file"
assert_exact_line "    diagnostic: Internal inconsistency" "$stdout_file"

setup_case defensive-inconsistency completed-inconsistency
run_status 1 upgrade-all
assert_contains "AUR reduction issue:" "$stderr_file"
assert_contains "fixture completed aggregate inconsistency" "$stderr_file"
assert_exact_line "  operation outcome: Inconsistent" "$stdout_file"

setup_case defensive-cleanup completed-cleanup-failure
run_status 1 upgrade-all
assert_exact_line \
    "  - package: defensive-cleanup" \
    "$stdout_file"
assert_exact_line "    diagnostic: Partial failure" "$stdout_file"
assert_contains \
    "execution failure: cleanup failure after successful package transaction" \
    "$stderr_file"

setup_case defensive-not-attempted completed-not-attempted
run_status 1 upgrade-all
assert_exact_line \
    "  - package: defensive-not-attempted" \
    "$stdout_file"
assert_exact_line "    diagnostic: Blocked" "$stdout_file"
assert_contains \
    "failure details despite a successful aggregate status" "$stderr_file"

# The top-level command boundary maps known and unknown exceptions to
# stderr + nonzero without fabricating a typed phase result.
setup_case prepare-exception prepare-exception
run_status 1 upgrade-all
assert_contains \
    "Unexpected upgrade-all command failure: fixture upgrade-all preparation exception" \
    "$stderr_file"
assert_not_contains "upgrade-all summary:" "$stdout_file"

setup_case execute-unknown-exception execute-unknown-exception
run_status 1 upgrade-all
assert_contains \
    "Unexpected upgrade-all command failure: unknown exception." "$stderr_file"
assert_not_contains "upgrade-all summary:" "$stdout_file"

# Every AUR-related enum value mapped by the production upgrade-all
# presenter. The row order mirrors the explicit enum arrays in the operation
# stub; the last row of each enum table injects an unknown value and proves
# that the command boundary fails closed.
run_matrix_table aur-phase-status 9 <<'EOF'
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
0|upgrade-all summary:|-
0|upgrade-all summary:|-
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|-|Unexpected upgrade-all command failure: Unknown upgrade-all AUR phase status.
EOF

run_matrix_table not-attempted-reason 9 <<'EOF'
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|upgrade-all summary:|The upgrade-all result contains failure details despite a successful aggregate status.
1|-|Unexpected upgrade-all command failure: Unknown upgrade-all NotAttempted reason.
EOF

run_matrix_table target-status 10 <<'EOF'
0|  - package: matrix-target|-
0|  - package: matrix-target|-
0|upgrade-all summary:|-
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|  - package: matrix-target|execution failure: build or install failure
1|  - package: matrix-target|execution failure: cleanup failure after successful package transaction
1|  - package: matrix-target|execution failure: cleanup failure after successful package transaction
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|-|Unexpected upgrade-all command failure: Unknown AUR update target status.
EOF

run_matrix_table operation-status 8 <<'EOF'
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|-|Unexpected upgrade-all command failure: Unknown AUR update operation status.
EOF

run_matrix_table preflight-reason 24 <<'EOF'
1|  - package: matrix-target|AUR preflight issue: none: matrix preflight diagnostic
0|upgrade-all summary:|-
1|  - package: matrix-target|AUR preflight issue: devel update requires check: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: devel update requires check: matrix preflight diagnostic
0|upgrade-all summary:|-
1|  - package: matrix-target|AUR preflight issue: AUR metadata unavailable: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: version comparison unavailable: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: installed reason unknown: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: update plan inconsistent: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: duplicate update target: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: repository metadata unavailable: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: AUR dependency metadata unavailable: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: provider metadata unavailable: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: unresolved dependency: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: version constraint unverified: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: dependency cycle: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: build plan inconsistent: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: package base mismatch: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: split package selection required: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: multiple package targets for package base: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: ambiguous provider: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: conflicts/replaces unresolved: matrix preflight diagnostic
1|  - package: matrix-target|AUR preflight issue: installed package metadata unavailable: matrix preflight diagnostic
1|  - package: matrix-target|Unexpected upgrade-all command failure: Unknown AUR update preflight reason.
EOF

run_matrix_table preparation-reason 17 <<'EOF'
1|  - package: matrix-target|AUR preparation issue: none: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: blocking preflight: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: preflight inconsistent: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: preflight inconsistent: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: build plan missing: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: build plan order empty: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: root attribution inconsistent: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: package target attribution inconsistent: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: desired install reason missing: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: source preference unavailable: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: source preference PKGDEST conflict: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: static work item invalid: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: pacman database unavailable: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: generic preparation inconsistent: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: build unit selection inconsistent: matrix preparation diagnostic
1|  - package: matrix-target|AUR preparation issue: external satisfaction inconsistent: matrix preparation diagnostic
1|  - package: matrix-target|Unexpected upgrade-all command failure: Unknown AUR update preparation reason.
EOF

run_matrix_table execution-failure-kind 6 <<'EOF'
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|  - package: matrix-target|execution failure: build or install failure
1|  - package: matrix-target|execution failure: cleanup failure after successful package transaction
1|  - package: matrix-target|execution failure: unknown exception
1|  - package: matrix-target|The upgrade-all result contains failure details despite a successful aggregate status.
1|-|Unexpected upgrade-all command failure: Unknown AUR work-item failure kind.
EOF

run_matrix_table reduction-stage 4 <<'EOF'
1|upgrade-all summary:|AUR reduction issue: preflight: duplicate preflight update plan index: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preparation: duplicate preflight update plan index: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: execution: duplicate preflight update plan index: matrix reduction diagnostic
1|upgrade-all summary:|Unexpected upgrade-all command failure: Unknown AUR reduction stage.
EOF

run_matrix_table reduction-reason 27 <<'EOF'
1|upgrade-all summary:|AUR reduction issue: preflight: duplicate preflight update plan index: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: out-of-range preflight update plan index: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: preflight target order inconsistent: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: duplicate preparation attribution: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: unknown preparation update plan index: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: preparation attribution inconsistent: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: preparation target snapshot inconsistent: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: duplicate execution work item index: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: execution work item order inconsistent: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: duplicate execution attribution: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: unknown execution update plan index: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: missing execution attribution: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: duplicate execution child attribution: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: missing execution child attribution: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: unexpected execution child attribution: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: unknown execution child update plan index: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: execution child snapshot inconsistent: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: unexpected selected artifact: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: unexpected unselected artifact identity: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: execution result with preparation issues: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: missing execution result: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: other correlation inconsistency: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: unknown enum value: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: work item result inconsistent: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: invocation result inconsistent: matrix reduction diagnostic
1|upgrade-all summary:|AUR reduction issue: preflight: other correlation inconsistency: matrix reduction diagnostic
1|upgrade-all summary:|Unexpected upgrade-all command failure: Unknown AUR reduction reason.
EOF

run_matrix_table filtered-issue-kind 17 <<'EOF'
1|upgrade-all summary:|AUR mapping issue: unknown update classification: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: target/planner mapping inconsistent: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: target/planner mapping inconsistent: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: filtered target mapping inconsistent: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: preflight target mapping inconsistent: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: preflight invocation index out of range: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: preflight invocation identity mismatch: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: build-plan root index missing: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: build-plan root index out of range: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: build-plan root identity mismatch: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: build-plan root package identity mismatch: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: build-unit order identity mismatch: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: build-unit root attribution inconsistent: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: build-unit selection mapping inconsistent: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: execution build-unit mapping inconsistent: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|AUR mapping issue: reduced target mapping inconsistent: package matrix-target: PackageBase matrix-base: matrix mapping diagnostic
1|upgrade-all summary:|Unexpected upgrade-all command failure: Unknown filtered AUR operation issue kind.
EOF

run_matrix_table planning-issue-kind 25 <<'EOF'
1|upgrade-all summary:|AUR planner issue: explicit preference package name missing: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: explicit produced package name missing: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: explicit PackageBase absent: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: explicit PackageBase resolution failed: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: explicit PackageBase empty: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: explicit source identity absent: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: explicit source identity resolution failed: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: explicit source identity empty: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: conflicting explicit source identity definition: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: conflicting explicit package name: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: conflicting explicit PackageBase: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: AUR target package name missing: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: AUR target PackageBase absent: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: AUR target PackageBase resolution failed: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: AUR target PackageBase empty: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: unsupported AUR target: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: incomplete AUR target: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: build-unit PackageBase absent: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: build-unit PackageBase resolution failed: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: build-unit PackageBase empty: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: build unit has no root attribution: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: build-unit target index out of range: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: duplicate selected target PackageBase: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|AUR planner issue: duplicate selected build-unit PackageBase: package matrix-target: PackageBase matrix-base
1|upgrade-all summary:|Unexpected upgrade-all command failure: Unknown upgrade-all planning issue kind.
EOF

run_matrix_table target-disposition 8 <<'EOF'
1|upgrade-all summary:|Unexpected upgrade-all command failure: Non-exclusion target disposition reached duplicate presentation.
0|reason: package name handled by explicit source preference|-
0|reason: PackageBase handled by explicit source preference|-
1|upgrade-all summary:|Unexpected upgrade-all command failure: Non-exclusion target disposition reached duplicate presentation.
1|upgrade-all summary:|Unexpected upgrade-all command failure: Non-exclusion target disposition reached duplicate presentation.
1|upgrade-all summary:|Unexpected upgrade-all command failure: Non-exclusion target disposition reached duplicate presentation.
1|upgrade-all summary:|Unexpected upgrade-all command failure: Non-exclusion target disposition reached duplicate presentation.
1|upgrade-all summary:|Unexpected upgrade-all command failure: Unknown upgrade-all target disposition.
EOF

run_matrix_table build-unit-role 5 <<'EOF'
0|affected AUR root: aur-root (root)|-
0|affected AUR root: aur-root (runtime dependency)|-
0|affected AUR root: aur-root (build dependency)|-
0|affected AUR root: aur-root (check dependency)|-
1|upgrade-all summary:|Unexpected upgrade-all command failure: Unknown upgrade-all build-unit role.
EOF

run_matrix_table preparation-warning 1 <<'EOF'
0|AUR preparation warning: matrix-preference: matrix preparation warning diagnostic|-
EOF

run_matrix_table external-attribution-missing 1 <<'EOF'
1|upgrade-all summary:|Unexpected upgrade-all command failure: External satisfaction has no explicit source identity.
EOF

# Issue #350 Slice 5: representative typed outcomes, observations, and
# diagnostic classes must retain their semantics in Japanese presentation.
command -v localedef >/dev/null 2>&1 ||
    fail_case "localedef is required for upgrade-all localization coverage"
locale_root=$tmp_dir/locale
mkdir -p "$locale_root"
localedef --no-archive -i en_US -f UTF-8 "$locale_root/en_US.UTF-8"

setup_case localized-no-op no-updates
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 0 upgrade-all
assert_exact_line "  操作結果: 操作不要" "$stdout_file"
assert_exact_line \
    "  パッケージ状態の観測: 変更なしを確認" "$stdout_file"
assert_exact_line \
    "    system/source: 変更なしを確認" "$stdout_file"
assert_exact_line "    AUR: 変更なしを確認" "$stdout_file"
assert_exact_line "  no-opの根拠: 該当作業なし" "$stdout_file"
assert_not_contains "  操作結果: 失敗" "$stdout_file"

setup_case localized-completed-changed completed-changed
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 0 upgrade-all
assert_exact_line "  操作結果: 成功" "$stdout_file"
assert_exact_line "  パッケージ状態の観測: 変更あり" "$stdout_file"
assert_not_contains "  操作結果: 操作不要" "$stdout_file"

setup_case localized-issue-455-exact \
    issue-455-system-changed-aur-up-to-date
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 0 upgrade-all
assert_exact_line "  操作結果: 成功" "$stdout_file"
assert_exact_line "  パッケージ状態の観測: 変更あり" "$stdout_file"
assert_exact_line "    system/source: 変更あり" "$stdout_file"
assert_exact_line "    AUR: 変更なしを確認" "$stdout_file"
assert_immediately_before \
    "  パッケージ状態の観測: 変更あり" \
    "    system/source: 変更あり" "$stdout_file"
assert_immediately_before \
    "    system/source: 変更あり" \
    "    AUR: 変更なしを確認" "$stdout_file"
assert_exact_line \
    "  項目: 全1件、通常1件、要確認0件" "$stdout_file"
assert_exact_line \
    "  更新候補: 0件、blocker: 0件、要確認: 0件、失敗: 0件" \
    "$stdout_file"
assert_not_contains "ttf-noto-sans-mono-cjk-vf" "$stdout_file"

setup_case localized-issue-455-reverse issue-455-aur-changed
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 0 upgrade-all
assert_exact_line "  操作結果: 成功" "$stdout_file"
assert_exact_line "  パッケージ状態の観測: 変更あり" "$stdout_file"
assert_exact_line \
    "    system/source: 変更なしを確認" "$stdout_file"
assert_exact_line "    AUR: 変更あり" "$stdout_file"
assert_immediately_before \
    "  パッケージ状態の観測: 変更あり" \
    "    system/source: 変更なしを確認" "$stdout_file"
assert_immediately_before \
    "    system/source: 変更なしを確認" \
    "    AUR: 変更あり" "$stdout_file"
assert_exact_line \
    "  項目: 全1件、通常0件、要確認1件" "$stdout_file"
assert_exact_line \
    "  更新候補: 1件、blocker: 0件、要確認: 0件、失敗: 0件" \
    "$stdout_file"
assert_contains \
    "確認済みソースの結果（PackageBase issue-455-aur-updated）: 正確なupstream commit 4444444444444444444444444444444444444444 の更新レビューを受理しました。" \
    "$stdout_file"
assert_contains \
    "確認済み状態（PackageBase issue-455-aur-updated）を世代 31 として確定しました。ビルドとインストールの結果は別に報告します。" \
    "$stdout_file"
assert_contains \
    "この実行内のeditor変更" "$stdout_file"
assert_contains \
    "正確な確認済みcommit treeそのものではありません" "$stdout_file"
assert_contains \
    "ビルド結果（PackageBase issue-455-aur-updated）: 成功。" \
    "$stdout_file"
assert_contains \
    "インストール結果（PackageBase issue-455-aur-updated）: 成功。" \
    "$stdout_file"
assert_not_contains "UpdateReview" "$stdout_file"
assert_not_contains "InvocationLocal" "$stdout_file"
assert_not_contains "exact upstream commit" "$stdout_file"
assert_not_contains "generation" "$stdout_file"
assert_not_contains "invocation-local editor overlay" "$stdout_file"

setup_case localized-completed-unknown completed-unknown
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 0 upgrade-all
assert_exact_line "upgrade-allの概要:" "$stdout_file"
assert_exact_line "  操作結果: 成功" "$stdout_file"
assert_exact_line "  パッケージ状態の観測: 未検証" "$stdout_file"
assert_exact_line "    system/source: 未検証" "$stdout_file"
assert_exact_line "    AUR: 変更なしを確認" "$stdout_file"
assert_exact_line "    観測理由: 観測未準備" "$stdout_file"
assert_exact_line "    診断: 確認が必要" "$stdout_file"
assert_exact_line "    確認が必要" "$stdout_file"
assert_not_contains "  操作結果: 失敗" "$stdout_file"
assert_line_before \
    "upgrade-allの概要:" \
    "確認が必要な詳細:" "$stdout_file"
if [ -s "$stderr_file" ]; then
    fail_case "localized successful-unverified case wrote to stderr"
fi

setup_case localized-blocked blocked-before-mutation
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 1 upgrade-all
assert_exact_line "  操作結果: 実行不可" "$stdout_file"
assert_exact_line "  パッケージ状態の観測: 未観測" "$stdout_file"
assert_exact_line "    system/source: 未観測" "$stdout_file"
assert_exact_line "    AUR: 未観測" "$stdout_file"
assert_exact_line "    診断: 未対応" "$stdout_file"
assert_exact_line "    診断: 確認が必要" "$stdout_file"
assert_contains "    実行を阻害" "$stdout_file"

setup_case localized-partial-failure stopped-on-source-failure
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 1 upgrade-all
assert_exact_line "  操作結果: 部分的失敗" "$stdout_file"
assert_exact_line "  パッケージ状態の観測: 変更あり" "$stdout_file"
assert_exact_line "    診断: 実行失敗" "$stdout_file"

setup_case localized-failed stopped-on-system-failure
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 1 upgrade-all
assert_exact_line "  操作結果: 失敗" "$stdout_file"
assert_exact_line "  パッケージ状態の観測: 未観測" "$stdout_file"
assert_exact_line "    診断: 実行失敗" "$stdout_file"
assert_contains "登録済みソースの更新を未試行" "$stdout_file"

setup_case localized-version-lock-compatible \
    stopped-system-version-lock-compatible
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 1 upgrade-all
assert_exact_line "  操作結果: 失敗" "$stdout_file"
assert_exact_line "  - ソース: pacman" "$stdout_file"
assert_exact_line "    診断: 実行失敗" "$stdout_file"
assert_exact_line \
    "repository/AURをまたぐversion-lock候補: 1件" "$stdout_file"
assert_exact_line "  - リポジトリパッケージ: virtualbox" "$stdout_file"
assert_exact_line \
    "    観測したリポジトリ候補: virtualbox 7.2.16-1（リポジトリ: extra）" \
    "$stdout_file"
assert_exact_line \
    "    インストール済みforeignパッケージ: virtualbox-ext-oracle 7.2.14-1" \
    "$stdout_file"
assert_exact_line \
    "    インストール済み依存条件: virtualbox=7.2.14" "$stdout_file"
assert_exact_line \
    "    観測したAUR置換候補: virtualbox-ext-oracle 7.2.16-1" \
    "$stdout_file"
assert_exact_line \
    "    置換候補の依存条件: virtualbox=7.2.16" "$stdout_file"
assert_exact_line \
    "    置換メタデータ: direct runtime依存条件は観測したリポジトリ候補で満たされます" \
    "$stdout_file"
assert_exact_line \
    "観測したリポジトリ候補はメタデータ上の根拠に限られ、この相関はシステム更新の失敗原因を特定しません。" \
    "$stdout_file"
assert_exact_line \
    "Moguetはrepository/AURをまたぐ協調更新を実行していません。表示されたバージョンと依存条件を手動で確認してください。" \
    "$stdout_file"
assert_contains "fixture system upgrade failed" "$stderr_file"
assert_not_contains "confirmed blocker" "$stdout_file"
assert_not_contains "safe to update" "$stdout_file"

setup_case localized-inconsistent inconsistent-result
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    run_status 1 upgrade-all
assert_exact_line "  操作結果: 不整合" "$stdout_file"
assert_exact_line "  パッケージ状態の観測: 未検証" "$stdout_file"
assert_exact_line "    診断: 内部不整合" "$stdout_file"

if [ "$case_count" -ne 251 ]; then
    echo "upgrade-all command test scenario count drifted: $case_count" >&2
    exit 1
fi
echo "upgrade-all command integration tests: $case_count scenarios passed"
