#!/bin/sh

set -eu

repo_root=$(CDPATH='' cd "$(dirname "$0")/../.." && pwd)
. "$repo_root/scripts/validation-status.sh"
fixture_root=$repo_root/containers/arch-live-validation/fixtures/local-package
fixture_pkgbuild=$fixture_root/PKGBUILD
fixture_contract=$fixture_root/contract.env
fixture_expected_srcinfo=$fixture_root/expected.srcinfo
fixture_payload_authority=$fixture_root/payload-authority.tsv
pty_runner=$repo_root/tests/run-with-pty.py
production_moguet=$repo_root/moguet
case_root=$HOME/live-provider-cases
sentinel_log_root=/var/log/moguet-live-validation
sentinel_status=86

current_case=preflight
current_output=

fail() {
    printf 'arch-live-provider: FAIL: %s\n' "$*" >&2
    if [ -n "$current_output" ] && [ -f "$current_output" ]; then
        printf 'arch-live-provider: case log (%s):\n' "$current_output" >&2
        sed -n '1,320p' "$current_output" >&2
    fi
    exit 1
}

assert_regular_non_symlink() {
    checked_path=$1
    checked_label=$2
    if [ ! -f "$checked_path" ] || [ -L "$checked_path" ]; then
        fail "$checked_label must be a regular non-symlink: $checked_path"
    fi
}

assert_contains() {
    expected_text=$1
    checked_file=$2
    if ! grep -F -- "$expected_text" "$checked_file" >/dev/null; then
        fail "missing expected output: $expected_text"
    fi
}

assert_not_contains() {
    unexpected_text=$1
    checked_file=$2
    if grep -F -- "$unexpected_text" "$checked_file" >/dev/null; then
        fail "unexpected output: $unexpected_text"
    fi
}

assert_count() {
    expected_count=$1
    pattern=$2
    checked_file=$3
    actual_count=$(validation_grep_count -F -c -- "$pattern" "$checked_file")
    if [ "$actual_count" -ne "$expected_count" ]; then
        fail "expected $expected_count occurrences of '$pattern', observed $actual_count"
    fi
}

