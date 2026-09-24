#!/bin/sh

set -eu
set -C

readonly_sentinel_status=86
log_root=/var/log/moguet-live-validation
contract_file=/usr/libexec/moguet-live-validation/contract.env

reject() {
    printf '%s\n' "moguet-live-pacman-sentinel: rejected argv ($1)" >&2
    exit "$readonly_sentinel_status"
}

if [ "$(/usr/bin/id -u)" -ne 0 ]; then
    reject 'effective user is not root'
fi

contract_metadata=$(/usr/bin/stat -c '%U:%G:%a:%F' "$contract_file")
if [ "$contract_metadata" != 'root:root:444:regular file' ]; then
    reject 'fixture contract ownership or mode changed'
fi
# shellcheck source=fixtures/local-package/contract.env
. "$contract_file"
case "$EXPECTED_PROVIDER_PACKAGES" in
    *,*,*|,*|*,|'')
        reject 'fixture contract must name exactly two reviewed providers'
        ;;
esac
first_provider=${EXPECTED_PROVIDER_PACKAGES%%,*}
second_provider=${EXPECTED_PROVIDER_PACKAGES#*,}
first_provider_target=$EXPECTED_PROVIDER_REPOSITORY/$first_provider
second_provider_target=$EXPECTED_PROVIDER_REPOSITORY/$second_provider

case_name=${MOGUET_LIVE_SENTINEL_CASE-}
case "$case_name" in
    sentinel-accept-first-provider|sentinel-accept-second-provider-noconfirm|\
    sentinel-reject-pacman-u|sentinel-reject-remove|sentinel-reject-syu|\
    sentinel-reject-multiple|sentinel-reject-unqualified|\
    sentinel-reject-option|sentinel-reject-unknown-target|\
    provider-discovery|first-provider-selection|second-provider-selection|\
    multiple-range|multiple-comma|multiple-space|exclude-only|include-exclude|invalid-retry|\
    cancel-empty|cancel-q|provider-eof|non-tty-pipe|noconfirm-tty)
        ;;
    *)
        reject 'missing or unknown case identity'
        ;;
esac

case_directory=$log_root/$case_name
if [ ! -d "$case_directory" ] || [ -L "$case_directory" ]; then
    reject 'case log directory is missing or unsafe'
fi

directory_metadata=$(/usr/bin/stat -c '%U:%G:%a:%F' "$case_directory")
if [ "$directory_metadata" != 'root:moguet-validation:750:directory' ]; then
    reject 'case log directory ownership or mode changed'
fi

log_file=$case_directory/sentinel.argv
umask 027
if ! : > "$log_file"; then
    reject 'case sentinel log already exists or cannot be created'
fi

{
    printf '%s\0' sudo pacman
    for argument do
        printf '%s\0' "$argument"
    done
} >> "$log_file"
/usr/bin/chown root:moguet-validation "$log_file"
/usr/bin/chmod 0640 "$log_file"

selected_second_target=
if [ "$#" -eq 5 ]; then
    [ "$1" = '-S' ] || reject 'operation is not -S'
    [ "$2" = '--asdeps' ] || reject '--asdeps is missing or reordered'
    [ "$3" = '--needed' ] || reject '--needed is missing or reordered'
    [ "$4" = '--' ] || reject 'target delimiter is missing or reordered'
    selected_target=$5
elif [ "$#" -eq 6 ]; then
    [ "$1" = '-S' ] || reject 'operation is not -S'
    [ "$2" = '--asdeps' ] || reject '--asdeps is missing or reordered'
    [ "$3" = '--needed' ] || reject '--needed is missing or reordered'
    if [ "$4" = '--noconfirm' ]; then
        [ "$5" = '--' ] || reject 'target delimiter is missing or reordered'
        selected_target=$6
    else
        [ "$4" = '--' ] || reject 'target delimiter is missing or reordered'
        case "$case_name" in
            multiple-range|multiple-comma|multiple-space) ;;
            *) reject 'multiple targets are not allowed for this case' ;;
        esac
        selected_target=$5
        selected_second_target=$6
    fi
else
    reject 'unexpected target count or option set'
fi

if [ "$selected_target" != "$first_provider_target" ] &&
    [ "$selected_target" != "$second_provider_target" ]; then
    reject 'target is not an allowed repo-qualified provider'
fi
if [ -n "$selected_second_target" ]; then
    if [ "$selected_second_target" = "$selected_target" ] ||
        { [ "$selected_second_target" != "$first_provider_target" ] &&
          [ "$selected_second_target" != "$second_provider_target" ]; }; then
        reject 'multiple targets are not the two distinct reviewed providers'
    fi
    selected_target="$selected_target $selected_second_target"
fi

printf '%s\n' \
    "moguet-live-pacman-sentinel: accepted and blocked sudo pacman argv for $selected_target" >&2
exit "$readonly_sentinel_status"
