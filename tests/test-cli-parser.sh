#!/bin/sh
set -eu

# Assertions target the canonical untranslated CLI output.
# Do not inherit locale settings from the invoking environment.
LANG=C
LC_ALL=C
export LANG LC_ALL
unset LANGUAGE

test_binary=$1
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
MOGUET_TEST_REPOSITORY_ROOT=$repo_root
export MOGUET_TEST_REPOSITORY_ROOT
. "$repo_root/tests/test-command-safety.sh"
. "$repo_root/scripts/validation-status.sh"
tmp_dir=$(mktemp -d)
server_pid=

cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

port_file=$tmp_dir/port
python3 "$repo_root/tests/aur_rpc_fixture_server.py" \
    "$repo_root/tests/fixtures/aur-rpc-conflicts-replaces.json" "$port_file" &
server_pid=$!

attempt=0
while [ ! -s "$port_file" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -gt 100 ]; then
        echo "fixture server did not start" >&2
        exit 1
    fi
    sleep 0.05
done

port=$(cat "$port_file")
export PATH=$repo_root/tests/stubs:/usr/bin:/bin
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
require_exact_test_command makepkg "$repo_root/tests/stubs/makepkg"
require_exact_test_command pacman "$repo_root/tests/stubs/pacman"
require_exact_test_command sudo "$repo_root/tests/stubs/sudo"
require_exact_test_command git "$repo_root/tests/stubs/git"
export MOGUET_TEST_AUR_RPC_BASE_URL=http://127.0.0.1:$port/rpc/

setup_case() {
    case_name=$1
    case_dir=$tmp_dir/cases/$case_name
    command_log=$case_dir/commands.log
    output_file=$case_dir/output
    repository_metadata_state=$case_dir/repository-metadata.state
    foreign_inventory_state=$case_dir/foreign-inventory.state

    mkdir -p \
        "$case_dir/home" \
        "$case_dir/xdg-config" \
        "$case_dir/xdg-state" \
        "$case_dir/xdg-cache"
    chmod 0700 "$case_dir/xdg-config"
    : > "$command_log"
    : > "$repository_metadata_state"
    : > "$foreign_inventory_state"
    export HOME=$case_dir/home
    export XDG_CONFIG_HOME=$case_dir/xdg-config
    export XDG_STATE_HOME=$case_dir/xdg-state
    export XDG_CACHE_HOME=$case_dir/xdg-cache
    export MOGUET_TEST_COMMAND_LOG=$command_log
    export MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE=$repository_metadata_state
    export MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE=$foreign_inventory_state
    export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
    export MOGUET_TEST_PACMAN_EXIT_CODE=0
    export MOGUET_TEST_SUDO_EXIT_CODE=0
    unset MOGUET_TEST_PACMAN_QM_OUTPUT
    unset MOGUET_TEST_PACMAN_REPO_PACKAGES
    unset MOGUET_TEST_GIT_REMOTE_URL
    unset MOGUET_TEST_GIT_CLONE_EXIT_CODE
    unset MOGUET_TEST_GIT_CLONE_SYMLINK_TARGET
    unset MOGUET_TEST_GIT_CLONE_FIXTURE_DIR
    unset MOGUET_TEST_MAKEPKG_EXIT_CODE
    unset MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE
    unset MOGUET_TEST_MAKEPKG_PACKAGELIST_OUTPUT_FILE
    unset PKGDEST
}

write_repository_package() {
    package=$1
    printf 'core %s 1 1\n' "$package" >> "$repository_metadata_state"
}