assert_exact_message_count() {
    expected_count=$1
    expected_message=$2
    checked_file=$3
    actual_count=$(python3 - "$expected_message" "$checked_file" <<'PY'
from pathlib import Path
import re
import sys

expected = sys.argv[1].encode("utf-8")
output = Path(sys.argv[2]).read_bytes()
ansi_csi = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
ansi_osc = re.compile(rb"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
count = 0
for line in output.splitlines():
    line = ansi_osc.sub(b"", ansi_csi.sub(b"", line))
    if line == expected or line.endswith(b" " + expected):
        count += 1
print(count)
PY
)
    if [ "$actual_count" -ne "$expected_count" ]; then
        fail "expected $expected_count exact occurrences of '$expected_message', observed $actual_count"
    fi
}

tracked_fixture_manifest_raw() {
    (
        cd "$fixture_root" || exit $?
        sha256sum \
            PKGBUILD contract.env expected.srcinfo payload-authority.tsv ||
            exit $?
        find . -mindepth 1 -maxdepth 1 -printf '%y %m %f\n' || exit $?
    )
}

tracked_fixture_manifest() {
    manifest_raw=$(mktemp "$case_root/tracked-fixture.raw.XXXXXX")
    manifest_sorted=$(mktemp "$case_root/tracked-fixture.sorted.XXXXXX")
    if validation_capture_sorted_output "$manifest_raw" "$manifest_sorted" \
        tracked_fixture_manifest_raw; then
        cat "$manifest_sorted" || return $?
        rm -f "$manifest_raw" "$manifest_sorted" || :
        return 0
    else
        manifest_status=$?
    fi
    fail "tracked fixture manifest failed with status $manifest_status; raw=$manifest_raw"
}

package_database_paths_raw() {
    (
        cd /var/lib/pacman/local || exit $?
        find . -mindepth 1 -printf '%P\n' || exit $?
    )
}

sha256_file() {
    if checksum_output=$(sha256sum -- "$1"); then
        printf '%s\n' "${checksum_output%% *}"
        return 0
    else
        return $?
    fi
}

package_database_rows() {
    sorted_paths=$1
    (
        cd /var/lib/pacman/local || exit $?
        while IFS= read -r relative_path; do
            database_path=./$relative_path
            entry_type=$(find "$database_path" -prune -printf '%y') || exit $?
            entry_mode=$(stat -c '%a' -- "$database_path") || exit $?
            entry_owner_group=$(stat -c '%u:%g' -- "$database_path") || exit $?
            content_hash=-
            if [ "$entry_type" = f ]; then
                content_hash=$(sha256_file "$database_path") || exit $?
            fi
            printf '%s\ttype=%s\tmode=%s\towner_group=%s\tsha256=%s\n' \
                "$relative_path" "$entry_type" "$entry_mode" \
                "$entry_owner_group" "$content_hash" || exit $?
        done <"$sorted_paths"
    )
}

package_database_manifest() {
    paths_raw=$(mktemp "$case_root/package-db-paths.raw.XXXXXX")
    paths_sorted=$(mktemp "$case_root/package-db-paths.sorted.XXXXXX")
    rows_file=$(mktemp "$case_root/package-db-rows.XXXXXX")
    if validation_capture_sorted_output "$paths_raw" "$paths_sorted" \
        package_database_paths_raw; then
        :
    else
        manifest_status=$?
        fail "package database path producer failed with status $manifest_status; raw=$paths_raw"
    fi
    if validation_capture_output "$rows_file" \
        package_database_rows "$paths_sorted"; then
        :
    else
        manifest_status=$?
        fail "package database row producer failed with status $manifest_status; partial=$rows_file"
    fi
    if manifest_hash=$(sha256_file "$rows_file"); then
        printf '%s  -\n' "$manifest_hash"
    else
        manifest_status=$?
        fail "package database hash producer failed with status $manifest_status"
    fi
    rm -f "$paths_raw" "$paths_sorted" "$rows_file" || :
}

source_manifest() {
    manifest_root=$1
    (
        cd "$manifest_root" || exit $?
        sha256sum PKGBUILD .SRCINFO || exit $?
        stat -c '%u:%g:%a:%F:%n' PKGBUILD .SRCINFO || exit $?
    )
}

assert_sentinel_log() {
    checked_case=$1
    shift
    checked_log=$sentinel_log_root/$checked_case/sentinel.argv
    assert_regular_non_symlink "$checked_log" "sentinel argv log for $checked_case"
    checked_metadata=$(stat -c '%U:%G:%a:%F' "$checked_log")
    if [ "$checked_metadata" != 'root:moguet-validation:640:regular file' ]; then
        fail "unsafe sentinel log ownership or mode for $checked_case: $checked_metadata"
    fi
    if ! python3 -c '
import pathlib
import sys

actual = pathlib.Path(sys.argv[1]).read_bytes().split(b"\0")
if not actual or actual[-1] != b"":
    raise SystemExit("sentinel log is not NUL-terminated")
actual = [value.decode("utf-8", "strict") for value in actual[:-1]]
expected = sys.argv[2:]
if actual != expected:
    raise SystemExit(f"sentinel argv mismatch: actual={actual!r} expected={expected!r}")
' "$checked_log" "$@"; then
        fail "byte-safe sentinel argv mismatch for $checked_case"
    fi
}

assert_sentinel_absent() {
    checked_case=$1
    if [ -e "$sentinel_log_root/$checked_case/sentinel.argv" ]; then
        fail "transaction sentinel was invoked before an allowed selection: $checked_case"
    fi
}

run_sentinel_policy_case() {
    policy_case=$1
    expected_verdict=$2
    shift 2
    current_case=$policy_case
    current_output=$case_root/$policy_case.output
    policy_status=0
    if env MOGUET_LIVE_SENTINEL_CASE="$policy_case" \
        sudo pacman "$@" >"$current_output" 2>&1; then
        policy_status=0
    else
        policy_status=$?
    fi
    if [ "$policy_status" -ne "$sentinel_status" ]; then
        fail "sentinel policy case returned $policy_status instead of $sentinel_status"
    fi
    assert_contains "moguet-live-pacman-sentinel: $expected_verdict" "$current_output"
    assert_sentinel_log "$policy_case" sudo pacman "$@"
    printf '  sentinel policy: %s -> %s (status %s)\n' \
        "$policy_case" "$expected_verdict" "$policy_status"
}

assert_runtime_sentinel_contract() {
    sentinel_metadata=$(stat -c '%U:%G:%a:%F' /usr/bin/pacman)
    if [ "$sentinel_metadata" != 'root:root:555:regular file' ]; then
        fail "runtime pacman sentinel is not root-owned mode 0555: $sentinel_metadata"
    fi
    if [ -w /usr/bin/pacman ]; then
        fail 'validation user can write the runtime pacman sentinel'
    fi
    real_pacman_metadata=$(stat -c '%U:%G:%a:%F' \
        /usr/libexec/moguet-live-validation/pacman.real)
    if [ "$real_pacman_metadata" != 'root:root:700:regular file' ]; then
        fail "isolated real pacman has unsafe ownership or mode: $real_pacman_metadata"
    fi
    if [ -x /usr/libexec/moguet-live-validation/pacman.real ]; then
        fail 'validation user can execute the isolated real pacman'
    fi
    printf '%s\n' ':: live provider sentinel policy preflight'
    run_sentinel_policy_case \
        sentinel-accept-first-provider 'accepted and blocked' \
        -S --asdeps --needed -- "$first_provider_target"
    run_sentinel_policy_case \
        sentinel-accept-second-provider-noconfirm 'accepted and blocked' \
        -S --asdeps --needed --noconfirm -- "$second_provider_target"
    run_sentinel_policy_case \
        sentinel-reject-pacman-u 'rejected argv' \
        -U -- /tmp/package.pkg.tar.zst
    run_sentinel_policy_case \
        sentinel-reject-remove 'rejected argv' \
        -R -- "$first_provider_target"
    run_sentinel_policy_case \
        sentinel-reject-syu 'rejected argv' \
        -Syu --noconfirm
    run_sentinel_policy_case \
        sentinel-reject-multiple 'rejected argv' \
        -S --asdeps --needed -- "$first_provider_target" "$second_provider_target"
    run_sentinel_policy_case \
        sentinel-reject-unqualified 'rejected argv' \
        -S --asdeps --needed -- "$first_provider"
    run_sentinel_policy_case \
        sentinel-reject-option 'rejected argv' \
        -S --asdeps --needed --overwrite '*' -- "$first_provider_target"
    run_sentinel_policy_case \
        sentinel-reject-unknown-target 'rejected argv' \
        -S --asdeps --needed -- extra/unknown-provider
}

prepare_case() {
    current_case=$1
    case_directory=$(mktemp -d "$case_root/$current_case.XXXXXX")
    case_source=$case_directory/local-package
    case_config=$case_directory/xdg-config
    case_cache=$case_directory/xdg-cache
    case_state=$case_directory/xdg-state
    case_output=$case_directory/moguet.output
    current_output=$case_output
    mkdir -m 0755 "$case_source"
    mkdir -m 0700 "$case_config" "$case_cache" "$case_state"
    cp "$fixture_pkgbuild" "$case_source/PKGBUILD"
    (
        cd "$case_source"
        makepkg --printsrcinfo > .SRCINFO
    )
    assert_regular_non_symlink "$case_source/PKGBUILD" 'case PKGBUILD'
    assert_regular_non_symlink "$case_source/.SRCINFO" 'generated case .SRCINFO'
    cmp -s "$fixture_expected_srcinfo" "$case_source/.SRCINFO" ||
        fail 'generated case .SRCINFO differs from the independent fixture projection'
    source_owner=$(stat -c '%u' "$case_source/.SRCINFO")
    if [ "$source_owner" -ne "$(id -u)" ]; then
        fail "generated .SRCINFO is not validation-user-owned: $source_owner"
    fi
    source_mode=$(stat -c '%a' "$case_source/.SRCINFO")
    if [ $((0$source_mode & 022)) -ne 0 ]; then
        fail "generated .SRCINFO is group/other writable: $source_mode"
    fi
    case_source_before=$(source_manifest "$case_source")
    case_package_database_before=$(package_database_manifest)
    assert_sentinel_absent "$current_case"
}

run_pty_case() {
    input_file=$1
    shift
    case_status=0
    if env \
        HOME="$HOME" \
        XDG_CONFIG_HOME="$case_config" \
        XDG_CACHE_HOME="$case_cache" \
        XDG_STATE_HOME="$case_state" \
        MOGUET_LIVE_SENTINEL_CASE="$current_case" \
        python3 "$pty_runner" --timeout 90 -- \
            "$production_moguet" "$@" \
            <"$input_file" >"$case_output" 2>&1; then
        case_status=0
    else
        case_status=$?
    fi
    validation_assert_status "live-provider-$current_case" 1 "$case_status" \
        "$case_output" "$case_output" python3 "$pty_runner" --timeout 90 -- \
        "$production_moguet" "$@" ||
        fail 'provider case returned a non-canonical business status'
}

run_non_tty_case() {
    piped_value=$1
    shift
    non_tty_input=$case_directory/non-tty.input
    printf '%s\n' "$piped_value" >"$non_tty_input"
    case_status=0
    if env \
        HOME="$HOME" \
        XDG_CONFIG_HOME="$case_config" \
        XDG_CACHE_HOME="$case_cache" \
        XDG_STATE_HOME="$case_state" \
        MOGUET_LIVE_SENTINEL_CASE="$current_case" \
        "$production_moguet" "$@" \
        <"$non_tty_input" >"$case_output" 2>&1; then
        case_status=0
    else
        case_status=$?
    fi
    validation_assert_status "live-provider-$current_case" 1 "$case_status" \
        "$case_output" "$case_output" "$production_moguet" "$@" ||
        fail 'non-TTY provider case returned a non-canonical business status'
}

assert_blocked_status() {
    [ "$case_status" -eq 1 ] ||
        fail "case returned $case_status instead of canonical status 1"
}

assert_no_source_or_install_execution() {
    assert_not_contains "Running: 'git'" "$case_output"
    assert_not_contains "Running: 'makepkg'" "$case_output"
    assert_not_contains "Running: 'sudo' 'pacman' '-U'" "$case_output"
    workspace_probe=$case_directory/workspaces.raw
    if validation_capture_output "$workspace_probe" find "$case_cache" -type d \
        \( -name '.local-source-workspace~-*' \
        -o -name '.artifact-workspace~-*' \) -print; then
        :
    else
        workspace_status=$?
        fail "workspace absence producer failed with status $workspace_status"
    fi
    if [ -s "$workspace_probe" ]; then
        fail 'source or artifact workspace exists after blocked phase'
    fi
    if [ -e "$case_source/src" ] || [ -e "$case_source/pkg" ]; then
        fail 'local makepkg build directories were created'
    fi
}

assert_common_case_integrity() {
    case_source_after=$(source_manifest "$case_source")
    if [ "$case_source_after" != "$case_source_before" ]; then
        fail 'case-local PKGBUILD or generated .SRCINFO changed'
    fi
    case_package_database_after=$(package_database_manifest)
    if [ "$case_package_database_after" != "$case_package_database_before" ]; then
        fail 'container package installation state changed'
    fi
    tracked_fixture_after=$(tracked_fixture_manifest)
    if [ "$tracked_fixture_after" != "$tracked_fixture_before" ]; then
        fail 'tracked live fixture changed during an E2E case'
    fi
    if [ -e "$fixture_root/.SRCINFO" ]; then
        fail 'tracked live fixture gained .SRCINFO'
    fi
    assert_no_source_or_install_execution
}

parse_candidate_contract() {
    parsed_output=$1
    parsed_table=$2
    normalized_output=$parsed_output.normalized
    # The normal candidate rows share -Ss styling on a PTY.
    python3 - "$parsed_output" "$normalized_output" <<'PY_NORMALIZE'
from pathlib import Path
import re
import sys
raw = Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_bytes(re.sub(rb"\x1b\[[0-9;]*m", b"", raw).replace(b"\r", b""))
PY_NORMALIZE

    dependency_header_count=$(validation_grep_count -F -c \
        ":: Choose a provider for $REQUIRED_MAKE_DEPENDENCY" "$normalized_output")
    if [ "$dependency_header_count" -ne 1 ]; then
        fail "expected one provider header, observed $dependency_header_count"
    fi

    presented_count=$(validation_grep_count -E -c \
        '^[0-9]+\) ' "$normalized_output")
    awk '
/^[0-9]+\) / {
    number = $1
    sub(/\)$/, "", number)
    split($2, identity, "/")
    repository = identity[1]
    package_name = identity[2]
    source = repository == "aur" ? "AUR" : "repository"
    provided = component = ""
    for (field_index = 4; field_index <= NF; ++field_index) {
        if ($field_index == "[repository]") source = "repository"
        if ($field_index == "[provides:") {
            provided = $(field_index + 1)
            sub(/\]$/, "", provided)
            sub(/[<>=].*$/, "", provided)
        }
        if ($field_index == "[component:") {
            component = $(field_index + 1)
            sub(/\]$/, "", component)
        }
    }
    if (component != "") provided = component
    if (number !~ /^[0-9]+$/ || package_name == "" ||
        repository == "" || provided == "" || identity[3] != "") {
        exit 9
    }
    print number "\t" source "\t" package_name "\t" repository "\t" provided
}
' "$normalized_output" > "$parsed_table" ||
        fail 'provider candidate presentation could not be parsed safely'

    parsed_count=$(wc -l < "$parsed_table")
    if [ "$presented_count" -ne "$parsed_count" ] || [ "$parsed_count" -ne 2 ]; then
        fail "provider drift: expected exactly 2 parseable candidates, observed $presented_count/$parsed_count"
    fi
    awk -F '\t' '
        { numbers[$1]++ }
        END {
            for (number in numbers) number_count++
            exit NR == 2 && number_count == 2 &&
                numbers[1] == 1 && numbers[2] == 1 ? 0 : 1
        }
    ' "$parsed_table" ||
        fail 'provider presentation has duplicate, non-contiguous, or unsafe numbers'
    awk -F '\t' '
        { numbers[$1] = 1 }
        END {
            for (number in numbers) number_count++
            exit number_count == 2 ? 0 : 1
        }
    ' "$parsed_table" ||
        fail 'provider presentation contains duplicate numbers'
    awk -F '\t' '
        { packages[$3] = 1 }
        END {
            for (package in packages) package_count++
            exit package_count == 2 ? 0 : 1
        }
    ' "$parsed_table" ||
        fail 'provider candidate set contains duplicate package identities'
    while IFS="$(printf '\t')" read -r \
        candidate_number candidate_source candidate_package \
        candidate_repository candidate_dependency; do
        if [ "$candidate_source" != repository ] ||
            [ "$candidate_repository" != "$EXPECTED_PROVIDER_REPOSITORY" ] ||
            [ "$candidate_dependency" != "$REQUIRED_MAKE_DEPENDENCY" ]; then
            fail "provider drift: unsafe candidate identity $candidate_number/$candidate_source/$candidate_repository/$candidate_dependency"
        fi
        case ",$EXPECTED_PROVIDER_PACKAGES," in
            *,"$candidate_package",*) ;;
            *) fail "provider drift: unexpected provider $candidate_package" ;;
        esac
    done < "$parsed_table"
    if [ "$(awk -F '\t' -v package="$first_provider" \
        '$3 == package { count++ } END { print count + 0 }' "$parsed_table")" -ne 1 ] ||
        [ "$(awk -F '\t' -v package="$second_provider" \
        '$3 == package { count++ } END { print count + 0 }' "$parsed_table")" -ne 1 ]; then
        fail 'provider drift: reviewed provider identities were not both present once'
    fi
}

