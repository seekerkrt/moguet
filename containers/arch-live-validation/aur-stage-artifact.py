#!/usr/bin/python3

from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time


CACHE_ROOT = Path("/home/moguet-validation/.cache/moguet")
STAGING_ROOT = Path("/var/lib/moguet-live-aur/staging")
STAGING_CASES = {
    "aur-install",
    "aur-content-drift-test",
    "aur-conflict-policy-test",
    "aur-xattr-metadata-test",
    "aur-acl-metadata-test",
    "aur-pkgdesc-authority-test",
}
WORKSPACE_PATTERN = re.compile(r"\.artifact-workspace~-[A-Za-z0-9]{6}")
MAX_ARTIFACT_AGE_SECONDS = 60 * 60
FUTURE_SKEW_SECONDS = 5

STATIC_HEADER = ("# path", "type", "mode", "sha256")
MANIFEST_HEADER = ("# path", "type", "mode", "owner", "group", "sha256")
PKGINFO_MANIFEST_HEADER = ("# key", "value")
EXPECTED_METADATA = {
    ".BUILDINFO": ("regular", "0644"),
    ".MTREE": ("regular", "0644"),
    ".PKGINFO": ("regular", "0644"),
}
PKGINFO_STABLE_SINGLE = {
    "arch",
    "pkgbase",
    "pkgdesc",
    "pkgname",
    "pkgver",
    "size",
    "url",
}
PKGINFO_VOLATILE_SINGLE = {"builddate", "packager"}
PKGINFO_ALLOWED_REPEATABLE = {"license", "depend", "makedepend", "xdata"}
PKGINFO_ALLOWED_KEYS = (
    PKGINFO_STABLE_SINGLE
    | PKGINFO_VOLATILE_SINGLE
    | PKGINFO_ALLOWED_REPEATABLE
)
PKGINFO_STABLE_KEYS = (
    PKGINFO_STABLE_SINGLE | PKGINFO_ALLOWED_REPEATABLE
)
PKGINFO_FORBIDDEN_TRANSACTION_FIELDS = {
    "backup",
    "checkdepend",
    "conflict",
    "group",
    "install",
    "optdepend",
    "provides",
    "replaces",
}
SAFE_ENV = {"PATH": "/usr/bin", "LC_ALL": "C"}


def fail(message: str) -> None:
    raise RuntimeError(message)


def parse_scenario(values: list[str]) -> dict[str, str | tuple[str, ...]]:
    if len(values) != 6:
        fail("scenario identity requires six fields")
    package_name, package_base, version, runtime_text, make_text, architecture = values
    package_pattern = re.compile(r"[a-z0-9][a-z0-9@._+-]*")
    dependency_pattern = re.compile(
        r"[a-z0-9][a-z0-9@._+-]*(?:[<>=]+[0-9A-Za-z@._+:~+-]+)?"
    )
    if not package_pattern.fullmatch(package_name) or not package_pattern.fullmatch(
        package_base
    ):
        fail("scenario package identity is unsafe")
    if package_name != package_base:
        fail("scenario requires one package matching its PackageBase")
    if not re.fullmatch(r"[0-9A-Za-z@._+:~=-]+", version):
        fail("scenario version is unsafe")
    if not re.fullmatch(r"[0-9A-Za-z_+-]+", architecture):
        fail("scenario architecture is unsafe")

    def dependencies(text: str, label: str) -> tuple[str, ...]:
        result = tuple(text.split(","))
        if not result or any(not dependency_pattern.fullmatch(item) for item in result):
            fail(f"scenario {label} dependency identity is unsafe")
        if len(set(result)) != len(result):
            fail(f"scenario {label} dependencies contain duplicates")
        return result

    return {
        "package_name": package_name,
        "package_base": package_base,
        "version": version,
        "runtime_dependencies": dependencies(runtime_text, "runtime"),
        "make_dependencies": dependencies(make_text, "make"),
        "architecture": architecture,
    }


