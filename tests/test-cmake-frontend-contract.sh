#!/bin/sh

set -eu

repo_root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
cmake_command=${1:-cmake}
test_root=$(mktemp -d)
root_compile_commands=$repo_root/compile_commands.json
compile_commands_publisher=$repo_root/cmake/MoguetPublishCompileCommands.cmake
root_compile_commands_state=absent
root_compile_commands_backup=$test_root/root-compile-commands.backup
root_compile_commands_target=

if [ -L "$root_compile_commands" ]; then
    root_compile_commands_state=symlink
    root_compile_commands_target=$(readlink "$root_compile_commands")
elif [ -f "$root_compile_commands" ]; then
    root_compile_commands_state=file
    cp "$root_compile_commands" "$root_compile_commands_backup"
elif [ -e "$root_compile_commands" ]; then
    printf '%s\n' \
        'cmake-frontend-contract-test: repository compile_commands.json is neither a file nor a symlink' >&2
    exit 1
fi

cleanup() {
    if [ -d "$root_compile_commands" ] && [ ! -L "$root_compile_commands" ]; then
        rmdir "$root_compile_commands" 2>/dev/null || true
    else
        rm -f "$root_compile_commands"
    fi
    case $root_compile_commands_state in
        symlink)
            ln -s "$root_compile_commands_target" "$root_compile_commands"
            ;;
        file)
            cp "$root_compile_commands_backup" "$root_compile_commands"
            ;;
        absent)
            ;;
    esac
    rm -rf "$test_root"
}
trap cleanup EXIT INT TERM

fail() {
    printf 'cmake-frontend-contract-test: %s\n' "$*" >&2
    exit 1
}

cache_value() {
    cache_file=$1
    cache_key=$2
    sed -n "s/^$cache_key:[^=]*=//p" "$cache_file"
}

