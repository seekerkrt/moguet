#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    printf 'usage: test-help-man-completion.sh CLI_LOCALIZATION_BINARY\n' >&2
    exit 2
fi

test_binary=$1
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
tmp_dir=$(mktemp -d)

cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

fail() {
    printf 'help-man-completion-test: %s\n' "$*" >&2
    exit 1
}

assert_contains() {
    pattern=$1
    file=$2
    grep -F -- "$pattern" "$file" >/dev/null ||
        fail "missing expected help text: $pattern"
}

assert_not_contains() {
    pattern=$1
    file=$2
    if grep -F -- "$pattern" "$file" >/dev/null; then
        fail "unexpected obsolete help text: $pattern"
    else
        grep_status=$?
        case $grep_status in
            1) ;;
            *) fail "help text inspection failed with status $grep_status: $file" ;;
        esac
    fi
}

for command_name in python3 bash zsh fish localedef; do
    command -v "$command_name" >/dev/null 2>&1 ||
        fail "$command_name is required."
done

PYTHONDONTWRITEBYTECODE=1 \
    python3 "$repo_root/scripts/generate_completions.py" --check
bash -n "$repo_root/completions/moguet.bash"
zsh -n "$repo_root/completions/_moguet"
fish --no-execute "$repo_root/completions/moguet.fish"

locale_root=$tmp_dir/locale
test_home=$tmp_dir/home
mkdir -p "$locale_root" "$test_home"
localedef --no-archive -i en_US -f UTF-8 "$locale_root/en_US.UTF-8"

english_help=$tmp_dir/help-en
english_help_short=$tmp_dir/help-en-short
japanese_help=$tmp_dir/help-ja
japanese_help_short=$tmp_dir/help-ja-short
HOME=$test_home \
XDG_CONFIG_HOME=$tmp_dir/config \
XDG_STATE_HOME=$tmp_dir/state \
XDG_CACHE_HOME=$tmp_dir/cache \
LC_ALL=C \
LANGUAGE= \
    "$test_binary" --help > "$english_help" 2>&1

HOME=$test_home \
XDG_CONFIG_HOME=$tmp_dir/config \
XDG_STATE_HOME=$tmp_dir/state \
XDG_CACHE_HOME=$tmp_dir/cache \
LC_ALL=C \
LANGUAGE= \
    "$test_binary" -h > "$english_help_short" 2>&1
cmp -s "$english_help" "$english_help_short" ||
    fail 'English -h and --help output differ.'

HOME=$test_home \
XDG_CONFIG_HOME=$tmp_dir/config \
XDG_STATE_HOME=$tmp_dir/state \
XDG_CACHE_HOME=$tmp_dir/cache \
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    "$test_binary" --help > "$japanese_help" 2>&1

HOME=$test_home \
XDG_CONFIG_HOME=$tmp_dir/config \
XDG_STATE_HOME=$tmp_dir/state \
XDG_CACHE_HOME=$tmp_dir/cache \
LOCPATH=$locale_root \
LANG=en_US.UTF-8 \
LC_ALL=en_US.UTF-8 \
LANGUAGE=ja \
    "$test_binary" -h > "$japanese_help_short" 2>&1
cmp -s "$japanese_help" "$japanese_help_short" ||
    fail 'Japanese -h and --help output differ.'

assert_contains '--details' "$english_help"
assert_contains 'Show detailed diagnostic and provenance information' "$english_help"
assert_contains 'For remote build, plan, deps, -S --select, -Qua, -Syu / -Su, upgrade-aur, upgrade-all, and --dry-run -S; changes presentation only, not execution' "$english_help"
assert_contains '--details' "$japanese_help"
assert_contains '詳細な診断情報と由来情報を表示' "$japanese_help"
assert_contains 'リモートbuild、plan、deps、-S --select、-Qua、-Syu / -Su、upgrade-aur、upgrade-all、--dry-run -Sで使用可能。表示だけを変更し、実行動作は変えません' "$japanese_help"
assert_not_contains '--verbose' "$english_help"
assert_not_contains '--verbose' "$japanese_help"
assert_contains 'Show detailed diagnostic and provenance information for' "$repo_root/man/moguet.1.in"
assert_contains 'This invocation-only option changes presentation, not execution, dependency' "$repo_root/man/moguet.1.in"
assert_contains 'This option displays the full escaped command instead.' "$repo_root/man/moguet.1.in"
assert_contains 'で詳細な診断情報と由来情報を表示します。' "$repo_root/man/ja/moguet.1.in"
assert_contains 'このinvocationだけの表示指定であり、実行、依存解決、選択、readinessは変更しません。' "$repo_root/man/ja/moguet.1.in"
assert_contains 'このoptionでは、代わりにエスケープ済みの完全なコマンドを表示します。' "$repo_root/man/ja/moguet.1.in"

