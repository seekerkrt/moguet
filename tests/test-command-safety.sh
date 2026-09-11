#!/bin/sh

# POLICY: package-command stubs are a system-safety boundary.  A missing or
# displaced stub must stop the suite before the tested binary can resolve a
# host package command later in PATH.
require_exact_test_command() {
    if [ "$#" -ne 2 ]; then
        printf '%s\n' \
            'require_exact_test_command requires a command name and expected path' >&2
        exit 1
    fi

    test_command_name=$1
    expected_test_command=$2
    case $expected_test_command in
        /*) ;;
        *)
            printf '%s\n' \
                "Test command path is not absolute: $expected_test_command" >&2
            exit 1
            ;;
    esac

    if [ ! -f "$expected_test_command" ] || [ ! -x "$expected_test_command" ]; then
        printf '%s\n' \
            "Required test stub is missing or not executable: $expected_test_command" >&2
        exit 1
    fi

    actual_test_command=$(command -v -- "$test_command_name" 2>/dev/null || true)
    if [ "$actual_test_command" != "$expected_test_command" ]; then
        printf '%s\n' \
            "Unsafe test command resolution: $test_command_name -> ${actual_test_command:-<missing>}, expected $expected_test_command" >&2
        exit 1
    fi

    resolved_expected_test_command=$(/usr/bin/readlink -f -- \
        "$expected_test_command") || {
        printf '%s\n' \
            "Cannot resolve required test stub: $expected_test_command" >&2
        exit 1
    }
    resolved_actual_test_command=$(/usr/bin/readlink -f -- \
        "$actual_test_command") || {
        printf '%s\n' \
            "Cannot resolve selected test command: $actual_test_command" >&2
        exit 1
    }
    if [ "$resolved_actual_test_command" != "$resolved_expected_test_command" ]; then
        printf '%s\n' \
            "Unsafe resolved test command: $test_command_name -> $resolved_actual_test_command, expected $resolved_expected_test_command" >&2
        exit 1
    fi

    test_repository_root=${MOGUET_TEST_REPOSITORY_ROOT:-}
    test_case_stub_root=${MOGUET_TEST_CASE_STUB_ROOT:-}
    resolved_repository_root=
    resolved_case_stub_root=
    if [ -n "$test_repository_root" ]; then
        resolved_repository_root=$(/usr/bin/readlink -f -- \
            "$test_repository_root") || exit 1
    fi
    if [ -n "$test_case_stub_root" ]; then
        resolved_case_stub_root=$(/usr/bin/readlink -f -- \
            "$test_case_stub_root") || exit 1
    fi

    case $resolved_expected_test_command in
        "$resolved_repository_root"/*)
            if [ -n "$resolved_repository_root" ]; then
                if [ "$test_command_name" = sudo ]; then
                    MOGUET_TEST_LEGACY_SUDO_STUB=$resolved_expected_test_command
                    export MOGUET_TEST_LEGACY_SUDO_STUB
                fi
                return 0
            fi
            ;;
        "$resolved_case_stub_root"/*)
            if [ -n "$resolved_case_stub_root" ]; then
                return 0
            fi
            ;;
    esac

    printf '%s\n' \
        "Required test stub resolves outside the repository and case-local stub roots: $expected_test_command -> $resolved_expected_test_command" >&2
    exit 1
}