assert_cache_value() {
    cache_file=$1
    cache_key=$2
    expected_value=$3
    actual_value=$(cache_value "$cache_file" "$cache_key")
    [ "$actual_value" = "$expected_value" ] ||
        fail "$cache_key is '$actual_value'; expected '$expected_value'"
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

assert_compile_commands_link() {
    expected_tree=$1
    [ -L "$root_compile_commands" ] ||
        fail 'repository compile_commands.json is not a symbolic link'
    expected_compile_commands=$(readlink -f "$expected_tree/compile_commands.json")
    actual_compile_commands=$(readlink -f "$root_compile_commands")
    [ "$actual_compile_commands" = "$expected_compile_commands" ] ||
        fail "repository compile database resolves to $actual_compile_commands; expected $expected_compile_commands"
}

assert_compile_commands_absent() {
    if [ -e "$root_compile_commands" ] || [ -L "$root_compile_commands" ]; then
        fail 'repository compile_commands.json was published by a failed configure'
    fi
}

publish_compile_commands() {
    publish_tree=$1
    "$cmake_command" \
        "-DMOGUET_COMPILE_COMMANDS_BUILD_DIR=$publish_tree" \
        -P "$compile_commands_publisher"
}

configure_developer_tree() {
    developer_build_tree=$1
    shift
    "$cmake_command" -S "$repo_root" -B "$developer_build_tree" "$@" ||
        return $?
    publish_compile_commands "$developer_build_tree"
}

run_dev_preset_frontend() {
    preset_build_tree=$1
    preset_configure_args=${2:-}
    env -u MAKEFLAGS -u MFLAGS \
        make -C "$repo_root" -f "$repo_root/Makefile" \
            --no-print-directory \
            "CMAKE=$cmake_command" \
            "CMAKE_CTEST_BUILD_DIR=$preset_build_tree" \
            "CMAKE_DEV_CONFIGURE_ARGS=$preset_configure_args" \
            cmake-dev-configure
}

launcher_a=$test_root/launcher-a
launcher_b=$test_root/launcher-b
ln -s /usr/bin/env "$launcher_a"
ln -s /usr/bin/env "$launcher_b"

# Opted-in frontends must export every input, using an explicit empty value
# when no flag or launcher is requested. Undefined input is rejected before
# project() can initialize a partial persistent cache.
undefined_tree=$test_root/undefined-input
if CPPFLAGS='' CXXFLAGS='' LDFLAGS='' env -u CCACHE \
    "$cmake_command" -S "$repo_root" -B "$undefined_tree" \
        -DMOGUET_SYNC_EXTERNAL_BUILD_INPUTS=ON \
        -DBUILD_TESTING=OFF
then
    fail 'synchronized configure accepted an undefined CCACHE input'
fi
if [ -f "$undefined_tree/CMakeCache.txt" ] &&
    grep -q '^CMAKE_CXX_COMPILER:' "$undefined_tree/CMakeCache.txt"
then
    fail 'undefined synchronized input reached compiler initialization'
fi

# Direct CMake keeps explicit cache inputs authoritative even when similarly
# named environment variables are present. The frontend sync signal is absent.
direct_tree=$test_root/direct
CPPFLAGS='-DMOGUET_ENV_CPP=1' \
CXXFLAGS='-DMOGUET_ENV_CXX=1' \
LDFLAGS='-Wl,--build-id=sha1' \
CCACHE=$launcher_a \
    "$cmake_command" -S "$repo_root" -B "$direct_tree" \
        -DBUILD_TESTING=OFF \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        '-DMOGUET_CPPFLAGS:STRING=-DMOGUET_DIRECT_CPP=1' \
        '-DCMAKE_CXX_FLAGS:STRING=-DMOGUET_DIRECT_CXX=1' \
        '-DCMAKE_EXE_LINKER_FLAGS:STRING=-Wl,--build-id=md5' \
        "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING=$launcher_b" \
        -DMOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS:BOOL=ON
direct_cache=$direct_tree/CMakeCache.txt
assert_cache_value "$direct_cache" MOGUET_CPPFLAGS '-DMOGUET_DIRECT_CPP=1'
assert_cache_value "$direct_cache" CMAKE_CXX_FLAGS '-DMOGUET_DIRECT_CXX=1'
assert_cache_value \
    "$direct_cache" CMAKE_EXE_LINKER_FLAGS '-Wl,--build-id=md5'
assert_cache_value \
    "$direct_cache" CMAKE_CXX_COMPILER_LAUNCHER "$launcher_b"
assert_cache_value \
    "$direct_cache" MOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS ON
assert_contains "$direct_tree/compile_commands.json" MOGUET_DIRECT_CPP
assert_contains "$direct_tree/compile_commands.json" MOGUET_DIRECT_CXX
assert_not_contains "$direct_tree/compile_commands.json" MOGUET_ENV_CPP
assert_not_contains "$direct_tree/compile_commands.json" MOGUET_ENV_CXX
if grep -q '^MOGUET_SYNC_EXTERNAL_BUILD_INPUTS:' "$direct_cache"; then
    fail 'frontend synchronization signal leaked into the persistent cache'
fi

# With no explicit project cache value, direct CMake still initializes the
# CPPFLAGS bridge from the environment once, matching the Slice 2B contract.
direct_environment_tree=$test_root/direct-environment
CPPFLAGS='-DMOGUET_DIRECT_ENVIRONMENT_CPP=1' \
    "$cmake_command" -S "$repo_root" -B "$direct_environment_tree" \
        -DBUILD_TESTING=OFF \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
assert_cache_value \
    "$direct_environment_tree/CMakeCache.txt" \
    MOGUET_CPPFLAGS \
    '-DMOGUET_DIRECT_ENVIRONMENT_CPP=1'
assert_contains \
    "$direct_environment_tree/compile_commands.json" \
    MOGUET_DIRECT_ENVIRONMENT_CPP

# A fresh plain direct configure retains the Slice 2B -O2/-pipe defaults.
plain_tree=$test_root/plain
env -u CPPFLAGS -u CXXFLAGS -u LDFLAGS -u CCACHE \
    "$cmake_command" -S "$repo_root" -B "$plain_tree" \
        -DBUILD_TESTING=OFF \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
assert_cache_value \
    "$plain_tree/CMakeCache.txt" MOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS ON
assert_contains "$plain_tree/compile_commands.json" '-O2 -pipe'

# The developer compile-database bridge is opt-in, requires database
# generation, overwrites only the exact generated root artifact, and follows
# the currently configured tree across reconfigure and tree recreation.
invalid_developer_tree=$test_root/invalid-developer-link
if configure_developer_tree "$invalid_developer_tree" \
    -DBUILD_TESTING=OFF \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON
then
    fail 'developer compile database link accepted export disabled'
fi

developer_tree=$test_root/developer
rm -f "$root_compile_commands"
assert_compile_commands_absent
configure_developer_tree "$developer_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON
assert_compile_commands_link "$developer_tree"

# A failure after project() and dependency discovery must retain the previous
# valid publication. The same failure on a first configure must publish
# nothing, including no dangling symlink.
failed_developer_tree=$test_root/failed-developer
if configure_developer_tree "$failed_developer_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON \
    -DMOGUET_LOCALE_DIRECTORY=relative
then
    fail 'post-project developer configure failure injection succeeded'
fi
assert_compile_commands_link "$developer_tree"

rm -f "$root_compile_commands"
failed_first_tree=$test_root/failed-first-developer
if configure_developer_tree "$failed_first_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON \
    -DMOGUET_LOCALE_DIRECTORY=relative
then
    fail 'first post-project developer configure failure injection succeeded'
fi
assert_compile_commands_absent

configure_developer_tree "$developer_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON
assert_compile_commands_link "$developer_tree"

# CMAKE_PROJECT_INCLUDE injects a generator expression whose target does not
# exist. Configuration reaches "Configuring done", but generation fails. The
# post-success frontend must therefore preserve the previous publication.
generation_failure_include=$test_root/generation-failure.cmake
printf '%s\n' \
    'add_custom_target(' \
    '    moguet-generation-failure ALL' \
    '    COMMAND' \
    '        "${CMAKE_COMMAND}" -E echo' \
    '        "$<TARGET_PROPERTY:moguet-generation-missing-target,NAME>"' \
    ')' \
    > "$generation_failure_include"

failed_generation_tree=$test_root/failed-generation-developer
failed_generation_log=$test_root/failed-generation-developer.log
if configure_developer_tree "$failed_generation_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON \
    "-DCMAKE_PROJECT_INCLUDE=$generation_failure_include" \
    > "$failed_generation_log" 2>&1
then
    fail 'generation-phase developer failure injection succeeded'
fi
assert_contains "$failed_generation_log" 'Configuring done'
assert_contains "$failed_generation_log" 'CMake Generate step failed'
assert_compile_commands_link "$developer_tree"

rm -f "$root_compile_commands"
failed_generation_first_tree=$test_root/failed-generation-first-developer
failed_generation_first_log=$test_root/failed-generation-first-developer.log
if configure_developer_tree "$failed_generation_first_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON \
    "-DCMAKE_PROJECT_INCLUDE=$generation_failure_include" \
    > "$failed_generation_first_log" 2>&1
then
    fail 'first generation-phase developer failure injection succeeded'
fi
assert_contains "$failed_generation_first_log" 'Configuring done'
assert_contains "$failed_generation_first_log" 'CMake Generate step failed'
assert_compile_commands_absent

configure_developer_tree "$developer_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON
assert_compile_commands_link "$developer_tree"

rm -f "$root_compile_commands"
printf '%s\n' 'stale generated compile database' > "$root_compile_commands"
configure_developer_tree "$developer_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON
assert_compile_commands_link "$developer_tree"

rm -f "$root_compile_commands"
ln -s "$test_root/missing-compile-database" "$root_compile_commands"
configure_developer_tree "$developer_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON
assert_compile_commands_link "$developer_tree"

rm -f "$root_compile_commands"
mkdir "$root_compile_commands"
if configure_developer_tree "$developer_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON
then
    fail 'developer compile database publication replaced a directory'
fi
[ -d "$root_compile_commands" ] ||
    fail 'failed publication removed the unexpected compile database directory'
rmdir "$root_compile_commands"
configure_developer_tree "$developer_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON
assert_compile_commands_link "$developer_tree"

rm -rf "$developer_tree"
configure_developer_tree "$developer_tree" \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOGUET_DEVELOPER_COMPILE_COMMANDS_LINK=ON
assert_compile_commands_link "$developer_tree"

# Presets were introduced in CMake 3.19. The project itself remains directly
# configurable with its declared CMake 3.18 minimum; newer hosts additionally
# exercise the tracked dev-debug workflow.
cmake_version=$("$cmake_command" --version | sed -n '1s/^[^0-9]*\([0-9][0-9.]*\).*$/\1/p')
cmake_major=${cmake_version%%.*}
cmake_minor_patch=${cmake_version#*.}
cmake_minor=${cmake_minor_patch%%.*}
if [ "$cmake_major" -gt 3 ] || \
    { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -ge 19 ]; }
then
    preset_tree=$test_root/preset-developer
    preset_failure_tree=$test_root/preset-failure
    preset_first_failure_tree=$test_root/preset-first-failure
    raw_preset_tree=$test_root/raw-preset-developer
    if run_dev_preset_frontend \
        "$preset_failure_tree" \
        -DMOGUET_LOCALE_DIRECTORY=relative
    then
        fail 'dev-debug preset failure injection succeeded'
    fi
    assert_compile_commands_link "$developer_tree"

    rm -f "$root_compile_commands"
    if run_dev_preset_frontend \
        "$preset_first_failure_tree" \
        -DMOGUET_LOCALE_DIRECTORY=relative
    then
        fail 'first dev-debug preset failure injection succeeded'
    fi
    assert_compile_commands_absent

    # Raw preset invocation remains configure-only. It must not bypass the
    # documented frontend's post-process-success publication boundary.
    ln -s "$developer_tree/compile_commands.json" "$root_compile_commands"
    (
        cd "$repo_root"
        "$cmake_command" --preset dev-debug -B "$raw_preset_tree"
    )
    assert_compile_commands_link "$developer_tree"

    run_dev_preset_frontend "$preset_tree"
    assert_cache_value "$preset_tree/CMakeCache.txt" BUILD_TESTING TRUE
    assert_cache_value "$preset_tree/CMakeCache.txt" CMAKE_BUILD_TYPE Debug
    assert_cache_value \
        "$preset_tree/CMakeCache.txt" \
        CMAKE_CXX_COMPILER \
        "$(command -v g++)"
    assert_cache_value \
        "$preset_tree/CMakeCache.txt" CMAKE_EXPORT_COMPILE_COMMANDS TRUE
    assert_cache_value \
        "$preset_tree/CMakeCache.txt" \
        MOGUET_DEVELOPER_COMPILE_COMMANDS_LINK \
        TRUE
    assert_compile_commands_link "$preset_tree"

    rm -f "$root_compile_commands"
    ln -s "$developer_tree/compile_commands.json" "$root_compile_commands"
    run_dev_preset_frontend "$preset_tree"
    assert_compile_commands_link "$preset_tree"
else
    printf '%s\n' \
        "cmake-frontend-contract-test: dev-debug preset NOT RUN (CMake $cmake_version lacks preset support)"
fi

printf '%s\n' \
    'cmake-frontend-contract-test: compile database success/failure publication checks passed'

run_compiler_preflight() {
    preflight_tree=$1
    requested_compiler=$2
    "$cmake_command" \
        "-DMOGUET_COMPILER_PREFLIGHT_BUILD_DIR=$preflight_tree" \
        "-DMOGUET_REQUESTED_CXX=$requested_compiler" \
        -P "$repo_root/cmake/MoguetCompilerPreflight.cmake"
}

configure_synced_tree() {
    synced_tree=$1
    build_testing=$2
    build_type=$3
    cppflags=$4
    cxxflags=$5
    ldflags=$6
    ccache=$7
    requested_compiler=${8:-g++}

    run_compiler_preflight "$synced_tree" "$requested_compiler"
    CPPFLAGS=$cppflags \
    CXXFLAGS=$cxxflags \
    LDFLAGS=$ldflags \
    CCACHE=$ccache \
    CXX=$requested_compiler \
        "$cmake_command" -S "$repo_root" -B "$synced_tree" \
            -DCMAKE_BUILD_TYPE="$build_type" \
            -DCMAKE_INSTALL_PREFIX=/opt/moguet-contract \
            -DCMAKE_INSTALL_BINDIR=/custom/bin \
            -DMOGUET_INSTALL_DOCUMENT_DIRECTORY=/custom/doc/moguet \
            -DMOGUET_LOCALE_DIRECTORY=/custom/locale \
            -DMOGUET_SYNC_EXTERNAL_BUILD_INPUTS=ON \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
            -DBUILD_TESTING="$build_testing"
}

verify_synced_state() {
    synced_tree=$1
    target=$2
    target_directory=$3
    state=$4
    cppflags=$5
    cxxflags=$6
    ldflags=$7
    ccache=$8
    build_log=$test_root/build-$state.log

    assert_cache_value "$synced_tree/CMakeCache.txt" MOGUET_CPPFLAGS "$cppflags"
    assert_cache_value "$synced_tree/CMakeCache.txt" CMAKE_CXX_FLAGS "$cxxflags"
    assert_cache_value \
        "$synced_tree/CMakeCache.txt" CMAKE_EXE_LINKER_FLAGS "$ldflags"
    assert_cache_value \
        "$synced_tree/CMakeCache.txt" CMAKE_CXX_COMPILER_LAUNCHER "$ccache"
    "$cmake_command" --build "$synced_tree" \
        --target "$target" --clean-first --verbose > "$build_log" 2>&1

    link_command=$synced_tree/CMakeFiles/$target_directory.dir/link.txt
    [ -f "$link_command" ] || fail "link command is missing: $link_command"
    if [ -n "$cppflags" ]; then
        assert_contains "$build_log" "$cppflags"
    fi
    if [ -n "$cxxflags" ]; then
        assert_contains "$build_log" "$cxxflags"
    fi
    if [ -n "$ldflags" ]; then
        assert_contains "$link_command" "$ldflags"
    fi
    if [ -n "$ccache" ]; then
        assert_contains "$build_log" "$ccache"
        assert_not_contains "$link_command" "$ccache"
    fi
}

exercise_synced_tree() {
    synced_tree=$1
    build_testing=$2
    build_type=$3
    target=$4
    target_directory=$5
    label=$6

    configure_synced_tree \
        "$synced_tree" "$build_testing" "$build_type" \
        '-DMOGUET_SYNC_CPP_A=1' \
        '-O1 -DMOGUET_SYNC_CXX_A=1' \
        '-Wl,--build-id=sha1' \
        "$launcher_a"
    verify_synced_state \
        "$synced_tree" "$target" "$target_directory" "$label-a" \
        '-DMOGUET_SYNC_CPP_A=1' \
        '-O1 -DMOGUET_SYNC_CXX_A=1' \
        '-Wl,--build-id=sha1' \
        "$launcher_a"

    configure_synced_tree \
        "$synced_tree" "$build_testing" "$build_type" \
        '-DMOGUET_SYNC_CPP_B=1' \
        '-O3 -DMOGUET_SYNC_CXX_B=1' \
        '-Wl,--build-id=md5' \
        "$launcher_b"
    verify_synced_state \
        "$synced_tree" "$target" "$target_directory" "$label-b" \
        '-DMOGUET_SYNC_CPP_B=1' \
        '-O3 -DMOGUET_SYNC_CXX_B=1' \
        '-Wl,--build-id=md5' \
        "$launcher_b"

    configure_synced_tree \
        "$synced_tree" "$build_testing" "$build_type" '' '' '' ''
    verify_synced_state \
        "$synced_tree" "$target" "$target_directory" "$label-empty" \
        '' '' '' ''
    assert_not_contains "$synced_tree/compile_commands.json" MOGUET_SYNC_CPP_A
    assert_not_contains "$synced_tree/compile_commands.json" MOGUET_SYNC_CPP_B
    assert_not_contains "$synced_tree/compile_commands.json" MOGUET_SYNC_CXX_A
    assert_not_contains "$synced_tree/compile_commands.json" MOGUET_SYNC_CXX_B
    assert_not_contains \
        "$synced_tree/compile_commands.json" "$launcher_a"
    assert_not_contains \
        "$synced_tree/compile_commands.json" "$launcher_b"
    assert_not_contains "$test_root/build-$label-empty.log" "$launcher_a"
    assert_not_contains "$test_root/build-$label-empty.log" "$launcher_b"
    assert_cache_value \
        "$synced_tree/CMakeCache.txt" MOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS OFF
}

assert_synced_configuration() {
    synced_tree=$1
    build_testing=$2
    build_type=$3

    assert_cache_value "$synced_tree/CMakeCache.txt" BUILD_TESTING "$build_testing"
    assert_cache_value "$synced_tree/CMakeCache.txt" CMAKE_BUILD_TYPE "$build_type"
    assert_cache_value \
        "$synced_tree/CMakeCache.txt" CMAKE_INSTALL_PREFIX /opt/moguet-contract
    assert_cache_value \
        "$synced_tree/CMakeCache.txt" CMAKE_INSTALL_BINDIR /custom/bin
    assert_cache_value \
        "$synced_tree/CMakeCache.txt" \
        MOGUET_INSTALL_DOCUMENT_DIRECTORY \
        /custom/doc/moguet
    assert_cache_value \
        "$synced_tree/CMakeCache.txt" MOGUET_LOCALE_DIRECTORY /custom/locale
}

reconfigure_with_equivalent_compiler() {
    synced_tree=$1
    build_testing=$2
    build_type=$3
    requested_compiler=$4
    label=$5
    cache_file=$synced_tree/CMakeCache.txt
    cache_snapshot=$test_root/$label-CMakeCache.before
    configure_log=$test_root/$label-configure.log
    configured_compiler=$(cache_value "$cache_file" CMAKE_CXX_COMPILER)

    cp "$cache_file" "$cache_snapshot"
    if configure_synced_tree \
        "$synced_tree" "$build_testing" "$build_type" \
        '' '' '' '' "$requested_compiler" \
        > "$configure_log" 2>&1
    then
        :
    else
        configure_status=$?
        cat "$configure_log" >&2
        fail "$label configure failed with status $configure_status"
    fi

    cmp -s "$cache_snapshot" "$cache_file" ||
        fail "$label configure changed the stable CMake cache"
    assert_cache_value \
        "$cache_file" CMAKE_CXX_COMPILER "$configured_compiler"
    assert_synced_configuration "$synced_tree" "$build_testing" "$build_type"
    assert_not_contains \
        "$configure_log" \
        'You have changed variables that require your cache to be deleted.'
    if [ "$build_testing" = ON ]; then
        assert_contains \
            "$configure_log" \
            'Moguet C++ tests: targets=115/115, support=32/32, firewalls=50/50, descriptors=50/50, CTest registrations=146'
    else
        assert_not_contains "$configure_log" 'Moguet C++ tests:'
    fi
}

production_tree=$test_root/production
testing_tree=$test_root/testing
package_tree=$test_root/package
exercise_synced_tree \
    "$production_tree" OFF '' \
    moguet-uninstall-helper moguet-uninstall-helper production
exercise_synced_tree \
    "$testing_tree" ON '' \
    application-identity-test application-identity-test testing
exercise_synced_tree \
    "$package_tree" OFF None \
    moguet-uninstall-helper moguet-uninstall-helper package

assert_synced_configuration "$production_tree" OFF ''
assert_synced_configuration "$testing_tree" ON ''
assert_synced_configuration "$package_tree" OFF None

# A multiword CXX initializes CMake's executable and immutable ARG1 fields as
# one compiler identity. Reuse accepts equivalent executable spellings with
# the same required argument and rejects argument changes before configure.
multiword_tree=$test_root/multiword-compiler
configure_synced_tree \
    "$multiword_tree" OFF '' '' '' '' '' 'g++ -m64'
assert_cache_value \
    "$multiword_tree/CMakeCache.txt" \
    CMAKE_CXX_COMPILER \
    "$(command -v g++)"
assert_cache_value \
    "$multiword_tree/CMakeCache.txt" CMAKE_CXX_COMPILER_ARG1 ' -m64'
assert_synced_configuration "$multiword_tree" OFF ''
# The first reuse may stabilize unrelated find-package INTERNAL cache entries;
# assert the compiler identity fields rather than requiring whole-cache byte
# equality for that warm reconfigure.
configure_synced_tree \
    "$multiword_tree" OFF '' '' '' '' '' 'g++ -m64'
assert_cache_value \
    "$multiword_tree/CMakeCache.txt" \
    CMAKE_CXX_COMPILER \
    "$(command -v g++)"
assert_cache_value \
    "$multiword_tree/CMakeCache.txt" CMAKE_CXX_COMPILER_ARG1 ' -m64'
reconfigure_with_equivalent_compiler \
    "$multiword_tree" OFF '' '/usr/bin/g++ -m64' multiword-spelling

multiword_cache=$multiword_tree/CMakeCache.txt
for multiword_case in changed removed
do
    case $multiword_case in
        changed) changed_multiword_cxx='g++ -m32' ;;
        removed) changed_multiword_cxx='g++' ;;
    esac
    multiword_snapshot=$test_root/multiword-$multiword_case.before
    cp "$multiword_cache" "$multiword_snapshot"
    if run_compiler_preflight "$multiword_tree" "$changed_multiword_cxx"; then
        fail "required compiler argument change unexpectedly succeeded: $changed_multiword_cxx"
    fi
    cmp -s "$multiword_snapshot" "$multiword_cache" ||
        fail "required compiler argument rejection mutated $multiword_cache"
