#!/usr/bin/env python3

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import json
from pathlib import Path
import re
import sys

from generate_completions import (
    REPOSITORY_ROOT,
    generated_files,
    load_descriptions,
    load_schema,
)


ANSI_HELP_ENTRY = re.compile(r"^    \x1b\[1m(.*?)\x1b\[0m", re.MULTILINE)
ANSI_HELP_LINE = re.compile(r"^    \x1b\[1m(.*?)\x1b\[0m(.*)$")
HELP_DESCRIPTION_COLUMN = 42
LONG_TOKEN = re.compile(r"(?<![A-Za-z0-9-])--[a-z][a-z0-9-]*(?![A-Za-z0-9-])")
SHORT_TOKEN = re.compile(r"(?<![A-Za-z0-9-])-(?!-)[A-Za-z][A-Za-z]*(?![A-Za-z0-9-])")
CLI_DASH_TOKEN = re.compile(
    r"(?<![A-Za-z0-9-])(?:--[a-z][a-z0-9-]*|-(?!-)[A-Za-z][A-Za-z]*)(?![A-Za-z0-9-])"
)
MARKDOWN_MARKER = re.compile(r"^<!-- parity:([a-z0-9][a-z0-9-]*) -->\s*$")
MAN_MARKER = re.compile(r'^\.\\" parity:([a-z0-9][a-z0-9-]*)\s*$')
README_SECTION_SLUGS = (
    "overview",
    "name",
    "status",
    "safety",
    "installation",
    "usage",
    "configuration",
    "xdg",
    "localization",
    "compatibility",
    "development",
    "license",
)
MIGRATION_SECTION_SLUGS = (
    "overview",
    "preparation",
    "identity",
    "backup",
    "remove-v1",
    "install-v2",
    "configuration",
    "legacy-data",
    "verification",
    "rollback",
    "maintenance",
)
MAN_SECTION_SLUGS = (
    "name",
    "synopsis",
    "description",
    "commands",
    "options",
    "configuration",
    "files",
    "environment",
    "safety",
    "examples",
    "migration",
    "see-also",
    "license",
    "author",
)
OBSOLETE_UNQUOTED_USER_CONFIG_HELP_SYNTAX = (
    "review.pkgbuild = prompt|skip",
    "review.diff = prompt|skip",
    "build.mode = normal|rebuild|clean",
)
OBSOLETE_UNQUOTED_USER_CONFIG_HELP_SOURCE_LITERALS = tuple(
    f'"{syntax}"' for syntax in OBSOLETE_UNQUOTED_USER_CONFIG_HELP_SYNTAX
)


@dataclass(frozen=True)
class PublicSurface:
    operations: frozenset[str]
    options: frozenset[str]


@dataclass(frozen=True)
class SemanticTextContract:
    required_patterns: tuple[str, ...]
    forbidden_patterns: tuple[str, ...] = ()


