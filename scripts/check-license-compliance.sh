#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH='' cd "$script_dir/.." && pwd)
. "$script_dir/validation-status.sh"
current_package_contract=$repo_root/tests/fixtures/current-package/contract.env

fail() {
    printf 'license-check: %s\n' "$*" >&2
    exit 1
}

pass() {
    printf 'license-check: ok: %s\n' "$*"
}

require_regular_file() {
    file=$1
    [ -f "$file" ] || fail "$file is missing or is not a regular file."
    [ ! -L "$file" ] || fail "$file must not be a symbolic link."
    [ -s "$file" ] || fail "$file is empty."
}

require_text() {
    file=$1
    expected=$2
    grep -F -- "$expected" "$file" >/dev/null ||
        fail "$file is missing required text: $expected"
}

reject_pattern() {
    file=$1
    rejected=$2
    if grep -E -- "$rejected" "$file" >/dev/null; then
        fail "$file contains version-dependent current-series wording: $rejected"
    fi
}

reject_text() {
    file=$1
    rejected=$2
    if grep -F -- "$rejected" "$file" >/dev/null; then
        fail "$file contains obsolete text: $rejected"
    fi
}

require_value_text() {
    label=$1
    value=$2
    expected=$3
    case $value in
        *"$expected"*) ;;
        *) fail "$label is missing required text: $expected" ;;
    esac
}

check_sha256() {
    file=$1
    expected=$2
    if checksum_output=$(sha256sum "$file"); then
        actual=${checksum_output%% *}
    else
        checksum_status=$?
        fail "$file checksum producer failed with status $checksum_status"
    fi
    [ "$actual" = "$expected" ] ||
        fail "$file checksum mismatch: expected $expected, got $actual"
    pass "$file matches audited SHA-256"
}

check_pkgbuild_metadata() {
    case_version=$1

    printf '%s\n' "$case_version" > "$pkgbuild_test_dir/VERSION"
    package_metadata_file=$pkgbuild_test_dir/.SRCINFO
    if validation_capture_output "$package_metadata_file" \
        evaluate_pkgbuild_metadata; then
        :
    else
        metadata_status=$?
        fail "PKGBUILD evaluation failed for $PROJECT_NAME $case_version (status $metadata_status)."
    fi

    evaluated_pkgbase=$(sed -n 's/^pkgbase = //p' "$package_metadata_file")
    [ "$evaluated_pkgbase" = "$PACKAGE_BASE" ] ||
        fail "PKGBUILD evaluated pkgbase=$evaluated_pkgbase; expected $PACKAGE_BASE."

    evaluated_pkgname=$(sed -n 's/^pkgname = //p' "$package_metadata_file")
    [ "$evaluated_pkgname" = "$PACKAGE_NAME" ] ||
        fail "PKGBUILD evaluated pkgname=$evaluated_pkgname; expected $PACKAGE_NAME."

    evaluated_version=$(sed -n \
        's/^[[:space:]]*pkgver = //p' "$package_metadata_file")
    [ "$evaluated_version" = "$case_version" ] ||
        fail "PKGBUILD evaluated pkgver=$evaluated_version; expected $case_version."

    evaluated_license=$(sed -n \
        's/^[[:space:]]*license = //p' "$package_metadata_file")
    [ "$evaluated_license" = "$PACKAGE_LICENSE" ] ||
        fail "PKGBUILD evaluated license=$evaluated_license; expected $PACKAGE_LICENSE."

    evaluated_url=$(sed -n \
        's/^[[:space:]]*url = //p' "$package_metadata_file")
    expected_url=$PROJECT_REPOSITORY_URL
    [ "$evaluated_url" = "$expected_url" ] ||
        fail "PKGBUILD URL mismatch: expected $expected_url, got $evaluated_url."

    evaluated_source=$(sed -n \
        's/^[[:space:]]*source = //p' "$package_metadata_file")
    expected_source="$PACKAGE_SOURCE_NAME::git+$PROJECT_REPOSITORY_URL.git#tag=v$case_version"
    [ "$evaluated_source" = "$expected_source" ] ||
        fail "PKGBUILD source mismatch: expected $expected_source, got $evaluated_source."

    pass "PKGBUILD describes $PROJECT_NAME $case_version under $PACKAGE_LICENSE"
}

evaluate_pkgbuild_metadata() {
    (
        cd "$pkgbuild_test_dir" || exit $?
        makepkg --printsrcinfo || exit $?
    )
}

require_regular_file "$current_package_contract"
# shellcheck source=../tests/fixtures/current-package/contract.env
. "$current_package_contract"