def hash_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def hash_descriptor(descriptor: int) -> str:
    os.lseek(descriptor, 0, os.SEEK_SET)
    digest = hashlib.sha256()
    while True:
        chunk = os.read(descriptor, 1024 * 1024)
        if not chunk:
            return digest.hexdigest()
        digest.update(chunk)


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


def require_path_bound_to_descriptor(
    source: Path, descriptor_status: os.stat_result
) -> None:
    path_status = os.lstat(source)
    if stat.S_ISLNK(path_status.st_mode):
        fail("source artifact path became a symlink")
    if stable_identity(path_status) != stable_identity(descriptor_status):
        fail("source artifact path changed identity or metadata")


def require_source_status(value: os.stat_result, expected_uid: int) -> None:
    if not stat.S_ISREG(value.st_mode):
        fail("source artifact is not a regular file")
    if value.st_uid != expected_uid:
        fail("source artifact is not owned by the validation user")
    if value.st_mode & 0o022:
        fail("source artifact is group/other writable")
    if value.st_nlink != 1:
        fail("source artifact has an unexpected hard-link count")

    now_ns = time.time_ns()
    if value.st_mtime_ns > now_ns + FUTURE_SKEW_SECONDS * 1_000_000_000:
        fail("source artifact timestamp is in the future")
    if now_ns - value.st_mtime_ns > MAX_ARTIFACT_AGE_SECONDS * 1_000_000_000:
        fail("source artifact is not fresh enough for this live invocation")


def require_no_symlink_components(path: Path) -> None:
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        if stat.S_ISLNK(os.lstat(current).st_mode):
            fail(f"path contains a symlink component: {current}")


def require_source_path(source: Path) -> None:
    if not source.is_absolute():
        fail("source artifact path is not absolute")
    if Path(os.path.normpath(source)) != source:
        fail("source artifact path is not lexically canonical")
    if Path(os.path.realpath(source)) != source:
        fail("source artifact path is not its canonical realpath")
    if source.parent.parent != CACHE_ROOT:
        fail("source artifact is outside the exact Moguet cache root")
    if not WORKSPACE_PATTERN.fullmatch(source.parent.name):
        fail("source artifact is outside an invocation-owned workspace")
    require_no_symlink_components(source)


def require_destination_path(destination: Path) -> None:
    if (
        not destination.is_absolute()
        or destination.parent.parent != STAGING_ROOT
        or destination.parent.name not in STAGING_CASES
    ):
        fail("staged artifact path is outside the root staging directory")
    if Path(os.path.realpath(destination.parent.parent)) != STAGING_ROOT:
        fail("root staging directory changed canonical identity")
    parent_status = os.stat(destination.parent, follow_symlinks=False)
    if not stat.S_ISDIR(parent_status.st_mode) or parent_status.st_uid != 0:
        fail("root case staging directory has unsafe ownership or type")
    if parent_status.st_mode & 0o022:
        fail("root case staging directory is group/other writable")


def copy_and_hash(
    source_descriptor: int, destination: Path, expected_gid: int
) -> tuple[str, str]:
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

    staged_descriptor = os.open(
        destination, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    )
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


