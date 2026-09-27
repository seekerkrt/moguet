#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
import difflib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Callable


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CLI_AUTHORITY_EXPORTER = (
    REPOSITORY_ROOT / "build/cmake-testing/moguet-cli-authority-exporter"
)
DESCRIPTION_ROOT = REPOSITORY_ROOT / "completions/descriptions"

# The exporter owns the C++ authority projection.  The loader does not trust
# wire values that it does not understand: adding an enum value in C++ also
# requires an explicit completion projection before rendering may continue.
KNOWN_OPERAND_KINDS = frozenset(
    {
        "none",
        "package",
        "directory",
        "patch-directory",
        "patch-file",
        "package-base",
        "query",
        "source-preference-item",
        "environment-assignment",
        "delegated-pacman-argument",
    }
)
KNOWN_OPERAND_ORDERINGS = frozenset(
    {
        "none",
        "preserve-input-order",
        "primary-then-environment-assignments",
        "package-introduces-following-assignment-scope",
        "delegated",
    }
)
KNOWN_TARGET_POLICIES = frozenset(
    {"none", "exactly-one", "one-or-more", "ordered-items", "delegated", "fixed-sequence"}
)
KNOWN_OPTION_OCCURRENCES = frozenset(
    {"once", "repeat-idempotent", "repeat-same-value", "delegated"}
)
KNOWN_OPTION_PLACEMENTS = frozenset(
    {
        "parser-global",
        "first-non-global",
        "operation-local",
        "pacman-grammar",
        "end-of-options",
    }
)
KNOWN_OPTION_VALUE_KINDS = frozenset(
    {"none", "attached-enum", "attached-value", "marker"}
)
KNOWN_OPTION_CONFLICT_RULES = frozenset(
    {"none", "mutually-exclusive", "operation-local-exclusion", "final-value-must-agree"}
)
KNOWN_OPTION_SEMANTIC_SCOPES = frozenset(
    {
        "information",
        "source-build-review",
        "source-checkout-review",
        "source-build",
        "dry-run-routing",
        "root-package-selection",
        "source-selection",
        "local-source-build",
        "dependency-inspection",
        "final-package-install",
        "pacman-delegation",
        "parser-boundary",
        "dependency-cleanup",
        "package-export",
        "presentation-detail",
    }
)
KNOWN_GRAMMAR_OWNERSHIPS = frozenset(
    {"moguet-owned", "intercepted-pacman", "delegated-pacman"}
)
KNOWN_OPTION_DEFINITION_ROLES = frozenset(
    {"definition", "syntax-only", "schema-only"}
)
KNOWN_OPTION_COMPLETION_VISIBILITIES = frozenset(
    {"suggested-and-described", "hidden"}
)
KNOWN_RELATION_REQUIREMENTS = frozenset({"optional", "required"})
KNOWN_PUBLIC_SYNTAX = frozenset({"hidden", "optional", "required"})
KNOWN_SEMANTIC_EFFECTS = frozenset(
    {
        "none",
        "moguet-control",
        "upstream-argument",
        "final-install-semantic",
        "parser-boundary",
    }
)
KNOWN_FORWARDING_TARGETS = frozenset(
    {"none", "pacman", "makepkg", "final-install-pacman"}
)
KNOWN_FORWARDING_OCCURRENCES = frozenset(
    {"none", "preserve-all", "consolidate-single"}
)
KNOWN_DELEGATED_TAIL_POLICIES = frozenset({"none", "repository-only"})
PRIMARY_OPERAND_KINDS = frozenset({"package", "directory", "query"})
ASSIGNMENT_PRIMARY_OPERAND_KINDS = frozenset({"package", "directory"})
CANONICAL_GRAMMAR_ATOM_BOUNDARIES = frozenset("[](){}|,")
CANONICAL_OPTION_TOKEN_PATTERN = re.compile(
    r"--(?:[A-Za-z0-9][A-Za-z0-9_-]*)?"
    r"|-[A-Za-z0-9][A-Za-z0-9_-]*"
)


@dataclass(frozen=True)
class Option:
    identity: int
    token: str
    completion_token: str
    occurrence: str
    placement: str
    value_kind: str
    allowed_values: tuple[str, ...]
    conflict_rule: str
    conflicts: tuple[int, ...]
    conflict_value_identity: str
    semantic_scopes: tuple[str, ...]
    ownership: str
    definition_role: str
    completion_visibility: str
    fixed_value: str = ""

    @property
    def is_completion_visible(self) -> bool:
        return self.completion_visibility == "suggested-and-described"


@dataclass(frozen=True)
class OperandTerm:
    kind: str
    min_count: int
    max_count: int | None


@dataclass(frozen=True)
class OptionRelation:
    identity: int
    requirement: str
    occurrence: str
    public_syntax: str
    semantic_effects: tuple[str, ...]
    forwarding_targets: tuple[str, ...]
    forwarding_occurrence: str


@dataclass(frozen=True)
class Form:
    syntax: str
    target_policy: str
    operand_ordering: str
    operand_terms: tuple[OperandTerm, ...]
    option_relations: tuple[OptionRelation, ...]
    delegated_tail_policy: str = "none"

    @property
    def option_ids(self) -> tuple[int, ...]:
        return tuple(relation.identity for relation in self.option_relations)

    @property
    def selector_ids(self) -> tuple[int, ...]:
        return tuple(
            relation.identity
            for relation in self.option_relations
            if relation.public_syntax == "required"
        )


@dataclass(frozen=True)
class Operation:
    token: str
    forms: tuple[Form, ...] = ()
    open_grammar: bool = False


@dataclass(frozen=True)
class CliSchema:
    operations: tuple[Operation, ...]
    options: tuple[Option, ...]
    delegated_form: Form | None
    terminal_tokens: tuple[str, ...]
    canonical_grammar: tuple[str, ...]
    presentation_scopes: tuple[tuple[str, int, int | None], ...] = ()
    operand_contexts: tuple[tuple[str, str], ...] = ()
    lexical_value_options: tuple[tuple[str, bool], ...] = ()
    parser_boundaries: tuple[tuple[str, int], ...] = ()

    @property
    def delegated_option_ids(self) -> tuple[int, ...]:
        if self.delegated_form is None:
            return ()
        return self.delegated_form.option_ids


@dataclass(frozen=True)
class Descriptions:
    operations: dict[str, str]
    options: dict[str, str]


