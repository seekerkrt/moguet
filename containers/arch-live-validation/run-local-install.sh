#!/bin/sh

set -eu

export LC_ALL=C.UTF-8
export LANG=C.UTF-8
export LANGUAGE=en

repo_root=$(CDPATH='' cd "$(dirname "$0")/../.." && pwd)
. "$repo_root/scripts/validation-status.sh"
fixture_root=/usr/libexec/moguet-live-local/fixtures/local-package
fixture_pkgbuild=$fixture_root/PKGBUILD
fixture_contract=$fixture_root/contract.env
fixture_expected_srcinfo=$fixture_root/expected.srcinfo
fixture_payload_authority=$fixture_root/payload-authority.tsv
pty_runner=$repo_root/tests/run-with-pty.py
production_moguet=$repo_root/moguet
case_root=$HOME/live-local-case
gateway_evidence_root=/var/log/moguet-live-local
gateway_staging_root=/var/lib/moguet-live-local/staging
gateway_case=local-root-install
gateway_status=97
current_phase=preflight
current_output=

fail() {
    printf 'arch-live-local: FAIL (%s): %s\n' "$current_phase" "$*" >&2
    if [ -n "$current_output" ] && [ -f "$current_output" ]; then
        printf 'arch-live-local: case output (%s):\n' "$current_output" >&2
        sed -n '1,360p' "$current_output" >&2
    fi
    if [ -d "$gateway_evidence_root/$gateway_case" ]; then
        printf 'arch-live-local: root gateway evidence:\n' >&2
        find "$gateway_evidence_root/$gateway_case" -maxdepth 1 -type f \
            -printf '  %f\n' | LC_ALL=C sort >&2
        for evidence_file in \
            trusted-source.json stage-hashes.txt staged-artifact-path.txt accepted.argv \
            PKGINFO PKGINFO.raw archive-members.txt archive-members.raw \
            expected-members.txt expected-members.raw marker.raw \
            marker.txt expected-marker.txt
        do
            if [ -f "$gateway_evidence_root/$gateway_case/$evidence_file" ]; then
                printf 'arch-live-local: %s:\n' "$evidence_file" >&2
                sed -n '1,180p' \
                    "$gateway_evidence_root/$gateway_case/$evidence_file" >&2
            fi
        done
    fi
    exit 1
}

assert_regular_non_symlink() {
    checked_path=$1
    checked_label=$2
    [ -f "$checked_path" ] && [ ! -L "$checked_path" ] ||
        fail "$checked_label must be a regular non-symlink: $checked_path"
}

assert_contains() {
    expected_text=$1
    checked_file=$2
    grep -F -- "$expected_text" "$checked_file" >/dev/null ||
        fail "missing expected output: $expected_text"
}

assert_not_contains() {
    unexpected_text=$1
    checked_file=$2
    if grep -F -- "$unexpected_text" "$checked_file" >/dev/null; then
        fail "unexpected output: $unexpected_text"
    fi
}

assert_metadata() {
    checked_path=$1
    expected_metadata=$2
    checked_label=$3
    actual_metadata=$(stat -c '%U:%G:%a:%F' "$checked_path")
    [ "$actual_metadata" = "$expected_metadata" ] ||
        fail "$checked_label metadata drift: $actual_metadata"
}

source_tree_manifest_raw() {
    manifest_root=$1
    (
        cd "$manifest_root" || exit $?
        sha256sum PKGBUILD || exit $?
        find . -mindepth 1 -maxdepth 1 -printf '%y %m %f\n' || exit $?
    )
}

source_content_manifest_raw() {
    manifest_root=$1
    (
        cd "$manifest_root" || exit $?
        sha256sum PKGBUILD || exit $?
        stat -c '%F:%n' PKGBUILD || exit $?
    )
}

fixture_manifest_raw() {
    (
        cd "$fixture_root" || exit $?
        sha256sum \
            PKGBUILD contract.env expected.srcinfo payload-authority.tsv ||
            exit $?
        find . -mindepth 1 -maxdepth 1 -printf '%y %m %f\n' || exit $?
    )
}

