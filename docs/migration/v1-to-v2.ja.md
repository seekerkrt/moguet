# jpacker v1からMoguet v2への移行

[English](v1-to-v2.md)

<!-- parity:overview -->
## 概要

Moguet v2.0.0はjpacker v1.16.0からのbreaking transitionです。pacman-firstな実行
基盤を維持しながら、public identity、user storage、config形式、localized
documentationをまとめて変更します。

このGuideは次の非破壊な順序を定めます。

```text
jpacker v1 dataを調査・backup
-> jpackerとcoexistさせて検証済みMoguet v2 packageをinstall
-> 理解できる設定だけを手動移行
-> command、man、completion、locale、pathを検証
-> rollback検証後に必要ならjpackerをremove
```

Moguetは`/etc/jpacker/jpacker.conf`を通常config layerとして使用しません。
`/etc/jpacker`を自動copy・rewrite・merge・deleteせず、root-owned dataを受け取るuserを
推測しません。v2.0.1以降、`/etc/jpacker/package.build/`も手動migration専用のlegacy
inputです。Moguetはruntimeでこれをread / writeしません。v2.0.0のtag、Release、release
notesは変更しないhistorical artifactとして維持します。

<!-- parity:preparation -->
## 始める前に

1. package操作を完了または停止します。pacman、makepkg、他のpackage helperがsystemを
   変更している間にmigrationを始めないでください。
2. installed legacy packageがjpacker v1.16.0であることと、そのinstall方法を記録します。
3. release固有のMoguet package手順を確認します。v2 packageはjpacker v1.16.0とのfile
   conflictやmetadata関係を持たないため、検証中は両方をinstallできます。`jpacker`
   command aliasは提供しません。
4. rollback用に、信頼できるjpacker v1.16.0 packageまたはsource archiveを確保します。
5. local userごとにmigrationを分けます。root-owned legacy directoryだけでは移行先userを
   特定できません。

変更前にinstalled packageを記録する例です。

```bash
pacman -Q jpacker
pacman -Qi jpacker
```

これらがpackageなしと報告した場合は、package removal commandを使う前に実際のinstall
方法を特定してください。

<!-- parity:identity -->
## Identityの変更

| Surface | jpacker v1.16.0 | Moguet v2.0.0 |
| --- | --- | --- |
| Project / brand | jpacker | Moguet |
| 読み | — | モグエット |
| CLI / binary | `jpacker` | `moguet` |
| Package | `jpacker` | `moguet` |
| XDG application名 | legacy jpacker path | `moguet` |
| Project environment prefix | legacy name | `MOGUET_*` |
| Runtime localization | legacy English-only surface | English authority、日本語正式翻訳 |

正式なproject表記は`Moguet`、読みは「モグエット」です。command / option tokenは
翻訳しません。正式なv2 commandは`moguet`です。localで`jpacker` symlinkを作り、packaging
supportがあると仮定しないでください。
packageは`jpacker`への`provides`、`conflicts`、`replaces`を意図的に宣言しません。旧commandを
実装せず、rollback packageを暗黙のupgradeとして削除しないためです。

canonical source identityはGitHub / GitLab上の`seekerkrt/moguet`です。旧
`seekerkrt/jpacker` URLはredirect専用のlegacy entry pointであり、別repositoryへ再利用
しません。

<!-- parity:backup -->
## v1 dataをbackupする

どちらかのpackageを変更する前にprivate backupを作成します。最低でも次を保持してください。

- installed jpacker versionとpackage metadata
- `/etc/jpacker/jpacker.conf`（存在する場合）
- `/etc/jpacker/package.build/`の内容・ownership・mode（存在する場合）
- `LOGFILE`が指定するcustom log
- 通常のpackage payload外で変更したpackage / source file

archive方法の一例です。

```bash
backup_root="$HOME/moguet-migration-backup-$(date +%Y%m%d-%H%M%S)"
install -d -m 700 "$backup_root"
pacman -Q jpacker > "$backup_root/jpacker-package.txt"
pacman -Qi jpacker > "$backup_root/jpacker-package-info.txt"
sudo tar --acls --xattrs -C /etc -cpf - jpacker \
    > "$backup_root/etc-jpacker.tar"
tar -tf "$backup_root/etc-jpacker.tar"
```

`/etc/jpacker`が存在しない場合だけ、そのarchive stepをskipします。失敗したarchiveや
empty archiveはbackupではありません。command statusとlistingを確認してください。
設定を失う影響が大きい場合はmachine外にもcopyを保管します。

test対象userに既存Moguet XDG directoryがある場合は、それも先に保持します。standard
locationは次のとおりです。