done

# Equivalent compiler spellings must pass the complete preflight + configure
# path without changing CMake's cached spelling or any frontend invariant.
compiler_alias=$test_root/compiler-alias
ln -s "$(command -v g++)" "$compiler_alias"
reconfigure_with_equivalent_compiler \
    "$production_tree" OFF '' /usr/bin/g++ production-absolute-compiler
reconfigure_with_equivalent_compiler \
    "$production_tree" OFF '' "$compiler_alias" production-symlink-compiler
reconfigure_with_equivalent_compiler \
    "$testing_tree" ON '' "$compiler_alias" testing-symlink-compiler
reconfigure_with_equivalent_compiler \
    "$package_tree" OFF None "$compiler_alias" package-symlink-compiler

if grep -F -- '-DCMAKE_CXX_COMPILER=' "$repo_root/Makefile" "$repo_root/PKGBUILD"
then
    fail 'a canonical frontend re-passes raw CXX as CMAKE_CXX_COMPILER'
fi

# A genuinely different compiler still fails before CMake can regenerate or
# mutate the cache.

different_compiler=$(command -v clang++ || command -v false)
for mismatch_tree in "$production_tree" "$testing_tree" "$package_tree"
do
    cache_snapshot=$mismatch_tree/CMakeCache.before-mismatch
    cp "$mismatch_tree/CMakeCache.txt" "$cache_snapshot"
    if run_compiler_preflight "$mismatch_tree" "$different_compiler"; then
        fail "compiler mismatch unexpectedly succeeded for $mismatch_tree"
    fi
    cmp -s "$cache_snapshot" "$mismatch_tree/CMakeCache.txt" ||
        fail "compiler mismatch mutated $mismatch_tree/CMakeCache.txt"