capture_source_manifest() {
    manifest_kind=$1
    manifest_root=$2
    manifest_raw=$(mktemp "$case_root/$manifest_kind.raw.XXXXXX")
    manifest_sorted=$(mktemp "$case_root/$manifest_kind.sorted.XXXXXX")
    if validation_capture_sorted_output "$manifest_raw" "$manifest_sorted" \
        "$manifest_kind" "$manifest_root"; then
        cat "$manifest_sorted" || return $?
        rm -f "$manifest_raw" "$manifest_sorted" || :
        return 0
    else
        manifest_status=$?
    fi
    fail "$manifest_kind failed with status $manifest_status; raw=$manifest_raw"
}

source_tree_manifest() {
    capture_source_manifest source_tree_manifest_raw "$1"
}

source_content_manifest() {
    capture_source_manifest source_content_manifest_raw "$1"
}

fixture_manifest() {
    capture_source_manifest fixture_manifest_raw "$fixture_root"
}

capture_inventory() {
    inventory_prefix=$1
    pacman -Q > "$inventory_prefix.packages"
    pacman -Qeq > "$inventory_prefix.explicit"
    pacman -Qdq > "$inventory_prefix.dependency"
    python3 - \
        "$inventory_prefix.packages" \
        "$inventory_prefix.explicit" \
        "$inventory_prefix.dependency" \
        "$inventory_prefix.tsv" <<'PY'
from pathlib import Path
import sys

packages = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
explicit = set(Path(sys.argv[2]).read_text(encoding="utf-8").splitlines())
dependency = set(Path(sys.argv[3]).read_text(encoding="utf-8").splitlines())
records = []
for line in packages:
    fields = line.split()
    if len(fields) != 2:
        raise SystemExit(f"unsafe pacman -Q record: {line!r}")
    name, version = fields
    if (name in explicit) == (name in dependency):
        raise SystemExit(f"install reason is not singular: {name}")
    records.append((name, version, "Explicit" if name in explicit else "Dependency"))
if len(records) != len({name for name, _version, _reason in records}):
    raise SystemExit("duplicate package identity in inventory")
Path(sys.argv[4]).write_text(
    "".join("\t".join(record) + "\n" for record in sorted(records)),
    encoding="utf-8",
)
PY
}

prepare_source_root() {
    prepared_root=$1
    fixture_before_copy=$(fixture_manifest)
    fixture_content_before_copy=$(source_content_manifest "$fixture_root")
    rm -rf -- "$prepared_root"
    mkdir -m 0700 "$prepared_root"
    mkdir -m 0755 "$prepared_root/source"
    cp "$fixture_pkgbuild" "$prepared_root/source/PKGBUILD"
    chmod 0644 "$prepared_root/source/PKGBUILD"
    [ ! -e "$prepared_root/source/.SRCINFO" ] ||
        fail 'root-owned local fixture authority unexpectedly has generated .SRCINFO'
    fixture_after_copy=$(fixture_manifest)
    fixture_content_after_copy=$(source_content_manifest "$fixture_root")
    case_copy_manifest=$(source_content_manifest "$prepared_root/source")
    [ "$fixture_before_copy" = "$fixture_after_copy" ] &&
        [ "$fixture_content_before_copy" = "$fixture_content_after_copy" ] &&
        [ "$fixture_content_before_copy" = "$case_copy_manifest" ] ||
        fail 'case-local source copy differs from the root-owned fixture authority'
    assert_metadata "$prepared_root/source" \
        'moguet-validation:moguet-validation:755:directory' \
        'case-local source root after authority copy'
    assert_metadata "$prepared_root/source/PKGBUILD" \
        'moguet-validation:moguet-validation:644:regular file' \
        'case-local PKGBUILD after authority copy'
}

assert_source_root_unchanged() {
    source_root=$1
    fixture_before=$2
    [ -d "$source_root" ] && [ ! -L "$source_root" ] ||
        fail 'case-local source root is not a regular directory'
    [ ! -e "$source_root/.SRCINFO" ] ||
        fail 'production local invocation wrote .SRCINFO into the case source root'
    [ ! -e "$source_root/src" ] && [ ! -e "$source_root/pkg" ] ||
        fail 'production local invocation wrote makepkg work directories into the case source root'
    case_manifest=$(source_tree_manifest "$source_root")
    [ "$case_manifest" = "$fixture_before" ] ||
        fail 'case-local source root changed during production execution'
}

