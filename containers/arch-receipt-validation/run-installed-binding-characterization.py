#!/usr/bin/python3

import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys


PACKAGE_NAME = "moguet-source-receipt-single"
PACKAGE_ROOT = Path("/home/moguet-validation/receipt-packages")
ALTERNATE_PACKAGE_ROOT = Path(
    "/home/moguet-validation/receipt-packages-alternate"
)
STATE_ROOT = Path("/var/lib/moguet-installed-binding-characterization")
INSTALL_ROOT = STATE_ROOT / "root"
DATABASE_ROOT = STATE_ROOT / "db"
CACHE_ROOT = STATE_ROOT / "cache"
PACMAN_LOG = STATE_ROOT / "pacman.log"
PROBE_USER = "moguet-validation"
MAX_SAME_SECOND_ATTEMPTS = 32
MAX_FILE_HANDLE_BYTES = 128
AT_FDCWD = -100


def fail(message: str) -> None:
    print(
        f"installed-binding-characterization: {message}",
        file=sys.stderr,
    )
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_bytes(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def archive_mtree_sha256(path: Path) -> str:
    result = subprocess.run(
        ["/usr/bin/bsdtar", "-xOf", str(path), ".MTREE"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
    )
    require(
        result.returncode == 0,
        f"unable to read archive ALPM-MTREE: {result.stderr.decode(errors='replace')}",
    )
    return sha256_bytes(result.stdout)


def filesystem_identity(path: Path) -> str:
    result = subprocess.run(
        ["/usr/bin/stat", "-f", "-c", "%T:%t:%i", "--", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
    )
    require(
        result.returncode == 0,
        f"unable to observe filesystem identity: {result.stderr.strip()}",
    )
    identity = result.stdout.strip()
    require(identity and "\n" not in identity, "filesystem identity is malformed")
    return identity


def name_to_handle_identity(path: Path) -> dict[str, object]:
    library = ctypes.CDLL(None, use_errno=True)
    mount_id = ctypes.c_int()

    sizing_buffer = ctypes.create_string_buffer(8)
    ctypes.c_uint.from_buffer(sizing_buffer).value = 0
    result = library.name_to_handle_at(
        AT_FDCWD,
        os.fsencode(path),
        sizing_buffer,
        ctypes.byref(mount_id),
        0,
    )
    error_number = ctypes.get_errno()
    required_bytes = ctypes.c_uint.from_buffer(sizing_buffer).value
    if result != -1 or error_number != errno.EOVERFLOW:
        return {
            "failure": "UnsupportedGeneration",
            "errno": error_number,
            "errno_name": errno.errorcode.get(error_number, "UNKNOWN"),
        }
    if required_bytes == 0 or required_bytes > MAX_FILE_HANDLE_BYTES:
        return {
            "failure": "MalformedBinding",
            "handle_bytes": required_bytes,
        }

    handle_buffer = ctypes.create_string_buffer(8 + required_bytes)
    ctypes.c_uint.from_buffer(handle_buffer).value = required_bytes
    result = library.name_to_handle_at(
        AT_FDCWD,
        os.fsencode(path),
        handle_buffer,
        ctypes.byref(mount_id),
        0,
    )
    if result != 0:
        error_number = ctypes.get_errno()
        return {
            "failure": "UnsupportedGeneration",
            "errno": error_number,
            "errno_name": errno.errorcode.get(error_number, "UNKNOWN"),
        }

    actual_bytes = ctypes.c_uint.from_buffer(handle_buffer).value
    if actual_bytes != required_bytes:
        return {
            "failure": "MalformedBinding",
            "sized_handle_bytes": required_bytes,
            "actual_handle_bytes": actual_bytes,
        }
    handle_type = ctypes.c_int.from_buffer(handle_buffer, 4).value
    handle = bytes(handle_buffer[8 : 8 + actual_bytes]).hex()
    fs_identity = filesystem_identity(path)
    return {
        "scheme": "linux-name-to-handle-at-v1",
        "filesystem": fs_identity,
        "handle_type": handle_type,
        "handle_bytes": handle,
        # mount_id is deliberately excluded: it identifies the current mount,
        # not the persistent filesystem-scoped handle generation.
        "opaque_identity": (
            "linux-name-to-handle-at-v1"
            f"|filesystem={fs_identity}"
            f"|type={handle_type}"
            f"|bytes={handle}"
        ),
    }


def probe_record_mode(path_argument: str) -> int:
    record = Path(path_argument)
    local_root = (DATABASE_ROOT / "local").resolve(strict=True)
    resolved = record.resolve(strict=True)
    require(
        resolved.parent == local_root,
        "record probe escaped the isolated local database",
    )
    metadata = resolved.lstat()
    require(stat.S_ISDIR(metadata.st_mode), "record probe target is not a directory")
    require(not record.is_symlink(), "record probe target is a symlink")
    observation = name_to_handle_identity(resolved)
    print(json.dumps(observation, sort_keys=True, separators=(",", ":")))
    return 3 if "failure" in observation else 0


def probe_generation(record: Path) -> dict[str, object]:
    command = [
        "/usr/bin/runuser",
        "-u",
        PROBE_USER,
        "--",
        "/usr/bin/python3",
        str(Path(__file__).resolve()),
        "--probe-record",
        str(record),
    ]
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
    )
    try:
        observation = json.loads(result.stdout)
    except json.JSONDecodeError:
        fail(
            "record generation probe returned malformed output: "
            f"status={result.returncode} stdout={result.stdout!r} "
            f"stderr={result.stderr!r}"
        )
    if result.returncode == 3:
        fail(
            "installed binding authority is unsupported on this filesystem/runtime: "
            f"{observation}"
        )
    require(
        result.returncode == 0 and "failure" not in observation,
        f"record generation probe failed: {observation}",
    )
    return observation


def parse_desc(path: Path) -> dict[str, str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    values: dict[str, str] = {}
    for key in (
        "NAME",
        "VERSION",
        "BASE",
        "ARCH",
        "BUILDDATE",
        "INSTALLDATE",
        "PACKAGER",
    ):
        marker = f"%{key}%"
        indexes = [index for index, line in enumerate(lines) if line == marker]
        require(
            len(indexes) == 1 and indexes[0] + 1 < len(lines),
            f"installed desc is missing a singular {key} record",
        )
        values[key] = lines[indexes[0] + 1]
    for key in ("ISIZE", "REASON"):
        marker = f"%{key}%"
        indexes = [index for index, line in enumerate(lines) if line == marker]
        require(len(indexes) <= 1, f"installed desc duplicated {key}")
        require(
            not indexes or indexes[0] + 1 < len(lines),
            f"installed desc has no value for {key}",
        )
        values[key] = (
            "MISSING"
            if not indexes
            else lines[indexes[0] + 1]
        )
    return values


def installed_state() -> dict[str, object] | None:
    local_root = DATABASE_ROOT / "local"
    if not local_root.exists():
        return None
    records = sorted(local_root.glob(f"{PACKAGE_NAME}-*"))
    if not records:
        return None
    require(len(records) == 1, f"installed record inventory is not singular: {records}")
    record = records[0]
    metadata = record.stat(follow_symlinks=False)
    require(stat.S_ISDIR(metadata.st_mode), "installed record is not a directory")

    generation = probe_generation(record)
    later_generation = probe_generation(record)
    require(
        generation == later_generation,
        "opaque record generation was not stable across a later normal-user process read",
    )

    desc_path = record / "desc"
    files_path = record / "files"
    mtree_path = record / "mtree"
    for path in (desc_path, files_path, mtree_path):
        require(path.is_file() and not path.is_symlink(), f"missing local DB file: {path}")
    desc = parse_desc(desc_path)
    require(desc["NAME"] == PACKAGE_NAME, "installed package name changed")
    return {
        "record": record.name,
        "generation": generation["opaque_identity"],
        "filesystem": generation["filesystem"],
        "handle_type": generation["handle_type"],
        "handle_bytes": generation["handle_bytes"],
        "inode": metadata.st_ino,
        "ctime_ns": metadata.st_ctime_ns,
        "mtime_ns": metadata.st_mtime_ns,
        "package_base": desc["BASE"],
        "version": desc["VERSION"],
        "architecture": desc["ARCH"],
        "build_date": desc["BUILDDATE"],
        "install_time": desc["INSTALLDATE"],
        "packager": desc["PACKAGER"],
        "installed_size": desc["ISIZE"],
        "install_reason": desc["REASON"],
        "mtree_digest": sha256_file(mtree_path),
        "desc_files_digest": sha256_bytes(
            b"desc\0"
            + desc_path.read_bytes()
            + b"files\0"
            + files_path.read_bytes()
        ),
    }


def log_delta(before: bytes) -> str:
    after = PACMAN_LOG.read_bytes() if PACMAN_LOG.exists() else b""
    require(after.startswith(before), "pacman log prefix changed during transaction")
    return after[len(before) :].decode("utf-8", errors="strict")


def classify_operation(output: str, delta: str, needed: bool) -> tuple[str, str]:
    markers = {
        "Install": f"[ALPM] installed {PACKAGE_NAME} (",
        "Upgrade": f"[ALPM] upgraded {PACKAGE_NAME} (",
        "Reinstall": f"[ALPM] reinstalled {PACKAGE_NAME} (",
    }
    observed = [
        (operation, line)
        for operation, marker in markers.items()
        for line in delta.splitlines()
        if marker in line
    ]
    if len(observed) == 1:
        return observed[0]
    if needed and not observed and (
        "is up to date -- skipping" in output or "there is nothing to do" in output
    ):
        return "NeededSkip", "no ALPM package operation"
    fail(
        "transaction operation was not singular and authoritative: "
        f"observed={observed!r} output={output!r} delta={delta!r}"
    )


def run_transaction(
    case: str,
    artifact: Path,
    expected_operation: str,
    *,
    needed: bool = False,
) -> dict[str, object]:
    before = installed_state()
    before_log = PACMAN_LOG.read_bytes() if PACMAN_LOG.exists() else b""
    command = [
        "/usr/bin/pacman",
        "--root",
        str(INSTALL_ROOT),
        "--dbpath",
        str(DATABASE_ROOT),
        "--cachedir",
        str(CACHE_ROOT),
        "--logfile",
        str(PACMAN_LOG),
        "--noconfirm",
        "--noscriptlet",
        "-U",
    ]
    if needed:
        command.append("--needed")
    command.extend(["--", str(artifact)])
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
    )
    require(result.returncode == 0, f"{case} pacman -U failed: {result.stdout}")
    delta = log_delta(before_log)
    operation, receipt = classify_operation(result.stdout, delta, needed)
    require(
        operation == expected_operation,
        f"{case} operation is {operation}, expected {expected_operation}",
    )
    after = installed_state()
    require(after is not None, f"{case} left no installed local DB record")

    archive_mtree = archive_mtree_sha256(artifact)
    require(
        after["mtree_digest"] == archive_mtree,
        f"{case} installed ALPM-MTREE does not match selected artifact",
    )
    if operation == "NeededSkip":
        require(before == after, "--needed skip replaced or changed the installed record")
        distinguishable = "no"
    else:
        require(
            before is None or before["generation"] != after["generation"],
            f"{case} did not replace the opaque installed record generation",
        )
        distinguishable = "yes"
    return {
        "case": case,
        "operation": operation,
        "receipt": receipt,
        "before": before,
        "after": after,
        "artifact_digest": sha256_file(artifact),
        "artifact_mtree_digest": archive_mtree,
        "distinguishable": distinguishable,
    }


def state_field(state: dict[str, object] | None, name: str) -> str:
    return "ABSENT" if state is None else str(state[name])


def print_matrix(rows: list[dict[str, object]]) -> None:
    headings = (
        "case",
        "pacman_operation",
        "before_db_identity",
        "after_db_identity",
        "version",
        "install_time",
        "mtree_digest",
        "desc_files_digest",
        "opaque_generation",
        "distinguishable",
        "inode_before",
        "inode_after",
        "ctime_ns_before",
        "ctime_ns_after",
        "mtime_ns_before",
        "mtime_ns_after",
    )
    print("\t".join(headings))
    for row in rows:
        before = row["before"]
        after = row["after"]
        require(isinstance(after, dict), "matrix row has no installed state")
        values = (
            str(row["case"]),
            str(row["operation"]),
            state_field(before, "generation"),
            state_field(after, "generation"),
            state_field(after, "version"),
            state_field(after, "install_time"),
            state_field(after, "mtree_digest"),
            state_field(after, "desc_files_digest"),
            state_field(after, "generation"),
            str(row["distinguishable"]),
            state_field(before, "inode"),
            state_field(after, "inode"),
            state_field(before, "ctime_ns"),
            state_field(after, "ctime_ns"),
            state_field(before, "mtime_ns"),
            state_field(after, "mtime_ns"),
        )
        print("\t".join(values))


def main() -> None:
    require(os.geteuid() == 0, "characterization must run as disposable container root")
    for path in (STATE_ROOT, INSTALL_ROOT, DATABASE_ROOT, CACHE_ROOT):
        path.mkdir(parents=True, exist_ok=True)

    v1 = PACKAGE_ROOT / f"{PACKAGE_NAME}-1-1-any.pkg.tar.zst"
    v2 = PACKAGE_ROOT / f"{PACKAGE_NAME}-2-1-any.pkg.tar.zst"
    v2_different = ALTERNATE_PACKAGE_ROOT / f"{PACKAGE_NAME}-2-1-any.pkg.tar.zst"
    for artifact in (v1, v2, v2_different):
        require(artifact.is_file(), f"missing package artifact: {artifact}")
    require(
        sha256_file(v2) != sha256_file(v2_different),
        "same-version different-artifact fixtures are byte-identical",
    )

    mount = subprocess.run(
        ["/usr/bin/findmnt", "-T", str(DATABASE_ROOT), "-o", "TARGET,FSTYPE,OPTIONS", "-n"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
    )
    require(mount.returncode == 0, f"unable to inspect fixture mount: {mount.stderr}")
    print(f"filesystem: {mount.stdout.strip()}")
    print(f"reader: user={PROBE_USER} root-required=no")
    print(f"artifact v1 SHA-256: {sha256_file(v1)}")
    print(f"artifact v2 SHA-256: {sha256_file(v2)}")
    print(f"artifact v2-different SHA-256: {sha256_file(v2_different)}")

    rows = [
        run_transaction("first Install", v1, "Install"),
        run_transaction("Upgrade", v2, "Upgrade"),
        run_transaction("--needed skip", v2, "NeededSkip", needed=True),
        run_transaction(
            "same-version different artifact reinstall",
            v2_different,
            "Reinstall",
        ),
        run_transaction(
            "same-version identical artifact reinstall",
            v2_different,
            "Reinstall",
        ),
    ]

    different = rows[3]
    different_before = different["before"]
    different_after = different["after"]
    require(
        isinstance(different_before, dict) and isinstance(different_after, dict),
        "different-artifact reinstall lacked both states",
    )
    require(
        different_before["mtree_digest"] != different_after["mtree_digest"],
        "different artifact did not change installed MTREE evidence",
    )

    identical = rows[4]
    identical_before = identical["before"]
    identical_after = identical["after"]
    require(
        isinstance(identical_before, dict) and isinstance(identical_after, dict),
        "identical-artifact reinstall lacked both states",
    )
    require(
        identical_before["version"] == identical_after["version"]
        and identical_before["mtree_digest"] == identical_after["mtree_digest"],
        "identical-artifact semantic evidence changed unexpectedly",
    )

    same_second_row: dict[str, object] | None = None
    for attempt in range(1, MAX_SAME_SECOND_ATTEMPTS + 1):
        candidate = run_transaction(
            f"same-second attempt {attempt}",
            v2_different,
            "Reinstall",
        )
        before = candidate["before"]
        after = candidate["after"]
        require(
            isinstance(before, dict) and isinstance(after, dict),
            "same-second attempt lacked both states",
        )
        if before["install_time"] == after["install_time"]:
            candidate["case"] = "same-version identical same-second reinstall"
            same_second_row = candidate
            break
    require(
        same_second_row is not None,
        f"no identical same-second reinstall observed in {MAX_SAME_SECOND_ATTEMPTS} attempts",
    )
    same_second_before = same_second_row["before"]
    same_second_after = same_second_row["after"]
    require(
        isinstance(same_second_before, dict) and isinstance(same_second_after, dict),
        "same-second proof lacked states",
    )
    require(
        same_second_before["version"] == same_second_after["version"]
        and same_second_before["install_time"] == same_second_after["install_time"]
        and same_second_before["mtree_digest"] == same_second_after["mtree_digest"]
        and same_second_before["desc_files_digest"]
        == same_second_after["desc_files_digest"]
        and same_second_before["generation"] != same_second_after["generation"],
        "identical same-second reinstall generation proof did not close",
    )
    rows.append(same_second_row)

    final_record = DATABASE_ROOT / "local" / str(same_second_after["record"])
    final_later_read = probe_generation(final_record)
    require(
        final_later_read["opaque_identity"] == same_second_after["generation"],
        "final installed generation could not be revalidated later",
    )

    print("transaction receipts:")
    for row in rows:
        print(f"{row['case']}: {row['receipt']}")
    print("installed binding matrix:")
    print_matrix(rows)
    print(
        "same-version identical reinstall generation: PROVEN "
        f"install_time={same_second_after['install_time']} "
        f"before={same_second_before['generation']} "
        f"after={same_second_after['generation']} "
        f"later_read={final_later_read['opaque_identity']}"
    )
    print("installed binding feasibility: PASS")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--probe-record":
        raise SystemExit(probe_record_mode(sys.argv[2]))
    require(len(sys.argv) == 1, "unexpected characterization arguments")
    main()
