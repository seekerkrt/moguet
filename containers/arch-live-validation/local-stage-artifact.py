#!/usr/bin/python3

import hashlib
import json
import os
import re
from pathlib import Path
import stat
import sys
import time


STAGING_ROOT = Path("/var/lib/moguet-live-local/staging")
MAX_ARTIFACT_AGE_SECONDS = 60 * 60
FUTURE_SKEW_SECONDS = 5


def fail(message: str) -> None:
    raise RuntimeError(message)


TRUSTED_STAGE_PATTERN = re.compile(
    r"/run/moguet/source-artifact-installs/active/[0-9a-f]{64}/artifacts/artifact-0\.pkg\.tar\.zst"
)


def require_trusted_status(value: os.stat_result) -> None:
    if (not stat.S_ISREG(value.st_mode) or value.st_uid != 0 or value.st_gid != 0
            or stat.S_IMODE(value.st_mode) != 0o600 or value.st_nlink != 1):
        fail("trusted source has unsafe type, owner, mode, or links")
    age = time.time_ns() - value.st_mtime_ns
    if age < -FUTURE_SKEW_SECONDS * 1_000_000_000 or age > MAX_ARTIFACT_AGE_SECONDS * 1_000_000_000:
        fail("trusted source timestamp is outside this live invocation")


