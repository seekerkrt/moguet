#!/usr/bin/env python3
"""Upgrade recipe patches through the real CLI, Git, makepkg and ALPM fixture."""
import ctypes
import difflib
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

from patch_cli_fixture import BINARY, REPO, Case, RECIPE, require, write


def recipe(base, version='2', child=None):
    return RECIPE.replace('patch-cli-base', base).replace('patch-cli-child', child or base).replace('pkgver=1', 'pkgver='+version)


class UpgradeCase(Case):
    def __init__(self, root, rpc, packages=('patch-upgrade',)):
        super().__init__(root, rpc)
        self.packages = packages
        self.recipes = {}
        self.git_log = root/'git-log'
        self.git_log.touch()
        self.env['PATCH_GIT_LOG'] = str(self.git_log)
        self.env['PATCH_REMOTES'] = str(root/'remotes')
        self.env['PATCH_SYSTEM_LOG'] = str(root/'system-log')
        write(root/'bin/sudo', '''#!/usr/bin/python3
import os, sys, shutil, tarfile
from pathlib import Path
args=sys.argv[1:]
root=Path(__file__).resolve().parents[1]
if args[:2]==['pacman','-U'] and '--' in args:
    separator=args.index('--')
    if not set(args[2:separator]) <= {'--noconfirm','--needed','--asdeps','--asexplicit'}: raise SystemExit(125)
    for arg in args[separator+1:]:
        path=Path(arg)
        if not path.resolve().is_relative_to(root/'cache') or not tarfile.is_tarfile(path): raise SystemExit(125)
        with tarfile.open(path) as archive:
            if not archive.extractfile('.PKGINFO').read(): raise SystemExit(125)
        shutil.copyfile(path,root/'archives'/path.name)
    with open(root/'commands','a') as f: f.write(' '.join(args)+'\\n')
    raise SystemExit(0)
if args not in (['pacman','-Syu'], ['pacman','-Syu','--noconfirm'], ['pacman','-Su'], ['pacman','-Su','--noconfirm']):
    raise SystemExit(125)
with open(os.environ['PATCH_SYSTEM_LOG'],'a') as f: f.write(' '.join(args)+'\\n')
version=os.environ.get('PATCH_SYSTEM_VERSION')
if version:
    if not version.replace('-','').isdigit(): raise SystemExit(125)
    records=list((root/'db/local').glob('patch-upgrade-*'))
    if len(records)!=1 or records[0].resolve()!=records[0]: raise SystemExit(125)
    record=records[0]
    text=(record/'desc').read_text()
    old=text.split('%VERSION%\\n',1)[1].split('\\n',1)[0]
    (record/'desc').write_text(text.replace('%VERSION%\\n'+old+'\\n','%VERSION%\\n'+version+'\\n'))
    record.rename(record.parent/('patch-upgrade-'+version))
raise SystemExit(int(os.environ.get('PATCH_SYSTEM_EXIT','0')))
''', 0o755)
        write(root/'bin/git-fixture', '''#!/usr/bin/python3
import os, subprocess, sys
from pathlib import Path
args=sys.argv[1:]
root=Path(__file__).resolve().parents[1]
with open(root/'git-log','a') as f: f.write(' '.join(args)+'\\n')
operations=('clone','fetch','ls-remote')
index=next((i for i,a in enumerate(args) if a in operations),None)
canonical=None
if index is not None:
    if args[index]=='clone': args.insert(index+1,'--no-local')
    for i,a in enumerate(args):
        if a.startswith('https://aur.archlinux.org/') and a.endswith('.git'):
            canonical=a
            args[i]=str(root/'remotes'/a.rsplit('/',1)[1])
    if args[index]=='fetch' and canonical is None:
        url=subprocess.check_output(['/usr/bin/git','config','--local','--get','remote.origin.url'], text=True).strip()
        if not url.startswith('https://aur.archlinux.org/') or not url.endswith('.git'): raise SystemExit(125)
        args[index:index]=['-c','remote.origin.url='+str(root/'remotes'/url.rsplit('/',1)[1])]
        index+=2
    args[index:index]=['-c','protocol.file.allow=always']
code=subprocess.call(['/usr/bin/git',*args])
if code==0 and 'clone' in args and canonical:
    code=subprocess.call(['/usr/bin/git','-C',args[-1],'remote','set-url','origin',canonical])
raise SystemExit(code)
''', 0o755)
        self.env['MOGUET_TEST_GIT_EXECUTABLE'] = str(root/'bin/git-fixture')
        for base in packages:
            self.add_upstream(base, recipe(base))
            self.install_record(base)

    def install_record(self, name, version='1-1', base=None):
        for previous in (self.root/'db/local').glob(name+'-*'):
            if previous.is_dir(): shutil.rmtree(previous)
        write(self.root/f'db/local/{name}-{version}/desc', f'%NAME%\n{name}\n\n%VERSION%\n{version}\n\n%BASE%\n{base or name}\n\n%ARCH%\nany\n\n%REASON%\n0\n\n%DESC%\nfixture\n\n')

    def add_upstream(self, base, contents):
        work=self.root/'upstreams'/base
        work.mkdir(parents=True, exist_ok=True)
        git=lambda *args: subprocess.run(['/usr/bin/git','-C',str(work),*args], check=True, capture_output=True)
        if not (work/'.git').exists():
            git('init','-q','-b','main')
            git('config','user.email','fixture@example.invalid')
            git('config','user.name','Fixture')
        write(work/'PKGBUILD', contents)
        result=subprocess.run(['/usr/bin/makepkg','--printsrcinfo'], cwd=work, env=self.env, text=True, capture_output=True, check=True)
        write(work/'.SRCINFO',result.stdout)
        git('add','PKGBUILD','.SRCINFO'); git('commit','-qm','recipe')
        remote=self.root/'remotes'/f'{base}.git'
        remote.parent.mkdir(exist_ok=True)
        if not remote.exists():
            subprocess.run(['/usr/bin/git','clone','--bare','-q',str(work),str(remote)],check=True,capture_output=True)
        else:
            git('push','-q',str(remote),'main')
        self.recipes[base]=contents

    def save(self, base, after=None, files=None, url=None):
        before=self.recipes[base]
        after=after or before.replace("pkgdesc='before'", "pkgdesc='custom'")
        material=self.material/base
        material.mkdir(exist_ok=True)
        if files is None:
            files={'one.patch': 'diff --git a/PKGBUILD b/PKGBUILD\n'+''.join(difflib.unified_diff(
                before.splitlines(True),after.splitlines(True),fromfile='a/PKGBUILD',tofile='b/PKGBUILD',n=1))}
        for name,text in files.items(): write(material/name,text)
        url=url or f'https://aur.archlinux.org/{base}.git'
        key=hashlib.sha256(b'moguet-aur-recipe-patch-v2\0aur\0'+url.encode()+b'\0'+base.encode()).hexdigest()
        record=self.root/'config/moguet/patches.d'/f'{key}.toml'
        entries=', '.join('{file='+json.dumps(name)+', sha256="'+hashlib.sha256(text.encode()).hexdigest()+'"}' for name,text in files.items())
        write(record, f'schema_version=2\nsource_kind="aur"\nsource_url={json.dumps(url)}\npackage_base={json.dumps(base)}\nmaterial_root={json.dumps(str(material))}\npatches=[{entries}]\n', 0o600)
        record.parent.chmod(0o700)
        record.parent.parent.chmod(0o700)
        return record,material

    def run_upgrade(self, route='upgrade-aur', choices=('y',), ok=True, tty=True, options=(), before_answer=None, allow_retained=False):
        self.command_log.write_text(''); self.eval_log.write_text(''); self.build_log.write_text(''); self.git_log.write_text('')
        if route=='upgrade':
            for base in self.packages: write(self.root/'config/moguet/source-build.d'/base, '', 0o600)
            (self.root/'config/moguet').chmod(0o700)
            (self.root/'config/moguet/source-build.d').chmod(0o700)
        argv=[BINARY,'--noedit',*options,route]
        if not tty:
            result=subprocess.run(argv,input='yes\n',env=self.env,text=True,capture_output=True,timeout=60)
            code,text=result.returncode,result.stdout+result.stderr
        else:
            pid,fd=pty.fork()
            if pid==0: os.execve(BINARY,argv,self.env)
            output=bytearray(); answered=0; selection=0; deadline=time.monotonic()+70
            try:
                while True:
                    require(time.monotonic()<deadline,'PTY timeout: '+output.decode(errors='replace'))
                    ready,_,_=select.select([fd],[],[],.1)
                    if not ready: continue
                    try: data=os.read(fd,65536)
                    except OSError: data=b''
                    if not data: break
                    output.extend(data)
                    prompts=list(re.finditer(rb':: ([^\r\n]+?) \[(?:Y/n|y/N|y/n)\] ',output))
                    for match in prompts[answered:]:
                        question=match.group(1).decode(errors='replace')
                        answer='n' if question.startswith(('Show ', 'Edit ', 'Clean ', 'Rebuild ')) else 'y'
                        if question.startswith('Apply saved patch customization to this update of '):
                            require(selection<len(choices),'unexpected repeated patch selection')
                            answer=choices[selection]; selection+=1
                        if before_answer: before_answer(question)
                        os.write(fd,b'\x04' if answer=='EOF' else (answer+'\n').encode())
                        answered+=1
                _,status=os.waitpid(pid,0); code=os.waitstatus_to_exitcode(status)
            except BaseException:
                try: os.killpg(pid,signal.SIGKILL)
                except ProcessLookupError: pass
                os.waitpid(pid,0)
                raise
            finally: os.close(fd)
            text=output.decode(errors='replace').replace('\r','')
        require((code==0)==ok,f'{argv}: exit={code}\n{text}\nGit (last calls):\n'+ '\n'.join(self.git_log.read_text().splitlines()[-8:]))
        if not allow_retained:
            require(not list((self.root/'cache/moguet').glob('.local-source-workspace*')), 'candidate was not cleaned')
        return text

    def native_install_effect(self):
        self.env['MOGUET_TEST_PATCH_NATIVE_DB']='1'
        write(self.root/'native-db-fixture','moguet-upgrade-patch-cli\n')

    def probe_aur(self,base='patch-upgrade',version='2'):
        with tarfile.open(self.root/'archives'/f'{base}-{version}-1-any.pkg.tar') as archive:
            return archive.extractfile('usr/share/patch-cli/probe').read().decode()