run_gateway_rejection() {
    rejection_name=$1
    case_mode=$2
    shift 2
    rejection_output=$case_root/reject-$rejection_name.output
    if env MOGUET_LIVE_LOCAL_CASE="$case_mode" \
        sudo pacman "$@" > "$rejection_output" 2>&1; then
        rejection_status=0
    else
        rejection_status=$?
    fi
    [ "$rejection_status" -eq "$gateway_status" ] ||
        fail "gateway rejection $rejection_name returned $rejection_status"
    assert_contains 'moguet-live-local-gateway: rejected:' "$rejection_output"
    [ ! -e "$gateway_evidence_root/$gateway_case" ] ||
        fail "gateway rejection $rejection_name consumed local install evidence"
    [ ! -e "$gateway_staging_root/$gateway_case" ] ||
        fail "gateway rejection $rejection_name consumed local install staging"
    printf '  gateway rejection: %s -> status %s\n' \
        "$rejection_name" "$rejection_status"
}

run_pty() {
    input_file=$1
    output_file=$2
    source_root=$3
    cache_root=$4
    state_root=$5
    mkdir -m 0700 -- "$cache_root" "$state_root"
    if env \
        XDG_CONFIG_HOME="$HOME/.config" \
        XDG_CACHE_HOME="$cache_root" \
        XDG_STATE_HOME="$state_root" \
        MOGUET_LIVE_LOCAL_CASE="$gateway_case" \
        python3 "$pty_runner" --timeout 900 -- \
            "$production_moguet" --noedit build --local "$source_root" \
            < "$input_file" > "$output_file" 2>&1; then
        run_status=0
    else
        run_status=$?
    fi
}

parse_selected_provider_choice() {
    output_file=$1
    candidate_table=$2
    tr -d '\r' < "$output_file" > "$output_file.normalized"
    awk '
/^[0-9]+\) / {
    number = $1
    sub(/\)$/, "", number)
    source = package_name = repository = provided = ""
    for (field_index = 2; field_index <= NF; ++field_index) {
        split($field_index, field, "=")
        if (field[1] == "source") source = field[2]
        else if (field[1] == "package") package_name = field[2]
        else if (field[1] == "repository") repository = field[2]
        else if (field[1] == "provided") provided = field[2]
    }
    if (number !~ /^[0-9]+$/ || source == "" || package_name == "" ||
        repository == "" || provided == "") exit 9
    print number "\t" source "\t" package_name "\t" repository "\t" provided
}
' "$output_file.normalized" > "$candidate_table" ||
        fail 'provider presentation could not be parsed safely'
    python3 - "$candidate_table" "$EXPECTED_PROVIDER_REPOSITORY" \
        "$REQUIRED_MAKE_DEPENDENCY" "$EXPECTED_PROVIDER_PACKAGES" \
        "$LOCAL_INSTALL_PROVIDER" <<'PY'
from pathlib import Path
import sys

table, expected_repository, expected_provided, provider_text, selected = sys.argv[1:]
expected_packages = provider_text.split(",")
rows = [line.split("\t") for line in Path(table).read_text(encoding="utf-8").splitlines()]
if any(len(row) != 5 for row in rows):
    raise SystemExit("provider table is not exact-tab data")
if len(rows) != len(expected_packages):
    raise SystemExit("provider candidate count drifted from the reviewed set")
if [row[0] for row in rows] != [str(index) for index in range(1, len(rows) + 1)]:
    raise SystemExit("provider numbering is unsafe")
if any(row[1] != "repository" or row[3] != expected_repository or
       row[4] != expected_provided for row in rows):
    raise SystemExit("provider source or constraint identity drift")
if {row[2] for row in rows} != set(expected_packages):
    raise SystemExit("provider package identity set drift")
choices = [row[0] for row in rows if row[2] == selected]
if len(choices) != 1:
    raise SystemExit("selected provider is not uniquely present")
print(choices[0])
PY
}

