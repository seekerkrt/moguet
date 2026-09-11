#!/bin/sh
set -eu

repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
pty_runner=$repo_root/tests/run-with-pty.py
tmp_dir=$(mktemp -d)

cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

run_case() {
    case_name=$1
    expected_status=$2
    expected_pattern=$3
    shift 3

    output_file=$tmp_dir/$case_name.output
    if python3 "$pty_runner" "$@" </dev/null >"$output_file" 2>&1; then
        actual_status=0
    else
        actual_status=$?
    fi

    if [ "$actual_status" -ne "$expected_status" ]; then
        printf '%s\n' "unexpected status for $case_name: $actual_status" >&2
        printf 'command: %s\n' "$*" >&2
        cat "$output_file" >&2
        exit 1
    fi

    if [ -n "$expected_pattern" ]; then
        if ! grep -F -- "$expected_pattern" "$output_file" >/dev/null; then
            printf '%s\n' "expected output not observed for $case_name: $expected_pattern" >&2
            cat "$output_file" >&2
            exit 1
        fi
    fi
}

run_case default_exit_success 0 'legacy-mode' \
    -- /bin/sh -c 'printf "legacy-mode\\n"'
run_case explicit_timeout_success 0 'explicit-timeout' \
    --timeout 5 -- /bin/sh -c 'printf "explicit-timeout\\n"'
run_case default_exit_code 7 'error 7' -- /bin/sh -c 'printf "error 7\\n"; exit 7'
run_case no_input_exit_code 7 'error 7' --timeout 5 --no-input \
    -- /bin/sh -c 'printf "error 7\\n"; exit 7'
run_case timeout_expired 124 'PTY command timed out.' --timeout 1 -- /bin/sh -c 'sleep 2'
run_case usage_missing_command 2 'usage:'
run_case usage_missing_timeout_value 2 'usage:' --timeout
run_case usage_zero_timeout 2 'usage:' --timeout 0 -- /bin/true
run_case usage_negative_timeout 2 'usage:' --timeout -1 -- /bin/true
run_case usage_non_numeric_timeout 2 'usage:' --timeout abc -- /bin/true
run_case usage_no_input_missing_command 2 'usage:' --no-input --

python3 - "$pty_runner" <<'PY'
import os
import subprocess
import sys
import time

runner = sys.argv[1]

# The writer stays open until the wrapper exits. communicate() would close it
# and hide the pre-fork EOF wait that this regression must detect.
def run_with_open_stdin(name, command, expected_status, expected_output):
    started = time.monotonic()
    with subprocess.Popen(
        [sys.executable, runner, "--no-input", "--timeout", "1", "--", *command],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    ) as process:
        try:
            status = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError(f"{name}: outer timeout hid the wrapper result")
        output = process.stdout.read().decode()
        assert not process.stdin.closed, f"{name}: parent stdin was closed"
        assert status == expected_status, (name, status, output)
        assert expected_output in output, (name, output)
    print(f"{name}: PASS ({time.monotonic() - started:.2f}s)")
    return output


run_with_open_stdin("no-input open stdin", ["/bin/true"], 0, "")
output = run_with_open_stdin(
    "inner timeout before outer deadline",
    [sys.executable, "-c",
     "import os, time; print('child-pid:', os.getpid(), flush=True); time.sleep(30)"],
    124,
    "PTY command timed out.",
)
# The child's buffered output survives the timeout, and the child is reaped.
child_pid = int(next(line.split(":")[1] for line in output.splitlines()
                     if line.startswith("child-pid:")))
try:
    os.kill(child_pid, 0)
except ProcessLookupError:
    pass
else:
    raise AssertionError(f"timeout left child {child_pid} alive")

result = subprocess.run(
    [sys.executable, runner, "--", "/bin/sh", "-c",
     'IFS= read -r first; IFS= read -r second; printf "received:<%s><%s>\\n" "$first" "$second"'],
    input=b"first line\nsecond line\n",
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    timeout=5,
)
assert result.returncode == 0, (result.returncode, result.stdout)
assert b"received:<first line><second line>" in result.stdout, result.stdout
print("default stdin consumer: PASS")
PY

if ! grep -F 'TIMEOUT_SECONDS = 20' "$repo_root/tests/run-with-pty.py" >/dev/null; then
    fail 'default timeout value regression in run-with-pty.py'
fi

printf '%s\n' 'run-with-pty focused tests: all checks passed'
