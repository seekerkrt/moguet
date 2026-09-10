#!/bin/sh

set -eu

[ "$#" -eq 7 ] || {
    printf 'usage: test-localization.sh VALID_BINARY MISSING_BINARY CATALOG_DIR MISSING_DIR INVALID_PO MSGFMT CLI_BINARY\n' >&2
    exit 2
}

valid_binary=$1
missing_binary=$2
catalog_dir=$3
missing_catalog_dir=$4
invalid_format_po=$5
msgfmt_command=$6
cli_binary=$7
tmp_dir=$(mktemp -d)
locale_root=$tmp_dir/locales

cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

fail() {
    printf 'localization-test: %s\n' "$*" >&2
    exit 1
}

assert_line() {
    expected=$1
    output_file=$2
    grep -Fqx -- "$expected" "$output_file" || {
        printf 'localization-test: missing line: %s\n' "$expected" >&2
        sed -n '1,160p' "$output_file" >&2
        exit 1
    }
}

assert_not_line() {
    unexpected=$1
    output_file=$2
    if grep -Fqx -- "$unexpected" "$output_file"; then
        printf 'localization-test: unexpected line: %s\n' "$unexpected" >&2
        sed -n '1,160p' "$output_file" >&2
        exit 1
    fi
}

assert_contains() {
    expected=$1
    output_file=$2
    grep -Fq -- "$expected" "$output_file" || {
        printf 'localization-test: missing text: %s\n' "$expected" >&2
        sed -n '1,200p' "$output_file" >&2
        exit 1
    }
}

assert_not_contains() {
    unexpected=$1
    output_file=$2
    if grep -Fq -- "$unexpected" "$output_file"; then
        printf 'localization-test: unexpected text: %s\n' "$unexpected" >&2
        sed -n '1,200p' "$output_file" >&2
        exit 1
    else
        grep_status=$?
        case $grep_status in
            1) ;;
            *)
                fail "text inspection failed with status $grep_status: $output_file"
                ;;
        esac
    fi
}

run_case() {
    case_name=$1
    binary=$2
    process_locale=$3
    language=$4
    count_case=$5
    output_file=$tmp_dir/$case_name.out
    xdg_root=$tmp_dir/$case_name-xdg

    mkdir -p \
        "$xdg_root/home" "$xdg_root/config" \
        "$xdg_root/state" "$xdg_root/cache"
    chmod 0700 "$xdg_root/config"

    LOCPATH=$locale_root \
    LANG=$process_locale \
    LC_ALL=$process_locale \
    LANGUAGE=$language \
    HOME=$xdg_root/home \
    XDG_CONFIG_HOME=$xdg_root/config \
    XDG_STATE_HOME=$xdg_root/state \
    XDG_CACHE_HOME=$xdg_root/cache \
        "$binary" "$count_case" > "$output_file" 2>&1 || {
        sed -n '1,160p' "$output_file" >&2
        fail "$case_name execution failed."
    }
    printf '%s\n' "$output_file"
}

run_help_case() {
    case_name=$1
    process_locale=$2
    language=$3
    help_option=$4
    output_file=$tmp_dir/$case_name.out
    xdg_root=$tmp_dir/$case_name-xdg

    mkdir -p \
        "$xdg_root/home" "$xdg_root/config" \
        "$xdg_root/state" "$xdg_root/cache"
    chmod 0700 "$xdg_root/config"

    LOCPATH=$locale_root \
    LANG=$process_locale \
    LC_ALL=$process_locale \
    LANGUAGE=$language \
    HOME=$xdg_root/home \
    XDG_CONFIG_HOME=$xdg_root/config \
    XDG_STATE_HOME=$xdg_root/state \
    XDG_CACHE_HOME=$xdg_root/cache \
        "$cli_binary" "$help_option" > "$output_file" 2>&1 || {
        sed -n '1,200p' "$output_file" >&2
        fail "$case_name execution failed."
    }
    printf '%s\n' "$output_file"
}

