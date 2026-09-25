#!/usr/bin/env python3

from __future__ import annotations

from contextlib import redirect_stderr
import io
from pathlib import Path
import sys


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPOSITORY_ROOT / "scripts"))

from generate_completions import export_authority, parse_exported_schema  # noqa: E402


def fail(message: str) -> None:
    print(f"completion-schema-validator-test: {message}", file=sys.stderr)
    raise SystemExit(1)


def option_record(
    identity: int = 0,
    token: str = "--help",
    completion_token: str | None = None,
    occurrence: str = "once",
    placement: str = "parser-global",
    value_kind: str = "none",
    allowed_values: str = "",
    conflict_rule: str = "none",
    conflicts: str = "",
    conflict_value_identity: str = "",
    semantic_scopes: str = "information",
    ownership: str = "moguet-owned",
    definition_role: str = "definition",
    completion_visibility: str = "suggested-and-described",
) -> str:
    return "\t".join(
        (
            "OPTION",
            str(identity),
            token,
            completion_token if completion_token is not None else token,
            occurrence,
            placement,
            value_kind,
            allowed_values,
            conflict_rule,
            conflicts,
            conflict_value_identity,
            semantic_scopes,
            ownership,
            definition_role,
            completion_visibility,
        )
    )


DELEGATED_NEEDED_RELATION = (
    "0:optional:delegated:hidden:upstream-argument:pacman:preserve-all"
)
DELEGATED_END_OF_OPTIONS_RELATION = (
    "2:optional:once:hidden:upstream-argument+parser-boundary:"
    "pacman:preserve-all"
)
DELEGATED_NO_CONFIRM_RELATION = (
    "1:optional:repeat-idempotent:hidden:moguet-control+upstream-argument:"
    "pacman:consolidate-single"
)


def delegated_options(*, end_ownership: str = "intercepted-pacman") -> tuple[str, ...]:
    return (
        option_record(
            identity=0,
            token="--needed",
            occurrence="delegated",
            placement="pacman-grammar",
            semantic_scopes=(
                "root-package-selection+final-package-install+pacman-delegation"
            ),
            ownership="intercepted-pacman",
        ),
        option_record(
            identity=1,
            token="--noconfirm",
            occurrence="repeat-idempotent",
            semantic_scopes=(
                "source-build+root-package-selection+pacman-delegation"
            ),
        ),
        option_record(
            identity=2,
            token="--",
            occurrence="once",
            placement="end-of-options",
            value_kind="marker",
            semantic_scopes="pacman-delegation+parser-boundary",
            ownership=end_ownership,
            definition_role="schema-only",
            completion_visibility="hidden",
        ),
    )


def delegated_schema(
    *,
    end_relation: str = DELEGATED_END_OF_OPTIONS_RELATION,
    end_ownership: str = "intercepted-pacman",
) -> str:
    relations = ",".join(
        (
            DELEGATED_NEEDED_RELATION,
            end_relation,
            DELEGATED_NO_CONFIRM_RELATION,
        )
        if end_relation
        else (
            DELEGATED_NEEDED_RELATION,
            DELEGATED_NO_CONFIRM_RELATION,
        )
    )
    return exported_schema(
        open_grammar=True,
        options=delegated_options(end_ownership=end_ownership),
        delegated_relations=relations,
    )


def recursive_public_schema(syntax: str) -> str:
    return exported_schema(
        target_policy="one-or-more",
        operand_ordering="preserve-input-order",
        operand_terms="package:1:*",
        syntax=syntax,
        relations=(
            "0:optional:repeat-idempotent:optional:"
            "moguet-control:none:none"
        ),
        options=(
            option_record(
                token="--recursive",
                occurrence="repeat-idempotent",
                placement="operation-local",
                semantic_scopes="dependency-inspection",
                definition_role="syntax-only",
            ),
        ),
    )


def exported_schema(
    *,
    target_policy: str = "none",
    operand_ordering: str = "none",
    operand_terms: str = "",
    open_grammar: bool = False,
    syntax: str = "fixture <operand>",
    relations: str = "",
    options: tuple[str, ...] | None = None,
    delegated_relations: str = "",
    delegated_tail_policy: str = "none",
    open_operation_token: str = "delegated-example",
    canonical: str | None = None,
) -> str:
    lines = [*(options if options is not None else (option_record(),))]
    lines.extend(
        (
            "OPERATION\tfixture\tclosed",
            "\t".join(
                (
                    "FORM",
                    "fixture",
                    syntax,
                    target_policy,
                    operand_ordering,
                    operand_terms,
                    relations,
                    delegated_tail_policy,
                )
            )
        )
    )
    if open_grammar:
        lines.extend(
            (
                f"OPERATION\t{open_operation_token}\topen",
                "\t".join(
                    (
                        "DELEGATED",
                        "delegated",
                        "delegated",
                        "delegated-pacman-argument:0:*",
                        delegated_relations,
                    )
                ),
            )
        )
    lines.append(f"CANONICAL\t{canonical if canonical is not None else syntax}")
    return "\n".join(lines) + "\n"


