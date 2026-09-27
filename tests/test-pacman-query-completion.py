#!/usr/bin/env python3
"""Raw upstream grammar/budgets/provenance and Moguet precedence, no catalogue."""
import os
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import pacman_query_completion as upstream
from generate_completions import load_schema, project_query_tokens, query_completion_tokens


def row(syntax, prose="opaque prose --never-extract-this"):
    prefix = "      " if syntax.startswith("--") else "  "
    head = prefix + syntax
    return head.ljust(23) + prose + "\n" if len(head) < 23 else head + "\n" + " " * 23 + prose + "\n"


def help_input(*records):
    return (upstream.QUERY_HEADER + "".join(records)).encode()


def rejected(raw):
    try:
        upstream.parse_query_help(raw)
    except upstream.ProjectionError:
        return
    raise AssertionError("malformed/budget input was accepted")


snapshot = upstream.load_snapshot()
raw = (upstream.SNAPSHOT_DIRECTORY / "help.txt").read_bytes()
tokens = {token for record in snapshot.options for token in record.tokens}
assert tokens and all("<" not in token and " " not in token for token in tokens)
assert snapshot.options == tuple(sorted(snapshot.options, key=lambda item: item.canonical_token))
assert upstream.parse_query_help(raw) == snapshot.options
version_input = (upstream.SNAPSHOT_DIRECTORY / "version.txt").read_bytes()
for invalid in (version_input.replace(b"Pacman v7.", b"Pacman v8."),
                version_input.replace(b"Pacman v", b"unknown v"), version_input + b"\r",
                version_input + b"\xc3\xa9", version_input * 2,
                version_input + b"x" * upstream.MAX_INPUT_BYTES):
    try:
        upstream.parse_version(invalid)
    except upstream.ProjectionError:
        pass
    else:
        raise AssertionError("unsupported version interface was accepted")
specimens = upstream.parse_query_help(help_input(row("-x, --fixture <arg>"), row("-z"), row("--another")))
assert {record.tokens for record in specimens} == {("-x", "--fixture"), ("-z",), ("--another",)}
assert all("never-extract" not in token for record in specimens for token in record.tokens)

for malformed in (
    help_input("  -x,--fixture          bad comma spacing\n"),
    help_input(row("-x, --fixture"), row("-x, --different")),
    help_input(row("-x, --fixture"), row("-y, --fixture")),
    help_input(row("--fixture"), row("--fixture")),
    help_input(row("--one, --two")),
    help_input(row("--fixture=value")),
    help_input(row("--fixture;touch")),
    help_input(row("--fixture <bad operand>")),
    help_input(" " * 23 + "orphan continuation\n"),
    help_input("      --very-long-syntax-only-option\n"),
    raw.replace(b"options:", b"localized options:"),
    raw.replace(b"  -b,", b"   -b,"),
    raw[:-1], raw + b"\0", raw.replace(b"  -b,", b"\t-b,"),
    raw + b"\xc3\xa9",
    raw + b"x" * upstream.MAX_INPUT_BYTES,
    help_input(*(row("--fixture" + str(i)) for i in range(upstream.MAX_OPTIONS + 1))),
):
    rejected(malformed)
# Isolate the token guard from the stricter record guard. Actual accepted
# records can have at most two tokens and only 52 distinct short spellings.
with patch.object(upstream, "MAX_OPTIONS", upstream.MAX_TOKENS + 1):
    rejected(help_input(*(row("--fixture" + str(i)) for i in range(upstream.MAX_TOKENS + 1))))

schema = load_schema()
collision = next(item for item in schema.query_tokens if item.token == "--noconfirm")
explicit = next(item for item in schema.options if item.token == collision.token)
assert collision.category == explicit.ownership and collision.category != "upstream-delegated"
assert query_completion_tokens(schema).count(collision.token) == 1
# A source-build-only Moguet token cannot be promoted by a discovered spelling.
owned = next(item for item in schema.options if item.token == "--rebuild")
synthetic = upstream.QuerySnapshot(snapshot.pacman_version, snapshot.libalpm_version, snapshot.help_sha256,
                                   upstream.parse_query_help(help_input(row(owned.token))))
projected = project_query_tokens(schema, synthetic)
assert projected.query_tokens[0].category == owned.ownership
assert not projected.query_tokens[0].projected and owned.token not in query_completion_tokens(projected)
# Rendering uses raw tracked input and never host capture, even if discovery is unavailable.
with patch.object(upstream, "capture", side_effect=FileNotFoundError("pacman absent")):
    assert load_schema().query_tokens == schema.query_tokens

