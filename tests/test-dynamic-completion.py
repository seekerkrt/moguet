#!/usr/bin/env python3
"""One scenario authority, actual shell adapters, and real PTY Tab insertion.

Bash/Zsh scenario runs supply completion framework variables (Zsh's UI APIs
are intercepted). Fish uses its actual complete -C engine. PTY checks use each
shell's actual line editor and completion framework, without those mocks.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import pty
import select
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from generate_completions import fish_quote, load_schema, finite_completion_options, query_completion_tokens

NAMES = ("ch++", "ch-tool", "ch.tool", "ch@tool", "ch_tool", "chromium")
# Semantic expected results are shared by all shell runners, not three lists.
SCENARIOS = [
    ("plain", ["-S", "ch"], 1, NAMES),
    ("literal regex", ["-S", "ch.*"], 1, ()),
    ("literal plus", ["-S", "ch+"], 1, ("ch++",)),
    ("literal at", ["-S", "ch@"], 1, ("ch@tool",)),
    ("search", ["-Ss", "ch"], 0, ()),
    ("select", ["-S", "--select", "ch"], 0, ()),
    ("AUR", ["-S", "--aur", "ch"], 0, ()),
    ("marker", ["-S", "--", "ch"], 0, ()),
    ("unknown modifier", ["-Sxyz", "ch"], 0, ()),
    ("open tail", ["-S", "--search", "ch"], 0, ()),
    ("info out of slice", ["-Si", "ch"], 0, ()),
    ("known modifier out of slice", ["-S", "--needed", "ch"], 0, ()),
    ("prior operand out of slice", ["-S", "chromium", "ch"], 0, ()),
    ("unknown before operation", ["--unknown", "-S", "ch"], 0, ()),
    ("assignment current", ["-S", "X=ch"], 1, ()),
    ("option current", ["-S", "--c"], 0, None),
]
for option in ("--color", "--config", "--dbpath", "--root", "--sysroot", "-b", "-r"):
    SCENARIOS.append((f"pending {option}", ["-S", option, "X"], 0, ()))
    SCENARIOS.append((f"complete {option}", ["-S", option, "X", "ch"], 0, ()))
    SCENARIOS.append((f"inline {option}", ["-S", option + "=X", "ch"], 0, ()))
for option in ("-bX", "-rX", "-SbX", "-SrX"):
    SCENARIOS.append((f"attached {option}", ["-S", option, "ch"], 0, ()))
# Bash's default COMP_WORDBREAKS separates =; never mistake its RHS for a package.
SCENARIOS.append(("wordbreak equals", ["-S", "--config", "=", "X", "ch"], 0, ()))


def run(shell: str, adapter: Path, args: list[str], mode: str, env: dict) -> tuple[str, ...]:
    if shell == "bash":
        code = f'''source {shlex.quote(str(adapter))}
COMP_WORDS=(moguet {' '.join(map(shlex.quote, args))})
COMP_CWORD=$((${{#COMP_WORDS[@]}}-1))
_moguet
printf '%s\\n' "${{COMPREPLY[@]}}"
'''
        command = [shell, "--noprofile", "--norc"]
    elif shell == "zsh":
        code = f'''function compdef {{ :; }}
source {shlex.quote(str(adapter))}
function _describe {{
    local candidate name=$argv[-1]
    for candidate in "${{(@P)name}}"; do
        candidate=${{candidate%%:*}}
        [[ $candidate == "$words[CURRENT]"* ]] && print -r -- "$candidate"
    done
    return 0
}}
function compadd {{ local name=$argv[-1]; printf '%s\\n' "${{(@P)name}}"; }}
words=(moguet {' '.join(map(shlex.quote, args))})
CURRENT=${{#words}}
_moguet
'''
        command = [shell, "-f"]
    else:
        line = "moguet " + " ".join(shlex.quote(a) if a else "" for a in args)
        code = f'''complete -e -c moguet
source {fish_quote(str(adapter))}
complete -C {fish_quote(line)}
'''
        command = [shell, "--no-config"]
    result = subprocess.run(command, input=code, text=True, capture_output=True,
                            env=env | {"MOGUET_TEST_MODE": mode}, timeout=6)
    assert result.returncode == 0, (shell, args, result)
    assert not result.stderr, (shell, args, result.stderr)
    return tuple(line.split("\t")[0] for line in result.stdout.splitlines() if line)


def tab(shell: str, adapter: Path, text: str, expected: str, env: dict,
        *, setup_extra: str = "", keys: bytes = b"\t") -> str:
    pid, master = pty.fork()
    if pid == 0:
        command = {"bash": ["bash", "--noprofile", "--norc", "-i"],
                   "zsh": ["zsh", "-f", "-i"],
                   "fish": ["fish", "--no-config", "-i"]}[shell]
        os.execvpe(command[0], command, env | {"TERM": "dumb" if shell == "fish" else "xterm",
                                              "MOGUET_TEST_MODE": "ok"})
    transcript = b""

    def until(needle: bytes) -> bytes:
        nonlocal transcript
        data = b""
        deadline = time.monotonic() + 8
        while needle not in data and time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                data += chunk
                transcript += chunk
        assert needle in data, (shell, needle, data)
        return data

    try:
        if shell == "fish":
            setup = f'''set -g fish_greeting ''; set -g fish_autosuggestion_enabled 0
function fish_prompt; printf 'READY> '; end
function moguet; printf 'INSERT[%s]\\n' $argv; printf 'FINAL-CURRENT[%s]\\n' "$argv[-1]"; end
complete -e -c moguet
source {fish_quote(str(adapter))}
'''
        else:
            setup = ""
            if shell == "zsh":
                setup = "autoload -Uz compinit; compinit -D\n"
            last = '"${@: -1}"' if shell == 'bash' else '"$argv[-1]"'
            setup += f'''PS1='READY> '; HISTFILE=''
moguet() {{ printf 'INSERT[%s]\\n' "$@"; printf 'FINAL-CURRENT[%s]\\n' {last}; }}
source {shlex.quote(str(adapter))}
'''
        # Initial commands can produce multiple prompts; explicit marker closes setup.
        os.write(master, (setup + setup_extra + "printf 'SETUP-DONE\\n'\n").encode())
        until(b"SETUP-DONE\r\n")
        # Wait for the post-setup prompt before submitting the completion line.
        os.write(master, b"\x0c")
        until(b"READY> ")
        os.write(master, text.encode() + keys)
        # Read line editor output; this is readiness, not a performance SLA.
        time.sleep(0.25)
        os.write(master, b"\n")
        # A prior equal option value must not satisfy the current-word proof.
        data = until(f"FINAL-CURRENT[{expected}]".encode())
        assert b"provider-garbage" not in data, (shell, data)
        return repr(data.decode(errors="replace"))
    finally:
        os.write(master, b"exit\n")
        os.close(master)
        os.waitpid(pid, 0)


def typed_scenarios(schema):
    option, = finite_completion_options(schema)
    header = option.completion_token
    values = option.allowed_values
    cases = [("empty", ["build", header], values),
             ("root discovery", [header], values),
             ("invalid", ["build", header + "x"], ()),
             ("literal prefix", ["build", header + "r*"], ()),
             ("marker", ["build", "--", header], ()),
             ("separate grammar", ["build", option.token, "r"], None),
             ("package separate grammar", ["-S", option.token, "r"], None),
             ("pending upstream value", ["--config", header], ())]
    for value in values:
        cases.extend([(f"prefix {value}", ["build", header + value[:1]], (value,)),
                      (f"exact {value}", ["build", header + value], (value,)),
                      (f"prior {value}", ["build", header + value, header], (value,))])
    # Applicability oracle is the existing form relation, not another parser.
    by_id = {item.identity: item for item in schema.options}
    for operation in schema.operations:
        for index, form in enumerate(operation.forms):
            args = [operation.token] + [by_id[i].token for i in form.selector_ids]
            cases.append((f"form {operation.token}:{index}", args + [header],
                          values if option.identity in form.option_ids else ()))
    aliases = [item for item in schema.options if item.fixed_value and
               item.conflict_value_identity == option.conflict_value_identity]
    for alias in aliases:
        cases.append((f"alias {alias.token} empty", ["build", alias.token, header], (alias.fixed_value,)))
        cases.append((f"alias has no value grammar {alias.token}", ["build", alias.token + "=r"], ()))
        for value in values:
            compatible = (value,) if value == alias.fixed_value else ()
            cases.append((f"alias {alias.token} prefix {value}", ["build", alias.token, header + value[:1]], compatible))
            cases.append((f"alias {alias.token} prior {value}", ["build", alias.token, header + value, header], compatible))
    cases.append(("contradicting aliases", ["build"] + [alias.token for alias in aliases] + [header], ()))
    return option, aliases, [(label, args, tuple(header + value for value in expected) if expected is not None else None)
                              for label, args, expected in cases]


def query_scenarios(schema):
    tokens = query_completion_tokens(schema)
    matched = tuple(token for token in tokens if token.startswith("--s"))
    generic = next(item.token for item in schema.query_tokens
                   if item.category == "upstream-delegated" and
                   item.token not in dict(schema.lexical_value_options))
    return [
        ("query initial", ["-Q", ""], tokens),
        ("query prefix", ["-Q", "--s"], matched),
        ("query known flag", ["-Q", "--noconfirm", "--s"], matched),
        ("query global", ["--noconfirm", "-Q", "--s"], matched),
        ("query marker", ["-Q", "--", "--s"], ()),
        ("query pending", ["-Q", "--config", "--s"], ()),
        ("query current inline value", ["-Q", "--config=--s"], ()),
        ("query complete value", ["-Q", "--config", "/fixture", "--s"], matched),
        ("query prior inline value", ["-Q", "--config=/fixture", "--s"], matched),
        ("query unknown arity", ["-Q", generic, "--s"], ()),
        ("query unknown option", ["-Q", "--not-known", "--s"], ()),
        ("query prior operand opaque", ["-Q", "fixture", "--s"], ()),
        ("query combined modifier out of scope", ["-Qs", "--s"], ()),
        ("query wrong build", ["build", "--s"], None),
        ("query wrong sync", ["-S", "--s"], ("--select",)),
        ("query wrong unknown operation", ["-R", "--s"], ()),
    ]


def option_context_scenarios(schema):
    # The shared lexical projection supplies arity/marker expectations. This
    # closes the option-looking value gap without inferring pacman semantics.
    marker, = (token for token, _ in schema.parser_boundaries)
    cases = [("root marker", [marker, "--ne"], ())]
    cases += [("marker " + op.token, [op.token, marker, "--ne"], ())
              for op in schema.operations]
    for token, _ in schema.lexical_value_options:
        cases.append(("pending " + token, ["-S", token, "--ne"], ()))
        cases.append(("marker consumed as value " + token, ["-S", token, marker, "--ne"], ("--needed",)))
    return cases


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapters", type=Path, help="existing bound adapters for staged proof")
    parser.add_argument("--results", type=Path, help="write canonical semantic test results outside tracked sources")
    options = parser.parse_args()
    for shell in ("bash", "zsh", "fish"):
        assert shutil.which(shell), f"required shell unavailable: {shell}"
    with tempfile.TemporaryDirectory(prefix="moguet-dynamic-completion-") as temporary:
        directory = Path(temporary)
        helper = directory / "provider ' with spaces"
        calls = directory / "calls"
        helper.write_text('''#!/usr/bin/python3
import json, os, pathlib, sys, time
with (pathlib.Path(__file__).parent / "calls").open("a") as f: f.write(json.dumps(sys.argv[1]) + "\\n")
mode = os.environ.get("MOGUET_TEST_MODE", "ok")
names = ''' + repr(NAMES) + '''
if mode == "nonzero":
    print("chromium"); print("provider-garbage", file=sys.stderr); sys.exit(1)
if mode == "timeout":
    time.sleep(0.5); print("provider-garbage", file=sys.stderr); sys.exit(1)
if mode == "exact-count":
    for i in range(256): print("ch%03d" % i)
    sys.exit(0)
if mode == "exact-bytes":
    print("ch" + "x" * 65533); sys.exit(0)
bad = {"invalid": b"chromium\\nprovider:garbage\\n", "duplicate": b"chromium\\nchromium\\n",
       "unordered": b"chromium\\nch++\\n", "partial": b"chromium", "blank": b"chromium\\n\\n",
       "nul": b"chro\\x00mium\\n", "overflow": b"ch" + b"x"*65536 + b"\\n",
       "too-many": b"".join(("ch%03d\\n" % i).encode() for i in range(257)),
       "wrong-prefix": b"chromium\\nzzz-not-the-prefix\\n"}
if mode in bad: sys.stdout.buffer.write(bad[mode]); sys.exit(0)
for name in names:
    if name.startswith(sys.argv[1]): print(name)
''')
        helper.chmod(0o755)
        adapters = options.adapters or directory
        if not options.adapters:
            for shell, filename in (("bash", "moguet.bash"), ("zsh", "_moguet"),
                                    ("fish", "moguet.fish")):
                # Exercise the public generator entry's path validation too.
                result = subprocess.run([sys.executable, str(ROOT / "scripts/generate_completions.py"),
                    "--render", shell, "--repository-prefix-helper", str(helper)],
                    capture_output=True, text=True, check=True)
                (directory / filename).write_text(result.stdout)
                assert "/usr/bin/pacman" not in result.stdout, "runtime upstream discovery executable leaked into adapter"
        env = os.environ.copy()
        env.update({"LC_ALL": "C", "HISTFILE": "", "XDG_CONFIG_HOME": str(directory / "config"),
                    "XDG_CACHE_HOME": str(directory / "cache"), "XDG_DATA_HOME": str(directory / "data")})
        files = {"bash": adapters / "moguet.bash", "zsh": adapters / "_moguet",
                 "fish": adapters / "moguet.fish"}
        schema = load_schema()
        option, aliases, value_cases = typed_scenarios(schema)
        query_cases = query_scenarios(schema)
        lexical_cases = option_context_scenarios(schema)
        report = {"schema": 1, "snapshot_sha256": schema.query_input_sha256, "shells": {}}
        if options.adapters:
            # Existing staged paths are exercised with the real provider by verifier;
            # fixture protocol matrix uses canonical generation with a fixture binding.
            for shell, adapter in files.items():
                result = run(shell, adapter, ["-S", "chrom"], "ok", env)
                assert "chromium" in result, (shell, result)
                print(f"{shell}: existing bound adapter actual local chromium PASS")
            return
        for shell, adapter in files.items():
            rows = []
            def record(family, label, args, expected, count, actual):
                assert len(actual) == len(set(actual)), (shell, family, label, "duplicate token", actual)
                rows.append(dict(family=family, label=label, argv=args,
                                 expected=None if expected is None else sorted(expected),
                                 tokens=sorted(actual), helper_calls=count))
            for label, args, count, expected in SCENARIOS:
                calls.write_text("")
                actual = run(shell, adapter, args, "ok", env)
                assert len(calls.read_text().splitlines()) == count, (shell, label, calls.read_text())
                if count:
                    assert json.loads(calls.read_text()) == args[-1], (shell, label, calls.read_text())
                if expected is not None:
                    assert set(actual) == set(expected), (shell, label, actual, expected)
                record("package", label, args, expected, count, actual)
            for label, args, expected in value_cases:
                calls.write_text("")
                actual = run(shell, adapter, args, "ok", env)
                assert not calls.read_text(), (shell, label, "typed value invoked package helper")
                if expected is None:
                    # Fish's existing static infix matching can show options.
                    # Unsupported separate grammar must add no finite values.
                    assert not set(actual).intersection(option.allowed_values + tuple(option.completion_token + v for v in option.allowed_values)), (shell, label, actual)
                else:
                    assert set(actual) == set(expected), (shell, label, actual, expected)
                record("typed", label, args, expected, 0, actual)
            for label, args, expected in query_cases:
                calls.write_text("")
                actual = run(shell, adapter, args, "ok", env)
                assert not calls.read_text(), (shell, label, "query invoked package helper")
                assert len(actual) == len(set(actual)), (shell, label, "duplicate token", actual)
                if expected is None:
                    # Keep ordinary Fish static infix policy, while proving no
                    # newly discovered query token enters another route.
                    new_tokens = {item.token for item in schema.query_tokens if item.category == "upstream-delegated"}
                    assert not set(actual).intersection(new_tokens), (shell, label, actual)
                else:
                    assert set(actual) == set(expected), (shell, label, actual, expected)
                record("query", label, args, expected, 0, actual)
            for label, args, expected in lexical_cases:
                calls.write_text("")
                actual = run(shell, adapter, args, "ok", env)
                assert not calls.read_text(), (shell, label, "lexical context invoked provider")
                assert set(actual) == set(expected), (shell, label, actual, expected)
                record("lexical", label, args, expected, 0, actual)
            report["shells"][shell] = rows
            for mode, expected in (("exact-count", tuple("ch%03d" % i for i in range(256))),
                                   ("exact-bytes", ("ch" + "x" * 65533,))):
                calls.write_text("")
                actual = run(shell, adapter, ["-S", "ch"], mode, env)
                assert actual == expected, (shell, mode, "exact protocol boundary rejected")
                assert len(calls.read_text().splitlines()) == 1
            calls.write_text("")
            baseline = run(shell, adapter, ["-S", ""], "nonzero", env)
            assert "--select" in baseline and "--noconfirm" in baseline, (shell, baseline)
            for mode in ("nonzero", "timeout", "invalid", "duplicate", "unordered", "partial", "blank",
                         "nul", "overflow", "too-many"):
                calls.write_text("")
                actual = run(shell, adapter, ["-S", ""], mode, env)
                assert set(actual) == set(baseline), (shell, mode, actual, baseline)
                assert len(calls.read_text().splitlines()) == 1, (shell, mode)
            calls.write_text("")
            assert not run(shell, adapter, ["-S", "ch"], "wrong-prefix", env), (shell, "partial prefix mismatch leaked")
            assert len(calls.read_text().splitlines()) == 1
            helper.chmod(0o644)
            calls.write_text("")
            assert set(run(shell, adapter, ["-S", ""], "ok", env)) == set(baseline)
            assert not calls.read_text()
            helper.chmod(0o755)
            absent = directory / "absent-provider"
            helper.rename(absent)
            assert set(run(shell, adapter, ["-S", ""], "ok", env)) == set(baseline)
            assert not calls.read_text()
            absent.rename(helper)
            for text, expected in (("moguet -S chrom", "chromium"),
                                   ("moguet -S ch+", "ch++"),
                                   ("moguet -S 'chrom", "chromium"),
                                   ('moguet -S "chrom', "chromium"),
                                   (r"moguet -S ch\+", "ch++"),
                                   ("moguet -S ch@", "ch@tool"),
                                   ("moguet -S ch@to", "ch@tool")):
                calls.write_text("")
                proof = tab(shell, adapter, text, expected, env)
                assert len(calls.read_text().splitlines()) == 1, (shell, text, calls.read_text())
                prefix = text.split()[-1].lstrip("'\"").replace("\\", "")
                assert json.loads(calls.read_text()) == prefix, (shell, text, calls.read_text())
                print(f"{shell}: true PTY Tab {text!r} => {expected!r}, calls=1; {proof}")
            for label, args, count, _ in SCENARIOS:
                if label not in {"search", "select", "AUR", "marker", "unknown modifier", "open tail",
                                 "pending --color", "pending --config", "pending -b", "pending -r",
                                 "inline --config", "prior operand out of slice"}:
                    continue
                calls.write_text("")
                tab(shell, adapter, "moguet " + " ".join(args), args[-1], env)
                assert count == 0 and not calls.read_text(), (shell, label, calls.read_text())
            for line, expected in (("moguet -S ch @", "@"),
                                   ("moguet -S --config=ch@", "--config=ch@")):
                calls.write_text("")
                tab(shell, adapter, line, expected, env)
                assert not calls.read_text(), (shell, line, calls.read_text())
            print(f"{shell}: {len(SCENARIOS)} parity scenarios, exact byte/count bounds, one-query, 13 failure fallbacks, 14 PTY suppressions PASS")
            for value in option.allowed_values:
                calls.write_text("")
                text = "moguet build " + option.completion_token + value[:1]
                proof = tab(shell, adapter, text, option.completion_token + value, env)
                assert not calls.read_text(), (shell, text, "package helper")
                print(f"{shell}: true PTY typed Tab {text!r} => {option.completion_token + value!r}; calls=0; {proof}")
            value = option.allowed_values[0]
            alias = aliases[0]
            matched = alias.fixed_value
            for raw, expected in ((option.completion_token + value, option.completion_token + value),
                                  ("'" + option.completion_token + value[:1], option.completion_token + value),
                                  (option.completion_token + "'" + value[:1], option.completion_token + value),
                                  (option.token + r"\=" + value[:1], option.completion_token + value),
                                  (alias.token + " " + option.completion_token, option.completion_token + matched),
                                  (alias.token + " " + option.completion_token + matched[:1], option.completion_token + matched),
                                  (option.completion_token + value + " " + option.completion_token + value[:1], option.completion_token + value),
                                  ('"' + alias.token + '" ' + option.completion_token + matched[:1], option.completion_token + matched)):
                calls.write_text("")
                proof = tab(shell, adapter, "moguet build " + raw, expected, env)
                assert not calls.read_text(), (shell, raw, "package helper")
                print(f"{shell}: true PTY typed quote/alias Tab {raw!r} => {expected!r}; calls=0; {proof}")
            print(f"{shell}: {len(value_cases)} shared typed-value scenarios, provider isolation PASS")
            for before, target in ((option.completion_token + value, value),
                                   ('"' + alias.token + '"', matched)):
                calls.write_text("")
                line = "moguet " + before + " build " + option.completion_token + target[:1]
                proof = tab(shell, adapter, line, option.completion_token + target, env)
                assert not calls.read_text(), (shell, line, "package helper")
                print(f"{shell}: true PTY prior-before-operation {line!r}; calls=0; {proof}")
            for alias in aliases:
                for value in option.allowed_values:
                    if value == alias.fixed_value:
                        continue
                    calls.write_text("")
                    current = option.completion_token + value[:1]
                    tab(shell, adapter, "moguet build " + alias.token + " " + current, current, env)
                    assert not calls.read_text(), (shell, alias.token, "package helper")
            print(f"{shell}: actual PTY typed alias mismatches remain uncompleted, calls=0 PASS")
            # Choose actual fixture-derived long tokens with a unique prefix.
            generic_tokens = [item.token for item in schema.query_tokens if item.category == "upstream-delegated" and item.token.startswith("--")]
            samples = [token for token in generic_tokens if token.startswith("--s")] or generic_tokens[:2]
            for token in samples:
                prefix = token[:-1]
                if sum(other.startswith(prefix) for other in query_completion_tokens(schema)) != 1:
                    continue
                calls.write_text("")
                proof = tab(shell, adapter, "moguet -Q " + prefix, token, env)
                assert not calls.read_text(), (shell, token, "package helper")
                print(f"{shell}: true PTY upstream option {prefix!r} => {token!r}; calls=0; {proof}")
            unknown = next(item.token for item in schema.query_tokens if item.category == "upstream-delegated" and item.token not in dict(schema.lexical_value_options))
            for tail in (["--"], ["--config"], [unknown]):
                calls.write_text("")
                current = "--se"
                tab(shell, adapter, "moguet -Q " + " ".join(tail) + " " + current, current, env)
                assert not calls.read_text(), (shell, tail, "package helper")
            print(f"{shell}: {len(query_cases)} query token parity/isolation scenarios PASS")
            for args in (["-S", "--", "--ne"], ["-S", "--color", "--ne"],
                         ["-S", "--config", "--ne"], ["-S", "-b", "--ne"]):
                calls.write_text("")
                tab(shell, adapter, "moguet " + " ".join(args), args[-1], env)
                assert not calls.read_text(), (shell, args)
            print(f"{shell}: {len(lexical_cases)} shared lexical contexts and 4 native PTY option-looking suppressions PASS")
        if options.results:
            options.results.parent.mkdir(parents=True, exist_ok=True)
            options.results.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
            print(f"semantic result artifact: {options.results}")
    print("dynamic completion: all focused shell checks PASS")


if __name__ == "__main__":
    main()
