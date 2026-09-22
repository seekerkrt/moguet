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

mkdir -p \
    "$tmp_dir/cache" \
    "$tmp_dir/config" \
    "$tmp_dir/home" \
    "$tmp_dir/pacman-db/local" \
    "$tmp_dir/pacman-db/sync" \
    "$tmp_dir/state"
chmod 0700 "$tmp_dir/config"
printf '9\n' > "$tmp_dir/pacman-db/local/ALPM_DB_VERSION"
/usr/bin/tar -czf "$tmp_dir/pacman-db/sync/core.db" \
    --files-from /dev/null
command_log=$tmp_dir/commands.log
: > "$command_log"

add_installed_package() {
    package_name=$1
    package_version=$2
    package_provides=${3:-}
    package_directory=$tmp_dir/pacman-db/local/$package_name-$package_version
    mkdir -p "$package_directory"
    {
        printf '%%NAME%%\n%s\n\n' "$package_name"
        printf '%%VERSION%%\n%s\n\n' "$package_version"
        printf '%%DESC%%\nMoguet relation assessment fixture\n\n'
        printf '%%ARCH%%\nany\n\n'
        printf '%%REASON%%\n0\n\n'
        if [ -n "$package_provides" ]; then
            printf '%%PROVIDES%%\n%s\n\n' "$package_provides"
        fi
    } > "$package_directory/desc"
}

add_installed_package conflict-old 1.0-1
add_installed_package replace-legacy 1.0-1
add_installed_package dep-old 3.0-1
add_installed_package dep-legacy 1.0-1
add_installed_package root-old 1.0-1
add_installed_package root-legacy 1.0-1
add_installed_package unknown-relation-provider 1.0-1 \
    unknown-relation-target
add_installed_package gegl 0.4.70-3 \
    'libgegl-0.4.so=0-64
libgegl-npd-0.4.so=libgegl-npd-0.4.so-64
libgegl-sc-0.4.so=libgegl-sc-0.4.so-64'
add_installed_package soname-regression-git 0.9-1 wezterm

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
export HOME=$tmp_dir/home
export XDG_CONFIG_HOME=$tmp_dir/config
export XDG_STATE_HOME=$tmp_dir/state
export XDG_CACHE_HOME=$tmp_dir/cache
export PATH=$repo_root/tests/stubs:/usr/bin:/bin
require_exact_test_command pacman-conf "$repo_root/tests/stubs/pacman-conf"
require_exact_test_command makepkg "$repo_root/tests/stubs/makepkg"
require_exact_test_command pacman "$repo_root/tests/stubs/pacman"
require_exact_test_command sudo "$repo_root/tests/stubs/sudo"
require_exact_test_command git "$repo_root/tests/stubs/git"
export MOGUET_TEST_AUR_RPC_BASE_URL=http://127.0.0.1:$port/rpc/
export MOGUET_TEST_COMMAND_LOG=$command_log
export MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST=core
export MOGUET_TEST_PACKAGE_METADATA_DB_PATH=$tmp_dir/pacman-db
export MOGUET_TEST_PACMAN_EXIT_CODE=1
export MOGUET_TEST_SUDO_EXIT_CODE=99
unset MOGUET_TEST_PACMAN_QM_OUTPUT
unset MOGUET_TEST_PACMAN_REPO_PACKAGES
unset MOGUET_TEST_GIT_REMOTE_URL
unset MOGUET_TEST_GIT_CLONE_EXIT_CODE
unset MOGUET_TEST_GIT_CLONE_SYMLINK_TARGET
unset MOGUET_TEST_GIT_CLONE_FIXTURE_DIR
unset MOGUET_TEST_MAKEPKG_EXIT_CODE

run_ok() {
    output_file=$1
    shift
    "$test_binary" "$@" </dev/null > "$output_file" 2>&1
}

run_fail() {
    output_file=$1
    shift
    if ! validation_expect_status conflicts-replaces-business-failure 1 \
        "$output_file" "$output_file" "$test_binary" "$@" </dev/null; then
        exit 1
    fi
}

