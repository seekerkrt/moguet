#!/bin/sh

set -eu

PATH=/usr/bin
export PATH
unset PYTHONPATH
unset PYTHONHOME
export LC_ALL=C

real_pacman=/usr/libexec/moguet-live-aur/pacman.real
stage_helper=/usr/libexec/moguet-live-aur/aur-stage-artifact.py
metadata_helper=/usr/libexec/moguet-live-aur/aur-archive-metadata-check
case_loader=/usr/libexec/moguet-live-aur/load-case.sh
policy_root=/usr/share/moguet-live-aur/policy
case_policy=$policy_root/aur-cases.tsv
payload_static_policy=$policy_root/payload-authority.tsv
reference_manifest=/usr/libexec/moguet-live-aur/fixtures/reference-payload.tsv
pkginfo_manifest=/usr/libexec/moguet-live-aur/fixtures/reference-pkginfo.tsv
evidence_root=/var/log/moguet-live-aur
staging_root=/var/lib/moguet-live-aur/staging
validation_user=moguet-validation
validation_uid=1000
validation_gid=1000
gateway_reject_status=97

reject() {
    printf 'moguet-live-aur-gateway: rejected: %s\n' "$*" >&2
    exit "$gateway_reject_status"
}

exec_real_pacman() {
    exec /usr/bin/env -i PATH=/usr/bin LC_ALL=C \
        "$real_pacman" "$@"
}

kernel_effective_uid=$(/usr/bin/awk '$1 == "Uid:" { print $3 }' "/proc/$$/status")
case "$kernel_effective_uid" in
    ''|*[!0-9]*)
        reject 'kernel effective UID is unavailable'
        ;;
esac
if [ "$kernel_effective_uid" -ne 0 ]; then
    exec_real_pacman "$@"
fi

case_identity=${MOGUET_LIVE_AUR_CASE-}
case "$case_identity" in
    aur-install)
        negative_case=false
        ;;
    aur-content-drift-test|aur-conflict-policy-test|\
    aur-xattr-metadata-test|aur-acl-metadata-test|\
    aur-pkgdesc-authority-test)
        negative_case=true
        ;;
    *)
        reject 'missing or unknown MOGUET_LIVE_AUR_CASE'
        ;;
esac

require_runtime_authority() {
    authority_path=$1
    expected_metadata=$2
    authority_label=$3
    if [ ! -f "$authority_path" ] || [ -L "$authority_path" ]; then
        reject "runtime authority is not a regular non-symlink: $authority_label"
    fi
    actual_metadata=$(/usr/bin/stat -c '%u:%g:%a:%F' -- "$authority_path")
    if [ "$actual_metadata" != "$expected_metadata" ]; then
        reject "runtime authority metadata drift: $authority_label"
    fi
}

require_runtime_authority "$real_pacman" '0:0:755:regular file' 'real pacman'
require_runtime_authority "$stage_helper" '0:0:555:regular file' 'staging helper'
require_runtime_authority "$metadata_helper" \
    '0:0:555:regular file' 'archive metadata helper'
require_runtime_authority "$case_loader" \
    '0:0:555:regular file' 'AUR case loader'
require_runtime_authority "$case_policy" '0:0:444:regular file' 'AUR case policy'
require_runtime_authority "$payload_static_policy" \
    '0:0:444:regular file' 'static payload authority'
require_runtime_authority "$reference_manifest" \
    '0:0:444:regular file' 'reference payload manifest'
require_runtime_authority "$pkginfo_manifest" \
    '0:0:444:regular file' 'reference PKGINFO manifest'

# shellcheck source=fixtures/aur/load-case.sh
. "$case_loader"
validation_load_aur_case "$case_policy" || reject 'AUR case policy is invalid'
package_name=$AUR_CASE_PACKAGE_NAME
package_base=$AUR_CASE_PACKAGE_BASE
expected_version=$AUR_CASE_EXPECTED_VERSION
runtime_dependencies=$AUR_CASE_RUNTIME_DEPENDENCIES
make_dependencies=$AUR_CASE_MAKE_DEPENDENCIES
source_kind=$AUR_CASE_SOURCE_KIND
install_reason=$AUR_CASE_INSTALL_REASON
expected_architecture=$AUR_CASE_EXPECTED_ARCHITECTURE

