#!/bin/sh

set -eu

PATH=/usr/bin
export PATH
unset PYTHONPATH
unset PYTHONHOME
export LC_ALL=C

real_pacman=/usr/libexec/moguet-live-local/pacman.real
stage_helper=/usr/libexec/moguet-live-local/local-stage-artifact.py
archive_validator=/usr/libexec/moguet-live-local/local-archive-validator.sh
status_library=/usr/libexec/moguet-live-local/validation-status.sh
fixture_root=/usr/libexec/moguet-live-local/fixtures/local-package
fixture_contract=$fixture_root/contract.env
staging_root=/var/lib/moguet-live-local/staging
evidence_root=/var/log/moguet-live-local
validation_user=moguet-validation
validation_uid=1000
validation_gid=1000
gateway_reject_status=97

reject() {
    printf 'moguet-live-local-gateway: rejected: %s\n' "$*" >&2
    exit "$gateway_reject_status"
}

exec_real_pacman() {
    exec /usr/bin/env -i PATH=/usr/bin LC_ALL=C "$real_pacman" "$@"
}

require_root_readonly_file() {
    checked_path=$1
    checked_label=$2
    expected_mode=$3
    [ -f "$checked_path" ] && [ ! -L "$checked_path" ] ||
        reject "$checked_label is not a regular non-symlink"
    metadata=$(/usr/bin/stat -c '%u:%g:%a:%F' -- "$checked_path")
    [ "$metadata" = "0:0:$expected_mode:regular file" ] ||
        reject "$checked_label has unsafe metadata"
}

kernel_effective_uid=$(/usr/bin/awk '$1 == "Uid:" { print $3 }' "/proc/$$/status")
case "$kernel_effective_uid" in
    ''|*[!0-9]*) reject 'kernel effective UID is unavailable' ;;
esac
if [ "$kernel_effective_uid" -ne 0 ]; then
    exec_real_pacman "$@"
fi

case_identity=${MOGUET_LIVE_LOCAL_CASE-}
[ "$case_identity" = local-root-install ] ||
    reject 'missing or unknown MOGUET_LIVE_LOCAL_CASE'

require_root_readonly_file "$real_pacman" 'real pacman' 755
require_root_readonly_file "$stage_helper" 'staging helper' 755
require_root_readonly_file "$archive_validator" 'archive validator' 755
require_root_readonly_file "$status_library" 'archive status library' 755
require_root_readonly_file "$fixture_contract" 'local fixture contract' 444
# shellcheck source=fixtures/local-package/contract.env
. "$fixture_contract"
fixture_artifact=${PACKAGE_NAME}-${PACKAGE_VERSION}-${PACKAGE_ARCHITECTURE}.pkg.tar.zst

# The provider is chosen by production Moguet from the current real sync DB;
# this gateway permits only the reviewed providers from the fixture contract
# and no other system transaction shape.
if [ "$#" -eq 5 ] && [ "$1" = -S ] && [ "$2" = --asdeps ] && \
   [ "$3" = --needed ] && [ "$4" = -- ]; then
    case "$5" in
        "$EXPECTED_PROVIDER_REPOSITORY"/*)
            provider_package=${5#*/}
            case ",$EXPECTED_PROVIDER_PACKAGES," in
                *,"$provider_package",*)
                    # Moguet's terminal reader can prefetch scripted PTY input
                    # before this child inherits the terminal. The gateway
                    # auto-confirms only this reviewed transaction shape.
                    exec_real_pacman --noconfirm "$@"
                    ;;
            esac
            ;;
    esac
fi

if [ "$#" -ne 3 ] || [ "$1" != -U ] || [ "$2" != -- ]; then
    reject 'root argv must be one selected provider transaction or local artifact install'
fi

source_artifact=$3
/usr/bin/python3 -I "$stage_helper" check-trusted "$source_artifact" ||
    reject 'positive artifact is not canonical trusted root staging'

evidence_directory=$evidence_root/$case_identity
staging_directory=$staging_root/$case_identity
/usr/bin/mkdir -m 0750 -- "$evidence_directory" ||
    reject 'case transaction already exists; refusing a second root install'
/usr/bin/chown root:"$validation_user" "$evidence_directory"
/usr/bin/chmod 0750 "$evidence_directory"
/usr/bin/mkdir -m 0750 -- "$staging_directory" ||
    reject 'case staging already exists; refusing a second root install'
/usr/bin/chown root:"$validation_user" "$staging_directory"
/usr/bin/chmod 0750 "$staging_directory"
staged_artifact=$staging_directory/$fixture_artifact

/usr/bin/python3 -I "$stage_helper" \
    "$source_artifact" "$staged_artifact" "$evidence_directory" \
    "$validation_uid:$validation_gid" ||
    reject 'safe artifact staging failed'

if "$archive_validator" "$staged_artifact" "$evidence_directory"; then
    archive_status=0
else
    archive_status=$?
fi
if [ "$archive_status" -ne 0 ]; then
    if [ "$archive_status" -eq "$gateway_reject_status" ]; then
        exit "$gateway_reject_status"
    fi
    reject "archive validator failed with infrastructure status $archive_status"
fi

pkginfo_file=$evidence_directory/PKGINFO
pkginfo_raw=$evidence_directory/PKGINFO.raw
member_list=$evidence_directory/archive-members.txt
member_list_raw=$evidence_directory/archive-members.raw
expected_members=$evidence_directory/expected-members.txt
expected_members_raw=$evidence_directory/expected-members.raw
marker_raw=$evidence_directory/marker.raw
marker_file=$evidence_directory/marker.txt
expected_marker=$evidence_directory/expected-marker.txt
for validated_evidence in \
    "$pkginfo_file" "$pkginfo_raw" \
    "$member_list" "$member_list_raw" \
    "$expected_members" "$expected_members_raw" \
    "$marker_raw" "$marker_file" "$expected_marker"
do
    [ -f "$validated_evidence" ] && [ ! -L "$validated_evidence" ] ||
        reject "archive validator omitted evidence: $validated_evidence"
    /usr/bin/chown root:"$validation_user" "$validated_evidence"
    /usr/bin/chmod 0640 "$validated_evidence"
done
/usr/bin/python3 -I "$stage_helper" verify-trusted \
    "$source_artifact" "$staged_artifact" "$evidence_directory/trusted-source.json" ||
    reject 'trusted input changed before real pacman'
printf '%s\0' sudo pacman "$@" > "$evidence_directory/accepted.argv"
/usr/bin/chown root:"$validation_user" "$evidence_directory/accepted.argv"
/usr/bin/chmod 0640 "$evidence_directory/accepted.argv"

exec_real_pacman --noconfirm -U --asexplicit -- "$source_artifact"