run_list_sources_case() {
    case_name=$1
    process_locale=$2
    language=$3
    preference_kind=$4
    output_file=$tmp_dir/$case_name.out
    xdg_root=$tmp_dir/$case_name-xdg
    preference_root=$xdg_root/config/moguet/source-build.d

    mkdir -p \
        "$xdg_root/home" "$xdg_root/config" \
        "$xdg_root/state" "$xdg_root/cache"
    chmod 0700 "$xdg_root/config"
    case $preference_kind in
        missing)
            ;;
        empty)
            mkdir -p "$preference_root"
            chmod 0700 "$preference_root"
            ;;
        regular)
            mkdir -p "$preference_root"
            chmod 0700 "$preference_root"
            printf '%s\n' 'CFLAGS=-Oidentity' > \
                "$preference_root/identity-package"
            chmod 0600 "$preference_root/identity-package"
            ;;
        *)
            fail "$case_name received unknown preference kind: $preference_kind"
            ;;
    esac
    LOCPATH=$locale_root \
    LANG=$process_locale \
    LC_ALL=$process_locale \
    LANGUAGE=$language \
    HOME=$xdg_root/home \
    XDG_CONFIG_HOME=$xdg_root/config \
    XDG_STATE_HOME=$xdg_root/state \
    XDG_CACHE_HOME=$xdg_root/cache \
        "$cli_binary" list-src > "$output_file" 2>&1 || {
        sed -n '1,200p' "$output_file" >&2
        fail "$case_name execution failed."
    }
    printf '%s\n' "$output_file"
}

run_xdg_resolution_case() {
    case_name=$1
    process_locale=$2
    language=$3
    output_file=$tmp_dir/$case_name.out
    xdg_root=$tmp_dir/$case_name-xdg
    empty_path=$xdg_root/empty-path
    work_dir=$xdg_root/work

    mkdir -p \
        "$xdg_root/home" "$xdg_root/state" "$xdg_root/cache" \
        "$empty_path" "$work_dir"
    set +e
    (
        CDPATH= cd "$work_dir"
        LOCPATH=$locale_root \
        LANG=$process_locale \
        LC_ALL=$process_locale \
        LANGUAGE=$language \
        HOME=$xdg_root/home \
        XDG_CONFIG_HOME=relative/config-secret \
        XDG_STATE_HOME=$xdg_root/state \
        XDG_CACHE_HOME=$xdg_root/cache \
        PATH=$empty_path \
            "$cli_binary" -Q filesystem
    ) > "$output_file" 2>&1
    exit_status=$?
    set -e

    [ "$exit_status" -eq 1 ] || {
        sed -n '1,200p' "$output_file" >&2
        fail "$case_name returned $exit_status instead of 1."
    }
    [ ! -e "$xdg_root/state/moguet" ] &&
        [ ! -L "$xdg_root/state/moguet" ] &&
        [ ! -e "$xdg_root/cache/moguet" ] &&
        [ ! -L "$xdg_root/cache/moguet" ] &&
        [ ! -e "$work_dir/relative" ] &&
        [ ! -L "$work_dir/relative" ] ||
        fail "$case_name mutated XDG storage before path rejection."
    printf '%s\n' "$output_file"
}

strip_ansi() {
    input_file=$1
    output_file=$2
    escape=$(printf '\033')
    sed "s/${escape}\\[[0-9;]*m//g" "$input_file" > "$output_file"
}

extract_help_tokens() {
    input_file=$1
    output_file=$2
    awk '
        BEGIN {
            prefix = "    \033[1m"
            suffix = "\033[0m"
        }
        index($0, prefix) == 1 {
            value = substr($0, length(prefix) + 1)
            suffix_position = index(value, suffix)
            if(suffix_position > 0) {
                print substr(value, 1, suffix_position - 1)
            }
        }
    ' "$input_file" > "$output_file"
}

assert_identity_contract() {
    output_file=$1
    assert_line 'domain=moguet' "$output_file"
    assert_line 'codeset=UTF-8' "$output_file"
    assert_line 'ctype_locale=C' "$output_file"
    assert_line 'command=moguet' "$output_file"
    assert_line 'option=--help' "$output_file"
    assert_line 'external=pacman output' "$output_file"
}