assert_same_candidate_presentation() {
    candidate_table=$case_directory/candidates.tsv
    parse_candidate_contract "$case_output" "$candidate_table"
    if ! cmp -s "$discovery_table" "$candidate_table"; then
        fail 'provider presentation changed after discovery; refusing implicit number reuse'
    fi
}

print_candidate_summary() {
    summary_table=$1
    printf '  candidate identities:'
    while IFS="$(printf '\t')" read -r \
        summary_number _ summary_package summary_repository _; do
        printf ' %s=%s/%s' \
            "$summary_number" "$summary_repository" "$summary_package"
    done < "$summary_table"
    printf '\n'
}

assert_ambiguous_diagnostic() {
    if ! grep -i 'ambiguous provider' "$case_output" >/dev/null; then
        fail 'ambiguous provider diagnostic was not emitted'
    fi
}

assert_selection_failure_case() {
    expected_prompt_count=$1
    assert_blocked_status
    assert_same_candidate_presentation
    assert_count "$expected_prompt_count" "$provider_prompt" \
        "$case_output.normalized"
    assert_ambiguous_diagnostic
    assert_sentinel_absent "$current_case"
    assert_not_contains 'Installing selected repository providers:' "$case_output"
    assert_not_contains "Running: 'sudo'" "$case_output"
    assert_common_case_integrity
}