assert_inventory_transition() {
    before_file=$1
    after_file=$2
    python3 - "$before_file" "$after_file" \
        "$fixture_name" "$fixture_version" "$ROOT_ARTIFACT_INSTALL_REASON" \
        "$LOCAL_INSTALL_PROVIDER" "$PROVIDER_INSTALL_REASON" \
        "$EXPECTED_PROVIDER_PACKAGES" <<'PY'
from pathlib import Path
import sys

def parse(path: str) -> dict[str, tuple[str, str]]:
    records = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 3 or not all(fields):
            raise SystemExit(f"unsafe inventory row: {line!r}")
        name, version, reason = fields
        if reason not in {"Explicit", "Dependency"} or name in records:
            raise SystemExit(f"unsafe inventory identity: {line!r}")
        records[name] = (version, reason)
    return records

before = parse(sys.argv[1])
after = parse(sys.argv[2])
fixture, fixture_version, root_reason, selected_provider, provider_reason, provider_text = sys.argv[3:]
reviewed_providers = set(provider_text.split(","))
for name, record in before.items():
    if after.get(name) != record:
        raise SystemExit(f"baseline package version or reason changed: {name}")
if fixture not in after or after[fixture] != (fixture_version, root_reason):
    raise SystemExit("local root did not install as the exact explicit fixture package")
if selected_provider not in after or after[selected_provider][1] != provider_reason:
    raise SystemExit("selected provider did not retain dependency install reason")
if fixture in before or reviewed_providers.intersection(before):
    raise SystemExit("fixture or reviewed provider was installed before the local case")
if f"{fixture}-debug" in after:
    raise SystemExit("unselected local debug artifact was installed")
new_names = set(after) - set(before)
if fixture not in new_names or selected_provider not in new_names:
    raise SystemExit("local fixture or selected provider was not newly installed")
for name in new_names - {fixture}:
    if after[name][1] != provider_reason:
        raise SystemExit(f"new provider transaction package is not a dependency: {name}")
print(f"inventory transition: {len(new_names)} new package(s), root explicit and providers dependency")
PY
}

assert_regular_non_symlink "$fixture_pkgbuild" 'root-owned local PKGBUILD fixture authority'
assert_regular_non_symlink "$fixture_contract" 'root-owned local fixture contract authority'
assert_regular_non_symlink "$fixture_expected_srcinfo" \
    'root-owned local expected .SRCINFO authority'
assert_regular_non_symlink "$fixture_payload_authority" \
    'root-owned local payload authority'
assert_regular_non_symlink "$pty_runner" 'PTY runner'
assert_regular_non_symlink "$production_moguet" 'production Moguet binary'
# shellcheck source=fixtures/local-package/contract.env
. "$fixture_contract"
fixture_name=$PACKAGE_NAME
fixture_version=$PACKAGE_VERSION
fixture_arch=$PACKAGE_ARCHITECTURE
fixture_artifact=${fixture_name}-${fixture_version}-${fixture_arch}.pkg.tar.zst
selected_provider=$LOCAL_INSTALL_PROVIDER
selected_provider_identity=$EXPECTED_PROVIDER_REPOSITORY/$selected_provider
provider_count=0
saved_ifs=$IFS
IFS=,
for reviewed_provider in $EXPECTED_PROVIDER_PACKAGES; do
    provider_count=$((provider_count + 1))
done
IFS=$saved_ifs
provider_prompt="Select a provider from [1-$provider_count]"
[ "$(id -u)" -eq 1000 ] && [ "$(id -g)" -eq 1000 ] ||
    fail 'live local runner must execute as validation uid/gid 1000'
[ ! -e "$repo_root/.git" ] || fail 'container source copy unexpectedly includes .git'
assert_metadata /usr/bin/pacman 'root:root:555:regular file' 'canonical local gateway'
assert_metadata /usr/libexec/moguet-live-local/pacman.real \
    'root:root:755:regular file' 'isolated real pacman'
assert_metadata /usr/libexec/moguet-live-local/local-stage-artifact.py \
    'root:root:755:regular file' 'root staging helper'
assert_metadata /usr/libexec/moguet-live-local/local-archive-validator.sh \
    'root:root:755:regular file' 'root archive validator'
assert_metadata /usr/libexec/moguet-live-local/validation-status.sh \
    'root:root:755:regular file' 'root archive status library'
assert_metadata "$fixture_root" \
    'root:root:555:directory' 'local fixture authority root'
assert_metadata "$fixture_pkgbuild" \
    'root:root:444:regular file' 'local PKGBUILD fixture authority'
assert_metadata "$fixture_contract" \
    'root:root:444:regular file' 'local fixture contract authority'
assert_metadata "$fixture_expected_srcinfo" \
    'root:root:444:regular file' 'local expected .SRCINFO authority'