```text
${XDG_CONFIG_HOME:-$HOME/.config}/moguet
${XDG_STATE_HOME:-$HOME/.local/state}/moguet
${XDG_CACHE_HOME:-$HOME/.cache}/moguet
```

backup時にこれらを`/etc/jpacker`とmergeしないでください。

<!-- parity:remove-v1 -->
## jpacker v1.16.0を保持するか明示的にremoveする

検証済みMoguetとjpacker v1.16.0のpayloadに共通fileはありません。可能ならMoguetの確認中は
jpackerをinstallしたまま保持してください。Moguetをremoveして変更前の`jpacker` commandへ
戻れるため、最短のrollbackになります。source-preference storeは別々ですが、両helperが
同じsystem package toolを呼ぶ可能性があるため、package-mutating operationを同時実行しないで
ください。

Moguetの検証後、必要ならinstallに使ったpackage managerでjpackerを明示的にremoveできます。
通常のpacman-managed installでは、慎重な形式は次です。

```bash
sudo pacman -R jpacker
```

renameだけを理由にrecursive dependency cleanupを追加しないでください。confirm前にpacmanが
提案するtransactionを確認します。別の方法でinstallしたpackageは、pacmanがownerであると
見なさず、その方法のdocumented uninstall pathを使ってください。

package removalを`/etc/jpacker`のcleanup commandとして使ってはいけません。migrationと
rollback validationの両方が完了するまで、backup、preserved `.pacsave`、preference fileを
保持します。

<!-- parity:install-v2 -->
## Moguet v2をinstallする

release固有手順でv2 package source、signature / checksum、dependency、file conflict監査、
payloadを検証してからMoguetをinstallします。jpackerを先にremoveする必要はありません。

package identityは`moguet`、唯一のexecutableは`/usr/bin/moguet`で、
`/usr/bin/jpacker` aliasはありません。package metadataはjpackerへの`provides`、
`conflicts`、`replaces`を持ちません。coexistenceはtransition / rollback特性であり、Moguetが
jpacker interfaceを提供するという意味ではありません。Moguet v2.0.0にはAUR publicationを
含めないため、このGuideはAUR URLや`pacman -S`のrepository commandを作りません。検証済み
transitionの代わりにdevelopment treeの`make install`を旧packageへ重ねないでください。

package installは`/etc/moguet`、user XDG config file、user XDG state / cache directoryを
作成せず、source-preference directoryも作成しません。commandは、そのoperationが実際に
必要とするuser directoryだけを、実行user自身のXDG context内へ作成します。

<!-- parity:configuration -->
## Configを手動移行する

Moguetは次のoptionalなuser-owned fileを使います。

```text
$XDG_CONFIG_HOME/moguet/config.toml
fallback: ~/.config/moguet/config.toml
```

non-default値を必要とする特定userについてだけ作成します。minimal schemaは次です。

```toml
schema_version = 1

[review]
pkgbuild = "prompt"
diff = "prompt"

[build]
mode = "normal"
```

理解できるjpacker v1 keyだけを対応付けます。

| jpacker v1 setting | Moguet v2 action |
| --- | --- |
| `NOEDIT=true` | `review.pkgbuild = "skip"`を設定 |
| `NOEDIT=false` | keyを省略、または`review.pkgbuild = "prompt"` |
| `NODIFF=true` | `review.diff = "skip"`を設定 |
| `NODIFF=false` | keyを省略、または`review.diff = "prompt"` |
| `EDITOR=...` | TOMLへcopyせず、user environmentの`VISUAL`、次に`EDITOR`を設定 |
| `LOGFILE=...` | v2.0.0 config keyなし。固定XDG state logを使用 |
| `RMDEPS=true` | 移行しない。v2には永続的な`RMDEPS` config keyはない。dependency cleanupはinvocationごとに明示する`--rmdeps` requestであり、現在はremote AUR buildだけをsupportする。 |

editor解決順は`VISUAL -> EDITOR -> nano`です。default logは次です。

```text
$XDG_STATE_HOME/moguet/moguet.log
fallback: ~/.local/state/moguet/moguet.log
```

Moguetはconfigをstrictなread-only inputとして読みます。fileが存在すれば
`schema_version = 1`が必須で、unknown key、invalid type / enum、future schema versionは
external mutation前に失敗します。broken fileをrewriteしたり、黙ってdefaultへfallback
したりしません。

<!-- parity:legacy-data -->
## Legacy preferenceとdataを扱う

Moguetのcanonicalなsource-build preference entryは次です。

```text
${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/<package-name>
```