assert_selected_provider_transaction() {
    expected_package=$1
    other_package=$first_provider
    if [ "$expected_package" = "$first_provider" ]; then
        other_package=$second_provider
    fi

    selected_install_intent="Installing selected repository providers: '$EXPECTED_PROVIDER_REPOSITORY/$expected_package'"
    selected_transaction_intent="Running: 'sudo' 'pacman' '-S' '--asdeps' '--needed' '--' '$EXPECTED_PROVIDER_REPOSITORY/$expected_package'"
    selected_sentinel_diagnostic="moguet-live-pacman-sentinel: accepted and blocked sudo pacman argv for $EXPECTED_PROVIDER_REPOSITORY/$expected_package"
    other_install_intent="Installing selected repository providers: '$EXPECTED_PROVIDER_REPOSITORY/$other_package'"
    other_transaction_intent="Running: 'sudo' 'pacman' '-S' '--asdeps' '--needed' '--' '$EXPECTED_PROVIDER_REPOSITORY/$other_package'"

    assert_exact_message_count 1 "$selected_install_intent" "$case_output.normalized"
    assert_exact_message_count 1 "$selected_transaction_intent" "$case_output.normalized"
    assert_exact_message_count 1 "$selected_sentinel_diagnostic" "$case_output.normalized"
    assert_exact_message_count 0 "$other_install_intent" "$case_output.normalized"
    assert_exact_message_count 0 "$other_transaction_intent" "$case_output.normalized"
    assert_contains 'Failed to install selected repository providers.' "$case_output"
    assert_sentinel_log "$current_case" \
        sudo pacman -S --asdeps --needed -- \
        "$EXPECTED_PROVIDER_REPOSITORY/$expected_package"
}