done

if command -v clang++ >/dev/null 2>&1; then
    reverse_tree=$test_root/reverse-compiler
    CPPFLAGS='' CXXFLAGS='' LDFLAGS='' CCACHE='' CXX=clang++ \
        "$cmake_command" -S "$repo_root" -B "$reverse_tree" \
            -DMOGUET_SYNC_EXTERNAL_BUILD_INPUTS=ON \
            -DBUILD_TESTING=OFF
    if run_compiler_preflight "$reverse_tree" g++; then
        fail 'clang++ to g++ compiler mismatch unexpectedly succeeded'
    fi
fi

# The canonical completion write frontend must build the CMake exporter first.
# Exercise it in an isolated minimal source tree so an authority edit can make
# the already-built exporter stale without touching the working repository.
completion_fixture=$test_root/completion-freshness
completion_baseline=$test_root/completion-baseline
mkdir -p \
    "$completion_fixture/cmake" \
    "$completion_fixture/source" \
    "$completion_fixture/scripts" \
    "$completion_fixture/completions/descriptions" \
    "$completion_baseline"
cp \
    "$repo_root/cmake/MoguetCompilerPreflight.cmake" \
    "$completion_fixture/cmake/MoguetCompilerPreflight.cmake"
cp \
    "$repo_root/cmake/MoguetGenerateCompletions.cmake" \
    "$repo_root/cmake/MoguetPublishCompileCommands.cmake" \
    "$completion_fixture/cmake/"
