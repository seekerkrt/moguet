#!/bin/sh
set -eu

repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
tmp_dir=$(mktemp -d)

cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

MOGUET_TEST_REPOSITORY_ROOT=$repo_root
export MOGUET_TEST_REPOSITORY_ROOT
. "$repo_root/tests/test-command-safety.sh"

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

expect_rejected() {
    case_name=$1
    shift
    status=0
    "$@" >/dev/null 2>&1 || status=$?
    if [ "$status" -ne 2 ]; then
        fail "$case_name returned $status instead of rejecting argv with status 2"
    fi
}

# The positive case proves that the helper accepts the committed regular,
# executable stub selected by PATH.
PATH=$repo_root/tests/stubs:/usr/bin:/bin
export PATH
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"

mkdir -p "$tmp_dir/unexpected" "$tmp_dir/non-executable" "$tmp_dir/symlink"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$tmp_dir/unexpected/pacman-conf"
chmod 755 "$tmp_dir/unexpected/pacman-conf"
if (
    PATH=$tmp_dir/unexpected:$PATH
    export PATH
    require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
    : > "$tmp_dir/binary-started"
) >/dev/null 2>&1; then
    fail 'unexpected PATH command was not rejected before binary startup'
fi
if [ -e "$tmp_dir/binary-started" ]; then
    fail 'test command ran past an unsafe command resolution check'
fi

printf '%s\n' '#!/bin/sh' 'exit 0' > "$tmp_dir/non-executable/pacman-conf"
chmod 644 "$tmp_dir/non-executable/pacman-conf"
if (
    MOGUET_TEST_CASE_STUB_ROOT=$tmp_dir/non-executable
    PATH=$tmp_dir/non-executable:/usr/bin:/bin
    export MOGUET_TEST_CASE_STUB_ROOT PATH
    require_exact_test_command pacman-conf \
        "$tmp_dir/non-executable/pacman-conf"
) >/dev/null 2>&1; then
    fail 'non-executable expected stub was not rejected before binary startup'
fi

ln -s /usr/bin/true "$tmp_dir/symlink/pacman-conf"
if (
    MOGUET_TEST_CASE_STUB_ROOT=$tmp_dir/symlink
    PATH=$tmp_dir/symlink:/usr/bin:/bin
    export MOGUET_TEST_CASE_STUB_ROOT PATH
    require_exact_test_command pacman-conf "$tmp_dir/symlink/pacman-conf"
) >/dev/null 2>&1; then
    fail 'stub symlink resolving outside its allowed root was not rejected'
fi

command_log=$tmp_dir/commands.log
: > "$command_log"
export MOGUET_TEST_COMMAND_LOG=$command_log
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
export MOGUET_TEST_PACMAN_EXIT_CODE=0
export MOGUET_TEST_SUDO_EXIT_CODE=0

makepkg_argv_log=$tmp_dir/makepkg-argv.log
expected_makepkg_argv_log=$tmp_dir/expected-makepkg-argv.log
: > "$makepkg_argv_log"
export MOGUET_TEST_MAKEPKG_ARGV_LOG=$makepkg_argv_log
if ! "$repo_root/tests/stubs/makepkg" --printsrcinfo \
    'FIRST=one value' EMPTY= 'FIRST=last=with=equals'; then
    fail 'makepkg stub rejected valid trailing environment assignments'
fi
printf '%s\n' \
    'argv-begin' \
    'arg[0]=<--printsrcinfo>' \
    'arg[1]=<FIRST=one value>' \
    'arg[2]=<EMPTY=>' \
    'arg[3]=<FIRST=last=with=equals>' \
    'argv-end' > "$expected_makepkg_argv_log"
if ! cmp -s "$expected_makepkg_argv_log" "$makepkg_argv_log"; then
    fail 'makepkg stub did not preserve exact trailing assignment argv'
fi

expect_rejected 'legacy makepkg install argv' \
    "$repo_root/tests/stubs/makepkg" -sic
expect_rejected 'makepkg --needed argv' \
    "$repo_root/tests/stubs/makepkg" -sc --needed
expect_rejected 'makepkg non-assignment suffix' \
    "$repo_root/tests/stubs/makepkg" --packagelist not-an-assignment
expect_rejected 'makepkg invalid assignment key suffix' \
    "$repo_root/tests/stubs/makepkg" --packagelist 9INVALID=value
expect_rejected 'makepkg option after assignment suffix' \
    "$repo_root/tests/stubs/makepkg" -sc VALID=value --noconfirm
expect_rejected 'arbitrary sudo command' \
    "$repo_root/tests/stubs/sudo" arbitrary-command --unexpected
expect_rejected 'unexpected pacman option through sudo' \
    "$repo_root/tests/stubs/sudo" pacman -S --unexpected target
expect_rejected 'unexpected sync pacman option through sudo' \
    "$repo_root/tests/stubs/commands-sync/sudo" \
        pacman -S --unexpected target
expect_rejected 'unexpected source-maintenance pacman option through sudo' \
    "$repo_root/tests/stubs/source-maintenance/sudo" \
        pacman -S --unexpected target
expect_rejected 'filesystem touch through generic sudo' \
    "$repo_root/tests/stubs/sudo" touch "$tmp_dir/preference"
expect_rejected 'filesystem tee through generic sudo' \
    "$repo_root/tests/stubs/sudo" tee -a "$tmp_dir/preference"
expect_rejected 'filesystem install through source-maintenance sudo' \
    "$repo_root/tests/stubs/source-maintenance/sudo" \
        install -Dm644 -- /dev/stdin "$tmp_dir/preference"
expect_rejected 'filesystem rm through source-maintenance sudo' \
    "$repo_root/tests/stubs/source-maintenance/sudo" \
        rm -f "$tmp_dir/preference"
expect_rejected 'unexpected read-only pacman option' \
    "$repo_root/tests/stubs/pacman" -Si target --unexpected
expect_rejected 'unexpected sync read-only pacman option' \
    "$repo_root/tests/stubs/commands-sync/pacman" -Si target --unexpected
expect_rejected 'identity query with extra argv' \
    "$repo_root/tests/stubs/pacman" -Qp --color never -- \
        "$tmp_dir/artifact" extra
expect_rejected 'typed install with two reason options' \
    "$repo_root/tests/stubs/sudo" pacman -U --asdeps --asexplicit -- \
        "$tmp_dir/artifact"

/usr/bin/python3 -I "$repo_root/tests/test-legacy-install-test-adapter.py"

printf '%s\n' 'test command safety and stub contracts: all checks passed'