def stage_artifact(arguments: list[str]) -> int:
    if len(arguments) != 4:
        print(
            "usage: aur-stage-artifact.py stage SOURCE DESTINATION EXPECTED_UID EXPECTED_GID",
            file=sys.stderr,
        )
        return 2

    source = Path(arguments[0])
    destination = Path(arguments[1])
    expected_uid = int(arguments[2])
    expected_gid = int(arguments[3])
    require_source_path(source)
    require_destination_path(destination)

    source_descriptor = os.open(
        source,
        os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK,
    )
    try:
        initial_status = os.fstat(source_descriptor)
        require_source_status(initial_status, expected_uid)
        require_path_bound_to_descriptor(source, initial_status)

        source_hash_before = hash_descriptor(source_descriptor)
        if stable_identity(os.fstat(source_descriptor)) != stable_identity(
            initial_status
        ):
            fail("source artifact changed during the before hash")
        require_path_bound_to_descriptor(source, initial_status)

        copied_hash, staged_hash = copy_and_hash(
            source_descriptor, destination, expected_gid
        )
        if stable_identity(os.fstat(source_descriptor)) != stable_identity(
            initial_status
        ):
            fail("source artifact changed during the staging copy")
        require_path_bound_to_descriptor(source, initial_status)

        source_hash_after = hash_descriptor(source_descriptor)
        if stable_identity(os.fstat(source_descriptor)) != stable_identity(
            initial_status
        ):
            fail("source artifact changed during the after hash")
        require_path_bound_to_descriptor(source, initial_status)
    finally:
        os.close(source_descriptor)

    if len({source_hash_before, copied_hash, staged_hash, source_hash_after}) != 1:
        fail("source/staged hashes differ; refusing a possible concurrent mutation")

    print(f"source_sha256_before={source_hash_before}")
    print(f"copy_sha256={copied_hash}")
    print(f"staged_sha256={staged_hash}")
    print(f"source_sha256_after={source_hash_after}")
    print(f"source_device={initial_status.st_dev}")
    print(f"source_inode={initial_status.st_ino}")
    print(f"source_size={initial_status.st_size}")
    print(f"source_mtime_ns={initial_status.st_mtime_ns}")
    print(f"source_ctime_ns={initial_status.st_ctime_ns}")
    print(f"source_uid={initial_status.st_uid}")
    print(f"source_mode={stat.S_IMODE(initial_status.st_mode):04o}")
    return 0


def require_root_readonly_file(path: Path, label: str) -> None:
    value = os.lstat(path)
    if (
        not stat.S_ISREG(value.st_mode)
        or stat.S_ISLNK(value.st_mode)
        or value.st_uid != 0
        or value.st_gid != 0
        or stat.S_IMODE(value.st_mode) != 0o444
        or value.st_nlink != 1
    ):
        fail(f"{label} is not a root-owned mode 0444 regular authority")


def validate_archive_path(path: str, entry_type: str) -> None:
    if not path or path.startswith("/") or "\\x00" in path:
        fail("authority contains an unsafe archive member path")
    if entry_type == "directory":
        if not path.endswith("/"):
            fail("directory authority path must end in a slash")
        check_path = path[:-1]
    else:
        if path.endswith("/"):
            fail("regular authority path must not end in a slash")
        check_path = path
    if not check_path or check_path.startswith("./") or "//" in check_path:
        fail("authority contains a non-canonical archive member path")
    if any(part in {"", ".", ".."} for part in check_path.split("/")):
        fail("authority contains path traversal")


def read_tsv(path: Path, header: tuple[str, ...], label: str) -> list[tuple[str, ...]]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        fail(f"{label} is not UTF-8: {error}")
    if not text.endswith("\n"):
        fail(f"{label} must end with one newline")
    lines = text.splitlines()
    if not lines or any(not line for line in lines):
        fail(f"{label} contains an empty line")
    rows = [tuple(line.split("\t")) for line in lines]
    if rows[0] != header:
        fail(f"{label} header drift")
    if any(len(row) != len(header) for row in rows):
        fail(f"{label} is not exact-tab TSV")
    return rows[1:]