assert_contains \
    'Classify AUR dependencies and show constraint and conflict/replacement assessments' \
    "$english_help"
assert_contains \
    'Show assessed conflict/replacement blockers before any supported mutation' \
    "$english_help"
assert_contains \
    'Do not bypass conflict/replacement safety stops or perform automatic replacement' \
    "$english_help"
assert_contains \
    'AURの依存関係を分類し、制約と競合/置換の判定結果を表示' \
    "$japanese_help"
assert_contains \
    '対応する変更の前に判定済みの競合/置換ブロッカーを表示' \
    "$japanese_help"
assert_contains \
    '競合/置換の安全停止を回避せず、自動置換も行いません' \
    "$japanese_help"

japanese_unsupported_option=$tmp_dir/unsupported-option-ja
if HOME=$test_home \
    XDG_CONFIG_HOME=$tmp_dir/config \
    XDG_STATE_HOME=$tmp_dir/state \
    XDG_CACHE_HOME=$tmp_dir/cache \
    LOCPATH=$locale_root \
    LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8 \
    LANGUAGE=ja \
    "$test_binary" -Syu --config custom.conf \
        > "$japanese_unsupported_option" 2>&1
then
    fail 'Japanese unsupported Auto option unexpectedly succeeded.'
else
    unsupported_status=$?
fi
[ "$unsupported_status" -eq 1 ] ||
    fail "Japanese unsupported Auto option returned $unsupported_status."
assert_contains \
    'pacmanオプションは複合-Syu経路では対応していません。moguet -Syu --repoを使用すると、pacman引数を完全に引き継ぐリポジトリ専用システム更新になります。' \
    "$japanese_unsupported_option"

japanese_unsupported_argument=$tmp_dir/unsupported-argument-ja
if HOME=$test_home \
    XDG_CONFIG_HOME=$tmp_dir/config \
    XDG_STATE_HOME=$tmp_dir/state \
    XDG_CACHE_HOME=$tmp_dir/cache \
    LOCPATH=$locale_root \
    LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8 \
    LANGUAGE=ja \
    "$test_binary" -Syu -- \
        > "$japanese_unsupported_argument" 2>&1
then
    fail 'Japanese unsupported Auto argument form unexpectedly succeeded.'
else
    unsupported_status=$?
fi
[ "$unsupported_status" -eq 1 ] ||
    fail "Japanese unsupported Auto argument form returned $unsupported_status."
assert_contains \
    'pacman引数形式は複合-Syu経路では対応していません。moguet -Syu --repoを使用すると、pacman引数を完全に引き継ぐリポジトリ専用システム更新になります。' \
    "$japanese_unsupported_argument"

for config_syntax in \
    'review.pkgbuild = "prompt"|"skip"' \
    'review.diff = "prompt"|"skip"' \
    'build.mode = "normal"|"rebuild"|"clean"'
do
    assert_contains "$config_syntax" "$english_help"
    assert_contains "$config_syntax" "$japanese_help"
done

for obsolete_config_syntax in \
    'review.pkgbuild = prompt|skip' \
    'review.diff = prompt|skip' \
    'build.mode = normal|rebuild|clean'
do
    assert_not_contains "$obsolete_config_syntax" "$english_help"
    assert_not_contains "$obsolete_config_syntax" "$japanese_help"
done

version_short=$tmp_dir/version-short
version_long=$tmp_dir/version-long
HOME=$test_home \
XDG_CONFIG_HOME=$tmp_dir/config \
XDG_STATE_HOME=$tmp_dir/state \
XDG_CACHE_HOME=$tmp_dir/cache \
LC_ALL=C \
LANGUAGE= \
    "$test_binary" -V > "$version_short" 2>&1
HOME=$test_home \
XDG_CONFIG_HOME=$tmp_dir/config \
XDG_STATE_HOME=$tmp_dir/state \
XDG_CACHE_HOME=$tmp_dir/cache \
LC_ALL=C \
LANGUAGE= \
    "$test_binary" --version > "$version_long" 2>&1
cmp -s "$version_short" "$version_long" ||
    fail '-V and --version output differ.'
version=$(tr -d '[:space:]' < "$repo_root/VERSION")
[ "$(cat "$version_short")" = "Moguet v$version" ] ||
    fail 'version output does not match VERSION.'

PYTHONDONTWRITEBYTECODE=1 \
python3 "$repo_root/scripts/check_public_documentation.py" \
    --help-en "$english_help" \
    --help-ja "$japanese_help"

[ ! -e "$tmp_dir/config" ] &&
    [ ! -e "$tmp_dir/state" ] &&
    [ ! -e "$tmp_dir/cache" ] ||
    fail 'help-only checks created XDG consumer directories.'

printf 'help-man-completion-test: all checks passed\n'