assert_metadata "$fixture_payload_authority" \
    'root:root:444:regular file' 'local payload authority'
[ ! -w "$fixture_root" ] && [ ! -w "$fixture_pkgbuild" ] &&
    [ ! -w "$fixture_contract" ] && [ ! -w "$fixture_expected_srcinfo" ] &&
    [ ! -w "$fixture_payload_authority" ] ||
    fail 'validation user can modify the root-owned local fixture authority'
assert_metadata "$gateway_evidence_root" \
    'root:moguet-validation:750:directory' 'local gateway evidence root'
assert_metadata "$gateway_staging_root" \
    'root:moguet-validation:750:directory' 'local gateway staging root'

mkdir -m 0700 "$case_root/runtime"
fixture_authority_before=$(fixture_manifest)
capture_inventory "$case_root/runtime/before-rejection"
printf '%s\n' ':: root gateway fail-closed self-test'
run_gateway_rejection system-upgrade "$gateway_case" -Syu
run_gateway_rejection removal "$gateway_case" -R "$selected_provider"
run_gateway_rejection outside-artifact "$gateway_case" \
    -U -- /tmp/unsafe.pkg.tar.zst
run_gateway_rejection wrong-provider "$gateway_case" \
    -S --asdeps --needed -- \
    "$EXPECTED_PROVIDER_REPOSITORY/$REQUIRED_MAKE_DEPENDENCY"
run_gateway_rejection missing-case missing -Syu
capture_inventory "$case_root/runtime/after-rejection"
cmp -s "$case_root/runtime/before-rejection.tsv" \
    "$case_root/runtime/after-rejection.tsv" ||
    fail 'gateway rejection self-test changed package inventory'

printf '%s\n' ':: live local PKGBUILD provider discovery'
discovery_root=$case_root/discovery
prepare_source_root "$discovery_root"
discovery_source=$discovery_root/source
discovery_source_before=$(source_tree_manifest "$discovery_source")
discovery_input=$discovery_root/input.txt
discovery_output=$discovery_root/output.txt
printf 'y\nq\n' > "$discovery_input"
run_pty "$discovery_input" "$discovery_output" "$discovery_source" \
    "$discovery_root/cache" "$discovery_root/state"
validation_assert_status live-local-provider-discovery 1 "$run_status" \
    "$discovery_output" "$discovery_output" python3 "$pty_runner" \
    --timeout 900 -- "$production_moguet" --noedit build --local \
    "$discovery_source" ||
    fail 'provider discovery did not return canonical business status 1'
tr -d '\r' < "$discovery_output" > "$discovery_output.normalized"
current_phase=provider-discovery
current_output=$discovery_output
assert_contains "$provider_prompt" "$discovery_output.normalized"
assert_contains "ambiguous providers: $REQUIRED_MAKE_DEPENDENCY" \
    "$discovery_output.normalized"
assert_source_root_unchanged "$discovery_source" "$discovery_source_before"
capture_inventory "$discovery_root/after"
cmp -s "$case_root/runtime/after-rejection.tsv" "$discovery_root/after.tsv" ||
    fail 'provider discovery changed package inventory'
selected_provider_choice=$(parse_selected_provider_choice \
    "$discovery_output" "$discovery_root/candidates.tsv")
printf '  reviewed provider choice: %s=%s\n' \
    "$selected_provider_choice" "$selected_provider_identity"

printf '%s\n' ':: real local PKGBUILD build and install'
actual_root=$case_root/actual
prepare_source_root "$actual_root"
actual_source=$actual_root/source
actual_source_before=$(source_tree_manifest "$actual_source")
actual_input=$actual_root/input.txt
actual_output=$actual_root/output.txt
printf 'y\n%s\ny\ny\n' "$selected_provider_choice" > "$actual_input"
current_phase=local-build-install
current_output=$actual_output
run_pty "$actual_input" "$actual_output" "$actual_source" \
    "$actual_root/cache" "$actual_root/state"
[ "$run_status" -eq 0 ] || fail "production local build/install returned $run_status"
tr -d '\r' < "$actual_output" > "$actual_output.normalized"
assert_contains "Installing selected repository providers: '$selected_provider_identity'" \
    "$actual_output.normalized"
assert_contains "Running: 'sudo' 'pacman' '-S' '--asdeps' '--needed' '--' '$selected_provider_identity'" \
    "$actual_output.normalized"