def load_static_authority(
    path: Path, scenario: dict[str, str | tuple[str, ...]]
) -> dict[str, tuple[str, str, str]]:
    require_root_readonly_file(path, "static payload authority")
    rows = read_tsv(path, STATIC_HEADER, "static payload authority")
    entries: dict[str, tuple[str, str, str]] = {}
    ordered_paths: list[str] = []
    for archive_path, entry_type, mode, content_hash in rows:
        validate_archive_path(archive_path, entry_type)
        if entry_type not in {"directory", "regular"}:
            fail("static payload authority has an unsupported type")
        if mode not in {"0644", "0755"}:
            fail("static payload authority has an unsupported mode")
        if entry_type == "directory" and (mode != "0755" or content_hash != "-"):
            fail("directory authority must be mode 0755 without a content hash")
        if entry_type == "regular" and content_hash != "-" and not re.fullmatch(
            r"[0-9a-f]{64}", content_hash
        ):
            fail("static payload authority has an invalid SHA-256")
        if archive_path in entries:
            fail("static payload authority contains a duplicate path")
        entries[archive_path] = (entry_type, mode, content_hash)
        ordered_paths.append(archive_path)
    if ordered_paths != sorted(ordered_paths):
        fail("static payload authority is not sorted")
    package_name = str(scenario["package_name"])
    required_regular = {
        f"usr/bin/{package_name}": ("0755", False),
        f"usr/share/doc/{package_name}/README.md": ("0644", True),
        f"usr/share/licenses/{package_name}/LICENSE": ("0644", True),
    }
    for archive_path, (expected_mode, requires_hash) in required_regular.items():
        identity = entries.get(archive_path)
        if identity is None or identity[:2] != ("regular", expected_mode):
            fail(f"static payload authority lacks required entry: {archive_path}")
        content_hash = identity[2]
        if requires_hash and not re.fullmatch(r"[0-9a-f]{64}", content_hash):
            fail(f"static payload authority lacks pinned content: {archive_path}")
        if not requires_hash and content_hash != "-":
            fail(f"generated payload must not have a tracked hash: {archive_path}")
    return entries


def load_runtime_manifest(
    path: Path, static_entries: dict[str, tuple[str, str, str]]
) -> dict[str, tuple[str, str, str, str, str]]:
    require_root_readonly_file(path, "reference payload manifest")
    rows = read_tsv(path, MANIFEST_HEADER, "reference payload manifest")
    entries: dict[str, tuple[str, str, str, str, str]] = {}
    ordered_paths: list[str] = []
    for archive_path, entry_type, mode, owner, group, content_hash in rows:
        validate_archive_path(archive_path, entry_type)
        if owner != "root" or group != "root":
            fail("reference payload manifest has a non-root archive owner/group")
        if archive_path in entries:
            fail("reference payload manifest contains a duplicate path")
        entries[archive_path] = (entry_type, mode, owner, group, content_hash)
        ordered_paths.append(archive_path)
    if ordered_paths != sorted(ordered_paths):
        fail("reference payload manifest is not sorted")
    if set(entries) != set(static_entries):
        fail("reference payload manifest path set drift")
    for archive_path, (entry_type, mode, static_hash) in static_entries.items():
        manifest_type, manifest_mode, owner, group, content_hash = entries[archive_path]
        if (manifest_type, manifest_mode, owner, group) != (
            entry_type,
            mode,
            "root",
            "root",
        ):
            fail("reference payload manifest type/mode/owner/group drift")
        if entry_type == "regular" and static_hash == "-":
            if not re.fullmatch(r"[0-9a-f]{64}", content_hash):
                fail("reference generated-payload hash is invalid")
        elif content_hash != static_hash:
            fail("reference payload manifest static content hash drift")
    return entries


def symbolic_mode(entry_type: str, mode: str) -> str:
    expected = {
        ("regular", "0644"): "-rw-r--r--",
        ("regular", "0755"): "-rwxr-xr-x",
        ("directory", "0755"): "drwxr-xr-x",
    }
    try:
        return expected[(entry_type, mode)]
    except KeyError:
        fail("unsupported exact archive mode")
        raise AssertionError("unreachable")


def run_bsdtar(arguments: list[str], text: bool = False) -> bytes | str:
    completed = subprocess.run(
        ["/usr/bin/bsdtar", *arguments],
        check=False,
        env=SAFE_ENV,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=text,
    )
    if completed.returncode != 0:
        fail("bsdtar rejected the staged package archive")
    return completed.stdout