assert_remaining_scope_english() {
    output_file=$1
    assert_line 'owner_logging=Error: {runtime-diagnostic}' "$output_file"
    assert_line "owner_config=User config error: '/tmp/{config}': key 'schema_version': missing required key; expected integer 1" "$output_file"
    assert_line 'owner_process=Failed to ignore SIGINT while waiting for an explicit process: {signal-error}' "$output_file"
    assert_line 'owner_artifact=Failed to inspect the descriptor for /tmp/{artifact}: {artifact-error}' "$output_file"
    assert_line 'owner_metadata=Repository package query failed: libalpm reported no error detail.' "$output_file"
    assert_line 'owner_inspect=Recursive dependency tree:' "$output_file"
    assert_line 'owner_sync=Repository      : aur' "$output_file"
    assert_line 'owner_aur=Checking AUR updates for 7 foreign packages...' "$output_file"
    assert_line 'owner_upgrade=excluded from AUR update: {package-name}' "$output_file"
    assert_line 'relation_installed=Installed conflict confirmed: declaring package declaring-a declares conflict legacy-a>=2 for target component legacy-a; matched installed package installed-a through provided-a=3; build/install is blocked before mutation.' "$output_file"
    assert_line 'relation_planned=Planned-target conflict confirmed: declaring package declaring-p declares conflict planned-api for target component planned-api; matched planned package planned-child through exact-planned; build/install is blocked before mutation.' "$output_file"
    assert_line 'relation_replacement=Potential replacement impact: declaring package declaring-r declares replacement legacy-r for target component legacy-r; matched installed package installed-r through exact-r is a replacement candidate; review is required and no automatic replacement is performed; build/install is blocked before mutation.' "$output_file"
    assert_line 'relation_no_match=Confirmed no matching current or planned target: declaring package declaring-n declares conflict absent-n for target component absent-n; complete current/planned observation found no matching package or provided component; this relation does not block build/install.' "$output_file"
    assert_line 'relation_unknown=Relation judgment unavailable: declaring package declaring-u declares conflict unknown-u>=2 for target component unknown-u; current/planned observation is unavailable; inventory-u; this is not a confirmed absence, so build/install is blocked.' "$output_file"
    assert_line 'relation_invalid=Invalid relation metadata or observation: declaring package declaring-i declares replacement invalid-i for target component invalid-i; invalid-old-self; invalid input is fail-closed, so build/install is blocked.' "$output_file"
    assert_line 'relation_declared=Declared relation awaiting assessment: declaring package declaring-d declares conflict declared-d for target component declared-d; current/planned assessment is incomplete, so build/install remains blocked under the fail-closed policy.' "$output_file"
    assert_line 'relation_check=Relation Check  : deferred to planning and build preflight' "$output_file"
}

assert_remaining_scope_japanese() {
    output_file=$1
    assert_line 'owner_logging=エラー: {runtime-diagnostic}' "$output_file"
    assert_line "owner_config=ユーザー設定エラー: '/tmp/{config}': キー'schema_version': 必須キーがありません。整数1が必要です" "$output_file"
    assert_line 'owner_process=明示的プロセスの待機中に SIGINT を無視する設定へ変更できませんでした: {signal-error}' "$output_file"
    assert_line 'owner_artifact=/tmp/{artifact} のディスクリプターを検査できませんでした: {artifact-error}' "$output_file"
    assert_line 'owner_metadata=リポジトリパッケージの照会に失敗しました: libalpm からエラーの詳細が報告されませんでした。' "$output_file"
    assert_line 'owner_inspect=再帰的な依存関係ツリー:' "$output_file"
    assert_line 'owner_sync=リポジトリ        : aur' "$output_file"
    assert_line 'owner_aur=AURの更新を外部パッケージ7個について確認しています...' "$output_file"
    assert_line 'owner_upgrade=AUR更新から除外: {package-name}' "$output_file"
    assert_line 'relation_installed=インストール済みパッケージとの競合を確認: 宣言元パッケージdeclaring-aは競合 legacy-a>=2 を宣言（対象コンポーネント: legacy-a）; インストール済みパッケージinstalled-aがprovided-a=3として一致; ビルド/インストールは変更前に停止します。' "$output_file"
    assert_line 'relation_planned=計画中の対象との競合を確認: 宣言元パッケージdeclaring-pは競合 planned-api を宣言（対象コンポーネント: planned-api）; 計画中のパッケージplanned-childがexact-plannedとして一致; ビルド/インストールは変更前に停止します。' "$output_file"
    assert_line 'relation_replacement=置換候補による影響: 宣言元パッケージdeclaring-rは置換 legacy-r を宣言（対象コンポーネント: legacy-r）; インストール済みパッケージinstalled-rがexact-rとして置換候補に一致; ユーザーの確認が必要で、自動置換は行いません; ビルド/インストールは変更前に停止します。' "$output_file"
    assert_line 'relation_no_match=現在または計画中の一致対象なしを確認: 宣言元パッケージdeclaring-nは競合 absent-n を宣言（対象コンポーネント: absent-n）; 現在/計画中の状態を完全に観測した結果、一致するパッケージまたは提供コンポーネントはありません; この関係だけを理由にビルド/インストールを停止しません。' "$output_file"
    assert_line 'relation_unknown=関係を確定できません: 宣言元パッケージdeclaring-uは競合 unknown-u>=2 を宣言（対象コンポーネント: unknown-u）; 現在/計画中の観測状態: 利用不可; inventory-u; 一致対象なしを確認した結果ではないため、ビルド/インストールを停止します。' "$output_file"
    assert_line 'relation_invalid=関係メタデータまたは観測が不正です: 宣言元パッケージdeclaring-iは置換 invalid-i を宣言（対象コンポーネント: invalid-i）; invalid-old-self; 不正な入力は安全側に扱い、ビルド/インストールを停止します。' "$output_file"
    assert_line 'relation_declared=宣言済み関係の判定待ち: 宣言元パッケージdeclaring-dは競合 declared-d を宣言（対象コンポーネント: declared-d）; 現在/計画中の判定が未完了のため、安全側にビルド/インストールを停止します。' "$output_file"
    assert_line 'relation_check=関係の判定        : 計画またはビルドの事前検査まで保留' "$output_file"
}

