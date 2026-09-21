#!/usr/bin/env bash

set -u

repo_root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 1
. "$repo_root/scripts/validation-status.sh"
export LC_ALL=C

lanes=(capture clean build host container live diff-check)
declare -A states statuses heads staged unstaged hygiene
for lane in "${lanes[@]}"; do states[$lane]='NOT RUN'; done
for phase in before after; do
    heads[$phase]=UNKNOWN
    staged[$phase]=UNKNOWN
    unstaged[$phase]=UNKNOWN
    hygiene[$phase]='NOT RUN'
done
candidate='NOT RECHECKED'
current_lane=capture
primary_status=0
first_failure=NONE
failed_command=NONE
last_success=NONE
started=UNKNOWN
scratch=''

# Unset the inputs owned by the Make/CMake frontend, including the outer
# Make's exported default-options signal. Do not erase unrelated environment.
default_make=(env -u MAKEFLAGS -u MFLAGS -u GNUMAKEFLAGS -u MAKEOVERRIDES -u MAKEFILES
    -u CPPFLAGS -u CXXFLAGS -u LDFLAGS -u CCACHE
    -u MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS
    -u CXX -u CMAKE -u CTEST -u DOCKER make)
git_command=(git --no-optional-locks)
diff_options=(--binary --full-index --no-ext-diff --no-textconv
    --no-color --no-renames --ignore-submodules=none)

record_failure() {
    if (( primary_status == 0 )); then
        primary_status=$1
        first_failure=$current_lane
        failed_command=$2
        if [[ $current_lane != candidate ]]; then
            states[$current_lane]=FAIL
            statuses[$current_lane]=$1
        fi
    fi
}

run_command() {
    local command_text
    printf -v command_text '%q ' "$@"
    printf ':: %s command: %s\n' "$current_lane" "$command_text" >&2 || :
    validation_run_command "$@"
    if (( VALIDATION_COMMAND_STATUS != 0 )); then
        record_failure "$VALIDATION_COMMAND_STATUS" "$command_text"
        return "$VALIDATION_COMMAND_STATUS"
    fi
}

capture_candidate() {
    local phase=$1 _ignored
    run_command "${git_command[@]}" rev-parse --verify 'HEAD^{commit}' \
        >"$scratch/head" || return $?
    IFS= read -r "heads[$phase]" <"$scratch/head" || return $?

    # Finish each producer before hashing: partial stdout is never evidence.
    run_command "${git_command[@]}" diff --cached "${diff_options[@]}" HEAD -- \
        >"$scratch/diff" || return $?
    run_command sha256sum <"$scratch/diff" >"$scratch/digest" || return $?
    read -r "staged[$phase]" _ignored <"$scratch/digest" || return $?
    run_command "${git_command[@]}" diff "${diff_options[@]}" -- \
        >"$scratch/diff" || return $?
    run_command sha256sum <"$scratch/diff" >"$scratch/digest" || return $?
    read -r "unstaged[$phase]" _ignored <"$scratch/digest" || return $?

    # NUL data stays in a file. Untracked input is hygiene, not identity.
    run_command "${git_command[@]}" ls-files --others --exclude-standard -z \
        >"$scratch/untracked" || return $?
    if [[ -s $scratch/untracked ]]; then
        hygiene[$phase]=FAIL
        record_failure 1 'source hygiene: non-ignored untracked files'
        printf 'release-validate: non-ignored untracked files are present\n' >&2 || :
        return 1
    fi
    hygiene[$phase]=PASS
}

run_lane() {
    current_lane=$1
    shift
    run_command "$@" || return $?
    states[$current_lane]=PASS
    statuses[$current_lane]=0
    last_success=$current_lane
}

