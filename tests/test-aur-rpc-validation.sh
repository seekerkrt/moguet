#!/bin/sh
set -eu

# Assertions target the canonical untranslated CLI output.
# Do not inherit locale settings from the invoking environment.
LANG=C
LC_ALL=C
export LANG LC_ALL
unset LANGUAGE

test_binary=$1
envelope_test_binary=$2
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
request_log=$tmp_dir/requests.log
user_agent_log=$tmp_dir/user-agents.log
: > "$request_log"
: > "$user_agent_log"
python3 "$repo_root/tests/aur_rpc_fixture_server.py" \
    "$repo_root/tests/fixtures/aur-rpc-validation.json" "$port_file" \
    "$request_log" "$user_agent_log" &
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
fixture_rpc_base_url=http://127.0.0.1:$port/rpc/
export PATH=$repo_root/tests/stubs:/usr/bin:/bin
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
require_exact_test_command makepkg "$repo_root/tests/stubs/makepkg"
require_exact_test_command pacman "$repo_root/tests/stubs/pacman"
require_exact_test_command sudo "$repo_root/tests/stubs/sudo"
require_exact_test_command git "$repo_root/tests/stubs/git"
export MOGUET_TEST_AUR_RPC_BASE_URL=$fixture_rpc_base_url
# All successful requests stay on the loopback fixture regardless of host proxies.
export no_proxy=127.0.0.1 NO_PROXY=127.0.0.1

setup_case() {
    case_name=$1
    case_dir=$tmp_dir/cases/$case_name
    command_log=$case_dir/commands.log
    output_file=$case_dir/output

    mkdir -p \
        "$case_dir/home" "$case_dir/xdg-config" \
        "$case_dir/xdg-state" "$case_dir/xdg-cache"
    chmod 0700 "$case_dir/xdg-config"
    : > "$command_log"
    : > "$request_log"
    : > "$user_agent_log"
    inventory_state=$case_dir/foreign-inventory.state
    : > "$inventory_state"
    export HOME=$case_dir/home
    export XDG_CONFIG_HOME=$case_dir/xdg-config
    export XDG_STATE_HOME=$case_dir/xdg-state
    export XDG_CACHE_HOME=$case_dir/xdg-cache
    export MOGUET_TEST_COMMAND_LOG=$command_log
    export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$inventory_state
    export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
    export MOGUET_TEST_PACMAN_EXIT_CODE=1
    export MOGUET_TEST_SUDO_EXIT_CODE=99
    unset MOGUET_TEST_PACMAN_QM_OUTPUT
    unset MOGUET_TEST_PACMAN_REPO_PACKAGES
    unset MOGUET_TEST_GIT_REMOTE_URL
    unset MOGUET_TEST_GIT_CLONE_EXIT_CODE
    unset MOGUET_TEST_GIT_CLONE_SYMLINK_TARGET
    unset MOGUET_TEST_GIT_CLONE_FIXTURE_DIR
    unset MOGUET_TEST_MAKEPKG_EXIT_CODE
    unset MOGUET_TEST_MAKEPKG_ARTIFACT_IDENTITIES
    unset MOGUET_TEST_AUR_RPC_ENCODE_FAILURE_PACKAGE
}