assert_english_messages() {
    output_file=$1
    assert_line 'help=Show this help message and exit' "$output_file"
    assert_line 'diagnostic_project=Do not run Moguet as root or with sudo.' "$output_file"
    assert_line 'diagnostic_command=Run moguet as a normal user; Moguet will invoke sudo/pacman when needed.' "$output_file"
    assert_line 'prompt=Rebuild package?' "$output_file"
    assert_line 'reviewed_target_failure=Reviewed source target revision resolution failed; the build was not started.' "$output_file"
    assert_line 'reviewed_uncertain_failure=Reviewed source state publication reached a post-commit ambiguity; the build was not started, and automatic retry, rollback, and compatibility fallback are forbidden.' "$output_file"
    assert_line 'reviewed_lease_failure=Reviewed source PackageBase lease is already held; the build was not started.' "$output_file"
    assert_line 'reviewed_update_outcome=Reviewed-source outcome for PackageBase localized-base: update review accepted for exact upstream commit 5555555555555555555555555555555555555555.' "$output_file"
    assert_line 'reviewed_update_state=Reviewed state for PackageBase localized-base was finalized as generation 41; build and install outcomes are reported separately.' "$output_file"
    assert_line 'reviewed_update_overlay=Build input for PackageBase localized-base includes an invocation-local editor overlay on reviewed upstream commit 5555555555555555555555555555555555555555; it is not the exact reviewed commit tree.' "$output_file"
    assert_line 'reviewed_build_outcome=Build outcome for PackageBase localized-base: succeeded.' "$output_file"
    assert_line 'reviewed_install_outcome=Install outcome for PackageBase localized-base: failed.' "$output_file"
    assert_line 'reviewed_compatibility=Reviewed-source outcome for PackageBase localized-base: --nodiff skipped review acceptance; this invocation has compatibility-only source authority, and reviewed state was not advanced.' "$output_file"
    assert_line 'reviewed_render_kind=review type: update review' "$output_file"
    assert_line 'reviewed_render_readiness=sensitive source cannot be rendered safely' "$output_file"
    assert_line 'reviewed_initial_kind=review type: initial full review' "$output_file"
    assert_line 'reviewed_rebind_prompt=Accept this explicit rebind/rebaseline of invalid reviewed state?' "$output_file"
    assert_line 'reviewed_tracked_source=tracked source' "$output_file"
    assert_line 'reviewed_source_identity=source identity' "$output_file"
    assert_line 'missing=Missing catalog entry.' "$output_file"
    assert_line 'braces=Use {name} as data.' "$output_file"
    assert_line 'data=Selected package: {danger}' "$output_file"
    assert_remaining_scope_english "$output_file"
    assert_line 'plural=Processed 2 packages.' "$output_file"
}

command -v localedef >/dev/null 2>&1 ||
    fail 'localedef is required for a controlled non-C LC_MESSAGES locale.'