cd "$repo_root"

command -v sha256sum >/dev/null 2>&1 ||
    fail "sha256sum is required for offline canonical-text verification."
command -v makepkg >/dev/null 2>&1 ||
    fail "makepkg is required for PKGBUILD metadata verification."

for file in \
    LICENSE \
    LICENSES/jpacker-MIT-legacy.txt \
    LICENSES/curl.txt \
    LICENSES/nlohmann-json-MIT.txt \
    LICENSES/tomlplusplus-MIT.txt \
    LICENSES/bjoern-hoehrmann-utf8-MIT.txt \
    THIRD_PARTY_NOTICES.md \
    docs/LICENSING.md
do
    require_regular_file "$file"
done
pass "required license and notice files are present and non-empty"

# POLICY: Pin the exact SPDX/Arch canonical bytes so release-check never needs network access.
check_sha256 LICENSE \
    fb981668c18a279e285fc4d83fba1e836cc84dd4daa73c9697d3cfd2d8aca6e0

installed_gpl=/usr/share/licenses/spdx/$PACKAGE_LICENSE.txt
if [ -f "$installed_gpl" ]; then
    cmp -s LICENSE "$installed_gpl" ||
        fail "LICENSE differs from the installed SPDX GPL-3.0-or-later canonical copy."
    pass "LICENSE matches the installed SPDX canonical copy"
else
    pass "installed SPDX copy unavailable; pinned checksum remains authoritative"
fi

check_sha256 LICENSES/jpacker-MIT-legacy.txt \
    2875473bd59e4ff421e2584c37cbff758702a7f291e719ce3ccd5ccc1f825261
check_sha256 LICENSES/curl.txt \
    82f2f4427d6545ee5aaac4f0b80428da6cc8ba41c2cf5da3a03680ec327b9681
check_sha256 LICENSES/nlohmann-json-MIT.txt \
    46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603
check_sha256 LICENSES/tomlplusplus-MIT.txt \
    529bc3900a9571e49db285b0df432397e70b881cc3bf48de6667ae74ff4b06d8
check_sha256 LICENSES/bjoern-hoehrmann-utf8-MIT.txt \
    065815f7f977a41b56bae26957d355a318a6962efcff0e789824d24b29274a35

for installed_notice in \
    /usr/share/licenses/curl/COPYING \
    /usr/share/licenses/nlohmann-json/LICENSE.MIT \
    /usr/share/licenses/tomlplusplus/LICENSE
do
    if [ -f "$installed_notice" ]; then
        case "$installed_notice" in
            */curl/COPYING)
                repository_notice=LICENSES/curl.txt
                ;;
            */nlohmann-json/LICENSE.MIT)
                repository_notice=LICENSES/nlohmann-json-MIT.txt
                ;;
            */tomlplusplus/LICENSE)
                repository_notice=LICENSES/tomlplusplus-MIT.txt
                ;;
        esac
        cmp -s "$repository_notice" "$installed_notice" ||
            fail "$repository_notice differs from installed $installed_notice; re-audit the dependency notice."
        pass "$repository_notice matches the installed package notice"
    fi
done

require_text docs/LICENSING.md \
    "jpacker v1.15.0 and later releases, and Moguet releases, are distributed under GPL-3.0-or-later."
require_text docs/LICENSING.md \
    "jpacker v1.14.0 and earlier releases were distributed under the MIT License."
require_text docs/LICENSING.md \
    "Those historical releases remain available under their original license."
require_text README.md 'Moguet releases and jpacker v1.15.0 through v1.16.0 are distributed under `GPL-3.0-or-later`.'
require_text README.md "jpacker v1.14.0 and earlier releases"
require_text README.ja.md 'Moguet releaseとjpacker v1.15.0からv1.16.0は、`GPL-3.0-or-later`で提供します。'
require_text README.ja.md "jpacker v1.14.0以前のreleaseはMIT License"
for readme in README.md README.ja.md
do
    require_text "$readme" \
        "[LICENSE](https://github.com/seekerkrt/moguet/blob/develop/LICENSE)"
    require_text "$readme" "[docs/LICENSING.md](docs/LICENSING.md)"
    require_text "$readme" "[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)"
done
require_text THIRD_PARTY_NOTICES.md \
    "the current GPL-licensed Moguet development series"
require_text docs/decisions.md \
    'Moguet releaseとjpacker v1.15.0以降は`GPL-3.0-or-later`で提供する。'
require_text docs/decisions.md \
    'Moguet releases and jpacker v1.15.0 or later are distributed under `GPL-3.0-or-later`.'

