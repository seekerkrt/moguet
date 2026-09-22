#!/usr/bin/env bash

set -euo pipefail

repo_root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
real_git=$(command -v git)
real_make=$(command -v make)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/moguet-release-test.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export MOGUET_TEST_RELEASE_GIT=$real_git
mkdir -p "$scratch/bin" "$scratch/template"

fail() {
    printf 'release-validate-test: %s\n' "$*" >&2
    [[ ! -f ${output:-} ]] || cat "$output" >&2
    [[ ! -f ${output:-}.stderr ]] || cat "$output.stderr" >&2
    exit 1
}

assert_contains() {
    grep -F -- "$1" "$output" >/dev/null || fail "missing: $1"
}

# This double never forwards to real make. Its only mutations are in each
# temporary Git fixture, and the log is outside that fixture's source tree.
cat >"$scratch/bin/make" <<'MAKE'
#!/usr/bin/env bash
set -eu
case "$*" in
    clean) lane=clean ;;
    '-j8 --output-sync=target') lane=build ;;
    '-j8 --output-sync=target test-host-release') lane=host ;;
    test-container) lane=container ;;
    test-container-live) lane=live ;;
    *) printf 'unexpected make argv: %s\n' "$*" >&2; exit 90 ;;
esac
printf '%s\n' "$lane" >>"$MOGUET_TEST_RELEASE_LOG"
for name in MAKEFLAGS MFLAGS GNUMAKEFLAGS MAKEOVERRIDES MAKEFILES CPPFLAGS CXXFLAGS LDFLAGS \
    CCACHE MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS CXX CMAKE CTEST DOCKER; do
    [[ ! ${!name+x} ]] || { printf 'leaked input: %s\n' "$name" >&2; exit 91; }
done
if [[ $lane == host ]]; then
    case ${MOGUET_TEST_RELEASE_MUTATION:-} in
        staged) "$MOGUET_TEST_RELEASE_GIT" add -- source ;;
        unstaged) printf 'new content\n' >> source ;;
        head) "$MOGUET_TEST_RELEASE_GIT" -c commit.gpgSign=false commit -q --allow-empty -m next ;;
        untracked) printf 'new source\n' >untracked ;;
        ignored) mkdir -p build; printf 'artifact\n' >build/output ;;
        canonical) printf 'initial\r\n' >source ;;
    esac
fi
if [[ ${MOGUET_TEST_RELEASE_FAIL:-} == "$lane" ]]; then exit 41; fi
printf 'expected negative diagnostic: FAIL error (fixture only)\n' || :
MAKE

cat >"$scratch/bin/git" <<'GIT'
#!/usr/bin/env bash
set -eu
case " $* " in
    *' rev-parse '*) step=head ;;
    *' --cached --check '*) step=diff-cached ;;
    *' --check '*) step=diff-check ;;
    *' --cached '*) step=staged ;;
    *' diff '*) step=unstaged ;;
    *' ls-files '*) step=untracked ;;
    *) printf 'unexpected git argv: %s\n' "$*" >&2; exit 92 ;;
esac
printf '%s\n' "$step" >>"$MOGUET_TEST_RELEASE_LOG"
if [[ ${MOGUET_TEST_RELEASE_FAIL:-} == "$step" ]] ||
   { [[ ${MOGUET_TEST_RELEASE_FAIL:-} == recheck && $step == staged ]] &&
     grep -Fx diff-cached "$MOGUET_TEST_RELEASE_LOG" >/dev/null; }; then
    printf 'partial producer output\n'
    exit 43
fi
exec "$MOGUET_TEST_RELEASE_GIT" "$@"
GIT
chmod 755 "$scratch/bin/make" "$scratch/bin/git"

