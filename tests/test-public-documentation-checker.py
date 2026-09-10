#!/usr/bin/env python3

from __future__ import annotations

from contextlib import redirect_stderr
import io
from pathlib import Path
import re
import sys
import tempfile


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPOSITORY_ROOT / "scripts"))

from check_public_documentation import (  # noqa: E402
    assert_semantic_text_contract,
    check_release_notes_documentation,
    check_reviewed_source_documentation,
    check_system_aur_update_documentation,
    exact_man_public_surface,
    expected_surface,
    reviewed_source_documentation_contracts,
    reviewed_source_runtime_help_contracts,
    system_aur_update_documentation_contracts,
)
from generate_completions import load_schema  # noqa: E402


SYNC_SELECT_SYNTAX = "-S <pkg> | -S --select [--needed] <query>"


def fail(message: str) -> None:
    print(f"public-documentation-checker-test: {message}", file=sys.stderr)
    raise SystemExit(1)


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        fail(f"mutation source must occur exactly once: {old!r}")
    return text.replace(old, new, 1)


def remove_needed_definition(text: str) -> str:
    mutated, count = re.subn(
        r'\.TP\n\.B "--needed"\n.*?(?=\.TP\n)',
        "",
        text,
        count=1,
        flags=re.DOTALL,
    )
    if count != 1:
        fail("PUBLIC OPTIONS --needed definition was not found exactly once")
    return mutated


def expect_rejected(label: str, text: str, schema, expected) -> None:
    with tempfile.TemporaryDirectory(prefix="moguet-doc-checker-") as directory:
        path = Path(directory) / "moguet.1.in"
        path.write_text(text, encoding="utf-8")
        diagnostic = io.StringIO()
        try:
            with redirect_stderr(diagnostic):
                exact_man_public_surface(path, expected, schema)
        except SystemExit as error:
            if error.code == 1 and "public-documentation-check:" in diagnostic.getvalue():
                print(f"  ok: rejected {label}")
                return
            fail(f"{label} returned unexpected status {error.code!r}")
    fail(f"{label} unexpectedly passed")


def copy_reviewed_source_documentation_fixture(directory: str) -> Path:
    fixture_root = Path(directory)
    for source in reviewed_source_documentation_contracts(REPOSITORY_ROOT):
        relative = source.relative_to(REPOSITORY_ROOT)
        destination = fixture_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(
            source.read_text(encoding="utf-8"), encoding="utf-8"
        )
    return fixture_root


def expect_reviewed_source_documentation_rejected(
    label: str,
    relative_path: str,
    old: str,
    new: str,
) -> None:
    with tempfile.TemporaryDirectory(
        prefix="moguet-reviewed-doc-checker-"
    ) as directory:
        fixture_root = copy_reviewed_source_documentation_fixture(directory)
        path = fixture_root / relative_path
        path.write_text(
            replace_once(path.read_text(encoding="utf-8"), old, new),
            encoding="utf-8",
        )
        diagnostic = io.StringIO()
        try:
            with redirect_stderr(diagnostic):
                check_reviewed_source_documentation(fixture_root)
        except SystemExit as error:
            if (
                error.code == 1
                and "public-documentation-check:" in diagnostic.getvalue()
            ):
                print(f"  ok: rejected {label}")
                return
            fail(f"{label} returned unexpected status {error.code!r}")
    fail(f"{label} unexpectedly passed")


def copy_system_aur_update_documentation_fixture(directory: str) -> Path:
    fixture_root = Path(directory)
    for source in system_aur_update_documentation_contracts(REPOSITORY_ROOT):
        relative = source.relative_to(REPOSITORY_ROOT)
        destination = fixture_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(
            source.read_text(encoding="utf-8"), encoding="utf-8"
        )
    return fixture_root