assert_selected_provider_transaction_multiple() {
    first_target=$EXPECTED_PROVIDER_REPOSITORY/$canonical_first_provider
    second_target=$EXPECTED_PROVIDER_REPOSITORY/$canonical_second_provider
    selected_install_intent="Installing selected repository providers: '$first_target' '$second_target'"
    selected_transaction_intent="Running: 'sudo' 'pacman' '-S' '--asdeps' '--needed' '--' '$first_target' '$second_target'"
    selected_sentinel_diagnostic="moguet-live-pacman-sentinel: accepted and blocked sudo pacman argv for $first_target $second_target"

    assert_exact_message_count 1 "$selected_install_intent" "$case_output.normalized"
    assert_exact_message_count 1 "$selected_transaction_intent" "$case_output.normalized"
    assert_exact_message_count 1 "$selected_sentinel_diagnostic" "$case_output.normalized"
    assert_contains 'Failed to install selected repository providers.' "$case_output"
    assert_sentinel_log "$current_case" \
        sudo pacman -S --asdeps --needed -- "$first_target" "$second_target"
}

assert_valid_selection_case() {
    expected_package=$1
    assert_blocked_status
    assert_same_candidate_presentation
    assert_count 1 "$provider_prompt" "$case_output.normalized"
    assert_selected_provider_transaction "$expected_package"
    assert_common_case_integrity
}