print_summary() {
    local finished lane result=INVALID
    finished=$(date -u '+%Y-%m-%dT%H:%M:%SZ') || return $?
    if (( primary_status == 0 )) && [[ $candidate == UNCHANGED ]]; then
        result=VALID
    fi
    printf '\nRELEASE VALIDATION\nScope: automated RC validation lanes only\n\n' || return $?
    printf 'candidate HEAD: %s\nstarted: %s\nfinished: %s\n' \
        "${heads[before]}" "$started" "$finished" || return $?
    printf 'before staged: %s\nbefore unstaged: %s\nafter HEAD: %s\nafter staged: %s\nafter unstaged: %s\n' \
        "${staged[before]}" "${unstaged[before]}" "${heads[after]}" \
        "${staged[after]}" "${unstaged[after]}" || return $?
    printf 'untracked hygiene: before=%s after=%s\ndefault profile:' \
        "${hygiene[before]}" "${hygiene[after]}" || return $?
    printf ' %q' "${default_make[@]}" || return $?
    printf '\n\n' || return $?
    for lane in "${lanes[@]}"; do
        printf '%s: %s' "$lane" "${states[$lane]}" || return $?
        if [[ ${statuses[$lane]+present} ]]; then
            printf ' (status %s)' "${statuses[$lane]}" || return $?
        fi
        printf '\n' || return $?
    done
    printf 'candidate: %s\nfirst failure: %s\nfirst failure status: %s\nfirst failure command: %s\nlast successful lane: %s\n\nRELEASE CANDIDATE: %s\n' \
        "$candidate" "$first_failure" "$primary_status" "$failed_command" \
        "$last_success" "$result"
}

finish() {
    local exit_status=$1 summary_status
    trap - EXIT
    # Do not let summary's closed pipe replace an observed child failure.
    # Keep the child lanes' own signal behavior unchanged.
    trap '' PIPE
    if (( exit_status != 0 && primary_status == 0 )); then
        record_failure "$exit_status" 'orchestration setup / capture failure'
        [[ $current_lane != capture && $current_lane != candidate ]] || candidate=ERROR
    fi
    if print_summary; then
        :
    else
        summary_status=$?
        printf 'release-validate: summary output failed (status %s)\n' "$summary_status" >&2 || :
        (( primary_status != 0 )) || primary_status=$summary_status
    fi
    if [[ -n $scratch ]]; then
        rm -rf -- "$scratch" >/dev/null 2>&1 || :
    fi
    exit "$primary_status"
}

trap 'finish "$?"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
main() {
    local scratch_root
    cd -- "$repo_root" || return $?
    started=$(date -u '+%Y-%m-%dT%H:%M:%SZ') || return $?
    # Kept outside the source/build tree so clean cannot remove the evidence.
    scratch_root=$(cd -- "${TMPDIR:-/tmp}" && pwd -P) || return $?
    case $scratch_root in "$repo_root"|"$repo_root"/*) scratch_root=/tmp ;; esac
    scratch=$(mktemp -d "$scratch_root/moguet-release-validate.XXXXXX") || return $?
    capture_candidate before || { candidate=ERROR; return 1; }
    states[capture]=PASS
    statuses[capture]=0
    last_success=capture

    run_lane clean "${default_make[@]}" clean || return $?
    run_lane build "${default_make[@]}" -j8 --output-sync=target || return $?
    run_lane host "${default_make[@]}" -j8 --output-sync=target test-host-release || return $?
    run_lane container "${default_make[@]}" test-container || return $?
    run_lane live "${default_make[@]}" test-container-live || return $?
    current_lane=diff-check
    run_command "${git_command[@]}" diff --check || return $?
    run_lane diff-check "${git_command[@]}" diff --cached --check || return $?

    current_lane=candidate
    capture_candidate after || { candidate=ERROR; return 1; }
    if [[ ${heads[before]} != "${heads[after]}" ||
          ${staged[before]} != "${staged[after]}" ||
          ${unstaged[before]} != "${unstaged[after]}" ]]; then
        candidate=CHANGED
        record_failure 1 'candidate identity comparison'
        return 1
    fi
    candidate=UNCHANGED
}

main
finish "$?"
