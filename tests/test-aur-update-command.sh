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
    command_log=$case_dir/commands.log
    config_file=$case_dir/config.toml

    mkdir -p \
        "$case_dir/home" "$case_dir/xdg-config" \
        "$case_dir/xdg-state" "$case_dir/xdg-cache" \
        "$case_dir/work"
    chmod 0700 "$case_dir/xdg-config"
    : > "$stdout_file"
    : > "$stderr_file"
    : > "$command_log"
    printf '%s\n' 'schema_version = 1' > "$config_file"

    export HOME=$case_dir/home
    export XDG_CONFIG_HOME=$case_dir/xdg-config
    export XDG_STATE_HOME=$case_dir/xdg-state
    export XDG_CACHE_HOME=$case_dir/xdg-cache
    export MOGUET_TEST_CONFIG_FILE=$config_file
    export MOGUET_TEST_COMMAND_LOG=$command_log
    export MOGUET_TEST_AUR_UPDATE_SCENARIO=$scenario_name
    export MOGUET_TEST_PACMAN_EXIT_CODE=91
    export MOGUET_TEST_SUDO_EXIT_CODE=92
    unset MOGUET_TEST_SYSTEM_AUR_PRESENTATION_CASE
    case_count=$((case_count + 1))
}

show_case_diagnostics() {
    echo "--- stdout ---" >&2
    sed -n '1,240p' "$stdout_file" >&2
    echo "--- stderr ---" >&2
    sed -n '1,240p' "$stderr_file" >&2
    echo "--- event log ---" >&2
    sed -n '1,280p' "$command_log" >&2
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

assert_output_count() {
    expected_count=$1
    expected=$2
    file=$3
    actual_count=$(validation_grep_count -Fc -- "$expected" "$file")
    if [ "$actual_count" -ne "$expected_count" ]; then
        fail_case "unexpected output count for '$expected': $actual_count (expected $expected_count)"
    fi
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

assert_cache_absent() {
    if [ -e "$XDG_CACHE_HOME/moguet" ]; then
        fail_case "invalid invocation initialized the Moguet cache"
    fi
}

assert_state_absent() {
    if [ -e "$XDG_STATE_HOME/moguet" ]; then
        fail_case "blocked invocation initialized Moguet state"
    fi
}

assert_pipeline_absent() {
    if grep -E '^(query|preflight|prepare|execute|reduce)( |$)' \
        "$command_log" >/dev/null; then
        fail_case "invalid or unrelated route entered the AUR update pipeline"
    fi
}

assert_no_real_package_command() {
    if grep -E '^(git|makepkg|pacman|sudo) ' "$command_log" >/dev/null; then
        fail_case "test scenario invoked a package command instead of the operation stub"
    fi
}

assert_no_external_mutation() {
    if grep -E '^(git|makepkg|pacman|sudo|external) ' "$command_log" >/dev/null; then
        fail_case "unexpected external mutation"
    fi
}

# First characterization: an empty installed foreign inventory is a successful
# no-op and must never fall through to pacman.
setup_case no-installed-foreign no-installed-foreign
run_status 0 upgrade-aur
assert_exact_line "AUR update: no updates" "$stdout_file"
assert_exact_line "query" "$command_log"
assert_exact_line "preflight" "$command_log"
assert_contains "prepare needed=false" "$command_log"
assert_exact_line "reduce execution=no" "$command_log"
assert_no_external_mutation
assert_cache_absent

# UpToDate / NonAurForeign are successful skips and never reach the runner.
setup_case all-up-to-date all-up-to-date
run_status 0 upgrade-aur
assert_exact_line "AUR update: no updates" "$stdout_file"
assert_exact_line "up-to-date-pkg: skipped: up to date" "$stdout_file"
assert_not_contains "fixture" "$stderr_file"
assert_exact_line "reduce execution=no" "$command_log"
assert_no_external_mutation

setup_case non-aur-foreign non-aur-foreign
run_status 0 upgrade-aur
assert_exact_line "AUR update: no updates" "$stdout_file"
assert_exact_line "non-aur-pkg: skipped: non-AUR foreign" "$stdout_file"
assert_exact_line "reduce execution=no" "$command_log"
assert_no_external_mutation

setup_case devel-requires-check devel-requires-check
run_status 1 upgrade-aur
assert_exact_line "AUR update: blocked before execution" "$stdout_file"
assert_exact_line \
    "manual-check-git: devel update requires check: suffix candidate only" \
    "$stdout_file"
assert_contains \
    "  preflight issue: devel update requires check: fixture suffix candidate only; authoritative build provenance is unavailable" \
    "$stderr_file"
assert_not_contains "manual-check-git 1.0-1 ->" "$stdout_file"
assert_not_contains "Warning: Requires check" "$stdout_file"
assert_not_contains "Warning: Requires check" "$stderr_file"
assert_exact_line "reduce execution=no" "$command_log"
assert_no_external_mutation
assert_cache_absent
assert_state_absent

setup_case devel-requires-check-noconfirm devel-requires-check
run_status 1 --noconfirm upgrade-aur
assert_exact_line "AUR update: blocked before execution" "$stdout_file"
assert_exact_line \
    "manual-check-git: devel update requires check: suffix candidate only" \
    "$stdout_file"
assert_not_contains "prompt" "$command_log"
assert_not_contains "Warning: Requires check" "$stdout_file"
assert_not_contains "Warning: Requires check" "$stderr_file"
assert_no_external_mutation
assert_cache_absent
assert_state_absent

# The dry-run command uses the same filtered-operation owner but has its own
# production call site. The operation stub rejects any policy other than the
# explicit current BlockOperation contract.
setup_case devel-requires-check-dry-run devel-requires-check
run_status 1 --dry-run upgrade-aur
assert_contains "Unified plan:" "$stdout_file"
assert_contains "  Status: Blocked" "$stdout_file"
assert_exact_line "preflight" "$command_log"
assert_contains "prepare needed=false" "$command_log"
assert_no_external_mutation
assert_cache_absent
assert_state_absent

# Incomplete/unsupported preflight targets block every executable work item.
setup_case metadata-unavailable metadata-unavailable
run_status 1 upgrade-aur
assert_exact_line "AUR update: blocked before execution" "$stdout_file"
assert_exact_line \
    "metadata-pkg: incomplete: AUR metadata unavailable" "$stdout_file"
assert_contains \
    "  preflight issue: AUR metadata unavailable: fixture AUR metadata request failed" \
    "$stderr_file"
assert_not_contains "fixture AUR metadata request failed" "$stdout_file"
assert_exact_line "reduce execution=no" "$command_log"
assert_no_external_mutation
assert_cache_absent

setup_case version-comparison-unavailable version-comparison-unavailable
run_status 1 upgrade-aur
assert_exact_line "AUR update: blocked before execution" "$stdout_file"
assert_exact_line \
    "version-pkg: incomplete: version comparison unavailable" "$stdout_file"
assert_contains \
    "  preflight issue: version comparison unavailable: fixture version comparator failed" \
    "$stderr_file"
assert_no_external_mutation

setup_case unsupported-blocker unsupported-blocker
run_status 1 upgrade-aur
assert_exact_line "AUR update: blocked before execution" "$stdout_file"
assert_exact_line \
    "unsupported-pkg: unsupported: split package selection required" \
    "$stdout_file"
assert_contains \
    "  preflight issue: split package selection required: fixture split package needs selection" \
    "$stderr_file"
assert_no_external_mutation

setup_case incomplete-blocker incomplete-blocker
run_status 1 upgrade-aur
assert_exact_line "AUR update: blocked before execution" "$stdout_file"
assert_exact_line \
    "incomplete-pkg: incomplete: unresolved dependency" "$stdout_file"
assert_contains \
    "  preflight issue: unresolved dependency: fixture dependency could not be resolved" \
    "$stderr_file"
assert_no_external_mutation
assert_cache_absent

# This fixture deliberately retains an invocation with an issue.  The command
# must use is_prepared(), not optional presence, before consuming the runner.
setup_case preparation-failure preparation-failure
run_status 1 upgrade-aur
assert_exact_line "AUR update: blocked before execution" "$stdout_file"
assert_exact_line \
    "preparation-pkg: incomplete: source preference unavailable" "$stdout_file"
assert_contains \
    "  preparation issue: source preference unavailable: fixture source preference read failed" \
    "$stderr_file"
assert_not_contains "fixture source preference read failed" "$stdout_file"
assert_exact_line "reduce execution=no" "$command_log"
assert_no_external_mutation

setup_case preparation-warning preparation-warning
run_status 0 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "warning-pkg: updated" "$stdout_file"
assert_contains \
    "  preparation warning: warning-pkg: fixture source preference warning" \
    "$stdout_file"
assert_not_contains "fixture source preference warning" "$stderr_file"
assert_exact_line "reduce execution=yes" "$command_log"
assert_no_real_package_command

# Successful execution variants keep the inventory order in presentation.
setup_case all-updated all-updated
run_status 0 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "updated-pkg: updated" "$stdout_file"
assert_exact_line \
    "Reviewed-source outcome for PackageBase updated-pkg: initial full review accepted for exact upstream commit 2222222222222222222222222222222222222222." \
    "$stdout_file"
assert_exact_line \
    "Reviewed state for PackageBase updated-pkg was finalized as generation 1; build and install outcomes are reported separately." \
    "$stdout_file"
assert_exact_line \
    "Build input for PackageBase updated-pkg includes an invocation-local editor overlay on reviewed upstream commit 2222222222222222222222222222222222222222; it is not the exact reviewed commit tree." \
    "$stdout_file"
assert_not_contains "PackageBase result:" "$stdout_file"
assert_exact_line "query" "$command_log"
assert_line_before "query" "preflight" "$command_log"
prepare_line=$(grep -F "prepare needed=false" "$command_log" | sed -n '1p')
execute_line=$(grep -F "execute noedit=" "$command_log" | sed -n '1p')
assert_line_before "preflight" "$prepare_line" "$command_log"
assert_line_before "$prepare_line" "$execute_line" "$command_log"
assert_line_before "$execute_line" "external git clone fixture" "$command_log"
assert_line_before \
    "external git clone fixture" "external makepkg -sc fixture" "$command_log"
assert_line_before \
    "external makepkg -sc fixture" "external sudo pacman -U fixture" \
    "$command_log"
assert_line_before \
    "external sudo pacman -U fixture" "reduce execution=yes" "$command_log"
assert_no_real_package_command
assert_not_contains "pacman upgrade-aur" "$command_log"

setup_case all-no-change all-no-change
run_status 0 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "no-change-pkg: no change" "$stdout_file"
assert_exact_line \
    "Reviewed-source outcome for PackageBase no-change-pkg: exact upstream commit 2222222222222222222222222222222222222222 was already reviewed; no review prompt or state rewrite occurred." \
    "$stdout_file"
assert_exact_line \
    "Reviewed state for PackageBase no-change-pkg remains at generation 4; build and install outcomes are reported separately." \
    "$stdout_file"
assert_not_contains "PackageBase result:" "$stdout_file"

setup_case updated-no-change-mixed updated-no-change-mixed
run_status 0 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "zeta-pkg: updated" "$stdout_file"
assert_exact_line "alpha-pkg: no change" "$stdout_file"
assert_contains \
    "Reviewed-source outcome for PackageBase zeta-pkg: update review accepted" \
    "$stdout_file"
assert_exact_line \
    "Reviewed-source outcome for PackageBase alpha-pkg: --nodiff skipped review acceptance; this invocation has compatibility-only source authority, and reviewed state was not advanced." \
    "$stdout_file"
assert_line_before \
    "zeta-pkg: updated" "alpha-pkg: no change" "$stdout_file"
assert_line_before \
    "Reviewed-source outcome for PackageBase zeta-pkg: update review accepted for exact upstream commit 2222222222222222222222222222222222222222." \
    "Reviewed-source outcome for PackageBase alpha-pkg: --nodiff skipped review acceptance; this invocation has compatibility-only source authority, and reviewed state was not advanced." \
    "$stdout_file"

# PackageBase detail is child-authoritative and only appears when the ordinary
# singular summary cannot explain the selected artifact set.
setup_case split-child-success split-child-success
run_status 0 upgrade-aur
assert_exact_line "split-cli: updated" "$stdout_file"
assert_exact_line "PackageBase result: split-suite" "$stdout_file"
assert_exact_line \
    "  required child: split-cli -> split-cli 2.4.1-3 (explicit): installed / updated" \
    "$stdout_file"
assert_line_before \
    "split-cli: updated" "PackageBase result: split-suite" "$stdout_file"

setup_case multiple-child-mixed multiple-child-mixed
run_status 0 upgrade-aur
assert_exact_line "split-main: updated" "$stdout_file"
assert_exact_line "PackageBase result: split-suite" "$stdout_file"
assert_exact_line \
    "  required child: split-main -> split-main 3.7.0-2 (explicit): installed / updated" \
    "$stdout_file"
assert_exact_line \
    "  required child: split-dependency -> split-dependency 3.7.0-2 (dependency): skipped as needed / no change" \
    "$stdout_file"
assert_exact_line \
    "  produced artifact: split-sibling 3.7.0-2 (not selected; not installed)" \
    "$stdout_file"
assert_exact_line \
    "  produced artifact: split-suite-debug 3.7.0-2 (not selected; not installed)" \
    "$stdout_file"
assert_line_before \
    "  required child: split-main -> split-main 3.7.0-2 (explicit): installed / updated" \
    "  required child: split-dependency -> split-dependency 3.7.0-2 (dependency): skipped as needed / no change" \
    "$stdout_file"
assert_line_before \
    "  produced artifact: split-sibling 3.7.0-2 (not selected; not installed)" \
    "  produced artifact: split-suite-debug 3.7.0-2 (not selected; not installed)" \
    "$stdout_file"
assert_not_contains "required child: split-sibling" "$stdout_file"
assert_not_contains "required child: split-suite-debug" "$stdout_file"

setup_case transaction-failure transaction-failure
run_status 1 upgrade-aur
assert_contains \
    "Reviewed-source outcome for PackageBase tx-suite: update review accepted" \
    "$stdout_file"
assert_exact_line \
    "Reviewed state for PackageBase tx-suite was finalized as generation 11; build and install outcomes are reported separately." \
    "$stdout_file"
assert_exact_line \
    "tx-main: failed: package transaction failed (exit code 73)" "$stdout_file"
assert_exact_line \
    "tx-later: not attempted: prior work item stopped" "$stdout_file"
assert_exact_line \
    "  required child: tx-main (explicit): no successful outcome" "$stdout_file"
assert_exact_line \
    "  required child: tx-dependency (dependency): no successful outcome" \
    "$stdout_file"
assert_contains \
    "execution failure for PackageBase tx-suite: package transaction failed (exit code 73)" \
    "$stderr_file"
assert_contains \
    "transaction attempt: tx-main 4.0.0-1 (explicit)" "$stderr_file"
assert_contains \
    "transaction attempt: tx-dependency 4.0.0-1 (dependency)" "$stderr_file"
assert_not_contains "required child: tx-main ->" "$stdout_file"
assert_not_contains "/private/workspace/aur-cli-secret" "$stdout_file"
assert_not_contains "/private/workspace/aur-cli-secret" "$stderr_file"
assert_not_contains "/private/artifacts/" "$stdout_file"
assert_not_contains "/private/artifacts/" "$stderr_file"

setup_case transaction-process-exception transaction-process-exception
run_status 1 upgrade-aur
assert_exact_line \
    "tx-main: failed: package transaction process exception" "$stdout_file"
assert_contains \
    "execution failure for PackageBase tx-suite: package transaction process exception" \
    "$stderr_file"
assert_not_contains "exit code" "$stdout_file"
assert_not_contains "exit code" "$stderr_file"
assert_not_contains "/private/workspace/aur-cli-secret" "$stdout_file"
assert_not_contains "/private/workspace/aur-cli-secret" "$stderr_file"

setup_case cleanup-mixed cleanup-mixed
run_status 1 upgrade-aur
assert_exact_line \
    "cleanup-main: updated, but cleanup failed" "$stdout_file"
assert_exact_line "PackageBase result: cleanup-suite" "$stdout_file"
assert_exact_line \
    "  required child: cleanup-main -> cleanup-main 5.1.0-4 (explicit): installed / updated, but cleanup failed" \
    "$stdout_file"
assert_exact_line \
    "  required child: cleanup-dependency -> cleanup-dependency 5.1.0-4 (dependency): skipped as needed / no change, but cleanup failed" \
    "$stdout_file"
assert_exact_line \
    "  produced artifact: cleanup-suite-debug 5.1.0-4 (not selected; not installed)" \
    "$stdout_file"
assert_exact_line \
    "cleanup-later: not attempted: prior work item stopped" "$stdout_file"
assert_contains \
    "execution failure for PackageBase cleanup-suite: cleanup failure after successful package transaction" \
    "$stderr_file"
assert_not_contains "/private/workspace/aur-cli-secret" "$stdout_file"
assert_not_contains "/private/workspace/aur-cli-secret" "$stderr_file"

setup_case unknown-child-result unknown-child-result
run_status 1 upgrade-aur
assert_not_contains "AUR update:" "$stdout_file"
assert_not_contains "defensive-split" "$stdout_file"
assert_contains "Unknown AUR child execution status." "$stderr_file"

setup_case incoherent-child-result incoherent-child-result
run_status 1 upgrade-aur
assert_not_contains "AUR update:" "$stdout_file"
assert_not_contains "defensive-split" "$stdout_file"
assert_contains \
    "Completed AUR child has no selected artifact identity." "$stderr_file"

# Fail-fast results must retain the decisive typed failure and partial state.
setup_case ordinary-execution-failure ordinary-execution-failure
run_status 1 upgrade-aur
assert_exact_line \
    "AUR update: stopped after work-item failure" "$stdout_file"
assert_exact_line \
    "failed-pkg: failed: build or install failure" "$stdout_file"
assert_contains \
    "  execution failure: build or install failure" \
    "$stderr_file"

# Reviewed-state publication ambiguity is a post-commit STOP, not a generic
# pre-publication failure or a compatibility retry. makepkg/pacman never start.
setup_case reviewed-published-uncertain reviewed-published-uncertain
run_status 1 upgrade-aur
assert_exact_line \
    "uncertain-pkg: failed: Reviewed source state publication reached a post-commit ambiguity; the build was not started, and automatic retry, rollback, and compatibility fallback are forbidden." \
    "$stdout_file"
assert_contains \
    "execution failure for PackageBase uncertain-pkg: Reviewed source state publication reached a post-commit ambiguity; the build was not started, and automatic retry, rollback, and compatibility fallback are forbidden." \
    "$stderr_file"
assert_exact_line "external git clone fixture" "$command_log"
assert_not_contains "external makepkg" "$command_log"
assert_not_contains "external sudo pacman -U" "$command_log"
assert_not_contains "compatibility-only" "$stdout_file"
assert_not_contains "retrying" "$stdout_file"

# A later makepkg failure retains and displays the already-published review
# outcome separately; install remains not attempted.
setup_case reviewed-build-failure reviewed-build-failure
run_status 1 upgrade-aur
assert_contains \
    "Reviewed-source outcome for PackageBase reviewed-build-pkg: update review accepted" \
    "$stdout_file"
assert_exact_line \
    "Reviewed state for PackageBase reviewed-build-pkg was finalized as generation 12; build and install outcomes are reported separately." \
    "$stdout_file"
assert_exact_line \
    "reviewed-build-pkg: failed: source build failure" "$stdout_file"
assert_contains \
    "execution failure for PackageBase reviewed-build-pkg: source build failure" \
    "$stderr_file"
assert_exact_line "external makepkg -sc fixture" "$command_log"
assert_not_contains "external sudo pacman -U" "$command_log"

# Selected repository provider failure is an invocation phase, not a work-item
# failure, and it stops before source checkout/build/install.
setup_case provider-transaction-failure provider-transaction-failure
run_status 1 upgrade-aur
assert_exact_line \
    "AUR update: stopped after repository provider transaction failure" \
    "$stdout_file"
assert_exact_line \
    "provider-pkg: not attempted: repository provider transaction failed" \
    "$stdout_file"
assert_contains \
    "selected repository provider transaction failed: fixture repository provider transaction failure" \
    "$stderr_file"
assert_exact_line \
    "external sudo pacman -S provider fixture" "$command_log"
assert_not_contains "external git clone fixture" "$command_log"
assert_not_contains "external makepkg -sc fixture" "$command_log"
assert_not_contains "external sudo pacman -U fixture" "$command_log"
assert_no_real_package_command

setup_case updated-cleanup-failure updated-cleanup-failure
run_status 1 upgrade-aur
assert_exact_line \
    "AUR update: stopped after cleanup failure" "$stdout_file"
assert_exact_line \
    "updated-cleanup-pkg: updated, but cleanup failed" "$stdout_file"
assert_contains \
    "  execution failure: cleanup failure after successful package transaction" \
    "$stderr_file"
assert_contains \
    "AUR update partially completed before failure." "$stdout_file"
assert_contains \
    "AUR update cleanup failed after a package transaction." "$stdout_file"

setup_case no-change-cleanup-failure no-change-cleanup-failure
run_status 1 upgrade-aur
assert_exact_line \
    "AUR update: stopped after cleanup failure" "$stdout_file"
assert_exact_line \
    "no-change-cleanup-pkg: no package change, but cleanup failed" "$stdout_file"
assert_contains \
    "  execution failure: cleanup failure after successful package transaction" \
    "$stderr_file"
assert_contains \
    "AUR update cleanup failed after a package transaction." "$stdout_file"
assert_not_contains \
    "AUR update partially completed before failure." "$stdout_file"

setup_case partial-completion partial-completion
run_status 1 upgrade-aur
assert_exact_line \
    "AUR update: stopped after work-item failure" "$stdout_file"
assert_exact_line "first-pkg: updated" "$stdout_file"
assert_exact_line \
    "failed-pkg: failed: build or install failure" "$stdout_file"
assert_exact_line \
    "later-pkg: not attempted: prior work item stopped" "$stdout_file"
assert_line_before "first-pkg: updated" \
    "failed-pkg: failed: build or install failure" "$stdout_file"
assert_line_before "failed-pkg: failed: build or install failure" \
    "later-pkg: not attempted: prior work item stopped" "$stdout_file"
assert_contains \
    "  execution failure: build or install failure" \
    "$stderr_file"
assert_not_contains \
    "execution failure: prior work item stopped" "$stderr_file"
assert_not_contains "diagnostic unavailable" "$stderr_file"
assert_contains \
    "AUR update partially completed before failure." "$stdout_file"
assert_contains \
    "AUR update has targets that were not attempted." "$stdout_file"

setup_case reducer-inconsistency reducer-inconsistency
run_status 1 upgrade-aur
assert_exact_line "AUR update: inconsistent result" "$stdout_file"
assert_contains \
    "  reduction issue: execution: unknown execution update plan index: fixture reducer mismatch" \
    "$stderr_file"
assert_not_contains "fixture reducer mismatch" "$stdout_file"

# Recoverable query diagnostics outlive reduction and force failure even when
# the reducer itself reports Completed.
setup_case query-recoverable-failure query-recoverable-failure
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "query-survivor: updated" "$stdout_file"
assert_contains \
    "AUR update query failure for query-broken, query-also-broken: fixture RPC timeout" \
    "$stderr_file"
assert_contains \
    "AUR update completed, but query failures were reported." "$stderr_file"
assert_not_contains "fixture RPC timeout" "$stdout_file"

# POLICY(#267): The following fixtures intentionally retain a Completed
# operation status alongside one typed abnormal field.  Production reduction
# does not create these combinations; the CLI boundary must still fail closed.
setup_case completed-preparation-issue completed-preparation-issue
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "defensive-pkg: updated" "$stdout_file"
assert_contains \
    "  preparation issue: source preference unavailable: fixture completed preparation issue" \
    "$stderr_file"

setup_case completed-reduction-issue completed-reduction-issue
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "defensive-pkg: updated" "$stdout_file"
assert_contains \
    "  reduction issue: execution: unknown execution update plan index: fixture completed reduction issue" \
    "$stderr_file"

setup_case completed-unsupported-target completed-unsupported-target
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line \
    "defensive-pkg: unsupported: split package selection required" \
    "$stdout_file"
assert_contains \
    "  preflight issue: split package selection required: fixture completed unsupported target" \
    "$stderr_file"

setup_case completed-incomplete-target completed-incomplete-target
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line \
    "defensive-pkg: incomplete: unresolved dependency" "$stdout_file"
assert_contains \
    "  preflight issue: unresolved dependency: fixture completed incomplete target" \
    "$stderr_file"

setup_case completed-execution-failure completed-execution-failure
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "defensive-pkg: updated" "$stdout_file"
assert_contains \
    "  execution failure for PackageBase defensive-pkg: build or install failure" \
    "$stderr_file"

# Invocation-level stopped states must independently fail even when every
# target and work-item field looks successful.
setup_case \
    completed-invocation-execution-failure \
    completed-invocation-execution-failure
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "defensive-pkg: updated" "$stdout_file"

setup_case \
    completed-invocation-cleanup-failure \
    completed-invocation-cleanup-failure
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "defensive-pkg: updated" "$stdout_file"

setup_case \
    completed-missing-invocation-status \
    completed-missing-invocation-status
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line "defensive-pkg: updated" "$stdout_file"

setup_case completed-cleanup-failure completed-cleanup-failure
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line \
    "defensive-pkg: updated, but cleanup failed" "$stdout_file"
assert_contains \
    "  execution failure: cleanup failure after successful package transaction" \
    "$stderr_file"
assert_exact_line \
    "AUR update cleanup failed after a package transaction." "$stdout_file"

setup_case completed-not-attempted completed-not-attempted
run_status 1 upgrade-aur
assert_exact_line "AUR update: completed" "$stdout_file"
assert_exact_line \
    "defensive-pkg: not attempted: prior work item stopped" "$stdout_file"
assert_exact_line \
    "AUR update has targets that were not attempted." "$stdout_file"
assert_not_contains \
    "execution failure: prior work item stopped" "$stderr_file"
assert_not_contains "diagnostic unavailable" "$stderr_file"

# Typed skip/rebuild options and an equivalent alias reach both boundaries once.
setup_case option-propagation-rebuild options-propagation
run_status 0 --noedit upgrade-aur --nodiff --noconfirm \
    --build-mode=rebuild --rebuild
assert_exact_line \
    "prepare needed=false noedit=true nodiff=true noconfirm=true rebuild=true cleanbuild=false rmdeps=false" \
    "$command_log"
assert_exact_line \
    "execute noedit=true nodiff=true noconfirm=true rebuild=true cleanbuild=false rmdeps=false" \
    "$command_log"

# Prompt/clean final values and the cleanbuild alias use the opposite legacy projection.
setup_case option-propagation-clean options-propagation
run_status 0 --edit upgrade-aur --diff --build-mode=clean --cleanbuild
assert_exact_line \
    "prepare needed=false noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=true rmdeps=false" \
    "$command_log"
assert_exact_line \
    "execute noedit=false nodiff=false noconfirm=false rebuild=false cleanbuild=true rmdeps=false" \
    "$command_log"

# Misuse is rejected before query and before default cache/log initialization.
setup_case rmdeps-rejection all-updated
run_status 1 --rmdeps upgrade-aur
assert_contains \
    "Separated build/install does not support --rmdeps." "$stderr_file"
assert_pipeline_absent
assert_cache_absent

setup_case needed-rejection all-updated
run_status 1 upgrade-aur --needed
assert_contains "Unsupported upgrade-aur option: --needed" "$stderr_file"
assert_pipeline_absent
assert_cache_absent

setup_case positional-target-rejection all-updated
run_status 1 upgrade-aur unexpected-target
assert_contains \
    "Operation upgrade-aur does not accept target operands." "$stderr_file"
assert_pipeline_absent
assert_cache_absent

setup_case aur-selector-rejection all-updated
run_status 1 --aur upgrade-aur
assert_contains \
    "--aur is not supported for operation upgrade-aur." "$stderr_file"
assert_pipeline_absent
assert_cache_absent

setup_case repo-selector-rejection all-updated
run_status 1 upgrade-aur --repo
assert_contains \
    "--repo is not supported for operation upgrade-aur." "$stderr_file"
assert_pipeline_absent
assert_cache_absent

# Repository-only system routes remain exact and never enter the AUR pipeline.
setup_case syu-repository-routing no-installed-foreign
export MOGUET_TEST_SUDO_EXIT_CODE=0
run_status 0 -Syu --repo
assert_exact_line "sudo pacman -Syu" "$command_log"
assert_pipeline_absent

setup_case syu-inconsistent-presentation no-installed-foreign
export MOGUET_TEST_SYSTEM_AUR_PRESENTATION_CASE=inconsistent
run_status 1 -Syu
assert_contains \
    "The repository and AUR update result is inconsistent; no success was reported." \
    "$stderr_file"
assert_not_contains "The repository system upgrade completed." "$stdout_file"
assert_not_contains \
    "The repository system upgrade and normal AUR update completed." \
    "$stdout_file"
assert_pipeline_absent
assert_no_external_mutation

setup_case syu-repository-exception-diagnostic no-installed-foreign
export MOGUET_TEST_SYSTEM_AUR_PRESENTATION_CASE=repository-exception
run_status 1 -Syu
assert_contains "The repository system upgrade failed." "$stderr_file"
assert_contains \
    "Execution failure: fixture repository exception\\x0A\\x1Bunsafe\\x5Cdetail [source=pacman]" \
    "$stderr_file"
assert_output_count 1 "fixture repository exception" "$stderr_file"
assert_exact_line "The AUR update was not attempted." "$stdout_file"
assert_pipeline_absent
assert_no_external_mutation

setup_case syu-preparation-exception-diagnostic no-installed-foreign
export MOGUET_TEST_SYSTEM_AUR_PRESENTATION_CASE=preparation-exception
run_status 1 -Syu
assert_exact_line "The repository system upgrade completed." "$stdout_file"
assert_contains \
    "The repository system upgrade completed, but the AUR update was blocked before execution." \
    "$stderr_file"
assert_contains \
    "Blocked: fixture preparation exception\\x0A\\x1Bunsafe\\x5Cdetail [source=aur]" \
    "$stderr_file"
assert_output_count 1 "fixture preparation exception" "$stderr_file"
assert_not_contains "The AUR update was not attempted." "$stdout_file"
assert_contains \
    "The completed repository system upgrade was not rolled back." \
    "$stdout_file"
assert_pipeline_absent
assert_no_external_mutation

setup_case syu-execution-exception-diagnostic no-installed-foreign
export MOGUET_TEST_SYSTEM_AUR_PRESENTATION_CASE=execution-exception
run_status 1 -Syu
assert_exact_line "The repository system upgrade completed." "$stdout_file"
assert_contains \
    "The repository and AUR update result is inconsistent; no success was reported." \
    "$stderr_file"
assert_contains \
    "The repository system upgrade completed, but the AUR update failed." \
    "$stderr_file"
assert_contains \
    "Internal inconsistency: fixture execution exception\\x0A\\x1Bunsafe\\x5Cdetail [source=aur]" \
    "$stderr_file"
assert_output_count 1 "fixture execution exception" "$stderr_file"
assert_contains \
    "The completed repository system upgrade was not rolled back." \
    "$stdout_file"
assert_pipeline_absent
assert_no_external_mutation

setup_case syu-child-diagnostic-owned-once query-recoverable-failure
export MOGUET_TEST_SYSTEM_AUR_PRESENTATION_CASE=child-query-failure
run_status 1 -Syu
assert_exact_line "The repository system upgrade completed." "$stdout_file"
assert_exact_line "AUR update: completed" "$stdout_file"
assert_contains \
    "The repository system upgrade completed, but the fresh AUR update query failed." \
    "$stderr_file"
assert_contains "fixture RPC timeout" "$stderr_file"
assert_output_count 1 "fixture RPC timeout" "$stderr_file"
assert_not_contains \
    "Query failure: fixture RPC timeout [source=aur]" "$stderr_file"

setup_case upgrade-routing-unchanged no-installed-foreign
export MOGUET_TEST_SUDO_EXIT_CODE=0
run_status 0 upgrade
assert_exact_line "sudo pacman -Syu" "$command_log"
assert_pipeline_absent

if [ "$case_count" -ne 55 ]; then
    fail_case "internal test case count changed: $case_count"
fi
echo "AUR update command integration tests passed ($case_count cases)."