def parse_pkginfo(
    value: bytes, scenario: dict[str, str | tuple[str, ...]]
) -> dict[str, list[str]]:
    try:
        lines = value.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        fail(f".PKGINFO is not UTF-8: {error}")
    fields: dict[str, list[str]] = {}
    generator_comments = 0
    fakeroot_comments = 0
    for line in lines:
        if line.startswith("# Generated by makepkg "):
            if not re.fullmatch(r"# Generated by makepkg [0-9][0-9A-Za-z.+:-]*", line):
                fail(".PKGINFO generator comment is malformed")
            generator_comments += 1
            continue
        if line.startswith("# using fakeroot version "):
            if not re.fullmatch(r"# using fakeroot version [0-9][0-9A-Za-z.+:-]*", line):
                fail(".PKGINFO fakeroot comment is malformed")
            fakeroot_comments += 1
            continue
        if not line or line.count(" = ") != 1:
            fail(f".PKGINFO has an unsafe record: {line!r}")
        key, field_value = line.split(" = ", 1)
        if not key or not field_value:
            fail(".PKGINFO has an empty field")
        if key in PKGINFO_FORBIDDEN_TRANSACTION_FIELDS:
            fail(f".PKGINFO contains forbidden transaction field: {key}")
        if key not in PKGINFO_ALLOWED_KEYS:
            fail(f".PKGINFO contains unexpected field: {key}")
        fields.setdefault(key, []).append(field_value)

    if generator_comments != 1:
        fail(".PKGINFO generator comment cardinality drift")
    if fakeroot_comments != 1:
        fail(".PKGINFO fakeroot comment cardinality drift")
    for key in PKGINFO_STABLE_SINGLE | PKGINFO_VOLATILE_SINGLE:
        if len(fields.get(key, [])) != 1:
            fail(f".PKGINFO singleton cardinality drift: {key}")
    if not fields.get("license"):
        fail(".PKGINFO has no license")
    if Counter(fields.get("depend", [])) != Counter(scenario["runtime_dependencies"]):
        fail(".PKGINFO dependency policy drift")
    if Counter(fields.get("makedepend", [])) != Counter(
        scenario["make_dependencies"]
    ):
        fail(".PKGINFO make dependency policy drift")
    expected_identity = {
        "pkgname": scenario["package_name"],
        "pkgbase": scenario["package_base"],
        "pkgver": scenario["version"],
        "arch": scenario["architecture"],
    }
    for key, expected in expected_identity.items():
        if fields[key] != [expected]:
            fail(f".PKGINFO identity drift: {key}")
    if fields["xdata"].count("pkgtype=pkg") != 1:
        fail(".PKGINFO package type policy drift")
    builddate = fields["builddate"][0]
    if not re.fullmatch(r"[0-9]+", builddate):
        fail(".PKGINFO builddate must be one non-negative ASCII decimal integer")
    packager = fields["packager"][0]
    if packager != packager.strip() or re.search(r"[\x00-\x1f\x7f]", packager):
        fail(".PKGINFO packager has unsafe whitespace or control characters")
    return fields


def pkginfo_stable_pairs(fields: dict[str, list[str]]) -> list[tuple[str, str]]:
    return sorted(
        (key, field_value)
        for key in PKGINFO_STABLE_KEYS
        for field_value in fields.get(key, [])
    )


def load_pkginfo_manifest(path: Path) -> list[tuple[str, str]]:
    require_root_readonly_file(path, "reference PKGINFO manifest")
    rows = read_tsv(path, PKGINFO_MANIFEST_HEADER, "reference PKGINFO manifest")
    pairs: list[tuple[str, str]] = []
    for key, field_value in rows:
        if key not in PKGINFO_STABLE_KEYS:
            fail("reference PKGINFO manifest contains a volatile or unknown field")
        if not field_value:
            fail("reference PKGINFO manifest contains an empty value")
        pairs.append((key, field_value))
    if pairs != sorted(pairs):
        fail("reference PKGINFO manifest is not sorted")
    if not pairs:
        fail("reference PKGINFO manifest has no stable fields")
    return pairs


