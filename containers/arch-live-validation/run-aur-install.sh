#!/bin/sh

set -eu

export LC_ALL=C.UTF-8
export LANG=C.UTF-8
export LANGUAGE=en

repo_root=$(CDPATH='' cd "$(dirname "$0")/../.." && pwd)
. "$repo_root/scripts/validation-status.sh"
case_policy=$repo_root/containers/arch-live-validation/aur-cases.tsv
case_loader=$repo_root/containers/arch-live-validation/fixtures/aur/load-case.sh
payload_policy=$repo_root/containers/arch-live-validation/fixtures/aur/payload-authority.tsv
conflict_mutator=$repo_root/containers/arch-live-validation/aur-mutate-pkginfo-conflict.py
runtime_policy_root=/usr/share/moguet-live-aur/policy
runtime_case_policy=$runtime_policy_root/aur-cases.tsv
runtime_payload_policy=$runtime_policy_root/payload-authority.tsv
runtime_reference_manifest=/usr/libexec/moguet-live-aur/fixtures/reference-payload.tsv
runtime_pkginfo_manifest=/usr/libexec/moguet-live-aur/fixtures/reference-pkginfo.tsv
metadata_helper=/usr/libexec/moguet-live-aur/aur-archive-metadata-check
pty_runner=$repo_root/tests/run-with-pty.py
production_moguet=$repo_root/moguet
case_root=$HOME/live-aur-case
preflight_root=$case_root/aur-preflight
preflight_checkout=$preflight_root/aur.git
preflight_pkgbuild=$preflight_root/PKGBUILD
preflight_srcinfo=$preflight_root/.SRCINFO
rpc_evidence=$preflight_root/aur-rpc.json
metadata_output=$case_root/production-metadata.output
production_output=$case_root/production-install.output
normalized_output=$case_root/production-install.normalized
gateway_evidence_root=/var/log/moguet-live-aur
gateway_staging_root=/var/lib/moguet-live-aur/staging
gateway_case=aur-install
gateway_negative_case=aur-content-drift-test
gateway_conflict_case=aur-conflict-policy-test
gateway_xattr_case=aur-xattr-metadata-test
gateway_acl_case=aur-acl-metadata-test
gateway_pkgdesc_case=aur-pkgdesc-authority-test
gateway_status=97
current_phase=preflight
current_output=

show_gateway_evidence() {
    for evidence_case in \
        "$gateway_case" "$gateway_negative_case" "$gateway_conflict_case" \
        "$gateway_xattr_case" "$gateway_acl_case" "$gateway_pkgdesc_case"
    do
        evidence_directory=$gateway_evidence_root/$evidence_case
        if [ ! -d "$evidence_directory" ]; then
            continue
        fi
        printf 'arch-live-aur: gateway evidence (%s) files:\n' "$evidence_case" >&2
        find "$evidence_directory" -mindepth 1 -maxdepth 1 \
            -type f -printf '  %f\n' | LC_ALL=C sort >&2
        for evidence_name in \
            original-artifact-path.txt \
            staged-artifact-path.txt \
            stage-hashes.txt \
            archive-metadata-check.txt \
            package-identity.txt \
            validated-payload.tsv \
            validation-timestamp.txt \
            validation-complete.txt
        do
            evidence_path=$evidence_directory/$evidence_name
            if [ -f "$evidence_path" ]; then
                printf 'arch-live-aur: %s:\n' "$evidence_name" >&2
                sed -n '1,160p' "$evidence_path" >&2
            fi
        done
    done
}

fail() {
    printf 'arch-live-aur: FAIL (%s): %s\n' "$current_phase" "$*" >&2
    if [ -n "$current_output" ] && [ -f "$current_output" ]; then
        printf 'arch-live-aur: case output (%s):\n' "$current_output" >&2
        sed -n '1,360p' "$current_output" >&2
    fi
    show_gateway_evidence
    exit 1
}

assert_regular_non_symlink() {
    checked_path=$1
    checked_label=$2
    if [ ! -f "$checked_path" ] || [ -L "$checked_path" ]; then
        fail "$checked_label must be a regular non-symlink: $checked_path"
    fi
}