run_multiple_expression_case() {
    expression_case=$1
    expression_input=$2
    printf ':: case=%s\n' "$expression_case"
    prepare_case "$expression_case"
    input_file=$case_directory/input
    printf '%s\n' "$expression_input" > "$input_file"
    run_pty_case "$input_file" --noedit build --local "$case_source"
    assert_blocked_status
    assert_same_candidate_presentation
    assert_count 1 "$provider_prompt" "$case_output.normalized"
    assert_selected_provider_transaction_multiple
    assert_common_case_integrity
    printf '  expression: %s; selected in candidate order: %s/%s %s/%s\n' \
        "$expression_input" "$EXPECTED_PROVIDER_REPOSITORY" \
        "$canonical_first_provider" "$EXPECTED_PROVIDER_REPOSITORY" \
        "$canonical_second_provider"
}

run_single_expression_case() {
    expression_case=$1
    expression_input=$2
    printf ':: case=%s\n' "$expression_case"
    prepare_case "$expression_case"
    input_file=$case_directory/input
    printf '%s\n' "$expression_input" > "$input_file"
    run_pty_case "$input_file" --noedit build --local "$case_source"
    assert_valid_selection_case "$canonical_first_provider"
    printf '  expression: %s; selected: %s/%s\n' \
        "$expression_input" "$EXPECTED_PROVIDER_REPOSITORY" \
        "$canonical_first_provider"
}

assert_invalid_retry_selection_before_mutation() {
    selected_package=$1
    if ! python3 - "$case_output.normalized" "$selected_package" \
        "$EXPECTED_PROVIDER_REPOSITORY" "$provider_prompt" \
        "$provider_invalid_token" "$provider_invalid_index" \
        "$provider_invalid_empty" <<'PY'
from pathlib import Path
import sys

output = Path(sys.argv[1]).read_bytes()
selected_package = sys.argv[2].encode("ascii")
repository = sys.argv[3].encode("ascii")

provider_prompt = sys.argv[4].encode("ascii")
invalid_diagnostics = [message.encode("ascii") for message in sys.argv[5:]]
install_intent = b"Installing selected repository providers: '" + repository + b"/" + selected_package + b"'"
transaction_intent = (
    b"Running: 'sudo' 'pacman' '-S' '--asdeps' '--needed' '--' '" + repository + b"/" +
    selected_package + b"'"
)

def positions(needle):
    found = []
    start = 0
    while True:
        position = output.find(needle, start)
        if position < 0:
            return found
        found.append(position)
        start = position + len(needle)

invalid_positions = sorted(
    position
    for message in invalid_diagnostics
    for position in positions(message)
)
prompt_positions = positions(provider_prompt)
install_positions = positions(install_intent)
transaction_positions = positions(transaction_intent)
if len(invalid_positions) != 5 or len(prompt_positions) != 6:
    raise SystemExit("invalid-retry diagnostic or prompt count changed before position check")
if len(install_positions) != 1 or len(transaction_positions) != 1:
    raise SystemExit("invalid-retry transaction intent is not exactly once before position check")

transaction_position = transaction_positions[0]
if any(position >= transaction_position for position in invalid_positions):
    raise SystemExit("an invalid-choice diagnostic appeared after transaction intent")
if prompt_positions[5] >= install_positions[0]:
    raise SystemExit("the final provider prompt did not precede selected-provider intent")
if install_positions[0] > transaction_position:
    raise SystemExit("provider install intent appeared after its transaction intent")
PY
    then
        fail 'invalid-retry did not prove selection-before-mutation ordering'
    fi
}

assert_regular_non_symlink "$fixture_pkgbuild" 'tracked live PKGBUILD fixture'
assert_regular_non_symlink "$fixture_contract" 'tracked live fixture contract'
assert_regular_non_symlink "$fixture_expected_srcinfo" \
    'tracked local expected .SRCINFO authority'
assert_regular_non_symlink "$fixture_payload_authority" \
    'tracked local payload authority'
assert_regular_non_symlink "$pty_runner" 'production PTY helper'
assert_regular_non_symlink "$production_moguet" 'production Moguet binary'
# shellcheck source=fixtures/local-package/contract.env
. "$fixture_contract"
saved_ifs=$IFS
IFS=,
set -- $EXPECTED_PROVIDER_PACKAGES
IFS=$saved_ifs
[ "$#" -eq 2 ] || fail 'provider-selection lane requires exactly two reviewed providers'
first_provider=$1
second_provider=$2
first_provider_target=$EXPECTED_PROVIDER_REPOSITORY/$first_provider
second_provider_target=$EXPECTED_PROVIDER_REPOSITORY/$second_provider
provider_prompt='Select providers from [1-2]'
provider_invalid_token='Invalid provider selection token.'
provider_invalid_index='Provider selection index is out of range.'
provider_invalid_empty='Provider selection is empty after exclusions.'
if [ "$(id -u)" -eq 0 ]; then
    fail 'live provider runner must execute as an unprivileged validation user'