cp \
    "$repo_root/scripts/export_cli_authority.cpp" \
    "$repo_root/scripts/generate_completions.py" \
    "$completion_fixture/scripts/"
cp \
    "$repo_root/source/cli_authority.hpp" \
    "$repo_root/source/cli_public_projection.cpp" \
    "$repo_root/source/cli_public_projection.hpp" \
    "$completion_fixture/source/"
cp \
    "$repo_root/completions/descriptions/en.json" \
    "$completion_fixture/completions/descriptions/en.json"
printf '%s\n' \
    'cmake_minimum_required(VERSION 3.18)' \
    'project(MoguetCompletionFreshness LANGUAGES CXX)' \
    'option(MOGUET_DEVELOPER_COMPILE_COMMANDS_LINK "" OFF)' \
    'add_executable(' \
    '    moguet-cli-authority-exporter' \
    '    EXCLUDE_FROM_ALL' \
    '    scripts/export_cli_authority.cpp' \
    '    source/cli_public_projection.cpp' \
    ')' \
    'set_target_properties(' \
    '    moguet-cli-authority-exporter' \
    '    PROPERTIES' \
    '        CXX_STANDARD 20' \
    '        CXX_STANDARD_REQUIRED YES' \
    '        CXX_EXTENSIONS NO' \
    ')' \
    'target_include_directories(' \
    '    moguet-cli-authority-exporter' \
    '    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source"' \
    ')' \
    'add_custom_target(' \
    '    moguet-generate-completions' \
    '    COMMAND' \
    '        "${CMAKE_COMMAND}"' \
    '        "-DMOGUET_COMPLETION_PYTHON=python3"' \
    '        "-DMOGUET_COMPLETION_GENERATOR=${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_completions.py"' \
    '        "-DMOGUET_COMPLETION_EXPORTER=$<TARGET_FILE:moguet-cli-authority-exporter>"' \
    '        "-DMOGUET_COMPLETION_OUTPUT_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/completions"' \
    '        -P' \
    '        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MoguetGenerateCompletions.cmake"' \
    '    VERBATIM' \
    ')' \
    'add_dependencies(' \
    '    moguet-generate-completions' \
    '    moguet-cli-authority-exporter' \
    ')' \
    > "$completion_fixture/CMakeLists.txt"