assert_directory_non_symlink() {
    checked_path=$1
    checked_label=$2
    if [ ! -d "$checked_path" ] || [ -L "$checked_path" ]; then
        fail "$checked_label must be a directory non-symlink: $checked_path"
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

assert_metadata() {
    checked_path=$1
    expected_metadata=$2
    checked_label=$3
    actual_metadata=$(stat -c '%U:%G:%a:%F' "$checked_path")
    if [ "$actual_metadata" != "$expected_metadata" ]; then
        fail "$checked_label metadata drift: $actual_metadata"
    fi
}

sha256_file() {
    if checksum_output=$(sha256sum -- "$1"); then
        printf '%s\n' "${checksum_output%% *}"
        return 0
    else
        return $?
    fi
}

tree_member_paths_raw() {
    member_root=$1
    (
        cd "$member_root" || exit $?
        find . -mindepth 1 -printf '%P\n' || exit $?
    )
}

capture_package_inventory() {
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

package_lines = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
explicit = set(Path(sys.argv[2]).read_text(encoding="utf-8").splitlines())
dependency = set(Path(sys.argv[3]).read_text(encoding="utf-8").splitlines())
records = []
for line in package_lines:
    fields = line.split()
    if len(fields) != 2:
        raise SystemExit(f"unsafe pacman -Q record: {line!r}")
    name, version = fields
    if (name in explicit) == (name in dependency):
        raise SystemExit(f"package reason is not singular: {name}")
    reason = "Explicit" if name in explicit else "Dependency"
    records.append((name, version, reason))
if len(records) != len({record[0] for record in records}):
    raise SystemExit("duplicate package identity in inventory")
Path(sys.argv[4]).write_text(
    "".join("\t".join(record) + "\n" for record in sorted(records)),
    encoding="utf-8",
)
PY
}

inventory_record() {
    package_identity=$1
    inventory_file=$2
    awk -F '\t' -v package_name="$package_identity" \
        '$1 == package_name { print; count++ }
         END { if (count != 1) exit 1 }' "$inventory_file"
}

run_gateway_rejection() {
    rejection_name=$1
    case_mode=$2
    shift 2
    rejection_output=$case_root/rejection-$rejection_name.output
    rejection_status=0
    if [ "$case_mode" = missing ]; then
        if env -u MOGUET_LIVE_AUR_CASE \
            sudo pacman "$@" > "$rejection_output" 2>&1; then
            rejection_status=0
        else
            rejection_status=$?
        fi
    else
        if env MOGUET_LIVE_AUR_CASE="$case_mode" \
            sudo pacman "$@" > "$rejection_output" 2>&1; then
            rejection_status=0
        else
            rejection_status=$?
        fi
    fi
    if [ "$rejection_status" -ne "$gateway_status" ]; then
        current_output=$rejection_output
        fail "gateway rejection $rejection_name returned $rejection_status"
    fi
    assert_contains 'moguet-live-aur-gateway: rejected:' "$rejection_output"
    if [ -e "$gateway_evidence_root/$gateway_case" ] ||
        [ -e "$gateway_staging_root/$gateway_case" ]; then
        current_output=$rejection_output
        fail "gateway rejection $rejection_name consumed accepted evidence"
    fi
    printf '  gateway rejection: %s -> status %s\n' \
        "$rejection_name" "$rejection_status"
}

assert_negative_case_rejected() {
    negative_label=$1
    negative_case=$2
    case_artifact=$3
    expected_diagnostic=$4
    negative_output=$case_root/$negative_label-gateway.output

    capture_package_inventory "$case_root/before-$negative_label"
    negative_status=0
    if env MOGUET_LIVE_AUR_CASE="$negative_case" \
        sudo pacman -U --noconfirm -- "$case_artifact" \
        > "$negative_output" 2>&1; then
        negative_status=0
    else
        negative_status=$?
    fi
    if [ "$negative_status" -ne "$gateway_status" ]; then
        current_output=$negative_output
        fail "$negative_label gateway rejection returned $negative_status"
    fi
    assert_contains "$expected_diagnostic" "$negative_output"
    assert_contains 'moguet-live-aur-gateway: rejected:' "$negative_output"
    capture_package_inventory "$case_root/after-$negative_label"
    cmp -s "$case_root/before-$negative_label.tsv" \
        "$case_root/after-$negative_label.tsv" ||
        fail "$negative_label rejection changed package inventory"
    [ ! -e "$gateway_evidence_root/$gateway_case" ] &&
        [ ! -e "$gateway_staging_root/$gateway_case" ] ||
        fail "$negative_label rejection consumed valid one-shot evidence"
    assert_directory_non_symlink "$gateway_evidence_root/$negative_case" \
        "$negative_label gateway evidence directory"
    assert_directory_non_symlink "$gateway_staging_root/$negative_case" \
        "$negative_label gateway staging directory"
    assert_metadata "$gateway_evidence_root/$negative_case" \
        'root:moguet-validation:750:directory' \
        "$negative_label evidence directory"
    assert_metadata "$gateway_staging_root/$negative_case" \
        'root:moguet-validation:750:directory' \
        "$negative_label staging directory"
    [ ! -e "$gateway_evidence_root/$negative_case/real-pacman-exec.txt" ] ||
        fail "$negative_label case reached real pacman"
    printf '  gateway rejection: %s -> status %s; real pacman not reached\n' \
        "$negative_label" "$negative_status"
}

assert_independent_artifact_unchanged() {
    expected_hash=$1
    artifact_path=$2
    actual_hash=$(sha256_file "$artifact_path") ||
        fail 'negative artifact checksum producer failed'
    [ "$actual_hash" = "$expected_hash" ] ||
        fail 'negative repack changed the independent valid artifact'
}

assert_repacked_path_set() {
    original_artifact=$1
    repacked_artifact=$2
    label=$3
    original_paths=$case_root/$label-original-paths.txt
    repacked_paths=$case_root/$label-repacked-paths.txt
    if validation_capture_sorted_output "$original_paths.raw" "$original_paths" \
        /usr/bin/bsdtar -tf "$original_artifact"; then
        :
    else
        archive_status=$?
        fail "$label original archive listing failed with status $archive_status"
    fi
    if validation_capture_sorted_output "$repacked_paths.raw" "$repacked_paths" \
        /usr/bin/bsdtar -tf "$repacked_artifact"; then
        :
    else
        archive_status=$?
        fail "$label repacked archive listing failed with status $archive_status"
    fi
    cmp -s "$original_paths" "$repacked_paths" ||
        fail "$label repack changed the package path set"
}

mutate_conflict_policy_archive() {
    mutation_archive_path=$1
    python3 "$conflict_mutator" \
        "$mutation_archive_path" "$AUR_CASE_MAKE_DEPENDENCIES"
}

assert_regular_non_symlink "$case_loader" 'tracked AUR case loader'
assert_regular_non_symlink "$conflict_mutator" 'tracked AUR conflict mutator'
# shellcheck source=fixtures/aur/load-case.sh
. "$case_loader"
validation_load_aur_case "$case_policy" || fail 'AUR case policy is invalid'
package_name=$AUR_CASE_PACKAGE_NAME
package_base=$AUR_CASE_PACKAGE_BASE
expected_version=$AUR_CASE_EXPECTED_VERSION
runtime_dependencies=$AUR_CASE_RUNTIME_DEPENDENCIES
source_kind=$AUR_CASE_SOURCE_KIND
install_reason=$AUR_CASE_INSTALL_REASON
fallback_policy=$AUR_CASE_FALLBACK_POLICY
review_required=$AUR_CASE_REVIEW_REQUIRED
expected_aur_git_head=$AUR_CASE_EXPECTED_GIT_HEAD
expected_pkgbuild_sha256=$AUR_CASE_EXPECTED_PKGBUILD_SHA256
expected_srcinfo_sha256=$AUR_CASE_EXPECTED_SRCINFO_SHA256
expected_source_filename=$AUR_CASE_EXPECTED_SOURCE_FILENAME
expected_source_url=$AUR_CASE_EXPECTED_SOURCE_URL
expected_source_sha256=$AUR_CASE_EXPECTED_SOURCE_SHA256
expected_rpc_url_path=$AUR_CASE_EXPECTED_RPC_URL_PATH
expected_architecture=$AUR_CASE_EXPECTED_ARCHITECTURE
tab=$validation_aur_tab

printf '%s\n' ':: AUR case and isolation preflight'
printf '  case identity: %s / PackageBase=%s\n' \
    "$package_name" "$package_base"
printf '  pinned AUR version: %s\n' "$expected_version"
printf '  pinned AUR HEAD: %s\n' "$expected_aur_git_head"
printf '  source authority: %s sha256=%s\n' \
    "$expected_source_filename" "$expected_source_sha256"

assert_regular_non_symlink "$production_moguet" 'production Moguet binary'
assert_regular_non_symlink "$pty_runner" 'PTY runner'
assert_regular_non_symlink "$case_policy" 'tracked AUR case policy'
assert_regular_non_symlink "$payload_policy" 'tracked payload policy'
if [ "$(id -u)" -eq 0 ]; then
    fail 'AUR runner must not execute as root'
fi
[ "$(id -u)" -eq 1000 ] || fail 'validation user UID must be 1000'
[ "$(id -g)" -eq 1000 ] || fail 'validation user GID must be 1000'
[ "$(stat -c '%u' "$production_moguet")" -eq "$(id -u)" ] ||
    fail 'production binary is not validation-user-owned'

if [ -e "$repo_root/.git" ] || [ -L "$repo_root/.git" ]; then
    fail 'container repository copy includes .git'
fi
for forbidden_name in \
    .git-credentials .netrc .ssh .gnupg .env
do
    credential_probe=$case_root/credential-$forbidden_name.raw
    if validation_capture_output "$credential_probe" \
        find "$repo_root" -name "$forbidden_name" -print; then
        :
    else
        credential_status=$?
        fail "credential producer failed with status $credential_status"
    fi
    if [ -s "$credential_probe" ]; then
        fail "container repository copy includes credential path: $forbidden_name"
    fi
done
socket_probe=$case_root/socket-paths.raw
if validation_capture_output "$socket_probe" find "$repo_root" -type s -print; then
    :
else
    socket_status=$?
    fail "socket producer failed with status $socket_status"
fi
if [ -s "$socket_probe" ]; then
    fail 'container repository copy includes a socket'
fi
if [ -e /var/run/docker.sock ] || [ -e /run/docker.sock ]; then
    fail 'Docker socket is visible inside the AUR container'
fi
host_path_probe=$case_root/host-paths.raw
if validation_capture_output "$host_path_probe" grep -R -l \
    --exclude='run-aur-install.sh' '/home/seeke/moguet' "$repo_root"; then
    host_path_status=0
else
    host_path_status=$?
fi
case $host_path_status in
    0) fail 'container source contains a host worktree path reference' ;;
    1) ;;
    *) fail "host-path producer failed with status $host_path_status" ;;
esac

assert_metadata /usr/bin/pacman 'root:root:555:regular file' \
    'canonical pacman gateway'
assert_metadata /usr/libexec/moguet-live-aur/pacman.real \
    'root:root:755:regular file' 'isolated real pacman'
assert_metadata /usr/libexec/moguet-live-aur/aur-stage-artifact.py \
    'root:root:555:regular file' 'root staging helper'
assert_metadata "$metadata_helper" \
    'root:root:555:regular file' 'direct archive metadata helper'
assert_metadata "$gateway_evidence_root" \
    'root:moguet-validation:750:directory' 'gateway evidence root'
assert_metadata "$gateway_staging_root" \
    'root:root:755:directory' 'gateway staging root'
assert_metadata "$runtime_case_policy" \
    'root:root:444:regular file' 'runtime AUR case policy'
assert_metadata "$runtime_payload_policy" \
    'root:root:444:regular file' 'runtime payload policy'
assert_metadata "$runtime_reference_manifest" \
    'root:root:444:regular file' 'runtime reference payload manifest'
assert_metadata "$runtime_pkginfo_manifest" \
    'root:root:444:regular file' 'runtime reference PKGINFO manifest'
cmp -s "$case_policy" "$runtime_case_policy" ||
    fail 'root-owned case policy differs from the tracked authority'
cmp -s "$payload_policy" "$runtime_payload_policy" ||
    fail 'root-owned payload policy differs from the tracked authority'
pacman_conf_noextract=$case_root/pacman-conf-noextract.txt
pacman-conf NoExtract > "$pacman_conf_noextract"
grep -Fx -- "!$AUR_CASE_README_PATH" \
    "$pacman_conf_noextract" >/dev/null ||
    fail 'container pacman config does not retain the exact expected README'

python3 - "$payload_policy" "$runtime_reference_manifest" "$package_name" <<'PY'
from pathlib import Path
import re
import sys

static_path, manifest_path = map(Path, sys.argv[1:3])
package_name = sys.argv[3]
static_header = ("# path", "type", "mode", "sha256")
manifest_header = ("# path", "type", "mode", "owner", "group", "sha256")

def parse(path: Path, header: tuple[str, ...]) -> list[tuple[str, ...]]:
    text = path.read_text(encoding="utf-8")
    if not text.endswith("\n"):
        raise SystemExit(f"{path} lacks a terminal newline")
    rows = [tuple(line.split("\t")) for line in text.splitlines()]
    if not rows or rows[0] != header or any(len(row) != len(header) for row in rows):
        raise SystemExit(f"{path} is not exact-tab authority TSV")
    if any(not row[0] for row in rows[1:]):
        raise SystemExit(f"{path} has an empty authority row")
    return rows[1:]

static_rows = parse(static_path, static_header)
static = {}
for path, entry_type, mode, content_hash in static_rows:
    if path in static:
        raise SystemExit("static payload authority has duplicate paths")
    if path.startswith("/") or "/../" in path or path.startswith("../") or "//" in path:
        raise SystemExit("static payload authority has path traversal")
    static[path] = (entry_type, mode, content_hash)
if [row[0] for row in static_rows] != sorted(static):
    raise SystemExit("static payload authority is not sorted")