assert_contains "Running: '/usr/bin/sudo' '--' '/usr/local/libexec/moguet/moguet-source-artifact-install-helper' 'install-legacy'" \
    "$actual_output.normalized"
assert_contains "Local PackageBase result: $PACKAGE_BASE" \
    "$actual_output.normalized"
assert_contains "required child: $fixture_name $fixture_version (explicit): installed" \
    "$actual_output.normalized"
assert_not_contains "Running: 'git'" "$actual_output.normalized"
assert_source_root_unchanged "$actual_source" "$actual_source_before"
[ "$fixture_authority_before" = "$(fixture_manifest)" ] ||
    fail 'root-owned local fixture authority changed during live execution'
workspace_probe=$actual_root/workspaces.raw
if validation_capture_output "$workspace_probe" find "$actual_root/cache" -type d \
    \( -name '.local-source-workspace~-*' -o -name '.artifact-workspace~-*' \) \
    -print; then
    :
else
    workspace_status=$?
    fail "workspace cleanup producer failed with status $workspace_status"
fi
if [ -s "$workspace_probe" ]; then
    fail 'production did not clean its local source or artifact workspace'
fi
capture_inventory "$actual_root/after"
assert_inventory_transition "$case_root/runtime/after-rejection.tsv" \
    "$actual_root/after.tsv"
pacman -Qe "$fixture_name" >/dev/null ||
    fail 'local root is not installed as explicit'
if pacman -Qd "$fixture_name" >/dev/null 2>&1; then
    fail 'local root is installed as a dependency'
fi
pacman -Qd "$selected_provider" >/dev/null ||
    fail 'selected provider is not installed as a dependency'
if pacman -Qe "$selected_provider" >/dev/null 2>&1; then
    fail 'selected provider is installed as explicit'
fi

assert_metadata "$gateway_evidence_root/$gateway_case" \
    'root:moguet-validation:750:directory' 'accepted root evidence directory'
assert_metadata "$gateway_staging_root/$gateway_case" \
    'root:moguet-validation:750:directory' 'accepted root staging directory'
for evidence_file in \
    trusted-source.json stage-hashes.txt staged-artifact-path.txt accepted.argv \
    PKGINFO PKGINFO.raw archive-members.txt archive-members.raw \
    expected-members.txt expected-members.raw marker.raw marker.txt \
    expected-marker.txt
do
    assert_regular_non_symlink "$gateway_evidence_root/$gateway_case/$evidence_file" \
        "accepted root evidence $evidence_file"
    assert_metadata "$gateway_evidence_root/$gateway_case/$evidence_file" \
        'root:moguet-validation:640:regular file' \
        "accepted root evidence $evidence_file"
done
python3 - "$gateway_evidence_root/$gateway_case/accepted.argv" \
    "$gateway_evidence_root/$gateway_case/trusted-source.json" \
    "$gateway_evidence_root/$gateway_case/staged-artifact-path.txt" <<'PY'
from pathlib import Path
import sys

argv = Path(sys.argv[1]).read_bytes().split(b"\0")
if argv[-1:] != [b""]:
    raise SystemExit("gateway argv evidence is not NUL terminated")
actual = [item.decode("utf-8", "strict") for item in argv[:-1]]
if actual[:4] != ["sudo", "pacman", "-U", "--"] or len(actual) != 5:
    raise SystemExit(f"unexpected accepted local gateway argv: {actual!r}")
import re
import json
import hashlib
import stat
if not re.fullmatch(r"/run/moguet/source-artifact-installs/active/[0-9a-f]{64}/artifacts/artifact-0\.pkg\.tar\.zst", actual[-1]):
    raise SystemExit("accepted local gateway artifact identity drift")
record = json.loads(Path(sys.argv[2]).read_text())
identity = record["identity"]
if record["path"] != actual[-1] or identity[3:6] != [0, 0, 1] or identity[2] != stat.S_IFREG | 0o600:
    raise SystemExit("accepted local trusted source metadata drift")
snapshot = Path(Path(sys.argv[3]).read_text().strip())
if hashlib.sha256(snapshot.read_bytes()).hexdigest() != record["sha256"]:
    raise SystemExit("accepted local trusted snapshot hash drift")
PY

printf '%s\n' 'arch-live-local: all checks passed'