with tempfile.TemporaryDirectory(prefix='moguet-upgrade-patch-cli-') as temporary:
    root=Path(temporary)
    names=('patch-upgrade','patch-second','patch-split-child','patch-split-base','patch-devel','patch-forced-git',
           'patch-blocked-git','patch-cycle-a','patch-cycle-b','patch-provider','patch-consumer',
           'patch-new-root','patch-new-dep','patch-unused-root','patch-unused-b','patch-unused-c','patch-external-root','patch-pref-blocked')
    packages={name:{'Name':name,'PackageBase': 'patch-split-base' if name=='patch-split-child' else name,
                    'Version':'1-1' if name in ('patch-forced-git','patch-blocked-git') else '2-1','Description':'fixture','Depends':[],'MakeDepends':[], 'CheckDepends':[],
                    'Provides':[], 'Conflicts':[], 'Replaces':[], 'Maintainer':'fixture'} for name in names}
    packages['patch-cycle-a']['Depends']=['patch-cycle-b']
    packages['patch-unused-b']['Depends']=['patch-unused-c']
    packages['patch-provider']['Provides']=['old-virtual']
    packages['patch-external-root']['Depends']=['patch-upgrade']
    packages['patch-pref-blocked']['Depends']=['missing-stock-dependency']
    fixture=root/'rpc.json'; fixture.write_text(json.dumps({'packages':packages, 'providers': {'old-virtual':['patch-provider']}}))
    port=root/'port'
    server=subprocess.Popen([sys.executable,str(REPO/'tests/aur_rpc_fixture_server.py'),str(fixture),str(port)],stdout=subprocess.DEVNULL)
    try:
        deadline=time.monotonic()+10
        while not port.exists():
            require(server.poll() is None and time.monotonic()<deadline,'RPC fixture failed'); time.sleep(.02)
        rpc='http://127.0.0.1:'+port.read_text().strip()+'/rpc/'
        for route in ('upgrade','upgrade-aur','upgrade-all'):
            c=UpgradeCase(root/route,rpc); record,material=c.save('patch-upgrade')
            saved=record.read_bytes()
            text=c.run_upgrade(route)
            require(text.count('Saved patch customization found for patch-upgrade.')==1,'route missed or repeated discovery')
            require(text.index('Running: git clone') < text.index('Cloning into'), 'source command was presented after execution')
            require(c.probe_aur()=='2:custom:stock','route did not build selected recipe')
            require(record.read_bytes()==saved,'consumer rewrote association')
            require((c.root/'upstreams/patch-upgrade/PKGBUILD').read_text()==recipe('patch-upgrade'),'consumer changed upstream')
        c=UpgradeCase(root/'absence',rpc)
        text=c.run_upgrade(choices=())
        require('Saved patch customization found' not in text and c.probe_aur()=='2:before:stock','absence changed stock behavior')
        for mode in ('no','missing','changed','noconfirm','non-tty'):
            c=UpgradeCase(root/mode,rpc); record,material=c.save('patch-upgrade'); saved=record.read_bytes()
            if mode=='missing': (material/'one.patch').unlink()
            if mode=='changed': (material/'one.patch').write_text('changed external material')
            libc=ctypes.CDLL(None,use_errno=True); fd=libc.inotify_init1(os.O_NONBLOCK|os.O_CLOEXEC)
            require(fd>=0,'inotify failed')
            require(libc.inotify_add_watch(fd,os.fsencode(material),0x1|0x20)>=0,'material watch failed')
            try:
                text=c.run_upgrade(choices=('n',),tty=mode!='non-tty',options=('--noconfirm',) if mode=='noconfirm' else ())
                try: events=os.read(fd,65536)
                except BlockingIOError: events=b''
                require(not events,'No opened/read external material')
            finally: os.close(fd)
            require(c.probe_aur()=='2:before:stock' and record.read_bytes()==saved,'No did not retain stock/association')
        c=UpgradeCase(root/'reuse',rpc); c.save('patch-upgrade'); c.run_upgrade()
        c.add_upstream('patch-upgrade',recipe('patch-upgrade','3'))
        c.run_upgrade(); require(c.probe_aur(version='3')=='3:custom:stock','old candidate reused instead of current upstream')
        c=UpgradeCase(root/'mixed',rpc,('patch-upgrade','patch-second'))
        c.save('patch-upgrade'); c.save('patch-second')
        c.run_upgrade(choices=('n','y'))
        outputs={c.probe_aur(name) for name in c.packages}
        require(outputs=={'2:before:stock','2:custom:stock'},'selection leaked between PackageBases')
        # A material path can change after acquisition; only the already-owned
        # digest-verified bytes may reach Git check/apply.
        c=UpgradeCase(root/'owned-bytes',rpc); record,material=c.save('patch-upgrade')
        replaced=[False]
        def replace_material(question):
            if question.startswith('Evaluate PKGBUILD') and not replaced[0]:
                (material/'one.patch').write_text('changed after strict acquisition')
                replaced[0]=True
        c.run_upgrade(before_answer=replace_material)
        require(replaced[0] and c.probe_aur()=='2:custom:stock','material was reopened after digest verification')
        # The child label's association must not select a different PackageBase.
        c=UpgradeCase(root/'split-child',rpc,('patch-split-child',))
        c.add_upstream('patch-split-base',recipe('patch-split-base',child='patch-split-child'))
        c.install_record('patch-split-child',base='patch-split-base')
        c.save('patch-split-child')
        text=c.run_upgrade(choices=())
        require('Saved patch customization found' not in text and c.probe_aur('patch-split-child')=='2:before:stock','child-only association was selected')
        c.save('patch-split-base'); c.run_upgrade()
        require(c.probe_aur('patch-split-child')=='2:custom:stock','resolved PackageBase association not selected')
        # Registry corruption is failure even when a corrupt record is not the
        # requested identity. These failures must precede any source acquisition.
        for mode in ('corrupt','unsupported','key-mismatch','wrong-url'):
            c=UpgradeCase(root/('registry-'+mode),rpc)
            record,material=c.save('patch-upgrade',url='https://example.invalid/patch-upgrade.git' if mode=='wrong-url' else None)
            if mode=='corrupt': record.write_text('schema_version =')
            if mode=='unsupported': record.write_text(record.read_text().replace('schema_version=2','schema_version=99'))
            if mode=='key-mismatch': record.rename(record.with_name('0'*64+'.toml'))
            text=c.run_upgrade(ok=False,choices=())
            require(not c.git_log.read_text() and not c.build_log.read_text() and 'Apply saved patch' not in text,'registry failure became absence or execution')
        for mode in ('missing','changed','shape'):
            c=UpgradeCase(root/('yes-'+mode),rpc); record,material=c.save('patch-upgrade')
            if mode=='missing': (material/'one.patch').unlink()
            if mode=='changed': (material/'one.patch').write_text('changed')
            if mode=='shape':
                record,material=c.save('patch-upgrade',files={'one.patch':'arbitrary shell text\n'})
            text=c.run_upgrade(ok=False)
            require('Saved patch customization failed' in text and not c.git_log.read_text() and not c.build_log.read_text(), 'invalid selected material reached acquisition/build')
        for mode in ('base','child','metadata','dependency','conflict','upstream-child'):
            c=UpgradeCase(root/('failure-'+mode),rpc)
            before=c.recipes['patch-upgrade']
            after=before.replace("pkgdesc='before'", "pkgdesc='custom'")
            if mode=='base': after=after.replace('pkgbase=patch-upgrade','pkgbase=wrong-base')
            if mode=='child': after=after.replace("pkgname=('patch-upgrade')", "pkgname=('wrong-child')")
            if mode=='metadata': after=after.replace("pkgdesc='custom'", "pkgdesc='custom'\nexit 7")
            if mode=='dependency': after=after.replace('depends=()', "depends=('missing-custom-dependency')")
            c.save('patch-upgrade',after)
            if mode=='conflict': c.add_upstream('patch-upgrade',before.replace("pkgdesc='before'", "pkgdesc='upstream'").replace('pkgver=2','pkgver=3'))
            if mode=='upstream-child': c.add_upstream('patch-upgrade',before.replace("pkgname=('patch-upgrade')", "pkgname=('wrong-child')"))
            text=c.run_upgrade(ok=False)
            require(not c.build_log.read_text() and not c.command_log.read_text() and "'makepkg' '-sc'" not in text,'selected failure fell through to stock/build/install')
        c=UpgradeCase(root/'fresh-dependency',rpc)
        c.save('patch-upgrade',c.recipes['patch-upgrade'].replace('depends=()', "depends=('patch-dep')"))
        c.run_upgrade()
        with tarfile.open(c.root/'archives/patch-upgrade-2-1-any.pkg.tar') as archive:
            require('depend = patch-dep' in archive.extractfile('.PKGINFO').read().decode(),'fresh dependency absent from archive')
        c=UpgradeCase(root/'candidate-drift',rpc); c.save('patch-upgrade')
        def drift(question):
            if question=='Proceed with build?':
                candidate=Path(c.eval_log.read_text().splitlines()[-1].split('|')[0])/'PKGBUILD'
                candidate.write_text(candidate.read_text()+'\n# external candidate change\n')
        c.run_upgrade(ok=False,before_answer=drift)
        require(not c.build_log.read_text() and not c.command_log.read_text(),'changed candidate reached build/install')
        # Ordinary version update: current upstream is authoritative before an
        # overlay could silently change the selector to Legacy.
        c=UpgradeCase(root/'ordinary-devel',rpc,('patch-devel',))
        c.add_upstream('patch-devel',recipe('patch-devel').replace('depends=()', "depends=()\nsource=('git+https://example.invalid/upstream.git')\nsha256sums=('SKIP')"))
        c.save('patch-devel')
        text=c.run_upgrade(ok=False)
        require('not supported by authoritative devel execution' in text and not c.build_log.read_text() and not c.eval_log.read_text(),'ordinary devel was downgraded or evaluated')
        # Existing devel-observation stub supplies GitRevision intent; all patch
        # discovery/selection/failure handling and the public route are real.
        c=UpgradeCase(root/'forced-devel',rpc,('patch-forced-git',))
        c.save('patch-forced-git'); c.env['MOGUET_TEST_INSPECTION_SCENARIO']='foreign-authoritative-different'
        text=c.run_upgrade(ok=False)
        require('not supported by authoritative devel execution' in text and not c.git_log.read_text() and not c.eval_log.read_text(),'forced devel entered candidate acquisition')
        c=UpgradeCase(root/'registered-current',rpc); c.save('patch-upgrade'); c.install_record('patch-upgrade','2-1')
        c.run_upgrade('upgrade')
        require(not c.build_log.read_text() and not c.command_log.read_text(),'up-to-date registered source built after Yes')
        c=UpgradeCase(root/'system-failure',rpc); c.save('patch-upgrade'); c.env['PATCH_SYSTEM_EXIT']='17'
        c.run_upgrade('upgrade',ok=False)
        require(not c.build_log.read_text() and not c.command_log.read_text(),'system failure continued into custom build/install')
        # Dry-run and exact targetless forms never even read a poisoned registry.
        for route in ('upgrade','upgrade-aur','upgrade-all','-Su','-Syu'):
            c=UpgradeCase(root/('zero-read-'+route.replace('-','')),rpc); record,material=c.save('patch-upgrade')
            record.write_text('schema_version = malformed')
            fd=libc.inotify_init1(os.O_NONBLOCK|os.O_CLOEXEC)
            require(libc.inotify_add_watch(fd,os.fsencode(record),0x1|0x20)>=0,'registry watch failed')
            try:
                opts=('--dry-run',) if route.startswith('upgrade') else ('--noconfirm',)
                text=c.run_upgrade(route,choices=(),options=opts)
                try: events=os.read(fd,65536)
                except BlockingIOError: events=b''
                require(not events and 'Saved patch customization found' not in text,'zero-read route consulted patch registry')
                if '--dry-run' in opts:
                    require(not c.git_log.read_text() and not c.eval_log.read_text() and not c.build_log.read_text(),'dry-run acquired/evaluated a candidate')
            finally: os.close(fd)
        # Stock graph blockers keep their original preference zero-read and
        # typed failure authority, including after an explicit patch No.
        for choice in ('absent','no'):
            c=UpgradeCase(root/('preference-blocker-'+choice),rpc,('patch-pref-blocked',))
            if choice=='no': c.save('patch-pref-blocked')
            preferences=c.root/'config/moguet/source-build.d';preferences.mkdir(parents=True,exist_ok=True);preferences.chmod(0o700)
            (c.root/'config/moguet').chmod(0o700)
            target=c.root/'unsafe-preference-target';write(target,'PATCH_FLAG=unexpected\n')
            (preferences/'patch-pref-blocked').symlink_to(target)
            fd=libc.inotify_init1(os.O_NONBLOCK|os.O_CLOEXEC)
            require(fd>=0 and libc.inotify_add_watch(fd,os.fsencode(preferences),0x1|0x20)>=0,'preference watch failed')
            try:
                text=c.run_upgrade(choices=('n',) if choice=='no' else (),ok=False,options=('--details',))
                try: events=os.read(fd,65536)
                except BlockingIOError: events=b''
                require(not events and 'missing-stock-dependency' in text and not c.git_log.read_text(),
                        'No/absence changed stock graph/preference authority: '+text)
            finally: os.close(fd)
        c=UpgradeCase(root/'selected-preference',rpc);c.save('patch-upgrade')
        preference=c.root/'config/moguet/source-build.d/patch-upgrade';write(preference,'PATCH_FLAG=from-preference\n',0o600);preference.parent.chmod(0o700)
        c.run_upgrade()
        require(c.probe_aur()=='2:custom:from-preference','selected recipe lost its existing saved build environment')
        # Ordered AUR series: the second input only applies after the first.
        c=UpgradeCase(root/'ordered',rpc)
        before=c.recipes['patch-upgrade']; first=before.replace("pkgdesc='before'","pkgdesc='first'"); second=first.replace("pkgdesc='first'","pkgdesc='second'")
        def delta(a,b): return 'diff --git a/PKGBUILD b/PKGBUILD\n'+''.join(difflib.unified_diff(a.splitlines(True),b.splitlines(True),fromfile='a/PKGBUILD',tofile='b/PKGBUILD',n=1))
        c.save('patch-upgrade',files={'first.patch':delta(before,first),'second.patch':delta(first,second)})
        c.run_upgrade(); require(c.probe_aur()=='2:second:stock','AUR ordered series was reordered')
        c=UpgradeCase(root/'mixed-absence',rpc,('patch-upgrade','patch-second'))
        c.save('patch-second'); text=c.run_upgrade(choices=('y',))
        require(text.count('Apply saved patch customization to this update of ')==1 and c.probe_aur()=='2:before:stock' and c.probe_aur('patch-second')=='2:custom:stock','absence borrowed another selection')
        c=UpgradeCase(root/'preserve-reason',rpc);c.native_install_effect();c.save('patch-upgrade')
        desc=c.root/'db/local/patch-upgrade-1-1/desc';desc.write_text(desc.read_text().replace('%REASON%\n0\n','%REASON%\n1\n'))
        c.run_upgrade()
        require('%REASON%\n1\n' in (c.root/'db/local/patch-upgrade-2-1/desc').read_text(),'custom update promoted a dependency-installed root')
        # Both recipes replace stale RPC graph facts: stock A -> B becomes
        # custom B -> A. The archive and build-event order must follow the latter.
        c=UpgradeCase(root/'fresh-order',rpc,('patch-cycle-a','patch-cycle-b'))
        c.native_install_effect()
        c.save('patch-cycle-a',c.recipes['patch-cycle-a'].replace("pkgdesc='before'","pkgdesc='A'"))
        c.save('patch-cycle-b',c.recipes['patch-cycle-b'].replace("pkgdesc='before'","pkgdesc='B'").replace('depends=()',"depends=('patch-cycle-a')"))
        c.run_upgrade(choices=('y','y'))
        require(c.build_log.read_text().splitlines()==['2:A:stock','2:B:stock'],'fresh combined graph did not replace stock order')
        # RPC advertises only old-virtual. The selected current recipe provides
        # new-virtual; provider discovery and refresh must share fresh metadata.
        c=UpgradeCase(root/'fresh-provider',rpc,('patch-provider','patch-consumer'))
        c.native_install_effect()
        c.save('patch-provider',c.recipes['patch-provider'].replace("pkgdesc='before'","pkgdesc='provider'").replace('depends=()',"depends=()\nprovides=('new-virtual')"))
        c.save('patch-consumer',c.recipes['patch-consumer'].replace("pkgdesc='before'","pkgdesc='consumer'").replace('depends=()',"depends=('new-virtual')"))
        c.run_upgrade(choices=('y','y'))
        require(c.build_log.read_text().splitlines()==['2:provider:stock','2:consumer:stock'],'fresh Provides did not determine provider/order')
        # A new dependency has an independent saved series, discovered only
        # after the root's modified metadata makes it a source-build candidate.
        c=UpgradeCase(root/'new-dependency',rpc,('patch-new-root',)); c.native_install_effect()
        c.add_upstream('patch-new-dep',recipe('patch-new-dep'))
        c.save('patch-new-root',c.recipes['patch-new-root'].replace("pkgdesc='before'","pkgdesc='root'").replace('depends=()',"depends=('patch-new-dep')"))
        c.save('patch-new-dep',c.recipes['patch-new-dep'].replace("pkgdesc='before'","pkgdesc='dependency'"))
        c.run_upgrade(choices=('y','y'))
        require(c.build_log.read_text().splitlines()==['2:dependency:stock','2:root:stock'],'new dependency did not get its own selected candidate')
        require('%REASON%\n1\n' in (c.root/'db/local/patch-new-dep-2-1/desc').read_text(),'dependency install reason changed')
        # Explicit source satisfaction must exclude dependency B before later
        # AUR patch lookup, even though B is still present in the raw RPC graph.
        c=UpgradeCase(root/'external-satisfaction',rpc,('patch-upgrade','patch-external-root'))
        c.save('patch-upgrade')
        preference=c.root/'config/moguet/source-build.d/patch-upgrade'; write(preference,'',0o600);preference.parent.chmod(0o700)
        text=c.run_upgrade('upgrade-all',choices=('y',))
        require(text.count('Apply saved patch customization to this update of patch-upgrade?')==1,'externally satisfied source was selected again')
        require(c.build_log.read_text().splitlines()==['2:custom:stock','2:before:stock'],'externally satisfied source built twice')
        for version,ok in (('3-1',True),('0-1',False)):
            c=UpgradeCase(root/('post-system-'+version),rpc); c.save('patch-upgrade');c.env['PATCH_SYSTEM_VERSION']=version
            c.run_upgrade('upgrade',ok=ok)
            require(not c.build_log.read_text() and not c.command_log.read_text(),'pre-system update decision was reused after DB change')
        c=UpgradeCase(root/'query-blocker',rpc,('patch-upgrade','patch-blocked-git'));c.save('patch-upgrade')
        c.run_upgrade(choices=(),ok=False)
        require(not c.git_log.read_text() and not c.eval_log.read_text(),'query-level blocker entered dependency patch discovery')
        for route in ('upgrade','upgrade-aur','upgrade-all'):
            for response in ('q','EOF'):
                c=UpgradeCase(root/(route+'-cancel-'+response),rpc);c.save('patch-upgrade')
                text=c.run_upgrade(route,choices=(response,),ok=False,options=('--details',) if route=='upgrade-all' else ())
                require('cancelled' in text.lower() and 'Unexpected' not in text and not c.git_log.read_text() and not c.eval_log.read_text(),'confirmation stop lost its disposition')
                if route=='upgrade-all':
                    require((c.root/'system-log').exists() and 'system/source: Verified unchanged' in text and
                            'diagnostic: Cancelled' in text and 'failures: 0' in text,
                            'late cancellation lost completed system phase or became execution failure: '+text)
        c=UpgradeCase(root/'build-failure',rpc)
        c.save('patch-upgrade',c.recipes['patch-upgrade'].replace('build() {', 'build() { return 7;'))
        text=c.run_upgrade(ok=False)
        require(not c.command_log.read_text() and not list((c.root/'archives').iterdir()),'build failure fell back or installed')
        c=UpgradeCase(root/'cleanup-failure',rpc);c.native_install_effect();c.save('patch-upgrade')
        c.env['MOGUET_TEST_PATCH_CANDIDATE_CLEANUP_FAILURE']='1'
        text=c.run_upgrade(ok=False,allow_retained=True)
        require(len(list((c.root/'cache/moguet').glob('*.cleanup-displaced')))==1,'cleanup failure was not injected')
        require('cleanup failed' in text and 'updated, but cleanup failed' in text and c.probe_aur()=='2:custom:stock','cleanup failure lost successful install or became success')
        # C is selected from stock B's dependency graph, then B's fresh metadata
        # removes C. The unused candidate must be cleaned without being built.
        c=UpgradeCase(root/'unused-cleanup',rpc,('patch-unused-root',))
        for name in ('patch-unused-b','patch-unused-c'): c.add_upstream(name,recipe(name));c.save(name)
        c.save('patch-unused-root',c.recipes['patch-unused-root'].replace('depends=()',"depends=('patch-unused-b')"))
        c.native_install_effect()
        text=c.run_upgrade(choices=('y','y','y'))
        require(c.build_log.read_text().splitlines()==['2:custom:stock','2:before:stock'] and
                not (c.root/'archives/patch-unused-c-2-1-any.pkg.tar').exists(),
                'unused selected candidate was built')
        print('upgrade patch CLI passed: three routes; exact base; No material zero-read; strict Yes; owned bytes; current upstream; fresh metadata; devel rejection; independent choices; zero-read routes')
    finally:
        server.terminate(); server.wait(timeout=5)