required = {
    f"usr/bin/{package_name}": ("regular", "0755", "-"),
    f"usr/share/doc/{package_name}/README.md": None,
    f"usr/share/licenses/{package_name}/LICENSE": None,
}
for path, identity in required.items():
    if path not in static:
        raise SystemExit(f"static payload authority lacks {path}")
    if identity is not None and static[path] != identity:
        raise SystemExit(f"static payload authority identity drift: {path}")

manifest_rows = parse(manifest_path, manifest_header)
manifest = {}
for path, entry_type, mode, owner, group, content_hash in manifest_rows:
    if path in manifest:
        raise SystemExit("reference payload manifest has duplicate paths")
    manifest[path] = (entry_type, mode, owner, group, content_hash)
if [row[0] for row in manifest_rows] != sorted(manifest):
    raise SystemExit("reference payload manifest is not sorted")
if set(manifest) != set(static):
    raise SystemExit("reference payload manifest path set drift")
for path, (entry_type, mode, static_hash) in static.items():
    actual_type, actual_mode, owner, group, actual_hash = manifest[path]
    if (actual_type, actual_mode, owner, group) != (entry_type, mode, "root", "root"):
        raise SystemExit(f"reference manifest identity drift: {path}")
    if entry_type == "regular" and static_hash == "-":
        if not re.fullmatch(r"[0-9a-f]{64}", actual_hash):
            raise SystemExit(f"reference dynamic hash is absent or malformed: {path}")
    elif actual_hash != static_hash:
        raise SystemExit(f"reference static content hash drift: {path}")
PY

python3 - "$runtime_pkginfo_manifest" <<'PY'
from collections import Counter
from pathlib import Path
import sys

path = Path(sys.argv[1])
rows = [tuple(line.split("\t")) for line in path.read_text(encoding="utf-8").splitlines()]
expected_header = ("# key", "value")
allowed = {
    "pkgname", "pkgbase", "pkgver", "pkgdesc", "url", "size", "arch",
    "license", "depend", "makedepend", "xdata",
}
if not rows or rows[0] != expected_header:
    raise SystemExit("root PKGINFO manifest header drift")
pairs = rows[1:]
if not pairs or any(len(pair) != 2 or not pair[1] for pair in pairs):
    raise SystemExit("root PKGINFO manifest is not exact-tab TSV")
if any(key not in allowed for key, _value in pairs):
    raise SystemExit("root PKGINFO manifest contains a volatile or unknown field")
if pairs != sorted(pairs):
    raise SystemExit("root PKGINFO manifest is not sorted")
if Counter(key for key, _value in pairs)["pkgdesc"] != 1:
    raise SystemExit("root PKGINFO manifest lacks exactly one pkgdesc")
for required in {"pkgdesc", "url", "license", "size"}:
    if not any(key == required for key, _value in pairs):
        raise SystemExit(f"root PKGINFO manifest lacks {required}")
PY

if pacman -Q "$package_name" >/dev/null 2>&1; then
    fail "$package_name is already installed"
fi
official_query_output=$case_root/official-query.output
if pacman -Si "$package_name" > "$official_query_output" 2>&1; then
    current_output=$official_query_output
    fail "$package_name unexpectedly exists in an official repository"
fi
if [ -e "$XDG_CACHE_HOME/moguet" ] || [ -L "$XDG_CACHE_HOME/moguet" ]; then
    fail 'Moguet cache is not fresh at container start'
fi
if [ -e "$gateway_evidence_root/$gateway_case" ] ||
    [ -e "$gateway_staging_root/$gateway_case" ]; then
    fail 'gateway accepted evidence already exists'
fi

mkdir -m 0700 "$preflight_root"

printf '%s\n' ':: root gateway fail-closed self-test'
capture_package_inventory "$case_root/before-rejections"
run_gateway_rejection syu "$gateway_case" -Syu
run_gateway_rejection remove "$gateway_case" -R "$package_name"
run_gateway_rejection outside-artifact "$gateway_case" \
    -U -- /tmp/fake.pkg.tar.zst
run_gateway_rejection multiple-artifacts "$gateway_case" \
    -U --noconfirm -- /tmp/path1.pkg.tar.zst /tmp/path2.pkg.tar.zst
run_gateway_rejection asdeps "$gateway_case" \
    -U --asdeps -- /tmp/fake.pkg.tar.zst
run_gateway_rejection unknown-case unknown-case -Syu
run_gateway_rejection missing-case missing -Syu
capture_package_inventory "$case_root/after-rejections"
cmp -s "$case_root/before-rejections.tsv" "$case_root/after-rejections.tsv" ||
    fail 'gateway rejection self-test changed package inventory'

printf '%s\n' ':: independent public AUR preflight'
current_phase=aur-rpc
current_output=$rpc_evidence
aur_rpc_url="https://aur.archlinux.org/rpc/v5/info?arg[]=$package_name"
curl --fail --silent --show-error --location \
    --proto '=https' --tlsv1.2 --max-time 30 \
    "$aur_rpc_url" > "$rpc_evidence" ||
    fail 'public AUR RPC query failed'
python3 - \
    "$rpc_evidence" "$package_name" "$package_base" "$expected_version" \
    "$runtime_dependencies" "$AUR_CASE_MAKE_DEPENDENCIES" \
    "$expected_rpc_url_path" <<'PY'
import json
from pathlib import Path
import sys

document = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
package, package_base, version = sys.argv[2:5]
runtime_dependencies = set(filter(None, sys.argv[5].split(",")))
make_dependencies = set(filter(None, sys.argv[6].split(",")))
expected_url_path = sys.argv[7]
if document.get("version") != 5:
    raise SystemExit("AUR RPC version drift")
if document.get("type") != "multiinfo":
    raise SystemExit("AUR RPC type drift")
results = document.get("results")
if document.get("resultcount") != 1 or not isinstance(results, list) or len(results) != 1:
    raise SystemExit("AUR RPC result cardinality drift")
result = results[0]
expected_scalars = {
    "Name": package,
    "PackageBase": package_base,
    "Version": version,
    "URLPath": expected_url_path,
}
for key, expected in expected_scalars.items():
    if result.get(key) != expected:
        raise SystemExit(f"AUR RPC {key} drift")
if set(result.get("Depends", [])) != runtime_dependencies:
    raise SystemExit("AUR RPC runtime dependency drift")
if set(result.get("MakeDepends", [])) != make_dependencies:
    raise SystemExit("AUR RPC make dependency drift")
if result.get("OutOfDate") is not None:
    raise SystemExit("AUR RPC package is out of date")
vcs_suffixes = ("-git", "-svn", "-hg", "-bzr", "-cvs", "-darcs")
if package.endswith(vcs_suffixes):
    raise SystemExit("tracked AUR package became a VCS package")
print(
    f"AUR RPC: Name={package} PackageBase={package_base} "
    f"Version={version} URLPath={expected_url_path}"
)
PY

aur_git_url="https://aur.archlinux.org/$package_base.git"
ls_remote_output=$preflight_root/ls-remote.output
current_phase=aur-git-head
current_output=$ls_remote_output
git ls-remote "$aur_git_url" HEAD > "$ls_remote_output" ||
    fail 'AUR git ls-remote failed'