fi
if [ -e "$repo_root/.git" ]; then
    fail 'Docker build context leaked host .git metadata into the image'
fi
credential_probe=$case_root/credential-paths.raw
mkdir -p "$case_root"
if validation_capture_output "$credential_probe" find "$repo_root" -xdev \
    \( -name .ssh -o -name .gnupg -o -name .git-credentials \
    -o -name .netrc -o -name docker.sock \) -print; then
    :
else
    credential_status=$?
    fail "credential-path producer failed with status $credential_status"
fi
if [ -s "$credential_probe" ]; then
    fail 'Docker build context leaked credential or Docker socket state'
fi

tracked_fixture_before=$(tracked_fixture_manifest)
tracked_pkgbuild_before=$(sha256sum "$fixture_pkgbuild")
initial_package_database=$(package_database_manifest)

printf '%s\n' ':: Arch live provider-selection validation'
printf '  production binary: %s\n' "$production_moguet"
printf '  runtime user: uid=%s gid=%s\n' "$(id -u)" "$(id -g)"
printf '  expected blocked phase: real pacman repository transaction\n'

assert_runtime_sentinel_contract

printf '%s\n' ':: case=provider-discovery'
prepare_case provider-discovery
discovery_input=$case_directory/input
printf 'q\n' > "$discovery_input"
run_pty_case "$discovery_input" --noedit build --local "$case_source"
assert_blocked_status
discovery_table=$case_directory/candidates.tsv
parse_candidate_contract "$case_output" "$discovery_table"
first_provider_choice=$(awk -F '\t' -v package="$first_provider" \
    '$3 == package { print $1 }' "$discovery_table")
second_provider_choice=$(awk -F '\t' -v package="$second_provider" \
    '$3 == package { print $1 }' "$discovery_table")
if [ -z "$first_provider_choice" ] || [ -z "$second_provider_choice" ] ||
    [ "$first_provider_choice" = "$second_provider_choice" ]; then
    fail 'provider choices could not be resolved from candidate identities'
fi
canonical_first_provider=$(awk -F '\t' '$1 == 1 { print $3 }' "$discovery_table")
canonical_second_provider=$(awk -F '\t' '$1 == 2 { print $3 }' "$discovery_table")
assert_count 1 "$provider_prompt" "$case_output.normalized"
assert_ambiguous_diagnostic
assert_sentinel_absent provider-discovery
assert_not_contains "Running: 'sudo'" "$case_output"
assert_common_case_integrity
print_candidate_summary "$discovery_table"
printf '  resolved choices from identities: %s/%s=%s %s/%s=%s\n' \
    "$EXPECTED_PROVIDER_REPOSITORY" "$first_provider" "$first_provider_choice" \
    "$EXPECTED_PROVIDER_REPOSITORY" "$second_provider" "$second_provider_choice"
printf '%s\n' '  expected blocked phase: provider selection cancellation'

printf '%s\n' ':: case=first-provider-selection'
prepare_case first-provider-selection
first_provider_input=$case_directory/input
printf '%s\n' "$first_provider_choice" > "$first_provider_input"
run_pty_case "$first_provider_input" --noedit build --local "$case_source"
assert_valid_selection_case "$first_provider"
print_candidate_summary "$candidate_table"
printf '  resolved choice: %s/%s=%s\n' \
    "$EXPECTED_PROVIDER_REPOSITORY" "$first_provider" "$first_provider_choice"
printf '  sentinel argv: sudo pacman -S --asdeps --needed -- %s/%s\n' \
    "$EXPECTED_PROVIDER_REPOSITORY" "$first_provider"
printf '%s\n' '  expected blocked phase: repository provider transaction'

printf '%s\n' ':: case=second-provider-selection'
prepare_case second-provider-selection
second_provider_input=$case_directory/input
printf '%s\n' "$second_provider_choice" > "$second_provider_input"
run_pty_case "$second_provider_input" --noedit build --local "$case_source"
assert_valid_selection_case "$second_provider"
print_candidate_summary "$candidate_table"
printf '  resolved choice: %s/%s=%s\n' \
    "$EXPECTED_PROVIDER_REPOSITORY" "$second_provider" "$second_provider_choice"
printf '  sentinel argv: sudo pacman -S --asdeps --needed -- %s/%s\n' \
    "$EXPECTED_PROVIDER_REPOSITORY" "$second_provider"
printf '%s\n' '  expected blocked phase: repository provider transaction'

run_multiple_expression_case multiple-range '1-2'
run_multiple_expression_case multiple-comma '1,2'
run_multiple_expression_case multiple-space '1 2'
run_single_expression_case exclude-only '^2'
run_single_expression_case include-exclude '1-2 ^2'