if [ "$#" -ne 4 ] ||
    [ "$1" != -U ] ||
    [ "$2" != --noconfirm ] ||
    [ "$3" != -- ]
then
    reject 'root argv must be exactly: -U --noconfirm -- <one artifact>'
fi

source_artifact=$4
expected_artifact_filename="${package_name}-${expected_version}-${expected_architecture}.pkg.tar.zst"
if [ "$negative_case" = true ]; then
case "$source_artifact" in
    /home/moguet-validation/.cache/moguet/.artifact-workspace~-*/*)
        ;;
    *)
        reject 'artifact path is outside the invocation-owned cache prefix'
        ;;
esac

case "$source_artifact" in
    /*)
        ;;
    *)
        reject 'artifact path must be absolute'
        ;;
esac
if [ ! -f "$source_artifact" ] || [ -L "$source_artifact" ]; then
    reject 'artifact path must be a regular non-symlink'
fi
canonical_artifact=$(/usr/bin/realpath -e -- "$source_artifact") ||
    reject 'artifact path has no canonical realpath'
[ "$canonical_artifact" = "$source_artifact" ] ||
    reject 'raw artifact path differs from its canonical realpath'
artifact_workspace=$(/usr/bin/dirname -- "$source_artifact")
[ "$(/usr/bin/dirname -- "$artifact_workspace")" = \
    /home/moguet-validation/.cache/moguet ] ||
    reject 'artifact workspace is not a direct cache child'
case "$(/usr/bin/basename -- "$artifact_workspace")" in
    .artifact-workspace~-??????)
        ;;
    *)
        reject 'artifact workspace identity is invalid'
        ;;
esac

expected_artifact_filename="${package_name}-${expected_version}-${expected_architecture}.pkg.tar.zst"
[ "$(/usr/bin/basename -- "$source_artifact")" = "$expected_artifact_filename" ] ||
    reject 'artifact filename identity drift'
[ "$(/usr/bin/stat -c '%u' -- "$source_artifact")" -eq "$validation_uid" ] ||
    reject 'source artifact is not validation-user-owned'
source_mode=$(/usr/bin/stat -c '%a' -- "$source_artifact")
[ $((0$source_mode & 022)) -eq 0 ] ||
    reject 'source artifact is group/other writable'
[ "$(/usr/bin/stat -c '%h' -- "$source_artifact")" -eq 1 ] ||
    reject 'source artifact has an unexpected hard-link count'

else
    /usr/bin/python3 -I "$stage_helper" check-trusted "$source_artifact" ||
        reject 'positive artifact is not canonical trusted root staging'
    canonical_artifact=$source_artifact
fi

evidence_directory=$evidence_root/$case_identity
if ! /usr/bin/mkdir -m 0750 -- "$evidence_directory"; then
    reject 'case transaction already exists; refusing a second root transaction'
fi
/usr/bin/chown root:"$validation_user" "$evidence_directory"
/usr/bin/chmod 0750 "$evidence_directory"

write_evidence_line() {
    evidence_file=$1
    shift
    : > "$evidence_file"
    /usr/bin/chown root:"$validation_user" "$evidence_file"
    /usr/bin/chmod 0640 "$evidence_file"
    printf '%s\n' "$@" >> "$evidence_file"
}

argv_evidence=$evidence_directory/original-argv.nul
: > "$argv_evidence"
/usr/bin/chown root:"$validation_user" "$argv_evidence"
/usr/bin/chmod 0640 "$argv_evidence"
printf '%s\0' sudo pacman "$@" >> "$argv_evidence"
write_evidence_line \
    "$evidence_directory/original-artifact-path.txt" \
    "$canonical_artifact"

staging_directory=$staging_root/$case_identity
/usr/bin/mkdir -m 0750 -- "$staging_directory"
/usr/bin/chown root:"$validation_user" "$staging_directory"
/usr/bin/chmod 0750 "$staging_directory"
staged_artifact=$staging_directory/$expected_artifact_filename
write_evidence_line \
    "$evidence_directory/staged-artifact-path.txt" \
    "$staged_artifact"

stage_evidence=$evidence_directory/stage-hashes.txt
: > "$stage_evidence"
/usr/bin/chown root:"$validation_user" "$stage_evidence"
/usr/bin/chmod 0640 "$stage_evidence"
stage_input() {
    if [ "$negative_case" = true ]; then
        /usr/bin/python3 -I "$stage_helper" stage \
            "$source_artifact" "$staged_artifact" "$validation_uid" "$validation_gid"
    else
        /usr/bin/python3 -I "$stage_helper" stage-trusted \
            "$source_artifact" "$staged_artifact" "$evidence_directory"
    fi
}
if ! stage_input >> "$stage_evidence"
then
    reject 'root staging copy or TOCTOU validation failed'
fi

metadata_evidence=$evidence_directory/archive-metadata-check.txt
if ! "$metadata_helper" "$staged_artifact" > "$metadata_evidence" 2>&1; then
    /usr/bin/chown root:"$validation_user" "$metadata_evidence"
    /usr/bin/chmod 0640 "$metadata_evidence"
    /usr/bin/cat "$metadata_evidence" >&2
    reject 'staged artifact direct metadata validation failed'
fi
/usr/bin/chown root:"$validation_user" "$metadata_evidence"
/usr/bin/chmod 0640 "$metadata_evidence"

if ! /usr/bin/python3 -I "$stage_helper" validate \
    "$payload_static_policy" "$reference_manifest" "$pkginfo_manifest" \
    "$staged_artifact" \
    "$evidence_directory" "$validation_gid" \
    "$package_name" "$package_base" "$expected_version" \
    "$runtime_dependencies" "$make_dependencies" "$expected_architecture"
then
    reject 'staged artifact path, content, or PKGINFO validation failed'
fi

if [ "$negative_case" = true ]; then
    reject 'negative test case must never invoke real pacman'
fi

/usr/bin/python3 -I "$stage_helper" verify-trusted \
    "$source_artifact" "$staged_artifact" "$evidence_directory/trusted-source.json" ||
    reject 'trusted input changed before real pacman'

pacman_identity_format=$(printf '%%n\t%%v')
pacman_identity=$(/usr/bin/env -i PATH=/usr/bin LC_ALL=C \
    "$real_pacman" -U --print --print-format "$pacman_identity_format" -- \
    "$source_artifact") || reject 'real pacman package identity query failed'
expected_pacman_identity=$(printf '%s\t%s' \
    "$package_name" "$expected_version")
[ "$pacman_identity" = "$expected_pacman_identity" ] ||
    reject 'real pacman package identity query drift'

write_evidence_line "$evidence_directory/package-identity.txt" \
    "package_name=$package_name" \
    "package_version=$expected_version" \
    "package_architecture=$expected_architecture" \
    "package_dependency=$runtime_dependencies" \
    "pacman_query=$pacman_identity"
write_evidence_line "$evidence_directory/validation-timestamp.txt" \
    "utc=$(/usr/bin/date -u '+%Y-%m-%dT%H:%M:%SZ')" \
    "epoch=$(/usr/bin/date '+%s')"
write_evidence_line "$evidence_directory/validation-complete.txt" \
    'validated=true' \
    'transaction=exactly-once' \
    'install_reason=Explicit'
write_evidence_line "$evidence_directory/real-pacman-exec.txt" \
    "argv=-U --noconfirm -- $source_artifact"

# Every retained diagnostic is immutable to the validation user and readable
# only through the evidence group. The directory is one-shot, so this cannot
# overwrite evidence from another transaction.
for retained_evidence in "$evidence_directory"/*; do
    if [ -f "$retained_evidence" ] && [ ! -L "$retained_evidence" ]; then
        /usr/bin/chown root:"$validation_user" "$retained_evidence"
        /usr/bin/chmod 0640 "$retained_evidence"
    fi
done

exec_real_pacman -U --noconfirm -- "$source_artifact"