ls_remote_line_count=$(wc -l < "$ls_remote_output")
ls_remote_parsed_count=$(awk -F '\t' \
    'NF == 2 && $2 == "HEAD" && $1 ~ /^[0-9a-f]{40}$/ { count++ }
     END { print count + 0 }' "$ls_remote_output")
[ "$ls_remote_line_count" -gt 0 ] || fail 'AUR git ls-remote returned no HEAD'
[ "$ls_remote_parsed_count" -eq "$ls_remote_line_count" ] ||
    fail 'AUR git ls-remote returned a non-HEAD or malformed record'
current_aur_head=$(awk -F '\t' '
    { heads[$1] = 1 }
    END {
        for (head in heads) {
            count++
            selected = head
        }
        if (count != 1) exit 1
        print selected
    }
' "$ls_remote_output") ||
    fail 'AUR git ls-remote returned multiple distinct HEAD identities'
[ "$current_aur_head" = "$expected_aur_git_head" ] ||
    fail "review-required AUR HEAD drift: $current_aur_head"
printf '  current AUR HEAD: %s\n' "$current_aur_head"

git clone --depth 1 --single-branch --no-checkout \
    "$aur_git_url" "$preflight_checkout" ||
    fail 'independent AUR preflight clone failed'
preflight_head=$(git -C "$preflight_checkout" rev-parse HEAD)
[ "$preflight_head" = "$expected_aur_git_head" ] ||
    fail 'preflight clone HEAD changed after ls-remote'
git -C "$preflight_checkout" show \
    "${preflight_head}:PKGBUILD" > "$preflight_pkgbuild"
git -C "$preflight_checkout" show \
    "${preflight_head}:.SRCINFO" > "$preflight_srcinfo"
assert_regular_non_symlink "$preflight_pkgbuild" 'preflight PKGBUILD'
assert_regular_non_symlink "$preflight_srcinfo" 'preflight .SRCINFO'
preflight_pkgbuild_hash=$(sha256_file "$preflight_pkgbuild") ||
    fail 'preflight PKGBUILD checksum producer failed'
[ "$preflight_pkgbuild_hash" = \
    "$expected_pkgbuild_sha256" ] || fail 'review-required PKGBUILD byte drift'
preflight_srcinfo_hash=$(sha256_file "$preflight_srcinfo") ||
    fail 'preflight .SRCINFO checksum producer failed'
[ "$preflight_srcinfo_hash" = \
    "$expected_srcinfo_sha256" ] || fail 'review-required .SRCINFO byte drift'
if validation_capture_sorted_output \
    "$preflight_root/tree.raw" "$preflight_root/tree.txt" \
    git -C "$preflight_checkout" ls-tree --name-only HEAD; then
    :
else
    tree_status=$?
    fail "AUR tree producer failed with status $tree_status"
fi
if validation_capture_sorted_output \
    "$preflight_root/expected-tree.raw" "$preflight_root/expected-tree.txt" \
    printf '%s\n' .SRCINFO .gitignore PKGBUILD; then
    :
else
    tree_status=$?
    fail "expected AUR tree normalization failed with status $tree_status"
fi
cmp -s "$preflight_root/expected-tree.txt" "$preflight_root/tree.txt" ||
    fail 'review-required AUR repository tree drift'

python3 - \
    "$preflight_pkgbuild" "$preflight_srcinfo" "$payload_policy" \
    "$package_name" "$package_base" "$expected_version" \
    "$runtime_dependencies" "$AUR_CASE_MAKE_DEPENDENCIES" \
    "$expected_source_filename" \
    "$expected_source_url" "$expected_source_sha256" "$expected_architecture" <<'PY'
from collections import defaultdict
from pathlib import Path
import re
import sys

(
    pkgbuild_path,
    srcinfo_path,
    payload_path,
    package,
    package_base,
    version,
    runtime_text,
    make_text,
    source_filename,
    source_url,
    source_sha256,
    architecture,
) = sys.argv[1:]
pkgver, pkgrel = version.rsplit("-", 1)
fields = defaultdict(list)
for raw_line in Path(srcinfo_path).read_text(encoding="utf-8").splitlines():
    line = raw_line.strip()
    if not line:
        continue
    if " = " not in line:
        raise SystemExit(f"unsafe .SRCINFO line: {raw_line!r}")
    key, value = line.split(" = ", 1)
    fields[key].append(value)

singular = {
    "pkgbase": package_base,
    "pkgname": package,
    "pkgver": pkgver,
    "pkgrel": pkgrel,
    "arch": architecture,
    "source": f"{source_filename}::{source_url}",
    "sha256sums": source_sha256,
}
for key, expected in singular.items():
    if fields.get(key) != [expected]:
        raise SystemExit(f".SRCINFO {key} drift")
if set(fields.get("depends", [])) != set(runtime_text.split(",")):
    raise SystemExit(".SRCINFO runtime dependency drift")
if set(fields.get("makedepends", [])) != set(make_text.split(",")):
    raise SystemExit(".SRCINFO make dependency drift")
if "install" in fields:
    raise SystemExit(".SRCINFO gained an install script")
if len(fields.get("source", [])) != 1:
    raise SystemExit(".SRCINFO no longer has exactly one source archive")
if re.match(r"^(git|svn|hg|bzr)\+", source_url):
    raise SystemExit("source archive changed to a VCS source")

pkgbuild = Path(pkgbuild_path).read_text(encoding="utf-8")
if re.search(r"(?m)^\s*install\s*=", pkgbuild):
    raise SystemExit("PKGBUILD gained an install script declaration")
package_match = re.search(r"(?ms)^package\(\) \{\n(.*?)\n\}\s*$", pkgbuild)
if package_match is None:
    raise SystemExit("PKGBUILD package() boundary drift")
package_lines = [line.strip() for line in package_match.group(1).splitlines() if line.strip()]
expected_package_lines = {
    'cd "$pkgname-$pkgver"',
    'install -Dm755 "bin/$pkgname" -t "$pkgdir/usr/bin"',
    'install -Dm644 LICENSE -t "$pkgdir/usr/share/licenses/$pkgname"',
    'install -Dm644 README.md -t "$pkgdir/usr/share/doc/$pkgname"',
}
if len(package_lines) != 4 or set(package_lines) != expected_package_lines:
    raise SystemExit("PKGBUILD package() payload command drift")
expected_payload = {
    f"usr/bin/{package}",
    f"usr/share/doc/{package}/README.md",
    f"usr/share/licenses/{package}/LICENSE",
}
payload_rows = [
    line.split("\t")
    for line in Path(payload_path).read_text(encoding="utf-8").splitlines()
]
if not payload_rows or payload_rows[0] != ["# path", "type", "mode", "sha256"]:
    raise SystemExit("tracked payload authority header drift")
payload = {row[0] for row in payload_rows[1:] if len(row) == 4 and row[1] == "regular"}
if payload != expected_payload:
    raise SystemExit("PKGBUILD package() no longer maps to tracked payload authority")
print(
    f"PKGBUILD/.SRCINFO: pkgname={package} pkgbase={package_base} "
    f"version={version} arch={architecture} source={source_filename}"
)
PY

printf '%s\n' ':: root gateway binary-content fail-closed test'
current_phase=content-drift-negative
negative_checkout=$case_root/content-drift-checkout
negative_artifact_root=$case_root/content-drift-artifact
negative_raw_tar=$negative_artifact_root/$package_name.pkg.tar
negative_workspace=
negative_artifact=
negative_output=$case_root/content-drift-gateway.output
/usr/bin/git clone --no-checkout "$aur_git_url" "$negative_checkout" ||
    fail 'content-drift clone failed'
/usr/bin/git -C "$negative_checkout" checkout --detach "$preflight_head" ||
    fail 'content-drift checkout failed'
cmp -s "$preflight_pkgbuild" "$negative_checkout/PKGBUILD" ||
    fail 'content-drift PKGBUILD differs from the pinned review bytes'
cmp -s "$preflight_srcinfo" "$negative_checkout/.SRCINFO" ||
    fail 'content-drift .SRCINFO differs from the pinned review bytes'
(
    cd "$negative_checkout"
    /usr/bin/makepkg --noconfirm --nodeps
) || fail 'content-drift package build failed'
negative_artifact=$negative_checkout/$package_name-$expected_version-$expected_architecture.pkg.tar.zst
assert_regular_non_symlink "$negative_artifact" 'content-drift source artifact'
/usr/bin/mkdir -m 0700 "$negative_artifact_root"
binary_payload_path=usr/bin/$package_name
reference_binary_hash=$(awk -F "$tab" -v target="$binary_payload_path" \
    '$1 == target { print $6; count++ }
     END { if (count != 1) exit 1 }' "$runtime_reference_manifest") ||
    fail 'reference manifest has no unique binary hash'
negative_binary=$negative_artifact_root/$package_name.binary.raw
if validation_capture_output "$negative_binary" \
    /usr/bin/bsdtar -xOf "$negative_artifact" "$binary_payload_path"; then
    negative_binary_hash=$(sha256_file "$negative_binary") ||
        fail 'negative binary checksum producer failed'
else
    archive_status=$?
    fail "negative binary archive producer failed with status $archive_status"
fi
[ "$negative_binary_hash" = "$reference_binary_hash" ] ||
    fail 'independent content-drift artifact does not match the per-image reference binary'
negative_artifact_hash=$(sha256_file "$negative_artifact") ||
    fail 'negative artifact checksum producer failed'
/usr/bin/mkdir -m 0700 "$XDG_CACHE_HOME/moguet"
negative_workspace=$(mktemp -d "$XDG_CACHE_HOME/moguet/.artifact-workspace~-XXXXXX")
negative_gateway_artifact=$negative_workspace/$package_name-$expected_version-$expected_architecture.pkg.tar.zst
/usr/bin/zstd --decompress --stdout "$negative_artifact" > "$negative_raw_tar"
python3 - "$negative_raw_tar" "$binary_payload_path" <<'PY'
from pathlib import Path
import sys

archive = Path(sys.argv[1])
target = sys.argv[2].encode("utf-8")
with archive.open("r+b") as stream:
    while True:
        header_offset = stream.tell()
        header = stream.read(512)
        if len(header) != 512:
            raise SystemExit("truncated tar while locating binary payload")
        if header == b"\0" * 512:
            raise SystemExit("binary payload is absent from the package tar")
        name = header[:100].split(b"\0", 1)[0]
        prefix = header[345:500].split(b"\0", 1)[0]
        path = b"/".join(part for part in (prefix, name) if part)
        size_text = header[124:136].split(b"\0", 1)[0].strip() or b"0"
        size = int(size_text, 8)
        data_offset = header_offset + 512
        if path == target:
            if header[156:157] not in {b"", b"0", b"\0"} or size < 1:
                raise SystemExit("binary payload tar member is not a non-empty regular file")
            stream.seek(data_offset)
            first = stream.read(1)
            stream.seek(data_offset)
            stream.write(bytes([first[0] ^ 1]))
            break
        stream.seek(data_offset + ((size + 511) // 512) * 512)
PY
/usr/bin/zstd --quiet --force -o "$negative_gateway_artifact" "$negative_raw_tar"
/usr/bin/touch "$negative_gateway_artifact"
assert_repacked_path_set "$negative_artifact" "$negative_gateway_artifact" content-drift
capture_package_inventory "$case_root/before-content-drift"
negative_status=0
if env MOGUET_LIVE_AUR_CASE="$gateway_negative_case" \
    sudo pacman -U --noconfirm -- "$negative_gateway_artifact" \
    > "$negative_output" 2>&1; then
    negative_status=0
else
    negative_status=$?
fi
[ "$negative_status" -eq "$gateway_status" ] || {
    current_output=$negative_output
    fail "content-drift gateway rejection returned $negative_status"
}
assert_contains "archive payload content hash drift: $binary_payload_path" \
    "$negative_output"
assert_contains 'moguet-live-aur-gateway: rejected:' "$negative_output"
capture_package_inventory "$case_root/after-content-drift"
cmp -s "$case_root/before-content-drift.tsv" \
    "$case_root/after-content-drift.tsv" ||
    fail 'content-drift gateway rejection changed package inventory'
[ ! -e "$gateway_evidence_root/$gateway_case" ] &&
    [ ! -e "$gateway_staging_root/$gateway_case" ] ||
    fail 'content-drift rejection consumed valid one-shot evidence'
assert_directory_non_symlink "$gateway_evidence_root/$gateway_negative_case" \
    'content-drift gateway evidence directory'
assert_directory_non_symlink "$gateway_staging_root/$gateway_negative_case" \
    'content-drift gateway staging directory'
assert_metadata "$gateway_evidence_root/$gateway_negative_case" \
    'root:moguet-validation:750:directory' 'content-drift evidence directory'
assert_metadata "$gateway_staging_root/$gateway_negative_case" \
    'root:moguet-validation:750:directory' 'content-drift staging directory'
[ ! -e "$gateway_evidence_root/$gateway_negative_case/real-pacman-exec.txt" ] ||
    fail 'content-drift case reached real pacman'
assert_independent_artifact_unchanged "$negative_artifact_hash" "$negative_artifact"
printf '%s\n' ':: root gateway transaction-metadata fail-closed test'
current_phase=conflict-policy-negative
conflict_raw_tar=$negative_artifact_root/$package_name-conflict.pkg.tar
conflict_workspace=$(mktemp -d "$XDG_CACHE_HOME/moguet/.artifact-workspace~-XXXXXX")
conflict_gateway_artifact=$conflict_workspace/$package_name-$expected_version-$expected_architecture.pkg.tar.zst
conflict_output=$case_root/conflict-policy-gateway.output
/usr/bin/zstd --decompress --stdout "$negative_artifact" > "$conflict_raw_tar"
if mutate_conflict_policy_archive "$conflict_raw_tar"; then
    :
else
    mutation_status=$?
    fail "conflict mutation failed with status $mutation_status"
fi
/usr/bin/zstd --quiet --force -o "$conflict_gateway_artifact" "$conflict_raw_tar"
/usr/bin/touch "$conflict_gateway_artifact"
assert_repacked_path_set "$negative_artifact" "$conflict_gateway_artifact" conflict-policy
capture_package_inventory "$case_root/before-conflict-policy"
conflict_status=0
if env MOGUET_LIVE_AUR_CASE="$gateway_conflict_case" \
    sudo pacman -U --noconfirm -- "$conflict_gateway_artifact" \
    > "$conflict_output" 2>&1; then
    conflict_status=0
else
    conflict_status=$?
fi
[ "$conflict_status" -eq "$gateway_status" ] || {
    current_output=$conflict_output
    fail "conflict-policy gateway rejection returned $conflict_status"
}
assert_contains 'forbidden transaction field: conflict' "$conflict_output"
assert_contains 'moguet-live-aur-gateway: rejected:' "$conflict_output"
capture_package_inventory "$case_root/after-conflict-policy"
cmp -s "$case_root/before-conflict-policy.tsv" \
    "$case_root/after-conflict-policy.tsv" ||
    fail 'conflict-policy gateway rejection changed package inventory'
[ ! -e "$gateway_evidence_root/$gateway_case" ] &&
    [ ! -e "$gateway_staging_root/$gateway_case" ] ||
    fail 'conflict-policy rejection consumed valid one-shot evidence'
assert_directory_non_symlink "$gateway_evidence_root/$gateway_conflict_case" \
    'conflict-policy gateway evidence directory'
assert_directory_non_symlink "$gateway_staging_root/$gateway_conflict_case" \
    'conflict-policy gateway staging directory'
assert_metadata "$gateway_evidence_root/$gateway_conflict_case" \
    'root:moguet-validation:750:directory' 'conflict-policy evidence directory'
assert_metadata "$gateway_staging_root/$gateway_conflict_case" \
    'root:moguet-validation:750:directory' 'conflict-policy staging directory'
[ ! -e "$gateway_evidence_root/$gateway_conflict_case/real-pacman-exec.txt" ] ||
    fail 'conflict-policy case reached real pacman'
assert_independent_artifact_unchanged "$negative_artifact_hash" "$negative_artifact"

printf '%s\n' ':: root gateway xattr metadata fail-closed test'
current_phase=xattr-metadata-negative
xattr_artifact_root=$case_root/xattr-metadata-artifact
xattr_tree=$xattr_artifact_root/tree
xattr_list=$xattr_artifact_root/archive-paths.txt
xattr_raw_tar=$xattr_artifact_root/$package_name.pkg.tar
xattr_workspace=$(mktemp -d "$XDG_CACHE_HOME/moguet/.artifact-workspace~-XXXXXX")
xattr_gateway_artifact=$xattr_workspace/$package_name-$expected_version-$expected_architecture.pkg.tar.zst
/usr/bin/mkdir -m 0700 "$xattr_artifact_root"
/usr/bin/mkdir -m 0700 "$xattr_tree"
/usr/bin/bsdtar -xf "$negative_artifact" -C "$xattr_tree"
python3 - "$xattr_tree/$binary_payload_path" <<'PY'
import os
from pathlib import Path
import sys

target = Path(sys.argv[1])
os.setxattr(target, b"user.moguet-live-test", b"xattr-negative")
PY
if validation_capture_sorted_output "$xattr_list.raw" "$xattr_list" \
    tree_member_paths_raw "$xattr_tree"; then
    :
else
    path_status=$?
    fail "xattr archive path producer failed with status $path_status"
fi
(
    cd "$xattr_tree"
    /usr/bin/tar --format=pax --xattrs --acls --numeric-owner --owner=0 --group=0 \
        --no-recursion -cf "$xattr_raw_tar" -T "$xattr_list"
)
/usr/bin/zstd --quiet --force -o "$xattr_gateway_artifact" "$xattr_raw_tar"
/usr/bin/touch "$xattr_gateway_artifact"
assert_repacked_path_set "$negative_artifact" "$xattr_gateway_artifact" xattr-metadata
assert_negative_case_rejected xattr-metadata "$gateway_xattr_case" \
    "$xattr_gateway_artifact" "category=xattr entry=$binary_payload_path"
assert_independent_artifact_unchanged "$negative_artifact_hash" "$negative_artifact"

printf '%s\n' ':: root gateway ACL metadata fail-closed test'
current_phase=acl-metadata-negative
acl_artifact_root=$case_root/acl-metadata-artifact
acl_tree=$acl_artifact_root/tree
acl_list=$acl_artifact_root/archive-paths.txt
acl_raw_tar=$acl_artifact_root/$package_name.pkg.tar
acl_workspace=$(mktemp -d "$XDG_CACHE_HOME/moguet/.artifact-workspace~-XXXXXX")
acl_gateway_artifact=$acl_workspace/$package_name-$expected_version-$expected_architecture.pkg.tar.zst
/usr/bin/mkdir -m 0700 "$acl_artifact_root"
/usr/bin/mkdir -m 0700 "$acl_tree"
/usr/bin/bsdtar -xf "$negative_artifact" -C "$acl_tree"
/usr/bin/setfacl -m u:65534:rx "$acl_tree/$binary_payload_path"
if validation_capture_sorted_output "$acl_list.raw" "$acl_list" \
    tree_member_paths_raw "$acl_tree"; then
    :
else
    path_status=$?
    fail "ACL archive path producer failed with status $path_status"
fi
(
    cd "$acl_tree"
    /usr/bin/tar --format=pax --xattrs --acls --numeric-owner --owner=0 --group=0 \
        --no-recursion -cf "$acl_raw_tar" -T "$acl_list"
)
/usr/bin/zstd --quiet --force -o "$acl_gateway_artifact" "$acl_raw_tar"
/usr/bin/touch "$acl_gateway_artifact"
assert_repacked_path_set "$negative_artifact" "$acl_gateway_artifact" acl-metadata
assert_negative_case_rejected acl-metadata "$gateway_acl_case" \
    "$acl_gateway_artifact" "category=acl entry=$binary_payload_path"
assert_independent_artifact_unchanged "$negative_artifact_hash" "$negative_artifact"

printf '%s\n' ':: root gateway PKGINFO authority fail-closed test'
current_phase=pkgdesc-authority-negative
pkgdesc_artifact_root=$case_root/pkgdesc-authority-artifact
pkgdesc_raw_tar=$pkgdesc_artifact_root/$package_name.pkg.tar
pkgdesc_workspace=$(mktemp -d "$XDG_CACHE_HOME/moguet/.artifact-workspace~-XXXXXX")
pkgdesc_gateway_artifact=$pkgdesc_workspace/$package_name-$expected_version-$expected_architecture.pkg.tar.zst
/usr/bin/mkdir -m 0700 "$pkgdesc_artifact_root"
/usr/bin/zstd --decompress --stdout "$negative_artifact" > "$pkgdesc_raw_tar"
python3 - "$pkgdesc_raw_tar" <<'PY'
from pathlib import Path
import sys

archive = Path(sys.argv[1])
with archive.open("r+b") as stream:
    while True:
        header_offset = stream.tell()
        header = stream.read(512)
        if len(header) != 512:
            raise SystemExit("truncated tar while locating .PKGINFO")
        if header == b"\0" * 512:
            raise SystemExit(".PKGINFO is absent from the package tar")
        name = header[:100].split(b"\0", 1)[0]
        prefix = header[345:500].split(b"\0", 1)[0]
        path = b"/".join(part for part in (prefix, name) if part)
        size_text = header[124:136].split(b"\0", 1)[0].strip() or b"0"
        size = int(size_text, 8)
        data_offset = header_offset + 512
        if path == b".PKGINFO":
            stream.seek(data_offset)
            payload = stream.read(size)
            prefix_value = b"pkgdesc = "
            start = payload.find(prefix_value)
            end = payload.find(b"\n", start)
            if start < 0 or end < 0:
                raise SystemExit(".PKGINFO has no pkgdesc line")
            value_length = end - (start + len(prefix_value))
            if value_length < 1:
                raise SystemExit(".PKGINFO pkgdesc is empty")
            replacement = prefix_value + (b"x" * value_length) + b"\n"
            stream.seek(data_offset + start)
            stream.write(replacement)
            break
        stream.seek(data_offset + ((size + 511) // 512) * 512)
PY
/usr/bin/zstd --quiet --force -o "$pkgdesc_gateway_artifact" "$pkgdesc_raw_tar"
/usr/bin/touch "$pkgdesc_gateway_artifact"
assert_repacked_path_set "$negative_artifact" "$pkgdesc_gateway_artifact" pkgdesc-authority
assert_negative_case_rejected pkgdesc-authority "$gateway_pkgdesc_case" \
    "$pkgdesc_gateway_artifact" '.PKGINFO pkgdesc authority mismatch'
assert_independent_artifact_unchanged "$negative_artifact_hash" "$negative_artifact"

/usr/bin/rm -rf -- \
    "$negative_checkout" "$negative_artifact_root" "$xattr_artifact_root" \
    "$acl_artifact_root" "$pkgdesc_artifact_root" "$negative_workspace" \
    "$conflict_workspace" "$xattr_workspace" "$acl_workspace" "$pkgdesc_workspace"
for removed_negative_path in \
    "$negative_checkout" "$negative_workspace" "$conflict_workspace" \
    "$xattr_workspace" "$acl_workspace" "$pkgdesc_workspace"
do
    [ ! -e "$removed_negative_path" ] ||
        fail 'negative artifact was retained near the positive install target'
done
/usr/bin/rmdir "$XDG_CACHE_HOME/moguet"
printf '%s\n' '  gateway rejection: binary content drift -> status 97; real pacman not reached'
printf '%s\n' '  gateway rejection: .PKGINFO conflict -> status 97; real pacman not reached'
printf '%s\n' '  gateway rejection: xattr artifact -> status 97; real pacman not reached'
printf '%s\n' '  gateway rejection: ACL artifact -> status 97; real pacman not reached'
printf '%s\n' '  gateway rejection: pkgdesc artifact -> status 97; real pacman not reached'

printf '%s\n' ':: production AUR RPC metadata query'
current_phase=production-metadata
current_output=$metadata_output
if ! "$production_moguet" -Si --aur "$package_name" \
    > "$metadata_output" 2>&1; then
    fail 'production Moguet AUR metadata query failed'
fi
for metadata_line in \
    "Repository      : aur" \
    "Name            : $package_name" \
    "Package Base    : $package_base" \
    "Version         : $expected_version" \
    "Out of Date     : no"
do
    assert_contains "$metadata_line" "$metadata_output"
done
python3 - \
    "$metadata_output" "$runtime_dependencies" \
    "$AUR_CASE_MAKE_DEPENDENCIES" <<'PY'
from pathlib import Path
import re
import sys

text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", Path(sys.argv[1]).read_text(encoding="utf-8"))
values = {}
for line in text.splitlines():
    if " : " in line:
        key, value = line.split(" : ", 1)
        values[key.strip()] = value.strip()
for label, expected in (
    ("Depends On", set(sys.argv[2].split(","))),
    ("Make Deps", set(sys.argv[3].split(","))),
):
    actual = set() if values.get(label) == "None" else set(values.get(label, "").split())
    if actual != expected:
        raise SystemExit(f"production metadata {label} drift: {sorted(actual)!r}")
PY
assert_not_contains 'Running: pacman -Si' "$metadata_output"
printf '  production metadata: %s / PackageBase=%s / Version=%s / AUR\n' \
    "$package_name" "$package_base" "$expected_version"

capture_package_inventory "$case_root/before-install"
saved_ifs=$IFS
IFS=,
all_dependencies=$runtime_dependencies,$AUR_CASE_MAKE_DEPENDENCIES
for dependency_name in $all_dependencies; do
    inventory_record "$dependency_name" "$case_root/before-install.tsv" \
        >> "$case_root/dependencies-before.tsv" ||
        fail "required existing dependency is absent: $dependency_name"
done
IFS=$saved_ifs

printf '%s\n' ':: production AUR clone / review / source build / install'
current_phase=production-install
current_output=$production_output
build_start_marker=$case_root/build-start.marker
touch "$build_start_marker"
build_start_ns=$(date '+%s%N')
production_status=0
production_input=$case_root/production-install.input
printf 'n\n' >"$production_input"
if env MOGUET_LIVE_AUR_CASE="$gateway_case" \
    python3 "$pty_runner" --timeout 900 -- \
        "$production_moguet" --nodiff --noconfirm -S --aur "$package_name" \
        <"$production_input" >"$production_output" 2>&1; then
    production_status=0
else
    production_status=$?
fi
if [ "$production_status" -eq 124 ]; then
    fail 'production AUR install timed out'
fi
if [ "$production_status" -ne 0 ]; then
    fail "production AUR install returned $production_status"
fi
python3 - "$production_output" "$normalized_output" <<'PY'
from pathlib import Path
import re
import sys

text = Path(sys.argv[1]).read_bytes().replace(b"\r", b"")
text = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", text)
Path(sys.argv[2]).write_bytes(text)
PY

assert_contains 'Review target: PKGBUILD' "$normalized_output"
assert_contains 'Skipping prompt (--noconfirm): Edit PKGBUILD? -> no' \
    "$normalized_output"
assert_not_contains 'Skipping PKGBUILD/.install review (--noedit).' \
    "$normalized_output"
assert_not_contains 'Install script(s) present; review before build:' \
    "$normalized_output"
assert_not_contains 'Edit install script' "$normalized_output"
assert_contains "Running: git clone $aur_git_url $package_base" \
    "$normalized_output"
assert_contains "'makepkg' '--packagelist'" "$normalized_output"
assert_contains "'makepkg' '-sc' '--noconfirm'" "$normalized_output"
assert_not_contains "Running: LC_ALL=C 'pacman' '-U' '--print' '--print-format'" \
    "$normalized_output"
assert_contains "Running: '/usr/bin/sudo' '--' '/usr/local/libexec/moguet/moguet-source-artifact-install-helper' 'install-legacy'" \
    "$normalized_output"
assert_contains "PackageBase result: $package_base" "$normalized_output"
assert_contains \
    "required child: $package_name -> $package_name $expected_version (explicit): installed" \
    "$normalized_output"
assert_contains \
    "produced artifact: $AUR_CASE_DEBUG_PACKAGE_NAME $expected_version (not selected; not installed)" \
    "$normalized_output"
assert_not_contains "Running: 'sudo' 'pacman' '-S'" "$normalized_output"
assert_not_contains 'Installing selected repository providers:' "$normalized_output"
assert_not_contains "'--asdeps'" "$normalized_output"
assert_not_contains "'--asexplicit'" "$normalized_output"
assert_not_contains "'--needed'" "$normalized_output"
assert_not_contains '/home/seeke/moguet' "$normalized_output"
printf '%s\n' '  review boundary: PKGBUILD shown; edit declined; no .install prompt'
printf '%s\n' '  makepkg evidence: --packagelist then -sc --noconfirm'
printf '  PackageBase result: %s; required child explicit/installed\n' \
    "$package_base"

printf '%s\n' ':: persistent checkout and source evidence'
current_phase=checkout-evidence
current_output=$normalized_output
cache_root=$XDG_CACHE_HOME/moguet
checkout_path=$cache_root/$package_base
assert_directory_non_symlink "$cache_root" 'production Moguet cache root'
assert_directory_non_symlink "$checkout_path" 'production AUR checkout'
assert_directory_non_symlink "$checkout_path/.git" 'production AUR .git'
assert_regular_non_symlink "$checkout_path/PKGBUILD" 'production PKGBUILD'
assert_regular_non_symlink "$checkout_path/.SRCINFO" 'production .SRCINFO'
checkout_remote=$(git -C "$checkout_path" config --get remote.origin.url)
[ "$checkout_remote" = "$aur_git_url" ] || fail 'production checkout remote drift'
checkout_head=$(git -C "$checkout_path" rev-parse HEAD)
[ "$checkout_head" = "$expected_aur_git_head" ] || fail 'production checkout HEAD drift'
cmp -s "$preflight_pkgbuild" "$checkout_path/PKGBUILD" ||
    fail 'production PKGBUILD differs from reviewed pinned bytes'
cmp -s "$preflight_srcinfo" "$checkout_path/.SRCINFO" ||
    fail 'production .SRCINFO differs from reviewed pinned bytes'
source_archive=$checkout_path/$expected_source_filename
assert_regular_non_symlink "$source_archive" 'downloaded source archive'
source_archive_hash=$(sha256_file "$source_archive") ||
    fail 'downloaded source checksum producer failed'
[ "$source_archive_hash" = \
    "$expected_source_sha256" ] || fail 'downloaded source checksum drift'
if [ ! "$source_archive" -nt "$build_start_marker" ]; then
    fail 'downloaded source archive is not fresh for this invocation'
fi
cache_entries=$case_root/cache-entries.txt
if validation_capture_sorted_output "$cache_entries.raw" "$cache_entries" \
    find "$cache_root" -mindepth 1 -maxdepth 1 -printf '%f\n'; then
    :
else
    cache_status=$?
    fail "cache entry producer failed with status $cache_status"
fi
[ "$(cat "$cache_entries")" = "$package_base" ] ||
    fail 'production cache contains another package or retained workspace'
printf '  checkout path: %s\n' "$checkout_path"
printf '  checkout HEAD: %s\n' "$checkout_head"
printf '  source: %s sha256=%s (fresh)\n' \
    "$expected_source_filename" "$expected_source_sha256"

printf '%s\n' ':: root gateway artifact evidence'
current_phase=artifact-evidence
evidence_directory=$gateway_evidence_root/$gateway_case
assert_directory_non_symlink "$evidence_directory" 'gateway evidence directory'
assert_metadata "$evidence_directory" \
    'root:moguet-validation:750:directory' 'gateway evidence directory'
for evidence_name in \
    trusted-source.json \
    original-argv.nul \
    original-artifact-path.txt \
    staged-artifact-path.txt \
    stage-hashes.txt \
    archive-metadata-check.txt \
    archive-members.tsv \
    PKGINFO \
    package-identity.txt \
    validated-payload.tsv \
    validation-timestamp.txt \
    validation-complete.txt \
    real-pacman-exec.txt
do
    assert_regular_non_symlink "$evidence_directory/$evidence_name" \
        "gateway $evidence_name"
    assert_metadata "$evidence_directory/$evidence_name" \
        'root:moguet-validation:640:regular file' \
        "gateway $evidence_name"
done
original_artifact=$(sed -n '1p' \
    "$evidence_directory/original-artifact-path.txt")
staged_artifact=$(sed -n '1p' \
    "$evidence_directory/staged-artifact-path.txt")
# Production reads archive identity through libalpm.  The live authority is
# the root gateway's exact artifact/transaction evidence, not a private
# metadata-query command in production output.
python3 - \
    "$evidence_directory/original-argv.nul" \
    "$evidence_directory/original-artifact-path.txt" \
    "$evidence_directory/staged-artifact-path.txt" \
    "$evidence_directory/package-identity.txt" \
    "$evidence_directory/validation-complete.txt" \
    "$evidence_directory/real-pacman-exec.txt" \
    "$package_name" "$expected_version" "$expected_architecture" \
    "$runtime_dependencies" "$gateway_staging_root/$gateway_case" <<'PY'
from pathlib import Path
import sys

(
    argv_path,
    original_path,
    staged_path,
    identity_path,
    completion_path,
    execution_path,
    package,
    version,
    architecture,
    dependencies,
    staging_directory,
) = sys.argv[1:]

def exact_lines(path: str, label: str) -> list[str]:
    text = Path(path).read_text(encoding="utf-8")
    if not text.endswith("\n"):
        raise SystemExit(f"gateway {label} evidence lacks a terminal newline")
    return text.splitlines()

original_lines = exact_lines(original_path, "original artifact")
if len(original_lines) != 1:
    raise SystemExit(f"gateway original artifact evidence drift: {original_lines!r}")
original_artifact = original_lines[0]
import re
if not re.fullmatch(r"/run/moguet/source-artifact-installs/active/[0-9a-f]{64}/artifacts/artifact-0\.pkg\.tar\.zst", original_artifact):
    raise SystemExit("gateway trusted root staging identity drift")

staged_lines = exact_lines(staged_path, "staged artifact")
expected_staged_artifact = str(
    Path(staging_directory) / f"{package}-{version}-{architecture}.pkg.tar.zst"
)
if staged_lines != [expected_staged_artifact]:
    raise SystemExit(f"gateway staged artifact evidence drift: {staged_lines!r}")
staged_artifact = staged_lines[0]

values = Path(argv_path).read_bytes().split(b"\0")
if not values or values[-1] != b"":
    raise SystemExit("gateway argv evidence is not NUL terminated")
actual = [value.decode("utf-8", "strict") for value in values[:-1]]
expected_argv = [
    "sudo", "pacman", "-U", "--noconfirm", "--", original_artifact,
]
if actual != expected_argv:
    raise SystemExit(f"gateway argv artifact does not match original artifact evidence: {actual!r}")

expected_identity = [
    f"package_name={package}",
    f"package_version={version}",
    f"package_architecture={architecture}",
    f"package_dependency={dependencies}",
    f"pacman_query={package}\t{version}",
]
identity_lines = exact_lines(identity_path, "package identity")
if identity_lines != expected_identity:
    raise SystemExit(f"gateway package identity evidence drift: {identity_lines!r}")

completion_lines = exact_lines(completion_path, "validation completion")
expected_completion = [
    "validated=true",
    "transaction=exactly-once",
    "install_reason=Explicit",
]
if completion_lines != expected_completion:
    raise SystemExit(f"gateway validation-complete evidence drift: {completion_lines!r}")

execution_lines = exact_lines(execution_path, "real pacman execution")
expected_execution = [f"argv=-U --noconfirm -- {original_artifact}"]
if execution_lines != expected_execution:
    raise SystemExit(f"gateway real-pacman execution evidence drift: {execution_lines!r}")
PY
# The helper owns private /run cleanup. Inspect only our readable cache here;
# an unprivileged test ! -e on /run would mistake EACCES for absence.
remaining_workspaces=$case_root/remaining-artifact-workspaces.txt
validation_capture_output "$remaining_workspaces" /usr/bin/find "$cache_root" \
    -mindepth 1 -maxdepth 1 -name '.artifact-workspace~-*' -print ||
    fail 'artifact workspace inventory failed'
[ ! -s "$remaining_workspaces" ] ||
    fail 'production did not clean the original artifact workspace'
[ "$(awk -F= '$1 == "source_uid" {print $2}' "$evidence_directory/stage-hashes.txt")" = 0 ] ||
    fail 'trusted artifact source uid drift'
[ "$(awk -F= '$1 == "source_mode" {print $2}' "$evidence_directory/stage-hashes.txt")" = 0600 ] ||
    fail 'trusted artifact source mode drift'
assert_regular_non_symlink "$staged_artifact" 'root-staged artifact'
assert_metadata "$staged_artifact" \
    'root:moguet-validation:440:regular file' 'root-staged artifact'
source_hash_before=$(awk -F= '$1 == "source_sha256_before" {print $2}' \
    "$evidence_directory/stage-hashes.txt")
copy_hash=$(awk -F= '$1 == "copy_sha256" {print $2}' \
    "$evidence_directory/stage-hashes.txt")
staged_hash=$(awk -F= '$1 == "staged_sha256" {print $2}' \
    "$evidence_directory/stage-hashes.txt")
source_hash_after=$(awk -F= '$1 == "source_sha256_after" {print $2}' \
    "$evidence_directory/stage-hashes.txt")
[ -n "$source_hash_before" ] || fail 'gateway source hash is absent'
[ "$source_hash_before" = "$copy_hash" ] &&
    [ "$source_hash_before" = "$staged_hash" ] &&
    [ "$source_hash_before" = "$source_hash_after" ] ||
    fail 'gateway source/copy/staged/after hashes differ'
staged_artifact_hash=$(sha256_file "$staged_artifact") ||
    fail 'staged artifact checksum producer failed'
[ "$staged_artifact_hash" = "$staged_hash" ] ||
    fail 'staged artifact changed after gateway validation'
artifact_mtime_ns=$(awk -F= '$1 == "source_mtime_ns" {print $2}' \
    "$evidence_directory/stage-hashes.txt")
python3 - "$build_start_ns" "$artifact_mtime_ns" <<'PY'
import sys

if int(sys.argv[2]) < int(sys.argv[1]):
    raise SystemExit("artifact predates the fresh production invocation")
PY
cmp -s "$runtime_reference_manifest" \
    "$evidence_directory/validated-payload.tsv" ||
    fail 'gateway validated payload differs from the root-owned reference manifest'
if grep -F -- '.INSTALL' "$evidence_directory/archive-members.tsv" >/dev/null; then
    fail 'gateway accepted a package .INSTALL entry'
fi
awk -F '\t' '
    NR == 1 {
        if (NF != 5 || $1 != "# type" || $2 != "permissions" ||
            $3 != "owner" || $4 != "group" || $5 != "path") exit 1
        next
    }
    $1 != "regular" && $1 != "directory" { exit 1 }
    $3 != "root" || $4 != "root" { exit 1 }
' "$evidence_directory/archive-members.tsv" ||
    fail 'gateway accepted an unsafe archive member identity'
assert_contains 'moguet-live-aur-archive-metadata: accepted: entries=' \
    "$evidence_directory/archive-metadata-check.txt"
package_query=$(LC_ALL=C pacman -Qp --color never -- \
    "$staged_artifact") || fail 'real non-root package query of staged artifact failed'
[ "$package_query" = "$package_name $expected_version" ] ||
    fail 'staged artifact identity query drift'
evidence_directories=$case_root/evidence-directories.raw
validation_capture_output "$evidence_directories" \
    find "$gateway_evidence_root" -mindepth 1 -maxdepth 1 -type d ||
    fail "gateway evidence directory producer failed with status $?"
[ "$(wc -l <"$evidence_directories")" -eq 6 ] ||
    fail 'gateway must retain one positive and five independent negative evidence directories'
staging_directories=$case_root/staging-directories.raw
validation_capture_output "$staging_directories" \
    find "$gateway_staging_root" -mindepth 1 -maxdepth 1 -type d ||
    fail "gateway staging directory producer failed with status $?"
[ "$(wc -l <"$staging_directories")" -eq 6 ] ||
    fail 'gateway must retain one positive and five independent negative staging directories'
printf '  artifact trusted input: %s (cleanup owned by root helper)\n' "$original_artifact"
printf '  artifact staged: %s\n' "$staged_artifact"
printf '  artifact identity: %s %s %s\n' \
    "$package_name" "$expected_version" "$expected_architecture"
printf '  artifact sha256: %s\n' "$staged_hash"
printf '%s\n' '  artifact payload:'
sed 's/^/    /' "$payload_policy"
printf '  exact gateway argv: sudo pacman -U --noconfirm -- %s\n' \
    "$original_artifact"

printf '%s\n' ':: installed state and package inventory evidence'
current_phase=installed-evidence
capture_package_inventory "$case_root/after-install"
python3 - \
    "$case_root/before-install.tsv" "$case_root/after-install.tsv" \
    "$package_name" "$expected_version" <<'PY'
from pathlib import Path
import sys

def load(path: str) -> dict[str, tuple[str, str]]:
    result = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        name, version, reason = line.split("\t")
        if name in result:
            raise SystemExit(f"duplicate inventory package: {name}")
        result[name] = (version, reason)
    return result

before = load(sys.argv[1])
after = load(sys.argv[2])
package, version = sys.argv[3:5]
if package in before:
    raise SystemExit("tracked AUR package existed before install")
if set(after) - set(before) != {package}:
    raise SystemExit(f"unexpected added packages: {sorted(set(after) - set(before))!r}")
if set(before) - set(after):
    raise SystemExit(f"unexpected removed packages: {sorted(set(before) - set(after))!r}")
if after.get(package) != (version, "Explicit"):
    raise SystemExit(f"installed package identity/reason drift: {after.get(package)!r}")
for name, identity in before.items():
    if after.get(name) != identity:
        raise SystemExit(
            f"existing package version/reason changed: {name}: "
            f"{identity!r} -> {after.get(name)!r}"
        )
print(f"inventory diff: added exactly {package} {version} Explicit")
PY

pacman -Q "$package_name" > "$case_root/installed-package.txt" ||
    fail 'installed package query failed'
[ "$(cat "$case_root/installed-package.txt")" = \
    "$package_name $expected_version" ] || fail 'installed version drift'
pacman -Qe "$package_name" >/dev/null || fail "$package_name is not Explicit"
if pacman -Qd "$package_name" >/dev/null 2>&1; then
    fail "$package_name was incorrectly installed as Dependency"
fi
assert_regular_non_symlink "/usr/bin/$package_name" 'installed fixture executable'
pacman -Qlq "$package_name" > "$case_root/installed-paths.raw"
python3 - \
    "$case_root/installed-paths.raw" "$runtime_reference_manifest" <<'PY'
import hashlib
from pathlib import Path
import os
import stat
import sys

installed_paths = set()
for raw_path in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    if not raw_path.startswith("/"):
        raise SystemExit(f"installed path is not absolute: {raw_path!r}")
    installed_paths.add(raw_path.removeprefix("/"))
if len(installed_paths) != len(Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()):
    raise SystemExit("installed payload contains duplicates")
manifest_rows = [line.split("\t") for line in Path(sys.argv[2]).read_text(encoding="utf-8").splitlines()]
if not manifest_rows or manifest_rows[0] != ["# path", "type", "mode", "owner", "group", "sha256"]:
    raise SystemExit("root reference manifest header drift")
manifest = {row[0]: row[1:] for row in manifest_rows[1:]}
if set(manifest) != installed_paths:
    raise SystemExit(f"installed payload path set drift: {sorted(installed_paths)!r}")
for archive_path, (entry_type, mode, owner, group, content_hash) in manifest.items():
    if owner != "root" or group != "root":
        raise SystemExit(f"root manifest owner/group drift: {archive_path}")
    installed = Path("/") / archive_path
    metadata = os.lstat(installed)
    if stat.S_ISLNK(metadata.st_mode):
        raise SystemExit(f"installed payload is a symlink: {archive_path}")
    if stat.S_IMODE(metadata.st_mode) != int(mode, 8):
        raise SystemExit(f"installed payload mode drift: {archive_path}")
    if entry_type == "directory":
        if not stat.S_ISDIR(metadata.st_mode) or content_hash != "-":
            raise SystemExit(f"installed directory identity drift: {archive_path}")
    elif entry_type == "regular":
        if not stat.S_ISREG(metadata.st_mode):
            raise SystemExit(f"installed payload is not regular: {archive_path}")
        if hashlib.sha256(installed.read_bytes()).hexdigest() != content_hash:
            raise SystemExit(f"installed payload content hash drift: {archive_path}")
    else:
        raise SystemExit(f"unexpected manifest type: {entry_type}")
PY
if pacman -Q "$AUR_CASE_DEBUG_PACKAGE_NAME" >/dev/null 2>&1; then
    fail 'unselected debug artifact was unexpectedly installed'
fi

: > "$case_root/dependencies-after.tsv"
saved_ifs=$IFS
IFS=,
all_dependencies=$runtime_dependencies,$AUR_CASE_MAKE_DEPENDENCIES
for dependency_name in $all_dependencies; do
    inventory_record "$dependency_name" "$case_root/after-install.tsv" \
        >> "$case_root/dependencies-after.tsv" ||
        fail "dependency disappeared after install: $dependency_name"
done
IFS=$saved_ifs
cmp -s "$case_root/dependencies-before.tsv" \
    "$case_root/dependencies-after.tsv" ||
    fail 'dependency version or install reason changed'

printf '  installed: %s %s reason=Explicit\n' \
    "$package_name" "$expected_version"
while IFS=$tab read -r dependency_name dependency_version dependency_reason; do
    printf '  dependency before/after: %s %s reason=%s (unchanged)\n' \
        "$dependency_name" "$dependency_version" "$dependency_reason"
done < "$case_root/dependencies-after.tsv"
printf '  package inventory diff: +%s %s Explicit; no other change\n' \
    "$package_name" "$expected_version"

python_cache_probe=$case_root/python-cache-paths.raw
if validation_capture_output "$python_cache_probe" \
    find "$repo_root" "$case_root" -type d -name __pycache__ -print; then
    :
else
    cache_status=$?
    fail "Python cache path producer failed with status $cache_status"
fi
if [ -s "$python_cache_probe" ]; then
    fail 'Python cache artifact was created'
fi
python_bytecode_probe=$case_root/python-bytecode-paths.raw
if validation_capture_output "$python_bytecode_probe" \
    find "$repo_root" "$case_root" -type f \
    \( -name '*.pyc' -o -name '*.pyo' \) -print; then
    :
else
    cache_status=$?
    fail "Python bytecode path producer failed with status $cache_status"
fi
if [ -s "$python_bytecode_probe" ]; then
    fail 'Python bytecode artifact was created'
fi

current_phase=complete
current_output=
printf '%s\n' ':: expected completion phase: live AUR install validated'
printf '%s\n' 'arch-live-aur: all checks passed'
