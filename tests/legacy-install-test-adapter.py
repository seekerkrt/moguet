#!/usr/bin/python3
"""Project the actual legacy wire/input onto the existing CLI fixture oracle.

Only the three committed sudo stubs can run. No privileged executable, helper,
PATH lookup, or arbitrary command is a fallback for missing fixture setup.
The projected log describes install intent; it is not a production exec trace.
"""

import fcntl
import hashlib
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
STUBS = tuple(ROOT / "tests/stubs" / name for name in (
    "sudo", "commands-sync/sudo", "source-maintenance/sudo"))
SEALS = fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL


def require(condition, message):
    if not condition:
        raise ValueError(message)


def selected_stub():
    configured = os.environ.get("MOGUET_TEST_LEGACY_SUDO_STUB", "")
    require(configured in tuple(map(str, STUBS)), "missing or unrecognized sudo stub")
    stub = Path(configured)
    require(stub.resolve(strict=True) == stub and not stub.is_symlink(), "redirected sudo stub")
    require(stat.S_ISREG(stub.stat().st_mode) and os.access(stub, os.X_OK), "unavailable sudo stub")
    require(bool(os.environ.get("MOGUET_TEST_COMMAND_LOG")), "missing command log")
    return stub


def verify_part(stream, path, size, digest):
    require(0 <= size <= 4 * 1024**3, "invalid snapshot size")
    if digest == "-":
        require(size == 0, "missing digest for nonempty input")
        return
    require(re.fullmatch(r"[0-9a-f]{64}", digest), "invalid digest")
    observed = hashlib.sha256()
    with path.open("rb") as original:
        remaining = size
        while remaining:
            block = stream.read(min(remaining, 65536))
            require(block and block == original.read(len(block)), "selected fixture bytes differ")
            observed.update(block)
            remaining -= len(block)
        require(not original.read(1), "selected fixture size differs")
    require(observed.hexdigest() == digest, "snapshot digest differs")


def project(helper, command, paths):
    args = shlex.split(command)
    require(len(args) >= 21 and args[:4] == ["/usr/bin/sudo", "--", helper, "install-legacy"],
            "unexpected legacy invocation")
    require(helper.startswith("/") and args[11] == "--" and (len(args) - 12) % 9 == 0,
            "invalid legacy argv shape")
    require(args[4].isdigit() and args[5].isdigit(), "invalid input descriptor")
    require(re.fullmatch(r"[0-9a-f]{64}", args[6]), "invalid transaction token")
    require(args[9] in ("0", "1") and args[10] in ("0", "1"), "invalid install flags")
    reasons = {"PreserveExistingReason": [], "AsDependency": ["--asdeps"], "AsExplicit": ["--asexplicit"]}
    require(args[8] in reasons, "invalid install reason")
    records = [args[i:i + 9] for i in range(12, len(args), 9)]
    require(len(records) == len(paths), "selected artifact count differs")
    require(len({record[0] for record in records}) == len(records), "duplicate artifact index")
    require(len({record[1] for record in records}) == len(records), "duplicate artifact identity")
    projected = ["pacman", "-U"]
    if args[10] == "1":
        projected.append("--noconfirm")
    if args[9] == "1":
        projected.append("--needed")
    projected.extend(reasons[args[8]])
    projected.append("--")
    with open(f"/proc/{args[4]}/fd/{args[5]}", "rb") as stream:
        require(fcntl.fcntl(stream, fcntl.F_GET_SEALS) & SEALS == SEALS, "unsealed legacy input")
        for record, path_text in zip(records, paths):
            require(record[0].isdigit() and record[3:5] == ["-", "-"], "invalid legacy record")
            path = Path(path_text)
            require(path.is_absolute(), "nonabsolute selected artifact")
            # Both committed pacman -Qp stubs read name/version from the
            # first line of the text archive fixture. Bind the wire identity
            # to that oracle as well as its selected path and sealed bytes.
            with path.open("rb") as original:
                identity = original.readline(4096).split()[:2]
            require(identity == [record[1].encode(), record[2].encode()], "selected fixture identity differs")
            verify_part(stream, path, int(record[5]), record[7])
            verify_part(stream, Path(path_text + ".sig"), int(record[6]), record[8])
            projected.append(path_text)
        require(not stream.read(1), "unselected snapshot bytes")
    return projected


def main():
    try:
        stub = selected_stub()
        require(len(sys.argv) >= 4, "missing adapter arguments")
        projected = project(sys.argv[1], sys.argv[2], sys.argv[3:])
        # The stub only logs/updates case-local fixtures; it never execs pacman.
        # Use an absolute interpreter and allowlisted script, never PATH sudo.
        return subprocess.run(["/bin/sh", str(stub), *projected], check=False).returncode
    except (OSError, ValueError) as error:
        print(f"legacy install test adapter refused: {error}", file=sys.stderr)
        return 125


if __name__ == "__main__":
    sys.exit(main())