class TrustedStageInput:
    """Retain the fixed root namespace and reprove every named component."""

    def __init__(self, raw: str):
        if not TRUSTED_STAGE_PATTERN.fullmatch(raw):
            fail("artifact path is outside the exact trusted root staging boundary")
        self.raw = raw
        self.directories = []
        self.descriptor = -1
        try:
            root = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
            self.directories.append((root, None, "", os.fstat(root)))
            root_metadata = os.fstat(root)
            if root_metadata.st_uid != 0 or root_metadata.st_gid != 0 or root_metadata.st_mode & 0o022:
                fail("root directory has unsafe ownership or mode")
            for component in raw.split("/")[1:-1]:
                parent = self.directories[-1][0]
                fd = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=parent)
                metadata = os.fstat(fd)
                self.directories.append((fd, parent, component, metadata))
                if (metadata.st_uid != 0 or metadata.st_gid != 0
                        or (stat.S_IMODE(metadata.st_mode) & 0o022)
                        or (component != "run" and stat.S_IMODE(metadata.st_mode) != 0o700)):
                    fail("trusted directory has unsafe ownership or mode")
            parent = self.directories[-1][0]
            if os.listdir(parent) != ["artifact-0.pkg.tar.zst"]:
                fail("trusted artifact directory contains unexpected entries")
            self.descriptor = os.open("artifact-0.pkg.tar.zst", os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
            self.metadata = os.fstat(self.descriptor)
            require_trusted_status(self.metadata)
            self.reprove()
        except BaseException:
            self.close()
            raise

    def reprove(self) -> None:
        for fd, parent, name, before in self.directories:
            current = os.fstat(fd)
            identity = lambda s: (s.st_dev, s.st_ino, s.st_mode, s.st_uid, s.st_gid)
            if identity(current) != identity(before):
                fail("trusted directory descriptor changed identity")
            if parent is not None and identity(os.stat(name, dir_fd=parent, follow_symlinks=False)) != identity(before):
                fail("trusted directory path changed identity")
        parent = self.directories[-1][0]
        if os.listdir(parent) != ["artifact-0.pkg.tar.zst"]:
            fail("trusted artifact directory entries changed")
        current = os.fstat(self.descriptor)
        require_trusted_status(current)
        if stable_identity(current) != stable_identity(self.metadata):
            fail("trusted source descriptor changed identity or metadata")
        if stable_identity(os.stat("artifact-0.pkg.tar.zst", dir_fd=parent, follow_symlinks=False)) != stable_identity(self.metadata):
            fail("trusted source pathname changed identity or metadata")

    def close(self) -> None:
        if self.descriptor >= 0:
            os.close(self.descriptor)
            self.descriptor = -1
        for fd, _, _, _ in reversed(self.directories):
            os.close(fd)
        self.directories = []


def check_trusted(arguments: list[str]) -> int:
    if len(arguments) != 1:
        fail("check-trusted requires one source")
    source = TrustedStageInput(arguments[0])
    source.close()
    return 0


def trusted_record(source: TrustedStageInput, digest: str) -> str:
    return json.dumps({"path": source.raw, "identity": stable_identity(source.metadata), "sha256": digest}) + "\n"


def verify_trusted(arguments: list[str]) -> int:
    if len(arguments) != 3:
        fail("verify-trusted requires source, snapshot, and identity evidence")
    source = TrustedStageInput(arguments[0])
    descriptors = []
    saved_metadata = []
    try:
        for raw, mode in ((arguments[1], 0o440), (arguments[2], 0o640)):
            fd = os.open(raw, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
            descriptors.append(fd)
            metadata = os.fstat(fd)
            saved_metadata.append(stable_identity(metadata))
            if (not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != 0 or metadata.st_gid != 1000
                    or metadata.st_nlink != 1 or stat.S_IMODE(metadata.st_mode) != mode):
                fail("trusted snapshot/evidence metadata drift")
        record = json.loads(os.read(descriptors[1], 4097))
        digest = hash_descriptor(source.descriptor)
        source.reprove()
        if record != json.loads(trusted_record(source, digest)) or hash_descriptor(descriptors[0]) != digest:
            fail("trusted source/snapshot/evidence differs before real pacman")
        source.reprove()
        if any(stable_identity(os.fstat(fd)) != saved for fd, saved in zip(descriptors, saved_metadata)):
            fail("trusted snapshot/evidence changed while reading")
    finally:
        for fd in descriptors:
            os.close(fd)
        source.close()
    return 0


def stable_identity(value: os.stat_result) -> tuple[int, ...]:
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_uid,
        value.st_gid,
        value.st_nlink,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def hash_descriptor(descriptor: int) -> str:
    os.lseek(descriptor, 0, os.SEEK_SET)
    digest = hashlib.sha256()
    while True:
        chunk = os.read(descriptor, 1024 * 1024)
        if not chunk:
            return digest.hexdigest()
        digest.update(chunk)


def copy_and_hash(source_descriptor: int, destination: Path, expected_gid: int) -> tuple[str, str]:
    destination_descriptor = os.open(
        destination,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
        0o440,
    )
    copied_digest = hashlib.sha256()
    try:
        os.lseek(source_descriptor, 0, os.SEEK_SET)
        while True:
            chunk = os.read(source_descriptor, 1024 * 1024)
            if not chunk:
                break
            copied_digest.update(chunk)
            offset = 0
            while offset < len(chunk):
                offset += os.write(destination_descriptor, chunk[offset:])
        os.fsync(destination_descriptor)
        os.fchown(destination_descriptor, 0, expected_gid)
        os.fchmod(destination_descriptor, 0o440)
    finally:
        os.close(destination_descriptor)

    staged_descriptor = os.open(destination, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        staged_status = os.fstat(staged_descriptor)
        if (
            not stat.S_ISREG(staged_status.st_mode)
            or staged_status.st_uid != 0
            or staged_status.st_gid != expected_gid
            or stat.S_IMODE(staged_status.st_mode) != 0o440
            or staged_status.st_nlink != 1
        ):
            fail("staged artifact has unsafe type, ownership, mode, or links")
        staged_hash = hash_descriptor(staged_descriptor)
    finally:
        os.close(staged_descriptor)
    return copied_digest.hexdigest(), staged_hash


def write_evidence(path: Path, value: str, expected_gid: int) -> None:
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
        0o640,
    )
    try:
        os.write(descriptor, value.encode("utf-8"))
        os.fsync(descriptor)
        os.fchown(descriptor, 0, expected_gid)
        os.fchmod(descriptor, 0o640)
    finally:
        os.close(descriptor)


def stage(arguments: list[str]) -> int:
    if len(arguments) != 4:
        print(
            "usage: local-stage-artifact.py SOURCE DESTINATION EVIDENCE_DIRECTORY UID:GID",
            file=sys.stderr,
        )
        return 2
    destination = Path(arguments[1])
    evidence_directory = Path(arguments[2])
    uid_text, separator, gid_text = arguments[3].partition(":")
    if not separator:
        fail("validation user identity is malformed")
    expected_uid, expected_gid = int(uid_text), int(gid_text)
    if (expected_uid, expected_gid) != (1000, 1000):
        fail("unexpected validation identity")
    if destination.parent != STAGING_ROOT / "local-root-install" or evidence_directory != Path("/var/log/moguet-live-local/local-root-install"):
        fail("trusted positive destination/case mismatch")

    if destination.parent.parent != STAGING_ROOT or not destination.is_absolute():
        fail("staged artifact is outside the local root staging directory")
    if Path(os.path.realpath(destination.parent.parent)) != STAGING_ROOT:
        fail("root staging directory changed canonical identity")
    destination_parent_status = os.lstat(destination.parent)
    if (
        not stat.S_ISDIR(destination_parent_status.st_mode)
        or stat.S_ISLNK(destination_parent_status.st_mode)
        or destination_parent_status.st_uid != 0
        or destination_parent_status.st_gid != expected_gid
        or stat.S_IMODE(destination_parent_status.st_mode) != 0o750
    ):
        fail("root staging directory has unsafe identity")
    evidence_status = os.lstat(evidence_directory)
    if (
        not stat.S_ISDIR(evidence_status.st_mode)
        or stat.S_ISLNK(evidence_status.st_mode)
        or evidence_status.st_uid != 0
        or evidence_status.st_gid != expected_gid
        or stat.S_IMODE(evidence_status.st_mode) != 0o750
    ):
        fail("root evidence directory has unsafe identity")

    trusted = TrustedStageInput(arguments[0])
    source_descriptor = trusted.descriptor
    try:
        initial_status = os.fstat(source_descriptor)
        require_trusted_status(initial_status)
        trusted.reprove()
        source_hash_before = hash_descriptor(source_descriptor)
        if stable_identity(os.fstat(source_descriptor)) != stable_identity(initial_status):
            fail("source artifact changed during the before hash")
        trusted.reprove()
        copied_hash, staged_hash = copy_and_hash(source_descriptor, destination, expected_gid)
        if stable_identity(os.fstat(source_descriptor)) != stable_identity(initial_status):
            fail("source artifact changed during copy")
        trusted.reprove()
        source_hash_after = hash_descriptor(source_descriptor)
        if source_hash_before != copied_hash or copied_hash != staged_hash or staged_hash != source_hash_after:
            fail("source and staged artifact content hashes differ")
        trusted.reprove()
        write_evidence(evidence_directory / "trusted-source.json", trusted_record(trusted, source_hash_after), expected_gid)
        write_evidence(
            evidence_directory / "stage-hashes.txt",
            "".join(
                (
                    f"source_before={source_hash_before}\n",
                    f"copied={copied_hash}\n",
                    f"staged={staged_hash}\n",
                    f"source_after={source_hash_after}\n",
                )
            ),
            expected_gid,
        )
        write_evidence(
            evidence_directory / "staged-artifact-path.txt",
            f"{destination}\n",
            expected_gid,
        )
    finally:
        trusted.close()
    return 0


if __name__ == "__main__":
    try:
        if sys.argv[1:2] == ["check-trusted"]:
            raise SystemExit(check_trusted(sys.argv[2:]))
        if sys.argv[1:2] == ["verify-trusted"]:
            raise SystemExit(verify_trusted(sys.argv[2:]))
        raise SystemExit(stage(sys.argv[1:]))
    except (OSError, RuntimeError, ValueError) as error:
        print(f"moguet-live-local-stage: {error}", file=sys.stderr)
        raise SystemExit(1)