def fail(message: str) -> None:
    print(f"public-documentation-check: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        fail(f"missing required file: {path.relative_to(REPOSITORY_ROOT)}")


def shown_path(path: Path) -> str:
    try:
        return str(path.relative_to(REPOSITORY_ROOT))
    except ValueError:
        return str(path)


def read_current_project_version(repository_root: Path) -> str:
    version_path = repository_root / "VERSION"
    lines = read_text(version_path).splitlines()
    if len(lines) != 1 or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", lines[0]) is None:
        fail(f"{shown_path(version_path)} must contain exactly one X.Y.Z version")
    return lines[0]


def assert_semantic_text_contract(
    label: str,
    text: str,
    contract: SemanticTextContract,
) -> None:
    normalized = " ".join(text.split()).casefold()
    missing = [
        pattern
        for pattern in contract.required_patterns
        if re.search(pattern, normalized) is None
    ]
    forbidden = [
        pattern
        for pattern in contract.forbidden_patterns
        if re.search(pattern, normalized) is not None
    ]
    if missing:
        fail(
            f"{label} is missing semantic pattern(s): "
            + ", ".join(repr(pattern) for pattern in missing)
        )
    if forbidden:
        fail(
            f"{label} retains obsolete semantics: "
            + ", ".join(repr(pattern) for pattern in forbidden)
        )


def runtime_help_descriptions(label: str, text: str) -> dict[str, str]:
    descriptions: dict[str, str] = {}
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        match = ANSI_HELP_LINE.match(lines[index])
        if match is None:
            index += 1
            continue

        syntax = match.group(1).strip()
        if syntax in descriptions:
            fail(f"{label} repeats help entry {syntax!r}")
        parts = [match.group(2).strip()] if match.group(2).strip() else []
        index += 1
        while index < len(lines) and lines[index].startswith(
            " " * HELP_DESCRIPTION_COLUMN
        ):
            continuation = lines[index][HELP_DESCRIPTION_COLUMN:].strip()
            if continuation:
                parts.append(continuation)
            index += 1
        descriptions[syntax] = " ".join(parts)
    return descriptions


def reviewed_source_runtime_help_contracts(
    locale: str,
) -> dict[str, SemanticTextContract]:
    if locale == "en":
        return {
            "review.pkgbuild = \"prompt\"|\"skip\"": SemanticTextContract(
                (
                    r"(?:invocation-local|this invocation)",
                    r"pkgbuild.*\.install",
                    r"(?:editor|editing)",
                    r"not (?:upstream )?(?:reviewed-source|reviewed source|review) acceptance",
                ),
                (r"pkgbuild review policy",),
            ),
            "review.diff = \"prompt\"|\"skip\"": SemanticTextContract(
                (
                    r"repository (?:update )?diff",
                    r"aur (?:reviewed-source|reviewed source) review",
                    r"(?:skip|skipping)",
                    r"(?:does not advance|without advancing).*reviewed state|reviewed state.*unchanged",
                ),
                (r"repository update diff policy",),
            ),
            "--diff": SemanticTextContract(
                (
                    r"repository (?:updates|diff)",
                    r"aur",
                    r"exact (?:fetched )?target",
                    r"(?:previous|last) reviewed revision",
                    r"all tracked source.*(?:first review|initial)|(?:first review|initial).*all tracked source|initial full tracked-file review",
                    r"advance.*reviewed state.*explicit acceptance|explicit acceptance.*advance.*reviewed state",
                ),
            ),
            "--nodiff": SemanticTextContract(
                (
                    r"repository diff",
                    r"(?:reviewed-source|reviewed source) review",
                    r"without advancing reviewed state|does not advance reviewed state",
                ),
                (r"skip reviewed source changes",),
            ),
        }
    if locale == "ja":
        return {
            "review.pkgbuild = \"prompt\"|\"skip\"": SemanticTextContract(
                (
                    r"invocation-local|今回のinvocation|この実行",
                    r"pkgbuild.*\.install",
                    r"editor|編集",
                    r"reviewed-source acceptanceでは(?:ない|ありません)",
                ),
                (r"pkgbuildの確認方針",),
            ),
            "review.diff = \"prompt\"|\"skip\"": SemanticTextContract(
                (
                    r"repository diff|リポジトリ(?:更新)?差分",
                    r"aur (?:reviewed-source|reviewed source) review",
                    r"skip|省略",
                    r"reviewed state.*進め(?:ない|ません)",
                ),
                (r"リポジトリ更新差分の確認方針",),
            ),
            "--diff": SemanticTextContract(
                (
                    r"(?:repository|リポジトリ)(?: update|更新)",
                    r"aur",
                    r"exact (?:fetched )?target",
                    r"(?:previous|前回の) reviewed revision",
                    r"初回review.*tracked (?:source|file)全体|initial full tracked-file review",
                    r"explicit acceptance.*reviewed state.*進め",
                ),
            ),
            "--nodiff": SemanticTextContract(
                (
                    r"repository diff|リポジトリ差分",
                    r"(?:reviewed-source|reviewed source) review",
                    r"reviewed state.*進め(?:ない|ません)",
                ),
            ),
        }
    raise ValueError(f"unsupported reviewed-source help locale: {locale}")


def check_reviewed_source_runtime_help(
    english_help: str,
    japanese_help: str,
) -> None:
    for locale, label, text in (
        ("en", "English runtime help", english_help),
        ("ja", "Japanese runtime help", japanese_help),
    ):
        descriptions = runtime_help_descriptions(label, text)
        obsolete_syntax = [
            syntax
            for syntax in OBSOLETE_UNQUOTED_USER_CONFIG_HELP_SYNTAX
            if syntax in descriptions
        ]
        if obsolete_syntax:
            fail(
                f"{label} retains obsolete unquoted user-config syntax: "
                + ", ".join(repr(syntax) for syntax in obsolete_syntax)
            )
        for syntax, contract in reviewed_source_runtime_help_contracts(
            locale
        ).items():
            description = descriptions.get(syntax)
            if description is None:
                fail(f"{label} is missing reviewed-source entry {syntax!r}")
            assert_semantic_text_contract(
                f"{label} entry {syntax!r}", description, contract
            )


def system_aur_update_runtime_help_contracts(
    locale: str,
) -> dict[str, SemanticTextContract]:
    if locale == "en":
        return {
            "-Syu [--needed]": SemanticTextContract(
                (
                    r"official repository packages.*normal installed aur packages",
                    r"do not read or apply saved source-build preferences",
                    r"sequentially",
                    r"does not roll back",
                )
            ),
            "-Syu --repo [--needed]": SemanticTextContract(
                (
                    r"repository system upgrade only",
                    r"full pacman-compatible repository argument tail",
                )
            ),
            "upgrade": SemanticTextContract(
                (r"source-aware system update", r"saved source-build preferences")
            ),
            "upgrade-aur": SemanticTextContract(
                (
                    r"installed aur packages only",
                    r"do not update repository packages",
                    r"apply saved source-build preferences",
                )
            ),
        }
    if locale == "ja":
        return {
            "-Syu [--needed]": SemanticTextContract(
                (
                    r"公式リポジトリパッケージ.*通常のインストール済みaurパッケージ",
                    r"保存済みソースビルド設定を読み取りも適用もしません",
                    r"順番に実行",
                    r"ロールバックしません",
                )
            ),
            "-Syu --repo [--needed]": SemanticTextContract(
                (
                    r"リポジトリのシステム更新だけを実行",
                    r"pacman互換のリポジトリ引数をすべて許可",
                )
            ),
            "upgrade": SemanticTextContract(
                (r"ソース対応システム更新", r"保存済みソースビルド設定")
            ),
            "upgrade-aur": SemanticTextContract(
                (
                    r"インストール済みのaurパッケージだけを更新",
                    r"リポジトリパッケージは更新せず",
                    r"保存済みソースビルド設定を適用",
                )
            ),
        }
    raise ValueError(f"unsupported system/AUR help locale: {locale}")


def check_system_aur_update_runtime_help(
    english_help: str,
    japanese_help: str,
) -> None:
    for locale, label, text in (
        ("en", "English runtime help", english_help),
        ("ja", "Japanese runtime help", japanese_help),
    ):
        descriptions = runtime_help_descriptions(label, text)
        for syntax, contract in system_aur_update_runtime_help_contracts(
            locale
        ).items():
            description = descriptions.get(syntax)
            if description is None:
                fail(f"{label} is missing system/AUR entry {syntax!r}")
            assert_semantic_text_contract(
                f"{label} entry {syntax!r}", description, contract
            )


def expected_surface(schema) -> PublicSurface:
    return PublicSurface(
        frozenset(operation.token for operation in schema.operations),
        frozenset(
            option.token
            for option in schema.options
            if option.definition_role != "schema-only"
        ),
    )


def format_tokens(tokens: frozenset[str] | set[str]) -> str:
    return ", ".join(sorted(tokens)) if tokens else "(none)"


def assert_surface(label: str, actual: PublicSurface, expected: PublicSurface) -> None:
    missing_operations = expected.operations - actual.operations
    extra_operations = actual.operations - expected.operations
    missing_options = expected.options - actual.options
    extra_options = actual.options - expected.options
    if not any((missing_operations, extra_operations, missing_options, extra_options)):
        return

    details = []
    if missing_operations:
        details.append("missing operations: " + format_tokens(missing_operations))
    if extra_operations:
        details.append("extra operations: " + format_tokens(extra_operations))
    if missing_options:
        details.append("missing options: " + format_tokens(missing_options))
    if extra_options:
        details.append("extra options: " + format_tokens(extra_options))
    fail(f"{label} differs from source/cli_authority.hpp ({'; '.join(details)})")


def help_surface(path: Path, expected: PublicSurface) -> PublicSurface:
    entries = ANSI_HELP_ENTRY.findall(read_text(path))
    if not entries:
        fail(f"no formatted help entries found in {path}")

    operation_tokens: set[str] = set()
    option_tokens: set[str] = set()
    unknown_tokens: set[str] = set()
    all_expected_operations = expected.operations
    all_expected_options = expected.options

    for entry in entries:
        dash_tokens = set(LONG_TOKEN.findall(entry)) | set(SHORT_TOKEN.findall(entry))
        for token in dash_tokens:
            if token in all_expected_operations:
                operation_tokens.add(token)
            elif token in all_expected_options:
                option_tokens.add(token)
            else:
                unknown_tokens.add(token)

        word_operation = re.match(r"^([a-z][a-z0-9-]*)(?=\s|$)", entry)
        if word_operation is not None:
            token = word_operation.group(1)
            if token in all_expected_operations:
                operation_tokens.add(token)
            else:
                unknown_tokens.add(token)

    if unknown_tokens:
        fail(
            f"{path} exposes tokens outside source/cli_authority.hpp: "
            + format_tokens(unknown_tokens)
        )
    return PublicSurface(frozenset(operation_tokens), frozenset(option_tokens))


def markdown_code_fragments(text: str) -> str:
    fragments: list[str] = []
    in_fence = False
    for line in text.splitlines():
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            fragments.append(line)
        fragments.extend(re.findall(r"(?<!`)`([^`\n]+)`(?!`)", line))
    return "\n".join(fragments)


def man_code_fragments(text: str) -> str:
    normalized = text.replace(r"\-", "-")
    fragments = re.findall(r"\\fB(.*?)\\f[PR]", normalized, flags=re.DOTALL)
    for line in normalized.splitlines():
        if re.match(r"^\.(?:B|BI|BR|RB|IR)\s+", line):
            fragments.append(re.sub(r"^\.[A-Z]+\s+", "", line))
    return "\n".join(fragments)


def man_public_region(text: str, category: str, path: Path) -> str:
    begin_marker = f'.\\" PUBLIC {category} BEGIN'
    end_marker = f'.\\" PUBLIC {category} END'
    begin_count = text.count(begin_marker)
    end_count = text.count(end_marker)
    if begin_count != 1 or end_count != 1:
        fail(
            f"{path.relative_to(REPOSITORY_ROOT)} must contain exactly one "
            f"{begin_marker!r} and {end_marker!r} marker"
        )
    begin = text.index(begin_marker) + len(begin_marker)
    end = text.index(end_marker, begin)
    return text[begin:end]


def man_public_entry_payloads(region: str, category: str, path: Path) -> list[str]:
    lines = region.replace(r"\-", "-").splitlines()
    payloads: list[str] = []
    for index, line in enumerate(lines):
        if line.strip() != ".TP":
            continue
        for candidate in lines[index + 1 :]:
            stripped = candidate.strip()
            if not stripped or stripped.startswith(r'.\"'):
                continue
            match = re.match(r"^\.(?:B|BI|BR|RB|IR)\s+(.+)$", stripped)
            if match is None:
                fail(
                    f"{path.relative_to(REPOSITORY_ROOT)} PUBLIC {category} "
                    f"entry after .TP does not start with a supported bold macro: {stripped}"
                )
            payloads.append(match.group(1).replace('"', ""))
            break
        else:
            fail(
                f"{path.relative_to(REPOSITORY_ROOT)} PUBLIC {category} has an unterminated .TP entry"
            )
    if not payloads:
        fail(
            f"{path.relative_to(REPOSITORY_ROOT)} PUBLIC {category} region has no .TP entries"
        )
    return payloads


def normalized_cli_form(value: str) -> str:
    normalized = " ".join(value.strip().split())
    if normalized.startswith("moguet "):
        return normalized[len("moguet ") :].lstrip()
    return normalized


def man_command_forms(payloads: list[str]) -> list[str]:
    forms: list[str] = []
    for payload in payloads:
        normalized = normalized_cli_form(payload)
        forms.extend(
            normalized_cli_form(form)
            for form in re.split(r"\s+\|\s+", normalized)
        )
    return forms


def option_tokens_for_ids(schema, identities: tuple[int, ...]) -> set[str]:
    identity_set = set(identities)
    return {
        option.token
        for option in schema.options
        if option.identity in identity_set
    }


def is_closed_command_form(form: str, schema) -> bool:
    words = form.split()
    if not words:
        return False
    operation = next(
        (operation for operation in schema.operations if operation.token == words[0]),
        None,
    )
    if operation is None or not operation.forms:
        return False
    if not operation.open_grammar:
        return True

    # An intercepted operation may retain an open pacman fallback while also
    # owning exact closed forms, including a selector-free default form such
    # as targetless -Syu. Exact canonical forms remain documentation-owned.
    if form in {operation_form.syntax for operation_form in operation.forms}:
        return True

    selector_ids = tuple(
        dict.fromkeys(
            identity
            for operation_form in operation.forms
            for identity in operation_form.selector_ids
        )
    )
    selector_tokens = option_tokens_for_ids(schema, selector_ids)
    return bool(selector_tokens.intersection(CLI_DASH_TOKEN.findall(form)))


def format_form_counts(forms: Counter[str]) -> str:
    return ", ".join(
        f"{form!r}" if count == 1 else f"{form!r} x{count}"
        for form, count in sorted(forms.items())
    )


def assert_man_command_projection(
    path: Path, payloads: list[str], schema
) -> None:
    expected_forms = Counter(
        normalized_cli_form(form) for form in schema.canonical_grammar
    )
    actual_forms = Counter(
        form
        for form in man_command_forms(payloads)
        if is_closed_command_form(form, schema)
    )
    if actual_forms == expected_forms:
        return

    missing = expected_forms - actual_forms
    unexpected = actual_forms - expected_forms
    details: list[str] = []
    if missing:
        details.append("missing forms: " + format_form_counts(missing))
    if unexpected:
        details.append("unexpected forms: " + format_form_counts(unexpected))
    fail(
        f"{shown_path(path)} PUBLIC COMMANDS differs from "
        f"the canonical form projection ({'; '.join(details)})"
    )


def expected_option_definition_counts(schema) -> Counter[str]:
    return Counter(
        option.token
        for option in schema.options
        if option.definition_role == "definition"
    )


def assert_man_option_definition_projection(
    path: Path, payloads: list[str], schema
) -> Counter[str]:
    expected_counts = expected_option_definition_counts(schema)
    actual_counts: Counter[str] = Counter()
    unknown: set[str] = set()
    known_options = {option.token for option in schema.options}
    for payload in payloads:
        tokens = CLI_DASH_TOKEN.findall(payload)
        if not tokens:
            unknown.add(payload.strip())
        for token in tokens:
            if token in known_options:
                actual_counts[token] += 1
            else:
                unknown.add(token)

    if unknown:
        fail(
            f"{shown_path(path)} PUBLIC OPTIONS contains "
            f"tokens outside source/cli_authority.hpp: {format_tokens(unknown)}"
        )
    if actual_counts != expected_counts:
        missing = expected_counts - actual_counts
        unexpected = actual_counts - expected_counts
        details: list[str] = []
        if missing:
            details.append("missing definitions: " + format_form_counts(missing))
        if unexpected:
            details.append(
                "unexpected or duplicate definitions: "
                + format_form_counts(unexpected)
            )
        fail(
            f"{shown_path(path)} PUBLIC OPTIONS differs from "
            f"the option-definition projection ({'; '.join(details)})"
        )
    return actual_counts


def exact_man_public_surface(
    path: Path, expected: PublicSurface, schema
) -> PublicSurface:
    text = read_text(path)
    operation_counts: Counter[str] = Counter()
    command_option_counts: Counter[str] = Counter()
    unknown_commands: set[str] = set()

    command_region = man_public_region(text, "COMMANDS", path)
    command_payloads = man_public_entry_payloads(command_region, "COMMANDS", path)
    assert_man_command_projection(path, command_payloads, schema)
    for payload in command_payloads:
        dash_tokens = CLI_DASH_TOKEN.findall(payload)
        entry_operations: set[str] = set()
        for token in dash_tokens:
            if token in expected.operations:
                entry_operations.add(token)
            elif token in expected.options:
                command_option_counts[token] += 1
            else:
                unknown_commands.add(token)

        command_payload = payload.strip()
        if command_payload.startswith("moguet "):
            command_payload = command_payload[len("moguet ") :].lstrip()
        if not command_payload.startswith("-"):
            word = re.match(r"([a-z][a-z0-9-]*)(?=\s|$)", command_payload)
            if word is not None:
                token = word.group(1)
                if token in expected.operations:
                    entry_operations.add(token)
                else:
                    unknown_commands.add(token)
        operation_counts.update(entry_operations)

    option_region = man_public_region(text, "OPTIONS", path)
    option_payloads = man_public_entry_payloads(option_region, "OPTIONS", path)
    definition_option_counts = assert_man_option_definition_projection(
        path, option_payloads, schema
    )

    if unknown_commands:
        fail(
            f"{path.relative_to(REPOSITORY_ROOT)} PUBLIC COMMANDS contains "
            f"tokens outside source/cli_authority.hpp: {format_tokens(unknown_commands)}"
        )
    duplicate_operations = {token for token, count in operation_counts.items() if count != 1}
    if duplicate_operations:
        fail(
            f"{path.relative_to(REPOSITORY_ROOT)} PUBLIC COMMANDS repeats: "
            + format_tokens(duplicate_operations)
        )
    actual = PublicSurface(
        frozenset(operation_counts),
        frozenset(command_option_counts) | frozenset(definition_option_counts),
    )
    assert_surface(str(path.relative_to(REPOSITORY_ROOT)), actual, expected)
    return actual


def documented_surface(text: str, kind: str, expected: PublicSurface) -> PublicSurface:
    normalized = text.replace(r"\-", "-") if kind == "man" else text
    formatted = man_code_fragments(text) if kind == "man" else markdown_code_fragments(text)

    operations: set[str] = set()
    options: set[str] = set()
    for token in expected.operations:
        haystack = normalized if token.startswith("-") else formatted
        if re.search(
            rf"(?<![A-Za-z0-9-]){re.escape(token)}(?![A-Za-z0-9-])",
            haystack,
        ):
            operations.add(token)
    for token in expected.options:
        if re.search(
            rf"(?<![A-Za-z0-9-]){re.escape(token)}(?![A-Za-z0-9-])",
            normalized,
        ):
            options.add(token)
    return PublicSurface(frozenset(operations), frozenset(options))


def marked_regions(path: Path, marker: re.Pattern[str]) -> list[tuple[str, str]]:
    regions: list[tuple[str, list[str]]] = []
    for line in read_text(path).splitlines():
        match = marker.match(line)
        if match is not None:
            slug = match.group(1)
            if any(existing_slug == slug for existing_slug, _ in regions):
                fail(f"duplicate parity marker '{slug}' in {path.relative_to(REPOSITORY_ROOT)}")
            regions.append((slug, []))
        elif regions:
            regions[-1][1].append(line)

    if not regions:
        fail(f"no parity markers found in {path.relative_to(REPOSITORY_ROOT)}")
    return [(slug, "\n".join(lines)) for slug, lines in regions]


def validate_marked_regions(
    path: Path,
    regions: list[tuple[str, str]],
    kind: str,
    expected_slugs: tuple[str, ...],
) -> None:
    actual_slugs = tuple(slug for slug, _ in regions)
    if actual_slugs != expected_slugs:
        fail(
            f"{path.relative_to(REPOSITORY_ROOT)} major sections differ: "
            f"actual={actual_slugs}, expected={expected_slugs}"
        )

    for slug, body in regions:
        lines = [line.strip() for line in body.splitlines() if line.strip()]
        expected_heading_prefix = "## " if kind == "markdown" else ".SH "
        if not lines or not lines[0].startswith(expected_heading_prefix):
            fail(
                f"{path.relative_to(REPOSITORY_ROOT)} parity region '{slug}' "
                f"must start with {expected_heading_prefix.strip()!r}"
            )

        if kind == "markdown":
            substantive_lines = [
                line
                for line in lines[1:]
                if not line.startswith(("#", "<!--", "```"))
            ]
        else:
            structural_macros = {
                ".",
                ".TP",
                ".PP",
                ".RS",
                ".RE",
                ".nf",
                ".fi",
                ".na",
                ".ad",
            }
            substantive_lines = [
                line
                for line in lines[1:]
                if line not in structural_macros and not line.startswith(r'.\"')
            ]
        if not substantive_lines:
            fail(
                f"{path.relative_to(REPOSITORY_ROOT)} parity region '{slug}' "
                "has no substantive body"
            )


def compare_marked_documents(
    label: str,
    english_path: Path,
    japanese_path: Path,
    marker: re.Pattern[str],
    kind: str,
    expected: PublicSurface,
    expected_slugs: tuple[str, ...],
    require_full_surface: bool = False,
) -> None:
    english_regions = marked_regions(english_path, marker)
    japanese_regions = marked_regions(japanese_path, marker)
    validate_marked_regions(english_path, english_regions, kind, expected_slugs)
    validate_marked_regions(japanese_path, japanese_regions, kind, expected_slugs)
    english_slugs = [slug for slug, _ in english_regions]
    japanese_slugs = [slug for slug, _ in japanese_regions]
    if english_slugs != japanese_slugs:
        fail(
            f"{label} parity marker order differs: "
            f"English={english_slugs}, Japanese={japanese_slugs}"
        )

    english_union_operations: set[str] = set()
    english_union_options: set[str] = set()
    japanese_union_operations: set[str] = set()
    japanese_union_options: set[str] = set()
    for (slug, english), (_, japanese) in zip(english_regions, japanese_regions, strict=True):
        english_tokens = documented_surface(english, kind, expected)
        japanese_tokens = documented_surface(japanese, kind, expected)
        if english_tokens != japanese_tokens:
            missing_ja_operations = english_tokens.operations - japanese_tokens.operations
            extra_ja_operations = japanese_tokens.operations - english_tokens.operations
            missing_ja_options = english_tokens.options - japanese_tokens.options
            extra_ja_options = japanese_tokens.options - english_tokens.options
            details = []
            if missing_ja_operations:
                details.append("Japanese missing operations: " + format_tokens(missing_ja_operations))
            if extra_ja_operations:
                details.append("Japanese extra operations: " + format_tokens(extra_ja_operations))
            if missing_ja_options:
                details.append("Japanese missing options: " + format_tokens(missing_ja_options))
            if extra_ja_options:
                details.append("Japanese extra options: " + format_tokens(extra_ja_options))
            fail(f"{label} parity region '{slug}' differs ({'; '.join(details)})")

        english_union_operations.update(english_tokens.operations)
        english_union_options.update(english_tokens.options)
        japanese_union_operations.update(japanese_tokens.operations)
        japanese_union_options.update(japanese_tokens.options)

    if require_full_surface:
        assert_surface(
            f"English {label}",
            PublicSurface(
                frozenset(english_union_operations), frozenset(english_union_options)
            ),
            expected,
        )
        assert_surface(
            f"Japanese {label}",
            PublicSurface(
                frozenset(japanese_union_operations), frozenset(japanese_union_options)
            ),
            expected,
        )


def check_generated_man(source: Path, generated: Path, version: str) -> None:
    expected = read_text(source).replace("@VERSION@", version)
    actual = read_text(generated)
    if expected != actual:
        fail(
            f"{generated.relative_to(REPOSITORY_ROOT)} differs from "
            f"{source.relative_to(REPOSITORY_ROOT)} with @VERSION@={version}"
        )


def check_generated_completions(schema) -> None:
    descriptions = load_descriptions(schema, "en")
    outputs = generated_files(
        schema, descriptions, "en", REPOSITORY_ROOT / "completions"
    )
    stale = [
        path.relative_to(REPOSITORY_ROOT)
        for path, expected in outputs.items()
        if read_text(path) != expected
    ]
    if stale:
        fail(
            "tracked completion differs from its generator output: "
            + ", ".join(str(path) for path in stale)
        )


def assert_canonical_syntax_present(
    label: str, text: str, canonical_grammar: tuple[str, ...]
) -> None:
    normalized = text.replace(r"\-", "-")
    missing = [syntax for syntax in canonical_grammar if syntax not in normalized]
    if missing:
        fail(f"{label} is missing canonical syntax: {', '.join(missing)}")


def assert_document_contract(
    path: Path,
    required_fragments: tuple[str, ...],
    forbidden_fragments: tuple[str, ...] = (),
) -> None:
    text = " ".join(read_text(path).split())
    compact_text = text.replace(" ", "")
    missing = [
        fragment
        for fragment in required_fragments
        if fragment not in text and fragment.replace(" ", "") not in compact_text
    ]
    forbidden = [fragment for fragment in forbidden_fragments if fragment in text]
    if missing:
        fail(
            f"{shown_path(path)} is missing required contract text: "
            + ", ".join(repr(fragment) for fragment in missing)
        )
    if forbidden:
        fail(
            f"{shown_path(path)} retains obsolete contract text: "
            + ", ".join(repr(fragment) for fragment in forbidden)
        )


def check_reviewed_source_completion_semantics(repository_root: Path) -> None:
    path = repository_root / "completions/descriptions/en.json"
    try:
        document = json.loads(read_text(path))
    except json.JSONDecodeError as error:
        fail(f"invalid JSON in {shown_path(path)}: {error}")
    if not isinstance(document, dict) or not isinstance(
        document.get("options"), dict
    ):
        fail(f"{shown_path(path)} has no option-description object")

    options = document["options"]
    runtime_contracts = reviewed_source_runtime_help_contracts("en")
    for token in ("--diff", "--nodiff"):
        contract = runtime_contracts[token]
        if token == "--diff":
            contract = SemanticTextContract(
                contract.required_patterns[:-1],
                contract.forbidden_patterns,
            )
        description = options.get(token)
        if not isinstance(description, str):
            fail(f"{shown_path(path)} has no description for {token}")
        assert_semantic_text_contract(
            f"{shown_path(path)} description for {token}",
            description,
            contract,
        )


def check_package_relation_documentation() -> None:
    obsolete = (
        "DeclaredMetadataActualRelationUnassessed",
        "actual relation: unassessed (#353)",
        "conflicts/replacements that Moguet cannot resolve",
        "安全に解決できないconflicts / replacesがあるplan",
    )
    contracts = {
        REPOSITORY_ROOT / "README.md": (
            "AUR `Conflicts` and `Replaces` declarations are assessed before build and install",
            "including provided components and versioned relations",
            "potential impact that requires review",
            "does not remove a package, select a replacement target, or resolve a conflict automatically",
            "unavailable (`Unknown`) or invalid relation assessment fails closed",
            "complete observation that confirms no matching current or planned package or provided component",
            "transaction authority",
            "never authorizes automatic removal or replacement",
        ),
        REPOSITORY_ROOT / "README.ja.md": (
            "AURの`Conflicts` / `Replaces`宣言",
            "provided componentとversion付きrelation",
            "reviewが必要なpotential impact",
            "packageの削除、replacement targetの選択、conflict解決を自動実行しません",
            "relation assessmentが利用不能（`Unknown`）またはinvalidならfail-closed",
            "current / planned packageまたはprovided componentに一致がないと確認できた場合だけ",
            "transaction authorityはpacman / libalpm",
            "自動削除や自動置換を許可しません",
        ),
        REPOSITORY_ROOT / "docs/COMPATIBILITY.md": (
            "metadata observation、typed classification、pre-transaction diagnostic、safety stop",
            "automatic package removal、automatic replacement、automatic conflict resolution",
            "full dependency / conflict solverの置換",
            "libalpm transaction prepare / commit",
            "replacement matchはautomatic replacementの予告や許可ではない",
            "`Unknown`とinvalid result",
            "completeな観測がpackageとprovided componentのいずれにも一致しない",
            "dry-run / unified planは同じblocking truthを`Blocked`とnon-zero statusへ投影する",
        ),
        REPOSITORY_ROOT / "man/moguet.1.in": (
            "including provided components and versioned relations",
            "Moguet does not remove, replace, or resolve packages automatically",
            "Unavailable or invalid judgments fail closed and are not reported as absence",
            "complete observation with no matching current or planned package or provided component",
            "Confirmed installed or planned conflicts",
            "potential replacement impacts can leave plan completeness Complete",
            "build and install require review and remain blocked by the safety guard",
            "Moguet does not resolve them automatically",
            "unavailable or not-yet-completed relation judgment",
            "completeness Unknown and fails closed",
            "Invalid relation metadata or observation",
            "completeness Incomplete and blocks build and install",
            "complete observation with no matching current or planned target",
            "adds no relation blocker",
            "pacman/libalpm remains the transaction authority",
        ),
        REPOSITORY_ROOT / "man/ja/moguet.1.in": (
            "provided componentとversion付きrelation",
            "packageの削除・置換・conflict解決を自動実行しません",
            "利用不能またはinvalidなjudgmentはfail-closedとし、absenceとして表示しません",
            "current / planned packageとprovided componentのいずれにも一致しない場合だけ",
            "確認済みのinstalled / planned conflict",
            "potential replacement impactがあっても",
            "plan completenessはCompleteのままになり得ます",
            "build / installは確認が必要で、safety guardにより停止します",
            "Moguetはこれらを自動解決しません",
            "relation judgmentが利用不能または未完了",
            "completenessはUnknownとなり、fail-closed",
            "relation metadataまたはobservationがinvalid",
            "completenessはIncompleteとなり、build / installをblock",
            "complete observationでcurrent / planned targetにmatchがなければ",
            "relation blockerはありません",
            "transaction authorityはpacman / libalpm",
        ),
    }
    for path, required in contracts.items():
        assert_document_contract(path, required, obsolete)


def reviewed_source_documentation_contracts(
    repository_root: Path,
) -> dict[Path, tuple[tuple[str, ...], tuple[str, ...]]]:
    obsolete_help = (
        "Prompt to review {} and {} files",
        "Skip {} and {} review",
        "Prompt to review PKGBUILD and .install files",
        "Skip PKGBUILD and .install review",
        "Prompt to view repository update diffs",
        "Skip the repository update diff prompt",
        "{} review policy",
        "Repository update diff policy",
        "Review changes from the previous reviewed revision to the exact target",
        "Skip reviewed source changes without advancing reviewed state",
    )
    return {
        repository_root / "README.md": (
            (
                "last explicitly accepted exact upstream commit for each PackageBase",
                "existing cache created before this workflow",
                "full tracked-file review",
                "previous reviewed revision",
                "only an explicit interactive `y` or `yes`",
                "do not advance reviewed state",
                "compare-and-swap guard",
                "separate overlay on the reviewed commit",
                "Official-repository and `build --local` routes do not create this state",
                "No manual migration is required",
                "never invents a reviewed revision from the legacy checkout HEAD",
            ),
            (),
        ),
        repository_root / "README.ja.md": (
            (
                "最後に明示acceptしたexact upstream commitをPackageBaseごと",
                "このworkflowより前から存在するcache",
                "tracked file全体をfull review",
                "previous reviewed revision",
                "interactive `y` / `yes`を明示入力した場合だけ",
                "reviewed stateは進めません",
                "compare-and-swap guard",
                "reviewed commit上の別overlay",
                "official repositoryと`build --local` routeはこのstateを作りません",
                "手動migrationは不要",
                "legacy checkout HEAD、branch、remote ref、build artifactからreviewed revisionを捏造しません",
            ),
            (),
        ),
        repository_root / "docs/COMPATIBILITY.md": (
            (
                "previous reviewed revisionからexact targetまで",
                "AUR Git treeのtracked file全体",
                "defaultなしのinteractive promptへ明示入力した`y` / `yes`だけ",
                "compatibility buildを継続し得る場合もstateを進めない",
                "CAS semantics",
                "後続build / install / cleanup failureでrollbackしない",
                "invocation-localなPKGBUILD / detected top-level `*.install` editor policy",
                "manual migrationは不要",
                "legacy checkout HEAD、branch、remote ref、artifactからreviewed revisionを捏造しない",
                "official repository source-buildとlocal PKGBUILD routeはreviewed-source stateをread / writeしない",
                "generic source identity projectionとreviewed-source persistent / build authorityは同じものではない",
            ),
            (),
        ),
        repository_root / "docs/contracts/README.md": (
            (
                "[Reviewed AUR source state](reviewed-source-state.md)",
                "PackageBase単位のexact reviewed revision",
            ),
            (),
        ),
        repository_root / "docs/contracts/reviewed-source-state.md": (
            (
                "Scopeとauthority",
                "stateの単位はPackageBase",
                "InitialFullReview",
                "already reviewed",
                "update review",
                "full rebaseline review",
                "full rebind / rebaseline review",
                "unsupported future schema、unsafe history",
                "review eligibilityのauthorityはAUR Git treeのtracked file全体",
                "root `PKGBUILD`またはtop-level `*.install`",
                "reviewのmaterialize / 表示成功とacceptanceは別event",
                "CAS semantics",
                "exact targetへdetached checkout",
                "invocation-local overlay",
                "no reviewed state -> Missing -> InitialFullReview",
                "userによるstate file作成、cache変換、手動 migrationは不要",
                "official repository source-buildと`build --local`",
                "generic source identity projection != reviewed-source persistent/build authority",
                "generic projectionを`Known`へ昇格させない",
            ),
            (),
        ),
        repository_root / "docs/contracts/source-package-identity.md": (
            (
                "Issue #411のreviewed-source lifecycle",
                "generic projection inputの拡張ではない",
                "common projectionの`Unknown`を`Known`へ昇格させたりしない",
                "exact target OIDを保持していても、このgeneric projection ruleは変わらない",
                "reviewed-source exact OIDの注入をgeneric projectionへ追加しない",
            ),
            (),
        ),
        repository_root / "man/moguet.1.in": (
            (
                "previous reviewed revision to the exact fetched target",
                "Only explicit acceptance after a complete review advances reviewed state",
                "does not advance reviewed state",
                "invocation-local editor policy, not upstream reviewed-source acceptance",
                "Persistent PackageBase-scoped reviewed AUR revision state",
                "compare-and-swap semantics",
                "does not roll back a correctly accepted and published revision",
                "requires no manual migration",
                "does not invent a reviewed revision from legacy checkout HEAD",
            ),
            (),
        ),
        repository_root / "man/ja/moguet.1.in": (
            (
                "previous reviewed revisionからexact fetched targetまで",
                "explicit acceptanceだけがreviewed stateを進めます",
                "reviewed stateを進めません",
                "invocation-localなeditor policyであり、upstream reviewed-source acceptanceではなく",
                "PackageBase単位のpersistent reviewed AUR revision state",
                "compare-and-swap semantics",
                "正常にaccept / publishしたrevisionをrollbackしません",
                "manual migrationは不要",
                "legacy checkout HEAD、branch、remote ref、build artifactからreviewed revisionを捏造しません",
            ),
            (),
        ),
        repository_root / "source/moguet.cpp": (
            (
                r"review.pkgbuild = \"prompt\"|\"skip\"",
                r"review.diff = \"prompt\"|\"skip\"",
            ),
            (
                *obsolete_help,
                *OBSOLETE_UNQUOTED_USER_CONFIG_HELP_SOURCE_LITERALS,
            ),
        ),
        repository_root / "completions/descriptions/en.json": (
            (),
            obsolete_help,
        ),
    }


def check_reviewed_source_documentation(
    repository_root: Path = REPOSITORY_ROOT,
) -> None:
    for path, (required, forbidden) in reviewed_source_documentation_contracts(
        repository_root
    ).items():
        assert_document_contract(path, required, forbidden)
    check_reviewed_source_completion_semantics(repository_root)


def system_aur_update_documentation_contracts(
    repository_root: Path,
) -> dict[Path, tuple[tuple[str, ...], tuple[str, ...]]]:
    return {
        repository_root / "README.md": (
            (
                "ordinary AUR-helper update",
                "It neither enumerates nor reads saved source-build preferences",
                "Initially, `--needed` is the only pacman semantic option",
                "fails before repository mutation",
                "`moguet -Syu --repo`",
                "does not affect this route",
                "reported as a non-zero partial outcome",
            ),
            (),
        ),
        repository_root / "README.ja.md": (
            (
                "通常のAUR helper update",
                "saved source-build preferenceを列挙・readせず",
                "initially対応するpacman semantic optionは`--needed`だけ",
                "repository mutation前に失敗",
                "`moguet -Syu --repo`",
                "このrouteへ影響しません",
                "non-zeroのpartial outcome",
            ),
            (),
        ),
        repository_root / "docs/COMPATIBILITY.md": (
            (
                "Exact target-less `-Syu` compatibility",
                "fresh installed foreign inventory / exact AUR metadata",
                "typed `NotAttempted`",
                "AUR `NoUpdates`だけを根拠にoperation全体を`NoOp`と呼ばない",
                "PackageBaseへのfallback read",
                "source-awareなStrict reader",
                "initially対応するpacman semantic optionは`--needed`だけ",
                "`moguet -Syu --repo`",
            ),
            (),
        ),
        repository_root / "man/moguet.1.in": (
            (
                "The exact target-less Auto form is the ordinary AUR-helper update",
                "does not snapshot, enumerate, read, or apply saved source-build preferences",
                "sequential, not atomic",
                "non-zero partial failure without rollback",
                "only initially supported pacman semantic option",
                "moguet -Syu --repo",
                "-Syu --aur",
            ),
            (),
        ),
        repository_root / "man/ja/moguet.1.in": (
            (
                "exact target-less Auto formは通常のAUR helper update",
                "saved source-build preference directoryのsnapshot、列挙、read、適用を行わず",
                "phaseはsequentialでatomicではありません",
                "non-zeroのpartial failure",
                "initially対応するpacman semantic option",
                "moguet -Syu --repo",
                "-Syu --aur",
            ),
            (),
        ),
        repository_root / "source/moguet.cpp": (
            (
                "Upgrade official repository packages and normal installed {} packages",
                "Do not read or apply saved source-build preferences",
                "Run repository and {} phases sequentially",
                "Run the repository system upgrade only",
                "For {}, assess the current installed {} state",
            ),
            ("Remain compatible with {}; do not scan all source-build preferences",),
        ),
        repository_root / "completions/descriptions/en.json": (
            (
                '"-Syu": "Update repository packages and normal installed AUR packages without saved source-build preferences"',
                '"--repo": "Use only official binary repositories; with -Syu, run the repository system upgrade only"',
            ),
            ('"-Syu": "Upgrade the system"',),
        ),
    }


def check_system_aur_update_documentation(
    repository_root: Path = REPOSITORY_ROOT,
) -> None:
    for path, (required, forbidden) in system_aur_update_documentation_contracts(
        repository_root
    ).items():
        assert_document_contract(path, required, forbidden)


def release_notes_section(text: str, heading: str) -> str:
    lines = text.splitlines()
    if lines.count(heading) != 1:
        fail(f"RELEASE_NOTES.md must contain exactly one {heading!r}")
    start = lines.index(heading) + 1
    end = next(
        (index for index in range(start, len(lines)) if lines[index].startswith("# ")),
        len(lines),
    )
    return "\n".join(lines[start:end])


def check_release_notes_documentation(
    repository_root: Path = REPOSITORY_ROOT,
) -> None:
    version = read_current_project_version(repository_root)
    notes = read_text(repository_root / "RELEASE_NOTES.md")
    current = release_notes_section(notes, f"# Moguet v{version}")
    for heading in ("## English", "## 日本語"):
        if current.splitlines().count(heading) != 1:
            fail(f"current release notes must contain exactly one {heading!r}")

    # This is the release that introduced the compatibility boundary, not
    # another authority for the current project version.
    historical = release_notes_section(notes, "# Moguet v2.6.0")
    normalized = " ".join(historical.split())
    compact = normalized.replace(" ", "")
    for fragment in (
        "Moguet v2.6.0 includes a behavior-changing compatibility correction",
        "Before v2.6.0",
        "Starting with v2.6.0",
        "does not roll back the completed repository transaction",
        "moguet -Syu --repo",
        "saved source-build preferences strictly",
        "Moguet v2.6.0では",
        "behavior-changingな compatibility correction",
        "v2.6.0より前",
        "v2.6.0以降",
    ):
        if fragment not in normalized and fragment.replace(" ", "") not in compact:
            fail(f"v2.6.0 release notes are missing historical contract text: {fragment!r}")


def markdown_canonical_grammar(path: Path) -> tuple[str, ...]:
    text = read_text(path)
    begin_marker = "<!-- CLI CANONICAL GRAMMAR BEGIN -->"
    end_marker = "<!-- CLI CANONICAL GRAMMAR END -->"
    if text.count(begin_marker) != 1 or text.count(end_marker) != 1:
        fail(
            f"{path.relative_to(REPOSITORY_ROOT)} must contain exactly one "
            "CLI canonical grammar marker pair"
        )
    begin = text.index(begin_marker) + len(begin_marker)
    end = text.index(end_marker, begin)
    region = text[begin:end]
    lines = [line.strip() for line in region.splitlines()]
    try:
        fence_start = lines.index("```text")
        fence_end = lines.index("```", fence_start + 1)
    except ValueError:
        fail(
            f"{path.relative_to(REPOSITORY_ROOT)} canonical grammar must use "
            "one ```text block"
        )
    return tuple(line for line in lines[fence_start + 1 : fence_end] if line)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check bilingual public-document and CLI schema parity."
    )
    parser.add_argument("--help-en", type=Path, required=True)
    parser.add_argument("--help-ja", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    schema = load_schema()
    expected = expected_surface(schema)

    english_help = help_surface(arguments.help_en, expected)
    japanese_help = help_surface(arguments.help_ja, expected)
    assert_surface("English runtime help", english_help, expected)
    assert_surface("Japanese runtime help", japanese_help, expected)
    if english_help != japanese_help:
        fail("English and Japanese runtime help token sets differ")
    assert_canonical_syntax_present(
        "English runtime help",
        read_text(arguments.help_en),
        schema.canonical_grammar,
    )
    assert_canonical_syntax_present(
        "Japanese runtime help",
        read_text(arguments.help_ja),
        schema.canonical_grammar,
    )
    check_reviewed_source_runtime_help(
        read_text(arguments.help_en),
        read_text(arguments.help_ja),
    )
    check_system_aur_update_runtime_help(
        read_text(arguments.help_en),
        read_text(arguments.help_ja),
    )

    version = read_current_project_version(REPOSITORY_ROOT)
    check_generated_man(
        REPOSITORY_ROOT / "man/moguet.1.in",
        REPOSITORY_ROOT / "man/moguet.1",
        version,
    )
    check_generated_man(
        REPOSITORY_ROOT / "man/ja/moguet.1.in",
        REPOSITORY_ROOT / "man/ja/moguet.1",
        version,
    )
    compare_marked_documents(
        "man page",
        REPOSITORY_ROOT / "man/moguet.1.in",
        REPOSITORY_ROOT / "man/ja/moguet.1.in",
        MAN_MARKER,
        "man",
        expected,
        MAN_SECTION_SLUGS,
    )
    english_man_surface = exact_man_public_surface(
        REPOSITORY_ROOT / "man/moguet.1.in", expected, schema
    )
    japanese_man_surface = exact_man_public_surface(
        REPOSITORY_ROOT / "man/ja/moguet.1.in", expected, schema
    )
    if english_man_surface != japanese_man_surface:
        fail("English and Japanese man PUBLIC token sets differ")
    assert_canonical_syntax_present(
        "English man page",
        man_public_region(
            read_text(REPOSITORY_ROOT / "man/moguet.1.in"),
            "COMMANDS",
            REPOSITORY_ROOT / "man/moguet.1.in",
        ),
        schema.canonical_grammar,
    )
    assert_canonical_syntax_present(
        "Japanese man page",
        man_public_region(
            read_text(REPOSITORY_ROOT / "man/ja/moguet.1.in"),
            "COMMANDS",
            REPOSITORY_ROOT / "man/ja/moguet.1.in",
        ),
        schema.canonical_grammar,
    )

    compare_marked_documents(
        "README",
        REPOSITORY_ROOT / "README.md",
        REPOSITORY_ROOT / "README.ja.md",
        MARKDOWN_MARKER,
        "markdown",
        expected,
        README_SECTION_SLUGS,
    )
    compare_marked_documents(
        "migration guide",
        REPOSITORY_ROOT / "docs/migration/v1-to-v2.md",
        REPOSITORY_ROOT / "docs/migration/v1-to-v2.ja.md",
        MARKDOWN_MARKER,
        "markdown",
        expected,
        MIGRATION_SECTION_SLUGS,
    )

    for path in (
        REPOSITORY_ROOT / "README.md",
        REPOSITORY_ROOT / "README.ja.md",
        REPOSITORY_ROOT / "docs/COMPATIBILITY.md",
    ):
        documented = markdown_canonical_grammar(path)
        if documented != schema.canonical_grammar:
            fail(
                f"{path.relative_to(REPOSITORY_ROOT)} canonical grammar differs "
                "from the structured CLI authority: "
                f"documented={documented}, expected={schema.canonical_grammar}"
            )

    check_package_relation_documentation()
    check_reviewed_source_documentation()
    check_system_aur_update_documentation()
    check_release_notes_documentation()
    check_generated_completions(schema)
    print("public-documentation-check: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
