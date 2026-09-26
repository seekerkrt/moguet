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


with tempfile.TemporaryDirectory(prefix='moguet-patch-cli-') as temporary:
    root=Path(temporary)
    fixture=root/'rpc.json'; fixture.write_text(json.dumps({'packages': {}}))
    port=root/'port'
    server=subprocess.Popen([sys.executable, str(REPO/'tests/aur_rpc_fixture_server.py'), str(fixture), str(port)], stdout=subprocess.DEVNULL)
    try:
        deadline=time.monotonic()+10
        while not port.exists():
            require(server.poll() is None and time.monotonic()<deadline, 'RPC fixture failed')
            time.sleep(.02)
        rpc='http://127.0.0.1:'+port.read_text().strip()+'/rpc/'
        case=Case(root/'journey', rpc)
        for args in (['list-patch'], ['list-patch', '--details'], ['--details', 'list-patch', '--details']):
            require('No patch customizations registered.' in case.run(args), 'empty registry diagnostic missing')
            require(not (case.root/'config/moguet/patches.d').exists(), 'listing created registry')
            require(not list((case.root/'state').iterdir()) and not list((case.root/'cache').iterdir()), 'listing created state/cache')
        for option in ('--json', '--check', '--local', '--details=yes', '--use-patches', '--noconfirm', '--noedit', '--dry-run', '--repo'):
            case.run(['list-patch', option], ok=False)
            case.no_build()
        for args in (['list-patch', 'unexpected'], ['list-patch', '--', '--details']):
            case.run(args, ok=False)
        original=case.source.joinpath('PKGBUILD').read_bytes()
        original_srcinfo=case.source.joinpath('.SRCINFO').read_bytes()
        material_before={p.name:p.read_bytes() for p in case.material.iterdir()}
        require('does not enable automatic' in case.register(), 'registration summary missing')
        case.no_build()
        require({p.name:p.read_bytes() for p in case.material.iterdir()}==material_before, 'registration modified user material')
        record=next((case.root/'config/moguet/patches.d').glob('*.toml'))
        saved=record.read_bytes()
        # Observe filesystem reads from the actual CLI, without a production hook.
        # Linux inotify watches source/material roots and bytes; listing may only read config.
        libc=ctypes.CDLL(None, use_errno=True)
        watch_fd=libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        require(watch_fd>=0, 'inotify initialization failed')
        try:
            for path in (case.source, case.source/'PKGBUILD', case.material, case.material/'one.patch', case.material/'two.patch'):
                require(libc.inotify_add_watch(watch_fd, os.fsencode(path), 0x1 | 0x20)>=0, 'inotify watch failed')
            normal=case.run(['list-patch'])
            detailed=case.run(['list-patch', '--details'])
            try: events=os.read(watch_fd, 65536)
            except BlockingIOError: events=b''
            require(not events, 'registry listing opened/read external source or patch material')
        finally:
            os.close(watch_fd)
        case.no_build()
        require(not case.eval_log.read_text(), 'listing evaluated recipe')
        require('patch-cli-base' in normal and f'local:{case.source}' in normal and
                f'patches=2' in normal and f'material={case.material}' in normal, 'normal projection incomplete')
        for name, contents in material_before.items():
            digest=hashlib.sha256(contents).hexdigest()
            require(name not in normal and digest not in normal, 'normal leaked per-patch detail')
            require(name in detailed and digest in detailed, 'details lost saved expected digest')
        require('Record schema version: 1' in detailed and 'saved expected SHA-256; material not checked' in detailed, 'details misrepresented record data')
        require(detailed.index('1. one.patch')<detailed.index('2. two.patch'), 'details sorted saved series')
        require(record.stem not in normal and 'schema' not in normal, 'normal leaked internal record fields')
        require(case.run(['--details', 'list-patch', '--details'])==detailed, 'details placement/idempotence drifted')
        # Missing, unreadable, changed or nonregular material never becomes a listing error.
        material_file=case.material/'one.patch'
        material_file.write_text('changed material')
        require(case.run(['list-patch', '--details'])==detailed, 'listing rehashed changed material')
        material_file.unlink()
        require(case.run(['list-patch'])==normal, 'listing checked missing patch')
        os.mkfifo(material_file)
        require(case.run(['list-patch', '--details'])==detailed, 'listing opened FIFO material')
        material_file.unlink(); material_file.write_bytes(material_before['one.patch'])
        case.material.chmod(0)
        try: require(case.run(['list-patch'])==normal, 'listing required material access')
        finally: case.material.chmod(0o755)
        for contents, diagnostic in ((b'invalid toml [', 'corrupt'),
                (saved.replace(b'schema_version = 1', b'schema_version = 99'), 'unsupported'),
                (saved.replace(b"source_kind = 'local'", b"source_kind = 'aur'"), 'unsupported')):
            require(contents!=saved, 'bad-record fixture did not change')
            record.write_bytes(contents)
            text=case.run(['list-patch'], ok=False)
            require(diagnostic in text and 'No patch customizations' not in text and 'Registered patch' not in text, 'bad registry was skipped or partially displayed')
        record.write_bytes(saved)
        require(case.run(['list-patch'])==normal, 'listing was nondeterministic')
        header, *series = saved.split(b'[[patches]]')
        require(len(series)==2, 'ordered-series fixture shape changed')
        record.write_bytes(header+b'[[patches]]'+series[1]+b'[[patches]]'+series[0])
        reversed_details=case.run(['list-patch', '--details'])
        require(reversed_details.index('1. two.patch')<reversed_details.index('2. one.patch'), 'listing sorted the saved patch series')
        record.write_bytes(saved)
        extra_sources=[]
        for directory, base in [('z-source','aaa-base'), ('a-source','patch-cli-base')]:
            source=case.root/directory
            write(source/'PKGBUILD', RECIPE.replace('patch-cli-base',base))
            case.run(['add-patch',source,case.material,'one.patch'],tty=True)
            extra_sources.append((source,base))
        multiple=case.run(['list-patch'])
        require(multiple.index('aaa-base')<multiple.index(f'local:{case.root/"a-source"}')<multiple.index(f'local:{case.source}'), 'multiple associations are not deterministically ordered')
        require(case.run(['list-patch'])==multiple, 'multiple listing was nondeterministic')
        for source,base in extra_sources:
            case.run(['del-patch',source,base])
        require(case.run(['list-patch'])==normal, 'listing changed existing association')

        # Independent persisted AUR v2 fixture: production writers are covered
        # by the component test; public listing consumes mixed historical data.
        aur_url='https://aur.archlinux.org/patch-cli-base.git'
        aur_key=hashlib.sha256(b'moguet-aur-recipe-patch-v2\0aur\0'+aur_url.encode()+b'\0patch-cli-base').hexdigest()
        aur_record=record.parent/(aur_key+'.toml')
        aur_bytes=("schema_version=2\nsource_kind='aur'\nsource_url='"+aur_url+"'\n"
                   "package_base='patch-cli-base'\nmaterial_root='"+str(case.material)+"'\n"
                   "[[patches]]\nfile='one.patch'\nsha256='"+hashlib.sha256(material_before['one.patch']).hexdigest()+"'\n")
        write(aur_record, aur_bytes, 0o600)
        watch_fd=libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        require(watch_fd>=0, 'mixed inotify initialization failed')
        try:
            for path in (case.source, case.material, case.material/'one.patch'):
                require(libc.inotify_add_watch(watch_fd, os.fsencode(path), 0x1 | 0x20)>=0, 'mixed watch failed')
            # No process lookup is needed, and RPC cannot resolve this source.
            inert_env={'PATH':'/nonexistent', 'MOGUET_TEST_AUR_RPC_BASE_URL':'http://127.0.0.1:1/rpc/'}
            mixed=case.run(['list-patch'], env=inert_env)
            mixed_details=case.run(['list-patch','--details'], env=inert_env)
            require(mixed.index('local:')<mixed.index('aur:'+aur_url), 'mixed source kind/order lost')
            require('Record schema version: 1' in mixed_details and 'Record schema version: 2' in mixed_details, 'mixed schema detail lost')
            require(case.run(['list-patch'], env=inert_env)==mixed, 'mixed listing nondeterministic')
            try: events=os.read(watch_fd, 65536)
            except BlockingIOError: events=b''
            require(not events, 'mixed listing accessed external source/material')
        finally:
            os.close(watch_fd)
        case.no_build()
        require(not case.eval_log.read_text() and not case.command_log.read_text(), 'mixed listing executed source commands')
        require(record.read_bytes()==saved and aur_record.read_text()==aur_bytes, 'mixed listing rewrote records')
        aur_record.unlink()


        require('already registered' in case.run(['add-patch', case.source, case.material, 'one.patch'], ok=False, tty=True), 'duplicate register not rejected')
        case.no_build()
        # Poison the selected record: plain local build must not consult it.
        record.unlink(); record.symlink_to(case.material/'one.patch')
        case.run(['--noedit','build','--local',case.source], tty=True)
        require(case.probe()=='1:before:stock', 'plain build applied saved customization')
        record.unlink(); record.write_bytes(saved); record.chmod(0o600)
        text=case.selected(preview=True,show=True)
        require(text.index("| pkgdesc='second'")<text.index('Evaluate patched PKGBUILD'), 'modified candidate not shown before evaluation')
        require(case.probe()=='1:second:effective' and 'Selected patch series' in text, 'selected build did not consume modified recipe')
        require('pacman -U' in case.command_log.read_text(), 'existing install boundary was not used')
        require(case.source.joinpath('PKGBUILD').read_bytes()==original and case.source.joinpath('.SRCINFO').read_bytes()==original_srcinfo, 'original was modified')
        require(not list((case.root/'cache/moguet').glob('.local-source*')), 'candidate cleanup failed')
        # New upstream, unchanged association/digests, fresh metadata/artifact.
        write(case.source/'PKGBUILD', RECIPE.replace('pkgver=1', 'pkgver=2'))
        case.selected()
        require(case.probe('2')=='2:second:effective' and record.read_bytes()==saved, 'upstream reuse used stale bytes/metadata or rewrote association')
        write(case.source/'PKGBUILD', RECIPE.replace("pkgdesc='before'", "pkgdesc='upstream-conflict'"))
        require('patch application failed' in case.selected(ok=False), 'upstream conflict was not actionable')
        case.no_build()
        write(case.source/'PKGBUILD', RECIPE)
        # Replace material deterministically in consumer prepatch evaluation,
        # after strict acquisition. The current invocation must use old bytes.
        write(case.root/'replacement', patch('before','replacement'))
        case.selected(env={'PATCH_REPLACE_FILE': str(case.material/'one.patch'), 'PATCH_REPLACEMENT': str(case.root/'replacement')})
        require(case.probe()=='1:second:effective', 'consumer reopened changed material')
        require('material changed' in case.selected(ok=False), 'digest change was not rejected')
        case.no_build(); require(record.read_bytes()==saved, 'build automatically updated digest')
        write(case.material/'two.patch',patch('before','first'))
        write(case.material/'one.patch',patch('first','replacement'))
        updated_material={p.name:p.read_bytes() for p in case.material.iterdir()}
        case.run(['update-patch',case.source,case.material,'two.patch','one.patch'], tty=True)
        require({p.name:p.read_bytes() for p in case.material.iterdir()}==updated_material, 'update modified user material')
        case.selected(); require(case.probe()=='1:replacement:effective', 'explicit digest/order update not consumed')
        # Missing material must stop selected builds but not config-only forget.
        (case.material/'one.patch').unlink()
        require('missing' in case.selected(ok=False), 'missing material not reported'); case.no_build()
        two=(case.material/'two.patch').read_bytes()
        case.run(['del-patch',case.source,'patch-cli-base'])
        require((case.material/'two.patch').read_bytes()==two and not record.exists(), 'forget changed user material')
        require('not registered' in case.selected(ok=False), 'absent selection not rejected'); case.no_build()
        require('not registered' in case.run(['del-patch',case.source,'patch-cli-base'],ok=False), 'absent forget not reported')
        require('not registered' in case.run(['update-patch',case.source,case.material,'two.patch'],ok=False,tty=True), 'absent update not reported')
        for name, mutate, expected in [
            ('unsafe', lambda c,r: (c.material/'one.patch').chmod(0o666), 'unsafe'),
            ('corrupt', lambda c,r: r.write_text('schema_version ='), 'corrupt'),
            ('unsupported', lambda c,r: r.write_text(r.read_text().replace('schema_version = 1','schema_version = 99')), 'unsupported'),
            ('source-base', lambda c,r: write(c.source/'PKGBUILD',RECIPE.replace('pkgbase=patch-cli-base','pkgbase=another-base')), 'not registered'),
            ('mismatch', lambda c,r: r.write_text(r.read_text().replace('patch-cli-base','wrong-base')), 'does not match'),
        ]:
            c=Case(root/name,rpc); c.register(); r=next((c.root/'config/moguet/patches.d').glob('*.toml'))
            mutate(c,r); require(expected in c.selected(ok=False), f'{name} failure missing'); c.no_build()
        # Public postpatch identity and metadata failures retain their phase.
        for name,modified,expected in [
            ('changed-base', RECIPE.replace('pkgbase=patch-cli-base','pkgbase=another-base'), 'identity differs'),
            ('changed-child', RECIPE.replace('patch-cli-child','another-child'), 'identity differs'),
            ('metadata-failure', RECIPE.replace("pkgdesc='before'", "pkgdesc='before'\nexit 7"), 'metadata evaluation failed'),
        ]:
            c=Case(root/name,rpc)
            content="diff --git a/PKGBUILD b/PKGBUILD\n"+''.join(difflib.unified_diff(
                RECIPE.splitlines(True),modified.splitlines(True),fromfile='a/PKGBUILD',tofile='b/PKGBUILD'))
            write(c.material/'one.patch',content)
            c.run(['add-patch',c.source,c.material,'one.patch'],tty=True)
            require(expected in c.selected(ok=False), f'{name} did not preserve failure phase');c.no_build()
            require((c.source/'PKGBUILD').read_text()==RECIPE, f'{name} changed original')
            require(not list((c.root/'cache/moguet').glob('.local-source-workspace*')), f'{name} failed candidate cleanup')
        c=Case(root/'consent',rpc)
        c.run(['add-patch',c.source,c.material,'one.patch'],ok=False)
        require(not c.eval_log.read_text(), 'non-TTY registration evaluated recipe')
        c.run(['--noconfirm','add-patch',c.source,c.material,'one.patch'],ok=False,tty=True)
        require(not c.eval_log.read_text(), '--noconfirm authorized evaluation')
        c.register()
        c.run(['--noconfirm','build','--local','--use-patches',c.source],ok=False,tty=True)
        require(not c.eval_log.read_text(), '--noconfirm selection authorized evaluation')
        c.selected(ok=False,answer='n'); c.no_build()
        for response in ('n','cancel','EOF'):
            c.selected(ok=False,post_answer=response)
            require(len(c.eval_log.read_text().splitlines())==2, 'post-review refusal executed patched metadata')
            c.no_build()
        # A metadata-visible dependency change must affect the public planner.
        c=Case(root/'dependency',rpc)
        dependency_patch="diff --git a/PKGBUILD b/PKGBUILD\n"+''.join(difflib.unified_diff(
            RECIPE.splitlines(True),RECIPE.replace("depends=()","depends=('patch-dep')").splitlines(True),
            fromfile='a/PKGBUILD',tofile='b/PKGBUILD'))
        write(c.material/'one.patch',dependency_patch)
        c.run(['add-patch',c.source,c.material,'one.patch'],tty=True)
        c.selected()
        with tarfile.open(c.root/'archives/patch-cli-child-1-1-any.pkg.tar') as archive:
            require('depend = patch-dep' in archive.extractfile('.PKGINFO').read().decode(), 'postpatch dependency metadata missing')
        (c.root/'db/local/patch-dep-1-1/desc').unlink()
        (c.root/'db/local/patch-dep-1-1').rmdir()
        def change_candidate():
            candidate=Path(c.eval_log.read_text().splitlines()[-1].split('|')[0])/'PKGBUILD'
            candidate.write_text(candidate.read_text()+'\n# changed during Proceed\n')
        c.selected(ok=False,before_proceed=change_candidate)
        c.no_build()
        bad=dependency_patch.replace('patch-dep','unavailable-patch-dependency')
        write(c.material/'one.patch',bad)
        c.run(['update-patch',c.source,c.material,'one.patch'],tty=True)
        require('dependency plan' in c.selected(ok=False), 'postpatch unresolved dependency bypassed planner'); c.no_build()
        # Lexical failures are all pre-log/pre-cache and never delegate options.
        c=Case(root/'grammar',rpc)
        for args in [
            ['build','remote','--use-patches'], ['--use-patches','build','--local',c.source],
            ['build','--local','--use-patches','--use-patches',c.source],
            ['--edit','build','--local','--use-patches',c.source],
            ['--dry-run','build','--local','--use-patches',c.source],
            ['build','--local','--use-preference','--use-patches',c.source],
            ['-Syu','--use-patches'], ['upgrade','--use-patches'],
            ['-Syu','--use-patches=yes'], ['build','--local','--use-patches=yes',c.source],
            ['add-patch',c.source], ['add-patch','--unsupported',c.source,c.material,'one.patch'],
            ['del-patch',c.source,'patch-cli-base','extra'],
        ]:
            c.run(args,ok=False)
            require(not (c.root/'state/moguet').exists() and not (c.root/'cache/moguet').exists(), 'invalid grammar mutated state/cache')
        print('public local patch CLI tests passed: lifecycle, zero implicit read/apply, real archive, upstream reuse, strict failures, consent, grammar')
    finally:
        server.terminate(); server.wait(timeout=5)
