#!/bin/sh

set -eu

if [ "$#" -lt 2 ]; then
    printf '%s\n' \
        'build-authority-closure-test: Make-only validation interface; run make test-build-authority-closure' >&2
    exit 2
fi

repo_root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
cmake_command=$1
cmake_build_dir=$2
shift 2
case $cmake_build_dir in
    /*) ;;
    *) cmake_build_dir=$repo_root/$cmake_build_dir ;;
esac

test_root=$(mktemp -d)
cleanup() {
    rm -rf "$test_root"
}
trap cleanup EXIT INT TERM

fail() {
    printf 'build-authority-closure-test: %s\n' "$*" >&2
    exit 1
}

assert_contains() {
    inspected_file=$1
    expected_text=$2
    grep -F -- "$expected_text" "$inspected_file" >/dev/null ||
        fail "$inspected_file does not contain: $expected_text"
}

assert_not_contains() {
    inspected_file=$1
    unexpected_text=$2
    if grep -F -- "$unexpected_text" "$inspected_file" >/dev/null; then
        fail "$inspected_file unexpectedly contains: $unexpected_text"
    fi
}

assert_not_matches() {
    inspected_file=$1
    unexpected_pattern=$2
    if grep -E -- "$unexpected_pattern" "$inspected_file" >/dev/null; then
        fail "$inspected_file unexpectedly matches: $unexpected_pattern"
    fi
}

makefile=$repo_root/Makefile
cmake_lists=$repo_root/CMakeLists.txt
presets=$repo_root/CMakePresets.json
completion_generator=$repo_root/scripts/generate_completions.py
completion_publisher=$repo_root/cmake/MoguetGenerateCompletions.cmake
compile_commands_publisher=$repo_root/cmake/MoguetPublishCompileCommands.cmake
cmake_cache=$cmake_build_dir/CMakeCache.txt

for required_file in \
    "$makefile" \
    "$cmake_lists" \
    "$presets" \
    "$completion_generator" \
    "$completion_publisher" \
    "$compile_commands_publisher" \
    "$repo_root/.gitignore"
do
    [ -f "$required_file" ] && [ ! -L "$required_file" ] ||
        fail "required authority file is missing, non-regular, or a symlink: $required_file"
done

[ -f "$cmake_cache" ] ||
    fail "configured CMake cache is unavailable: $cmake_cache"
expected_default_options=${MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS:-}
[ "$expected_default_options" = ON ] || [ "$expected_default_options" = OFF ] ||
    fail 'Make did not export its default compile-option decision'
actual_default_options=$(
    sed -n \
        's/^MOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS:[^=]*=//p' \
        "$cmake_cache"
)
[ "$actual_default_options" = "$expected_default_options" ] ||
    fail "recursive Make default option decision is $actual_default_options; expected $expected_default_options"

# Make may select a compiler for CMake preflight and pass external inputs, but
# it must not retain any production/test C++ graph or compiler recipe.
assert_not_contains "$makefile" 'legacy-cpp-build-authority'

# Issue #476 store remains behind its semantic API. S6-B consumes it through
# the publication header; normal route consumers remain disallowed below.
provenance_store_includes=$test_root/provenance-store-includes.txt
if grep -l -F -- \
    '#include "devel_build_provenance_store.hpp"' \
    "$repo_root"/source/*.cpp > "$provenance_store_includes"
then
    :
else
    include_status=$?
    [ "$include_status" -eq 1 ] ||
        fail "provenance production include scan failed with status $include_status"
fi
printf '%s\n' \
    "$repo_root/source/devel_build_provenance_store.cpp" \
    > "$test_root/expected-provenance-store-includes.txt"
cmp -s \
    "$test_root/expected-provenance-store-includes.txt" \
    "$provenance_store_includes" ||
    fail 'Issue #476 Slice 2 provenance store gained a production caller'

# Issue #476 Slice 3 is linked into the production binary as a dormant
# foundation. Slice 4 is its only additional production-compiled consumer;
# normal source-build routes remain outside this authority chain.
source_build_context_includes=$test_root/source-build-context-includes.txt
if grep -l -F -- \
    '#include "invocation_owned_source_build_context.hpp"' \
    "$repo_root"/source/*.cpp > "$source_build_context_includes"
then
    :
else
    include_status=$?
    [ "$include_status" -eq 1 ] ||
        fail "source-build context production include scan failed with status $include_status"
fi
printf '%s\n' \
    "$repo_root/source/invocation_owned_source_build_context.cpp" \
    > "$test_root/expected-source-build-context-includes.txt"
cmp -s \
    "$test_root/expected-source-build-context-includes.txt" \
    "$source_build_context_includes" ||
    fail 'Issue #476 Slice 3 context gained a production caller'
assert_not_contains \
    "$repo_root/source/invocation_owned_source_build_context.cpp" \
    'publish_devel_build_provenance'
assert_not_contains \
    "$repo_root/source/invocation_owned_source_build_context.cpp" \
    'devel_build_provenance_store.hpp'

# Slice 4 proves only the exact actually built inside the sealed Slice 3
# context. Its API remains self-owned and has no normal route caller or #475
# remote observation. The S5-A bridge, S5-B live observer and S6-B semantic
# projector are the only downstream production consumers.
evaluated_build_includes=$test_root/evaluated-build-includes.txt
if grep -l -F -- \
    '#include "evaluated_devel_source_build.hpp"' \
    "$repo_root"/source/*.cpp > "$evaluated_build_includes"
then
    :
else
    include_status=$?
    [ "$include_status" -eq 1 ] ||
        fail "evaluated build production include scan failed with status $include_status"
fi
printf '%s\n' \
    "$repo_root/source/devel_build_provenance_publication.cpp" \
    "$repo_root/source/evaluated_devel_source_build.cpp" \
    "$repo_root/source/installed_artifact_binding_observer.cpp" \
    "$repo_root/source/source_artifact_install_trusted_transport.cpp" \
    > "$test_root/expected-evaluated-build-includes.txt"
cmp -s \
    "$test_root/expected-evaluated-build-includes.txt" \
    "$evaluated_build_includes" ||
    fail 'Issue #476 Slice 4 gained a normal production caller'
assert_not_contains \
    "$repo_root/source/evaluated_devel_source_build.cpp" \
    'publish_devel_build_provenance'
assert_not_contains \
    "$repo_root/source/evaluated_devel_source_build.cpp" \
    'devel_build_provenance_store.hpp'
assert_not_contains \
    "$repo_root/source/evaluated_devel_source_build.cpp" \
    'observe_git_remote_revision('
assert_not_matches \
    "$repo_root/source/evaluated_devel_source_build.cpp" \
    '(^|[^A-Za-z])(sudo|pacman)([^A-Za-z]|$)'
assert_contains \
    "$repo_root/source/evaluated_devel_source_build.cpp" \
    'context.makepkg_executable().descriptor_'
assert_contains \
    "$repo_root/source/evaluated_devel_source_build.hpp" \
    '#include "invocation_owned_source_build_context.hpp"'
# S5-A owner exists only in the shared transport TU. No normal route or
# provenance store may construct/execute it, nor gain a live binding mint.
bridge_consumers=$test_root/evaluated-transport-consumers.txt
if grep -l -E -- \
    'evaluated_devel_source_artifact_transport|EvaluatedDevelSourceArtifactTransport' \
    "$repo_root"/source/*.cpp > "$bridge_consumers"
then
    :
else
    scan_status=$?
    [ "$scan_status" -eq 1 ] || fail "S5-A consumer scan failed: $scan_status"
fi
printf '%s\n' "$repo_root/source/reviewed_devel_source_build_execution.cpp" "$repo_root/source/source_artifact_install_trusted_transport.cpp" \
    > "$test_root/expected-evaluated-transport-consumers.txt"
cmp -s "$bridge_consumers" "$test_root/expected-evaluated-transport-consumers.txt" ||
    fail 'S5-A transport gained a normal production caller'
assert_not_matches "$repo_root/source/source_artifact_install_trusted_transport.cpp" \
    '(publish_devel_build_provenance|make_devel_build_provenance|devel_build_provenance_store|InstalledArtifactBinding::make|name_to_handle_at|InstalledDevelSourceBuildProof)'
live_observer_consumers=$test_root/live-installed-observer-consumers.txt
if grep -l -E -- '(^|[^A-Za-z0-9_])InstalledArtifactBindingObserver::observe' "$repo_root"/source/*.cpp > "$live_observer_consumers"
then
    :
else
    scan_status=$?
    [ "$scan_status" -eq 1 ] || fail "S5-B observer scan failed: $scan_status"
fi
printf '%s\n' \
    "$repo_root/source/installed_artifact_binding_observer.cpp" \
    "$repo_root/source/source_artifact_install_trusted_transport.cpp" \
    > "$test_root/expected-live-installed-observer-consumers.txt"
cmp -s "$live_observer_consumers" "$test_root/expected-live-installed-observer-consumers.txt" ||
    fail 'S5-B live observer gained an unexpected caller'
assert_not_matches "$repo_root/source/installed_artifact_binding_observer.cpp" \
    '(publish_devel_build_provenance|make_devel_build_provenance|devel_build_provenance_store|build_evaluated_devel_source\(|observe_git_remote_revision\()'
assert_contains "$repo_root/source/installed_artifact_binding.hpp" \
    '#include "installed_artifact_binding_observer_authority.hpp"'
assert_contains "$repo_root/source/installed_package_record_observation.cpp" \
    'name_to_handle_at(descriptor, "", handle, mount_id, AT_EMPTY_PATH)'
# S5-C joins only the transport-owned state, never normal CLI/source routes.
final_consumers=$test_root/installed-final-proof-consumers.txt
if grep -l -E -- \
    'devel_source_artifact_install|InstalledDevelSourceBuildProof|DevelSourceArtifactInstallAuthority|DevelSourceArtifactInstallResult' \
    "$repo_root"/source/*.cpp > "$final_consumers"
then
    :
else
    scan_status=$?
    [ "$scan_status" -eq 1 ] || fail "S5-C producer scan failed: $scan_status"
fi
printf '%s\n' \
    "$repo_root/source/devel_build_provenance_publication.cpp" \
    "$repo_root/source/devel_source_artifact_install.cpp" \
    "$repo_root/source/source_artifact_install_trusted_transport.cpp" \
    > "$test_root/expected-installed-final-proof-consumers.txt"
cmp -s "$final_consumers" "$test_root/expected-installed-final-proof-consumers.txt" ||
    fail 'S5-C final proof gained a normal production caller'
assert_not_matches "$repo_root/source/devel_source_artifact_install.cpp" \
    '(publish_devel_build_provenance|make_devel_build_provenance|devel_build_provenance_store|observe_git_remote_revision\(|observe_installed_package_record\()'
assert_contains "$repo_root/source/devel_source_artifact_install.hpp" \
    '#include "devel_source_artifact_install_authority.hpp"'
assert_not_contains "$repo_root/cmake/MoguetSources.cmake" \
    'tests/devel_source_artifact_install_fixture.cpp'
# S6-B consumes only sealed S5 output; normal route/observer callers stay absent.
publication_consumers=$test_root/provenance-publication-consumers.txt
if grep -l -E -- 'publish_installed_devel_source_build|devel_build_provenance_publication' "$repo_root"/source/*.cpp > "$publication_consumers"
then :
else
    scan_status=$?
    [ "$scan_status" -eq 1 ] || fail "S6-B publisher scan failed: $scan_status"
fi
printf '%s\n' "$repo_root/source/devel_build_provenance_publication.cpp" "$repo_root/source/reviewed_devel_source_build_execution.cpp" > "$test_root/expected-publication-consumers.txt"
cmp -s "$publication_consumers" "$test_root/expected-publication-consumers.txt" || fail 'S6-B gained a normal route caller'
assert_not_matches "$repo_root/source/devel_build_provenance_publication.cpp" \
    '(observe_installed|observe_git_remote_revision|InstalledArtifactBindingObserver|execute_exact|capture_process|read_xdg_generation_store|openat\(|fopen\(|\.path\()'
assert_not_contains "$repo_root/cmake/MoguetSources.cmake" 'tests/devel_build_provenance_publication_fixture.cpp'
# S7-B is the sole new consumer of the S7-A/#475 comparison entrances.
for entry in observe_current_installed_artifact_binding compare_devel_git_revision observe_git_remote_revision; do
    case $entry in
        observe_current_installed_artifact_binding) owner=current_installed_artifact_binding_observer ;;
        compare_devel_git_revision) owner=devel_git_revision_comparison ;;
        observe_git_remote_revision) owner=git_remote_revision_observer ;;
    esac
    if grep -l -E -- "$entry" "$repo_root"/source/*.cpp > "$test_root/s7a-$entry.txt"; then :
    else
        scan_status=$?
        [ "$scan_status" -eq 1 ] || fail "S7-A scan failed: $scan_status"
    fi
    printf '%s\n' "$repo_root/source/$owner.cpp" "$repo_root/source/devel_package_assessment.cpp" | LC_ALL=C sort > "$test_root/s7a-$entry-expected.txt"
    cmp -s "$test_root/s7a-$entry.txt" "$test_root/s7a-$entry-expected.txt" || fail "S7-B gained an unexpected observer/comparator caller: $entry"
done
if grep -l -E -- '(^|[^[:alnum:]_])assess_current_devel_package[[:space:]]*\(' "$repo_root"/source/*.cpp > "$test_root/s7b-callers.txt"; then :
else
    scan_status=$?
    [ "$scan_status" -eq 1 ] || fail "S7-B coordinator scan failed: $scan_status"
fi
printf '%s\n' "$repo_root/source/aur_devel_update.cpp" "$repo_root/source/devel_package_assessment.cpp" > "$test_root/s7b-callers-expected.txt"
cmp -s "$test_root/s7b-callers.txt" "$test_root/s7b-callers-expected.txt" || fail 'S7-B gained a normal route caller'
assert_not_matches "$repo_root/source/devel_package_assessment.cpp" \
    '(publish_.*provenance|publish_reviewed|publish_xdg|execute_exact|InstalledDevelSourceBuildProof|build_evaluated_devel_source|makepkg|checkout\(|vercmp|AurVersionRelation)'
assert_not_matches "$repo_root/source/current_installed_artifact_binding_observer.cpp" \
    '(observe_git_remote_revision|publish_devel_build_provenance|read_devel_build_provenance|read_reviewed_source_state|execute_exact|ExactArtifactTransactionReceipt|FreshInstalledArtifactBinding)'
assert_not_matches "$repo_root/source/devel_git_revision_comparison.cpp" \
    '(observe_git_remote_revision|read_.*store|capture_process|vercmp|pkgver|AurVersionRelation)'
# S7-C composes existing sealed owners; only its dedicated entrance is new.
if grep -l -E -- '(^|[^[:alnum:]_])(prepare_reviewed_production_source_execution|execute_reviewed_devel_source_build)[[:space:]]*\(' "$repo_root"/source/*.cpp > "$test_root/s7c-callers.txt"; then :
else
    scan_status=$?
    [ "$scan_status" -eq 1 ] || fail "S7-C scan failed: $scan_status"
fi
printf '%s\n' "$repo_root/source/reviewed_devel_source_build_execution.cpp" "$repo_root/source/reviewed_devel_source_route.cpp" > "$test_root/s7c-callers-expected.txt"
cmp -s "$test_root/s7c-callers.txt" "$test_root/s7c-callers-expected.txt" || fail 'S7-C gained a normal route caller'
assert_not_matches "$repo_root/source/reviewed_devel_source_build_execution.cpp" \
    '(assess_current_devel_package|observe_git_remote_revision|parse_git_remote_revision|printsrcinfo|shared_ptr<void>|static_pointer_cast|reinterpret_cast|publish_devel_build_provenance\(|execute_separated_)'
assert_not_contains "$makefile" 'legacy-cpp-focused-authority'
assert_not_contains "$makefile" '-std=c++20'
assert_not_contains "$makefile" '-Wall'
assert_not_contains "$makefile" '-Wextra'
assert_not_contains "$makefile" '-fsyntax-only'
assert_not_contains "$makefile" 'LIBALPM_CPPFLAGS'
assert_not_contains "$makefile" 'LIBALPM_LDLIBS'
assert_not_contains "$makefile" 'MY_CXXFLAGS'
assert_not_contains "$makefile" 'MY_LDLIBS'
assert_not_matches \
    "$makefile" \
    '(^|[^A-Za-z0-9_])(SRCS|TEST_SRCS|OBJS|DEPS|[A-Z0-9_]+_TEST_SRCS|[A-Z0-9_]+_OBJECTS|[A-Z0-9_]+_COMPILE_SIGNATURE|[A-Z0-9_]+_LINK_SIGNATURE)[[:space:]]*[:+?]?='
assert_not_matches \
    "$makefile" \
    '^[[:space:]]+.*\$\(CXX\).*(-c([[:space:]]|$)|-o([[:space:]]|$)|-fsyntax-only)'
assert_not_matches "$makefile" '\.cpp([[:space:]\\]|$)'

# Completion generation is repository validation, but its C++ helper must use
# the same CMake-owned compile/link contract as every other project target.
assert_not_contains "$completion_generator" '-std=c++20'
assert_not_contains "$completion_generator" '-Wall'
assert_not_contains "$completion_generator" '-Wextra'
assert_not_contains "$completion_generator" 'TemporaryDirectory'
assert_contains "$completion_generator" 'MOGUET_CLI_AUTHORITY_EXPORTER'
assert_not_contains "$completion_generator" 'MOGUET_COMPLETION_WRITE_FRONTEND'
assert_contains "$completion_generator" 'render one completion to stdout'
assert_contains \
    "$makefile" \
    'generate-completions: cmake-test-configure'
assert_contains "$makefile" '--target moguet-generate-completions'
assert_contains "$cmake_lists" 'moguet-generate-completions'
assert_contains "$cmake_lists" 'add_dependencies('
assert_contains "$completion_publisher" '--render "${_moguet_completion_shell}"'

# Root compile database publication is a post-success frontend operation, not
# a listfile side effect that can run before CMake generation has completed.
assert_not_contains "$cmake_lists" 'CREATE_LINK'
assert_not_contains "$cmake_lists" '_moguet_publish_developer_compile_commands'
assert_contains "$makefile" 'cmake-dev-configure:'
assert_contains "$makefile" 'MoguetPublishCompileCommands.cmake'
assert_contains "$compile_commands_publisher" 'CREATE_LINK'
assert_contains "$compile_commands_publisher" '"${CMAKE_COMMAND}" -E rename'

exporter_target_block=$test_root/exporter-target-block.txt
if ! awk '
    function trim(value) {
        sub(/^[[:space:]]+/, "", value)
        sub(/[[:space:]]+$/, "", value)
        return value
    }
    /^[[:space:]]*add_executable[[:space:]]*\([[:space:]]*$/ {
        in_block = 1
        block = $0 ORS
        is_exporter = 0
        is_excluded = 0
        next
    }
    in_block {
        block = block $0 ORS
        line = trim($0)
        if(line == "moguet-cli-authority-exporter") is_exporter = 1
        if(line == "EXCLUDE_FROM_ALL") is_excluded = 1
        if(line == ")") {
            if(is_exporter) {
                exporter_blocks++
                printf "%s", block
                if(!is_excluded) invalid_exporter_block = 1
            }
            in_block = 0
        }
    }
    END {
        if(exporter_blocks != 1 || invalid_exporter_block) exit 1
    }
' "$cmake_lists" > "$exporter_target_block"
then
    fail 'moguet-cli-authority-exporter is not one target-scoped EXCLUDE_FROM_ALL executable'
fi
assert_contains "$exporter_target_block" 'moguet-cli-authority-exporter'
assert_contains "$exporter_target_block" 'EXCLUDE_FROM_ALL'

# Schema version 1 is the oldest preset format (CMake 3.19); the CMake project
# itself keeps its independently declared 3.18 minimum and remains usable
# without the optional preset CLI.
assert_contains "$presets" '"version": 1'
assert_contains "$presets" '"name": "dev-debug"'
assert_contains "$presets" '"binaryDir": "${sourceDir}/build/cmake-testing"'
assert_contains "$presets" '"CXX": "g++"'
assert_contains "$presets" '"BUILD_TESTING": true'
assert_contains "$presets" '"CMAKE_BUILD_TYPE": "Debug"'
assert_contains "$presets" '"CMAKE_EXPORT_COMPILE_COMMANDS": true'
assert_contains \
    "$presets" \
    '"MOGUET_DEVELOPER_COMPILE_COMMANDS_LINK": true'
assert_contains "$repo_root/.gitignore" '/compile_commands.json'

# Compare the historical Make aliases with the actual CMake focused targets.
# This checks the frontend mapping without duplicating either inventory here.
[ "$#" -eq 126 ] ||
    fail "Make focused alias inventory is $#, expected 126"
make_aliases=$test_root/make-focused-aliases.txt
cmake_aliases=$test_root/cmake-focused-aliases.txt
cmake_help=$test_root/cmake-target-help.txt
missing_aliases=$test_root/missing-focused-aliases.txt
unexpected_aliases=$test_root/unexpected-focused-aliases.txt

printf '%s\n' "$@" | LC_ALL=C sort > "$make_aliases"
[ "$(LC_ALL=C sort -u "$make_aliases" | wc -l)" -eq 126 ] ||
    fail 'Make focused alias inventory contains duplicates'

"$cmake_command" --build "$cmake_build_dir" --target help > "$cmake_help"
sed -n \
    's/.*moguet-focus-\(test-[a-z0-9-][a-z0-9-]*\).*/\1/p' \
    "$cmake_help" | LC_ALL=C sort -u > "$cmake_aliases"