new_fixture() {
    fixture=$scratch/$1
    mkdir -p "$fixture/scripts"
    cp "$repo_root/scripts/release-validate.sh" "$repo_root/scripts/validation-status.sh" \
        "$fixture/scripts/"
    "$real_git" init -q --template="$scratch/template" "$fixture"
    "$real_git" -C "$fixture" config user.name 'Release validation fixture'
    "$real_git" -C "$fixture" config user.email 'release-validation@example.invalid'
    "$real_git" -C "$fixture" config core.hooksPath /dev/null
    printf 'build/\n' >"$fixture/.gitignore"
    printf '* text=auto eol=lf\n' >"$fixture/.gitattributes"
    printf 'initial\n' >"$fixture/source"
    "$real_git" -C "$fixture" add -- .
    "$real_git" -C "$fixture" -c commit.gpgSign=false commit -qm initial
    output=$scratch/$1.output
    export MOGUET_TEST_RELEASE_LOG=$scratch/$1.commands
    : >"$MOGUET_TEST_RELEASE_LOG"
    unset MOGUET_TEST_RELEASE_FAIL MOGUET_TEST_RELEASE_MUTATION
}

run_fixture() {
    local expected=$1 destination=${2:-$output} actual=0
    env PATH="$scratch/bin:$PATH" TMPDIR="$scratch" \
        MAKEFLAGS=-ikn MFLAGS=-ikn GNUMAKEFLAGS=-ikn MAKEOVERRIDES=unexpected MAKEFILES=missing \
        CPPFLAGS=-DUNEXPECTED CXXFLAGS= LDFLAGS=-fuse-ld=unexpected CCACHE=unexpected \
        MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS=OFF \
        CXX=unexpected CMAKE=unexpected CTEST=unexpected DOCKER=unexpected \
        bash "$fixture/scripts/release-validate.sh" >"$destination" 2>"$output.stderr" || actual=$?
    [[ $actual == "$expected" ]] || fail "expected status $expected, got $actual"
}

expected_order=$'head\nstaged\nunstaged\nuntracked\nclean\nbuild\nhost\ncontainer\nlive\ndiff-check\ndiff-cached\nhead\nstaged\nunstaged\nuntracked'

new_fixture happy
# Dirty tracked candidates are supported, including both diff components.
printf 'staged\n' >>"$fixture/source"
"$real_git" -C "$fixture" add -- source
printf 'unstaged\n' >>"$fixture/source"
index_before=$(sha256sum <"$fixture/.git/index")
source_before=$(sha256sum <"$fixture/source")
run_fixture 0
[[ $(cat "$MOGUET_TEST_RELEASE_LOG") == "$expected_order" ]] || fail 'command order changed'
for lane in capture clean build host container live diff-check; do
    assert_contains "$lane: PASS (status 0)"
done
assert_contains 'candidate: UNCHANGED'
assert_contains 'RELEASE CANDIDATE: VALID'
assert_contains 'expected negative diagnostic: FAIL error'
assert_contains 'default profile: env -u MAKEFLAGS'
[[ $(sha256sum <"$fixture/.git/index") == "$index_before" ]] || fail 'capture changed the index'
[[ $(sha256sum <"$fixture/source") == "$source_before" ]] || fail 'capture changed source'

# One matrix covers every first-failure boundary; the command prefix proves
# that no later lane was entered, including actual-live's aggregate boundary.
for failed in clean build host container live diff-check diff-cached; do
    new_fixture "failure-$failed"
    export MOGUET_TEST_RELEASE_FAIL=$failed
    expected_status=41
    [[ $failed != diff-* ]] || expected_status=43
    run_fixture "$expected_status"
    failed_lane=$failed
    [[ $failed != diff-cached ]] || failed_lane=diff-check
    assert_contains "$failed_lane: FAIL (status $expected_status)"
    assert_contains "first failure: $failed_lane"
    assert_contains "first failure status: $expected_status"
    assert_contains 'candidate: NOT RECHECKED'
    assert_contains 'RELEASE CANDIDATE: INVALID'
    expected_prefix=''
    while IFS= read -r step; do
        expected_prefix+="${expected_prefix:+$'\n'}$step"
        [[ $step != "$failed" ]] || break
    done <<<"$expected_order"
    [[ $(cat "$MOGUET_TEST_RELEASE_LOG") == "$expected_prefix" ]] || fail "continued after $failed"
    later=false
    for lane in clean build host container live diff-check; do
        if $later; then assert_contains "$lane: NOT RUN"; fi
        [[ $lane != "$failed_lane" ]] || later=true
    done