mkdir -p "$locale_root"
localedef --no-archive -i en_US -f UTF-8 \
    "$locale_root/en_US.UTF-8"

c_help_short=$(run_help_case cli-help-c-short C '' -h)
c_help_long=$(run_help_case cli-help-c-long C '' --help)
cmp -s "$c_help_short" "$c_help_long" ||
    fail 'C locale -h and --help output differ.'

ja_help_short=$(run_help_case cli-help-ja-short en_US.UTF-8 ja -h)
ja_help_long=$(run_help_case cli-help-ja-long en_US.UTF-8 ja --help)
cmp -s "$ja_help_short" "$ja_help_long" ||
    fail 'Japanese -h and --help output differ.'

c_help_plain=$tmp_dir/cli-help-c.txt
ja_help_plain=$tmp_dir/cli-help-ja.txt
strip_ansi "$c_help_short" "$c_help_plain"
strip_ansi "$ja_help_short" "$ja_help_plain"

assert_line 'USAGE' "$c_help_plain"
assert_contains \
    'Build one remote package or local PKGBUILD root without saving a preference' \
    "$c_help_plain"
assert_line '使用方法' "$ja_help_plain"
assert_contains \
    '設定を保存せず、リモートパッケージ1件またはローカルPKGBUILDルート1件をビルド' \
    "$ja_help_plain"
assert_contains '$XDG_CONFIG_HOME/moguet/config.toml' "$ja_help_plain"
assert_contains 'review.pkgbuild = "prompt"|"skip"' "$ja_help_plain"
assert_contains 'review.diff = "prompt"|"skip"' "$ja_help_plain"
assert_contains 'build.mode = "normal"|"rebuild"|"clean"' "$ja_help_plain"
for obsolete_config_syntax in \
    'review.pkgbuild = prompt|skip' \
    'review.diff = prompt|skip' \
    'build.mode = normal|rebuild|clean'
do
    assert_not_contains "$obsolete_config_syntax" "$c_help_plain"
    assert_not_contains "$obsolete_config_syntax" "$ja_help_plain"
done
assert_not_contains 'legacy jpacker.conf' "$c_help_plain"
assert_not_contains 'legacy jpacker.conf' "$ja_help_plain"

c_help_tokens=$tmp_dir/cli-help-c.tokens
ja_help_tokens=$tmp_dir/cli-help-ja.tokens
extract_help_tokens "$c_help_short" "$c_help_tokens"
extract_help_tokens "$ja_help_short" "$ja_help_tokens"
[ -s "$c_help_tokens" ] || fail 'C locale help token list is empty.'
cmp -s "$c_help_tokens" "$ja_help_tokens" || {
    diff -u "$c_help_tokens" "$ja_help_tokens" >&2 || true
    fail 'English and Japanese help token sequences differ.'
}
assert_line '--edit' "$ja_help_tokens"
assert_line '--diff' "$ja_help_tokens"
assert_line '--build-mode=normal|rebuild|clean' "$ja_help_tokens"
assert_line '$XDG_CONFIG_HOME/moguet/config.toml' "$ja_help_tokens"

for help_xdg_root in "$tmp_dir"/cli-help-*-xdg; do
    for consumer_directory in \
        "$help_xdg_root/config/moguet" \
        "$help_xdg_root/state/moguet" \
        "$help_xdg_root/cache/moguet"
    do
        [ ! -e "$consumer_directory" ] && [ ! -L "$consumer_directory" ] ||
            fail 'help-only CLI localization case created an XDG consumer directory.'
    done
done

c_xdg_resolution=$(run_xdg_resolution_case \
    cli-xdg-resolution-c C '')
assert_contains \
    'Cannot resolve Moguet config directory: XDG_CONFIG_HOME must be an absolute path.' \
    "$c_xdg_resolution"
assert_not_contains 'relative/config-secret' "$c_xdg_resolution"

ja_xdg_resolution=$(run_xdg_resolution_case \
    cli-xdg-resolution-ja en_US.UTF-8 ja)
assert_contains \
    'Moguetのconfigディレクトリを解決できません: XDG_CONFIG_HOMEは絶対パスである必要があります。' \
    "$ja_xdg_resolution"
assert_not_contains 'relative/config-secret' "$ja_xdg_resolution"

zz_xdg_resolution=$(run_xdg_resolution_case \
    cli-xdg-resolution-missing-translation en_US.UTF-8 zz)
