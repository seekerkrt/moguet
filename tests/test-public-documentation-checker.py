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
    SemanticTextContract,
    assert_semantic_text_contract,
    check_public_release_status,
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


def check_rmdeps_migration_row(label: str, text: str, locale: str) -> None:
    rows = re.findall(r"^\|\s*`?RMDEPS=true`?\s*\|([^\n]*)\|\s*$", text, re.MULTILINE)
    if len(rows) != 1:
        fail(f"{label}: expected exactly one RMDEPS=true migration row")
    contracts = {
        "en": SemanticTextContract(
            required_patterns=(
                r"do not migrate",
                r"no persistent `?rmdeps`? config key",
                r"explicit per-invocation `?--rmdeps`? request",
                r"supported only for remote aur builds?",
            ),
            forbidden_patterns=(r"dependency cleanup remains unsupported",),
        ),
        "ja": SemanticTextContract(
            required_patterns=(
                r"移行しない",
                r"永続的な\s*`?rmdeps`?\s*config keyはない",
                r"invocationごとに明示する\s*`?--rmdeps`?\s*request",
                r"remote aur buildだけをsupport",
            ),
            forbidden_patterns=(r"dependency cleanupはunsupported",),
        ),
    }
    assert_semantic_text_contract(label, rows[0], contracts[locale])


def check_rmdeps_migration_regressions() -> int:
    scenario_count = 0
    for locale, filename, non_migration, persistent, request, scope, stale in (
        (
            "en", "v1-to-v2.md", "Do not migrate",
            "no persistent", "explicit per-invocation", "only for remote AUR builds",
            "dependency cleanup remains unsupported",
        ),
        (
            "ja", "v1-to-v2.ja.md", "移行しない",
            "永続的な", "invocationごとに明示する", "remote AUR buildだけ",
            "dependency cleanupはunsupported",
        ),
    ):
        path = REPOSITORY_ROOT / "docs/migration" / filename
        text = path.read_text(encoding="utf-8")
        check_rmdeps_migration_row(filename, text, locale)
        print(f"  ok: {filename} current RMDEPS migration contract")
        row = next(
            line for line in text.splitlines()
            if re.match(r"^\|\s*`?RMDEPS=true`?\s*\|", line)
        )
        # Positive wording variation: do not lock the entire row as a golden string.
        cases = (
            ("valid formatting variation", row.replace("`", "").replace(" | ", "  |  "), False),
            ("missing row", "", True),
            ("duplicate row", row + "\n" + row, True),
            ("stale unsupported claim", f"| `RMDEPS=true` | {non_migration}; {stale} |", True),
            ("additive stale claim", row[:-1] + stale + " |", True),
            ("persistent config migration", row.replace(non_migration, "Migrate to persistent config"), True),
            ("missing no-persistent-key policy", row.replace(persistent, ""), True),
            ("missing explicit invocation request", row.replace(request, ""), True),
            ("missing remote-only scope", row.replace(scope, "AUR builds"), True),
            ("contract outside migration row", "| `RMDEPS=true` | Migrate |\n" + row.split("|", 2)[2], True),
        )
        for label, candidate, rejected in cases:
            diagnostic = io.StringIO()
            try:
                with redirect_stderr(diagnostic):
                    check_rmdeps_migration_row(filename, candidate, locale)
            except SystemExit as error:
                if error.code != 1 or not rejected or not diagnostic.getvalue():
                    fail(f"{filename} {label}: unexpected failure: {diagnostic.getvalue()}")
            else:
                if rejected:
                    fail(f"{filename} {label} unexpectedly passed")
            print(f"  ok: {filename} {label}")
        scenario_count += 1 + len(cases)
    return scenario_count


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


