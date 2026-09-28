#!/usr/bin/env python3
"""Ownership metadata, unchanged semantic scenarios, and native UI insertion.

Scenario membership comes from the existing shared completion tests. Zsh
metadata checks intercept _describe; Fish checks use complete -C. PTY checks
use native line editors, including Zsh's actual grouped listing.
"""
from __future__ import annotations

from contextlib import redirect_stderr
from dataclasses import replace
import importlib.util
import io
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import generate_completions as gen

spec = importlib.util.spec_from_file_location("dynamic_completion", ROOT / "tests/test-dynamic-completion.py")
dynamic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dynamic)


def metadata(shell, adapter, args, env):
    if shell == "zsh":
        code = f'''function compdef {{ :; }}
source {shlex.quote(str(adapter))}
function _describe {{
    local tag label name record
    while [[ $1 == -* ]]; do
        case $1 in
        -t) tag=$2; shift 2 ;;
        *) shift ;;
        esac
    done
    label=$1; name=$2
    for record in "${{(@P)name}}"; do
        [[ ${{record%%:*}} == "$words[CURRENT]"* ]] || continue
        printf '%s\\t%s\\t%s\\n' "$tag" "$label" "$record"
    done
    return 0
}}
function compadd {{ return 0; }}
words=(moguet {' '.join(map(shlex.quote, args))})
CURRENT=${{#words}}
_moguet
'''
        command = ["zsh", "-f"]
    else:
        line = "moguet " + " ".join(shlex.quote(arg) if arg else "" for arg in args)
        code = f'''complete -e -c moguet
source {gen.fish_quote(str(adapter))}
complete -C {gen.fish_quote(line)}
'''
        command = ["fish", "--no-config"]
    result = subprocess.run(command, input=code, text=True, capture_output=True, env=env, timeout=6)
    assert result.returncode == 0 and not result.stderr, (shell, args, result)
    return [tuple(row.split("\t")) for row in result.stdout.splitlines() if row]