def expect_system_aur_update_documentation_rejected(
    label: str,
    relative_path: str,
    old: str,
    new: str,
) -> None:
    with tempfile.TemporaryDirectory(
        prefix="moguet-system-aur-doc-checker-"
    ) as directory:
        fixture_root = copy_system_aur_update_documentation_fixture(directory)
        path = fixture_root / relative_path
        path.write_text(
            replace_once(path.read_text(encoding="utf-8"), old, new),
            encoding="utf-8",
        )
        diagnostic = io.StringIO()
        try:
            with redirect_stderr(diagnostic):
                check_system_aur_update_documentation(fixture_root)
        except SystemExit as error:
            if (
                error.code == 1
                and "public-documentation-check:" in diagnostic.getvalue()
            ):
                print(f"  ok: rejected {label}")
                return
            fail(f"{label} returned unexpected status {error.code!r}")
    fail(f"{label} unexpectedly passed")


def check_release_notes_regressions() -> int:
    # Independent historical fixture: these dates must not follow VERSION.
    historical = """# Moguet v2.6.0
## English
Moguet v2.6.0 includes a behavior-changing compatibility correction.
Before v2.6.0, -Syu was repository-only.
Starting with v2.6.0, it also updates normal installed AUR packages.
A later failure does not roll back the completed repository transaction.
Use moguet -Syu --repo for repository-only behavior.
Explicit upgrade workflows check saved source-build preferences strictly.
## 日本語
Moguet v2.6.0ではbehavior-changingな compatibility correctionを行います。
v2.6.0より前はrepository-only、v2.6.0以降はnormal installed AURも更新します。
"""
    current = "# Moguet v2.7.0\n\n## English\nCurrent scope.\n\n## 日本語\n今回のscope。\n\n"
    notes = current + historical
    cases = (
        ("2.7.0 retains 2.6.0 history", "2.7.0\n", notes, None),
        (
            "next release follows fixture VERSION",
            "2.8.0\n",
            notes.replace("# Moguet v2.7.0", "# Moguet v2.8.0"),
            None,
        ),
        ("missing current release", "2.7.0\n", historical, "exactly one '# Moguet v2.7.0'"),
        ("duplicate current release", "2.7.0\n", notes + current, "exactly one '# Moguet v2.7.0'"),
        ("wrong VERSION", "2.8.0\n", notes, "exactly one '# Moguet v2.8.0'"),
        (
            "missing current English section",
            "2.7.0\n",
            replace_once(notes, "## English\nCurrent scope.", "Current scope."),
            "current release notes must contain exactly one '## English'",
        ),
        (
            "historical Japanese does not replace current Japanese",
            "2.7.0\n",
            replace_once(notes, "## 日本語\n今回のscope。", "今回のscope。"),
            "current release notes must contain exactly one '## 日本語'",
        ),
        (
            "missing historical section",
            "2.7.0\n",
            current,
            "exactly one '# Moguet v2.6.0'",
        ),
        (
            "missing historical assertion",
            "2.7.0\n",
            replace_once(notes, "Before v2.6.0", "Previously"),
            "missing historical contract text: 'Before v2.6.0'",
        ),
        (
            "wrong historical introduction version",
            "2.7.0\n",
            replace_once(notes, "Starting with v2.6.0", "Starting with v2.7.0"),
            "missing historical contract text: 'Starting with v2.6.0'",
        ),
        (
            "current text cannot satisfy missing historical assertion",
            "2.7.0\n",
            current + "Before v2.6.0\n\n" + historical.replace("Before v2.6.0", "Previously"),
            "missing historical contract text: 'Before v2.6.0'",
        ),
        (
            "wrong Japanese historical version",
            "2.7.0\n",
            replace_once(notes, "v2.6.0以降", "v2.7.0以降"),
            "missing historical contract text: 'v2.6.0以降'",
        ),
    )
    for label, version, text, expected_diagnostic in cases:
        with tempfile.TemporaryDirectory(prefix="moguet-release-doc-checker-") as directory:
            fixture_root = Path(directory)
            (fixture_root / "VERSION").write_text(version, encoding="utf-8")
            (fixture_root / "RELEASE_NOTES.md").write_text(text, encoding="utf-8")
            diagnostic = io.StringIO()
            try:
                with redirect_stderr(diagnostic):
                    check_release_notes_documentation(fixture_root)
            except SystemExit as error:
                if (
                    error.code != 1
                    or expected_diagnostic is None
                    or expected_diagnostic not in diagnostic.getvalue()
                ):
                    fail(f"{label}: unexpected failure: {diagnostic.getvalue()}")
            else:
                if expected_diagnostic is not None:
                    fail(f"{label} unexpectedly passed")
            print(f"  ok: {label}")
    return len(cases)


