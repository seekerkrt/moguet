#!/usr/bin/env python3
"""Public local patch workflow, real Git/makepkg/libalpm; install is a sealed-input fixture."""
import io
import ctypes
import hashlib
import difflib
import json
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import tarfile
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
BINARY = str(Path(sys.argv[1]).resolve())


def require(value, message):
    if not value:
        raise AssertionError(message)


def write(path, text, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(mode)


def patch(before, after):
    return ("diff --git a/PKGBUILD b/PKGBUILD\n--- a/PKGBUILD\n+++ b/PKGBUILD\n"
            "@@ -5,3 +5,3 @@\n arch=('any')\n-pkgdesc='" + before + "'\n+pkgdesc='" + after +
            "'\n depends=()\n")


RECIPE = """pkgbase=patch-cli-base
pkgname=('patch-cli-child')
pkgver=1
pkgrel=1
arch=('any')
pkgdesc='before'
depends=()
printf '%s|%s|%s\\n' "$PWD" "$pkgver" "$pkgdesc" >> "$PATCH_EVAL_LOG"
if [[ -n ${PATCH_REPLACE_FILE:-} && $(wc -l < "$PATCH_EVAL_LOG") == 2 ]]; then
    cp -- "$PATCH_REPLACEMENT" "$PATCH_REPLACE_FILE.next"
    mv -- "$PATCH_REPLACE_FILE.next" "$PATCH_REPLACE_FILE"
fi
build() { printf '%s:%s:%s\\n' "$pkgver" "$pkgdesc" "${PATCH_FLAG:-stock}" >> "$PATCH_BUILD_LOG"; }
package() {
    install -Dm644 /dev/null "$pkgdir/usr/share/patch-cli/probe"
    printf '%s:%s:%s' "$pkgver" "$pkgdesc" "${PATCH_FLAG:-stock}" > "$pkgdir/usr/share/patch-cli/probe"
}
"""


class Case:
    def __init__(self, root, rpc):
        self.root = root
        root.mkdir()
        self.source, self.material = root / 'source', root / 'patches'
        self.source.mkdir()
        self.material.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('MOGUET_TEST_')}
        for key in ('PKGDEST', 'SRCDEST', 'BUILDDIR', 'CARCH', 'MAKEPKG_CONF', 'LANGUAGE'):
            self.env.pop(key, None)
        for key, name in [('HOME', 'home'), ('XDG_CONFIG_HOME', 'config'),
                          ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache')]:
            (root / name).mkdir(mode=0o700)
            self.env[key] = str(root / name)
        (root / 'archives').mkdir()
        self.command_log, self.eval_log, self.build_log = (root / name for name in ('commands', 'evaluations', 'builds'))
        for path in (self.command_log, self.eval_log, self.build_log):
            path.touch()
        self.env.update(LANG='C', LC_ALL='C', PATCH_EVAL_LOG=str(self.eval_log), PATCH_BUILD_LOG=str(self.build_log),
                        MOGUET_TEST_COMMAND_LOG=str(self.command_log), MOGUET_TEST_AUR_RPC_BASE_URL=rpc,
                        MOGUET_TEST_LEGACY_SUDO_STUB=str(REPO / 'tests/stubs/source-maintenance/sudo'))
        # An isolated, standard read-only ALPM fixture; no host database mutation.
        db = root / 'db'
        (root / 'pacman-root').mkdir()
        write(db / 'local/ALPM_DB_VERSION', '9\n')
        def desc(name):
            return f'%NAME%\n{name}\n\n%VERSION%\n1-1\n\n%BASE%\n{name}\n\n%ARCH%\nany\n\n%REASON%\n0\n\n%DESC%\nfixture\n\n'
        for name in ('base-devel', 'patch-dep'):
            write(db / f'local/{name}-1-1/desc', desc(name))
        (db / 'sync').mkdir()
        with tarfile.open(db / 'sync/core.db', 'w') as archive:
            for name in ('base-devel', 'patch-dep'):
                data=(desc(name)+f'%FILENAME%\n{name}-1-1-any.pkg.tar\n\n%CSIZE%\n1\n\n%ISIZE%\n1\n\n').encode()
                info=tarfile.TarInfo(f'{name}-1-1/desc'); info.size=len(data)
                archive.addfile(info, io.BytesIO(data))
        write(root / 'pacman.conf', f'[options]\nRootDir = {root / "pacman-root"}\nDBPath = {db}\nArchitecture = x86_64\nSigLevel = Never\n[core]\nServer = file:///unreachable-fixture\n')
        self.env['PATCH_PACMAN_CONF'] = str(root / 'pacman.conf')
        write(root / 'bin/pacman-conf', '#!/bin/sh\nexec /usr/bin/pacman-conf --config "$PATCH_PACMAN_CONF" "$@"\n', 0o755)
        write(root / 'bin/pacman', '#!/bin/sh\ncase "$1" in -T|-Q*) exec /usr/bin/pacman --config "$PATCH_PACMAN_CONF" "$@";; *) exit 125;; esac\n', 0o755)
        write(root / 'bin/sudo', '#!/bin/sh\nprintf \'denied sudo\\n\' >> "$MOGUET_TEST_COMMAND_LOG"\nexit 125\n', 0o755)
        self.env['PATH'] = str(root / 'bin') + ':/usr/bin:/bin'
        write(root / 'makepkg.conf', f"""CARCH=x86_64
CHOST=x86_64-unknown-linux-gnu
CFLAGS=''
CXXFLAGS=''
CPPFLAGS=''
LDFLAGS=''
LTOFLAGS=''
DEBUG_CFLAGS=''
DEBUG_CXXFLAGS=''
MAKEFLAGS=''
BUILDENV=(!distcc !color !ccache !check !sign)
OPTIONS=(!strip !docs !libtool !staticlibs !emptydirs !zipman !purge !debug !lto !autodeps)
INTEGRITY_CHECK=(sha256)
PKGEXT='.pkg.tar'
SRCEXT='.src.tar'
PACMAN='{root / 'bin/pacman'}'
""")
        self.env['MAKEPKG_CONF'] = str(root / 'makepkg.conf')
        write(self.source / 'PKGBUILD', RECIPE)
        write(self.source / '.SRCINFO', 'pkgbase = patch-cli-base\n\tpkgver = 1\n\tpkgrel = 1\n\tarch = any\npkgname = patch-cli-child\n')
        os.utime(self.source / 'PKGBUILD', (100, 100))
        os.utime(self.source / '.SRCINFO', (101, 101))
        write(self.material / 'one.patch', patch('before', 'first'))
        write(self.material / 'two.patch', patch('first', 'second'))

    def run(self, args, ok=True, tty=False, answer='y', env=None, post_answer=None, show=False, before_proceed=None):
        self.build_log.write_text(''); self.eval_log.write_text(''); self.command_log.write_text('')
        actual=self.env | (env or {})
        argv=[BINARY, *map(str, args)]
        if tty:
            pid, fd=pty.fork()
            if pid == 0:
                os.execve(BINARY, argv, actual)
            output=bytearray(); answered={}; deadline=time.monotonic()+45
            try:
                while True:
                    require(time.monotonic()<deadline, 'PTY timeout: '+output.decode(errors='replace'))
                    readable, _, _=select.select([fd], [], [], .1)
                    if readable:
                        try: data=os.read(fd, 65536)
                        except OSError: data=b''
                        if not data: break
                        output.extend(data)
                        for question in (b'Show PKGBUILD for read-only review?', b'Evaluate PKGBUILD metadata with makepkg --printsrcinfo?',
                                         b'Evaluate patched PKGBUILD metadata with makepkg --printsrcinfo?', b'Proceed with build?'):
                            count=output.count(question)
                            if count>answered.get(question,0):
                                response=answer
                                if question.startswith(b'Show '): response='y' if show else 'n'
                                if question.startswith(b'Evaluate patched') and post_answer is not None: response=post_answer
                                if question.startswith(b'Proceed') and before_proceed: before_proceed()
                                os.write(fd, b'\x04' if response=='EOF' else (response+'\n').encode())
                                answered[question]=count
                _, status=os.waitpid(pid, 0)
                code=os.waitstatus_to_exitcode(status)
            except BaseException:
                try: os.killpg(pid,signal.SIGKILL)
                except ProcessLookupError: pass
                os.waitpid(pid,0)
                raise
            finally:
                os.close(fd)
            text=output.decode(errors='replace').replace('\r', '')
        else:
            result=subprocess.run(argv, input='', text=True, capture_output=True, env=actual, timeout=45)
            code=result.returncode; text=result.stdout+result.stderr
        if (code==0)!=ok:
            raise AssertionError(f'{argv}: exit {code}\n{text}')
        return text

    def register(self):
        return self.run(['add-patch', self.source, self.material, 'one.patch', 'two.patch'], tty=True)

    def selected(self, ok=True, preview=False, **kwargs):
        return self.run(([] if preview else ['--noedit'])+['build', '--local', '--use-patches', self.source, 'PATCH_FLAG=effective'], ok=ok, tty=True, **kwargs)

    def no_build(self):
        require(not self.build_log.read_text() and not self.command_log.read_text(), 'failure fell through to build/install')

    def probe(self, version='1'):
        path=self.root / f'archives/patch-cli-child-{version}-1-any.pkg.tar'
        with tarfile.open(path) as archive:
            return archive.extractfile('usr/share/patch-cli/probe').read().decode()