def without_record(schema: str, record: str) -> str:
    prefix = record + "\t"
    return "\n".join(
        line for line in schema.splitlines() if not line.startswith(prefix)
    ) + "\n"


def duplicate_first_record(schema: str, record: str) -> str:
    lines = schema.splitlines()
    prefix = record + "\t"
    for index, line in enumerate(lines):
        if line.startswith(prefix):
            lines.insert(index + 1, line)
            return "\n".join(lines) + "\n"
    fail(f"fixture record was not found: {record}")


def expect_accepted(label: str, schema: str) -> None:
    try:
        parse_exported_schema(schema)
    except SystemExit as error:
        fail(f"{label} unexpectedly failed with status {error.code!r}")
    print(f"  ok: accepted {label}")


def expect_rejected(label: str, schema: str, expected_diagnostic: str) -> None:
    diagnostic = io.StringIO()
    try:
        with redirect_stderr(diagnostic):
            parse_exported_schema(schema)
    except SystemExit as error:
        output = diagnostic.getvalue()
        if error.code == 1 and expected_diagnostic in output:
            print(f"  ok: rejected {label}")
            return
        fail(
            f"{label} returned status {error.code!r} with unexpected diagnostic: "
            f"{output.strip()!r}"
        )
    fail(f"{label} unexpectedly passed")


def expect_current_authority_projection() -> None:
    schema = parse_exported_schema(export_authority())
    delegated_ids = set(schema.delegated_option_ids)
    delegated_tokens = {
        option.token
        for option in schema.options
        if option.identity in delegated_ids
    }
    required_tokens = {"--needed", "--noconfirm", "--"}
    if not required_tokens.issubset(delegated_tokens):
        fail(
            "current C++ delegated projection is missing: "
            + ", ".join(sorted(required_tokens - delegated_tokens))
        )
    if sum(option.token == "--" for option in schema.options) != 1:
        fail("current C++ authority did not project exactly one -- OPTION record")
    system_update = next(
        (
            operation
            for operation in schema.operations
            if operation.token == "-Syu"
        ),
        None,
    )
    if system_update is None or not system_update.open_grammar:
        fail("current C++ authority lacks the -Syu delegated fallback")
    if len(system_update.forms) != 2:
        fail("current C++ authority lacks both intercepted -Syu forms")
    automatic = next(
        (form for form in system_update.forms if not form.selector_ids),
        None,
    )
    repository_only = next(
        (form for form in system_update.forms if form.selector_ids),
        None,
    )
    if (
        automatic is None
        or repository_only is None
        or automatic.delegated_tail_policy != "none"
        or repository_only.delegated_tail_policy != "repository-only"
    ):
        fail("current C++ -Syu delegated-tail projection differs")
    option_by_identity = {option.identity: option for option in schema.options}
    repository_selectors = {
        option_by_identity[identity].token
        for identity in repository_only.selector_ids
    }
    automatic_tokens = {
        option_by_identity[identity].token
        for identity in automatic.option_ids
    }
    if repository_selectors != {"--repo"} or "--aur" in automatic_tokens:
        fail("current C++ -Syu selector projection differs")
    print("  ok: accepted current C++ delegated authority projection")