run_ok() {
    : > "$command_log"
    if ! "$test_binary" "$@" </dev/null > "$output_file" 2>&1; then
        echo "expected command to succeed: $*" >&2
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_fail() {
    : > "$command_log"
    if ! validation_expect_status aur-rpc-business-failure 1 \
        "$output_file" "$output_file" "$test_binary" "$@" </dev/null; then
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_envelope_ok() {
    : > "$command_log"
    if ! "$envelope_test_binary" "$@" </dev/null > "$output_file" 2>&1; then
        echo "expected strict envelope command to succeed: $*" >&2
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_envelope_fail() {
    : > "$command_log"
    if ! validation_expect_status aur-envelope-business-failure 1 \
        "$output_file" "$output_file" \
        "$envelope_test_binary" "$@" </dev/null; then
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

assert_no_mutation_commands() {
    if grep -E '^(git|makepkg|sudo) |^pacman -S( |$)' "$command_log" >/dev/null; then
        echo "schema validation allowed an external mutation command" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_no_pacman_command() {
    if grep -E '^pacman( |$)' "$command_log" >/dev/null; then
        echo "foreign update validation unexpectedly ran pacman" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

set_foreign_inventory() {
    printf '%s\n' "$1" > "$inventory_state"
}

assert_command_log_empty() {
    if [ -s "$command_log" ]; then
        echo "external command ran before AUR RPC schema preflight completed" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_request_count() {
    expected=$1
    actual=$(wc -l < "$request_log")
    if [ "$actual" -ne "$expected" ]; then
        echo "unexpected AUR fixture request count: expected $expected, got $actual" >&2
        cat "$request_log" >&2
        exit 1
    fi
}

assert_moguet_user_agents() {
    expected_user_agent="moguet/$(tr -d '[:space:]' < "$repo_root/VERSION")"
    if [ ! -s "$user_agent_log" ]; then
        echo "AUR fixture did not observe a User-Agent header" >&2
        exit 1
    fi
    unexpected_user_agent_count=$(validation_grep_count \
        -Fvxc -- "$expected_user_agent" "$user_agent_log")
    if [ "$unexpected_user_agent_count" -ne 0 ]; then
        echo "unexpected AUR RPC User-Agent; expected $expected_user_agent" >&2
        cat "$user_agent_log" >&2
        exit 1
    fi
}

assert_no_version_comparison() {
    if grep -E '^vercmp( |$)' "$command_log" >/dev/null; then
        echo "AUR encode failure returned a partial batch result" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_validation_error() {
    context=$1
    assert_contains "AUR RPC response validation failed for $context" "$output_file"
}

assert_cache_entry_absent() {
    entry=$XDG_CACHE_HOME/moguet/$1
    if [ -e "$entry" ] || [ -L "$entry" ]; then
        echo "cache entry was created before metadata preflight completed: $entry" >&2
        exit 1
    fi
}

prepare_source_preferences() {
    preference_dir=$XDG_CONFIG_HOME/moguet/source-build.d
    mkdir -p "$preference_dir"
    chmod 0700 "$preference_dir"
    for package in "$@"; do
        : > "$preference_dir/$package"
        chmod 0600 "$preference_dir/$package"
    done
}

# strict APIだけがAUR RPC v5 envelopeを検証する。legacy APIのpermissive境界は維持する。
setup_case strict-envelope-valid-info
run_envelope_ok info-strict valid-minimal
assert_contains "valid-minimal" "$output_file"
assert_command_log_empty

setup_case write-callback-contract
run_envelope_ok write-callback-contract unused
assert_contains "write-callback-contract-ok" "$output_file"
assert_command_log_empty

setup_case write-callback-exception
run_envelope_fail write-failure-strict valid-minimal
assert_contains "AUR request failed:" "$output_file"
assert_not_contains "AUR request returned an empty response." "$output_file"
assert_not_contains "valid-minimal" "$output_file"
assert_command_log_empty

setup_case info-many-normal
run_envelope_ok info-many-normal unused
assert_contains "valid-minimal" "$output_file"
assert_contains "arrays-null" "$output_file"
assert_request_count 1
assert_moguet_user_agents

setup_case info-many-encode-failure-first
run_envelope_fail info-many-fail-first unused
assert_contains "Failed to encode AUR package name: valid-minimal" "$output_file"
assert_not_contains "arrays-null" "$output_file"
assert_request_count 0

setup_case info-many-encode-failure-middle
run_envelope_fail info-many-fail-middle unused
assert_contains "Failed to encode AUR package name: arrays-null" "$output_file"
assert_not_contains "valid-minimal" "$output_file"
assert_not_contains "arrays-empty" "$output_file"
assert_request_count 0

setup_case strict-envelope-valid-not-found
run_envelope_ok info-strict strict-not-found
assert_contains "not-found" "$output_file"
assert_command_log_empty

setup_case strict-envelope-valid-search
run_envelope_ok provides-strict virtual-one
assert_contains "provider-one" "$output_file"
assert_command_log_empty

setup_case strict-envelope-valid-typed-search
run_envelope_ok search-strict search-valid-query
assert_contains "search-valid-result|search-valid-result|1.0-1" "$output_file"
assert_command_log_empty

setup_case strict-envelope-valid-empty-typed-search
run_envelope_ok search-strict strict-search-empty
if [ -s "$output_file" ]; then
    echo "valid empty strict typed search returned output" >&2
    cat "$output_file" >&2
    exit 1
fi
assert_command_log_empty

setup_case strict-typed-search-encode-failure
run_envelope_fail search-encode-failure-strict encode-failure-query
assert_contains "Failed to encode AUR search query: encode-failure-query" "$output_file"
assert_request_count 0

setup_case strict-typed-search-transport-failure
export MOGUET_TEST_AUR_RPC_BASE_URL=http://127.0.0.1:9/rpc/
run_envelope_fail search-strict transport-failure-query
export MOGUET_TEST_AUR_RPC_BASE_URL=$fixture_rpc_base_url
assert_contains "AUR request failed:" "$output_file"
assert_not_contains "AUR request returned an empty response." "$output_file"
assert_request_count 0

setup_case strict-typed-search-http-failure
run_envelope_fail search-strict strict-search-http-failure
assert_contains "AUR request failed:" "$output_file"
assert_contains "503" "$output_file"
assert_request_count 1

setup_case strict-envelope-valid-empty-search
run_envelope_ok provides-strict strict-search-empty
if [ -s "$output_file" ]; then
    echo "valid empty strict provider search returned output" >&2
    cat "$output_file" >&2
    exit 1
fi
assert_command_log_empty

while IFS='|' read -r package detail; do
    setup_case "strict-envelope-$package"
    run_envelope_fail info-strict "$package"
    assert_validation_error "info[package=\"$package\"]"
    assert_contains "$detail" "$output_file"
    assert_command_log_empty
done <<'CASES'
strict-error-string|field error reported "fixture AUR RPC failure"
strict-error-number|field error expected string, got number
strict-version-missing|field version expected integer, got missing
strict-version-string|field version expected integer, got string
strict-version-unsupported|field version expected 5, got 4
strict-type-missing|field type expected string, got missing
strict-type-number|field type expected string, got number
strict-type-wrong|field type expected "multiinfo", got "search"
strict-resultcount-missing|field resultcount expected integer, got missing
strict-resultcount-string|field resultcount expected integer, got string
strict-resultcount-negative|field resultcount expected non-negative integer, got -1
strict-resultcount-mismatch|field resultcount was 1 but results contained 0 entries
strict-results-missing|field results expected array, got missing
strict-results-object|field results expected array, got object
strict-info-multiple|expected zero or one result, got 2
CASES

# Provider candidate presentation metadata must be terminal-safe before it
# reaches any interactive selection UI. Diagnostics do not echo the raw value.
escape_character=$(printf '\033')
while IFS='|' read -r package detail; do
    setup_case "strict-presentation-$package"
    run_envelope_fail info-strict "$package"
    assert_validation_error "info[package=\"$package\"]"
    assert_contains "$detail" "$output_file"
    assert_not_contains "$escape_character" "$output_file"
    assert_command_log_empty
done <<'CASES'
version-control|field Version contains a control character
depends-control|field Depends[0] contains a control character
semantic-provides-control|field Provides[0] contains a control character
semantic-provides-malformed|field Provides[0] contains an invalid version constraint
semantic-provides-non-equality|field Provides[0] contains an invalid version constraint
semantic-provides-empty-version|field Provides[0] contains an invalid version constraint
semantic-conflicts-malformed|field Conflicts[0] contains an invalid version constraint
semantic-conflicts-empty-version|field Conflicts[0] contains an invalid version constraint
semantic-replaces-malformed|field Replaces[0] contains an invalid version constraint
semantic-replaces-invalid-version|field Replaces[0] contains an invalid version constraint
CASES

setup_case strict-constraint-metadata-projection
run_envelope_ok constraint-metadata-strict arrays-valid
assert_contains "arrays-valid|arrays-valid|1|1|1|1|2" "$output_file"
assert_command_log_empty

setup_case strict-envelope-search-type
run_envelope_fail provides-strict strict-search-wrong-type
assert_validation_error "search[provides=\"strict-search-wrong-type\"]"
assert_contains 'field type expected "search", got "multiinfo"' "$output_file"
assert_command_log_empty

setup_case strict-envelope-typed-search-type
run_envelope_fail search-strict strict-search-wrong-type
assert_validation_error "search[query=\"strict-search-wrong-type\"]"
assert_contains 'field type expected "search", got "multiinfo"' "$output_file"
assert_command_log_empty

setup_case legacy-envelope-info
run_envelope_ok info-legacy legacy-envelope-permissive
assert_contains "not-found" "$output_file"
assert_command_log_empty

setup_case legacy-envelope-search
run_envelope_ok provides-legacy strict-search-wrong-type
assert_contains "provider-one" "$output_file"
assert_command_log_empty

# F-537-03: use production AurClient/libcurl, not commands-sync's info stub.
# A valid empty result and a failed request must reach distinct caller branches.
for route in aur auto build; do
    case "$route" in
        aur) set -- -Si --aur ;;
        auto) set -- -Si ;;
        build) set -- build ;;
    esac
    setup_case "info-absence-$route"
    run_fail "$@" strict-not-found
    if [ "$route" = aur ]; then
        assert_contains "AUR package not found: strict-not-found" "$output_file"
    else
        assert_contains "Package not found in repos or AUR: strict-not-found" "$output_file"
    fi
    assert_not_contains "Failed to fetch AUR info" "$output_file"
    assert_not_contains "Warning:" "$output_file"
    assert_request_count 1
    assert_no_mutation_commands

    setup_case "info-invalid-proxy-$route"
    (
        # libcurl rejects this syntax before connecting; even the target URL
        # remains loopback, so this regression never needs an external server.
        export http_proxy='http://[' https_proxy='http://[' all_proxy='http://['
        export HTTP_PROXY='http://[' HTTPS_PROXY='http://[' ALL_PROXY='http://['
        export no_proxy= NO_PROXY=
        run_fail "$@" audit-rpc-failure
    )
    assert_contains "Failed to fetch AUR info for audit-rpc-failure: AUR request failed:" "$output_file"
    assert_not_contains "package not found" "$output_file"
    assert_not_contains "Package not found" "$output_file"
    assert_not_contains "Warning:" "$output_file"
    assert_request_count 0
    assert_no_mutation_commands
done

for route in aur auto; do
    case "$route" in
        aur) set -- -Si --aur ;;
        auto) set -- -Si ;;
    esac
    while IFS='|' read -r package diagnostic; do
        setup_case "info-query-failure-$route-$package"
        run_fail "$@" "$package"
        assert_contains "Failed to fetch AUR info for $package:" "$output_file"
        assert_contains "$diagnostic" "$output_file"
        assert_not_contains "package not found" "$output_file"
        assert_not_contains "Package not found" "$output_file"
        assert_not_contains "Warning:" "$output_file"
        assert_request_count 1
        assert_no_mutation_commands
    done <<'CASES'
info-http-failure|AUR request failed:
info-http-redirect|AUR request failed with HTTP status 302.
info-empty-response|AUR request returned an empty response.
info-parse-failure|AUR RPC response parse failed
strict-error-string|field error reported "fixture AUR RPC failure"
strict-error-number|field error expected string, got number
CASES
done

# missing/null/emptyはoptional arrayの正常契約。全fieldを同じentryで通す。
for package in valid-minimal arrays-null arrays-empty arrays-valid valid-split; do
    setup_case "normal-$package"
    run_ok -Si "$package"
    assert_contains "Name            : $package" "$output_file"
    if [ "$package" = "arrays-valid" ]; then
        assert_contains "Depends On      : foo>=1" "$output_file"
        assert_contains "Optional Deps   : optional-package: optional description" "$output_file"
    fi
done

# Exercise the real RPC parser and both presentation sinks. Keep stdout separate
# and report only repr(bytes), so a regression cannot replay controls in diagnostics.
for package in free-text-description free-text-maintainer free-text-utf8 \
    free-text-controls free-text-empty valid-minimal arrays-null; do
    setup_case "terminal-safe-$package"
    python3 - "$test_binary" "$package" <<'PYTHON'
import subprocess
import sys

binary, package = sys.argv[1:]
controls = br"TEXT\x09\x0A\x0D\x00\x7F\xC2\x85\x5CEND"
utf8 = "日本語 café 😀".encode("utf-8")
# Independent expected bytes, not derived by sanitizing the fixture or output.
description, maintainer = {
    "free-text-description": (br"\x1B[31mDESC\x1B[0m", b"fixture-maintainer"),
    "free-text-maintainer": (b"normal description", br"\x1B[31mMAINT\x1B[0m"),
    "free-text-utf8": (utf8, utf8),
    "free-text-controls": (controls, controls),
    "free-text-empty": (b"", b""),
    "valid-minimal": (b"", b""),
    "arrays-null": (b"", b""),
}[package]


def require(condition, message, output):
    if not condition:
        raise SystemExit(f"{package}: {message}: {output!r}")


for operation in ("-Ss", "-Si"):
    # Missing/null fixtures have no search mapping; their info covers None/orphan.
    if operation == "-Ss" and package in ("valid-minimal", "arrays-null"):
        continue
    result = subprocess.run(
        [binary, operation, "--aur", package],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False,
    )
    require(result.returncode == 0, f"{operation} exit {result.returncode}",
            (result.stdout, result.stderr))
    output = result.stdout
    if operation == "-Ss":
        prefix = b"\x1b[1;35maur\x1b[0m/\x1b[1m"
        header = prefix + package.encode("ascii") + b"\x1b[0m \x1b[1;32m1.0-1\x1b[0m"
        if not maintainer:
            header += b" \x1b[1;33m[orphaned]\x1b[0m"
        expected = header + b"\n"
        if description:
            expected += b"    " + description + b"\n"
        # Preserve Moguet's ANSI exactly; do not strip ANSI or ban ESC globally.
        start = output.find(prefix)
        require(start >= 0 and output[start:] == expected,
                "search bytes / ANSI / orphan annotation differ", output)
        payload = output[start + len(header) + 1:]
        require(b"\x1b" not in payload, "raw ESC in description", payload)
        if package == "free-text-maintainer":
            require(b"MAINT" not in output, "search exposed maintainer", output)
    else:
        for label, expected in ((b"Description     : ", description or b"None"),
                                (b"Maintainer      : ", maintainer or b"None")):
            lines = [line for line in output.split(b"\n") if line.startswith(label)]
            require(lines == [label + expected], "info field bytes differ", output)
            require(b"\x1b" not in lines[0], "raw ESC in remote info field", lines[0])
        orphan = b"\x1b[1;33myes\x1b[0m" if not maintainer else b"no"
        require(b"Orphaned        : " + orphan + b"\n" in output,
                "info orphan state differs", output)
PYTHON
    assert_no_mutation_commands
done

# safety-critical arrayはfieldごとに独立して刺激する。1 entryにまとめると最初のerrorで後続fieldを検証できない。
while IFS='|' read -r package detail; do
    setup_case "array-$package"
    run_fail -Si "$package"
    assert_validation_error "info[package=\"$package\"]"
    assert_contains "$detail" "$output_file"
    assert_no_mutation_commands
done <<'CASES'
depends-scalar-string|field Depends expected array or null, got string
makedepends-scalar-number|field MakeDepends expected array or null, got number
checkdepends-scalar-object|field CheckDepends expected array or null, got object
optdepends-scalar-boolean|field OptDepends expected array or null, got boolean
provides-element-number|field Provides[1] expected string, got number
conflicts-element-null|field Conflicts[0] expected string, got null
replaces-element-object|field Replaces[0] expected string, got object
depends-element-boolean|field Depends[0] expected string, got boolean
CASES

while IFS='|' read -r package detail; do
    setup_case "identifier-$package"
    run_fail -Si "$package"
    assert_validation_error "info[package=\"$package\"]"
    assert_contains "$detail" "$output_file"
done <<'CASES'
id-name-missing|field Name expected string, got missing
id-name-null|field Name expected string, got null
id-name-number|field Name expected string, got number
id-name-empty|field Name expected non-empty string
id-name-whitespace|field Name expected non-empty string
id-name-invalid|invalid Name "../escape"
id-base-missing|field PackageBase expected string, got missing
id-base-null|field PackageBase expected string, got null
id-base-number|field PackageBase expected string, got number
id-base-empty|field PackageBase expected non-empty string
id-base-whitespace|field PackageBase expected non-empty string
id-base-invalid|invalid PackageBase "../escape"
single-mismatch-request|requested single-mismatch-request but response Name was other-package
single-multiple-request|expected zero or one result, got 2
CASES

while IFS='|' read -r package detail; do
    setup_case "scalar-$package"
    run_fail -Si "$package"
    assert_validation_error "info[package=\"$package\"]"
    assert_contains "$detail" "$output_file"
done <<'CASES'
version-missing|field Version expected string, got missing
version-null|field Version expected string, got null
version-number|field Version expected string, got number
version-empty|field Version expected non-empty string
description-number|field Description expected string or null, got number
maintainer-boolean|field Maintainer expected string or null, got boolean
outofdate-float|field OutOfDate expected integer or null, got number
outofdate-overflow|field OutOfDate integer is outside supported range
CASES

while IFS='|' read -r package detail; do
    setup_case "semantic-$package"
    run_fail -Si "$package"
    assert_validation_error "info[package=\"$package\"]"
    assert_contains "$detail" "$output_file"
done <<'CASES'
semantic-depends|field Depends[0] contains invalid package identifier "../escape"
semantic-makedepends|field MakeDepends[0] contains invalid package identifier "bad/name"
semantic-checkdepends|field CheckDepends[0] contains invalid package identifier ""
semantic-provides|field Provides[0] contains invalid package identifier "../virtual"
semantic-conflicts|field Conflicts[0] contains invalid package identifier "bad name"
semantic-replaces|field Replaces[0] contains invalid package identifier "-bad"
CASES

# read-only inspection経路のgeneric catchがschema errorをunknown/unresolvedへ潰さないことを確認する。
setup_case direct-deps
run_fail deps direct-root
assert_validation_error "info[package=\"malformed-direct\"]"
assert_not_contains "Unknown dependencies:" "$output_file"

setup_case recursive-deps
run_fail deps --recursive recursive-root
assert_validation_error "info[package=\"recursive-malformed\"]"

setup_case direct-plan
run_fail plan direct-root
assert_validation_error "info[package=\"malformed-direct\"]"
assert_not_contains "Unresolved dependencies:" "$output_file"

setup_case recursive-plan
run_fail plan recursive-root
assert_validation_error "info[package=\"recursive-malformed\"]"
assert_not_contains "Unresolved dependencies:" "$output_file"

while IFS='|' read -r root context detail; do
    setup_case "provider-$root"
    run_fail plan "$root"
    assert_validation_error "$context"
    assert_contains "$detail" "$output_file"
    assert_not_contains "Unresolved dependencies:" "$output_file"
done <<'CASES'
provider-name-root|search[provides="virtual-provider-name"]|invalid Name "../provider"
provider-base-root|search[provides="virtual-provider-base"]|invalid PackageBase "../provider-base"
provider-provides-root|search[provides="virtual-provider-provides"]|field Provides expected array or null, got string
provider-candidate-root|info[package="provider-candidate"]|field Depends expected array or null, got number
provider-mismatch-root|info[package="provider-mismatch"]|requested provider-mismatch but response Name was other-provider
CASES

# mutation-capable commandはplan全体のvalidation完了前にclone/build/installへ進めない。
setup_case preflight-fetch-root
run_fail fetch invalid-root-preflight
assert_validation_error "info[package=\"invalid-root-preflight\"]"
assert_no_mutation_commands
assert_cache_entry_absent valid-dep
assert_cache_entry_absent invalid-root-preflight
assert_not_contains "Review target:" "$output_file"

setup_case preflight-fetch-multiple-targets
run_fail fetch valid-minimal invalid-root-preflight
assert_validation_error "info[package=\"invalid-root-preflight\"]"
assert_no_mutation_commands
assert_cache_entry_absent valid-minimal
assert_cache_entry_absent valid-dep
assert_cache_entry_absent invalid-root-preflight

while IFS='|' read -r case_name root context; do
    setup_case "$case_name"
    run_fail --noconfirm -S "$root"
    assert_validation_error "$context"
    assert_no_mutation_commands
    assert_not_contains "Review target:" "$output_file"
done <<'CASES'
preflight-sync-root|invalid-root-preflight|info[package="invalid-root-preflight"]
preflight-sync-direct|direct-root|info[package="malformed-direct"]
preflight-sync-recursive|recursive-root|info[package="recursive-malformed"]
preflight-sync-provider|provider-candidate-root|info[package="provider-candidate"]
CASES

# AUR search側のschema errorは、read-only pacman searchが成功してもcommand全体を成功扱いしない。
setup_case normal-search
run_ok -Ss search-valid-query
assert_contains "search-valid-result" "$output_file"
assert_command "pacman -Ss search-valid-query"

setup_case malformed-search
export MOGUET_TEST_PACMAN_EXIT_CODE=0
run_fail -Ss search-invalid-query
assert_validation_error "search[query=\"search-invalid-query\"]"
assert_contains "field Description expected string or null, got object" "$output_file"
assert_no_mutation_commands

setup_case malformed-refresh-search
export MOGUET_TEST_PACMAN_EXIT_CODE=0
export MOGUET_TEST_SUDO_EXIT_CODE=0
run_fail -Ssy search-invalid-query
assert_validation_error "search[query=\"search-invalid-query\"]"
assert_contains "field Description expected string or null, got object" "$output_file"
assert_command_log_empty

# info_manyはrequested setとのmappingとduplicateを厳格に確認する。
setup_case multi-duplicate
set_foreign_inventory 'multi-dup-a 1.0-1
multi-dup-b 1.0-1'
run_fail -Qua
assert_validation_error multiinfo
assert_contains "duplicate response Name multi-dup-a" "$output_file"
assert_no_mutation_commands
assert_no_pacman_command

setup_case multi-unrequested
set_foreign_inventory 'multi-unrequested-a 1.0-1
multi-unrequested-b 1.0-1'
run_fail -Qua
assert_validation_error multiinfo
assert_contains "response Name multi-outside was not requested" "$output_file"
assert_no_mutation_commands
assert_no_pacman_command

setup_case multi-missing-allowed
set_foreign_inventory 'multi-present 1.0-1
multi-missing 1.0-1'
run_ok -Qua
assert_contains "Foreign package not found in AUR: multi-missing" "$output_file"
assert_no_mutation_commands
assert_no_pacman_command

setup_case multi-encode-failure
set_foreign_inventory 'valid-minimal 0.9-1
arrays-null 0.9-1
arrays-empty 0.9-1'
export MOGUET_TEST_AUR_RPC_ENCODE_FAILURE_PACKAGE=arrays-null
run_fail -Qua
unset MOGUET_TEST_AUR_RPC_ENCODE_FAILURE_PACKAGE
assert_contains \
    "Failed to fetch AUR info: Failed to encode AUR package name: arrays-null" \
    "$output_file"
assert_request_count 0
assert_no_version_comparison
assert_no_mutation_commands
assert_no_pacman_command

# 正常schemaの主要CLIと既存のprovider/split/cycle/unresolved guardをsmoke確認する。
setup_case normal-deps
run_ok deps valid-root
assert_contains "valid-dep" "$output_file"
assert_moguet_user_agents

setup_case normal-plan
run_ok plan valid-root
assert_contains "1. valid-dep" "$output_file"
assert_contains "2. valid-root" "$output_file"

setup_case normal-provider
run_ok plan single-provider-root
assert_contains "provider-one" "$output_file"

setup_case ambiguous-provider
run_ok plan ambiguous-provider-root
assert_contains "construction: Constructed" "$output_file"
assert_contains "completeness: Incomplete" "$output_file"
assert_contains "provider decision: Ambiguous" "$output_file"
assert_contains "Fetch readiness: Blocked" "$output_file"
assert_contains "Build readiness: Blocked" "$output_file"
assert_contains "Install readiness: Blocked" "$output_file"
assert_contains "Ambiguous provided dependencies:" "$output_file"

setup_case cycle
run_ok plan cycle-root-174
assert_contains "construction: Constructed" "$output_file"
assert_contains "completeness: Incomplete" "$output_file"
assert_contains "Fetch readiness: Blocked" "$output_file"
assert_contains "Cyclic dependencies:" "$output_file"

setup_case unresolved
run_ok plan unresolved-root-174
assert_contains "construction: Constructed" "$output_file"
assert_contains "completeness: Incomplete" "$output_file"
assert_contains "Fetch readiness: Blocked" "$output_file"
assert_contains "Unresolved dependencies:" "$output_file"

setup_case split
run_ok plan valid-split
assert_contains "Split package install targets:" "$output_file"
assert_contains "valid-split (base: valid-split-base)" "$output_file"
assert_contains "construction: Constructed" "$output_file"
assert_contains "completeness: Complete" "$output_file"
assert_contains "Fetch readiness: Ready" "$output_file"
assert_contains "Build readiness: Ready" "$output_file"
assert_contains "Install readiness: Blocked" "$output_file"

setup_case normal-fetch
run_ok fetch valid-root
assert_command "git clone https://aur.archlinux.org/valid-dep.git valid-dep"
assert_command "git clone https://aur.archlinux.org/valid-root.git valid-root"

setup_case split-fetch
run_ok fetch valid-split
assert_command "git clone https://aur.archlinux.org/valid-split-base.git valid-split-base"
assert_not_contains "makepkg " "$command_log"
assert_not_contains "sudo " "$command_log"

setup_case normal-build
run_fail --noedit build valid-minimal
assert_command "git clone https://aur.archlinux.org/valid-minimal.git valid-minimal"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "makepkg --packagelist"

setup_case build-guard
run_fail build ambiguous-provider-root
assert_contains "ambiguous providers" "$output_file"
assert_no_mutation_commands

# upgrade preflightはsplit install guardより先にdependency/provider schemaを全走査する。
setup_case upgrade-split-dependency-preflight
prepare_source_preferences upgrade-split-root
export MOGUET_TEST_SUDO_EXIT_CODE=0
run_fail upgrade
assert_validation_error "info[package=\"upgrade-split-malformed\"]"
assert_contains "field Conflicts expected array or null, got string" "$output_file"
assert_no_mutation_commands
assert_cache_entry_absent upgrade-split-root
assert_cache_entry_absent upgrade-split-base
assert_cache_entry_absent upgrade-split-malformed

# registered source upgradeはtarget-less legacy singular lifecycleのため、
# requested split childをsystem mutation前に引き続き拒否する。
setup_case upgrade-registered-split-guard
prepare_source_preferences valid-split
export MOGUET_TEST_SUDO_EXIT_CODE=0
run_fail upgrade
assert_contains "Cannot execute singular install plan for valid-split" "$output_file"
assert_contains "split package targets require the PackageBase set lifecycle: valid-split (base: valid-split-base)" "$output_file"
assert_no_mutation_commands
assert_cache_entry_absent valid-split
assert_cache_entry_absent valid-split-base

# 全targetのplan preflight中、最後のRPCでschema errorを検出しても
# system/source mutationへ進まない。directory iteratorの順序には依存させない。
setup_case upgrade-later-preflight-schema-stops-all-source
prepare_source_preferences upgrade-sequence-a upgrade-sequence-b
export MOGUET_TEST_SUDO_EXIT_CODE=0
run_fail upgrade
assert_validation_error "info[package=\"upgrade-sequence-"
assert_contains "field Depends expected array or null, got string" "$output_file"
assert_no_mutation_commands
assert_cache_entry_absent upgrade-sequence-a
assert_cache_entry_absent upgrade-sequence-b

echo "AUR RPC validation integration tests: all checks passed"

# Real runner/filtered/route propagation through a production confirmation.
python3 "$repo_root/tests/test-aur-partial-cancellation.py" "$test_binary"
