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
from generate_completions import fish_quote

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
    local candidate
    for candidate in "${{described[@]}}"; do
        candidate=${{candidate%%:*}}
        [[ $candidate == "$words[CURRENT]"* ]] && print -r -- "$candidate"
    done
    return 0
}}
function compadd {{ printf '%s\\n' "${{packages[@]}}"; }}
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


def tab(shell: str, adapter: Path, text: str, expected: str, env: dict) -> str:
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
function moguet; printf 'INSERT[%s]\\n' $argv; end
complete -e -c moguet
source {fish_quote(str(adapter))}
'''
        else:
            setup = ""
            if shell == "zsh":
                setup = "autoload -Uz compinit; compinit -D\n"
            setup += f'''PS1='READY> '; HISTFILE=''
moguet() {{ printf 'INSERT[%s]\\n' "$@"; }}
source {shlex.quote(str(adapter))}
'''
        # Initial commands can produce multiple prompts; explicit marker closes setup.
        os.write(master, (setup + "printf 'SETUP-DONE\\n'\n").encode())
        until(b"SETUP-DONE\r\n")
        # Wait for the post-setup prompt before submitting the completion line.
        os.write(master, b"\x0c")
        until(b"READY> ")
        os.write(master, text.encode() + b"\t")
        # Read line editor output; this is readiness, not a performance SLA.
        time.sleep(0.25)
        os.write(master, b"\n")
        data = until(f"INSERT[{expected}]".encode())
        assert b"provider-garbage" not in data, (shell, data)
        return repr(data.decode(errors="replace"))
    finally:
        os.write(master, b"exit\n")
        os.close(master)
        os.waitpid(pid, 0)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapters", type=Path, help="existing bound adapters for staged proof")
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
bad = {"invalid": b"chromium\\nprovider:garbage\\n", "duplicate": b"chromium\\nchromium\\n",
       "unordered": b"chromium\\nch++\\n", "partial": b"chromium", "blank": b"chromium\\n\\n",
       "nul": b"chro\\x00mium\\n", "overflow": b"ch" + b"x"*65536 + b"\\n",
       "too-many": b"".join(("ch%03d\\n" % i).encode() for i in range(257))}
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
        env = os.environ.copy()
        env.update({"LC_ALL": "C", "HISTFILE": "", "XDG_CONFIG_HOME": str(directory / "config"),
                    "XDG_CACHE_HOME": str(directory / "cache"), "XDG_DATA_HOME": str(directory / "data")})
        files = {"bash": adapters / "moguet.bash", "zsh": adapters / "_moguet",
                 "fish": adapters / "moguet.fish"}
        if options.adapters:
            # Existing staged paths are exercised with the real provider by verifier;
            # fixture protocol matrix uses canonical generation with a fixture binding.
            for shell, adapter in files.items():
                result = run(shell, adapter, ["-S", "chrom"], "ok", env)
                assert "chromium" in result, (shell, result)
                print(f"{shell}: existing bound adapter actual local chromium PASS")
            return
        for shell, adapter in files.items():
            for label, args, count, expected in SCENARIOS:
                calls.write_text("")
                actual = run(shell, adapter, args, "ok", env)
                assert len(calls.read_text().splitlines()) == count, (shell, label, calls.read_text())
                if count:
                    assert json.loads(calls.read_text()) == args[-1], (shell, label, calls.read_text())
                if expected is not None:
                    assert set(actual) == set(expected), (shell, label, actual, expected)
            calls.write_text("")
            baseline = run(shell, adapter, ["-S", ""], "nonzero", env)
            assert "--select" in baseline and "--noconfirm" in baseline, (shell, baseline)
            for mode in ("nonzero", "timeout", "invalid", "duplicate", "unordered", "partial", "blank",
                         "nul", "overflow", "too-many"):
                calls.write_text("")
                actual = run(shell, adapter, ["-S", ""], mode, env)
                assert set(actual) == set(baseline), (shell, mode, actual, baseline)
                assert len(calls.read_text().splitlines()) == 1, (shell, mode)
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
            print(f"{shell}: {len(SCENARIOS)} parity scenarios, one-query, 12 failure fallbacks, 14 PTY suppressions PASS")
    print("dynamic completion: all focused shell checks PASS")


if __name__ == "__main__":
    main()
