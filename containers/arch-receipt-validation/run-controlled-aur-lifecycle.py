#!/usr/bin/env python3
"""Two-revision F-02 evidence through the installed production CLI.

Run only in the disposable receipt container with --network=none and
--cap-add=SYS_PTRACE (the existing live lanes' sealed /proc/PID/fd handoff). Its private
hosts/CA route canonical AUR HTTPS to these two recipes; no product test seam,
command stub, dependency bypass, or public AUR service participates.
"""

import functools
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


APP = "moguet-controlled-app"
LIB = "moguet-controlled-lib"
REPO = "nano"  # Already in the receipt toolchain; not a product contract.
ROOT = Path("/tmp/moguet-controlled-aur-lifecycle")
SERVER = ROOT / "server"
USER = "moguet-validation"
USER_HOME = Path("/home") / USER
ENV = {"PATH": "/usr/bin", "LC_ALL": "C", "HOME": str(USER_HOME),
       "XDG_CONFIG_HOME": str(USER_HOME / ".config"),
       "XDG_CACHE_HOME": str(USER_HOME / ".cache"),
       "XDG_STATE_HOME": str(USER_HOME / ".local/state")}
revision = 0
packages = {}
requests = []


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(argv, *, user=False, cwd=None, label=None):
    command = (["/usr/bin/runuser", "-u", USER, "--"] if user else []) + argv
    result = subprocess.run(command, cwd=cwd, env=ENV, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    if label:
        (ROOT / (label + ".log")).write_text(result.stdout)
    print(f"COMMAND {json.dumps(command)} exit={result.returncode}", flush=True)
    print(result.stdout, end="", flush=True)
    require(result.returncode == 0, f"command failed: {argv}")
    return result.stdout


class Handler(SimpleHTTPRequestHandler):
    # The existing fixture strategy: RPC v5 envelopes and Git dumb HTTP. This
    # handler intentionally serves only this fixed sequence, with no forwarding.
    def do_GET(self):
        parsed = urlsplit(self.path)
        record = {"revision": revision, "path": self.path}
        if parsed.path == "/rpc/":
            query = parse_qs(parsed.query)
            require(query.get("v") == ["5"] and query.get("type") == ["info"],
                    "unexpected RPC operation")
            names = query.get("arg[]", [])
            results = [packages[name] for name in names if name in packages]
            record["results"] = results
            body = json.dumps({"version": 5, "type": "multiinfo",
                               "resultcount": len(results), "results": results}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            super().do_GET()
        requests.append(record)

    def log_message(self, *_):
        pass


def publish(number):
    global revision, packages
    metadata = {}
    oids = {}
    for name in (LIB, APP):
        work = ROOT / name
        if number == 1:
            work.mkdir()
            run(["git", "init", "-q", "--initial-branch=main", str(work)])
        dependency = REPO if name == LIB else f"{LIB}>={number}"
        # The app build itself requires the new lib to be installed already.
        # This adds a native ordering oracle independent of progress messages.
        build = (f"test \"$(cat /usr/share/{LIB}/revision)\" = {number}"
                 if name == APP else f"/usr/bin/{REPO} --version >/dev/null")
        (work / "PKGBUILD").write_text(
            f"pkgname={name}\npkgver={number}\npkgrel=1\n"
            "pkgdesc='Moguet controlled lifecycle fixture'\n"
            "arch=('any')\nlicense=('GPL')\n"
            # Arch predicts a debug sibling even for data-only recipes. Keep
            # expected/actual inventory equal without relaxing product guards.
            "options=('!debug')\n"
            f"depends=('{dependency}')\n"
            f"build() {{\n    {build}\n}}\n"
            f"package() {{\n    install -dm755 \"$pkgdir/usr/share/{name}\"\n"
            f"    printf '%s\\n' {number} >\"$pkgdir/usr/share/{name}/revision\"\n}}\n")
        run(["chown", "-R", f"{USER}:{USER}", str(work)])
        srcinfo = run(["makepkg", "--printsrcinfo"], user=True, cwd=work)
        (work / ".SRCINFO").write_text(srcinfo)
        run(["git", "-C", str(work), "add", "PKGBUILD", ".SRCINFO"], user=True)
        run(["git", "-C", str(work), "-c", "user.name=Controlled Fixture",
             "-c", "user.email=fixture@example.invalid", "commit", "-qm",
             f"revision {number}"], user=True)
        oids[name] = run(["git", "-C", str(work), "rev-parse", "HEAD"], user=True).strip()
        served = SERVER / (name + ".git")
        if number == 1:
            run(["git", "clone", "--bare", str(work), str(served)], user=True)
        else:
            run(["git", "-C", str(work), "push", str(served), "main"], user=True)
        run(["git", "--git-dir", str(served), "update-server-info"], user=True)
        metadata[name] = {"Name": name, "PackageBase": name,
                          "Version": f"{number}-1", "Description": "controlled F-02",
                          "Depends": [dependency], "MakeDepends": [], "CheckDepends": [],
                          "Provides": [], "Conflicts": [], "Replaces": []}
    packages = metadata
    revision = number
    (ROOT / f"revision-{number}.json").write_text(json.dumps(
        {"oids": oids, "metadata": metadata}, indent=2))
    print("PUBLISHED " + json.dumps({"revision": number, "oids": oids,
                                      "metadata": metadata}), flush=True)
    return oids


def inventory():
    return dict(line.split(" ", 1) for line in run(["pacman", "-Q"]).splitlines())


def main():
    require(os.geteuid() == 0 and Path("/.dockerenv").exists(), "container root required")
    require(not ROOT.exists(), "fresh disposable container required")
    SERVER.mkdir(parents=True)
    run(["chown", f"{USER}:{USER}", str(SERVER)])
    before = inventory()
    explicit_before = set(run(["pacman", "-Qqe"]).splitlines())
    require(APP not in before and LIB not in before and REPO in explicit_before,
            "fixture needs absent AUR roots and preinstalled Explicit repo dependency")
    run(["pacman", "-Si", REPO], label="repository-metadata")

    # Trust is scoped to this networkless container. No host trust/config mount.
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
                                functools.partial(Handler, directory=str(SERVER)))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    old_oids = None
    try:
        for number in (1, 2):
            oids = publish(number)
            require(old_oids is None or all(oids[n] != old_oids[n] for n in oids),
                    "remote revision did not change")
            old_oids = oids
            request_start = len(requests)
            plan = run(["moguet", "plan", APP], user=True, label=f"plan-{number}")
            deps = run(["moguet", "deps", "--recursive", APP], user=True,
                       label=f"deps-{number}")
            require(f"{LIB}>={number}" in deps and f"- {REPO} [repo]" in deps
                    and re.search(rf"{LIB}>={number}.*\[aur\]", deps)
                    and f"  1. {LIB}\n  2. {APP}\n" in plan,
                    "fresh plan did not expose the controlled dependency graph")
            build_request_start = len(requests)
            log_path = Path("/var/log/pacman.log")
            log_start = log_path.stat().st_size
            run(["moguet", "build", "--noconfirm", APP], user=True,
                label=f"build-{number}")
            build_requests = requests[build_request_start:]
            rpc_results = [p for r in build_requests for p in r.get("results", [])]
            require(all(any(p == packages[n] for p in rpc_results) for n in (APP, LIB)),
                    "build did not fetch fresh RPC metadata for both AUR packages")
            transaction = log_path.read_bytes()[log_start:].decode()
            (ROOT / f"pacman-{number}.log").write_text(transaction)
            print("PACMAN TRANSACTIONS\n" + transaction, flush=True)
            action = "installed" if number == 1 else "upgraded"
            operations = re.findall(r"\[ALPM\] (installed|upgraded|reinstalled|removed) (\S+)", transaction)
            require(operations == [(action, LIB), (action, APP)],
                    f"wrong actual transaction order/set: {operations}")
            after = inventory()
            require(after == before | {LIB: f"{number}-1", APP: f"{number}-1"},
                    "actual package inventory/version differs")
            require(set(run(["pacman", "-Qqe"]).splitlines()) == explicit_before | {APP},
                    "root Explicit/dependency reason/repo Explicit preservation differs")
            require(LIB in run(["pacman", "-Qqd"]).splitlines(), "AUR lib is not Dependency")
            for name in (LIB, APP):
                checkout = USER_HOME / ".cache/moguet" / name
                actual_oid = run(["git", "-C", str(checkout), "rev-parse", "HEAD"], user=True).strip()
                require(actual_oid == oids[name], "source checkout did not reach published revision")
                run(["pacman", "-Qi", name], label=f"installed-{name}-{number}")
                run(["pacman", "-Qk", name])
                desc = Path(f"/var/lib/pacman/local/{name}-{number}-1/desc").read_text()
                (ROOT / f"db-{name}-{number}.txt").write_text(desc)
                print("DB SNAPSHOT " + name + "\n" + desc, flush=True)
                require(Path(f"/usr/share/{name}/revision").read_text() == f"{number}\n",
                        "installed payload is stale")
                dependency = REPO if name == LIB else f"{LIB}>={number}"
                require(dependency in desc.split("%DEPENDS%\n", 1)[1].split("\n\n", 1)[0].splitlines(),
                        "installed dependency metadata is stale")
            # Request/response evidence belongs to this invocation, not an old run.
            print("REQUESTS " + json.dumps(requests[request_start:]), flush=True)
            print(f"REVISION {number}: fresh graph -> native build -> actual install/reason PASS", flush=True)
        print("F-02 controlled production CLI lifecycle PASS (loopback HTTPS, external network none)")
    finally:
        server.shutdown()
        server.server_close()
        (ROOT / "requests.json").write_text(json.dumps(requests, indent=2))
        print("ALL REQUESTS " + json.dumps(requests), flush=True)


if __name__ == "__main__":
    main()