(
    cd "$completion_fixture"
    env -u MAKEFLAGS -u MFLAGS \
        make -f "$repo_root/Makefile" \
            CXX='g++ -m64' generate-completions
)
completion_exporter=$completion_fixture/build/cmake-testing/moguet-cli-authority-exporter
completion_exporter_before=$test_root/completion-exporter.before
cp "$completion_exporter" "$completion_exporter_before"
completion_outputs='moguet.bash _moguet moguet.fish'
for completion_output in $completion_outputs
do
    cp \
        "$completion_fixture/completions/$completion_output" \
        "$completion_baseline/$completion_output"
done

completion_authority=$completion_fixture/source/cli_public_projection.cpp
awk '
    $0 == "    OperationId::Build," {
        print "    OperationId::Upgrade,"
        next
    }
    $0 == "    OperationId::Upgrade," {
        print "    OperationId::Build,"
        next
    }
    { print }
' "$completion_authority" > "$completion_authority.next"
mv "$completion_authority.next" "$completion_authority"

# A caller-controlled marker cannot grant tracked write authority. Python has
# no write mode and must leave all three last-generated files unchanged.
if MOGUET_CLI_AUTHORITY_EXPORTER=$completion_exporter \
    MOGUET_COMPLETION_WRITE_FRONTEND=1 \
    PYTHONDONTWRITEBYTECODE=1 \
        python3 "$completion_fixture/scripts/generate_completions.py"
then
    fail 'direct completion write mode accepted a potentially stale exporter'
fi
for completion_output in $completion_outputs
do
    cmp -s \
        "$completion_baseline/$completion_output" \
        "$completion_fixture/completions/$completion_output" ||
        fail "stale exporter unexpectedly changed $completion_output"
done

(
    cd "$completion_fixture"
    env -u MAKEFLAGS -u MFLAGS \
        make -f "$repo_root/Makefile" \
            CXX='g++ -m64' generate-completions
)
cmp -s "$completion_exporter_before" "$completion_exporter" &&
    fail 'canonical completion write frontend did not rebuild the stale exporter'
for completion_output in $completion_outputs
do
    if cmp -s \
        "$completion_baseline/$completion_output" \
        "$completion_fixture/completions/$completion_output"
    then
        fail "rebuilt completion exporter did not update $completion_output"
    fi
done
MOGUET_CLI_AUTHORITY_EXPORTER=$completion_exporter \
PYTHONDONTWRITEBYTECODE=1 \
    python3 "$completion_fixture/scripts/generate_completions.py" --check

printf '%s\n' \
    'cmake-frontend-contract-test: completion marker bypass and canonical rebuild checks passed'

printf 'cmake-frontend-contract-test: all checks passed\n'