run_ok() {
    : > "$command_log"
    if ! "$test_binary" "$@" </dev/null > "$output_file" 2>&1; then
        echo "expected command to succeed: $*" >&2
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_fail() {
    : > "$command_log"
    if ! validation_expect_status cli-parser-business-failure 1 \
        "$output_file" "$output_file" "$test_binary" "$@" </dev/null; then
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_contains() {
    pattern=$1
    file=$2
    if ! grep -F -- "$pattern" "$file" >/dev/null; then
        echo "missing expected output: $pattern" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_not_contains() {
    pattern=$1
    file=$2
    if grep -F -- "$pattern" "$file" >/dev/null; then
        echo "unexpected output: $pattern" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_command() {
    expected=$1
    if ! grep -Fx -- "$expected" "$command_log" >/dev/null; then
        echo "missing expected command: $expected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_count() {
    expected=$1
    expected_count=$2
    actual_count=$(validation_grep_count -Fxc -- "$expected" "$command_log")
    if [ "$actual_count" -ne "$expected_count" ]; then
        echo "unexpected command count for: $expected" >&2
        echo "expected $expected_count, got $actual_count" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_only_command() {
    expected=$1
    assert_command "$expected"
    if [ "$(wc -l < "$command_log")" -ne 1 ]; then
        echo "unexpected additional command(s)" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_absent() {
    unexpected=$1
    if grep -Fx -- "$unexpected" "$command_log" >/dev/null; then
        echo "unexpected command: $unexpected" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_command_log_empty() {
    if [ -s "$command_log" ]; then
        echo "external command ran before CLI validation completed" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_storage_roots_absent() {
    if [ -e "$XDG_CACHE_HOME/moguet" ] || [ -L "$XDG_CACHE_HOME/moguet" ]; then
        echo "default cache/log initialization ran before entry validation completed" >&2
        find "$XDG_CACHE_HOME/moguet" -maxdepth 2 -print >&2 || true
        exit 1
    fi
    for directory in \
        "$XDG_CONFIG_HOME/moguet" \
        "$XDG_STATE_HOME/moguet" \
        "$XDG_CACHE_HOME/moguet"
    do
        if [ -e "$directory" ] || [ -L "$directory" ]; then
            echo "XDG directory preparation ran before entry validation completed" >&2
            find "$directory" -maxdepth 2 -print >&2 || true
            exit 1
        fi
    done
}

assert_pre_log_exit() {
    assert_command_log_empty
    assert_not_contains "Started Moguet v" "$output_file"
    assert_storage_roots_absent
}

assert_no_mutation_commands() {
    if grep -E '^(sudo|git|makepkg) |^pacman -S( |$)' "$command_log" >/dev/null; then
        echo "CLI validation allowed an external mutation command" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

assert_only_sudo_command() {
    expected=$1
    assert_command_count "$expected" 1
    sudo_count=$(validation_grep_count -c '^sudo ' "$command_log")
    if [ "$sudo_count" -ne 1 ]; then
        echo "unexpected additional sudo command(s)" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

run_exact() {
    case_name=$1
    expected=$2
    shift 2
    setup_case "$case_name"
    run_ok "$@"
    assert_only_command "$expected"
}

# Matrix A: option value待ちはglobal option認識より優先する。
run_exact value-root-rmdeps \
    "pacman -Q --root --rmdeps filesystem" \
    -Q --root --rmdeps filesystem
run_exact value-root-select \
    "pacman -Q --root --select filesystem" \
    -Q --root --select filesystem
run_exact value-root-dry-run \
    "pacman -Q --root --dry-run filesystem" \
    -Q --root --dry-run filesystem
run_exact value-config-noconfirm \
    "pacman -Q --config --noconfirm filesystem" \
    -Q --config --noconfirm filesystem
run_exact value-dbpath-rebuild \
    "pacman -Q --dbpath --rebuild filesystem" \
    -Q --dbpath --rebuild filesystem
run_exact value-cachedir-cleanbuild \
    "pacman -Q --cachedir --cleanbuild filesystem" \
    -Q --cachedir --cleanbuild filesystem
run_exact value-short-dbpath-noedit \
    "pacman -Q -b --noedit filesystem" \
    -Q -b --noedit filesystem
run_exact value-short-root-nodiff \
    "pacman -Q -r --nodiff filesystem" \
    -Q -r --nodiff filesystem
run_exact value-root-edit \
    "pacman -Q --root --edit filesystem" \
    -Q --root --edit filesystem
run_exact value-config-diff \
    "pacman -Q --config --diff filesystem" \
    -Q --config --diff filesystem
run_exact value-dbpath-build-mode \
    "pacman -Q --dbpath --build-mode=clean filesystem" \
    -Q --dbpath --build-mode=clean filesystem
run_exact invalid-build-mode-as-option-value \
    "pacman -Q --root --build-mode=cleanbuild filesystem" \
    -Q --root --build-mode=cleanbuild filesystem

# Parser分離時のtable移し忘れを検出するため、未coverageのvalue-taking long optionを全件固定する。
for value_option in \
    --arch --assume-installed --color --gpgdir --hookdir --ignore \
    --ignoregroup --logfile --overwrite --print-format --sysroot
do
    case_name=value-${value_option#--}-rmdeps
    run_exact "$case_name" \
        "pacman -Q $value_option --rmdeps filesystem" \
        -Q "$value_option" --rmdeps filesystem
done

# Matrix B: semantic `--`後は全tokenをopaque operandとして保持する。
for global_option in \
    --rmdeps --select --noconfirm --dry-run --edit --noedit --diff --nodiff \
    --build-mode=normal --build-mode=rebuild --build-mode=clean \
    --rebuild --cleanbuild; do
    case_name=opaque-${global_option#--}
    run_exact "$case_name" "sudo pacman -U -- $global_option" -U -- "$global_option"
done
run_exact opaque-conflicting-review-values \
    "sudo pacman -U -- --edit --noedit" \
    -U -- --edit --noedit
run_exact opaque-invalid-build-mode \
    "sudo pacman -U -- --build-mode=cleanbuild" \
    -U -- --build-mode=cleanbuild

# Matrix B2: --dry-run is a global modifier only in semantic global
# positions. Supported owned routes reach production projection before all
# persistent state, while unsupported routes fail closed instead of falling
# through to pacman or Git.
assert_dry_run_rendered() {
    case_name=$1
    shift

    setup_case "$case_name"
    export MOGUET_TEST_PACMAN_REPO_PACKAGES=official-only
    export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
    if "$test_binary" "$@" </dev/null > "$output_file" 2>&1; then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne 0 ] && [ "$status" -ne 1 ]; then
        echo "unexpected dry-run status $status: $*" >&2
        sed -n '1,240p' "$output_file" >&2
        cat "$command_log" >&2
        exit 1
    fi
    if ! grep -Fx -- "Unified plan:" "$output_file" >/dev/null &&
       ! grep -Fx -- "System + normal AUR update plan:" "$output_file" >/dev/null
    then
        echo "supported dry-run did not render a recognized plan" >&2
        sed -n '1,240p' "$output_file" >&2
        exit 1
    fi
    assert_not_contains "Started Moguet v" "$output_file"
    assert_storage_roots_absent
    assert_no_mutation_commands
}

assert_dry_run_rejected() {
    case_name=$1
    shift

    setup_case "$case_name"
    run_fail "$@"
    assert_contains "Option --dry-run is not supported for operation" \
        "$output_file"
    assert_pre_log_exit
}

assert_dry_run_rendered dry-run-before-operation \
    --dry-run --aur -S clean-root
assert_dry_run_rendered dry-run-after-operation \
    fetch clean-root --dry-run
assert_dry_run_rendered dry-run-duplicate \
    --dry-run build clean-root --dry-run
assert_dry_run_rendered dry-run-sync-system-update \
    -Syu --dry-run

setup_case dry-run-local-build
local_dry_run_source=$case_dir/local-source
cp -a "$repo_root/tests/fixtures/unified-plan-local-blocked" \
    "$local_dry_run_source"
touch "$local_dry_run_source/.SRCINFO"
if ! "$test_binary" build --local "$local_dry_run_source" --dry-run \
        </dev/null > "$output_file" 2>&1; then
    echo "supported local dry-run did not succeed" >&2
    sed -n '1,240p' "$output_file" >&2
    cat "$command_log" >&2
    exit 1
fi
assert_contains "Unified plan:" "$output_file"
assert_not_contains "Started Moguet v" "$output_file"
assert_storage_roots_absent
assert_no_mutation_commands

assert_dry_run_rendered dry-run-upgrade upgrade --dry-run
assert_dry_run_rendered dry-run-upgrade-aur upgrade-aur --dry-run
assert_dry_run_rendered dry-run-upgrade-all upgrade-all --dry-run

assert_dry_run_rejected dry-run-deps deps clean-root --dry-run
assert_dry_run_rejected dry-run-plan plan clean-root --dry-run
assert_dry_run_rejected dry-run-search -Ss clean-root --dry-run
assert_dry_run_rejected dry-run-info -Si clean-root --dry-run
assert_dry_run_rejected dry-run-clean clean --dry-run
assert_dry_run_rejected dry-run-pkgbuild-export -G clean-root --dry-run
assert_dry_run_rejected dry-run-pkgbuild-print -Gp clean-root --dry-run
assert_dry_run_rejected dry-run-source-maintenance \
    add-src clean-root --dry-run
assert_dry_run_rejected dry-run-query-passthrough -Q --dry-run clean-root
assert_dry_run_rejected dry-run-remove-passthrough -R clean-root --dry-run
assert_dry_run_rejected dry-run-upgrade-passthrough -U clean-root --dry-run
assert_dry_run_rejected dry-run-database-passthrough -D clean-root --dry-run
assert_dry_run_rejected dry-run-files-passthrough -F clean-root --dry-run
assert_dry_run_rejected dry-run-deptest-passthrough -T clean-root --dry-run
assert_dry_run_rejected dry-run-unsupported-sync -Sc --dry-run
assert_dry_run_rejected dry-run-unknown-sync-modifier -Sz clean-root --dry-run
assert_dry_run_rejected dry-run-targetless-sync --dry-run -S
assert_dry_run_rejected dry-run-repo-passthrough \
    --dry-run --repo -S official-only
assert_dry_run_rejected dry-run-separate-sysupgrade \
    --dry-run -S --sysupgrade
assert_dry_run_rejected dry-run-sync-pacman-option \
    --dry-run -S --config custom.conf clean-root
assert_dry_run_rejected dry-run-sync-end-of-options \
    --dry-run -S -- clean-root
setup_case dry-run-targetless-fetch
run_fail --dry-run fetch
assert_contains "Invalid: Usage: moguet fetch <pkg>..." "$output_file"
assert_pre_log_exit
assert_dry_run_rejected dry-run-fetch-pacman-option \
    --dry-run fetch --needed clean-root
assert_dry_run_rejected dry-run-fetch-end-of-options \
    --dry-run fetch -- clean-root
assert_dry_run_rejected dry-run-build-pacman-option \
    --dry-run build --needed clean-root
assert_dry_run_rejected dry-run-build-end-of-options \
    --dry-run build -- clean-root
assert_dry_run_rejected dry-run-local-build-extra-option \
    --dry-run build --local . --needed
assert_dry_run_rejected dry-run-upgrade-end-of-options \
    --dry-run upgrade --

# Matrix C/D: 通常位置のglobalだけを消費し、generated optionはoperation直後へ1件置く。
run_exact global-leading-noconfirm \
    "pacman -Q --noconfirm filesystem" \
    --noconfirm -Q filesystem
run_exact global-trailing-noconfirm \
    "pacman -Q --noconfirm filesystem" \
    -Q filesystem --noconfirm
global_option_index=0
for global_option in \
    --edit --noedit --diff --nodiff \
    --build-mode=normal --build-mode=rebuild --build-mode=clean \
    --rebuild --cleanbuild --rmdeps; do
    global_option_index=$((global_option_index + 1))
    run_exact "global-option-$global_option_index" \
        "pacman -Q filesystem" \
        "$global_option" -Q filesystem
done
run_exact global-prompt-duplicates \
    "pacman -Q filesystem" \
    --edit --edit --diff --diff -Q filesystem
run_exact global-skip-duplicates \
    "pacman -Q filesystem" \
    --noedit --noedit --nodiff --nodiff -Q filesystem
run_exact global-normal-duplicates \
    "pacman -Q filesystem" \
    --build-mode=normal --build-mode=normal -Q filesystem
run_exact global-rebuild-equivalent-duplicates \
    "pacman -Q filesystem" \
    --build-mode=rebuild --rebuild --rebuild -Q filesystem
run_exact global-clean-equivalent-duplicates \
    "pacman -Q filesystem" \
    --build-mode=clean --cleanbuild --cleanbuild -Q filesystem
run_exact option-value-does-not-conflict-with-global \
    "pacman -Q --root --edit filesystem" \
    -Q --root --edit --noedit filesystem
run_exact generated-before-marker-leading \
    "pacman -Q --noconfirm -- filesystem" \
    --noconfirm -Q -- filesystem
run_exact generated-before-marker-trailing \
    "pacman -Q --noconfirm -- filesystem" \
    -Q --noconfirm -- filesystem
run_exact generated-distinct-from-option-value \
    "pacman -Q --noconfirm --root --noconfirm filesystem" \
    --noconfirm -Q --root --noconfirm filesystem
run_exact generated-distinct-from-opaque-token \
    "pacman -Q --noconfirm -- filesystem --noconfirm" \
    --noconfirm -Q -- filesystem --noconfirm

# Matrix E: separated formだけが次tokenをvalueとして消費する。
run_exact separated-root \
    "pacman -Q --root value filesystem" \
    -Q --root value filesystem
run_exact attached-root \
    "pacman -Q --root=value filesystem" \
    -Q --root=value filesystem
run_exact separated-config \
    "pacman -Q --config value filesystem" \
    -Q --config value filesystem
run_exact attached-config \
    "pacman -Q --config=value filesystem" \
    -Q --config=value filesystem

# Matrix F: tracked value optionのmissing valueはexternal command前に停止する。
missing_index=0
for missing_option in --root --config -b -r; do
    missing_index=$((missing_index + 1))
    setup_case missing-value-$missing_index
    run_fail -Q "$missing_option"
    assert_contains "Missing value for option" "$output_file"
    assert_contains "$missing_option" "$output_file"
    assert_pre_log_exit
done

# Matrix G: value位置の`--`はmarkerではなくvalue。次の`--`だけがmarkerになる。
run_exact marker-as-root-value \
    "pacman -Q --root -- filesystem" \
    -Q --root -- filesystem
run_exact marker-after-root-value \
    "pacman -Q --root -- -- filesystem" \
    -Q --root -- -- filesystem

# Matrix H: typed final-value overrideは同じsettingへ異なる値を要求した時点で拒否する。
assert_cli_override_conflict() {
    case_name=$1
    setting=$2
    first_option=$3
    second_option=$4

    setup_case "$case_name"
    run_fail "$first_option" "$second_option" -Q filesystem
    assert_contains "Conflicting CLI overrides for $setting" "$output_file"
    assert_pre_log_exit
}

assert_cli_override_conflict conflict-edit-noedit \
    review.pkgbuild --edit --noedit
assert_cli_override_conflict conflict-noedit-edit \
    review.pkgbuild --noedit --edit
assert_cli_override_conflict conflict-diff-nodiff \
    review.diff --diff --nodiff
assert_cli_override_conflict conflict-nodiff-diff \
    review.diff --nodiff --diff
assert_cli_override_conflict conflict-normal-rebuild \
    build.mode --build-mode=normal --build-mode=rebuild
assert_cli_override_conflict conflict-rebuild-normal \
    build.mode --rebuild --build-mode=normal
assert_cli_override_conflict conflict-normal-clean \
    build.mode --build-mode=normal --cleanbuild
assert_cli_override_conflict conflict-clean-normal \
    build.mode --build-mode=clean --build-mode=normal
assert_cli_override_conflict conflict-rebuild-clean \
    build.mode --rebuild --cleanbuild
assert_cli_override_conflict conflict-clean-rebuild \
    build.mode --cleanbuild --build-mode=rebuild

setup_case bare-build-mode
run_fail --build-mode -Q filesystem
assert_contains "Option --build-mode requires an attached value" "$output_file"
assert_pre_log_exit

invalid_build_mode_index=0
for invalid_build_mode in --build-mode= --build-mode=cleanbuild; do
    invalid_build_mode_index=$((invalid_build_mode_index + 1))
    setup_case "invalid-build-mode-$invalid_build_mode_index"
    run_fail "$invalid_build_mode" -Q filesystem
    assert_contains "Invalid value for --build-mode" "$output_file"
    assert_pre_log_exit
done

# Matrix I: direct route / file routeでも非global argvの相対順を維持する。
run_exact query-relative-order \
    "pacman -Q target-a --config custom.conf target-b" \
    -Q target-a --config custom.conf target-b
run_exact file-relative-order \
    "pacman -F usr/bin/a --config custom.conf usr/bin/b" \
    -F usr/bin/a --config custom.conf usr/bin/b
run_exact remove-relative-order \
    "sudo pacman -R target-a --config custom.conf target-b" \
    -R target-a --config custom.conf target-b
run_exact database-relative-order \
    "sudo pacman -D target-a --config custom.conf target-b" \
    -D target-a --config custom.conf target-b

# argc/help/versionはconfig・default cache・Logger初期化より前に終了する。
setup_case no-arguments
run_fail
assert_contains "USAGE" "$output_file"
assert_pre_log_exit

setup_case help-after-global
run_ok --noedit --help
assert_contains "USAGE" "$output_file"
assert_pre_log_exit

setup_case version-after-global
run_ok --noedit --version
assert_contains "Moguet v" "$output_file"
assert_pre_log_exit

setup_case help-operation
run_ok --help
assert_contains "USAGE" "$output_file"
assert_contains \
    "build <pkg> [V=K...] | build --local <directory> [V=K...]" \
    "$output_file"
assert_contains "upgrade-all" "$output_file"
assert_contains "clean" "$output_file"
assert_contains "deps [--recursive] <pkg>..." "$output_file"
assert_contains "plan <pkg>..." "$output_file"
assert_contains "fetch <pkg>..." "$output_file"
assert_contains "add-src <item>..." "$output_file"
assert_contains "edit-src <pkg>..." "$output_file"
assert_contains "list-src" "$output_file"
assert_contains "del-src <pkg>..." "$output_file"
assert_contains "revert <pkg>..." "$output_file"
assert_contains \
    "Unsupported for separated source builds; no dependency cleanup is performed" \
    "$output_file"
assert_pre_log_exit

setup_case version-operation
run_ok --version
assert_contains "Moguet v" "$output_file"
assert_pre_log_exit

setup_case unknown-custom-operation
run_fail unknown-operation
assert_contains "Unknown operation: unknown-operation" "$output_file"
assert_pre_log_exit

setup_case targetless-custom-operation
run_fail plan
assert_contains "Invalid: Usage: moguet plan <pkg>..." "$output_file"
assert_pre_log_exit

# Matrix I2 / Issue #350 Slice 3: productionとdry-runは同じrich operand
# contractを参照する。ignored bare operandはdispatch前にcompatibility
# correctionとしてrejectし、delegated pacmanのopen grammarは上のmatrixを維持する。
assert_operand_contract_rejected() {
    case_name=$1
    expected=$2
    shift 2

    setup_case "$case_name"
    run_fail "$@"
    assert_contains "Invalid: $expected" "$output_file"
    assert_pre_log_exit
}

assert_operand_contract_rejected remote-build-extra-target \
    "Operation build requires exactly one <pkg> operand." \
    build clean-root extra-target
assert_operand_contract_rejected dry-run-remote-build-extra-target \
    "Operation build requires exactly one <pkg> operand." \
    --dry-run build clean-root extra-target
assert_operand_contract_rejected upgrade-target \
    "Operation upgrade does not accept target operands." \
    upgrade ignored-target
assert_operand_contract_rejected dry-run-upgrade-target-contract \
    "Operation upgrade does not accept target operands." \
    --dry-run upgrade ignored-target
assert_operand_contract_rejected clean-target \
    "Operation clean does not accept target operands." \
    clean ignored-target
assert_operand_contract_rejected list-src-target \
    "Operation list-src does not accept target operands." \
    list-src ignored-target

setup_case multi-target-deps
run_ok deps clean-root conflict-only
assert_contains "clean-root" "$output_file"
assert_contains "conflict-only" "$output_file"

setup_case multi-target-plan
run_ok plan clean-root conflict-only
assert_contains "Plan state:" "$output_file"
assert_contains "clean-root" "$output_file"
assert_contains "conflict-only" "$output_file"

setup_case multi-target-fetch
run_ok fetch clean-root conflict-only
assert_contains "Fetch targets:" "$output_file"
assert_contains "https://aur.archlinux.org/clean-root.git" "$command_log"
assert_contains "https://aur.archlinux.org/conflict-only.git" "$command_log"

setup_case multi-target-source-maintenance
run_ok add-src maintenance-one maintenance-two CFLAGS=-O2
assert_contains "Added maintenance-one to source-build list." "$output_file"
assert_contains "Added maintenance-two to source-build list." "$output_file"
run_ok del-src maintenance-one maintenance-two
assert_contains "Removing maintenance-one from list..." "$output_file"
assert_contains "Removing maintenance-two from list..." "$output_file"

# POLICY(#335): semantic `--` is accepted only by target-bearing source-
# preference operations so a leading-hyphen package reaches their validator.
# Other optionless operations retain their unsupported-option behavior.
for operation in clean list-src upgrade upgrade-aur; do
    setup_case "$operation-rejects-end-of-options"
    run_fail "$operation" --
    assert_contains "Unsupported $operation option: --" "$output_file"
    assert_pre_log_exit
done

setup_case upgrade-all-rejects-end-of-options
run_fail upgrade-all --
assert_contains "upgrade-all does not accept the -- operand marker." "$output_file"
assert_pre_log_exit

setup_case build-rejects-end-of-options
run_fail build -- clean-root
assert_contains "Unsupported build option: --" "$output_file"
assert_pre_log_exit

run_exact help-as-option-value \
    "pacman -Q --root --help filesystem" \
    -Q --root --help filesystem
run_exact version-as-option-value \
    "pacman -Q --root --version filesystem" \
    -Q --root --version filesystem
run_exact help-as-opaque-operand \
    "sudo pacman -U -- --help" \
    -U -- --help
run_exact version-as-opaque-operand \
    "sudo pacman -U -- --version" \
    -U -- --version

# Matrix J: official transactionはAUR targetだけを元indexで除外し、残りの順序を保つ。
setup_case official-sync-order
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a official-b'
write_repository_package official-a
write_repository_package official-b
run_ok -S official-a --config custom.conf official-b
assert_only_sudo_command "sudo pacman -S official-a --config custom.conf official-b"
assert_command_absent "sudo pacman -S --config custom.conf official-a official-b"

setup_case official-sync-generated-option
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a official-b'
write_repository_package official-a
write_repository_package official-b
run_ok --noconfirm -S official-a --config custom.conf official-b
assert_only_sudo_command "sudo pacman -S --noconfirm official-a --config custom.conf official-b"

setup_case mixed-sync-unsupported
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a official-c'
write_repository_package official-a
write_repository_package official-c
run_fail -S official-a --config custom.conf clean-root official-c
assert_contains "Unsupported pacman option for AUR/source-build target: --config" "$output_file"
assert_no_mutation_commands

setup_case mixed-sync-supported
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a official-c'
write_repository_package official-a
write_repository_package official-c
run_fail --noconfirm --noedit -S official-a clean-root official-c
assert_only_sudo_command "sudo pacman -S --noconfirm official-a official-c"
assert_command_absent "sudo pacman -S --noconfirm official-a clean-root official-c"

# 同名option valueとAUR targetを文字列一致でまとめて削除しない。
setup_case info-removes-target-by-index
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-a'
write_repository_package official-a
run_ok -Si official-a --config clean-root clean-root
assert_command_count "pacman -Si official-a --config clean-root" 1
assert_command_absent "pacman -Si official-a --config"

# Matrix K: 通常位置のglobalはAUR/makepkgへ反映し、value/opaque位置では反映しない。
setup_case aur-global-options
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-only'
export MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE=0
run_fail --noedit --nodiff --noconfirm \
    --build-mode=clean --cleanbuild -S clean-root
assert_command_count "makepkg -sc --noconfirm -C" 1

setup_case aur-global-rmdeps
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-only'
run_fail --noedit --nodiff --noconfirm \
    --build-mode=rebuild --rebuild --rmdeps -S clean-root
assert_contains "Separated build/install does not support --rmdeps." "$output_file"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "pacman-conf --repo-list"
assert_no_mutation_commands

setup_case aur-trailing-rmdeps
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-only'
run_fail -S clean-root --rmdeps --noedit --noconfirm
assert_contains "Separated build/install does not support --rmdeps." "$output_file"
assert_command "pacman-conf --verbose RootDir DBPath"
assert_command "pacman-conf --repo-list"
assert_no_mutation_commands

setup_case aur-global-name-as-option-value
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-only'
run_fail -S --root --rmdeps clean-root
assert_contains "Unsupported pacman option for AUR/source-build target: --root" "$output_file"
assert_no_mutation_commands

setup_case aur-global-name-as-opaque-target
export MOGUET_TEST_PACMAN_REPO_PACKAGES='official-only'
run_fail -S -- --rmdeps
assert_contains "Invalid package name: --rmdeps" "$output_file"
assert_command_log_empty

# Matrix L: upgrade-aurはglobal optionを消費するが、targetやopaque operandは受けない。
setup_case upgrade-aur-global-options-with-target
run_fail --noedit upgrade-aur --nodiff --noconfirm \
    --build-mode=clean --cleanbuild clean-root
assert_contains "Operation upgrade-aur does not accept target operands." "$output_file"
assert_command_log_empty

setup_case upgrade-aur-rejects-rmdeps
run_fail --noedit --nodiff --noconfirm \
    --build-mode=rebuild --rebuild --rmdeps upgrade-aur
assert_contains "Separated build/install does not support --rmdeps." "$output_file"
assert_command_log_empty

setup_case upgrade-aur-needed-as-opaque-target
run_fail upgrade-aur -- --needed
assert_contains "Operation upgrade-aur does not accept target operands." "$output_file"
assert_not_contains "Unsupported upgrade-aur option: --needed" "$output_file"
assert_command_log_empty

# Matrix M: root package selection intentは通常位置だけで消費し、plain -Sの
# strict entry validationとinput gateをsearch・state log・external commandより先に行う。
assert_select_stops_before_search() {
    case_name=$1
    shift

    setup_case "$case_name"
    run_fail "$@"
    assert_contains \
        "Interactive package selection requires a TTY on standard input." \
        "$output_file"
    assert_pre_log_exit
}

assert_select_rejected_pre_log() {
    case_name=$1
    expected=$2
    shift 2

    setup_case "$case_name"
    run_fail "$@"
    assert_contains "$expected" "$output_file"
    assert_pre_log_exit
}

# operation前後と重複指定はいずれも同じselection intentとして消費する。
assert_select_stops_before_search select-before-operation \
    --select -S query
assert_select_stops_before_search select-after-operation \
    -S --select query
assert_select_stops_before_search select-duplicates \
    --select -S query --select --select

# --neededだけはselection installへ投影できるpacman optionとして受理する。
assert_select_stops_before_search select-needed \
    -S --needed --select query

# plain -S以外のoperation / modifierはselection routeへ入れない。
assert_select_rejected_pre_log select-search-operation \
    "Option --select is supported only with plain -S." \
    -Ss --select query
assert_select_rejected_pre_log select-info-operation \
    "Option --select is supported only with plain -S." \
    -Si --select query
assert_select_rejected_pre_log select-system-upgrade-operation \
    "Option --select is supported only with plain -S." \
    --select -Syu
assert_select_rejected_pre_log select-query-operation \
    "Option --select is supported only with plain -S." \
    --select -Q query
assert_select_rejected_pre_log select-custom-operation \
    "Option --select is supported only with plain -S." \
    --select plan query

setup_case select-unknown-bare-operation
run_fail --select unknown-select-operation
assert_contains "Unknown operation: unknown-select-operation" "$output_file"
assert_pre_log_exit

# queryはASCII whitespaceをtrimした非空・control-freeの1 operandに限る。
assert_select_rejected_pre_log select-missing-query \
    "Operation -S --select requires exactly one <query> operand." \
    -S --select
assert_select_rejected_pre_log select-multiple-queries \
    "Operation -S --select requires exactly one <query> operand." \
    -S --select query-a query-b

ascii_whitespace_query=$(printf ' \011\015\014\013 ')
assert_select_rejected_pre_log select-ascii-whitespace-query \
    "Root package search query must not be empty." \
    -S --select "$ascii_whitespace_query"

control_query=$(printf 'query\001value')
assert_select_rejected_pre_log select-control-query \
    "Root package search query contains a control character." \
    -S --select "$control_query"

# --needed以外のpacman option、operand marker、rmdepsはpre-logで拒否する。
assert_select_rejected_pre_log select-unsupported-pacman-option \
    "Unsupported option --config for -S --select." \
    -S --select --config custom.conf query
assert_select_rejected_pre_log select-end-of-options \
    "Cannot use -- with --select." \
    -S --select -- query
assert_select_rejected_pre_log select-rmdeps \
    "Cannot combine --select and --rmdeps." \
    -S query --select --rmdeps

# Matrix N: `--local`はbuild所有のexact semantic optionとしてだけrouteし、
# local rootへ触れる前にoperation / option / operand grammarを確定する。
assert_local_build_rejected_pre_log() {
    case_name=$1
    expected=$2
    shift 2

    setup_case "$case_name"
    run_fail "$@"
    assert_contains "$expected" "$output_file"
    assert_pre_log_exit
}

# 正式形と許可済みglobal / ordered assignmentはstrict parserを通過し、
# downstreamのdescriptor-first root inspectionへ到達する。
setup_case local-build-formal-route
missing_local_root=$case_dir/missing-local-root
run_fail --noedit --noconfirm --build-mode=clean \
    build --local "$missing_local_root" FIRST=one EMPTY=
assert_contains \
    "Local source root entry is missing: $missing_local_root" \
    "$output_file"
assert_pre_log_exit

assert_local_build_rejected_pre_log local-build-wrong-operation \
    "Option --local is supported only with operation build." \
    -S --local .
assert_local_build_rejected_pre_log local-build-selector-in-operation-slot \
    "Option --local is supported only with operation build." \
    --local build .
assert_local_build_rejected_pre_log local-build-missing-directory \
    "Operation build --local requires exactly one <directory> operand." \
    build --local
assert_local_build_rejected_pre_log local-build-duplicate-selector \
    "Option --local may be specified only once for operation build." \
    build --local . --local
assert_local_build_rejected_pre_log local-build-package-co-use \
    "Operation build --local requires exactly one <directory> operand." \
    build --local . remote-package
assert_local_build_rejected_pre_log local-build-other-pacman-option \
    "Unsupported option --needed for build --local." \
    build --local . --needed
assert_local_build_rejected_pre_log local-build-value-taking-pacman-option \
    "Unsupported option --root for build --local." \
    build --local . --root custom-root
assert_local_build_rejected_pre_log local-build-end-of-options \
    "Cannot use -- with build --local." \
    build --local -- .
assert_local_build_rejected_pre_log local-build-assignment-before-directory \
    "Environment assignment requires a preceding directory: KEY=value" \
    build --local KEY=value .
assert_local_build_rejected_pre_log local-build-invalid-assignment \
    "Invalid environment assignment: 9BAD=value" \
    build --local . 9BAD=value

# local routeで意味を維持できないglobal optionもoperation-local parserが拒否する。
for unsupported_local_option in \
    --diff --nodiff --rmdeps --select --aur --repo
do
    case_name=local-build-rejects-${unsupported_local_option#--}
    assert_local_build_rejected_pre_log "$case_name" \
        "Unsupported option $unsupported_local_option for build --local." \
        build --local . "$unsupported_local_option"
done

# Pacman option value / opaque operandに現れた同じ綴りはlocal selectorへ昇格しない。
setup_case local-name-as-pacman-option-value
run_fail build --root --local .
assert_contains "Unsupported build option: --root" "$output_file"
assert_not_contains "supported only with operation build" "$output_file"
assert_pre_log_exit

setup_case local-name-as-opaque-operand
run_fail build -- --local .
assert_contains \
    "Invalid: Operation build requires exactly one <pkg> operand." \
    "$output_file"
assert_not_contains "supported only with operation build" "$output_file"
assert_pre_log_exit

# Matrix O: `--output-dir=DIR`は-Gだけが所有するattached-value optionであり、
# global/configや-Gpへ昇格させない。
assert_output_directory_rejected_pre_log() {
    case_name=$1
    expected=$2
    shift 2

    setup_case "$case_name"
    run_fail "$@"
    assert_contains "$expected" "$output_file"
    assert_pre_log_exit
}

assert_output_directory_rejected_pre_log output-directory-bare \
    "Option --output-dir requires a non-empty attached value in the form --output-dir=DIR." \
    -G clean-root --output-dir
assert_output_directory_rejected_pre_log output-directory-empty \
    "Option --output-dir requires a non-empty attached value in the form --output-dir=DIR." \
    -G clean-root --output-dir=
assert_output_directory_rejected_pre_log output-directory-space-separated \
    "Operation -G requires exactly one <pkg> operand." \
    -G clean-root --output-dir ./exports
assert_output_directory_rejected_pre_log output-directory-duplicate-same \
    "Option --output-dir may be specified only once for operation -G." \
    -G clean-root --output-dir=./exports --output-dir=./exports
assert_output_directory_rejected_pre_log output-directory-duplicate-different \
    "Option --output-dir may be specified only once for operation -G." \
    -G clean-root --output-dir=./exports --output-dir=/tmp
assert_output_directory_rejected_pre_log output-directory-print-scope \
    "Option --output-dir=DIR is supported only with operation -G." \
    -Gp clean-root --output-dir=./exports
assert_output_directory_rejected_pre_log output-directory-other-operation \
    "Option --output-dir=DIR is supported only with operation -G." \
    plan clean-root --output-dir=./exports
assert_output_directory_rejected_pre_log output-directory-global-position \
    "Option --output-dir=DIR is supported only with operation -G." \
    --output-dir=./exports -G clean-root
assert_output_directory_rejected_pre_log output-directory-after-marker \
    "Operation -G requires exactly one <pkg> operand." \
    -G clean-root -- --output-dir=./exports

# pacman option valueとopaque operandに現れた同名tokenはsemantic optionではない。
setup_case output-directory-name-as-pacman-option-value
run_ok -Q --root --output-dir=./exports filesystem
assert_only_command "pacman -Q --root --output-dir=./exports filesystem"

setup_case output-directory-name-as-opaque-operand
run_ok -U -- --output-dir=./exports
assert_only_command "sudo pacman -U -- --output-dir=./exports"

echo "CLI parser integration tests: all checks passed"