`XDG_CONFIG_HOME`がunsetまたはemptyなら`$HOME/.config`へfallbackします。明示した
`XDG_CONFIG_HOME`はabsoluteで、安全かつ既存のdirectoryでなければならず、relativeまたは
unsafeな値はfail-closedで拒否します。fallback pathは最初のwriteで必要になったときだけ
安全に作成できます。rootでCLIを実行した場合も実行user自身のXDG contextを使い、
`SUDO_USER`から別userを推測しません。

全source-preference commandとbuild / upgrade readerはこのauthorityだけを使います。read、
`moguet list-src`、missingな`moguet del-src` / `moguet revert`は、missingなsource-preference
storeを作成しません。
storageを最初に必要とする`moguet add-src <pkg> [V=K]`または
`moguet edit-src <pkg>`だけがsource-preference operationとしてmanagedな
`moguet/source-build.d`階層をmode `0700`で作成し、
entryはmode `0600`にします。preferenceのfilesystem操作は`sudo`を使いません。
`moguet revert`は別責務であるpacman transactionに限り`sudo`を使うことがあります。

`/etc/jpacker/package.build/`は手動migration専用のlegacy inputとして扱います。Moguetは
runtimeでread、fallback、merge、rewrite、deleteせず、legacy entryをXDG storeへ自動copy
しません。package installer / reinstaller / uninstallerはlegacyとcanonicalの両entryを
保持し、user XDG directoryをcreate / removeしません。

理解した1 packageを、明示的に選んだ1 userについて順に移行します。

1. 検証済みlegacy backupを保持し、legacy entryを変更せずに確認します。
2. 対象userとしてcanonical destination entryがabsentであることを確認します。symlinkや
   unexpected objectはabsent entryではなく、overwriteせずに調査します。
3. `XDG_CONFIG_HOME`を明示する場合、Moguetを実行する前にbase directoryを別途作成・検証
   します。relativeまたはshared directoryを指定しないでください。
4. 理解したassignmentだけを、`sudo`なしで次のどちらかから再入力します。

   ```bash
   moguet add-src <package-name> [V=K ...]
   moguet edit-src <package-name>
   ```

5. そのpackageを移行済みと判断する前に、結果のsnapshot全体を検証します。

   ```bash
   moguet list-src
   ```

bulk loopを自動化せず、canonical entryをoverwriteせず、未確認の値をmergeしないでください。
package nameはdirectory作成やeditor実行より前に検証されます。missing store / entryだけが
「absent」です。invalid entry name、symlink、non-regular file、ownership / mode違反、
permission / I/O error、検出したraceはhard errorです。`moguet list-src`は何も出力する前に
snapshot全体を検証するため、不正entryを含むpartial listingはtrusted resultになりません。

Moguetは次を自動実行しません。

- `/etc/jpacker/jpacker.conf`をconfig layerとして読む
- `/etc/jpacker`を1人以上のuser homeへcopyする
- legacy source-preference fileをread / merge / delete / rewriteする
- `/etc/moguet`を作成・参照する
- `LOGFILE`、`RMDEPS`、arbitrary editor command、credential、shell fragment、unknown
  uppercase keyを移行する
- root ownership、`sudo`、`SUDO_USER`から移行先userを推測する

config、source preference、state、cacheはv2で別の責務を持ちます。

```text
config:             $XDG_CONFIG_HOME/moguet/config.toml
source preferences: $XDG_CONFIG_HOME/moguet/source-build.d/
state:              $XDG_STATE_HOME/moguet/  (fallback ~/.local/state/moguet/)
cache:              $XDG_CACHE_HOME/moguet/  (fallback ~/.cache/moguet/)

config/source-preference fallback: ~/.config/moguet/
```

cacheは再生成可能でありbackup先ではありません。Moguet uninstallをuser config、state、
cacheの削除許可として扱わないでください。

<!-- parity:verification -->
## Migrationを検証する

対象の通常userとして、`sudo`を付けずに確認します。

```bash
command -v moguet
moguet --version
LC_ALL=C moguet --help
```

次にpackageが提供するdocumentationとcompletion fileを確認します。

```bash
man -w moguet
LC_ALL=C man moguet
LANG=ja_JP.UTF-8 man moguet
```

期待するstandard pathは次です。

