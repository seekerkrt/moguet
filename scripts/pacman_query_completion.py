#!/usr/bin/env python3
"""Pinned raw upstream spelling authority for one delegated query operation.

Only syntax-column tokens are projected. Placeholders/prose carry no arity,
effect, occurrence or ownership information. Normal rendering never captures
the host; capture/host validation are explicit operator actions.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import sys
import time

PACMAN_PATH = "/usr/bin/pacman"  # Same fixed path as the repository's trusted transports.
SNAPSHOT_DIRECTORY = Path(__file__).resolve().parent.parent / "completions/upstream/pacman-query"
MAX_INPUT_BYTES = 16384
MAX_OPTIONS = 64
MAX_TOKENS = 128
CAPTURE_SECONDS = 2.0
QUERY_OPERATION = "-Q"
QUERY_HEADER = "usage:  pacman {-Q --query} [options] [package(s)]\noptions:\n"
TOKEN = r"(?:-[A-Za-z]|--[a-z][a-z0-9]*(?:-[a-z0-9]+)*)"
SYNTAX = re.compile(rf"(?P<tokens>{TOKEN}(?:, {TOKEN})?)(?: <[A-Za-z][A-Za-z0-9_-]*>)?")
VERSION = re.compile(rb"Pacman v([0-9]+\.[0-9]+\.[0-9]+) - libalpm v([0-9]+\.[0-9]+\.[0-9]+)")


class ProjectionError(ValueError):
    pass


@dataclass(frozen=True)
class QueryOption:
    tokens: tuple[str, ...]

    @property
    def canonical_token(self) -> str:
        # Identity for dedup/presentation only, not a semantic alias contract.
        return self.tokens[-1]


@dataclass(frozen=True)
class QuerySnapshot:
    pacman_version: str
    libalpm_version: str
    help_sha256: str
    options: tuple[QueryOption, ...]


def checked_ascii(raw: bytes) -> str:
    if not raw or len(raw) > MAX_INPUT_BYTES:
        raise ProjectionError("upstream input is empty or exceeds byte budget")
    if any(byte != 10 and not 32 <= byte <= 126 for byte in raw):
        raise ProjectionError("upstream input has unsupported/control characters")
    return raw.decode("ascii")


def parse_query_help(raw: bytes) -> tuple[QueryOption, ...]:
    text = checked_ascii(raw)
    if not text.startswith(QUERY_HEADER) or not text.endswith("\n"):
        raise ProjectionError("unsupported pacman query help header/termination")
    rows = []
    seen = set()
    needs_description = False
    for line in text[len(QUERY_HEADER):].splitlines():
        if line.startswith("                       "):
            continuation = line[23:]
            if not rows or not continuation or continuation.startswith((" ", "-")):
                raise ProjectionError("ambiguous help continuation")
            needs_description = False
            continue
        if needs_description:
            raise ProjectionError("option record lacks description continuation")
        if not line.startswith("  ") or line.startswith("       "):
            raise ProjectionError("unsupported help record indentation")
        # The supported upstream format has a fixed description column (23),
        # with only one separator space for a syntax field ending at 22.
        # Wrapped long-only rows have syntax alone. No prose is parsed.
        body = line[2:] if not line.startswith("      ") else line[6:]
        match = SYNTAX.match(body)
        if not match:
            raise ProjectionError("unsupported/ambiguous option syntax")
        syntax = match.group()
        token_text = syntax.split(" <", 1)[0]
        tokens = tuple(token_text.split(", "))
        if len(tokens) == 2 and not (re.fullmatch(r"-[A-Za-z]", tokens[0]) and tokens[1].startswith("--")):
            raise ProjectionError("unsupported alias pairing")
        expected_indent = 2 if tokens[0].startswith("-") and not tokens[0].startswith("--") else 6
        if not line.startswith(" " * expected_indent + tokens[0]):
            raise ProjectionError("option indentation does not match upstream syntax")
        if any(token in seen for token in tokens):
            raise ProjectionError("duplicate upstream token/alias")
        seen.update(tokens)
        rows.append(QueryOption(tokens))
        if len(rows) > MAX_OPTIONS or len(seen) > MAX_TOKENS:
            raise ProjectionError("upstream option count exceeds budget")
        syntax_end = expected_indent + match.end()
        if syntax_end == len(line):
            if len(tokens) != 1 or not tokens[0].startswith("--") or syntax_end < 23:
                raise ProjectionError("only long-only records may wrap before prose")
            needs_description = True
        elif (syntax_end >= 23 or len(line) <= 23 or
              line[syntax_end:23] != " " * (23 - syntax_end) or line[23] == " "):
            raise ProjectionError("unsupported syntax/description column boundary")
    if not rows or needs_description:
        raise ProjectionError("incomplete query option input")
    return tuple(sorted(rows, key=lambda row: row.canonical_token))


def parse_version(raw: bytes) -> tuple[str, str]:
    checked_ascii(raw)
    versions = VERSION.findall(raw)
    if len(versions) != 1 or versions[0][0].split(b".")[0] != b"7":
        raise ProjectionError("unsupported pacman version interface (supported major: 7)")
    return tuple(value.decode("ascii") for value in versions[0])


def read_bounded(path: Path) -> bytes:
    with path.open("rb") as source:
        raw = source.read(MAX_INPUT_BYTES + 1)
    if len(raw) > MAX_INPUT_BYTES:
        raise ProjectionError(f"snapshot input exceeds byte budget: {path.name}")
    return raw


def unique_metadata(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProjectionError("duplicate snapshot metadata field")
        result[key] = value
    return result


def load_snapshot(directory: Path = SNAPSHOT_DIRECTORY) -> QuerySnapshot:
    metadata = json.loads(read_bounded(directory / "capture.json"), object_pairs_hook=unique_metadata)
    keys = {"schema", "operation", "locale", "help_argv", "version_argv", "pacman_version",
            "libalpm_version", "help_sha256", "version_sha256"}
    if (not isinstance(metadata, dict) or set(metadata) != keys or
        metadata.get("schema") != 1 or metadata.get("operation") != QUERY_OPERATION or
        metadata.get("locale") != "C" or
        metadata.get("help_argv") != [PACMAN_PATH, "-Q", "--help"] or
        metadata.get("version_argv") != [PACMAN_PATH, "--version"]):
        raise ProjectionError("unsupported upstream snapshot identity")
    raw_help = read_bounded(directory / "help.txt")
    raw_version = read_bounded(directory / "version.txt")
    for name, raw in (("help", raw_help), ("version", raw_version)):
        if hashlib.sha256(raw).hexdigest() != metadata.get(name + "_sha256"):
            raise ProjectionError("upstream snapshot hash mismatch: " + name)
    version, alpm = parse_version(raw_version)
    if (version, alpm) != (metadata.get("pacman_version"), metadata.get("libalpm_version")):
        raise ProjectionError("upstream snapshot version mismatch")
    return QuerySnapshot(version, alpm, metadata["help_sha256"], parse_query_help(raw_help))


def capture(arguments: tuple[str, ...]) -> bytes:
    # Absolute executable, shell-free argv, no caller-selected PATH or locale.
    with subprocess.Popen([PACMAN_PATH, *arguments], stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env={"LC_ALL": "C"}, start_new_session=True) as process:
        output = bytearray()
        error = bytearray()
        deadline = time.monotonic() + CAPTURE_SECONDS
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ, output)
                selector.register(process.stderr, selectors.EVENT_READ, error)
                while selector.get_map():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise ProjectionError("pacman capture deadline exceeded")
                    for key, _ in selector.select(remaining):
                        chunk = os.read(key.fileobj.fileno(), 4096)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        key.data.extend(chunk)
                        if len(key.data) > MAX_INPUT_BYTES:
                            raise ProjectionError("pacman capture byte budget exceeded")
                status = process.wait(timeout=max(0.001, deadline - time.monotonic()))
            if status or error:
                raise ProjectionError("pacman capture failed or emitted diagnostics")
            return bytes(output)
        except BaseException:
            # Captures are read-only; kill/reap the private group on a failed
            # budget rather than publishing partial input as a new authority.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            raise


def capture_inputs() -> tuple[dict, bytes, bytes]:
    raw_version = capture(("--version",))
    version, alpm = parse_version(raw_version)
    raw_help = capture(("-Q", "--help"))
    parse_query_help(raw_help)
    metadata = dict(schema=1, operation=QUERY_OPERATION, locale="C",
                    help_argv=[PACMAN_PATH, "-Q", "--help"], version_argv=[PACMAN_PATH, "--version"],
                    pacman_version=version, libalpm_version=alpm,
                    help_sha256=hashlib.sha256(raw_help).hexdigest(),
                    version_sha256=hashlib.sha256(raw_version).hexdigest())
    return metadata, raw_help, raw_version


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--capture", type=Path, metavar="NEW_DIRECTORY")
    mode.add_argument("--validate-host", action="store_true")
    args = parser.parse_args()
    try:
        metadata, raw_help, raw_version = capture_inputs()
        if args.capture:
            args.capture.mkdir(parents=True, exist_ok=False)
            for name, raw in (("help.txt", raw_help), ("version.txt", raw_version),
                              ("capture.json", (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode())):
                with (args.capture / name).open("xb") as target:
                    target.write(raw)
            print(json.dumps(metadata, sort_keys=True))
        else:
            snapshot = load_snapshot()
            current = parse_query_help(raw_help)
            if current != snapshot.options:
                raise ProjectionError("host token projection differs from selected snapshot; explicit refresh required")
            print(json.dumps(metadata | {"options": len(current), "tokens": sum(len(row.tokens) for row in current),
                                         "token_projection_matches_snapshot": True}, sort_keys=True))
        return 0
    except (ProjectionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print("pacman-query-completion: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