for current_series_file in \
    README.md \
    README.ja.md \
    THIRD_PARTY_NOTICES.md \
    docs/decisions.md \
    docs/LICENSING.md
do
    reject_pattern "$current_series_file" \
        'current( (jpacker|Moguet))? v[0-9]+\.[0-9]+\.[0-9]+ development series'
    reject_pattern "$current_series_file" \
        '現在のv[0-9]+\.[0-9]+\.[0-9]+開発系列'
done
pass "project notice and README preserve the version boundary"

for heading in \
    "## Linked or compiled components" \
    "## External programs invoked" \
    "## System/toolchain runtime" \
    "## Distribution notes" \
    "### libalpm" \
    "### libcurl" \
    "### nlohmann-json" \
    "### toml++"
do
    require_text THIRD_PARTY_NOTICES.md "$heading"
done
require_text THIRD_PARTY_NOTICES.md '`GPL-2.0-or-later`'
require_text THIRD_PARTY_NOTICES.md '`curl` (SPDX identifier)'
require_text THIRD_PARTY_NOTICES.md \
    '[`LICENSES/curl.txt`](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/curl.txt)'
require_text THIRD_PARTY_NOTICES.md \
    '[`LICENSES/nlohmann-json-MIT.txt`](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/nlohmann-json-MIT.txt)'
require_text THIRD_PARTY_NOTICES.md \
    '[`LICENSES/tomlplusplus-MIT.txt`](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/tomlplusplus-MIT.txt)'
require_text THIRD_PARTY_NOTICES.md \
    '[`LICENSES/bjoern-hoehrmann-utf8-MIT.txt`](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/bjoern-hoehrmann-utf8-MIT.txt)'
require_text THIRD_PARTY_NOTICES.md "Every entry below is a separately installed program"
require_text THIRD_PARTY_NOTICES.md "It is not linked into Moguet and is not bundled with Moguet."
require_text THIRD_PARTY_NOTICES.md \
    'GNU gettext tools (`xgettext`, `msgmerge`, and `msgfmt`)'

linked_headings=$(awk '
    /^## Linked or compiled components$/ { in_section = 1; next }
    /^## / && in_section { exit }
    in_section && /^### / { print }
' THIRD_PARTY_NOTICES.md)
expected_linked_headings=$(printf '%s\n' '### libalpm' '### libcurl' '### nlohmann-json' '### toml++')
[ "$linked_headings" = "$expected_linked_headings" ] ||
    fail "linked/compiled component headings contain an unexpected classification."

external_program_section=$(awk '
    /^## External programs invoked$/ { in_section = 1; next }
    /^## / && in_section { exit }
    in_section { print }
' THIRD_PARTY_NOTICES.md)

for program in pacman pacman-conf makepkg vercmp git bsdtar sudo
do
    require_value_text "THIRD_PARTY_NOTICES.md external-program section" \
        "$external_program_section" "| \`$program\` |"
done
require_value_text "THIRD_PARTY_NOTICES.md external-program section" \
    "$external_program_section" '| `/bin/sh` |'
require_value_text "THIRD_PARTY_NOTICES.md external-program section" \
    "$external_program_section" '| User-selected editor (default `nano`) |'
pass "third-party components and external-process boundary are classified"

require_regular_file VERSION
require_regular_file PKGBUILD
current_version=$(tr -d '[:space:]' < VERSION)
[ -n "$current_version" ] || fail "VERSION must identify the current $PROJECT_NAME package."

# POLICY(#310): The current PKGBUILD describes the release named by VERSION. Historical
# jpacker tags retain their own metadata; this validator does not reinterpret them.
pkgbuild_test_dir=$(mktemp -d)
cleanup_pkgbuild_test() {
    rm -rf "$pkgbuild_test_dir"
}
trap cleanup_pkgbuild_test EXIT INT TERM
cp PKGBUILD "$pkgbuild_test_dir/PKGBUILD"

check_pkgbuild_metadata "$current_version"

reject_text PKGBUILD '_license_version_comparison'
reject_text PKGBUILD 'if [[ ${license[0]} == '\''MIT'\'' ]]; then'
reject_text PKGBUILD 'install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"'
pass "PKGBUILD has no historical license evaluation or legacy file fallback"

for readme in README.md README.ja.md
do
    if grep -Fx -- "MIT License" "$readme" >/dev/null; then
        fail "$readme still contains the stale standalone current-project MIT label."
    fi
done
pass "PKGBUILD metadata and current-project READMEs follow the license boundary"

printf 'license-check: all checks passed\n'