def expect_runtime_help_rejected(
    label: str,
    locale: str,
    syntax: str,
    description: str,
) -> None:
    contract = reviewed_source_runtime_help_contracts(locale).get(syntax)
    if contract is None:
        fail(f"unknown runtime-help mutation entry: {locale} {syntax}")
    diagnostic = io.StringIO()
    try:
        with redirect_stderr(diagnostic):
            assert_semantic_text_contract(
                f"{locale} runtime help entry {syntax!r}",
                description,
                contract,
            )
    except SystemExit as error:
        if (
            error.code == 1
            and "public-documentation-check:" in diagnostic.getvalue()
        ):
            print(f"  ok: rejected {label}")
            return
        fail(f"{label} returned unexpected status {error.code!r}")
    fail(f"{label} unexpectedly passed")


def main() -> int:
    schema = load_schema()
    expected = expected_surface(schema)
    source = (REPOSITORY_ROOT / "man/moguet.1.in").read_text(encoding="utf-8")

    exact_man_public_surface(
        REPOSITORY_ROOT / "man/moguet.1.in", expected, schema
    )
    mutations = (
        (
            "duplicate trailing option",
            replace_once(
                source,
                SYNC_SELECT_SYNTAX,
                SYNC_SELECT_SYNTAX + " --needed",
            ),
        ),
        (
            "missing canonical option",
            replace_once(
                source,
                SYNC_SELECT_SYNTAX,
                "-S <pkg> | -S --select <query>",
            ),
        ),
        (
            "unexpected trailing token",
            replace_once(
                source,
                SYNC_SELECT_SYNTAX,
                SYNC_SELECT_SYNTAX + " extra",
            ),
        ),
        (
            "reordered canonical option",
            replace_once(
                source,
                SYNC_SELECT_SYNTAX,
                "-S <pkg> | -S [--needed] --select <query>",
            ),
        ),
        ("missing PUBLIC OPTIONS definition", remove_needed_definition(source)),
    )
    for label, mutated in mutations:
        expect_rejected(label, mutated, schema, expected)

    with tempfile.TemporaryDirectory(
        prefix="moguet-reviewed-doc-checker-"
    ) as directory:
        check_reviewed_source_documentation(
            copy_reviewed_source_documentation_fixture(directory)
        )

    with tempfile.TemporaryDirectory(
        prefix="moguet-system-aur-doc-checker-"
    ) as directory:
        check_system_aur_update_documentation(
            copy_system_aur_update_documentation_fixture(directory)
        )

    system_aur_mutations = (
        (
            "ordinary -Syu reverted to repository-only wording",
            "README.md",
            "ordinary AUR-helper update",
            "repository-only update",
        ),
        (
            "completion reverted to old -Syu behavior",
            "completions/descriptions/en.json",
            (
                '"-Syu": "Update repository packages and normal installed '
                'AUR packages without saved source-build preferences"'
            ),
            '"-Syu": "Upgrade the system"',
        ),
    )
    for label, relative_path, old, new in system_aur_mutations:
        expect_system_aur_update_documentation_rejected(
            label, relative_path, old, new
        )

    quoted_pkgbuild_help_entry = (
        "    print_help_entry(\n"
        '        "review.pkgbuild = \\"prompt\\"|\\"skip\\"",'
    )
    additive_unquoted_pkgbuild_help_entry = (
        "    print_help_entry(\n"
        '            "review.pkgbuild = prompt|skip",\n'
        '            localization::translate_message("Source-build mode"));\n'
        + quoted_pkgbuild_help_entry
    )

    reviewed_source_mutations = (
        (
            "missing legacy-cache migration contract",
            "README.md",
            "No manual migration is required",
            "Manual migration may be required",
        ),
        (
            "generic identity promoted from Unknown",
            "docs/contracts/source-package-identity.md",
            "common projectionの`Unknown`を`Known`へ昇格させたりしない",
            "common projectionを`Known`へ昇格させる",
        ),
        (
            "missing reviewed-state CAS contract",
            "docs/contracts/reviewed-source-state.md",
            "CAS semantics",
            "last-writer-wins semantics",
        ),
        (
            "stale runtime review.pkgbuild source wording",
            "source/moguet.cpp",
            "Invocation-local {} / {} editor policy; not reviewed-source acceptance",
            "{} review policy",
        ),
        (
            "stale runtime review.diff source wording",
            "source/moguet.cpp",
            "Repository diff / {} reviewed-source review policy; skipping does not advance reviewed state",
            "Repository update diff policy",
        ),
        (
            "unquoted runtime review.pkgbuild config syntax",
            "source/moguet.cpp",
            r"review.pkgbuild = \"prompt\"|\"skip\"",
            "review.pkgbuild = prompt|skip",
        ),
        (
            "additive unquoted runtime review.pkgbuild config syntax",
            "source/moguet.cpp",
            quoted_pkgbuild_help_entry,
            additive_unquoted_pkgbuild_help_entry,
        ),
        (
            "completion wording without initial full review",
            "completions/descriptions/en.json",
            "Review repository updates; for AUR, review the exact target from the previous reviewed revision or all tracked source initially",
            "Prompt to view repository update diffs",
        ),
        (
            "ambiguous completion --nodiff wording",
            "completions/descriptions/en.json",
            "Skip repository diff / reviewed-source review without advancing reviewed state",
            "Skip reviewed source changes",
        ),
    )
    for label, relative_path, old, new in reviewed_source_mutations:
        expect_reviewed_source_documentation_rejected(
            label, relative_path, old, new
        )

    runtime_help_mutations = (
        (
            "stale English review.pkgbuild config wording",
            "en",
            "review.pkgbuild = \"prompt\"|\"skip\"",
            "PKGBUILD review policy",
        ),
        (
            "stale English review.diff config wording",
            "en",
            "review.diff = \"prompt\"|\"skip\"",
            "Repository update diff policy",
        ),
        (
            "ambiguous English --nodiff wording",
            "en",
            "--nodiff",
            "Skip reviewed source changes",
        ),
        (
            "English --diff without initial full review",
            "en",
            "--diff",
            (
                "Review repository updates; for AUR, review from the previous "
                "reviewed revision to the exact target. Advance reviewed state "
                "only after explicit acceptance"
            ),
        ),
        (
            "editor action described as review acceptance",
            "en",
            "review.pkgbuild = \"prompt\"|\"skip\"",
            (
                "PKGBUILD / .install editor action for this invocation is "
                "reviewed-source acceptance"
            ),
        ),
        (
            "stale Japanese review.pkgbuild config wording",
            "ja",
            "review.pkgbuild = \"prompt\"|\"skip\"",
            "PKGBUILDの確認方針",
        ),
    )
    for label, locale, syntax, description in runtime_help_mutations:
        expect_runtime_help_rejected(
            label,
            locale,
            syntax,
            description,
        )

    scenario_count = (
        len(mutations)
        + len(reviewed_source_mutations)
        + len(runtime_help_mutations)
        + len(system_aur_mutations)
        + check_release_notes_regressions()
        + 3
    )
    print(
        "public-documentation-checker-test: "
        f"{scenario_count} scenarios passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