done

for mutation in staged unstaged head; do
    new_fixture "mutation-$mutation"
    if [[ $mutation == staged ]]; then printf 'dirty\n' >>"$fixture/source"; fi
    export MOGUET_TEST_RELEASE_MUTATION=$mutation
    run_fixture 1
    assert_contains 'candidate: CHANGED'
    assert_contains 'RELEASE CANDIDATE: INVALID'
done

new_fixture untracked-start
printf 'new\n' >"$fixture/untracked"
run_fixture 1
assert_contains 'capture: FAIL'
assert_contains 'clean: NOT RUN'
assert_contains 'untracked hygiene: before=FAIL'
[[ $(cat "$MOGUET_TEST_RELEASE_LOG") == $'head\nstaged\nunstaged\nuntracked' ]] || fail 'started with untracked source'
[[ -f $fixture/untracked ]] || fail 'removed untracked source'

new_fixture untracked-after
export MOGUET_TEST_RELEASE_MUTATION=untracked
run_fixture 1
assert_contains 'untracked hygiene: before=PASS after=FAIL'
assert_contains 'RELEASE CANDIDATE: INVALID'

# Git-canonical equivalent text and ignored generated output are not mutations.
for mutation in ignored canonical; do
    new_fixture "$mutation"
    export MOGUET_TEST_RELEASE_MUTATION=$mutation
    run_fixture 0
    assert_contains 'candidate: UNCHANGED'
done

for failure in staged recheck; do
    new_fixture "capture-$failure"
    export MOGUET_TEST_RELEASE_FAIL=$failure
    run_fixture 43
    assert_contains 'candidate: ERROR'
    assert_contains 'first failure status: 43'
    assert_contains 'RELEASE CANDIDATE: INVALID'
    if [[ $failure == staged ]]; then
        assert_contains 'clean: NOT RUN'
        [[ $(cat "$MOGUET_TEST_RELEASE_LOG") == $'head\nstaged' ]] || fail 'continued after partial capture'
    fi
done

# /dev/full fails real summary writes without introducing a production hook.
new_fixture summary-primary
export MOGUET_TEST_RELEASE_FAIL=host
run_fixture 41 /dev/full
grep -F 'summary output failed' "$output.stderr" >/dev/null || fail 'summary failure was not exercised'
new_fixture summary-only
run_fixture 1 /dev/full

# Exercise the real Make entry, but all child makes still hit our double.
# Copying the actual frontend avoids a second recipe oracle in this test.
new_fixture entrypoint
cp "$repo_root/Makefile" "$fixture/Makefile"
printf '0.0.0\n' >"$fixture/VERSION"
"$real_git" -C "$fixture" add -- Makefile VERSION
"$real_git" -C "$fixture" -c commit.gpgSign=false commit -qm frontend
env -u MAKEFLAGS -u MFLAGS -u MAKEOVERRIDES -u MAKEFILES \
    PATH="$scratch/bin:$PATH" TMPDIR="$scratch" \
    "$real_make" -C "$fixture" -n release-validate >"$output" 2>&1
[[ ! -s $MOGUET_TEST_RELEASE_LOG ]] || fail 'make -n executed validation'
env -u MAKEFLAGS -u MFLAGS -u MAKEOVERRIDES -u MAKEFILES \
    PATH="$scratch/bin:$PATH" TMPDIR="$scratch" \
    "$real_make" -C "$fixture" release-validate >"$output" 2>&1
assert_contains 'RELEASE CANDIDATE: VALID'
[[ $(cat "$MOGUET_TEST_RELEASE_LOG") == "$expected_order" ]] || fail 'Make entry changed command order'

printf 'release-validate-test: all scenarios passed\n'
