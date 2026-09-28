#!/usr/bin/env python3
"""Issue #665 pinned v1→v2 replay with real makepkg and pacman in Docker.

The fixture uses the existing loopback AUR HTTPS/RPC/Git server and no external
network. Each shape runs in its own fresh receipt container.
"""

import functools
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import ssl
import subprocess
import sys
import threading
import time
import tomllib
from http.server import ThreadingHTTPServer


SOURCE = Path(__file__).with_name("run-controlled-aur-lifecycle.py")
spec = importlib.util.spec_from_file_location("controlled_aur_lifecycle", SOURCE)
controlled = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controlled)
ROOT = controlled.ROOT
USER = controlled.USER
HOME = controlled.USER_HOME
APP = controlled.APP
INSTALL = f"{APP}.install"
INSTALL_MARKER = "# moguet issue 665 controlled install marker"
PKGBUILD_MARKER = "Moguet controlled lifecycle custom description"
DESTINATION_PROMPT = (
    "Patch directory (absolute path or relative to the command's starting "
    "directory; '~' is not expanded):"
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(argv, **options):
    return controlled.run(argv, **options)


def publish_with_install(number):
    result = controlled.publish(number)
    work = ROOT / APP
    pkgbuild = work / "PKGBUILD"
    source = pkgbuild.read_text()
    source = source.replace(
        "pkgrel=1\n",
        "pkgrel=1\n# stable patch context one\n"
        "# stable patch context two\n# stable patch context three\n",
        1,
    )
    source = source.replace(
        "options=('!debug')\n",
        f"options=('!debug')\ninstall={INSTALL}\n",
        1,
    )
    require(f"install={INSTALL}" in source, "fixture PKGBUILD install declaration missing")
    pkgbuild.write_text(source)
    (work / INSTALL).write_text("post_install() {\n    :\n}\n")
    run(["chown", f"{USER}:{USER}", str(pkgbuild), str(work / INSTALL)])
    srcinfo = run(["makepkg", "--printsrcinfo"], user=True, cwd=work)
    (work / ".SRCINFO").write_text(srcinfo)
    run(["chown", f"{USER}:{USER}", str(work / ".SRCINFO")])
    run(["git", "-C", str(work), "add", "PKGBUILD", ".SRCINFO", INSTALL], user=True)
    run(["git", "-C", str(work), "-c", "user.name=Controlled Fixture",
         "-c", "user.email=fixture@example.invalid", "commit", "-qm",
         f"issue 665 revision {number}"], user=True)
    remote = controlled.SERVER / (APP + ".git")
    run(["git", "-C", str(work), "push", str(remote), "main"], user=True)
    run(["git", "--git-dir", str(remote), "update-server-info"], user=True)
    result[APP] = run(["git", "-C", str(work), "rev-parse", "HEAD"], user=True).strip()
    return result


def interactive(command, shape, material, editor):
    environment = dict(controlled.ENV)
    environment.update({"TERM": "xterm", "EDITOR": str(editor), "VISUAL": str(editor),
                        "MOGUET_DOGFOOD_SHAPE": shape})
    argv = ["/usr/bin/runuser", "-u", USER, "--", "/usr/bin/env", "-i"]
    argv += [f"{key}={value}" for key, value in environment.items()]
    argv += command
    pid, descriptor = pty.fork()
    if pid == 0:
        os.chdir(HOME)
        os.execv(argv[0], argv)
    output = bytearray()
    answered = 0
    destination_answered = False
    save_prompts = 0
    apply_prompts = 0
    deadline = time.monotonic() + 900
    try:
        while True:
            require(time.monotonic() < deadline, "controlled issue 665 command timed out")
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
            if os.environ.get("MOGUET_DOGFOOD_DEBUG") == "1":
                os.write(sys.stdout.fileno(), data)
                if b"Patch directory" in output[-400:]:
                    print("DESTINATION_MATCH", DESTINATION_PROMPT.encode() in output,
                          "SENT", destination_answered,
                          "TAIL", repr(bytes(output[-180:])), flush=True)
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
                    save_prompts += 1
                if question.startswith("Apply saved patch customization to this update of "):
                    apply_prompts += 1
                os.write(descriptor, (answer + "\n").encode())
                answered += 1
            if DESTINATION_PROMPT.encode() in output and not destination_answered:
                os.write(descriptor, (str(material) + "\n").encode())
                destination_answered = True
                if os.environ.get("MOGUET_DOGFOOD_DEBUG") == "1":
                    print("DESTINATION_SENT", str(material), flush=True)
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
    if code != 0:
        print("CONTROLLED_TRANSCRIPT_TAIL\n" +
              "\n".join(transcript.splitlines()[-100:]), flush=True)
    require(code == 0, f"controlled command failed: {code}")
    return {"save_prompts": save_prompts, "apply_prompts": apply_prompts,
            "destination": destination_answered}


def main():
    require(os.geteuid() == 0 and Path("/.dockerenv").exists(),
            "disposable container root required")
    require(len(sys.argv) == 2 and sys.argv[1] in ("install-only", "mixed"),
            "choose install-only or mixed")
    shape = sys.argv[1]
    require(not ROOT.exists(), "fresh container required")
    controlled.SERVER.mkdir(parents=True)
    run(["chown", f"{USER}:{USER}", str(controlled.SERVER)])
    material = HOME / "issue665-controlled-materials"
    material.mkdir(mode=0o700)
    editor = HOME / "issue665-controlled-editor.py"
    editor.write_text(
        "#!/usr/bin/env python3\n"
        "import os,sys\n"
        "from pathlib import Path\n"
        "path=Path(sys.argv[-1])\n"
        f"if path.name=={INSTALL!r}:\n"
        f"    marker={INSTALL_MARKER!r}\n"
        "    with path.open('ab') as output: output.write(('\\n'+marker+'\\n').encode())\n"
        f"elif path.name=='PKGBUILD' and path.resolve().parent.name=={APP!r} and os.environ['MOGUET_DOGFOOD_SHAPE']=='mixed':\n"
        "    source=path.read_text()\n"
        "    before=\"Moguet controlled lifecycle fixture\"\n"
        f"    after={PKGBUILD_MARKER!r}\n"
        "    if before not in source: raise SystemExit('missing baseline description')\n"
        "    path.write_text(source.replace(before,after,1))\n"
    )
    editor.chmod(0o700)
    run(["chown", f"{USER}:{USER}", str(material), str(editor)])
    certificate = ROOT / "fixture.pem"
    key = ROOT / "fixture.key"
    run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256",
         "-days", "2", "-subj", "/CN=aur.archlinux.org",
         "-addext", "subjectAltName=DNS:aur.archlinux.org",
         "-keyout", str(key), "-out", str(certificate)], label="certificate-setup")
    run(["install", "-m644", str(certificate),
         "/etc/ca-certificates/trust-source/anchors/moguet-controlled.pem"])
    run(["update-ca-trust"])
    with Path("/etc/hosts").open("a") as hosts:
        hosts.write("\n127.0.0.1 aur.archlinux.org\n")
    server = ThreadingHTTPServer(("127.0.0.1", 443),
                                 functools.partial(controlled.Handler,
                                                   directory=str(controlled.SERVER)))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        first_oids = publish_with_install(1)
        first = interactive(["/usr/bin/moguet", "--edit", "build", APP],
                            shape, material, editor)
        require(first["save_prompts"] == 1 and first["destination"],
                "initial Save Yes did not commit")
        records = list((HOME / ".config/moguet/patches.d").glob("*.toml"))
        require(len(records) == 1, "expected one AUR association")
        record = tomllib.loads(records[0].read_text())
        entries = record["patches"]
        require(len(entries) == (1 if shape == "install-only" else 2),
                "wrong ordered material count")
        names = [entry["file"] for entry in entries]
        require(names[-1].startswith("INSTALL-") and
                (shape == "install-only" or names[0].startswith("PKGBUILD-")),
                "material order changed")
        for entry in entries:
            require(hashlib.sha256((material / entry["file"]).read_bytes()).hexdigest() ==
                    entry["sha256"], "material digest mismatch")
        first_db = list(Path("/var/lib/pacman/local").glob(APP + "-1-1/install"))
        require(len(first_db) == 1 and INSTALL_MARKER in first_db[0].read_text(),
                "v1 actual pacman install lacks edited script")
        second_oids = publish_with_install(2)
        require(second_oids[APP] != first_oids[APP], "fixture revision did not move")
        second = interactive(["/usr/bin/moguet", "--noedit", "upgrade-aur"],
                             shape, material, editor)
        require(second["apply_prompts"] == 1 and second["save_prompts"] == 0,
                "future Apply consent was absent or reused Save consent")
        second_db = list(Path("/var/lib/pacman/local").glob(APP + "-2-1/install"))
        require(len(second_db) == 1 and INSTALL_MARKER in second_db[0].read_text(),
                "v2 actual pacman install lacks replayed script")
        if shape == "mixed":
            info = run(["pacman", "-Qi", APP])
            require(PKGBUILD_MARKER in info, "v2 installed metadata lacks PKGBUILD customization")
        require(tomllib.loads(records[0].read_text()) == record,
                "future Apply changed saved association")
        print("ISSUE665_CONTROLLED_DOGFOOD_PASS " + json.dumps({
            "shape": shape, "v1_oid": first_oids[APP], "v2_oid": second_oids[APP],
            "materials": names, "digests": [entry["sha256"] for entry in entries],
            "v1_install_marker": True, "v2_install_marker": True,
            "save_prompts": first["save_prompts"],
            "future_apply_prompts": second["apply_prompts"],
        }), flush=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