def validate_pkginfo_authority(
    fields: dict[str, list[str]], reference_pairs: list[tuple[str, str]]
) -> None:
    actual_pairs = pkginfo_stable_pairs(fields)
    expected_counts = Counter(reference_pairs)
    actual_counts = Counter(actual_pairs)
    if actual_counts == expected_counts:
        return
    for key in sorted(PKGINFO_STABLE_KEYS):
        actual_for_key = Counter(
            field_value for candidate_key, field_value in actual_pairs if candidate_key == key
        )
        expected_for_key = Counter(
            field_value
            for candidate_key, field_value in reference_pairs
            if candidate_key == key
        )
        if actual_for_key != expected_for_key:
            fail(f".PKGINFO {key} authority mismatch")
    fail(".PKGINFO stable authority mismatch")


def validate_package_archive(
    archive: Path,
    expected_entries: dict[str, tuple[str, str, str, str, str]],
    scenario: dict[str, str | tuple[str, ...]],
) -> tuple[list[str], bytes, dict[str, list[str]]]:
    archive_status = os.lstat(archive)
    if not stat.S_ISREG(archive_status.st_mode) or stat.S_ISLNK(archive_status.st_mode):
        fail("package archive is not a regular non-symlink")
    listing = run_bsdtar(["-tvf", str(archive)], text=True)
    assert isinstance(listing, str)
    actual_members: dict[str, tuple[str, str, str, str]] = {}
    serialized_members: list[str] = []
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) != 9:
            fail("archive member listing is not safely parseable")
        permissions, _links, owner, group, _size, _month, _day, _time, archive_path = fields
        if owner != "root" or group != "root":
            fail("archive member owner/group is not root/root")
        if permissions.startswith("-"):
            entry_type = "regular"
        elif permissions.startswith("d"):
            entry_type = "directory"
        else:
            fail("archive contains a non-regular/non-directory member")
        validate_archive_path(archive_path, entry_type)
        if archive_path in actual_members:
            fail("archive contains a duplicate member path")
        actual_members[archive_path] = (entry_type, permissions, owner, group)
        serialized_members.append(
            "\t".join((entry_type, permissions, owner, group, archive_path))
        )

    expected_all = dict(expected_entries)
    for archive_path, (entry_type, mode) in EXPECTED_METADATA.items():
        expected_all[archive_path] = (entry_type, mode, "root", "root", "-")
    if set(actual_members) != set(expected_all):
        fail("archive member path set drift")
    for archive_path, expected in expected_all.items():
        expected_type, expected_mode, expected_owner, expected_group, _expected_hash = expected
        actual_type, actual_permissions, actual_owner, actual_group = actual_members[archive_path]
        if (actual_type, actual_permissions, actual_owner, actual_group) != (
            expected_type,
            symbolic_mode(expected_type, expected_mode),
            expected_owner,
            expected_group,
        ):
            fail("archive member type/mode/owner/group drift")

    pkginfo = run_bsdtar(["-xOf", str(archive), ".PKGINFO"])
    assert isinstance(pkginfo, bytes)
    pkginfo_fields = parse_pkginfo(pkginfo, scenario)
    for archive_path, (_entry_type, _mode, _owner, _group, expected_hash) in expected_entries.items():
        if expected_hash == "-":
            continue
        member = run_bsdtar(["-xOf", str(archive), archive_path])
        assert isinstance(member, bytes)
        if hash_bytes(member) != expected_hash:
            fail(f"archive payload content hash drift: {archive_path}")
    return sorted(serialized_members), pkginfo, pkginfo_fields


def write_new_file(path: Path, value: bytes, mode: int, gid: int) -> None:
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
        mode,
    )
    try:
        offset = 0
        while offset < len(value):
            offset += os.write(descriptor, value[offset:])
        os.fsync(descriptor)
        os.fchown(descriptor, 0, gid)
        os.fchmod(descriptor, mode)
    finally:
        os.close(descriptor)