def fail(message: str) -> None:
    print(f"completion-generator: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_identity(value: str, context: str) -> int:
    try:
        identity = int(value)
    except ValueError:
        fail(f"invalid {context} identity: {value!r}")
    if identity < 0:
        fail(f"invalid {context} identity: {value!r}")
    return identity


def parse_comma_list(value: str, context: str) -> tuple[str, ...]:
    if not value:
        return ()
    items = tuple(value.split(","))
    if any(not item for item in items):
        fail(f"empty {context} list element: {value!r}")
    return items


def parse_id_list(value: str) -> tuple[int, ...]:
    return tuple(
        parse_identity(item, "option")
        for item in parse_comma_list(value, "option conflict")
    )


def parse_name_set(
    value: str, known_values: frozenset[str], context: str
) -> tuple[str, ...]:
    names = tuple(value.split("+")) if value else ()
    if not names or any(name not in known_values for name in names):
        fail(f"unsupported {context} projection: {value!r}")
    if len(set(names)) != len(names):
        fail(f"duplicate {context} projection: {value!r}")
    if "none" in names and len(names) != 1:
        fail(f"inconsistent {context} projection: {value!r}")
    return names


def parse_option_relations(value: str) -> tuple[OptionRelation, ...]:
    relations: list[OptionRelation] = []
    for encoded in parse_comma_list(value, "option relation"):
        fields = encoded.split(":")
        if len(fields) != 7:
            fail(f"invalid option relation: {encoded!r}")
        requirement, occurrence, public_syntax = fields[1:4]
        if requirement not in KNOWN_RELATION_REQUIREMENTS:
            fail(f"unsupported option requirement projection: {requirement!r}")
        if occurrence not in KNOWN_OPTION_OCCURRENCES:
            fail(f"unsupported relation occurrence projection: {occurrence!r}")
        if public_syntax not in KNOWN_PUBLIC_SYNTAX:
            fail(f"unsupported public syntax projection: {public_syntax!r}")
        forwarding_occurrence = fields[6]
        if forwarding_occurrence not in KNOWN_FORWARDING_OCCURRENCES:
            fail(
                "unsupported forwarding occurrence projection: "
                f"{forwarding_occurrence!r}"
            )
        relation = OptionRelation(
            identity=parse_identity(fields[0], "relation option"),
            requirement=requirement,
            occurrence=occurrence,
            public_syntax=public_syntax,
            semantic_effects=parse_name_set(
                fields[4], KNOWN_SEMANTIC_EFFECTS, "semantic effect"
            ),
            forwarding_targets=parse_name_set(
                fields[5], KNOWN_FORWARDING_TARGETS, "forwarding target"
            ),
            forwarding_occurrence=forwarding_occurrence,
        )
        relations.append(relation)
    return tuple(relations)


def parse_operand_terms(value: str) -> tuple[OperandTerm, ...]:
    terms: list[OperandTerm] = []
    for encoded in parse_comma_list(value, "operand term"):
        fields = encoded.split(":")
        if len(fields) != 3:
            fail(f"invalid operand term: {encoded!r}")
        if fields[0] not in KNOWN_OPERAND_KINDS:
            fail(f"unsupported operand kind projection: {fields[0]!r}")
        try:
            min_count = int(fields[1])
            max_count = None if fields[2] == "*" else int(fields[2])
        except ValueError:
            fail(f"invalid operand cardinality: {encoded!r}")
        if min_count < 0 or (max_count is not None and max_count < min_count):
            fail(f"invalid operand cardinality range: {encoded!r}")
        terms.append(OperandTerm(fields[0], min_count, max_count))
    return tuple(terms)


def export_authority() -> str:
    executable = Path(
        os.environ.get(
            "MOGUET_CLI_AUTHORITY_EXPORTER",
            str(DEFAULT_CLI_AUTHORITY_EXPORTER),
        )
    )
    if not executable.is_file():
        fail(
            "CMake-built CLI authority exporter is unavailable: "
            f"{executable}; run 'make cmake-cli-authority-exporter-build'"
        )

    export_result = subprocess.run(
        [str(executable)],
        cwd=REPOSITORY_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if export_result.returncode != 0:
        fail(
            "CLI authority projection failed:\n"
            + export_result.stderr.rstrip()
        )
    return export_result.stdout


def parse_exported_schema(exported_schema: str) -> CliSchema:
    """Parse and validate the exporter wire schema before any renderer uses it."""

    options: list[Option] = []
    operations: dict[str, Operation] = {}
    operation_modes: set[tuple[str, str]] = set()
    delegated_form: Form | None = None
    terminal_tokens: list[str] = []
    canonical_grammar: list[str] = []
    presentation_scopes: list[tuple[str, int, int | None]] = []

    operand_contexts: dict[str, str] = {}
    lexical_value_options: dict[str, bool] = {}
    parser_boundaries: dict[str, int] = {}
    fixed_values: dict[int, str] = {}

    for line in exported_schema.splitlines():
        fields = line.split("\t")
        record = fields[0]
        if record == "OPTION" and len(fields) == 15:
            occurrence = fields[4]
            placement = fields[5]
            value_kind = fields[6]
            conflict_rule = fields[8]
            ownership = fields[12]
            definition_role = fields[13]
            completion_visibility = fields[14]
            if occurrence not in KNOWN_OPTION_OCCURRENCES:
                fail(f"unsupported option occurrence projection: {occurrence!r}")
            if placement not in KNOWN_OPTION_PLACEMENTS:
                fail(f"unsupported option placement projection: {placement!r}")
            if value_kind not in KNOWN_OPTION_VALUE_KINDS:
                fail(f"unsupported option value projection: {value_kind!r}")
            if conflict_rule not in KNOWN_OPTION_CONFLICT_RULES:
                fail(f"unsupported option conflict projection: {conflict_rule!r}")
            if ownership not in KNOWN_GRAMMAR_OWNERSHIPS:
                fail(f"unsupported option ownership projection: {ownership!r}")
            if definition_role not in KNOWN_OPTION_DEFINITION_ROLES:
                fail(
                    "unsupported option definition role projection: "
                    f"{definition_role!r}"
                )
            if completion_visibility not in KNOWN_OPTION_COMPLETION_VISIBILITIES:
                fail(
                    "unsupported option completion visibility projection: "
                    f"{completion_visibility!r}"
                )
            options.append(
                Option(
                    identity=parse_identity(fields[1], "option"),
                    token=fields[2],
                    completion_token=fields[3],
                    occurrence=occurrence,
                    placement=placement,
                    value_kind=value_kind,
                    allowed_values=parse_comma_list(
                        fields[7], "allowed value"
                    ),
                    conflict_rule=conflict_rule,
                    conflicts=parse_id_list(fields[9]),
                    conflict_value_identity=fields[10],
                    semantic_scopes=parse_name_set(
                        fields[11],
                        KNOWN_OPTION_SEMANTIC_SCOPES,
                        "option semantic scope",
                    ),
                    ownership=ownership,
                    definition_role=definition_role,
                    completion_visibility=completion_visibility,
                )
            )
        elif record == "OPERATION" and len(fields) == 3:
            token = fields[1]
            mode = fields[2]
            if not token or mode not in {"closed", "open"}:
                fail(f"invalid operation projection: {line!r}")
            if (token, mode) in operation_modes:
                fail(f"duplicate operation projection: {token!r} {mode!r}")
            operation_modes.add((token, mode))
            existing = operations.get(token, Operation(token))
            operations[token] = replace(
                existing,
                open_grammar=existing.open_grammar or mode == "open",
            )
        elif record == "FORM" and len(fields) == 8:
            token = fields[1]
            if (token, "closed") not in operation_modes:
                fail(f"grammar form has no closed operation projection: {token!r}")
            existing = operations.get(token, Operation(token))
            form = Form(
                syntax=fields[2],
                target_policy=fields[3],
                operand_ordering=fields[4],
                operand_terms=parse_operand_terms(fields[5]),
                option_relations=parse_option_relations(fields[6]),
                delegated_tail_policy=fields[7],
            )
            if form.delegated_tail_policy not in KNOWN_DELEGATED_TAIL_POLICIES:
                fail(
                    "unsupported delegated tail policy projection: "
                    f"{form.delegated_tail_policy!r}"
                )
            operations[token] = replace(existing, forms=existing.forms + (form,))
        elif record == "DELEGATED" and len(fields) == 5:
            if delegated_form is not None:
                fail("CLI authority exporter returned duplicate delegated grammar")
            delegated_form = Form(
                syntax="<delegated-pacman-grammar>",
                target_policy=fields[1],
                operand_ordering=fields[2],
                operand_terms=parse_operand_terms(fields[3]),
                option_relations=parse_option_relations(fields[4]),
                delegated_tail_policy="none",
            )
        elif record == "PRESENTATION" and len(fields) == 4:
            presentation_scopes.append((fields[1], parse_identity(fields[2], "presentation option"),
                                        parse_identity(fields[3], "required option") if fields[3] else None))
        elif record == "OPERAND_CONTEXT" and len(fields) == 3:
            token, kind = fields[1:]
            if token in operand_contexts or kind not in KNOWN_OPERAND_KINDS:
                fail("invalid exact operand context")
            operand_contexts[token] = kind
        elif record == "LEXICAL_VALUE" and len(fields) == 3:
            token, database_context = fields[1:]
            if (token in lexical_value_options or token == "--" or
                    CANONICAL_OPTION_TOKEN_PATTERN.fullmatch(token) is None or
                    database_context not in {"alternate-db", "default-db"}):
                fail("invalid lexical value option")
            lexical_value_options[token] = database_context == "alternate-db"
        elif record == "BOUNDARY" and len(fields) == 3:
            if fields[1] in parser_boundaries:
                fail("duplicate parser boundary")
            parser_boundaries[fields[1]] = parse_identity(fields[2], "boundary option")
        elif record == "FIXED_VALUE" and len(fields) == 3:
            identity = parse_identity(fields[1], "fixed value option")
            if identity in fixed_values or not fields[2]:
                fail("invalid fixed value binding")
            fixed_values[identity] = fields[2]
        elif record == "TERMINAL" and len(fields) == 2:
            terminal_tokens.append(fields[1])
        elif record == "CANONICAL" and len(fields) == 2:
            canonical_grammar.append(fields[1])
        else:
            fail(f"invalid exporter record: {line!r}")

    if not options or not operations or not canonical_grammar:
        fail("CLI authority exporter returned an incomplete schema")
    if len({option.token for option in options}) != len(options):
        fail("CLI authority exporter returned duplicate option tokens")
    if any(not operation.forms and not operation.open_grammar for operation in operations.values()):
        fail("CLI authority exporter returned an operation without a grammar form")
    if not set(fixed_values).issubset(option.identity for option in options):
        fail("fixed value binding has unknown option")
    options = [replace(option, fixed_value=fixed_values.get(option.identity, ""))
               for option in options]

    schema = CliSchema(
        operations=tuple(operations.values()),
        options=tuple(options),
        delegated_form=delegated_form,
        terminal_tokens=tuple(terminal_tokens),
        canonical_grammar=tuple(canonical_grammar),
        presentation_scopes=tuple(presentation_scopes),
        operand_contexts=tuple(operand_contexts.items()),
        lexical_value_options=tuple(lexical_value_options.items()),
        parser_boundaries=tuple(parser_boundaries.items()),
    )
    for token in operand_contexts:
        if (token, "open") not in operation_modes:
            fail("exact operand context has no open operation")
    for token, identity in parser_boundaries.items():
        if not any(option.token == token and option.identity == identity and
                   option.value_kind == "marker" and not option.is_completion_visible
                   for option in options):
            fail("parser boundary must identify a hidden marker option")
    validate_schema_projection(schema)
    for operation, option_id, required in schema.presentation_scopes:
        if operation not in operations or not any(option.identity == option_id for option in options):
            fail("invalid presentation scope")
        if required is not None and not any(option.identity == required for option in options):
            fail("invalid presentation scope requirement")
    return schema


def load_schema() -> CliSchema:
    return parse_exported_schema(export_authority())


def validate_description_map(
    category: str, descriptions: object, expected_tokens: tuple[str, ...]
) -> dict[str, str]:
    if not isinstance(descriptions, dict):
        fail(f"description category '{category}' must be a JSON object")
    if not all(
        isinstance(key, str) and isinstance(value, str)
        for key, value in descriptions.items()
    ):
        fail(f"description category '{category}' must contain string keys and values")

    expected = set(expected_tokens)
    actual = set(descriptions)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing or extra:
        details = []
        if missing:
            details.append("missing: " + ", ".join(missing))
        if extra:
            details.append("extra: " + ", ".join(extra))
        fail(
            f"{category} descriptions do not match CLI authority "
            f"({'; '.join(details)})"
        )

    for token, description in descriptions.items():
        if not description or "\n" in description or "\r" in description:
            fail(f"invalid single-line description for {token!r}")
    return descriptions


def load_descriptions(schema: CliSchema, locale: str) -> Descriptions:
    if re.fullmatch(r"[a-z][a-z0-9_-]*", locale) is None:
        fail(f"invalid description locale: {locale}")
    path = DESCRIPTION_ROOT / f"{locale}.json"
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail(f"missing description authority: {path}")
    except json.JSONDecodeError as error:
        fail(f"invalid JSON in {path}: {error}")
    if not isinstance(raw, dict):
        fail(f"description authority must be a JSON object: {path}")

    return Descriptions(
        validate_description_map(
            "operations",
            raw.get("operations"),
            tuple(operation.token for operation in schema.operations),
        ),
        validate_description_map(
            "options",
            raw.get("options"),
            tuple(
                option.token
                for option in schema.options
                if option.is_completion_visible
            ),
        ),
    )


def shell_quote(value: str) -> str:
    return shlex.quote(value)


def options_for_ids(schema: CliSchema, identities: tuple[int, ...]) -> tuple[Option, ...]:
    by_identity: dict[int, list[Option]] = {}
    for option in schema.options:
        by_identity.setdefault(option.identity, []).append(option)
    return tuple(
        option
        for identity in identities
        for option in by_identity.get(identity, ())
        if option.is_completion_visible
    )


def unique_completion_tokens(options: tuple[Option, ...]) -> tuple[str, ...]:
    return tuple(dict.fromkeys(option.completion_token for option in options))


def completion_ids_for_form(schema: CliSchema, form: Form) -> tuple[int, ...]:
    # Hide route-owned suggestions; retain the existing delegated tail grammar.
    identities = tuple(
        option.identity for option in options_for_ids(schema, form.option_ids)
    )
    if form.delegated_tail_policy != "none":
        identities = identities + schema.delegated_option_ids
    return tuple(dict.fromkeys(identities))


def union_completion_form_ids(
    schema: CliSchema, forms: tuple[Form, ...]
) -> tuple[int, ...]:
    selectors = {identity for form in forms for identity in form.selector_ids}
    local_only = {option.identity for option in schema.options
                  if "local-source-build" in option.semantic_scopes and option.identity not in selectors}
    return tuple(
        dict.fromkeys(
            identity
            for form in forms
            for identity in completion_ids_for_form(schema, form)
            if identity not in local_only
        )
    )


def completion_conflicts(schema: CliSchema, option: Option) -> tuple[int, ...]:
    # A local selector can exclude a global option that already owns a
    # final-value override relation. Render the local exclusion both ways.
    return tuple(dict.fromkeys(option.conflicts + tuple(
        other.identity for other in schema.options
        if other.conflict_rule == "operation-local-exclusion" and option.identity in other.conflicts
    )))


def option_case_patterns(schema: CliSchema) -> list[tuple[int, tuple[str, ...]]]:
    grouped: dict[int, list[str]] = {}
    for option in schema.options:
        if not option.is_completion_visible:
            continue
        patterns = grouped.setdefault(option.identity, [])
        patterns.append(option.token)
        if option.completion_token.endswith("="):
            patterns.append(option.token + "=*")
    return [(identity, tuple(patterns)) for identity, patterns in grouped.items()]


def canonical_syntax_option_tokens(syntax: str) -> frozenset[str]:
    tokens: set[str] = set()
    metavariable_depth = 0
    index = 0

    while index < len(syntax):
        character = syntax[index]
        if character == "<":
            metavariable_depth += 1
            index += 1
            continue
        if character == ">" and metavariable_depth:
            metavariable_depth -= 1
            index += 1
            continue
        if metavariable_depth:
            index += 1
            continue

        if (
            index
            and not syntax[index - 1].isspace()
            and syntax[index - 1] not in CANONICAL_GRAMMAR_ATOM_BOUNDARIES
        ):
            index += 1
            continue

        match = CANONICAL_OPTION_TOKEN_PATTERN.match(syntax, index)
        if match is None:
            index += 1
            continue

        token_end = match.end()
        if (
            token_end == len(syntax)
            or syntax[token_end].isspace()
            or syntax[token_end] in CANONICAL_GRAMMAR_ATOM_BOUNDARIES
            or syntax[token_end] == "="
        ):
            tokens.add(match.group(0))
        index = token_end

    return frozenset(tokens)


def validate_operand_projection(operation: Operation, form: Form) -> None:
    terms = form.operand_terms
    if form.target_policy not in KNOWN_TARGET_POLICIES:
        fail(f"unsupported target policy projection: {form.target_policy!r}")
    if form.operand_ordering not in KNOWN_OPERAND_ORDERINGS:
        fail(f"unsupported operand ordering projection: {form.operand_ordering!r}")

    if form.target_policy == "none":
        if form.operand_ordering != "none" or terms:
            fail(f"invalid targetless operand projection: {form.syntax}")
        return

    if form.target_policy == "exactly-one":
        if form.operand_ordering == "preserve-input-order":
            if (
                len(terms) != 1
                or terms[0].kind not in PRIMARY_OPERAND_KINDS
                or terms[0].min_count != 1
                or terms[0].max_count != 1
            ):
                fail(f"invalid exactly-one operand projection: {form.syntax}")
            return
        if form.operand_ordering == "primary-then-environment-assignments":
            if (
                len(terms) != 2
                or terms[0].kind not in ASSIGNMENT_PRIMARY_OPERAND_KINDS
                or terms[0].min_count != 1
                or terms[0].max_count != 1
                or terms[1].kind != "environment-assignment"
                or terms[1].min_count != 0
                or terms[1].max_count is not None
            ):
                fail(
                    "invalid exactly-one primary/assignment projection: "
                    f"{form.syntax}"
                )
            return
        fail(f"invalid exactly-one operand ordering: {form.syntax}")

    if form.target_policy == "one-or-more":
        if (
            form.operand_ordering != "preserve-input-order"
            or len(terms) != 1
            or terms[0].kind not in PRIMARY_OPERAND_KINDS
            or terms[0].min_count != 1
            or terms[0].max_count is not None
        ):
            fail(f"invalid one-or-more operand projection: {form.syntax}")
        return

    if form.target_policy == "ordered-items":
        if (
            form.operand_ordering
            != "package-introduces-following-assignment-scope"
            or len(terms) != 1
            or terms[0].kind != "source-preference-item"
            or terms[0].min_count != 1
            or terms[0].max_count is not None
        ):
            fail(f"invalid ordered-items operand projection: {form.syntax}")
        return

    if form.target_policy == "fixed-sequence":
        shape = tuple((term.kind, term.min_count, term.max_count) for term in terms)
        if form.operand_ordering != "preserve-input-order" or shape not in {
            (("directory", 1, 1), ("patch-directory", 1, 1), ("patch-file", 1, None)),
            (("directory", 1, 1), ("package-base", 1, 1)),
        }:
            fail(f"invalid fixed-sequence operand projection: {form.syntax}")
        return

    if form.target_policy == "delegated":
        if (
            not operation.open_grammar
            or form.operand_ordering != "delegated"
            or len(terms) != 1
            or terms[0].kind != "delegated-pacman-argument"
            or terms[0].min_count != 0
            or terms[0].max_count is not None
        ):
            fail(f"invalid delegated operand projection: {form.syntax}")
        return

    fail(f"unsupported target policy projection: {form.target_policy!r}")


def option_contract_projection(option: Option) -> tuple[object, ...]:
    return (
        option.occurrence,
        option.placement,
        option.value_kind,
        option.allowed_values,
        option.conflict_rule,
        option.conflicts,
        option.conflict_value_identity,
        option.semantic_scopes,
        option.ownership,
        option.definition_role,
        option.completion_visibility,
        option.fixed_value,
    )


def validate_option_projection(schema: CliSchema) -> dict[int, Option]:
    options_by_identity: dict[int, Option] = {}
    for option in schema.options:
        existing = options_by_identity.get(option.identity)
        if existing is None:
            options_by_identity[option.identity] = option
        elif option_contract_projection(existing) != option_contract_projection(option):
            fail(f"option aliases disagree for identity {option.identity}")

        if not option.token or not option.completion_token:
            fail(f"empty option token projection for identity {option.identity}")
        if len(set(option.allowed_values)) != len(option.allowed_values):
            fail(f"duplicate option values for {option.token}")
        if option.value_kind == "attached-enum":
            if (
                not option.allowed_values
                or option.completion_token != option.token + "="
            ):
                fail(f"invalid attached-enum option projection: {option.token}")
        elif option.value_kind == "attached-value":
            if (
                len(option.allowed_values) != 1
                or option.completion_token != option.token + "="
            ):
                fail(f"invalid attached-value option projection: {option.token}")
        elif option.allowed_values or option.completion_token != option.token:
            fail(f"invalid valueless option projection: {option.token}")

        is_end_of_options = (
            option.value_kind == "marker"
            or option.placement == "end-of-options"
        )
        if is_end_of_options and (
            option.token != "--"
            or option.completion_token != "--"
            or option.occurrence != "once"
            or option.placement != "end-of-options"
            or option.value_kind != "marker"
            or option.allowed_values
            or option.conflict_rule != "none"
            or option.conflicts
            or option.conflict_value_identity
            or frozenset(option.semantic_scopes)
            != frozenset({"parser-boundary", "pacman-delegation"})
            or option.ownership != "intercepted-pacman"
            or option.definition_role != "schema-only"
            or option.completion_visibility != "hidden"
        ):
            fail(f"inconsistent end-of-options option projection: {option.token}")

        if len(set(option.conflicts)) != len(option.conflicts):
            fail(f"duplicate option conflict projection: {option.token}")
        if option.identity in option.conflicts:
            fail(f"self-conflicting option projection: {option.token}")
        if option.conflict_rule == "none":
            if option.conflicts or option.conflict_value_identity:
                fail(f"inconsistent conflict-free option projection: {option.token}")
        elif not option.conflicts:
            fail(f"conflict rule has no conflicting options: {option.token}")
        elif option.conflict_rule in {"mutually-exclusive", "operation-local-exclusion"}:
            if option.conflict_value_identity:
                fail(f"mutual exclusion has a value identity: {option.token}")
            if option.conflict_rule == "operation-local-exclusion" and option.placement != "operation-local":
                fail(f"local exclusion requires operation-local placement: {option.token}")
        elif not option.conflict_value_identity:
            fail(f"final-value conflict lacks a value identity: {option.token}")

    for option in options_by_identity.values():
        if option.fixed_value:
            parents = [parent for parent in options_by_identity.values()
                       if parent.value_kind == "attached-enum" and
                       parent.conflict_value_identity == option.conflict_value_identity]
            if (option.value_kind != "none" or
                option.conflict_rule != "final-value-must-agree" or
                len(parents) != 1 or
                option.identity not in parents[0].conflicts or
                option.fixed_value not in parents[0].allowed_values):
                fail(f"invalid fixed value family projection: {option.token}")
        for conflicting_identity in option.conflicts:
            conflicting = options_by_identity.get(conflicting_identity)
            if conflicting is None:
                fail(
                    f"option {option.token} references unknown conflict "
                    f"identity {conflicting_identity}"
                )
            if option.conflict_rule == "operation-local-exclusion":
                continue
            if option.identity not in conflicting.conflicts:
                fail(
                    f"asymmetric option conflict projection: "
                    f"{option.token} -> {conflicting.token}"
                )
            if (
                option.conflict_rule != conflicting.conflict_rule
                or option.conflict_value_identity
                != conflicting.conflict_value_identity
            ):
                fail(
                    f"inconsistent option conflict semantics: "
                    f"{option.token} <-> {conflicting.token}"
                )
    for option in finite_completion_options(schema):
        if any(member.identity != option.identity and not member.fixed_value
               for member in value_family_members(schema, option)):
            fail("finite value family lacks fixed binding")
    return options_by_identity


def validate_relation_projection(
    form: Form,
    options_by_identity: dict[int, Option],
    option_tokens_by_identity: dict[int, tuple[str, ...]],
    *,
    delegated_grammar: bool = False,
) -> None:
    relation_ids = form.option_ids
    syntax_option_tokens = canonical_syntax_option_tokens(form.syntax)
    if len(set(relation_ids)) != len(relation_ids):
        fail(f"duplicate option relation projection: {form.syntax}")

    for relation in form.option_relations:
        option = options_by_identity.get(relation.identity)
        if option is None:
            fail(
                f"form {form.syntax} references unknown option "
                f"identity {relation.identity}"
            )
        if relation.occurrence == "delegated" and not delegated_grammar:
            fail(f"delegated option occurrence in closed form: {form.syntax}")
        has_forwarding_target = relation.forwarding_targets != ("none",)
        has_forwarding_occurrence = relation.forwarding_occurrence != "none"
        if has_forwarding_target != has_forwarding_occurrence:
            fail(f"inconsistent option forwarding projection: {form.syntax}")
        if (
            relation.public_syntax != "hidden"
            and not any(
                token in syntax_option_tokens
                for token in option_tokens_by_identity[relation.identity]
            )
        ):
            fail(
                f"public option syntax is absent from form projection: "
                f"{form.syntax}"
            )


def validate_delegated_tail_projection(
    operation: Operation,
    form: Form,
    options_by_identity: dict[int, Option],
) -> None:
    if form.delegated_tail_policy == "none":
        return
    if form.delegated_tail_policy != "repository-only":
        fail(
            "unsupported delegated tail policy projection: "
            f"{form.delegated_tail_policy!r}"
        )
    if not operation.open_grammar:
        fail(
            "repository-only delegated tail has no open pacman fallback: "
            f"{form.syntax}"
        )
    repository_relation = next(
        (
            relation
            for relation in form.option_relations
            if relation.identity in options_by_identity
            and options_by_identity[relation.identity].token == "--repo"
        ),
        None,
    )
    if repository_relation is None or repository_relation.requirement != "required":
        fail(
            "repository-only delegated tail lacks required --repo selector: "
            f"{form.syntax}"
        )
    if (
        repository_relation.public_syntax != "required"
        or repository_relation.semantic_effects != ("moguet-control",)
        or repository_relation.forwarding_targets != ("none",)
        or repository_relation.forwarding_occurrence != "none"
    ):
        fail(
            "repository-only delegated tail forwards or misclassifies --repo: "
            f"{form.syntax}"
        )


def validate_end_of_options_relation(
    schema: CliSchema,
    options_by_identity: dict[int, Option],
) -> None:
    marker_identities = {
        option.identity
        for option in options_by_identity.values()
        if option.value_kind == "marker"
        or option.placement == "end-of-options"
    }
    if not marker_identities:
        return
    if len(marker_identities) != 1:
        fail("multiple end-of-options option projections")
    if schema.delegated_form is None:
        fail("end-of-options option has no delegated relation projection")

    marker_identity = next(iter(marker_identities))
    relation = next(
        (
            candidate
            for candidate in schema.delegated_form.option_relations
            if candidate.identity == marker_identity
        ),
        None,
    )
    if relation is None:
        fail("delegated end-of-options relation is absent")
    if (
        relation.requirement != "optional"
        or relation.occurrence != "once"
        or relation.public_syntax != "hidden"
        or frozenset(relation.semantic_effects)
        != frozenset({"parser-boundary", "upstream-argument"})
        or relation.forwarding_targets != ("pacman",)
        or relation.forwarding_occurrence != "preserve-all"
    ):
        fail("inconsistent delegated end-of-options relation projection")


def validate_schema_projection(schema: CliSchema) -> None:
    options_by_identity = validate_option_projection(schema)
    option_tokens_by_identity = {
        identity: tuple(
            option.token
            for option in schema.options
            if option.identity == identity
        )
        for identity in options_by_identity
    }
    public_syntax_ids: set[int] = set()
    closed_forms: list[str] = []
    has_open_grammar = False

    for operation in schema.operations:
        has_open_grammar = has_open_grammar or operation.open_grammar
        for form in operation.forms:
            validate_operand_projection(operation, form)
            validate_relation_projection(
                form, options_by_identity, option_tokens_by_identity
            )
            validate_delegated_tail_projection(
                operation, form, options_by_identity
            )
            public_syntax_ids.update(
                relation.identity
                for relation in form.option_relations
                if relation.public_syntax != "hidden"
            )
            closed_forms.append(form.syntax)

    if has_open_grammar:
        if schema.delegated_form is None:
            fail("open grammar has no delegated operand projection")
        validate_operand_projection(
            Operation("<delegated>", open_grammar=True), schema.delegated_form
        )
        validate_relation_projection(
            schema.delegated_form,
            options_by_identity,
            option_tokens_by_identity,
            delegated_grammar=True,
        )
    elif schema.delegated_form is not None:
        fail("delegated operand projection has no open grammar")

    validate_end_of_options_relation(schema, options_by_identity)

    for option in options_by_identity.values():
        if (
            option.definition_role == "syntax-only"
            and option.identity not in public_syntax_ids
        ):
            fail(f"syntax-only option has no public grammar form: {option.token}")

    if len(set(closed_forms)) != len(closed_forms):
        fail("CLI authority exporter returned duplicate closed grammar forms")
    if tuple(closed_forms) != schema.canonical_grammar:
        fail("canonical grammar differs from closed form projection")
    if len(set(schema.terminal_tokens)) != len(schema.terminal_tokens):
        fail("CLI authority exporter returned duplicate terminal tokens")


def finite_operand_max(form: Form) -> int | None:
    if form.operand_ordering == "none":
        return 0
    if form.operand_ordering == "preserve-input-order":
        if any(term.max_count is None for term in form.operand_terms):
            return None
        return sum(term.max_count for term in form.operand_terms)
    return None


def bash_form_prefix_cases(schema: CliSchema) -> str:
    cases: list[str] = []
    for operation in schema.operations:
        for form_index, form in enumerate(operation.forms):
            key = shell_quote(f"{operation.token}:{form_index}")
            maximum = finite_operand_max(form)
            if maximum is not None:
                body = (
                    f"            (( ${{#operands[@]}} <= {maximum} )) && return 0\n"
                    "            return 1"
                )
            elif form.operand_ordering == "preserve-input-order":
                body = "            return 0"
            elif form.operand_ordering == "primary-then-environment-assignments":
                body = (
                    "            (( ${#operands[@]} == 0 )) && return 0\n"
                    "            _moguet_is_assignment_operand \"${operands[0]}\" && return 1\n"
                    "            for word in \"${operands[@]:1}\"; do\n"
                    "                _moguet_is_assignment_operand \"$word\" || return 1\n"
                    "            done\n"
                    "            return 0"
                )
            elif form.operand_ordering == "package-introduces-following-assignment-scope":
                body = (
                    "            (( ${#operands[@]} == 0 )) && return 0\n"
                    "            _moguet_is_assignment_operand \"${operands[0]}\" && return 1\n"
                    "            return 0"
                )
            elif form.operand_ordering == "delegated":
                body = "            return 0"
            else:
                fail(f"unsupported Bash operand projection: {form.syntax}")
            cases.append(f"        {key})\n{body}\n            ;;")
    return "\n".join(cases)


def zsh_form_prefix_cases(schema: CliSchema) -> str:
    cases: list[str] = []
    for operation in schema.operations:
        for form_index, form in enumerate(operation.forms):
            key = shell_quote(f"{operation.token}:{form_index}")
            maximum = finite_operand_max(form)
            if maximum is not None:
                body = (
                    f"            (( ${{#operands[@]}} <= {maximum} )) && return 0\n"
                    "            return 1"
                )
            elif form.operand_ordering == "preserve-input-order":
                body = "            return 0"
            elif form.operand_ordering == "primary-then-environment-assignments":
                body = (
                    "            (( ${#operands[@]} == 0 )) && return 0\n"
                    "            _moguet_is_assignment_operand \"${operands[1]}\" && return 1\n"
                    "            for (( operand_index=2; operand_index<=${#operands[@]}; ++operand_index )); do\n"
                    "                _moguet_is_assignment_operand \"${operands[operand_index]}\" || return 1\n"
                    "            done\n"
                    "            return 0"
                )
            elif form.operand_ordering == "package-introduces-following-assignment-scope":
                body = (
                    "            (( ${#operands[@]} == 0 )) && return 0\n"
                    "            _moguet_is_assignment_operand \"${operands[1]}\" && return 1\n"
                    "            return 0"
                )
            elif form.operand_ordering == "delegated":
                body = "            return 0"
            else:
                fail(f"unsupported Zsh operand projection: {form.syntax}")
            cases.append(f"        {key})\n{body}\n            ;;")
    return "\n".join(cases)


def fish_form_prefix_cases(schema: CliSchema) -> list[str]:
    cases: list[str] = []
    for operation in schema.operations:
        for form_index, form in enumerate(operation.forms):
            key = fish_quote(f"{operation.token}:{form_index}")
            maximum = finite_operand_max(form)
            if maximum is not None:
                body = [
                    f"            test (count $operands) -le {maximum}; and return 0",
                    "            return 1",
                ]
            elif form.operand_ordering == "preserve-input-order":
                body = ["            return 0"]
            elif form.operand_ordering == "primary-then-environment-assignments":
                body = [
                    "            test (count $operands) -eq 0; and return 0",
                    "            __moguet_is_assignment_operand \"$operands[1]\"; and return 1",
                    "            for word in $operands[2..-1]",
                    "                __moguet_is_assignment_operand \"$word\"; or return 1",
                    "            end",
                    "            return 0",
                ]
            elif form.operand_ordering == "package-introduces-following-assignment-scope":
                body = [
                    "            test (count $operands) -eq 0; and return 0",
                    "            __moguet_is_assignment_operand \"$operands[1]\"; and return 1",
                    "            return 0",
                ]
            elif form.operand_ordering == "delegated":
                body = ["            return 0"]
            else:
                fail(f"unsupported Fish operand projection: {form.syntax}")
            cases.extend([f"        case {key}", *body])
    return cases


def bash_array(values: tuple[str, ...] | list[str], indent: str = "            ") -> str:
    return " ".join(shell_quote(value) for value in values)


def presentation_additions(schema: CliSchema, shell: str) -> str:
    lines = []
    for operation, option_id, required in schema.presentation_scopes:
        token = next(option.completion_token for option in schema.options if option.identity == option_id)
        if shell == "fish":
            condition = f'test "$operation" = {fish_quote(operation)}; and test "$option_id" = {option_id}'
            if required is not None:
                condition += f'; and __moguet_has_option_id {required}'
            lines.append(f"    if {condition}; return 0; end")
        else:
            condition = f'[[ $operation == {shell_quote(operation)} ]]'
            if required is not None:
                condition += f' && _moguet_has_option_id {required}'
            array = "candidates" if shell == "bash" else "reply"
            condition += f' && [[ " ${{{array}[*]}} " != *{shell_quote(" " + token + " ")}* ]]'
            lines.append(f"    if {condition}; then {array}+=({shell_quote(token)}); fi")
    return "\n".join(lines)


def local_prefix_operations(schema: CliSchema) -> tuple[str, ...]:
    # Slice 1b deliberately projects only the first operand of plain sync.
    # No modifiers, prior operands, globals or cursor suffix are accepted.
    # This allowlist narrows the shared operand authority; shells do not infer
    # meaning from an operation's spelling or from delegated pacman grammar.
    supported = ("-S",)
    contexts = dict(schema.operand_contexts)
    return tuple(token for token in supported if contexts.get(token) == "package")


def prefix_helper_binding(path: str, shell: str) -> str:
    if path == "@MOGUET_REPOSITORY_PREFIX_HELPER@":
        return f"@MOGUET_REPOSITORY_PREFIX_HELPER_{shell.upper()}@"
    return fish_quote(path) if shell == "fish" else shell_quote(path)


def finite_completion_options(schema: CliSchema) -> tuple[Option, ...]:
    # Slice 2 selects an existing public contract, never internal enum types.
    return tuple(option for option in schema.options
                 if option.token == "--build-mode" and option.is_completion_visible
                 and option.value_kind == "attached-enum")


def value_family_members(schema: CliSchema, option: Option) -> tuple[Option, ...]:
    return tuple(member for member in schema.options
                 if member.conflict_value_identity == option.conflict_value_identity
                 and member.conflict_rule == "final-value-must-agree")


def typed_value_cases(schema: CliSchema, shell: str) -> str:
    cases = []
    for option in finite_completion_options(schema):
        if shell == "fish":
            cases.append(f"        case {fish_quote(option.completion_token + '*')}\n"
                         f"            set option_id {option.identity}\n"
                         f"            set option_prefix {fish_quote(option.completion_token)}\n"
                         f"            set values {' '.join(map(fish_quote, option.allowed_values))}")
        else:
            cases.append(f"        {shell_quote(option.completion_token)}*)\n"
                         f"            option_prefix={shell_quote(option.completion_token)}\n"
                         f"            values=({' '.join(map(shell_quote, option.allowed_values))})\n"
                         "            ;;")
    return "\n".join(cases)


def typed_family_cases(schema: CliSchema, shell: str) -> str:
    cases = []
    for option in finite_completion_options(schema):
        for member in value_family_members(schema, option):
            if member.identity == option.identity:
                if shell == "fish":
                    cases.append(f"        case {fish_quote(member.token)} {fish_quote(member.completion_token + '*')}\n"
                                 f"            string sub -s {len(member.completion_token) + 1} -- \"$word\"\n"
                                 "            return 0")
                else:
                    cases.append(f"        {shell_quote(member.token)}|{shell_quote(member.completion_token)}*)\n"
                                 f"            REPLY=${{1#{shell_quote(member.completion_token)}}}\n"
                                 f"            [[ $1 == {shell_quote(member.token)} ]] && REPLY=\n"
                                 "            return 0 ;;")
            elif member.fixed_value:
                if shell == "fish":
                    cases.append(f"        case {fish_quote(member.token)}\n"
                                 f"            echo {fish_quote(member.fixed_value)}\n"
                                 "            return 0")
                else:
                    cases.append(f"        {shell_quote(member.token)}) REPLY={shell_quote(member.fixed_value)}; return 0 ;;")
            else:
                fail(f"finite value family lacks a fixed binding: {member.token}")
    return "\n".join(cases)


def bash_typed_functions(schema: CliSchema) -> str:
    lexical = "|".join(shell_quote(token) for token, _ in schema.lexical_value_options)
    boundary = "|".join(shell_quote(token) for token, _ in schema.parser_boundaries)
    return f'''
_moguet_current_word() {{
    local word="${{COMP_WORDS[COMP_CWORD]}}" joined start
    local LC_ALL=C
    current_word_start=$COMP_CWORD
    if [[ -n ${{COMP_LINE-}} ]]; then
        while (( current_word_start > 1 )); do
            joined=${{COMP_WORDS[current_word_start-1]}}$word
            start=$((COMP_POINT-${{#joined}}))
            (( start >= 0 )) || break
            [[ ${{COMP_LINE:start:${{#joined}}}} == "$joined" ]] || break
            word=$joined
            ((--current_word_start))
        done
    fi
    _moguet_unquote_prefix "$word"
}}

_moguet_value_family() {{
    REPLY=
    case "$1" in
{typed_family_cases(schema, 'bash')}
    esac
    return 1
}}

_moguet_typed_words() {{
    # COMP_WORDS also splits completed attached values. Reassemble the native
    # fragments only where COMP_LINE proves adjacency, then decode shell quotes
    # once. This is lexical plumbing, not an operation/value grammar parser.
    local index position=0 word spaced logical_cword=$COMP_CWORD
    local LC_ALL=C
    local -a logical_words=()
    if [[ -n ${{COMP_LINE-}} ]]; then
        for ((index=0; index<${{#COMP_WORDS[@]}}; ++index)); do
            word=${{COMP_WORDS[index]}}
            spaced=false
            while [[ ${{COMP_LINE:position:1}} == [$' \\t\\n'] ]]; do
                spaced=true
                ((++position))
            done
            [[ ${{COMP_LINE:position:${{#word}}}} == "$word" ]] || return 1
            if [[ $spaced == true || ${{#logical_words[@]}} == 0 ]]; then
                logical_words+=("$word")
            else
                logical_words[${{#logical_words[@]}}-1]+=$word
            fi
            (( index == COMP_CWORD )) && logical_cword=$((${{#logical_words[@]}}-1))
            ((position+=${{#word}}))
        done
    else
        logical_words=("${{COMP_WORDS[@]}}")
    fi
    for ((index=0; index<${{#logical_words[@]}}; ++index)); do
        _moguet_unquote_prefix "${{logical_words[index]}}"
        logical_words[index]=$REPLY
    done
    COMP_WORDS=("${{logical_words[@]}}")
    COMP_CWORD=$logical_cword
    return 0
}}

_moguet_typed_values() {{
    local option_prefix word requested fixed= pending=false value
    local -a values
    case "$cur" in
{typed_value_cases(schema, 'bash')}
        *) return 1 ;;
    esac
    # Recognized attached values own this event, including suppressed/no-match
    # contexts. They never fall through to the package provider.
    COMPREPLY=()
    [[ ${{#COMP_WORDS[@]}} == $((COMP_CWORD+1)) ]] || return 0
    [[ " ${{candidates[*]}} " == *" $option_prefix "* ]] || return 0
    for word in "${{COMP_WORDS[@]:1:COMP_CWORD-1}}"; do
        if [[ $pending == true ]]; then pending=false; continue; fi
        case "$word" in
        {boundary or '__no_boundary__'}) return 0 ;;
        {lexical or '__no_value_option__'}) pending=true; continue ;;
        esac
        if _moguet_value_family "$word"; then
            requested=$REPLY
            [[ " ${{values[*]}} " == *" $requested "* && -n $requested ]] || return 0
            [[ -z $fixed || $fixed == "$requested" ]] || return 0
            fixed=$requested
        fi
    done
    [[ $pending == false ]] || return 0
    local prefix=${{cur#"$option_prefix"}}
    for value in "${{values[@]}}"; do
        [[ -z $fixed || $fixed == "$value" ]] || continue
        [[ $value == "$prefix"* ]] || continue
        COMPREPLY+=("$typed_insert_prefix$value")
    done
    return 0
}}
'''


def zsh_typed_functions(schema: CliSchema) -> str:
    lexical = "|".join(shell_quote(token) for token, _ in schema.lexical_value_options)
    boundary = "|".join(shell_quote(token) for token, _ in schema.parser_boundaries)
    return f'''
_moguet_value_family() {{
    REPLY=
    case "$1" in
{typed_family_cases(schema, 'zsh')}
    esac
    return 1
}}

_moguet_typed_values() {{
    local option_prefix word requested fixed= pending=false value
    local -a values typed_candidates
    case "$cur" in
{typed_value_cases(schema, 'zsh')}
        *) return 1 ;;
    esac
    (( CURRENT == ${{#words}} )) || return 0
    (( ${{candidates[(Ie)$option_prefix]}} )) || return 0
    local index
    for ((index=2; index<CURRENT; ++index)); do
        word=$words[index]
        if [[ $pending == true ]]; then pending=false; continue; fi
        case "$word" in
        {boundary or '__no_boundary__'}) return 0 ;;
        {lexical or '__no_value_option__'}) pending=true; continue ;;
        esac
        if _moguet_value_family "$word"; then
            requested=$REPLY
            (( ${{values[(Ie)$requested]}} )) || return 0
            [[ -z $fixed || $fixed == "$requested" ]] || return 0
            fixed=$requested
        fi
    done
    [[ $pending == false ]] || return 0
    local prefix=${{cur#"$option_prefix"}}
    for value in "${{values[@]}}"; do
        [[ -z $fixed || $fixed == "$value" ]] || continue
        [[ $value == "$prefix"* ]] || continue
        typed_candidates+=("$option_prefix$value")
    done
    (( ${{#typed_candidates}} )) && compadd -a typed_candidates
    return 0
}}
'''


def fish_typed_functions(schema: CliSchema) -> list[str]:
    return [
        "function __moguet_value_word",
        "    set -l raw (commandline -ct | string collect -a)",
        "    set -l current (string unescape -- \"$raw\")",
        "    if test $status -ne 0",
        "        set current (string unescape -- \"$raw'\")",
        "        if test $status -ne 0",
        "            set current (string unescape -- \"$raw\\\"\")",
        "            test $status -eq 0; or return 1",
        "        end", "    end",
        "    printf '%s\\n' \"$current\"", "end", "",
        "function __moguet_value_family --argument-names word",
        "    switch $word", typed_family_cases(schema, "fish"),
        "    end", "    return 1", "end", "",
        "function __moguet_typed_values",
        "    set -l current (__moguet_value_word)",
        "    set -l option_id option_prefix values",
        "    switch $current", typed_value_cases(schema, "fish"),
        "        case '*'", "            return 0", "    end",
        "    __moguet_operation_allows $option_id; or return 0",
        "    set -l pending false", "    set -l fixed ''",
        "    for word in (commandline -opc)[2..-1]",
        "        set word (string unescape -- \"$word\")",
        "        if test $pending = true; set pending false; continue; end",
        "        switch $word",
        "            case " + " ".join(fish_quote(t) for t, _ in schema.parser_boundaries),
        "                return 0",
        "            case " + " ".join(fish_quote(t) for t, _ in schema.lexical_value_options),
        "                set pending true; continue", "        end",
        "        set -l requested (__moguet_value_family \"$word\" | string collect -a)",
        "        if test $pipestatus[1] -eq 0",
        "            contains -- \"$requested\" $values; or return 0",
        "            test -z \"$fixed\"; or test \"$fixed\" = \"$requested\"; or return 0",
        "            set fixed \"$requested\"", "        end", "    end",
        "    test $pending = false; or return 0",
        "    set -l prefix (string sub -s (math (string length -- \"$option_prefix\") + 1) -- \"$current\")",
        "    for value in $values",
        "        test -z \"$fixed\"; or test \"$fixed\" = \"$value\"; or continue",
        "        test (string sub -l (string length -- \"$prefix\") -- \"$value\") = \"$prefix\"; or continue",
        "        printf '%s%s\\n' \"$option_prefix\" \"$value\"", "    end", "end", "",
        "complete -c moguet -f -a '(__moguet_typed_values)'",
    ]


def render_bash(schema: CliSchema, descriptions: Descriptions, locale: str,
                prefix_helper: str = "@MOGUET_REPOSITORY_PREFIX_HELPER@") -> str:
    del descriptions
    operations = tuple(operation.token for operation in schema.operations)
    root_options = tuple(
        option
        for option in schema.options
        if option.is_completion_visible
        and option.placement in {"parser-global", "first-non-global"}
    )
    terminal_pattern = "|".join(schema.terminal_tokens)
    operation_pattern = "|".join(operations)
    once_ids = tuple(
        dict.fromkeys(
            option.identity
            for option in schema.options
            if option.is_completion_visible and option.occurrence == "once"
        )
    )

    option_id_cases = "\n".join(
        f"        {'|'.join(patterns)}) printf '%s' {identity} ;;"
        for identity, patterns in option_case_patterns(schema)
    )
    conflict_cases = "\n".join(
        f"        {option.identity}) "
        + " || ".join(
            f"_moguet_has_option_id {conflict}"
            for conflict in completion_conflicts(schema, option)
        )
        + " ;;"
        for option in schema.options
        if completion_conflicts(schema, option)
    )

    operation_cases: list[str] = []
    delegated_tokens = unique_completion_tokens(
        options_for_ids(schema, schema.delegated_option_ids)
    )
    for operation in schema.operations:
        if len(operation.forms) > 1:
            selector_forms = [form for form in operation.forms if form.selector_ids]
            default_forms = [form for form in operation.forms if not form.selector_ids]
            if len(selector_forms) != 1 or len(default_forms) != 1:
                fail(f"unsupported multi-form completion projection: {operation.token}")
            selected = selector_forms[0]
            default = default_forms[0]
            selected_index = operation.forms.index(selected)
            default_index = operation.forms.index(default)
            selector_checks = " || ".join(
                f"_moguet_has_option_id {identity}"
                for identity in selected.selector_ids
            )
            selected_tokens = unique_completion_tokens(
                options_for_ids(
                    schema, completion_ids_for_form(schema, selected)
                )
            )
            default_tokens = unique_completion_tokens(
                options_for_ids(
                    schema, completion_ids_for_form(schema, default)
                )
            )
            union_tokens = unique_completion_tokens(
                options_for_ids(
                    schema,
                    union_completion_form_ids(schema, operation.forms),
                )
            )
            if operation.open_grammar:
                operand_branch = (
                    f"            elif _moguet_has_operand {shell_quote(operation.token)}; then\n"
                    f"                candidates=({bash_array(delegated_tokens)})\n"
                )
            else:
                operand_branch = (
                    f"            elif _moguet_has_operand {shell_quote(operation.token)}; then\n"
                    f"                if _moguet_form_prefix_valid {shell_quote(operation.token)} {default_index}; then\n"
                    f"                    candidates=({bash_array(default_tokens)})\n"
                    f"                else\n"
                    f"                    candidates=()\n"
                    f"                fi\n"
                )
            operation_cases.append(
                f"        {operation.token})\n"
                f"            if {selector_checks}; then\n"
                f"                if _moguet_form_prefix_valid {shell_quote(operation.token)} {selected_index}; then\n"
                f"                    candidates=({bash_array(selected_tokens)})\n"
                f"                else\n"
                f"                    candidates=()\n"
                f"                fi\n"
                f"{operand_branch}"
                f"            else\n"
                f"                candidates=({bash_array(union_tokens)})\n"
                f"            fi\n"
                f"            ;;"
            )
        elif operation.forms and operation.open_grammar:
            form = operation.forms[0]
            selector_checks = " || ".join(
                f"_moguet_has_option_id {identity}"
                for identity in form.selector_ids
            )
            selected_tokens = unique_completion_tokens(
                options_for_ids(
                    schema, completion_ids_for_form(schema, form)
                )
            )
            preselection_ids = tuple(
                dict.fromkeys(schema.delegated_option_ids + form.selector_ids)
            )
            preselection_tokens = unique_completion_tokens(
                options_for_ids(schema, preselection_ids)
            )
            operation_cases.append(
                f"        {operation.token})\n"
                f"            if {selector_checks}; then\n"
                f"                if _moguet_form_prefix_valid {shell_quote(operation.token)} 0; then\n"
                f"                    candidates=({bash_array(selected_tokens)})\n"
                f"                else\n"
                f"                    candidates=()\n"
                f"                fi\n"
                f"            else\n"
                f"                candidates=({bash_array(preselection_tokens)})\n"
                f"            fi\n"
                f"            ;;"
            )
        elif operation.forms:
            form = operation.forms[0]
            tokens = unique_completion_tokens(
                options_for_ids(
                    schema, completion_ids_for_form(schema, form)
                )
            )
            operation_cases.append(
                f"        {operation.token})\n"
                f"            if _moguet_form_prefix_valid {shell_quote(operation.token)} 0; then\n"
                f"                candidates=({bash_array(tokens)})\n"
                f"            else\n"
                f"                candidates=()\n"
                f"            fi\n"
                f"            ;;"
            )
        else:
            operation_cases.append(
                f"        {operation.token}) candidates=({bash_array(delegated_tokens)}) ;;"
            )
    for terminal in schema.terminal_tokens:
        operation_cases.append(f"        {terminal}) candidates=() ;;")

    canonical_comments = "\n".join(
        f"#   {syntax}" for syntax in schema.canonical_grammar
    )
    return f"""# Generated by scripts/generate_completions.py; do not edit.
# Description locale: {locale}
# Canonical closed grammar (projected from source/cli_authority.hpp):
{canonical_comments}

_moguet_option_id() {{
    case "$1" in
{option_id_cases}
        *) return 1 ;;
    esac
}}

_moguet_has_option_id() {{
    local expected="$1" word actual
    for word in "${{COMP_WORDS[@]:1:COMP_CWORD-1}}"; do
        actual="$(_moguet_option_id "$word" || true)"
        [[ $actual == "$expected" ]] && return 0
    done
    return 1
}}

_moguet_find_operation() {{
    local word
    for word in "${{COMP_WORDS[@]:1:COMP_CWORD-1}}"; do
        case "$word" in
        {terminal_pattern}|{operation_pattern}) printf '%s' "$word"; return 0 ;;
        esac
        _moguet_option_id "$word" >/dev/null && continue
        if [[ $word == -* ]]; then
            printf '%s' __delegated__
            return 0
        fi
        printf '%s' __invalid__
        return 0
    done
    return 1
}}

_moguet_has_operand() {{
    local expected_operation="$1" word actual seen_operation=false
    for word in "${{COMP_WORDS[@]:1:COMP_CWORD-1}}"; do
        if [[ $seen_operation == false ]]; then
            if [[ $word == "$expected_operation" ]]; then
                seen_operation=true
            fi
            continue
        fi
        actual="$(_moguet_option_id "$word" || true)"
        [[ -z $actual ]] && return 0
    done
    return 1
}}

_moguet_is_assignment_operand() {{
    [[ $1 == *=* ]]
}}

_moguet_form_prefix_valid() {{
    local expected_operation="$1" form_index="$2" word actual
    local seen_operation=false
    local -a operands=()
    for word in "${{COMP_WORDS[@]:1:COMP_CWORD-1}}"; do
        if [[ $seen_operation == false ]]; then
            [[ $word == "$expected_operation" ]] && seen_operation=true
            continue
        fi
        actual="$(_moguet_option_id "$word" || true)"
        [[ -n $actual ]] && continue
        operands+=("$word")
    done
    case "$expected_operation:$form_index" in
{bash_form_prefix_cases(schema)}
        *) return 1 ;;
    esac
}}

_moguet_conflicts_with_present_option() {{
    case "$1" in
{conflict_cases}
        *) return 1 ;;
    esac
}}

{bash_typed_functions(schema)}

_moguet() {{
    local cur operation candidate option_id REPLY strip_prefix typed_insert_prefix
    local current_word_start original_cword=$COMP_CWORD
    local -a COMP_WORDS=("${{COMP_WORDS[@]}}")
    local COMP_CWORD=$COMP_CWORD
    local -a candidates filtered
    cur="${{COMP_WORDS[COMP_CWORD]}}"
    _moguet_current_word
    case "$REPLY" in
    {'|'.join(shell_quote(o.completion_token) + '*' for o in finite_completion_options(schema)) or '__no_finite_value__'})
        cur=$REPLY
        if (( current_word_start == original_cword )); then
            typed_insert_prefix=${{cur%%=*}}=
        else
            typed_insert_prefix=
        fi
        _moguet_typed_words || {{ COMPREPLY=(); return 0; }}
        cur=${{COMP_WORDS[COMP_CWORD]}}
        ;;
    esac
    operation="$(_moguet_find_operation || true)"

    if [[ -z $operation ]]; then
        candidates=({bash_array(list(operations) + [option.completion_token for option in root_options])})
    else
        case "$operation" in
{chr(10).join(operation_cases)}
        __delegated__) candidates=({bash_array(delegated_tokens)}) ;;
        *) candidates=() ;;
        esac
    fi

{presentation_additions(schema, "bash")}

    _moguet_typed_values && return 0

    filtered=()
    for candidate in "${{candidates[@]}}"; do
        option_id="$(_moguet_option_id "$candidate" || true)"
        if [[ -n $option_id ]]; then
            case "$option_id" in
            {'|'.join(str(identity) for identity in once_ids)})
                _moguet_has_option_id "$option_id" && continue
                ;;
            esac
            _moguet_conflicts_with_present_option "$option_id" && continue
        fi
        filtered+=("$candidate")
    done

    COMPREPLY=()
    while IFS= read -r candidate; do
        COMPREPLY+=("$candidate")
    done < <(compgen -W "${{filtered[*]}}" -- "$cur" || true)

    # One provider call, outside all static candidate scans.
    _moguet_repository_context && _moguet_repository_packages "$REPLY" "$strip_prefix"
    return 0
}}

_moguet_repository_context() {{
    local word before= index start
    local LC_ALL=C
    [[ $COMP_CWORD -ge 2 && ${{#COMP_WORDS[@]}} == $((COMP_CWORD+1)) ]] || return 1
    case "${{COMP_WORDS[1]}}" in
    {'|'.join(local_prefix_operations(schema)) or '__no_local_prefix_context__'}) ;;
    *) return 1 ;;
    esac
    word=${{COMP_WORDS[2]}}
    for ((index=3; index<=COMP_CWORD; ++index)); do
        before=$word
        word+=${{COMP_WORDS[index]}}
    done
    if (( COMP_CWORD > 2 )); then
        # Bash splits valid package '@' at COMP_WORDBREAKS. Rejoin only
        # adjacent fragments of this raw current word, never separate argv.
        [[ -n ${{COMP_LINE-}} ]] || return 1
        start=$((COMP_POINT-${{#word}}))
        (( start > 0 )) || return 1
        [[ ${{COMP_LINE:start:${{#word}}}} == "$word" &&
           ${{COMP_LINE:start-1:1}} == [$' \\t'] ]] || return 1
    fi
    _moguet_unquote_prefix "$before"
    # Readline replaces the preceding wordbreak character together with its
    # RHS (ch@to), even though COMP_WORDS exposes '@' as a separate entry.
    strip_prefix=${{REPLY%@}}
    _moguet_unquote_prefix "$word"
    [[ $REPLY != -* ]]
}}

_moguet_unquote_prefix() {{
    # COMP_WORDS retains shell quotes. Decode syntax only, never eval/expand
    # substitutions. An unfinished quote is normal in an incomplete word.
    local word="$1" quote= char next index
    REPLY=
    for ((index=0; index<${{#word}}; ++index)); do
        char=${{word:index:1}}
        if [[ $quote == "'" ]]; then
            if [[ $char == "'" ]]; then quote=; else REPLY+=$char; fi
        elif [[ $char == "$quote" && -n $quote ]]; then
            quote=
        elif [[ -z $quote && ( $char == "'" || $char == '"' ) ]]; then
            quote=$char
        elif [[ $char == $'\\\\' ]]; then
            next=${{word:index+1:1}}
            if [[ -n $next && ( -z $quote || $next == [\\$\\`\\"\\\\] ) ]]; then
                REPLY+=$next
                ((++index))
            else
                REPLY+=$char
            fi
        else
            REPLY+=$char
        fi
    done
}}

_moguet_repository_packages() {{
    local prefix="$1" strip_prefix="$2" helper={prefix_helper_binding(prefix_helper, 'bash')} output candidate previous=
    local LC_ALL=C
    local -a packages=()
    [[ $helper == /* && -x $helper ]] || return 0
    # Preserve the final newline and exit status. read -d NUL detects binary
    # protocol corruption before Bash command substitution can drop NULs.
    output="$(set -o pipefail
        "$helper" "$prefix" 2>/dev/null | {{
            IFS= read -r -d '' output && exit 1
            printf '%s\\001' "$output"
        }}
    )" || return 0
    output=${{output%$'\\001'}}
    [[ ${{#output}} -le 65536 ]] || return 0
    [[ -z $output ]] && return 0
    [[ $output == *$'\\n' ]] || return 0
    while IFS= read -r candidate; do
        [[ $candidate =~ ^[A-Za-z0-9@._+][A-Za-z0-9@._+-]*$ &&
           $candidate != . && $candidate != .. &&
           $candidate == "$prefix"* ]] || return 0
        [[ -z $previous || $candidate > $previous ]] || return 0
        packages+=("$candidate")
        [[ ${{#packages[@]}} -le 256 ]] || return 0
        previous=$candidate
    done <<< "${{output%$'\\n'}}"
    for candidate in "${{packages[@]}}"; do
        COMPREPLY+=("${{candidate#"$strip_prefix"}}")
    done
    return 0
}}

complete -F _moguet moguet
"""


def zsh_case_values(values: tuple[str, ...] | list[str]) -> str:
    return " ".join(shell_quote(value) for value in values)


def render_zsh(schema: CliSchema, descriptions: Descriptions, locale: str,
               prefix_helper: str = "@MOGUET_REPOSITORY_PREFIX_HELPER@") -> str:
    operations = tuple(operation.token for operation in schema.operations)
    root_options = tuple(
        option
        for option in schema.options
        if option.is_completion_visible
        and option.placement in {"parser-global", "first-non-global"}
    )
    terminal_pattern = "|".join(schema.terminal_tokens)
    operation_pattern = "|".join(operations)
    once_ids = tuple(
        dict.fromkeys(
            option.identity
            for option in schema.options
            if option.is_completion_visible and option.occurrence == "once"
        )
    )
    option_id_cases = "\n".join(
        f"        {'|'.join(patterns)}) REPLY={identity} ;;"
        for identity, patterns in option_case_patterns(schema)
    )
    conflict_cases = "\n".join(
        f"        {option.identity}) "
        + " || ".join(
            f"_moguet_has_option_id {conflict}"
            for conflict in completion_conflicts(schema, option)
        )
        + " ;;"
        for option in schema.options
        if completion_conflicts(schema, option)
    )

    delegated_tokens = unique_completion_tokens(
        options_for_ids(schema, schema.delegated_option_ids)
    )
    operation_cases: list[str] = []
    for operation in schema.operations:
        if len(operation.forms) > 1:
            selected = next(form for form in operation.forms if form.selector_ids)
            default = next(form for form in operation.forms if not form.selector_ids)
            selected_index = operation.forms.index(selected)
            default_index = operation.forms.index(default)
            selector_checks = " || ".join(
                f"_moguet_has_option_id {identity}"
                for identity in selected.selector_ids
            )
            selected_tokens = unique_completion_tokens(
                options_for_ids(
                    schema, completion_ids_for_form(schema, selected)
                )
            )
            default_tokens = unique_completion_tokens(
                options_for_ids(
                    schema, completion_ids_for_form(schema, default)
                )
            )
            union_tokens = unique_completion_tokens(
                options_for_ids(
                    schema,
                    union_completion_form_ids(schema, operation.forms),
                )
            )
            if operation.open_grammar:
                operand_branch = (
                    f"            elif _moguet_has_operand {shell_quote(operation.token)}; then\n"
                    f"                reply=({zsh_case_values(delegated_tokens)})\n"
                )
            else:
                operand_branch = (
                    f"            elif _moguet_has_operand {shell_quote(operation.token)}; then\n"
                    f"                if _moguet_form_prefix_valid {shell_quote(operation.token)} {default_index}; then\n"
                    f"                    reply=({zsh_case_values(default_tokens)})\n"
                    f"                else\n"
                    f"                    reply=()\n"
                    f"                fi\n"
                )
            operation_cases.append(
                f"        {operation.token})\n"
                f"            if {selector_checks}; then\n"
                f"                if _moguet_form_prefix_valid {shell_quote(operation.token)} {selected_index}; then\n"
                f"                    reply=({zsh_case_values(selected_tokens)})\n"
                f"                else\n"
                f"                    reply=()\n"
                f"                fi\n"
                f"{operand_branch}"
                f"            else\n"
                f"                reply=({zsh_case_values(union_tokens)})\n"
                f"            fi\n"
                f"            ;;"
            )
        elif operation.forms and operation.open_grammar:
            form = operation.forms[0]
            selector_checks = " || ".join(
                f"_moguet_has_option_id {identity}"
                for identity in form.selector_ids
            )
            selected_tokens = unique_completion_tokens(
                options_for_ids(
                    schema, completion_ids_for_form(schema, form)
                )
            )
            preselection_ids = tuple(
                dict.fromkeys(schema.delegated_option_ids + form.selector_ids)
            )
            preselection_tokens = unique_completion_tokens(
                options_for_ids(schema, preselection_ids)
            )
            operation_cases.append(
                f"        {operation.token})\n"
                f"            if {selector_checks}; then\n"
                f"                if _moguet_form_prefix_valid {shell_quote(operation.token)} 0; then\n"
                f"                    reply=({zsh_case_values(selected_tokens)})\n"
                f"                else\n"
                f"                    reply=()\n"
                f"                fi\n"
                f"            else\n"
                f"                reply=({zsh_case_values(preselection_tokens)})\n"
                f"            fi\n"
                f"            ;;"
            )
        elif operation.forms:
            form = operation.forms[0]
            tokens = unique_completion_tokens(
                options_for_ids(
                    schema, completion_ids_for_form(schema, form)
                )
            )
            operation_cases.append(
                f"        {operation.token})\n"
                f"            if _moguet_form_prefix_valid {shell_quote(operation.token)} 0; then\n"
                f"                reply=({zsh_case_values(tokens)})\n"
                f"            else\n"
                f"                reply=()\n"
                f"            fi\n"
                f"            ;;"
            )
        else:
            operation_cases.append(
                f"        {operation.token}) reply=({zsh_case_values(delegated_tokens)}) ;;"
            )
    for terminal in schema.terminal_tokens:
        operation_cases.append(f"        {terminal}) reply=() ;;")

    description_cases: list[str] = []
    for operation in schema.operations:
        description = descriptions.operations[operation.token].replace(":", r"\:")
        description_cases.append(
            f"        {operation.token}) REPLY={shell_quote(description)} ;;"
        )
    for option in schema.options:
        if not option.is_completion_visible:
            continue
        description = descriptions.options[option.token].replace(":", r"\:")
        description_cases.append(
            f"        {shell_quote(option.completion_token)}) "
            f"REPLY={shell_quote(description)} ;;"
        )

    canonical_comments = "\n".join(
        f"#   {syntax}" for syntax in schema.canonical_grammar
    )
    root_candidates = list(operations) + [
        option.completion_token for option in root_options
    ]
    return f"""#compdef moguet
# Generated by scripts/generate_completions.py; do not edit.
# Description locale: {locale}
# Canonical closed grammar (projected from source/cli_authority.hpp):
{canonical_comments}

_moguet_option_id() {{
    REPLY=
    case "$1" in
{option_id_cases}
    esac
    [[ -n $REPLY ]]
}}

_moguet_has_option_id() {{
    local expected="$1" word actual
    local index
    for (( index=2; index<CURRENT; ++index )); do
        word=$words[index]
        _moguet_option_id "$word" || continue
        actual=$REPLY
        [[ $actual == "$expected" ]] && return 0
    done
    return 1
}}

_moguet_find_operation() {{
    local word index
    REPLY=
    for (( index=2; index<CURRENT; ++index )); do
        word=$words[index]
        case "$word" in
        {terminal_pattern}|{operation_pattern}) REPLY=$word; return 0 ;;
        esac
        _moguet_option_id "$word" && continue
        if [[ $word == -* ]]; then
            REPLY=__delegated__
            return 0
        fi
        REPLY=__invalid__
        return 0
    done
    return 1
}}

_moguet_has_operand() {{
    local expected_operation="$1" word index
    local seen_operation=false
    for (( index=2; index<CURRENT; ++index )); do
        word=$words[index]
        if [[ $seen_operation == false ]]; then
            [[ $word == "$expected_operation" ]] && seen_operation=true
            continue
        fi
        _moguet_option_id "$word" || return 0
    done
    return 1
}}

_moguet_is_assignment_operand() {{
    [[ $1 == *=* ]]
}}

_moguet_form_prefix_valid() {{
    local expected_operation="$1" form_index="$2" word actual operand_index
    local seen_operation=false
    local -a operands
    for (( operand_index=2; operand_index<CURRENT; ++operand_index )); do
        word=$words[operand_index]
        if [[ $seen_operation == false ]]; then
            [[ $word == "$expected_operation" ]] && seen_operation=true
            continue
        fi
        _moguet_option_id "$word" && continue
        operands+=("$word")
    done
    case "$expected_operation:$form_index" in
{zsh_form_prefix_cases(schema)}
        *) return 1 ;;
    esac
}}

_moguet_conflicts_with_present_option() {{
    case "$1" in
{conflict_cases}
        *) return 1 ;;
    esac
}}

_moguet_collect_candidates() {{
    local operation="$1"
    typeset -ga reply
    reply=()
    case "$operation" in
{chr(10).join(operation_cases)}
        __delegated__) reply=({zsh_case_values(delegated_tokens)}) ;;
    esac
{presentation_additions(schema, "zsh")}
}}

_moguet_description() {{
    REPLY=
    case "$1" in
{chr(10).join(description_cases)}
    esac
}}

{zsh_typed_functions(schema)}

_moguet() {{
    local operation candidate option_id cur=${{PREFIX-${{(Q)words[CURRENT]}}}}
    local -a words=("${{words[@]}}")
    local -a candidates filtered described
    case "$cur" in
    {'|'.join(shell_quote(o.completion_token) + '*' for o in finite_completion_options(schema)) or '__no_finite_value__'})
        # Applicability and agreement consume the same logical prior words.
        local index
        for ((index=2; index<CURRENT; ++index)); do
            words[index]=${{(Q)words[index]}}
        done
        ;;
    esac
    _moguet_find_operation
    operation=$REPLY

    if [[ -z $operation ]]; then
        candidates=({zsh_case_values(root_candidates)})
    else
        _moguet_collect_candidates "$operation"
        candidates=("${{reply[@]}}")
    fi

    _moguet_typed_values && return 0

    for candidate in "${{candidates[@]}}"; do
        if _moguet_option_id "$candidate"; then
            option_id=$REPLY
            case "$option_id" in
            {'|'.join(str(identity) for identity in once_ids)})
                _moguet_has_option_id "$option_id" && continue
                ;;
            esac
            _moguet_conflicts_with_present_option "$option_id" && continue
        fi
        filtered+=("$candidate")
    done

    for candidate in "${{filtered[@]}}"; do
        _moguet_description "$candidate"
        described+=("$candidate:$REPLY")
    done
    _describe -t moguet-values 'moguet value' described
    # Package tokens use compadd directly, without description delimiters.
    if (( CURRENT == 3 && ${{#words}} == 3 )) && [[ $words[CURRENT] != -* ]]; then
        case "$words[2]" in
        {'|'.join(local_prefix_operations(schema)) or '__no_local_prefix_context__'})
            local prefix=${{PREFIX-${{(Q)words[CURRENT]}}}}
            [[ $prefix != -* ]] && _moguet_repository_packages "$prefix"
            ;;
        esac
    fi
    return 0
}}

_moguet_repository_packages() {{
    emulate -L zsh
    local prefix="$1" helper={prefix_helper_binding(prefix_helper, 'zsh')} output candidate previous=
    local LC_ALL=C
    local -a packages
    [[ $helper == /* && -x $helper ]] || return 0
    output="$("$helper" "$prefix" 2>/dev/null && printf '\\001')" || return 0
    output=${{output%$'\\001'}}
    (( ${{#output}} <= 65536 )) || return 0
    [[ $output != *$'\\0'* ]] || return 0
    [[ -z $output ]] && return 0
    [[ $output == *$'\\n' ]] || return 0
    packages=("${{(@f)${{output%$'\\n'}}}}")
    (( ${{#packages}} <= 256 )) || return 0
    for candidate in "${{packages[@]}}"; do
        [[ $candidate =~ '^[A-Za-z0-9@._+][A-Za-z0-9@._+-]*$' &&
           $candidate != . && $candidate != .. &&
           $candidate == "$prefix"* ]] || return 0
        [[ -z $previous || $candidate > $previous ]] || return 0
        previous=$candidate
    done
    compadd -a packages
    return 0
}}

compdef _moguet moguet
"""


def fish_quote(value: str) -> str:
    return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"


def fish_contains(ids: tuple[int, ...]) -> str:
    if not ids:
        return "return 1"
    return (
        "contains -- $option_id "
        + " ".join(str(identity) for identity in ids)
        + "; and return 0; or return 1"
    )


def render_fish(schema: CliSchema, descriptions: Descriptions, locale: str,
                prefix_helper: str = "@MOGUET_REPOSITORY_PREFIX_HELPER@") -> str:
    operations = tuple(operation.token for operation in schema.operations)
    root_ids = tuple(
        dict.fromkeys(
            option.identity
            for option in schema.options
            if option.is_completion_visible
            and option.placement in {"parser-global", "first-non-global"}
        )
    )
    once_ids = tuple(
        dict.fromkeys(
            option.identity
            for option in schema.options
            if option.is_completion_visible and option.occurrence == "once"
        )
    )
    option_id_cases = "\n".join(
        "        case "
        + " ".join(fish_quote(pattern) for pattern in patterns)
        + f"\n            echo {identity}\n            return 0"
        for identity, patterns in option_case_patterns(schema)
    )
    terminal_cases = " ".join(fish_quote(token) for token in schema.terminal_tokens)
    operation_cases = " ".join(fish_quote(token) for token in operations)

    allow_cases: list[str] = []
    delegated_ids = tuple(
        identity
        for identity in schema.delegated_option_ids
        if any(
            option.identity == identity and option.is_completion_visible
            for option in schema.options
        )
    )
    for operation in schema.operations:
        if len(operation.forms) > 1:
            selected = next(form for form in operation.forms if form.selector_ids)
            default = next(form for form in operation.forms if not form.selector_ids)
            selected_index = operation.forms.index(selected)
            default_index = operation.forms.index(default)
            selector_test = "\n".join(
                f"            __moguet_has_option_id {identity}; and set selected true"
                for identity in selected.selector_ids
            )
            selected_ids = completion_ids_for_form(schema, selected)
            default_ids = completion_ids_for_form(schema, default)
            union_ids = union_completion_form_ids(
                schema, operation.forms
            )
            if operation.open_grammar:
                operand_branch = (
                    f"            else if __moguet_has_operand {fish_quote(operation.token)}\n"
                    f"                {fish_contains(delegated_ids)}\n"
                )
            else:
                operand_branch = (
                    f"            else if __moguet_has_operand {fish_quote(operation.token)}\n"
                    f"                __moguet_form_prefix_valid {fish_quote(operation.token)} {default_index}; or return 1\n"
                    f"                {fish_contains(default_ids)}\n"
                )
            allow_cases.append(
                f"        case {fish_quote(operation.token)}\n"
                f"            set -l selected false\n"
                f"{selector_test}\n"
                f"            if test $selected = true\n"
                f"                __moguet_form_prefix_valid {fish_quote(operation.token)} {selected_index}; or return 1\n"
                f"                {fish_contains(selected_ids)}\n"
                f"{operand_branch}"
                f"            else\n"
                f"                {fish_contains(union_ids)}\n"
                f"            end"
            )
        elif operation.forms and operation.open_grammar:
            form = operation.forms[0]
            selector_test = "\n".join(
                f"            __moguet_has_option_id {identity}; and set selected true"
                for identity in form.selector_ids
            )
            preselection_ids = tuple(
                dict.fromkeys(delegated_ids + form.selector_ids)
            )
            allow_cases.append(
                f"        case {fish_quote(operation.token)}\n"
                f"            set -l selected false\n"
                f"{selector_test}\n"
                f"            if test $selected = true\n"
                f"                __moguet_form_prefix_valid {fish_quote(operation.token)} 0; or return 1\n"
                f"                {fish_contains(completion_ids_for_form(schema, form))}\n"
                f"            else\n"
                f"                {fish_contains(preselection_ids)}\n"
                f"            end"
            )
        elif operation.forms:
            form = operation.forms[0]
            allow_cases.append(
                f"        case {fish_quote(operation.token)}\n"
                f"            __moguet_form_prefix_valid {fish_quote(operation.token)} 0; or return 1\n"
                f"            {fish_contains(completion_ids_for_form(schema, form))}"
            )
        else:
            allow_cases.append(
                f"        case {fish_quote(operation.token)}\n"
                f"            {fish_contains(delegated_ids)}"
            )

    conflict_cases = "\n".join(
        f"        case {option.identity}\n"
        + "\n".join(
            f"            __moguet_has_option_id {conflict}; and return 1"
            for conflict in completion_conflicts(schema, option)
        )
        for option in schema.options
        if completion_conflicts(schema, option)
    )
    canonical_comments = "\n".join(
        f"#   {syntax}" for syntax in schema.canonical_grammar
    )

    lines = [
        "# Generated by scripts/generate_completions.py; do not edit.",
        f"# Description locale: {locale}",
        "# Canonical closed grammar (projected from source/cli_authority.hpp):",
        canonical_comments,
        "",
        "function __moguet_option_id --argument-names word",
        "    switch $word",
        option_id_cases,
        "    end",
        "    return 1",
        "end",
        "",
        "function __moguet_has_option_id --argument-names expected",
        "    for word in (commandline -opc)[2..-1]",
        "        set -l actual (__moguet_option_id $word)",
        "        test \"$actual\" = \"$expected\"; and return 0",
        "    end",
        "    return 1",
        "end",
        "",
        "function __moguet_operation",
        "    for word in (commandline -opc)[2..-1]",
        "        switch $word",
        f"        case {terminal_cases} {operation_cases}",
        "            echo $word",
        "            return 0",
        "        end",
        "        __moguet_option_id $word >/dev/null; and continue",
        "        string match -q -- '-*' $word; and echo __delegated__; and return 0",
        "        echo __invalid__",
        "        return 0",
        "    end",
        "    return 1",
        "end",
        "",
        "function __moguet_has_operand --argument-names expected_operation",
        "    set -l seen_operation false",
        "    for word in (commandline -opc)[2..-1]",
        "        if test $seen_operation = false",
        "            test \"$word\" = \"$expected_operation\"; and set seen_operation true",
        "            continue",
        "        end",
        "        __moguet_option_id $word >/dev/null; or return 0",
        "    end",
        "    return 1",
        "end",
        "",
        "function __moguet_is_assignment_operand --argument-names word",
        "    string match -q -- '*=*' \"$word\"",
        "end",
        "",
        "function __moguet_form_prefix_valid --argument-names expected_operation form_index",
        "    set -l seen_operation false",
        "    set -l operands",
        "    for word in (commandline -opc)[2..-1]",
        "        if test $seen_operation = false",
        "            test \"$word\" = \"$expected_operation\"; and set seen_operation true",
        "            continue",
        "        end",
        "        __moguet_option_id $word >/dev/null; and continue",
        "        set -a operands \"$word\"",
        "    end",
        "    switch \"$expected_operation:$form_index\"",
        *fish_form_prefix_cases(schema),
        "    end",
        "    return 1",
        "end",
        "",
        "function __moguet_operation_allows --argument-names option_id",
        "    set -l operation (__moguet_operation)",
        presentation_additions(schema, "fish"),
        "    if test -z \"$operation\"",
        f"        {fish_contains(root_ids)}",
        "    end",
        "    switch $operation",
        *allow_cases,
        "        case __delegated__",
        f"            {fish_contains(delegated_ids)}",
        "    end",
        "    return 1",
        "end",
        "",
        "function __moguet_candidate_available --argument-names option_id",
        "    if contains -- $option_id " + " ".join(str(o.identity) for o in finite_completion_options(schema)),
        "        switch \"$option_id:\"(__moguet_value_word)",
        "            case " + " ".join(fish_quote(str(o.identity) + ':' + o.completion_token + '*') for o in finite_completion_options(schema)),
        "                return 1",
        "        end",
        "    end",
        "    __moguet_operation_allows $option_id; or return 1",
        f"    contains -- $option_id {' '.join(str(identity) for identity in once_ids)}; and __moguet_has_option_id $option_id; and return 1",
        "    switch $option_id",
        conflict_cases,
        "    end",
        "    return 0",
        "end",
        "",
        "function __moguet_no_operation",
        "    not __moguet_operation >/dev/null",
        "end",
        "",
    ]
    for operation in schema.operations:
        lines.append(
            "complete -c moguet -f -n '__moguet_no_operation' -a "
            f"{fish_quote(operation.token)} -d "
            f"{fish_quote(descriptions.operations[operation.token])}"
        )
    for option in schema.options:
        if not option.is_completion_visible:
            continue
        lines.append(
            "complete -c moguet -f -n "
            f"{fish_quote(f'__moguet_candidate_available {option.identity}')} "
            f"-a {fish_quote(option.completion_token)} -d "
            f"{fish_quote(descriptions.options[option.token])}"
        )
    lines.extend([
        "",
        "function __moguet_repository_packages",
        "    set -l before (commandline -opc)",
        "    test (count $before) -eq 2; or return 0",
        "    test (count (commandline -op)) -le 3; or return 0",
        "    switch $before[2]",
        "        case " + " ".join(fish_quote(t) for t in local_prefix_operations(schema)),
        "        case '*'",
        "            return 0",
        "    end",
        "    set -l raw (commandline -ct | string collect -a)",
        "    set -l prefix (string unescape -- \"$raw\")",
        "    if test $status -ne 0",
        "        switch (string sub -l 1 -- \"$raw\")",
        '''            case "'"''',
        '''                set prefix (string unescape -- "$raw'")''',
        "            case '\"'",
        '''                set prefix (string unescape -- "$raw\\\"")''',
        "            case '*'",
        "                return 0",
        "        end",
        "    end",
        "    test (count $prefix) -eq 1; or return 0",
        "    string match -q -- '-*' \"$prefix\"; and return 0",
        f"    set -l helper {prefix_helper_binding(prefix_helper, 'fish')}",
        "    string match -q -- '/*' \"$helper\"; or return 0",
        "    test -x \"$helper\"; or return 0",
        # split0 preserves newline bytes and exposes every NUL as a separate
        # item. A success sentinel makes even a trailing NUL observable.
        "    set -l output (begin; \"$helper\" \"$prefix\" 2>/dev/null; and printf '\\x01'; end | string split0)",
        "    test $pipestatus[1] -eq 0; or return 0",
        "    test (count $output) -eq 1; or return 0",
        "    test (string length -- \"$output\") -le 65537; or return 0",
        "    string match -qr -- '\\n\\x01$' \"$output\"; or return 0",
        "    set -l packages (string split \\n -- \"$output\")",
        "    set -e packages[-1]",
        "    test (count $packages) -le 256; or return 0",
        "    for candidate in $packages",
        "        string match -qr -- '^[A-Za-z0-9@._+][A-Za-z0-9@._+-]*$' \"$candidate\"; or return 0",
        "        contains -- \"$candidate\" . ..; and return 0",
        "        test (string sub -l (string length -- \"$prefix\") -- \"$candidate\") = \"$prefix\"; or return 0",
        "    end",
        # Fish has no bytewise string ordering operator. Check, never sort or
        # transform, the complete protocol once using the Arch coreutils tool.
        "    printf '%s\\n' $packages | LC_ALL=C /usr/bin/sort -cu 2>/dev/null; or return 0",
        "    printf '%s\\n' $packages",
        "end",
        "",
        # Exactly one dynamic argument producer; conditions stay static.
        "complete -c moguet -f -a '(__moguet_repository_packages)'",
    ])
    lines.extend(fish_typed_functions(schema))
    return "\n".join(lines) + "\n"


def generated_files(
    schema: CliSchema, descriptions: Descriptions, locale: str, output_dir: Path,
    prefix_helper: str = "@MOGUET_REPOSITORY_PREFIX_HELPER@",
) -> dict[Path, str]:
    renderers: tuple[tuple[str, Callable[[CliSchema, Descriptions, str, str], str]], ...] = (
        ("moguet.bash", render_bash),
        ("_moguet", render_zsh),
        ("moguet.fish", render_fish),
    )
    return {
        output_dir / filename: renderer(schema, descriptions, locale, prefix_helper)
        for filename, renderer in renderers
    }


def check_generated(path: Path, expected: str) -> bool:
    try:
        actual = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        print(f"completion-generator: missing generated file: {path}", file=sys.stderr)
        return False
    if actual == expected:
        return True

    print(f"completion-generator: generated file is stale: {path}", file=sys.stderr)
    diff = difflib.unified_diff(
        actual.splitlines(),
        expected.splitlines(),
        fromfile=str(path),
        tofile=f"generated:{path.name}",
        lineterm="",
    )
    for line in diff:
        print(line, file=sys.stderr)
    return False


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate Moguet shell completions from the shared CLI authority."
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--check", action="store_true", help="fail if tracked output differs"
    )
    mode.add_argument(
        "--render",
        choices=("bash", "zsh", "fish"),
        help="render one completion to stdout without writing tracked files",
    )
    parser.add_argument("--locale", default="en", help="description locale (default: en)")
    parser.add_argument("--repository-prefix-helper",
                        default="@MOGUET_REPOSITORY_PREFIX_HELPER@",
                        help="resolved absolute helper path (CMake install authority)")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=REPOSITORY_ROOT / "completions",
        help="destination directory (default: repository completions directory)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    output_dir = arguments.output_dir
    if not output_dir.is_absolute():
        output_dir = Path.cwd() / output_dir

    schema = load_schema()
    descriptions = load_descriptions(schema, arguments.locale)
    helper = arguments.repository_prefix_helper
    if helper != "@MOGUET_REPOSITORY_PREFIX_HELPER@" and (
        not Path(helper).is_absolute() or any(ch in helper for ch in '\0\n\r')
    ):
        fail("repository prefix helper must be an absolute single-line path")
    outputs = generated_files(schema, descriptions, arguments.locale, output_dir, helper)

    if arguments.check:
        return 0 if all(
            check_generated(path, content) for path, content in outputs.items()
        ) else 1

    output_names = {
        "bash": "moguet.bash",
        "zsh": "_moguet",
        "fish": "moguet.fish",
    }
    sys.stdout.write(outputs[output_dir / output_names[arguments.render]])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