assert_contains() {
    pattern=$1
    file=$2
    if ! grep -F "$pattern" "$file" >/dev/null; then
        echo "missing expected output: $pattern" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_not_contains() {
    pattern=$1
    file=$2
    if grep -F "$pattern" "$file" >/dev/null; then
        echo "unexpected output: $pattern" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_no_relation_mutation() {
    if grep -E \
        '^(git|makepkg|sudo) |^pacman (-S|-U|-R)( |$)' \
        "$command_log" >/dev/null
    then
        echo "relation blocker allowed a mutation command" >&2
        cat "$command_log" >&2
        exit 1
    fi
}

# Normal keeps relation identity and blocking truth; --details owns raw
# metadata, source/root attribution, and component-level diagnostic evidence.
run_ok "$tmp_dir/conflict-plan-normal.out" plan conflict-only
assert_contains "Plan targets: conflict-only" "$tmp_dir/conflict-plan-normal.out"
assert_contains "conflict: conflict-only -> conflict-old; installed conflict with conflict-old" \
    "$tmp_dir/conflict-plan-normal.out"
assert_contains "conflict: conflict-only -> conflict-git; no matching installed or planned target" \
    "$tmp_dir/conflict-plan-normal.out"
assert_contains "Build/install is blocked." "$tmp_dir/conflict-plan-normal.out"
assert_contains "Build readiness: Requires check" "$tmp_dir/conflict-plan-normal.out"
assert_contains "Install readiness: Requires check" "$tmp_dir/conflict-plan-normal.out"
assert_not_contains "Plan state:" "$tmp_dir/conflict-plan-normal.out"
assert_not_contains "target component" "$tmp_dir/conflict-plan-normal.out"
assert_no_relation_mutation
run_ok "$tmp_dir/conflict-plan.out" --details plan conflict-only
assert_contains "conflicts: conflict-old, conflict-git" "$tmp_dir/conflict-plan.out"
assert_contains "Installed conflict confirmed" "$tmp_dir/conflict-plan.out"
assert_contains "declaring package conflict-only" "$tmp_dir/conflict-plan.out"
assert_contains "matched installed package conflict-old" "$tmp_dir/conflict-plan.out"
assert_contains "target component conflict-old" "$tmp_dir/conflict-plan.out"
assert_contains "build/install is blocked before mutation" \
    "$tmp_dir/conflict-plan.out"
assert_not_contains "ConfirmedInstalledConflict" "$tmp_dir/conflict-plan.out"

run_ok "$tmp_dir/replace-plan-normal.out" plan replace-only
assert_contains "replacement: replace-only -> replace-legacy; potential replacement of replace-legacy" \
    "$tmp_dir/replace-plan-normal.out"
assert_contains "Review required; no automatic replacement. Build/install is blocked." \
    "$tmp_dir/replace-plan-normal.out"
assert_contains "Build readiness: Requires check" "$tmp_dir/replace-plan-normal.out"
assert_contains "Install readiness: Requires check" "$tmp_dir/replace-plan-normal.out"
assert_no_relation_mutation
run_ok "$tmp_dir/replace-plan.out" --details plan replace-only
assert_contains "replaces: replace-legacy" "$tmp_dir/replace-plan.out"
assert_contains "Potential replacement impact" "$tmp_dir/replace-plan.out"
assert_contains "matched installed package replace-legacy" \
    "$tmp_dir/replace-plan.out"
assert_contains "is a replacement candidate" "$tmp_dir/replace-plan.out"
assert_contains "review is required and no automatic replacement is performed" \
    "$tmp_dir/replace-plan.out"
assert_not_contains "PotentialReplacement" "$tmp_dir/replace-plan.out"

: > "$command_log"
run_ok "$tmp_dir/dependency-plan-normal.out" plan dependency-risk-root
assert_contains "Plan targets: dependency-risk-root" "$tmp_dir/dependency-plan-normal.out"
assert_contains "conflict: risk-dep -> dep-old>=2; installed conflict with dep-old" \
    "$tmp_dir/dependency-plan-normal.out"
assert_contains "replacement: risk-dep -> dep-legacy; potential replacement of dep-legacy" \
    "$tmp_dir/dependency-plan-normal.out"
assert_contains "Build/install is blocked." "$tmp_dir/dependency-plan-normal.out"
assert_contains "Build readiness: Requires check" "$tmp_dir/dependency-plan-normal.out"
assert_contains "Install readiness: Requires check" "$tmp_dir/dependency-plan-normal.out"
assert_no_relation_mutation
# Deduplication of raw metadata remains a Detailed diagnostic contract.
run_ok "$tmp_dir/dependency-plan.out" --details plan dependency-risk-root
assert_contains "risk-dep" "$tmp_dir/dependency-plan.out"
assert_contains "conflicts: dep-old>=2" "$tmp_dir/dependency-plan.out"
assert_contains "construction: Constructed" "$tmp_dir/dependency-plan.out"
assert_contains "completeness: Complete" "$tmp_dir/dependency-plan.out"
assert_contains "Fetch readiness: Ready" "$tmp_dir/dependency-plan.out"
assert_contains "Build readiness: Requires check" "$tmp_dir/dependency-plan.out"
assert_contains "Install readiness: Requires check" "$tmp_dir/dependency-plan.out"
assert_contains "Installed conflict confirmed" "$tmp_dir/dependency-plan.out"
assert_contains "Potential replacement impact" "$tmp_dir/dependency-plan.out"
assert_contains "declaring package risk-dep" "$tmp_dir/dependency-plan.out"
assert_not_contains "actual relation: unassessed (#353)" \
    "$tmp_dir/dependency-plan.out"
risk_dep_count=$(validation_grep_count -c '^  risk-dep$' \
    "$tmp_dir/dependency-plan.out")
if [ "$risk_dep_count" -ne 1 ]; then
    echo "risk-dep metadata was not deduplicated" >&2
    exit 1
fi

run_ok "$tmp_dir/clean-plan.out" plan clean-root
assert_not_contains "Metadata conflicts/replaces:" "$tmp_dir/clean-plan.out"
assert_contains "Plan targets: clean-root" "$tmp_dir/clean-plan.out"
assert_contains "Fetch/build/install: ready" "$tmp_dir/clean-plan.out"
assert_not_contains "Plan state:" "$tmp_dir/clean-plan.out"
assert_not_contains "construction:" "$tmp_dir/clean-plan.out"
assert_not_contains "completeness:" "$tmp_dir/clean-plan.out"

run_ok "$tmp_dir/planned-conflict-plan.out" plan planned-conflict-root
assert_contains "planned conflict with planned-conflict-target" \
    "$tmp_dir/planned-conflict-plan.out"
assert_contains "Build/install is blocked." \
    "$tmp_dir/planned-conflict-plan.out"
assert_not_contains "roots:" \
    "$tmp_dir/planned-conflict-plan.out"
assert_not_contains "ConfirmedPlannedTargetConflict" \
    "$tmp_dir/planned-conflict-plan.out"
assert_not_contains "Fetch readiness:" "$tmp_dir/planned-conflict-plan.out"
assert_contains "Build readiness: Requires check" \
    "$tmp_dir/planned-conflict-plan.out"
assert_contains "Install readiness: Requires check" \
    "$tmp_dir/planned-conflict-plan.out"
run_ok "$tmp_dir/planned-conflict-details.out" --details plan planned-conflict-root
assert_contains "Planned-target conflict confirmed" "$tmp_dir/planned-conflict-details.out"
assert_contains "matched planned package planned-conflict-target" "$tmp_dir/planned-conflict-details.out"
assert_contains "roots: input #1 requested planned-conflict-root" "$tmp_dir/planned-conflict-details.out"
assert_contains "Fetch readiness: Ready" "$tmp_dir/planned-conflict-details.out"

run_ok "$tmp_dir/no-match-plan-normal.out" plan no-match-root
assert_contains "conflict: no-match-root -> absent-relation-target; no matching installed or planned target" \
    "$tmp_dir/no-match-plan-normal.out"
assert_contains "Fetch/build/install: ready" "$tmp_dir/no-match-plan-normal.out"
assert_not_contains "Build/install is blocked" "$tmp_dir/no-match-plan-normal.out"
run_ok "$tmp_dir/no-match-plan.out" --details plan no-match-root
assert_contains "Confirmed no matching current or planned target" \
    "$tmp_dir/no-match-plan.out"
assert_contains "declares conflict absent-relation-target" \
    "$tmp_dir/no-match-plan.out"
assert_contains "complete current/planned observation" \
    "$tmp_dir/no-match-plan.out"
assert_contains "this relation does not block build/install" \
    "$tmp_dir/no-match-plan.out"
assert_not_contains "no conflict" "$tmp_dir/no-match-plan.out"
assert_not_contains "ConfirmedNoMatchingCurrentOrPlannedTarget" \
    "$tmp_dir/no-match-plan.out"
assert_contains "completeness: Complete" "$tmp_dir/no-match-plan.out"
assert_contains "Build readiness: Ready" "$tmp_dir/no-match-plan.out"
assert_contains "Install readiness: Ready" "$tmp_dir/no-match-plan.out"

: > "$command_log"
run_ok "$tmp_dir/soname-old-self-plan.out" plan soname-regression-git
assert_not_contains "Installed package provides metadata is malformed." \
    "$tmp_dir/soname-old-self-plan.out"
assert_not_contains "invalid relation metadata or observation" \
    "$tmp_dir/soname-old-self-plan.out"
assert_contains "no matching installed or planned target" \
    "$tmp_dir/soname-old-self-plan.out"
assert_not_contains "installed conflict with" \
    "$tmp_dir/soname-old-self-plan.out"
assert_contains "Fetch/build/install: ready" \
    "$tmp_dir/soname-old-self-plan.out"
assert_no_relation_mutation

add_installed_package wezterm 1.0-1
run_ok "$tmp_dir/soname-real-conflict-plan.out" plan soname-regression-git
assert_not_contains "Installed package provides metadata is malformed." \
    "$tmp_dir/soname-real-conflict-plan.out"
assert_contains "installed conflict with wezterm" \
    "$tmp_dir/soname-real-conflict-plan.out"
assert_not_contains "installed conflict with soname-regression-git" \
    "$tmp_dir/soname-real-conflict-plan.out"
assert_not_contains "completeness:" \
    "$tmp_dir/soname-real-conflict-plan.out"
assert_contains "Build readiness: Requires check" \
    "$tmp_dir/soname-real-conflict-plan.out"
assert_contains "Install readiness: Requires check" \
    "$tmp_dir/soname-real-conflict-plan.out"

: > "$command_log"
run_fail "$tmp_dir/soname-real-conflict-build.out" build soname-regression-git
assert_contains "Installed conflict confirmed" \
    "$tmp_dir/soname-real-conflict-build.out"
assert_no_relation_mutation

: > "$command_log"
run_fail "$tmp_dir/conflict-dry-run.out" --dry-run build conflict-only
assert_contains "Unified plan:" "$tmp_dir/conflict-dry-run.out"
assert_contains "Status: Blocked" "$tmp_dir/conflict-dry-run.out"
assert_contains "package relation blocker: Installed conflict confirmed" \
    "$tmp_dir/conflict-dry-run.out"
assert_contains "matched installed package conflict-old" \
    "$tmp_dir/conflict-dry-run.out"
assert_no_relation_mutation

: > "$command_log"
run_ok "$tmp_dir/no-match-dry-run.out" --dry-run build no-match-root
assert_contains "Unified plan:" "$tmp_dir/no-match-dry-run.out"
assert_contains "Status: Ready" "$tmp_dir/no-match-dry-run.out"
assert_contains "Confirmed no matching current or planned target" \
    "$tmp_dir/no-match-dry-run.out"
assert_contains "this relation does not block build/install" \
    "$tmp_dir/no-match-dry-run.out"
assert_not_contains "package relation blocker:" \
    "$tmp_dir/no-match-dry-run.out"
assert_no_relation_mutation

run_ok "$tmp_dir/self-conflict-plan.out" plan foo-git
assert_contains "no matching installed or planned target" \
    "$tmp_dir/self-conflict-plan.out"
assert_not_contains "planned conflict with" \
    "$tmp_dir/self-conflict-plan.out"
assert_contains "Fetch/build/install: ready" "$tmp_dir/self-conflict-plan.out"

run_ok "$tmp_dir/other-planned-conflict-plan.out" plan foo-git-with-target
assert_contains "planned conflict with foo" \
    "$tmp_dir/other-planned-conflict-plan.out"
assert_contains "Build/install is blocked." "$tmp_dir/other-planned-conflict-plan.out"

add_installed_package foo-git 0.9-1 foo=1
run_ok "$tmp_dir/installed-old-self-plan.out" plan foo-git
assert_contains "no matching installed or planned target" \
    "$tmp_dir/installed-old-self-plan.out"
assert_not_contains "installed conflict with" \
    "$tmp_dir/installed-old-self-plan.out"
assert_not_contains "planned conflict with" \
    "$tmp_dir/installed-old-self-plan.out"
assert_contains "Fetch/build/install: ready" \
    "$tmp_dir/installed-old-self-plan.out"

: > "$command_log"
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_fail "$tmp_dir/installed-old-self-build.out" build foo-git
assert_not_contains "package relation assessment requires manual review" \
    "$tmp_dir/installed-old-self-build.out"
assert_contains "git clone https://aur.archlinux.org/foo-git.git" \
    "$command_log"
assert_contains "makepkg -sc" "$command_log"
assert_contains "sudo pacman -U" "$command_log"
assert_not_contains "pacman -R" "$command_log"
unset MOGUET_TEST_MAKEPKG_EXIT_CODE

add_installed_package foo 1.0-1
run_ok "$tmp_dir/installed-real-conflict-plan.out" plan foo-git
assert_contains "installed conflict with foo" \
    "$tmp_dir/installed-real-conflict-plan.out"
assert_not_contains "installed conflict with foo-git" \
    "$tmp_dir/installed-real-conflict-plan.out"
assert_not_contains "Fetch readiness:" \
    "$tmp_dir/installed-real-conflict-plan.out"
assert_contains "Build readiness: Requires check" \
    "$tmp_dir/installed-real-conflict-plan.out"
assert_contains "Install readiness: Requires check" \
    "$tmp_dir/installed-real-conflict-plan.out"

: > "$command_log"
run_fail "$tmp_dir/installed-real-conflict-build.out" build foo-git
assert_contains "Installed conflict confirmed" \
    "$tmp_dir/installed-real-conflict-build.out"
assert_contains "declaring package foo-git" \
    "$tmp_dir/installed-real-conflict-build.out"
assert_contains "matched installed package foo" \
    "$tmp_dir/installed-real-conflict-build.out"
assert_no_relation_mutation

run_ok "$tmp_dir/deps.out" deps risk-root
assert_contains "Metadata conflicts/replaces:" "$tmp_dir/deps.out"
assert_contains "conflicts: root-old, root-git" "$tmp_dir/deps.out"
assert_contains "replaces: root-legacy" "$tmp_dir/deps.out"

run_ok "$tmp_dir/info.out" -Si risk-root
assert_contains "Conflicts With  : root-old  root-git" "$tmp_dir/info.out"
assert_contains "Replaces        : root-legacy" "$tmp_dir/info.out"
assert_contains "Relation Check  : deferred to planning and build preflight" \
    "$tmp_dir/info.out"
assert_not_contains "Installed conflict confirmed" "$tmp_dir/info.out"

: > "$command_log"
run_fail "$tmp_dir/build.out" build dependency-risk-root
assert_contains "Installed conflict confirmed" \
    "$tmp_dir/build.out"
assert_contains "declaring package risk-dep" \
    "$tmp_dir/build.out"
assert_no_relation_mutation

: > "$command_log"
run_fail "$tmp_dir/noconfirm.out" --noconfirm -S risk-root
assert_contains "Installed conflict confirmed" \
    "$tmp_dir/noconfirm.out"
assert_no_relation_mutation

: > "$command_log"
run_fail "$tmp_dir/planned-build.out" build planned-conflict-root
assert_contains "Planned-target conflict confirmed" "$tmp_dir/planned-build.out"
assert_contains "matched planned package planned-conflict-target" \
    "$tmp_dir/planned-build.out"
assert_no_relation_mutation

: > "$command_log"
run_fail "$tmp_dir/replacement-noconfirm.out" --noconfirm -S replace-only
assert_contains "Potential replacement impact" \
    "$tmp_dir/replacement-noconfirm.out"
assert_contains "no automatic replacement is performed" \
    "$tmp_dir/replacement-noconfirm.out"
assert_no_relation_mutation

: > "$command_log"
run_ok "$tmp_dir/unknown-plan.out" plan unknown-relation-root
assert_contains "relation judgment unavailable" "$tmp_dir/unknown-plan.out"
assert_contains "version judgment unavailable" "$tmp_dir/unknown-plan.out"
assert_contains "Build/install is blocked." "$tmp_dir/unknown-plan.out"
assert_not_contains "no matching installed or planned target" \
    "$tmp_dir/unknown-plan.out"
assert_contains "completeness: Unknown" "$tmp_dir/unknown-plan.out"
assert_not_contains "Fetch readiness:" "$tmp_dir/unknown-plan.out"
assert_contains "Build readiness: Requires check" "$tmp_dir/unknown-plan.out"
assert_contains "Install readiness: Requires check" "$tmp_dir/unknown-plan.out"
run_ok "$tmp_dir/unknown-plan-details.out" --details plan unknown-relation-root
assert_contains "Relation judgment unavailable" "$tmp_dir/unknown-plan-details.out"
assert_contains "not a confirmed absence" "$tmp_dir/unknown-plan-details.out"
assert_contains "Fetch readiness: Ready" "$tmp_dir/unknown-plan-details.out"
run_fail "$tmp_dir/unknown-noconfirm.out" \
    --noconfirm build unknown-relation-root
assert_contains "Relation judgment unavailable" \
    "$tmp_dir/unknown-noconfirm.out"
assert_no_relation_mutation

export MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE=42
run_ok "$tmp_dir/inventory-failure-plan.out" plan no-match-root
assert_contains "relation judgment unavailable" \
    "$tmp_dir/inventory-failure-plan.out"
assert_contains "completeness: Unknown" "$tmp_dir/inventory-failure-plan.out"
assert_contains "Build/install is blocked." "$tmp_dir/inventory-failure-plan.out"
assert_not_contains "no matching installed or planned target" \
    "$tmp_dir/inventory-failure-plan.out"
unset MOGUET_TEST_PACKAGE_METADATA_PACMAN_CONF_EXIT_CODE

: > "$command_log"
export MOGUET_TEST_MAKEPKG_EXIT_CODE=0
run_fail "$tmp_dir/no-match-build.out" build no-match-root
assert_not_contains "Relation judgment unavailable" \
    "$tmp_dir/no-match-build.out"
assert_contains "git clone https://aur.archlinux.org/no-match-root.git" \
    "$command_log"
assert_contains "makepkg -sc" "$command_log"
assert_contains "sudo pacman -U" "$command_log"
assert_not_contains "pacman -R" "$command_log"
unset MOGUET_TEST_MAKEPKG_EXIT_CODE

: > "$command_log"
run_ok "$tmp_dir/fetch-first.out" fetch risk-root
assert_contains "Fetch targets:" "$tmp_dir/fetch-first.out"
assert_contains "Installed conflict confirmed" "$tmp_dir/fetch-first.out"
assert_contains "git clone https://aur.archlinux.org/risk-dep.git risk-dep" "$command_log"
assert_contains "git clone https://aur.archlinux.org/risk-root.git risk-root" "$command_log"
run_ok "$tmp_dir/fetch-second.out" fetch risk-root
assert_contains "git fetch origin" "$command_log"
assert_not_contains "makepkg " "$command_log"
assert_not_contains "sudo " "$command_log"

run_ok "$tmp_dir/unresolved.out" plan unresolved-root
assert_contains "completeness: Incomplete" "$tmp_dir/unresolved.out"
assert_contains "Fetch readiness: Blocked" "$tmp_dir/unresolved.out"
assert_contains "Unresolved dependencies:" "$tmp_dir/unresolved.out"
run_ok "$tmp_dir/ambiguous.out" plan ambiguous-root
assert_contains "provider decision: Ambiguous" "$tmp_dir/ambiguous.out"
assert_contains "Fetch readiness: Blocked" "$tmp_dir/ambiguous.out"
assert_contains "Ambiguous provided dependencies:" "$tmp_dir/ambiguous.out"
run_ok "$tmp_dir/cycle.out" plan cycle-root
assert_contains "completeness: Incomplete" "$tmp_dir/cycle.out"
assert_contains "Fetch readiness: Blocked" "$tmp_dir/cycle.out"
assert_contains "Cyclic dependencies:" "$tmp_dir/cycle.out"
run_ok "$tmp_dir/split.out" plan split-child
assert_contains "Plan targets: split-child" "$tmp_dir/split.out"
assert_contains "1. split-base" "$tmp_dir/split.out"
assert_contains "target package: split-child" "$tmp_dir/split.out"
assert_contains "Install readiness: Blocked" "$tmp_dir/split.out"
assert_contains "Use the package-base set lifecycle" "$tmp_dir/split.out"
assert_not_contains "Fetch readiness:" "$tmp_dir/split.out"
assert_not_contains "Build readiness:" "$tmp_dir/split.out"
run_ok "$tmp_dir/split.out" --details plan split-child
assert_contains "Split package install targets:" "$tmp_dir/split.out"
assert_contains "split-child (base: split-base)" "$tmp_dir/split.out"
assert_contains "construction: Constructed" "$tmp_dir/split.out"
assert_contains "completeness: Complete" "$tmp_dir/split.out"
assert_contains "Fetch readiness: Ready" "$tmp_dir/split.out"
assert_contains "Build readiness: Ready" "$tmp_dir/split.out"
assert_contains "Install readiness: Blocked" "$tmp_dir/split.out"
assert_contains "required action: Use the package-base set lifecycle" "$tmp_dir/split.out"

for guard_target in unresolved-root ambiguous-root cycle-root; do
    : > "$command_log"
    run_fail "$tmp_dir/$guard_target-guard.out" build "$guard_target"
    if grep -E '^(git|makepkg|sudo) ' "$command_log" >/dev/null; then
        echo "existing plan guard regressed for $guard_target" >&2
        cat "$command_log" >&2
        exit 1
    fi
done

: > "$command_log"
run_ok "$tmp_dir/split-fetch.out" fetch split-child
assert_contains "git clone https://aur.archlinux.org/split-base.git split-base" "$command_log"
assert_not_contains "makepkg " "$command_log"
assert_not_contains "sudo " "$command_log"

add_installed_package unsupported-provides 1.0-1 \
    'unsupported-capability>=1'
: > "$command_log"
run_ok "$tmp_dir/unsupported-provides-plan.out" plan no-match-root
assert_contains "invalid relation metadata or observation" \
    "$tmp_dir/unsupported-provides-plan.out"
assert_contains "installed: metadata malformed" "$tmp_dir/unsupported-provides-plan.out"
assert_contains "Build/install is blocked." "$tmp_dir/unsupported-provides-plan.out"
assert_not_contains "no matching installed or planned target" "$tmp_dir/unsupported-provides-plan.out"
assert_contains "completeness: Incomplete" \
    "$tmp_dir/unsupported-provides-plan.out"
assert_contains "Build readiness: Blocked" \
    "$tmp_dir/unsupported-provides-plan.out"
assert_contains "Install readiness: Blocked" \
    "$tmp_dir/unsupported-provides-plan.out"
assert_no_relation_mutation

: > "$command_log"
run_fail "$tmp_dir/unsupported-provides-build.out" build no-match-root
assert_contains "Installed package provides metadata is malformed." \
    "$tmp_dir/unsupported-provides-build.out"
assert_no_relation_mutation

echo "conflicts/replaces integration tests: all checks passed"
