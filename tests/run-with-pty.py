#!/usr/bin/env python3

import errno
import os
import pty
import select
import signal
import sys
import termios
import time


TIMEOUT_SECONDS = 20


def usage() -> None:
    print(
        f"usage: {sys.argv[0]} [--no-input] [--timeout SECONDS] -- command [argument ...]",
        file=sys.stderr,
    )


def disable_input_echo(descriptor: int) -> None:
    attributes = termios.tcgetattr(descriptor)
    attributes[3] &= ~(termios.ECHO | termios.ECHONL)
    termios.tcsetattr(descriptor, termios.TCSANOW, attributes)


def write_all(descriptor: int, value: bytes) -> None:
    offset = 0
    while offset < len(value):
        offset += os.write(descriptor, value[offset:])


def terminate_child(pid: int) -> None:
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return


def main() -> int:
    timeout_seconds = TIMEOUT_SECONDS
    no_input = False

    argument_index = 1
    while argument_index < len(sys.argv) and sys.argv[argument_index] != "--":
        if sys.argv[argument_index] == "--no-input":
            no_input = True
            argument_index += 1
            continue

        if sys.argv[argument_index] != "--timeout":
            usage()
            return 2

        if argument_index + 1 >= len(sys.argv):
            usage()
            return 2

        timeout_text = sys.argv[argument_index + 1]
        if not timeout_text.isdigit():
            usage()
            return 2

        timeout_value = int(timeout_text)
        if timeout_value <= 0:
            usage()
            return 2

        timeout_seconds = timeout_value
        argument_index += 2

    if argument_index + 1 >= len(sys.argv) or sys.argv[argument_index] != "--":
        usage()
        return 2

    # Input-free consumers must not wait for the parent's stdin to reach EOF.
    input_bytes = b"" if no_input else sys.stdin.buffer.read()
    command = sys.argv[argument_index + 1:]
    pid, master_descriptor = pty.fork()
    if pid == 0:
        os.execvpe(command[0], command, os.environ)

    output = bytearray()
    deadline = time.monotonic() + timeout_seconds
    child_status = None
    try:
        disable_input_echo(master_descriptor)
        write_all(master_descriptor, input_bytes)

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                terminate_child(pid)
                os.waitpid(pid, 0)
                print("PTY command timed out.", file=sys.stderr)
                return 124

            readable, _, _ = select.select(
                [master_descriptor], [], [], remaining
            )
            if not readable:
                continue
            try:
                chunk = os.read(master_descriptor, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            output.extend(chunk)

        _, child_status = os.waitpid(pid, 0)
    finally:
        os.close(master_descriptor)
        sys.stdout.buffer.write(output)

    return os.waitstatus_to_exitcode(child_status)


if __name__ == "__main__":
    raise SystemExit(main())