```text
/usr/share/man/man1/moguet.1
/usr/share/man/ja/man1/moguet.1
/usr/share/bash-completion/completions/moguet
/usr/share/zsh/site-functions/_moguet
/usr/share/fish/vendor_completions.d/moguet.fish
/usr/share/locale/ja/LC_MESSAGES/moguet.mo
/usr/share/licenses/moguet/LICENSE
/usr/share/licenses/moguet/jpacker-MIT-legacy.txt
/usr/share/licenses/moguet/curl.txt
/usr/share/licenses/moguet/nlohmann-json-MIT.txt
/usr/share/licenses/moguet/tomlplusplus-MIT.txt
/usr/share/licenses/moguet/bjoern-hoehrmann-utf8-MIT.txt
/usr/share/doc/moguet/README.md
/usr/share/doc/moguet/README.ja.md
/usr/share/doc/moguet/THIRD_PARTY_NOTICES.md
/usr/share/doc/moguet/docs/LICENSING.md
/usr/share/doc/moguet/docs/migration/v1-to-v2.md
/usr/share/doc/moguet/docs/migration/v1-to-v2.ja.md
```

新しいshellを起動するか、使用shellがdocumentするcompletion mechanismだけをreloadします。
systemがman page cacheを使う場合は、通常のpackage hookまたはadministrator procedureで
更新します。completionがlegacy `jpacker` commandではなく、`moguet` commandと
`--edit`、`--diff`、`--build-mode=normal|rebuild|clean`等のfinal optionを提示することを
確認してください。

最後に、意図したuserのresolved XDG pathを確認します。help / version確認はXDG consumer
directoryを作りません。`moguet list-src`と、read / build / upgrade経路による
source-preference readは、missingなsource-preference storeを作りません。ただしこれらは
それ以外の点では通常commandです。既存のstate logging contractにより、`moguet list-src`を
含む通常commandはstate directoryとlogを作成することがあります。preferenceを移行した場合、
managed directoryがmode `0700`、entryがmode `0600`であること、legacy entryが未変更であること、
完全な`moguet list-src`出力が意図したpackageと一致することを確認します。operationのexternal
commandとpackage effectをreviewしてから実行testへ進んでください。

<!-- parity:rollback -->
## jpacker v1.16.0へrollbackする

rollbackは明示的なpackage transitionであり、自動transaction rollbackではありません。

1. Moguetを停止し、activeなpackage operationを完了します。
2. userのMoguet config、source preference、stateをbackupします。cacheは診断に必要な場合
   だけ保持します。
3. installに使ったpackage managerでMoguet packageをremoveします。package removalの一部
   としてuser XDG directoryを削除しません。
4. jpackerを保持していた場合はpackage fileとcommandが変わっていないことを確認します。
   removeしていた場合は、migration前に記録した信頼できるjpacker v1.16.0 package / source
   archiveをreinstallします。
5. current destinationを確認したうえで、verified backupからだけ`/etc/jpacker`をrestore
   します。新しいfileへ盲目的にoverwriteしないでください。
6. package transaction前に`jpacker --version`、v1 man / completion、read-only operationを
   検証します。

MoguetのXDG dataをjpacker v1は解釈しないため、後の再試行用に保持できます。
`/etc/jpacker/package.build/`へ自動同期されることもありません。完了済みpacman transactionは
helperを切り替えても戻りません。helper rollbackがinstalled packageを変更したと仮定せず、
実際のpackage database stateを比較してください。

<!-- parity:maintenance -->
## v1 maintenanceとrepository remote

jpacker v1.16.0はimmutableなtag / Releaseの`jpacker` identityで維持します。Moguetへ
名称変更せず、このGuideだけでv2 XDG / config形式を導入しません。permanentなv1 maintenance
branchは作りません。重大修正が必要な場合だけ`v1.16.0` tagからbranchを作成し、`develop`を
v1 sourceとして扱いません。

canonical repository URLは次のとおりです。

- GitHub: `https://github.com/seekerkrt/moguet`
- GitLab mirror: `https://gitlab.com/seekerkrt/moguet`

旧`https://github.com/seekerkrt/jpacker`と`https://gitlab.com/seekerkrt/jpacker`は
redirectとしてだけ保持します。old slugへ新しいprojectを作成しません。old bookmarkやv1
Release linkを利用する前に、Moguet repositoryへ到達することを確認してください。

既存cloneではremoteを調査し、設定済みremoteごとにURLを更新します。

```bash
git remote -v
git remote set-url origin https://github.com/seekerkrt/moguet.git
git remote set-url gitlab https://gitlab.com/seekerkrt/moguet.git
git remote get-url origin
git remote get-url gitlab
```

SSHを使うcloneでは、代わりに`git@github.com:seekerkrt/moguet.git`と
`git@gitlab.com:seekerkrt/moguet.git`を使用します。`gitlab` remoteが未設定なら、該当commandは
実行しません。remote URLの変更はpackage stateや`/etc/jpacker` dataを移行しません。

現在のsource contractは[README.md](../../README.md)、
[README.ja.md](../../README.ja.md)、
[compatibility.md](https://github.com/seekerkrt/moguet/blob/develop/docs/compatibility.md)を
参照してください。