def create_reference_manifest(arguments: list[str]) -> int:
    if len(arguments) != 9:
        print(
            "usage: aur-stage-artifact.py manifest STATIC_AUTHORITY ARCHIVE OUTPUT "
            "PACKAGE PACKAGE_BASE VERSION RUNTIME_DEPS MAKE_DEPS ARCH",
            file=sys.stderr,
        )
        return 2
    scenario = parse_scenario(arguments[3:])
    static_entries = load_static_authority(Path(arguments[0]), scenario)
    archive = Path(arguments[1])
    output = Path(arguments[2])
    output_parent = output.parent
    parent_status = os.lstat(output_parent)
    if (
        not stat.S_ISDIR(parent_status.st_mode)
        or stat.S_ISLNK(parent_status.st_mode)
        or parent_status.st_uid != 0
        or parent_status.st_gid != 0
        or stat.S_IMODE(parent_status.st_mode) != 0o755
    ):
        fail("reference manifest parent is not a root-owned mode 0755 directory")
    reference_entries = {
        archive_path: (entry_type, mode, "root", "root", content_hash)
        for archive_path, (entry_type, mode, content_hash) in static_entries.items()
    }
    serialized_members, _pkginfo, _pkginfo_fields = validate_package_archive(
        archive, reference_entries, scenario
    )
    del serialized_members
    for archive_path, (entry_type, mode, _owner, _group, content_hash) in list(
        reference_entries.items()
    ):
        if entry_type != "regular" or content_hash != "-":
            continue
        generated_payload = run_bsdtar(["-xOf", str(archive), archive_path])
        assert isinstance(generated_payload, bytes)
        reference_entries[archive_path] = (
            entry_type,
            mode,
            "root",
            "root",
            hash_bytes(generated_payload),
        )
    lines = ["\t".join(MANIFEST_HEADER)]
    for archive_path in sorted(reference_entries):
        lines.append("\t".join((archive_path, *reference_entries[archive_path])))
    write_new_file(output, ("\n".join(lines) + "\n").encode("utf-8"), 0o444, 0)
    return 0


def create_reference_pkginfo_manifest(arguments: list[str]) -> int:
    if len(arguments) != 9:
        print(
            "usage: aur-stage-artifact.py pkginfo-manifest "
            "STATIC_AUTHORITY ARCHIVE OUTPUT PACKAGE PACKAGE_BASE VERSION "
            "RUNTIME_DEPS MAKE_DEPS ARCH",
            file=sys.stderr,
        )
        return 2
    scenario = parse_scenario(arguments[3:])
    static_entries = load_static_authority(Path(arguments[0]), scenario)
    archive = Path(arguments[1])
    output = Path(arguments[2])
    output_parent = output.parent
    parent_status = os.lstat(output_parent)
    if (
        not stat.S_ISDIR(parent_status.st_mode)
        or stat.S_ISLNK(parent_status.st_mode)
        or parent_status.st_uid != 0
        or parent_status.st_gid != 0
        or stat.S_IMODE(parent_status.st_mode) != 0o755
    ):
        fail("reference PKGINFO manifest parent is not a root-owned mode 0755 directory")
    reference_entries = {
        archive_path: (entry_type, mode, "root", "root", content_hash)
        for archive_path, (entry_type, mode, content_hash) in static_entries.items()
    }
    _serialized_members, _pkginfo, fields = validate_package_archive(
        archive, reference_entries, scenario
    )
    del _serialized_members, _pkginfo
    pairs = pkginfo_stable_pairs(fields)
    if not pairs:
        fail("reference PKGINFO package has no stable fields")
    lines = ["\t".join(PKGINFO_MANIFEST_HEADER)]
    lines.extend("\t".join(pair) for pair in pairs)
    write_new_file(output, ("\n".join(lines) + "\n").encode("utf-8"), 0o444, 0)
    return 0