def check_public_release_status_regressions() -> int:
    # Independent publication snapshot. The candidate VERSION may advance
    # without a GitHub Release; no network or current-version ledger is used.
    release_base = "https://github.com/seekerkrt/moguet/releases/"
    stable_link = f"**[v3.4.0]({release_base}tag/v3.4.0)**"
    latest_link = f"[GitHub Release]({release_base}latest)"
    english_snapshot = f"As of 2030-01-15, the current released stable version is {stable_link}."
    japanese_snapshot = f"2030-01-15時点の公開stableは {stable_link} です。"
    fixture = {
        "README.md": (
            f"# Moguet\n\n## Project status\n{english_snapshot}\n"
            f"Latest published stable: {latest_link}. Source VERSION is a build version.\n"
            "\n## Development\nmain is stable; develop integrates planned work.\n"
        ),
        "README.ja.md": (
            f"# Moguet\n\n## Project status\n{japanese_snapshot}\n"
            f"最新の公開stableは{latest_link}。VERSIONはbuild version。\n"
            "\n## 開発\nmainはstable、developはintegration。\n"
        ),
        "docs/project-stance.md": (
            "# Stance\n\n## 概要\nMilestones track planned scope.\n"
            f"\n## Current capabilityとcustomizationの姿勢\n{japanese_snapshot}\n"
            f"最新の公開stableは{latest_link}。次releaseの予定は公開保証ではない。\n"
        ),
        "docs/development.md": (
            "# Historical release preparation\n"
            "v2.8.0 release preparation historical example.\n"
            "v2.11.0 release preparation exact path set.\n"
            "v2.12.0 release preparation: current released stableはv2.11.0、"
            "準備中のreleaseはv2.12.0。\n"
        ),
        "RELEASE_NOTES.md": "# Moguet v3.4.1\nRC scope; not a publication assertion.\n",
        "VERSION": "3.4.1\n",
    }
    neutral = {
        path: text.replace(english_snapshot, "").replace(japanese_snapshot, "")
        for path, text in fixture.items()
    }
    cases = [
        ("unpublished RC VERSION may differ from published snapshot", {}, None),
        ("version-neutral guidance remains valid in immutable tags", neutral, None),
        ("source VERSION advances independently", {"VERSION": "3.5.0\n"}, None),
        (
            "historical preparation outside current sections is ignored",
            {"README.md": fixture["README.md"] + "\n## History\nThis tree prepares v2.8.0 (historical).\n"},
            None,
        ),
        (
            "Japanese stable drift",
            {"README.ja.md": fixture["README.ja.md"].replace("v3.4.0", "v3.3.0")},
            "dated stable snapshots differ",
        ),
        (
            "stance stable drift",
            {"docs/project-stance.md": fixture["docs/project-stance.md"].replace("v3.4.0", "v3.3.0")},
            "dated stable snapshots differ",
        ),
        (
            "snapshot observation date drift",
            {"README.ja.md": fixture["README.ja.md"].replace("2030-01-15", "2030-01-16")},
            "dated stable snapshots differ",
        ),
        (
            "invalid snapshot date",
            {"README.md": fixture["README.md"].replace("2030-01-15", "2030-02-30")},
            "invalid stable snapshot date",
        ),
        (
            "stable label and Release URL drift",
            {"README.md": fixture["README.md"].replace("tag/v3.4.0", "tag/v3.3.0")},
            "stable snapshot version and official Release URL differ",
        ),
        (
            "stable URL must identify official repository",
            {"README.md": fixture["README.md"].replace(release_base + "tag/", "https://example.org/releases/tag/")},
            "stable snapshot version and official Release URL differ",
        ),
        (
            "undated old stable assertion",
            {"README.md": fixture["README.md"].replace("As of 2030-01-15, the current", "The current")},
            "must use a dated stable snapshot",
        ),
        (
            "duplicate stable snapshot",
            {"README.md": fixture["README.md"].replace(english_snapshot, english_snapshot + "\n" + english_snapshot)},
            "at most one dated stable snapshot",
        ),
        (
            "missing current-status heading",
            {"README.md": fixture["README.md"].replace("## Project status", "## Old status")},
            "must contain exactly one '## Project status'",
        ),
        (
            "mixed neutral and snapshot guidance loses parity",
            {"README.md": neutral["README.md"]},
            "dated stable snapshots differ",
        ),
    ]
    for path in ("README.md", "README.ja.md", "docs/project-stance.md"):
        cases.append((
            f"missing version-neutral publication link in {path}",
            {path: fixture[path].replace(latest_link, "GitHub Release")},
            "must link to the latest GitHub Release",
        ))
    for path, anchor, stale in (
        ("README.md", english_snapshot, "This tree prepares **v3.4.1**."),
        ("README.ja.md", japanese_snapshot, "このtreeは **v3.4.1** の準備中です。"),
        ("docs/project-stance.md", japanese_snapshot, "準備中のreleaseはv3.4.1である。"),
        ("README.md", "## Development", "The current release-preparation scope is v3.4.1."),
        ("README.ja.md", "## 開発", "現在のrelease preparation scopeはv3.4.1。"),
        ("docs/project-stance.md", "## 概要", "準備中のreleaseはv3.4.1。"),
        ("README.md", english_snapshot, "Preparation and validation do not mean that the release has been published."),
        ("README.ja.md", japanese_snapshot, "準備や検証の完了はrelease公開済みを意味しません。"),
    ):
        cases.append((
            f"mutable preparation assertion in {path}: {stale}",
            {path: fixture[path].replace(anchor, anchor + "\n" + stale)},
            "retains obsolete semantics",
        ))
    for label, replacements, expected_diagnostic in cases:
        with tempfile.TemporaryDirectory(prefix="moguet-public-status-checker-") as directory:
            fixture_root = Path(directory)
            for relative, text in (fixture | replacements).items():
                path = fixture_root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            diagnostic = io.StringIO()
            try:
                with redirect_stderr(diagnostic):
                    check_public_release_status(fixture_root)
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
    if "--details" not in expected.options:
        fail("public authority lost --details")
    mutations = (
        (
            "obsolete presentation option",
            replace_once(source, '.B "--details"', '.B "--verbose"'),
        ),
        (
            "missing details option definition",
            replace_once(source, '.B "--details"', '.B ""'),
        ),
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
        + check_public_release_status_regressions()
        + check_rmdeps_migration_regressions()
        + 3
    )
    print(
        "public-documentation-checker-test: "
        f"{scenario_count} scenarios passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