assert_contains \
    'Cannot resolve Moguet config directory: XDG_CONFIG_HOME must be an absolute path.' \
    "$zz_xdg_resolution"
assert_not_contains 'relative/config-secret' "$zz_xdg_resolution"

c_list_missing=$(run_list_sources_case \
    cli-list-sources-c-missing C '' missing)
assert_contains 'No source-build packages registered.' "$c_list_missing"

ja_list_missing=$(run_list_sources_case \
    cli-list-sources-ja-missing en_US.UTF-8 ja missing)
assert_contains 'ソースビルド対象のパッケージは登録されていません。' \
    "$ja_list_missing"

c_list_empty=$(run_list_sources_case \
    cli-list-sources-c-empty C '' empty)
assert_contains 'Registered Source Packages:' "$c_list_empty"
assert_contains '(none)' "$c_list_empty"

ja_list_empty=$(run_list_sources_case \
    cli-list-sources-ja-empty en_US.UTF-8 ja empty)
assert_contains '登録済みソースパッケージ:' "$ja_list_empty"
assert_contains '（なし）' "$ja_list_empty"

zz_list_empty=$(run_list_sources_case \
    cli-list-sources-missing-translation en_US.UTF-8 zz \
    empty)
assert_contains 'Registered Source Packages:' "$zz_list_empty"
assert_contains '(none)' "$zz_list_empty"

ja_list_regular=$(run_list_sources_case \
    cli-list-sources-ja-regular en_US.UTF-8 ja regular)
assert_contains '登録済みソースパッケージ:' "$ja_list_regular"
assert_contains 'identity-package' "$ja_list_regular"
assert_contains 'CFLAGS=-Oidentity' "$ja_list_regular"

[ ! -e "$tmp_dir/cli-list-sources-c-missing-xdg/config/moguet/source-build.d" ] &&
    [ ! -L "$tmp_dir/cli-list-sources-c-missing-xdg/config/moguet/source-build.d" ] ||
    fail 'list-src localization case created the missing preference root.'

[ ! -e "$missing_catalog_dir" ] && [ ! -L "$missing_catalog_dir" ] ||
    fail "missing-catalog fixture path already exists: $missing_catalog_dir"

c_output=$(run_case c-locale "$valid_binary" C '' two)
assert_identity_contract "$c_output"
assert_line "locale_directory=$catalog_dir" "$c_output"
assert_line 'message_locale=C' "$c_output"
assert_english_messages "$c_output"