with tempfile.TemporaryDirectory(prefix="moguet-pacman-capture-", dir=os.environ.get("TMPDIR")) as temp:
    root = Path(temp)
    executable = root / "fixed-test-pacman"
    def capture_body(body):
        executable.write_text("#!/usr/bin/python3\nimport os, sys, time\n" + body)
        executable.chmod(0o755)
        with patch.object(upstream, "PACMAN_PATH", str(executable)):
            return upstream.capture(("-Q", "--help"))
    assert capture_body("sys.stdout.write(os.environ['LC_ALL'])\n") == b"C"
    assert len(capture_body(f"sys.stdout.write('x' * {upstream.MAX_INPUT_BYTES})\n")) == upstream.MAX_INPUT_BYTES
    for body in ("sys.stdout.write('partial'); sys.exit(1)\n",
                 "sys.stderr.write('diagnostic')\n",
                 "sys.stdout.write('x' * 20000)\n",
                 "sys.stderr.write('x' * 20000)\n",
                 "time.sleep(5)\n"):
        try:
            capture_body(body)
        except upstream.ProjectionError:
            pass
        else:
            raise AssertionError("failed capture was accepted")
    # Snapshot corruption/missing input fails before canonical generation.
    import shutil
    directory = root / "snapshot"
    shutil.copytree(upstream.SNAPSHOT_DIRECTORY, directory)
    (directory / "help.txt").write_bytes(raw + b"changed")
    try:
        upstream.load_snapshot(directory)
    except upstream.ProjectionError:
        pass
    else:
        raise AssertionError("snapshot hash corruption accepted")
    (directory / "help.txt").write_bytes(raw)
    original_metadata = (directory / "capture.json").read_text()
    for changed in (original_metadata.replace('"locale": "C"', '"locale": "ja_JP"'),
                    original_metadata.replace('"operation": "-Q"', '"operation": "-S"'),
                    original_metadata.replace('"schema": 1', '"schema": true'),
                    original_metadata.replace('"schema": 1', '"schema": 1.0'),
                    original_metadata.replace('"schema": 1', '"schema": 1, "schema": 1'),
                    '[]'):
        (directory / "capture.json").write_text(changed)
        try:
            upstream.load_snapshot(directory)
        except upstream.ProjectionError:
            pass
        else:
            raise AssertionError("invalid snapshot identity accepted")
    import json, hashlib
    original = json.loads(original_metadata)
    def snapshot_rejected():
        try:
            upstream.load_snapshot(directory)
        except (upstream.ProjectionError, OSError, ValueError):
            return
        raise AssertionError("invalid/missing snapshot accepted")
    for name in ("capture.json", "help.txt", "version.txt"):
        shutil.rmtree(directory)
        shutil.copytree(upstream.SNAPSHOT_DIRECTORY, directory)
        (directory / name).unlink()
        snapshot_rejected()
        shutil.copytree(upstream.SNAPSHOT_DIRECTORY, directory, dirs_exist_ok=True)
        (directory / name).write_bytes(b"x" * (upstream.MAX_INPUT_BYTES + 1))
        snapshot_rejected()
    for changed_help, changed_version in ((raw.replace(b"options:", b"unexpected:"), version_input),
                                         (raw, version_input.replace(b"Pacman v7.", b"Pacman v8.")),
                                         (raw, version_input.replace(b"Pacman v", b"unknown v"))):
        metadata = original | {"help_sha256": hashlib.sha256(changed_help).hexdigest(),
                               "version_sha256": hashlib.sha256(changed_version).hexdigest()}
        (directory / "capture.json").write_text(json.dumps(metadata))
        (directory / "help.txt").write_bytes(changed_help)
        (directory / "version.txt").write_bytes(changed_version)
        snapshot_rejected()
    shutil.copytree(upstream.SNAPSHOT_DIRECTORY, directory, dirs_exist_ok=True)
    (directory / "capture.json").write_text('{broken json')
    snapshot_rejected()
    shutil.copytree(upstream.SNAPSHOT_DIRECTORY, directory, dirs_exist_ok=True)
    (directory / "version.txt").write_bytes(version_input + b"changed")
    snapshot_rejected()
    with patch.object(upstream, "PACMAN_PATH", str(root / "missing-pacman")):
        try:
            upstream.capture(("--version",))
        except OSError:
            pass
        else:
            raise AssertionError("absent host pacman became a capture")

print(f"pacman-query projection fixture/budgets/collision/reproducibility: PASS; {len(snapshot.options)} records, {len(tokens)} raw tokens")
