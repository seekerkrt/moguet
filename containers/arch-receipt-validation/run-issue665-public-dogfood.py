#!/usr/bin/env python3
"""Issue #665 public-AUR interaction in a disposable receipt container.

Run one shape per fresh container. The public AUR revision is not treated as a
fabricated update; future update replay is covered by the controlled fixture.
"""

import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import subprocess
import sys
import time
import tomllib


USER = "moguet-validation"
HOME = Path("/home") / USER
ROOT = HOME / "issue665-public-dogfood"
PACKAGE = "gclone"
INSTALL_MARKER = "# moguet issue 665 saved install marker"
PKGBUILD_MARKER = "# moguet issue 665 saved PKGBUILD marker"
DESTINATION_PROMPT = (
    "Patch directory (absolute path or relative to the command's starting "
    "directory; '~' is not expanded):"
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    require(os.geteuid() == 0 and Path("/.dockerenv").exists(),
            "disposable container root required")
    require(len(sys.argv) == 2 and sys.argv[1] in ("install-only", "mixed"),
            "choose install-only or mixed")
    shape = sys.argv[1]
    require(not ROOT.exists(), "fresh container required")
    ROOT.mkdir(mode=0o700)
    material = ROOT / "materials"
    material.mkdir(mode=0o700)
    config = ROOT / "config"
    cache = ROOT / "cache"
    state = ROOT / "state"
    for directory in (config, cache, state):
        directory.mkdir(mode=0o700)
    subprocess.run(["chown", "-R", f"{USER}:{USER}", str(ROOT)], check=True)
    editor = ROOT / "editor.py"
    editor.write_text(
        "#!/usr/bin/env python3\n"
        "import os,sys\n"
        "from pathlib import Path\n"
        "target=Path(sys.argv[-1])\n"
        "if target.name=='gclone.install':\n"
        f"    marker={INSTALL_MARKER!r}\n"
        "elif target.name=='PKGBUILD' and os.environ['MOGUET_DOGFOOD_SHAPE']=='mixed':\n"
        f"    marker={PKGBUILD_MARKER!r}\n"
        "else:\n"
        "    raise SystemExit('unexpected editor target: '+str(target))\n"
        "with target.open('ab') as output:\n"
        "    output.write(('\\n'+marker+'\\n').encode())\n"
    )
    editor.chmod(0o700)
    subprocess.run(["chown", f"{USER}:{USER}", str(editor)], check=True)
    env = {
        "PATH": "/usr/bin:/bin",
        "LC_ALL": "C",
        "LANG": "C",
        "TERM": "xterm",
        "HOME": str(HOME),
        "XDG_CONFIG_HOME": str(config),
        "XDG_CACHE_HOME": str(cache),
        "XDG_STATE_HOME": str(state),
        "EDITOR": str(editor),
        "VISUAL": str(editor),
        "MOGUET_DOGFOOD_SHAPE": shape,
    }
    command = ["/usr/bin/runuser", "-u", USER, "--", "/usr/bin/env", "-i"]
    command += [f"{key}={value}" for key, value in env.items()]
    command += ["/usr/bin/moguet", "--edit", "build", PACKAGE]
    pid, descriptor = pty.fork()
    if pid == 0:
        os.chdir(ROOT)
        os.execv(command[0], command)
    output = bytearray()
    answered = 0
    destination_answered = False
    save_prompt_count = 0
    deadline = time.monotonic() + 900
    try:
        while True:
            require(time.monotonic() < deadline, "public AUR dogfood timed out")
            ready, _, _ = select.select([descriptor], [], [], 0.25)
            if not ready:
                continue
            try:
                data = os.read(descriptor, 65536)
            except OSError:
                data = b""
            if not data:
                break
            output.extend(data)
            prompts = list(re.finditer(rb":: ([^\r\n]+?) \[(?:Y/n|y/N|y/n)\] ", output))
            for match in prompts[answered:]:
                question = match.group(1).decode(errors="replace")
                answer = "y"
                if question.startswith(("Show ", "Clean ", "Rebuild ", "Remove ")):
                    answer = "n"
                if question == "Edit PKGBUILD?":
                    answer = "n" if shape == "install-only" else "y"
                if question.startswith("Edit install script "):
                    answer = "y"
                if question == "Save this edit as patch customization?":
                    save_prompt_count += 1
                    answer = "y"
                os.write(descriptor, (answer + "\n").encode())
                answered += 1
            if DESTINATION_PROMPT.encode() in output and not destination_answered:
                os.write(descriptor, (str(material) + "\n").encode())
                destination_answered = True
        _, status = os.waitpid(pid, 0)
        code = os.waitstatus_to_exitcode(status)
    except BaseException:
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(pid, 0)
        raise
    finally:
        os.close(descriptor)
    transcript = output.decode(errors="replace").replace("\r", "")
    print("PUBLIC_AUR_TRANSCRIPT_TAIL\n" + "\n".join(transcript.splitlines()[-90:]), flush=True)
    require(code == 0, f"public AUR build/install failed: {code}")
    require(save_prompt_count == 1 and destination_answered,
            "explicit Save consent/destination did not occur once")
    registry = list((config / "moguet/patches.d").glob("*.toml"))
    require(len(registry) == 1, "one AUR association expected")
    record = tomllib.loads(registry[0].read_text())
    entries = record["patches"]
    expected_count = 1 if shape == "install-only" else 2
    require(record["package_base"] == PACKAGE and len(entries) == expected_count,
            "wrong logical material series")
    names = [entry["file"] for entry in entries]
    require(names[-1].startswith("INSTALL-") and
            (shape == "install-only" or names[0].startswith("PKGBUILD-")),
            "wrong generated material ordering")
    for entry in entries:
        content = (material / entry["file"]).read_bytes()
        require(hashlib.sha256(content).hexdigest() == entry["sha256"],
                "material digest differs from association")
    package = subprocess.run(["pacman", "-Q", PACKAGE], check=True,
                             text=True, capture_output=True).stdout.strip()
    database = list(Path("/var/lib/pacman/local").glob(PACKAGE + "-*/install"))
    require(len(database) == 1 and INSTALL_MARKER in database[0].read_text(),
            "actual pacman install script lacks accepted customization")
    print("ISSUE665_PUBLIC_DOGFOOD_PASS " + json.dumps({
        "shape": shape,
        "public_package": package,
        "save_prompt_count": save_prompt_count,
        "materials": names,
        "digests": [entry["sha256"] for entry in entries],
        "installed_script_marker": True,
    }), flush=True)


if __name__ == "__main__":
    main()
