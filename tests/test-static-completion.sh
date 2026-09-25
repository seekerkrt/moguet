#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
completion_file="${1:-${script_dir}/../completions/moguet.bash}"

# Production static fallbackをそのまま読み込み、Bashが渡すcontextを再現する。
source "${completion_file}"

case_count=0

run_completion() {
    COMP_WORDS=("$@")
    COMP_CWORD=$((${#COMP_WORDS[@]} - 1))
    COMPREPLY=()
    _moguet
}

assert_reply() {
    local label="$1"
    shift
    local -a expected=("$@")
    local index

    case_count=$((case_count + 1))
    if [[ ${#COMPREPLY[@]} -ne ${#expected[@]} ]]; then
        printf 'FAIL: %s\nexpected: %s\nactual:   %s\n' \
            "${label}" "${expected[*]}" "${COMPREPLY[*]}" >&2
        exit 1
    fi

    for ((index = 0; index < ${#expected[@]}; ++index)); do
        if [[ ${COMPREPLY[index]} != "${expected[index]}" ]]; then
            printf 'FAIL: %s\nexpected: %s\nactual:   %s\n' \
                "${label}" "${expected[*]}" "${COMPREPLY[*]}" >&2
            exit 1
        fi
    done
}

root_candidates=(
    build upgrade upgrade-aur upgrade-all clean deps plan fetch
    add-src edit-src list-src del-src revert
    -G -Gp -S -Syu -Su -Ss -Si -Qua
    -h --help -V --version
    --edit --noedit --diff --nodiff --noconfirm --dry-run --build-mode=
    --rebuild --cleanbuild --rmdeps --select --aur --repo --details
)

registration=$(complete -p moguet)
[[ ${registration} == 'complete -F _moguet moguet' ]] || {
    printf 'FAIL: unexpected completion registration: %s\n' "${registration}" >&2
    exit 1
}
case_count=$((case_count + 1))

run_completion moguet ""
assert_reply "operation未選択時のpublic root候補" "${root_candidates[@]}"

run_completion moguet unknown-operation ""
assert_reply "unknown bare operation後はroot候補へ戻らない"

run_completion moguet upgrade-all --noedit ""
assert_reply \
    "upgrade-allのoption scopeとconflict" \
    --noedit --diff --nodiff --noconfirm --dry-run --build-mode= \
    --rebuild --cleanbuild --details

run_completion moguet upgrade-all unexpected-target ""
assert_reply "targetless operationの不正operand後は候補を提示しない"

run_completion moguet list-src unexpected-target ""
assert_reply "optionなしtargetless operationも不正operand後は閉じる"

run_completion moguet up
assert_reply "operation prefix" upgrade upgrade-aur upgrade-all

run_completion moguet deps --r
assert_reply "deps固有option prefix" --recursive

run_completion moguet -G pkg --o
assert_reply "-G output parent option prefix" --output-dir=

run_completion moguet -G pkg --output-dir=./exports ""
assert_reply "-G output parent optionは1回だけ"

run_completion moguet -Gp pkg --o
assert_reply "-Gpはoutput parent optionを提示しない"

run_completion moguet --s
assert_reply "root discovery option prefix" --select

run_completion moguet --l
assert_reply "operation-local optionはrootで提示しない"

run_completion moguet --b
assert_reply "attached-value option token" --build-mode=

run_completion moguet --d
assert_reply "diagnostic and dry-run option prefix" --diff --dry-run --details

# enum / package候補のdynamic completionは#253へ残す。
run_completion moguet --build-mode=n
assert_reply "typed build-mode valueは提示しない"

run_completion moguet build ""
assert_reply \
    "build form未選択時はremote/localのunion" \
    --use-preference --edit --noedit --diff --nodiff --noconfirm --dry-run --build-mode= \
    --rebuild --cleanbuild --details --local

run_completion moguet build pkg ""
assert_reply \
    "remote build target後はlocal selectorを提示しない" \
    --use-preference --edit --noedit --diff --nodiff --noconfirm --dry-run --build-mode= \
    --rebuild --cleanbuild --details

run_completion moguet build pkg V=1 ""
assert_reply \
    "remote buildのtrailing assignmentを維持" \
    --use-preference --edit --noedit --diff --nodiff --noconfirm --dry-run --build-mode= \
    --rebuild --cleanbuild --details

run_completion moguet build pkg extra ""
assert_reply "remote buildのsecond bare operand後は候補を提示しない"

run_completion moguet build --local ""
assert_reply \
    "local build固有scopeとonce selector" \
    --edit --noedit --noconfirm --dry-run --build-mode= --rebuild --cleanbuild

run_completion moguet build --local directory V=1 ""
assert_reply \
    "local buildのdirectory/trailing assignmentを維持" \
    --edit --noedit --noconfirm --dry-run --build-mode= --rebuild --cleanbuild

run_completion moguet build --local directory extra ""
assert_reply "local buildのsecond bare operand後は候補を提示しない"

run_completion moguet clean ""
assert_reply "cleanのroute-owned option" --noconfirm

run_completion moguet list-src ""
assert_reply "list-srcはoptionを持たない"

run_completion moguet deps first ""
assert_reply "deps multi-target formを閉じない" --noconfirm --details --recursive

run_completion moguet deps first second ""
assert_reply "deps second target後もmulti-target formを閉じない" --noconfirm --details --recursive

run_completion moguet plan first second ""
assert_reply "plan multi-target formを閉じない" --noconfirm --details

for operation in build plan deps; do
    run_completion moguet "$operation" --details --det
    assert_reply "$operationのdetailsはrepeat-idempotent" --details
done
run_completion moguet -S --select --det
assert_reply "selected -Sはdetailsを提示する" --details
run_completion moguet -S --det
assert_reply "plain -Sはdetailsを提示しない"
run_completion moguet fetch --det
assert_reply "fetchはdetailsを提示しない"

run_completion moguet fetch first second ""
assert_reply "fetch multi-target formを閉じない" --noconfirm --dry-run

run_completion moguet revert first second ""
assert_reply "source-maintenance multi-target formを閉じない" --noconfirm

COMP_WORDS=(moguet add-src first V=1 second "")
COMP_CWORD=5
_moguet_form_prefix_valid add-src 0 || {
    printf 'FAIL: add-src ordered multi-item formを閉じた\n' >&2
    exit 1
}
case_count=$((case_count + 1))

for source_operation in edit-src del-src; do
    COMP_WORDS=(moguet "${source_operation}" first second "")
    COMP_CWORD=4
    _moguet_form_prefix_valid "${source_operation}" 0 || {
        printf 'FAIL: %s multi-target formを閉じた\n' "${source_operation}" >&2
        exit 1
    }
    case_count=$((case_count + 1))
done

run_completion moguet -S ""
assert_reply "plain -Sはopen grammarとselect入口を維持" --needed --noconfirm --select

run_completion moguet -S --select ""
assert_reply \
    "source-aware select固有option scope" \
    --select --needed --edit --noedit --diff --nodiff --noconfirm --dry-run \
    --build-mode= --rebuild --cleanbuild --aur --repo --details

run_completion moguet -S --select query ""
assert_reply \
    "source-aware select exactly-one queryを維持" \
    --select --needed --edit --noedit --diff --nodiff --noconfirm --dry-run \
    --build-mode= --rebuild --cleanbuild --aur --repo --details

run_completion moguet -S --select query extra ""
assert_reply "source-aware select extra query後は候補を提示しない"

for sync_operation in -Syu -Su; do
    run_completion moguet $sync_operation ""
    assert_reply \
        "$sync_operation Autoはnormal AUR対応optionとRepoOnly escape hatchを提示" \
        --edit --noedit --diff --nodiff --noconfirm --dry-run --details --build-mode= \
        --rebuild --cleanbuild --needed --repo

    run_completion moguet $sync_operation --repo ""
    assert_reply \
        "$sync_operation RepoOnlyはrepository surfaceだけを提示" \
        --repo --needed --noconfirm --dry-run --details

    run_completion moguet $sync_operation package ""
    assert_reply \
        "target-bearing $sync_operationはopen pacman grammarを維持" \
        --needed --noconfirm

    run_completion moguet $sync_operation --a
    assert_reply "$sync_operationはunsupported --aurを提示しない"
done

run_completion moguet -Qua --det
assert_reply "foreign updatesはdetailsを提示する" --details
run_completion moguet -S --dry-run --det
assert_reply "single-target dry-runはdetailsを提示する" --details
run_completion moguet -S --select --dry-run --det
assert_reply "selected dry-runでdetailsを重複提示しない" --details

run_completion moguet -Q ""
assert_reply "未列挙pacman operationもopen grammarとして扱う" --needed --noconfirm

run_completion moguet build --rebuild ""
assert_reply \
    "repeat可能aliasを維持しconflict候補を除外" \
    --use-preference --edit --noedit --diff --nodiff --noconfirm --dry-run --rebuild --details --local

zsh_completion="$(dirname -- "${completion_file}")/_moguet"
fish_completion="$(dirname -- "${completion_file}")/moguet.fish"
if command -v zsh >/dev/null 2>&1; then
    zsh -n "${zsh_completion}"
    MOGUET_COMPLETION_FILE="${zsh_completion}" zsh -f <<'ZSH'
compdef() { return 0 }
source "$MOGUET_COMPLETION_FILE"

fail() {
    print -u2 -- "FAIL: Zsh completion semantic projection: $1"
    exit 1
}

has_candidate() {
    local expected="$1" candidate
    for candidate in "${reply[@]}"; do
        [[ $candidate == "$expected" ]] && return 0
    done
    return 1
}

words=(moguet '')
CURRENT=2
_moguet_find_operation && fail 'root state was classified as an operation'
[[ -z $REPLY ]] || fail 'root state retained a stale operation marker'

words=(moguet unknown-operation '')
CURRENT=3
_moguet_find_operation || fail 'unknown bare operation state was lost'
[[ $REPLY == __invalid__ ]] || fail 'unknown bare operation was not invalid'
_moguet_collect_candidates "$REPLY"
(( ${#reply[@]} == 0 )) || fail 'unknown bare operation returned root candidates'

words=(moguet deps '')
CURRENT=3
_moguet_find_operation || fail 'deps operation not found'
[[ $REPLY == deps ]] || fail 'deps operation identity differs'
_moguet_collect_candidates "$REPLY"
has_candidate --recursive || fail 'deps lost --recursive'
has_candidate --details || fail 'deps lost --details'
has_candidate --local && fail 'deps leaked --local'

words=(moguet deps first second '')
CURRENT=5
_moguet_collect_candidates deps
has_candidate --recursive || fail 'deps multi-target form was closed'

words=(moguet -G pkg '')
CURRENT=4
_moguet_collect_candidates -G
has_candidate --output-dir= || fail '-G lost --output-dir='

words=(moguet -Gp pkg '')
CURRENT=4
_moguet_collect_candidates -Gp
has_candidate --output-dir= && fail '-Gp exposed --output-dir='

words=(moguet build pkg V=1 '')
CURRENT=5
_moguet_collect_candidates build
has_candidate --edit || fail 'remote build assignment flow was closed'

words=(moguet build pkg '')
CURRENT=4
_moguet_collect_candidates build
has_candidate --edit || fail 'remote build primary operand was closed'
has_candidate --details || fail 'remote build lost --details'
has_candidate --use-preference || fail 'remote build lost --use-preference'

words=(moguet build pkg extra '')
CURRENT=5
_moguet_collect_candidates build
(( ${#reply[@]} == 0 )) || fail 'remote build second bare operand remained open'

words=(moguet build --local '')
CURRENT=4
_moguet_collect_candidates build
has_candidate --edit || fail 'local build lost --edit'
has_candidate --use-preference && fail 'local build leaked --use-preference'
has_candidate --diff && fail 'local build leaked --diff'
has_candidate --details && fail 'local build leaked --details'

words=(moguet build --local directory V=1 '')
CURRENT=6
_moguet_collect_candidates build
has_candidate --edit || fail 'local build assignment flow was closed'

words=(moguet build --local directory '')
CURRENT=5
_moguet_collect_candidates build
has_candidate --edit || fail 'local build directory operand was closed'

words=(moguet build --local directory extra '')
CURRENT=6
_moguet_collect_candidates build
(( ${#reply[@]} == 0 )) || fail 'local build second bare operand remained open'

words=(moguet upgrade-all unexpected-target '')
CURRENT=4
_moguet_collect_candidates upgrade-all
(( ${#reply[@]} == 0 )) || fail 'targetless operation remained open'

words=(moguet -S --select '')
CURRENT=4
_moguet_collect_candidates -S
has_candidate --needed || fail 'selected -S lost --needed'
has_candidate --details || fail 'selected -S lost --details'
has_candidate --recursive && fail 'selected -S leaked --recursive'
_moguet_description --details
[[ $REPLY == *'diagnostic and provenance'* ]] || fail 'details description missing'

words=(moguet -S --select query '')
CURRENT=5
_moguet_collect_candidates -S
has_candidate --needed || fail 'selected -S legal query was closed'

words=(moguet -S --select query extra '')
CURRENT=6
_moguet_collect_candidates -S
(( ${#reply[@]} == 0 )) || fail 'selected -S extra query remained open'

words=(moguet -Syu '')
CURRENT=3
_moguet_collect_candidates -Syu
has_candidate --repo || fail '-Syu lost --repo'
has_candidate --dry-run || fail '-Syu lost --dry-run'
has_candidate --needed || fail '-Syu lost --needed'
has_candidate --noconfirm || fail '-Syu lost --noconfirm'
has_candidate --edit || fail '-Syu lost normal-AUR review options'
has_candidate --aur && fail '-Syu exposed unsupported --aur'
has_candidate --rmdeps && fail '-Syu exposed unsupported --rmdeps'

words=(moguet -Syu --repo '')
CURRENT=4
_moguet_collect_candidates -Syu
has_candidate --dry-run || fail '-Syu --repo lost --dry-run'
has_candidate --edit && fail '-Syu --repo leaked AUR review options'
has_candidate --aur && fail '-Syu --repo exposed --aur'

words=(moguet -Syu package '')
CURRENT=4
_moguet_collect_candidates -Syu
has_candidate --needed || fail 'target-bearing -Syu lost delegated options'
has_candidate --repo && fail 'target-bearing -Syu leaked exact RepoOnly selector'

words=(moguet add-src first V=1 second '')
CURRENT=6
_moguet_form_prefix_valid add-src 0 || fail 'add-src ordered multi-item form was closed'

words=(moguet revert first second '')
CURRENT=5
_moguet_collect_candidates revert
has_candidate --noconfirm || fail 'source-maintenance multi-target form was closed'

words=(moguet -Q '')
CURRENT=3
_moguet_find_operation || fail 'delegated operation not found'
[[ $REPLY == __delegated__ ]] || fail 'delegated operation was closed'
_moguet_collect_candidates "$REPLY"
has_candidate --noconfirm || fail 'delegated grammar lost --noconfirm'
ZSH
    case_count=$((case_count + 1))
fi
if command -v fish >/dev/null 2>&1; then
    fish -n "${fish_completion}"
    MOGUET_COMPLETION_FILE="${fish_completion}" fish --no-config <<'FISH'
set -g mock_words

function commandline
    printf '%s\n' $mock_words
end

source "$MOGUET_COMPLETION_FILE"

function fail --argument-names detail
    echo "FAIL: Fish completion semantic projection: $detail" >&2
    exit 1
end

set mock_words moguet
__moguet_no_operation; or fail 'root state lost operation candidates'

set mock_words moguet unknown-operation
test (__moguet_operation) = __invalid__; or fail 'unknown bare operation was not invalid'
__moguet_no_operation; and fail 'unknown bare operation returned to root state'
__moguet_candidate_available 4; and fail 'unknown bare operation exposed options'

set mock_words moguet deps
test (__moguet_operation) = deps; or fail 'deps operation identity differs'
__moguet_candidate_available 17; or fail 'deps lost --recursive'
__moguet_candidate_available 20; or fail 'deps lost --details'
__moguet_candidate_available 15; and fail 'deps leaked --local'

set mock_words moguet deps first second
__moguet_candidate_available 17; or fail 'deps multi-target form was closed'

set mock_words moguet -G pkg
__moguet_candidate_available 16; or fail '-G lost --output-dir='
set mock_words moguet -G pkg --output-dir=./exports
__moguet_candidate_available 16; and fail '-G repeated --output-dir='
set mock_words moguet -Gp pkg
__moguet_candidate_available 16; and fail '-Gp exposed --output-dir='

set mock_words moguet build pkg V=1
__moguet_candidate_available 0; or fail 'remote build assignment flow was closed'
set mock_words moguet build pkg
__moguet_candidate_available 0; or fail 'remote build primary operand was closed'
__moguet_candidate_available 20; or fail 'remote build lost --details'
__moguet_candidate_available 21; or fail 'remote build lost --use-preference'
set mock_words moguet build pkg extra
__moguet_candidate_available 0; and fail 'remote build second bare operand remained open'

set mock_words moguet build --local
__moguet_candidate_available 0; or fail 'local build lost --edit'
__moguet_candidate_available 21; and fail 'local build leaked --use-preference'
__moguet_candidate_available 2; and fail 'local build leaked --diff'
__moguet_candidate_available 20; and fail 'local build leaked --details'
__moguet_candidate_available 15; and fail 'once --local remained available'
set mock_words moguet build --local directory V=1
__moguet_candidate_available 0; or fail 'local build assignment flow was closed'
set mock_words moguet build --local directory
__moguet_candidate_available 0; or fail 'local build directory operand was closed'
set mock_words moguet build --local directory extra
__moguet_candidate_available 0; and fail 'local build second bare operand remained open'

set mock_words moguet upgrade-all
__moguet_candidate_available 4; or fail 'upgrade-all lost --noconfirm'
set mock_words moguet upgrade-all unexpected-target
__moguet_candidate_available 4; and fail 'targetless operation remained open'

set mock_words moguet -S --select
__moguet_candidate_available 18; or fail 'selected -S lost --needed'
__moguet_candidate_available 20; or fail 'selected -S lost --details'
__moguet_candidate_available 17; and fail 'selected -S leaked --recursive'
set mock_words moguet -S --select query
__moguet_candidate_available 18; or fail 'selected -S legal query was closed'
set mock_words moguet -S --select query extra
__moguet_candidate_available 18; and fail 'selected -S extra query remained open'

set mock_words moguet -Syu
__moguet_candidate_available 12; or fail '-Syu lost --repo'
__moguet_candidate_available 5; or fail '-Syu lost --dry-run'
__moguet_candidate_available 18; or fail '-Syu lost --needed'
__moguet_candidate_available 4; or fail '-Syu lost --noconfirm'
__moguet_candidate_available 0; or fail '-Syu lost normal-AUR review options'
__moguet_candidate_available 11; and fail '-Syu exposed unsupported --aur'
__moguet_candidate_available 9; and fail '-Syu exposed unsupported --rmdeps'

set mock_words moguet -Syu --repo
__moguet_candidate_available 5; or fail '-Syu --repo lost --dry-run'
__moguet_candidate_available 0; and fail '-Syu --repo leaked AUR review options'
__moguet_candidate_available 11; and fail '-Syu --repo exposed --aur'

set mock_words moguet -Syu package
__moguet_candidate_available 18; or fail 'target-bearing -Syu lost delegated options'
__moguet_candidate_available 12; and fail 'target-bearing -Syu leaked exact RepoOnly selector'

set mock_words moguet add-src first V=1 second
__moguet_form_prefix_valid add-src 0; or fail 'add-src ordered multi-item form was closed'

set mock_words moguet revert first second
__moguet_candidate_available 4; or fail 'source-maintenance multi-target form was closed'

set mock_words moguet -Q
test (__moguet_operation) = __delegated__; or fail 'delegated operation was closed'
__moguet_candidate_available 20; and fail 'delegated grammar leaked --details'
__moguet_candidate_available 4; or fail 'delegated grammar lost --noconfirm'
FISH
    case_count=$((case_count + 1))
fi

printf 'Moguet static Bash/Zsh/Fish completion tests: %d scenarios passed\n' "${case_count}"