def validate_staged_archive(arguments: list[str]) -> int:
    if len(arguments) != 12:
        print(
            "usage: aur-stage-artifact.py validate STATIC_AUTHORITY "
            "PAYLOAD_MANIFEST PKGINFO_MANIFEST ARCHIVE EVIDENCE_DIRECTORY "
            "EVIDENCE_GID PACKAGE PACKAGE_BASE VERSION RUNTIME_DEPS MAKE_DEPS ARCH",
            file=sys.stderr,
        )
        return 2
    scenario = parse_scenario(arguments[6:])
    static_entries = load_static_authority(Path(arguments[0]), scenario)
    manifest_entries = load_runtime_manifest(Path(arguments[1]), static_entries)
    pkginfo_reference = load_pkginfo_manifest(Path(arguments[2]))
    archive = Path(arguments[3])
    evidence_directory = Path(arguments[4])
    evidence_gid = int(arguments[5])
    evidence_status = os.lstat(evidence_directory)
    if (
        not stat.S_ISDIR(evidence_status.st_mode)
        or stat.S_ISLNK(evidence_status.st_mode)
        or evidence_status.st_uid != 0
        or evidence_status.st_gid != evidence_gid
        or stat.S_IMODE(evidence_status.st_mode) != 0o750
    ):
        fail("evidence directory is unsafe")
    serialized_members, pkginfo, pkginfo_fields = validate_package_archive(
        archive, manifest_entries, scenario
    )
    validate_pkginfo_authority(pkginfo_fields, pkginfo_reference)
    write_new_file(
        evidence_directory / "archive-members.tsv",
        ("# type\tpermissions\towner\tgroup\tpath\n" + "\n".join(serialized_members) + "\n").encode(
            "utf-8"
        ),
        0o640,
        evidence_gid,
    )
    write_new_file(evidence_directory / "PKGINFO", pkginfo, 0o640, evidence_gid)
    manifest_lines = ["\t".join(MANIFEST_HEADER)]
    for archive_path in sorted(manifest_entries):
        manifest_lines.append("\t".join((archive_path, *manifest_entries[archive_path])))
    write_new_file(
        evidence_directory / "validated-payload.tsv",
        ("\n".join(manifest_lines) + "\n").encode("utf-8"),
        0o640,
        evidence_gid,
    )
    return 0


def stage_trusted(arguments: list[str]) -> int:
    if len(arguments) != 3:
        fail("stage-trusted requires source, snapshot, and evidence directory")
    destination = Path(arguments[1])
    evidence = Path(arguments[2])
    if destination.parent != STAGING_ROOT / "aur-install" or evidence != Path("/var/log/moguet-live-aur/aur-install"):
        fail("trusted positive destination/case mismatch")
    require_destination_path(destination)
    source = TrustedStageInput(arguments[0])
    try:
        require_source_status(source.metadata, 0)
        digest = hash_descriptor(source.descriptor)
        source.reprove()
        copied, staged = copy_and_hash(source.descriptor, destination, 1000)
        source.reprove()
        after = hash_descriptor(source.descriptor)
        source.reprove()
        if len({digest, copied, staged, after}) != 1:
            fail("trusted source/copy/snapshot hashes differ")
        write_new_file(evidence / "trusted-source.json", trusted_record(source, digest).encode("utf-8"), 0o640, 1000)
        print(f"source_sha256_before={digest}\ncopy_sha256={copied}\nstaged_sha256={staged}\nsource_sha256_after={after}")
        print(f"source_mtime_ns={source.metadata.st_mtime_ns}\nsource_uid=0\nsource_mode=0600")
    finally:
        source.close()
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: aur-stage-artifact.py COMMAND ...", file=sys.stderr)
        return 2
    command = sys.argv[1]
    if command == "check-trusted":
        return check_trusted(sys.argv[2:])
    if command == "stage-trusted":
        return stage_trusted(sys.argv[2:])
    if command == "verify-trusted":
        return verify_trusted(sys.argv[2:])
    if command == "stage":
        return stage_artifact(sys.argv[2:])
    if command == "manifest":
        return create_reference_manifest(sys.argv[2:])
    if command == "pkginfo-manifest":
        return create_reference_pkginfo_manifest(sys.argv[2:])
    if command == "validate":
        return validate_staged_archive(sys.argv[2:])
    print(f"unknown command: {command}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"moguet-live-aur-stage: {error}", file=sys.stderr)
        raise SystemExit(1)