[ "$(wc -l < "$cmake_aliases")" -eq 126 ] ||
    fail "CMake focused target inventory is $(wc -l < "$cmake_aliases"), expected 126"

LC_ALL=C comm -23 "$make_aliases" "$cmake_aliases" > "$missing_aliases"
LC_ALL=C comm -13 "$make_aliases" "$cmake_aliases" > "$unexpected_aliases"
if [ -s "$missing_aliases" ] || [ -s "$unexpected_aliases" ]; then
    printf '%s\n' 'missing focused aliases:' >&2
    sed 's/^/  /' "$missing_aliases" >&2
    printf '%s\n' 'unexpected focused aliases:' >&2
    sed 's/^/  /' "$unexpected_aliases" >&2
    fail 'Make/CMake focused inventories differ'
fi

# A filesystem object named test must not suppress the canonical aggregate.
# Use a recording recursive-Make stand-in so this checks dispatch without
# recursively running the full suite inside the closure test.
phony_fixture=$test_root/phony-collision
phony_marker=$phony_fixture/recursive-targets.txt
phony_runner=$phony_fixture/record-make
mkdir -p "$phony_fixture/test"
printf '%s\n' \
    '#!/bin/sh' \
    'printf '\''%s\n'\'' "$*" >> "$MOGUET_PHONY_MARKER"' \
    > "$phony_runner"
chmod +x "$phony_runner"
MOGUET_PHONY_MARKER=$phony_marker \
env -u MAKEFLAGS -u MFLAGS \
    make -f "$makefile" -C "$phony_fixture" \
        MAKE="$phony_runner" --no-print-directory test
assert_contains "$phony_marker" 'test-cmake'
assert_contains "$phony_marker" 'test-repository'

printf '%s\n' \
    'build-authority-closure-test: Make aliases=126, CMake targets=126, missing=0, unexpected=0'
printf '%s\n' 'build-authority-closure-test: all checks passed'