ja_output=$(run_case japanese "$valid_binary" en_US.UTF-8 ja two)
assert_identity_contract "$ja_output"
assert_line "locale_directory=$catalog_dir" "$ja_output"
assert_not_line 'message_locale=C' "$ja_output"
assert_line 'help=このヘルプを表示して終了' "$ja_output"
assert_line 'diagnostic_project=Moguetをrootとして、またはsudo経由で実行しないでください。' "$ja_output"
assert_line 'diagnostic_command=moguetは通常ユーザーとして実行してください。Moguetは必要に応じてsudo/pacmanを呼び出します。' "$ja_output"
assert_line 'prompt=パッケージを再ビルドしますか？' "$ja_output"
assert_line 'reviewed_target_failure=確認済みソースの対象リビジョンを解決できなかったため、ビルドを開始しませんでした。' "$ja_output"
assert_line 'reviewed_uncertain_failure=確認済みソースの状態公開はcommit後に結果を確定できない状態になりました。ビルドは開始せず、自動再試行、rollback、互換fallbackも行いません。' "$ja_output"
assert_line 'reviewed_lease_failure=確認済みソースのPackageBaseロックは別の処理が保持中です。ビルドを開始しませんでした。' "$ja_output"
assert_line 'reviewed_update_outcome=確認済みソースの結果（PackageBase localized-base）: 正確なupstream commit 5555555555555555555555555555555555555555 の更新レビューを受理しました。' "$ja_output"
assert_line 'reviewed_update_state=確認済み状態（PackageBase localized-base）を世代 41 として確定しました。ビルドとインストールの結果は別に報告します。' "$ja_output"
assert_line 'reviewed_update_overlay=PackageBase localized-base のビルド入力には、確認済みupstream commit 5555555555555555555555555555555555555555 上のこの実行内のeditor変更が含まれます。これは正確な確認済みcommit treeそのものではありません。' "$ja_output"
assert_line 'reviewed_build_outcome=ビルド結果（PackageBase localized-base）: 成功。' "$ja_output"
assert_line 'reviewed_install_outcome=インストール結果（PackageBase localized-base）: 失敗。' "$ja_output"
assert_line 'reviewed_compatibility=確認済みソースの結果（PackageBase localized-base）: --nodiff によりレビュー受理をスキップしました。この呼び出しには互換経路のソース権限だけがあり、確認済み状態は進めていません。' "$ja_output"
assert_line 'reviewed_render_kind=レビュー種別: 更新レビュー' "$ja_output"
assert_line 'reviewed_render_readiness=レビュー上重要なソースを安全に表示不可' "$ja_output"
assert_line 'reviewed_initial_kind=レビュー種別: 初回全体レビュー' "$ja_output"
assert_line 'reviewed_rebind_prompt=不正な確認済み状態の明示的な再関連付け／再ベースライン確認を受理しますか？' "$ja_output"
assert_line 'reviewed_tracked_source=追跡対象ソース' "$ja_output"
assert_line 'reviewed_source_identity=ソース識別情報' "$ja_output"
assert_not_contains 'UpdateReview' "$ja_output"
assert_not_contains 'InvocationLocal' "$ja_output"
assert_not_contains 'review-sensitive source' "$ja_output"
assert_not_contains 'exact upstream commit' "$ja_output"
assert_not_contains 'full review' "$ja_output"
assert_not_contains 'full rebaseline' "$ja_output"
assert_not_contains 'rebind/rebaseline' "$ja_output"
assert_not_contains 'source identity' "$ja_output"
assert_not_contains 'generation' "$ja_output"
assert_not_contains 'invocation-local editor overlay' "$ja_output"
assert_not_contains 'tracked source' "$ja_output"
assert_not_contains 'target resolution' "$ja_output"
assert_not_contains 'uncertain outcome' "$ja_output"
assert_not_contains 'lease acquisition' "$ja_output"
assert_line 'missing=Missing catalog entry.' "$ja_output"
assert_line 'braces=Use {name} as data.' "$ja_output"
assert_line 'data=Selected package: {danger}' "$ja_output"
assert_remaining_scope_japanese "$ja_output"
assert_line 'plural=Processed 2 packages.' "$ja_output"

missing_output=$(run_case missing-catalog "$missing_binary" en_US.UTF-8 ja two)
assert_identity_contract "$missing_output"
assert_line "locale_directory=$missing_catalog_dir" "$missing_output"
assert_english_messages "$missing_output"

unsupported_output=$(run_case unsupported-locale "$valid_binary" moguet_INVALID.UTF-8 ja two)
assert_identity_contract "$unsupported_output"
assert_line 'message_locale=C' "$unsupported_output"
assert_english_messages "$unsupported_output"

zz_one_output=$(run_case additional-locale-one "$valid_binary" en_US.UTF-8 zz one)
assert_identity_contract "$zz_one_output"
assert_remaining_scope_english "$zz_one_output"
assert_line 'help=ZZ help' "$zz_one_output"
assert_line 'diagnostic_project=ZZ do not run Moguet as root or with sudo' "$zz_one_output"
assert_line 'diagnostic_command=ZZ run moguet; Moguet uses sudo/pacman' "$zz_one_output"
assert_line 'prompt=ZZ rebuild?' "$zz_one_output"
assert_line 'data=ZZ selected: {danger}' "$zz_one_output"
assert_line 'plural=ZZ processed 1 package.' "$zz_one_output"

zz_two_output=$(run_case additional-locale-two "$valid_binary" en_US.UTF-8 zz two)
assert_line 'plural=ZZ processed 2 packages.' "$zz_two_output"

broken_output=$(run_case broken-runtime-catalog "$valid_binary" en_US.UTF-8 broken two)
assert_identity_contract "$broken_output"
assert_line 'data=Selected package: {danger}' "$broken_output"
assert_line 'plural=Processed 2 packages.' "$broken_output"

invalid_log=$tmp_dir/invalid-format.log
if "$msgfmt_command" --check --check-format --check-domain \
        --output-file="$tmp_dir/invalid-format.mo" \
        "$invalid_format_po" > "$invalid_log" 2>&1; then
    fail 'msgfmt accepted a catalog with mismatched C++ format placeholders.'
fi

printf 'localization-test: all checks passed\n'