printf '%s\n' ':: case=invalid-retry'
prepare_case invalid-retry
invalid_input=$case_directory/input
out_of_range_choice=3
printf '1 2 foo\n^1-2\nnot-a-number\n0\n%s\n%s\n' \
    "$out_of_range_choice" "$first_provider_choice" > "$invalid_input"
run_pty_case "$invalid_input" --noedit build --local "$case_source"
assert_blocked_status
assert_same_candidate_presentation
assert_count 2 "$provider_invalid_token" "$case_output.normalized"
assert_count 2 "$provider_invalid_index" "$case_output.normalized"
assert_count 1 "$provider_invalid_empty" "$case_output.normalized"
assert_count 6 "$provider_prompt" "$case_output.normalized"
assert_selected_provider_transaction "$first_provider"
assert_invalid_retry_selection_before_mutation "$first_provider"
assert_common_case_integrity
print_candidate_summary "$candidate_table"
printf '  retry inputs: partial-token, exclude-all, non-numeric, zero, out-of-range=%s; valid=%s\n' \
    "$out_of_range_choice" "$first_provider_choice"
printf '%s\n' '  retry prompts=6 invalid diagnostics=5 sentinel calls before valid=0'
printf '%s\n' '  expected blocked phase: repository provider transaction after valid retry'

printf '%s\n' ':: case=cancel-empty'
prepare_case cancel-empty
cancel_empty_input=$case_directory/input
printf '\n' > "$cancel_empty_input"
run_pty_case "$cancel_empty_input" --noedit build --local "$case_source"
assert_selection_failure_case 1
printf '%s\n' '  input: empty; default selection: none'
printf '%s\n' '  expected blocked phase: provider selection cancellation'

printf '%s\n' ':: case=cancel-q'
prepare_case cancel-q
cancel_q_input=$case_directory/input
printf 'q\n' > "$cancel_q_input"
run_pty_case "$cancel_q_input" --noedit build --local "$case_source"
assert_selection_failure_case 1
printf '%s\n' '  input: q; default selection: none'
printf '%s\n' '  expected blocked phase: provider selection cancellation'

printf '%s\n' ':: case=provider-eof'
prepare_case provider-eof
eof_input=$case_directory/input
# In canonical PTY mode, VEOF at an empty line makes getline observe EOF while
# retaining a real terminal on stdin.
printf '\004' > "$eof_input"
run_pty_case "$eof_input" --noedit build --local "$case_source"
assert_selection_failure_case 1
printf '%s\n' '  input: PTY VEOF; default selection: none'
printf '%s\n' '  expected blocked phase: provider selection EOF cancellation'

printf '%s\n' ':: case=non-tty-pipe'
prepare_case non-tty-pipe
run_non_tty_case "$first_provider_choice" --noedit build --local "$case_source"
assert_blocked_status
tr -d '\r' < "$case_output" > "$case_output.normalized"
assert_not_contains ":: Choose a provider for $REQUIRED_MAKE_DEPENDENCY" \
    "$case_output.normalized"
assert_not_contains 'Select a provider from' "$case_output.normalized"
assert_ambiguous_diagnostic
assert_sentinel_absent non-tty-pipe
assert_not_contains "Running: 'sudo'" "$case_output"
assert_common_case_integrity
printf '  piped candidate-like value: %s; consumed as selection: no\n' \
    "$first_provider_choice"
printf '%s\n' '  expected blocked phase: non-TTY ambiguous provider guard'

printf '%s\n' ':: case=noconfirm-tty'
prepare_case noconfirm-tty
noconfirm_input=$case_directory/input
printf '%s\n' "$first_provider_choice" > "$noconfirm_input"
run_pty_case "$noconfirm_input" --noedit --noconfirm build --local "$case_source"
assert_blocked_status
tr -d '\r' < "$case_output" > "$case_output.normalized"
assert_not_contains ":: Choose a provider for $REQUIRED_MAKE_DEPENDENCY" \
    "$case_output.normalized"
assert_not_contains 'Select a provider from' "$case_output.normalized"
assert_ambiguous_diagnostic
assert_sentinel_absent noconfirm-tty
assert_not_contains "Running: 'sudo'" "$case_output"
assert_common_case_integrity
printf '  TTY candidate-like value: %s; auto-selected: no\n' \
    "$first_provider_choice"
printf '%s\n' '  expected blocked phase: --noconfirm ambiguous provider guard'

final_tracked_fixture=$(tracked_fixture_manifest)
final_tracked_pkgbuild=$(sha256sum "$fixture_pkgbuild")
final_package_database=$(package_database_manifest)
if [ "$final_tracked_fixture" != "$tracked_fixture_before" ] ||
    [ "$final_tracked_pkgbuild" != "$tracked_pkgbuild_before" ]; then
    fail 'tracked provider fixture checksum changed across the live E2E lane'
fi
if [ "$final_package_database" != "$initial_package_database" ]; then
    fail 'container package database changed across the live E2E lane'
fi

printf '%s\n' 'arch-live-provider: all provider-selection cases passed'