def main():
    schema = gen.load_schema()
    descriptions = gen.load_descriptions(schema, "en")
    ownerships = gen.option_ownerships(schema)
    # Labels are presentation authority, not a second classification catalogue.
    for option in schema.options:
        if option.is_completion_visible:
            assert ownerships[option.completion_token] == option.ownership
    collisions = [item for item in schema.query_tokens if item.projected and item.category != "upstream-delegated"]
    assert collisions
    for item in collisions:
        assert ownerships[item.token] == item.category
        assert item.canonical_identity.startswith("moguet:")

    for invalid in ("", "bad\nlabel", "bad\tlabel", "bad\x1blabel", "bad\x7flabel"):
        with redirect_stderr(io.StringIO()):
            try:
                gen.validate_description_map("ownership", {"key": invalid}, ("key",))
            except SystemExit:
                pass
            else:
                raise AssertionError(invalid)
    with redirect_stderr(io.StringIO()):
        try:
            gen.validate_description_map("ownership", {"wrong": "label"}, ("expected",))
        except SystemExit:
            pass
        else:
            raise AssertionError("ownership key drift accepted")

    # Changing UI strings, including _describe delimiters, must not change tokens.
    stress = replace(descriptions,
        ownership={key: value + r": category\path 'quoted'" for key, value in descriptions.ownership.items()},
        options={key: value + r": description\path 'quoted'" for key, value in descriptions.options.items()})
    cases = [(label, args) for label, args, _, _ in dynamic.SCENARIOS]
    cases += [(label, args) for label, args, _ in dynamic.typed_scenarios(schema)[2]]
    cases += [(label, args) for label, args, _ in dynamic.query_scenarios(schema)]
    with tempfile.TemporaryDirectory(prefix="moguet-presentation-") as temporary:
        directory = Path(temporary)
        helper = directory / "provider ' fixture"
        calls = directory / "calls"
        helper.write_text('''#!/usr/bin/python3
import pathlib, sys
with (pathlib.Path(__file__).parent / "calls").open("a") as f: f.write("1\\n")
names = ''' + repr(dynamic.NAMES) + '''
for name in names:
    if name.startswith(sys.argv[1]): print(name)
''')
        helper.chmod(0o755)
        env = os.environ | {"LC_ALL": "C", "HISTFILE": "", "XDG_CONFIG_HOME": str(directory / "config"),
                            "XDG_CACHE_HOME": str(directory / "cache"), "XDG_DATA_HOME": str(directory / "data")}
        adapters = {}
        for variant, data in (("ordinary", descriptions), ("stress", stress)):
            target = directory / variant
            target.mkdir()
            adapters[variant] = gen.generated_files(schema, data, "en", target, str(helper))
            for path, content in adapters[variant].items():
                path.write_text(content)
        # Bash is byte-identical even when ownership descriptions are changed.
        assert (directory / "ordinary/moguet.bash").read_bytes() == (directory / "stress/moguet.bash").read_bytes()
        for shell, filename in (("bash", "moguet.bash"), ("zsh", "_moguet"), ("fish", "moguet.fish")):
            adapter = directory / "ordinary" / filename
            stressed = directory / "stress" / filename
            for label, args in cases:
                calls.write_text("")
                ordinary = dynamic.run(shell, adapter, args, "ok", env)
                ordinary_count = len(calls.read_text().splitlines())
                calls.write_text("")
                changed = dynamic.run(shell, stressed, args, "ok", env)
                assert len(ordinary) == len(set(ordinary)), (shell, label, ordinary)
                assert len(changed) == len(set(changed)), (shell, label, changed)
                assert set(ordinary) == set(changed), (shell, label, ordinary, changed)
                assert ordinary_count == len(calls.read_text().splitlines()), (shell, label, "provider count")
            print(f"{shell}: {len(cases)} shared scenarios invariant under presentation changes; no duplicates; helper counts unchanged")
            if shell != "bash":
                for variant, data in (("ordinary", descriptions), ("stress", stress)):
                    path = directory / variant / filename
                    for args in (["-Q", ""], ["-Q", "--s"], ["-Q", "--nocon"], ["-Syu", "--"], ["build", "--"], ["-Ss", "--"]):
                        calls.write_text("")
                        rows = metadata(shell, path, args, env)
                        seen = []
                        for row in rows:
                            if shell == "zsh":
                                tag, label, record = row
                                token, description = record.split(":", 1)
                            else:
                                token, description = row
                            ownership = ownerships[token]
                            expected_label = data.ownership[ownership]
                            option = next((opt for opt in schema.options if opt.completion_token == token), None)
                            expected_description = data.options[option.token] if option else ""
                            if shell == "zsh":
                                assert tag == "moguet-options-" + ownership, (token, tag)
                                assert label == expected_label, (token, label)
                                assert description == gen.zsh_description(expected_description), (token, description)
                            else:
                                expected = expected_label + (": " + expected_description if option else "")
                                assert description == expected, (token, description, expected)
                            seen.append(token)
                        assert len(seen) == len(set(seen)), (shell, args, seen)
                        expected_tokens = dynamic.run(shell, path, args, "ok", env)
                        assert set(seen) == set(expected_tokens), (shell, args, seen, expected_tokens)
                        assert not calls.read_text(), (shell, args, "presentation invoked provider")
                print(f"{shell}: ownership metadata, delimiter escaping, explicit collision precedence PASS")
                finite, = gen.finite_completion_options(schema)
                for args, expected_count in ((["-S", "ch"], 1),
                                              (["build", finite.completion_token + finite.allowed_values[1][:1]], 0)):
                    calls.write_text("")
                    rows = metadata(shell, adapter, args, env)
                    if shell == "fish":
                        # These producers have no option description registration.
                        assert all(len(row) == 1 for row in rows), (args, rows)
                    else:
                        assert not rows, (args, "package/value entered ownership groups", rows)
                    assert len(calls.read_text().splitlines()) == expected_count
                print(f"{shell}: package/typed candidates carry no option ownership metadata; calls=1/0 PASS")
            # Native editors, with no _describe/compadd mocks, insert raw tokens.
            for text, expected in (("moguet -Q --searc", "--search"),
                                   ("moguet -Q --noconfir", "--noconfirm"),
                                   ("moguet -Q --neede", "--needed")):
                calls.write_text("")
                proof = dynamic.tab(shell, adapter, text, expected, env)
                assert not calls.read_text()
                print(f"{shell}: true PTY Tab {text!r} => {expected!r}; calls=0; {proof}")
            if shell == "zsh":
                calls.write_text("")
                proof = dynamic.tab(shell, adapter, "moguet -Q --s", "--s", env,
                    setup_extra="zstyle ':completion:*' format 'GROUP[%d]'\nzstyle ':completion:*' group-name ''\nbindkey '^X?' list-choices\n",
                    keys=b"\x18?")
                assert "GROUP[" + descriptions.ownership["upstream-delegated"] + "]" in proof, proof
                assert not calls.read_text()
                print("zsh: native _describe grouped PTY listing PASS; " + proof)
    print("completion ownership presentation: PASS")


if __name__ == "__main__":
    main()