def main() -> int:
    positive_controls = (
        (
            "remote build primary plus repeated assignments",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="primary-then-environment-assignments",
                operand_terms="package:1:1,environment-assignment:0:*",
            ),
        ),
        (
            "local build primary plus repeated assignments",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="primary-then-environment-assignments",
                operand_terms="directory:1:1,environment-assignment:0:*",
            ),
        ),
        (
            "ordinary exactly-one form",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:1",
            ),
        ),
        (
            "one-or-more form",
            exported_schema(
                target_policy="one-or-more",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:*",
            ),
        ),
        (
            "ordered source-preference items",
            exported_schema(
                target_policy="ordered-items",
                operand_ordering="package-introduces-following-assignment-scope",
                operand_terms="source-preference-item:1:*",
            ),
        ),
        (
            "targetless form",
            exported_schema(
                target_policy="none",
                operand_ordering="none",
            ),
        ),
        (
            "delegated open grammar without a closed form",
            exported_schema(open_grammar=True),
        ),
        (
            "delegated open option projection",
            exported_schema(
                open_grammar=True,
                delegated_relations=(
                    "0:optional:delegated:hidden:upstream-argument:"
                    "pacman:preserve-all"
                ),
            ),
        ),
        (
            "delegated end-of-options authority projection",
            delegated_schema(),
        ),
        (
            "repository-only delegated tail projection",
            exported_schema(
                open_grammar=True,
                open_operation_token="fixture",
                syntax="fixture --repo",
                relations=(
                    "3:required:repeat-idempotent:required:"
                    "moguet-control:none:none"
                ),
                options=delegated_options()
                + (
                    option_record(
                        identity=3,
                        token="--repo",
                        occurrence="repeat-idempotent",
                        semantic_scopes="source-selection",
                        definition_role="syntax-only",
                    ),
                ),
                delegated_relations=",".join(
                    (
                        DELEGATED_NEEDED_RELATION,
                        DELEGATED_END_OF_OPTIONS_RELATION,
                        DELEGATED_NO_CONFIRM_RELATION,
                    )
                ),
                delegated_tail_policy="repository-only",
            ),
        ),
        (
            "attached enum option semantics",
            exported_schema(
                options=(
                    option_record(
                        token="--mode",
                        completion_token="--mode=",
                        occurrence="repeat-same-value",
                        value_kind="attached-enum",
                        allowed_values="normal,rebuild,clean",
                        semantic_scopes="source-build",
                    ),
                )
            ),
        ),
        (
            "attached free-form value semantics",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:1",
                syntax="fixture <operand> [--output-dir=DIR]",
                relations=(
                    "0:optional:once:optional:moguet-control:none:none"
                ),
                options=(
                    option_record(
                        token="--output-dir",
                        completion_token="--output-dir=",
                        occurrence="once",
                        placement="operation-local",
                        value_kind="attached-value",
                        allowed_values="DIR",
                        semantic_scopes="package-export",
                    ),
                ),
            ),
        ),
        (
            "public optional operation-local syntax",
            exported_schema(
                target_policy="one-or-more",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:*",
                syntax="fixture [--recursive] <operand>",
                relations=(
                    "0:optional:repeat-idempotent:optional:"
                    "moguet-control:none:none"
                ),
                options=(
                    option_record(
                        token="--recursive",
                        occurrence="repeat-idempotent",
                        placement="operation-local",
                        semantic_scopes="dependency-inspection",
                        definition_role="syntax-only",
                    ),
                ),
            ),
        ),
        (
            "public optional syntax in an alternative",
            exported_schema(
                target_policy="one-or-more",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:*",
                syntax="fixture [--recursive|--shallow] <operand>",
                relations=(
                    "0:optional:repeat-idempotent:optional:"
                    "moguet-control:none:none"
                ),
                options=(
                    option_record(
                        token="--recursive",
                        occurrence="repeat-idempotent",
                        placement="operation-local",
                        semantic_scopes="dependency-inspection",
                        definition_role="syntax-only",
                    ),
                ),
            ),
        ),
        (
            "public required operation-local syntax",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="preserve-input-order",
                operand_terms="directory:1:1",
                syntax="fixture --local <operand>",
                relations=(
                    "0:required:once:required:moguet-control:none:none"
                ),
                options=(
                    option_record(
                        token="--local",
                        placement="operation-local",
                        semantic_scopes="local-source-build",
                        definition_role="syntax-only",
                    ),
                ),
            ),
        ),
        (
            "public attached-value syntax",
            exported_schema(
                target_policy="one-or-more",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:*",
                syntax="fixture [--mode=<mode>] <operand>",
                relations=(
                    "0:optional:repeat-same-value:optional:"
                    "moguet-control:none:none"
                ),
                options=(
                    option_record(
                        token="--mode",
                        completion_token="--mode=",
                        occurrence="repeat-same-value",
                        placement="operation-local",
                        value_kind="attached-enum",
                        allowed_values="normal,rebuild,clean",
                        semantic_scopes="source-build",
                        definition_role="syntax-only",
                    ),
                ),
            ),
        ),
        (
            "local selector excludes a global final-value option",
            exported_schema(options=(
                option_record(identity=0, token="--local-choice", placement="operation-local",
                              conflict_rule="operation-local-exclusion", conflicts="1"),
                option_record(identity=1, token="--left", conflict_rule="final-value-must-agree",
                              conflicts="2", conflict_value_identity="fixture.choice"),
                option_record(identity=2, token="--right", conflict_rule="final-value-must-agree",
                              conflicts="1", conflict_value_identity="fixture.choice"),
            )),
        ),
        (
            "symmetric final-value conflicts",
            exported_schema(
                options=(
                    option_record(
                        identity=0,
                        token="--left",
                        occurrence="repeat-idempotent",
                        conflict_rule="final-value-must-agree",
                        conflicts="1",
                        conflict_value_identity="fixture.choice",
                    ),
                    option_record(
                        identity=1,
                        token="--right",
                        occurrence="repeat-idempotent",
                        conflict_rule="final-value-must-agree",
                        conflicts="0",
                        conflict_value_identity="fixture.choice",
                    ),
                )
            ),
        ),
    )
    for label, schema in positive_controls:
        expect_accepted(label, schema)
    expect_current_authority_projection()

    rejected_controls = (
        (
            "global option cannot own local exclusion",
            exported_schema(options=(
                option_record(identity=0, conflict_rule="operation-local-exclusion", conflicts="1"),
                option_record(identity=1, token="--other"),
            )),
            "local exclusion requires operation-local placement",
        ),
        (
            "unknown operand kind",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="preserve-input-order",
                operand_terms="unknown-kind:1:1",
            ),
            "unsupported operand kind projection",
        ),
        (
            "exactly-one with unbounded maximum",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:*",
            ),
            "invalid exactly-one operand projection",
        ),
        (
            "one-or-more with finite maximum",
            exported_schema(
                target_policy="one-or-more",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:1",
            ),
            "invalid one-or-more operand projection",
        ),
        (
            "ordered-items with wrong kind",
            exported_schema(
                target_policy="ordered-items",
                operand_ordering="package-introduces-following-assignment-scope",
                operand_terms="package:1:*",
            ),
            "invalid ordered-items operand projection",
        ),
        (
            "missing operand term",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="preserve-input-order",
            ),
            "invalid exactly-one operand projection",
        ),
        (
            "unknown target policy",
            exported_schema(
                target_policy="unknown-policy",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:1",
            ),
            "unsupported target policy projection",
        ),
        (
            "unknown operand ordering",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="unknown-ordering",
                operand_terms="package:1:1",
            ),
            "unsupported operand ordering projection",
        ),
        (
            "targetless form with an operand",
            exported_schema(
                target_policy="none",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:1",
            ),
            "invalid targetless operand projection",
        ),
        (
            "closed form with delegated operands",
            exported_schema(
                target_policy="delegated",
                operand_ordering="delegated",
                operand_terms="delegated-pacman-argument:0:*",
            ),
            "invalid delegated operand projection",
        ),
        (
            "unknown delegated tail policy",
            exported_schema(delegated_tail_policy="unknown-tail"),
            "unsupported delegated tail policy projection",
        ),
        (
            "repository-only tail without open fallback",
            exported_schema(delegated_tail_policy="repository-only"),
            "repository-only delegated tail has no open pacman fallback",
        ),
        (
            "repository-only tail without required selector",
            exported_schema(
                open_grammar=True,
                open_operation_token="fixture",
                delegated_tail_policy="repository-only",
            ),
            "repository-only delegated tail lacks required --repo selector",
        ),
        (
            "repository-only tail forwarding selector",
            exported_schema(
                open_grammar=True,
                open_operation_token="fixture",
                syntax="fixture --repo",
                relations=(
                    "3:required:repeat-idempotent:required:"
                    "moguet-control+upstream-argument:pacman:preserve-all"
                ),
                options=delegated_options()
                + (
                    option_record(
                        identity=3,
                        token="--repo",
                        occurrence="repeat-idempotent",
                        semantic_scopes="source-selection",
                        definition_role="syntax-only",
                    ),
                ),
                delegated_relations=",".join(
                    (
                        DELEGATED_NEEDED_RELATION,
                        DELEGATED_END_OF_OPTIONS_RELATION,
                        DELEGATED_NO_CONFIRM_RELATION,
                    )
                ),
                delegated_tail_policy="repository-only",
            ),
            "repository-only delegated tail forwards or misclassifies --repo",
        ),
        (
            "unknown option occurrence",
            exported_schema(
                options=(option_record(occurrence="unknown-occurrence"),)
            ),
            "unsupported option occurrence projection",
        ),
        (
            "unknown option placement",
            exported_schema(
                options=(option_record(placement="unknown-placement"),)
            ),
            "unsupported option placement projection",
        ),
        (
            "unknown option value kind",
            exported_schema(
                options=(option_record(value_kind="unknown-value"),)
            ),
            "unsupported option value projection",
        ),
        (
            "unknown option conflict rule",
            exported_schema(
                options=(option_record(conflict_rule="unknown-conflict"),)
            ),
            "unsupported option conflict projection",
        ),
        (
            "unknown option semantic scope",
            exported_schema(
                options=(option_record(semantic_scopes="unknown-scope"),)
            ),
            "unsupported option semantic scope projection",
        ),
        (
            "unknown option ownership",
            exported_schema(
                options=(option_record(ownership="unknown-owner"),)
            ),
            "unsupported option ownership projection",
        ),
        (
            "unknown option definition role",
            exported_schema(
                options=(option_record(definition_role="unknown-role"),)
            ),
            "unsupported option definition role projection",
        ),
        (
            "unknown option completion visibility",
            exported_schema(
                options=(option_record(completion_visibility="unknown"),)
            ),
            "unsupported option completion visibility projection",
        ),
        (
            "consecutive allowed-value delimiter",
            exported_schema(
                options=(
                    option_record(
                        token="--mode",
                        completion_token="--mode=",
                        occurrence="repeat-same-value",
                        value_kind="attached-enum",
                        allowed_values="normal,,clean",
                        semantic_scopes="source-build",
                    ),
                )
            ),
            "empty allowed value list element",
        ),
        (
            "leading allowed-value delimiter",
            exported_schema(
                options=(
                    option_record(
                        token="--mode",
                        completion_token="--mode=",
                        occurrence="repeat-same-value",
                        value_kind="attached-enum",
                        allowed_values=",normal",
                        semantic_scopes="source-build",
                    ),
                )
            ),
            "empty allowed value list element",
        ),
        (
            "trailing allowed-value delimiter",
            exported_schema(
                options=(
                    option_record(
                        token="--mode",
                        completion_token="--mode=",
                        occurrence="repeat-same-value",
                        value_kind="attached-enum",
                        allowed_values="normal,",
                        semantic_scopes="source-build",
                    ),
                )
            ),
            "empty allowed value list element",
        ),
        (
            "empty conflict-list element",
            exported_schema(
                options=(option_record(conflicts="1,,2"),)
            ),
            "empty option conflict list element",
        ),
        (
            "empty relation-list element",
            exported_schema(
                relations=(
                    "0:optional:once:hidden:moguet-control:none:none,,"
                    "0:optional:once:hidden:moguet-control:none:none"
                )
            ),
            "empty option relation list element",
        ),
        (
            "empty operand-list element",
            exported_schema(
                target_policy="exactly-one",
                operand_ordering="preserve-input-order",
                operand_terms="package:1:1,",
            ),
            "empty operand term list element",
        ),
        (
            "asymmetric option conflict",
            exported_schema(
                options=(
                    option_record(
                        identity=0,
                        token="--left",
                        conflict_rule="mutually-exclusive",
                        conflicts="1",
                    ),
                    option_record(identity=1, token="--right"),
                )
            ),
            "asymmetric option conflict projection",
        ),
        (
            "unknown option conflict identity",
            exported_schema(
                options=(
                    option_record(
                        token="--left",
                        conflict_rule="mutually-exclusive",
                        conflicts="9",
                    ),
                )
            ),
            "references unknown conflict identity",
        ),
        (
            "unknown relation option",
            exported_schema(
                relations="1:optional:once:hidden:moguet-control:none:none"
            ),
            "references unknown option identity",
        ),
        (
            "duplicate relation option",
            exported_schema(
                relations=(
                    "0:optional:once:hidden:moguet-control:none:none,"
                    "0:optional:once:hidden:moguet-control:none:none"
                )
            ),
            "duplicate option relation projection",
        ),
        (
            "unknown relation requirement",
            exported_schema(
                relations="0:unknown:once:hidden:moguet-control:none:none"
            ),
            "unsupported option requirement projection",
        ),
        (
            "unknown relation occurrence",
            exported_schema(
                relations="0:optional:unknown:hidden:moguet-control:none:none"
            ),
            "unsupported relation occurrence projection",
        ),
        (
            "delegated relation occurrence in closed form",
            exported_schema(
                relations="0:optional:delegated:hidden:moguet-control:none:none"
            ),
            "delegated option occurrence in closed form",
        ),
        (
            "unknown public syntax",
            exported_schema(
                relations="0:optional:once:unknown:moguet-control:none:none"
            ),
            "unsupported public syntax projection",
        ),
        (
            "unknown semantic effect",
            exported_schema(
                relations="0:optional:once:hidden:unknown:none:none"
            ),
            "unsupported semantic effect projection",
        ),
        (
            "unknown forwarding target",
            exported_schema(
                relations="0:optional:once:hidden:moguet-control:unknown:none"
            ),
            "unsupported forwarding target projection",
        ),
        (
            "unknown forwarding occurrence",
            exported_schema(
                relations="0:optional:once:hidden:moguet-control:none:unknown"
            ),
            "unsupported forwarding occurrence projection",
        ),
        (
            "inconsistent forwarding occurrence",
            exported_schema(
                relations="0:optional:once:hidden:upstream-argument:pacman:none"
            ),
            "inconsistent option forwarding projection",
        ),
        (
            "delegated end-of-options relation missing",
            delegated_schema(end_relation=""),
            "delegated end-of-options relation is absent",
        ),
        (
            "delegated end-of-options occurrence mismatch",
            delegated_schema(
                end_relation=(
                    "2:optional:repeat-idempotent:hidden:"
                    "upstream-argument+parser-boundary:pacman:preserve-all"
                )
            ),
            "inconsistent delegated end-of-options relation projection",
        ),
        (
            "delegated end-of-options forwarding mismatch",
            delegated_schema(
                end_relation=(
                    "2:optional:once:hidden:upstream-argument+parser-boundary:"
                    "pacman:consolidate-single"
                )
            ),
            "inconsistent delegated end-of-options relation projection",
        ),
        (
            "delegated end-of-options ownership mismatch",
            delegated_schema(end_ownership="moguet-owned"),
            "inconsistent end-of-options option projection",
        ),
        (
            "public syntax missing from rendered form",
            exported_schema(
                relations="0:required:once:required:moguet-control:none:none"
            ),
            "public option syntax is absent from form projection",
        ),
        (
            "public syntax prefix only",
            recursive_public_schema(
                "fixture [--recursive-extra] <operand>"
            ),
            "public option syntax is absent from form projection",
        ),
        (
            "public syntax embedded in another token",
            recursive_public_schema("fixture [foo--recursive] <operand>"),
            "public option syntax is absent from form projection",
        ),
        (
            "public syntax appears only in an option value",
            recursive_public_schema(
                "fixture [--mode=<--recursive>] <operand>"
            ),
            "public option syntax is absent from form projection",
        ),
        (
            "public syntax appears only in an attached value alternative",
            recursive_public_schema(
                "fixture [--mode=<normal|--recursive|clean>] <operand>"
            ),
            "public option syntax is absent from form projection",
        ),
        (
            "public syntax appears only in a metavariable alternative",
            recursive_public_schema(
                "fixture <foo|--recursive|bar> <operand>"
            ),
            "public option syntax is absent from form projection",
        ),
        (
            "public syntax is embedded only in an attached value alternative",
            recursive_public_schema(
                "fixture [--mode=<normal|foo--recursive|clean>] <operand>"
            ),
            "public option syntax is absent from form projection",
        ),
        (
            "canonical grammar mismatch",
            exported_schema(canonical="fixture changed"),
            "canonical grammar differs from closed form projection",
        ),
        (
            "duplicate closed grammar form",
            duplicate_first_record(exported_schema(), "FORM"),
            "duplicate closed grammar forms",
        ),
        (
            "open grammar without delegated projection",
            without_record(exported_schema(open_grammar=True), "DELEGATED"),
            "open grammar has no delegated operand projection",
        ),
        (
            "syntax-only option without public form",
            exported_schema(
                options=(
                    option_record(
                        token="--local",
                        placement="operation-local",
                        semantic_scopes="local-source-build",
                        definition_role="syntax-only",
                    ),
                )
            ),
            "syntax-only option has no public grammar form",
        ),
    )
    for label, schema, expected_diagnostic in rejected_controls:
        expect_rejected(label, schema, expected_diagnostic)

    print(
        "completion-schema-validator-test: "
        f"{len(positive_controls) + len(rejected_controls) + 1} "
        "scenarios passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
