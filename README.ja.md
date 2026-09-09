# Moguet

[English](README.md)

<!-- parity:overview -->
## 概要

Moguetは、検証済みsource buildとpackageごとのbuild preferenceを提供する、
Arch Linux向けのpacman-first AUR helperです。package transactionは`pacman`、
package buildは`makepkg`、repository取得は`git`へ委ね、Moguetはplan、review、
artifact validationと各tool間の安全な引き渡しを担います。

MoguetはArch Linux、pacman、AURの公式projectではありません。独立したpackage
manager、既存AUR helperの完全なclone、pacmanやmakepkgの契約を置き換えるtoolでも
ありません。

<!-- parity:name -->
## 名称とidentity

project / brandの正式名称は **Moguet**、読みは **モグエット** です。command、
binary、package、XDG application名は`moguet`、projectが所有するenvironment
variableは`MOGUET_*` prefixを使います。

名称は、日本語の「もぐもぐ」と、小さなものを表す英語の接尾辞`-let`を出発点に
しています。そこから生まれた*Mogulet*を、フランス語で「すずらん」を意味する
*muguet*へ寄せました。小さく慎ましい姿でありながら毒を持つ花の性質を、危険や
曖昧さを警告し、authoritativeな判断ができなければexternal mutation前に停止する
小さなhelperへ重ねています。

Moguetはproject固有の造語です。正式なproject表記は **Moguet**、正式な読みは
**モグエット** です。

<!-- parity:status -->
## Project status

Moguet v2.0.0は、jpacker v1.16.0の実行基盤を土台に、identity、保存先、config、
localization、packagingを移行するbreaking releaseです。localの`moguet` binary、
XDG path、typed TOML config、gettextによる英日CLI surfaceは実装済みです。local
package identity、payload、dependency metadata、documentation、jpacker v1.16.0からの
非破壊transitionをv2 release contractとして確定しました。

Moguet v2.0.1は、採用済みXDG storage契約のうちsource-preference部分を完成させます。
新しいstorage方針の追加ではなく、v2.0.0で欠けた実装の修正です。source-build
preferenceは実行user自身のXDG config contextだけを使い、公開済みv2.0.0のtag、Release、
release noteは歴史的記録のまま変更しません。

Moguet v2.6.0は最新releaseです。exact target-less `moguet -Syu`はofficial repository
system updateを実行し、その成功後にnormal installed-AUR updateを続けます。saved
source-build preferenceは明示的なsource-aware `upgrade*` workflowだけに限定したままです。
通常の`-Syu`ではindependent devel `RequiresCheck` targetを局所的にskipし、unrelatedな
AUR updateをblockしません。一方、required `RequiresCheck` relationと明示的なupgrade
workflowはstrictなままです。利用者から見える変更の全体は
[v2.6.0 release](https://github.com/seekerkrt/moguet/releases/tag/v2.6.0)を参照してください。

canonical repository identityはGitHub上のMoguetで、GitLab mirrorを持ちます。Moguet
packageは`jpacker` command aliasを提供しません。AUR publicationは将来の別判断であり、
この文書はAUR endpointが存在すると断定しません。

Moguet v2.xは公開済みで利用できますが、完成済みの一般向けAUR helperではなく、
development-phaseのproductのままです。basicなpacman wrapper、AUR source build、
update、package別のsource-build preferenceは現在すでに動作しますが、AUR support全体と
edge case対応は段階的に実装中で、UXも成熟途上です。Moguetはfull dependency solverや
provider / conflictの自動解決を再実装せずpacman-firstを維持し、既存AUR helperと同等の
自動解決能力・完成度を約束しません。unsupportedまたはambiguousなcaseは、推測せず
fail-closedで停止します。v2.xは、Moguetのsource-aware入口、安全境界、検証基盤を
築く公開開発期です。v3.0.0は、Moguet固有のbuild-profileとPKGBUILD差分workflowが揃う
地点であり、projectは内部的にこれをMoguetの本格的な正式就役と位置付けています。詳細な
計画はrelease roadmap（[issue #344](https://github.com/seekerkrt/moguet/issues/344)）
を参照してください。

<!-- parity:safety -->
## 設計と安全境界

- `moguet`は通常ユーザーで実行します。system package transactionが必要な操作だけ
  `sudo pacman`を呼び、AUR sourceの取得・review・buildをrootでは実行しません。
- package database stateとpackage transactionのauthorityは`pacman` / libalpmです。
  package buildは`makepkg`、AUR repository取得は`git`が所有し、Moguetはこれらを
  再実装しません。
- `deps`と`plan`は調査・表示だけを行い、clone、build、installしません。`fetch`は
  未取得repositoryをcloneし、既存cloneでは`git fetch origin`だけを実行します。
  pull、merge、reset、working tree更新、build、installは行いません。
- repository metadataの取得失敗と、確認済みの不在は区別します。Auto `-Si`は不在を
  確認できた場合だけAURへfallbackし、取得失敗targetをdiagnostic付きで除外して独立targetを
  続行します。`revert`は対象ごとのmetadata確認をpreference削除前に行い、失敗targetの
  preferenceを保持し、成功したrepository targetを既存のgrouped reinstallへ渡します。
- AUR infoはinstalled stateを取得できない場合、`no`ではなく取得不能表示とwarningを出します。
  Auto searchの`[installed]`は成功したforeign inventoryへの所属だけで付け、取得不能時は
  warningとともにannotationを省略します。これらのoptional表示だけではsearch / infoの
  成功規則を変更しません。AurOnly searchはinstalled / repository metadataをqueryしません。
- dependency edgeはtyped requirement、source-aware candidate、constraint resultを保持します。
  `deps`は`Unsatisfied` / `Unknown`をwarning付きで継続し、`plan`はincompleteとして表示します。
  `Invalid` / `Conflicting`はfail-closedです。`fetch`、build、install、upgrade、local buildは
  `Unsatisfied` / `Unknown`の場合、clone、fetch、source mutation、build、sudo、pacman、transaction
  より前に停止します。
- AURの`Conflicts` / `Replaces`宣言は、provided componentとversion付きrelationを含め、
  build / install前にinstalled packageとplanned packageへ照合します。diagnosticはinstalled
  packageとのconflictとplanned targetとのconflictを分け、matching replacementはreviewが
  必要なpotential impactとしてだけ表示します。Moguetはpackageの削除、replacement targetの
  選択、conflict解決を自動実行しません。
- relation assessmentが利用不能（`Unknown`）またはinvalidならfail-closedとし、absenceとして
  表示しません。completeな観測でcurrent / planned packageまたはprovided componentに一致が
  ないと確認できた場合だけ、そのrelation guardを解除します。宣言自体は残り、transaction
  authorityはpacman / libalpmです。`-Si`はsource metadataを表示し、このstatefulな判定を
  plan / build preflightへ明示的に延期します。
- 複数provider candidateが残る場合、interactive TTYではsource-aware candidateを番号付きで
  表示し、exactly oneの明示選択を要求します。defaultはありません。empty input、`q`、
  `quit`、`cancel`、EOFは選択を取り消し、invalid / out-of-range inputは再入力します。
- interactive provider一覧では、candidateのpackage名と同名のpackageがread-only local
  package databaseにあればlocalizedな`[installed]`を末尾へ付けます。未install candidateには
  suffixを付けず、lookup不能時はwarningと`[installed state unknown]`を表示します。この
  name-only observationはprovenanceやversion compatibilityを証明せず、candidate順、番号、
  明示選択の必要性を変更しません。
- non-TTYと`--noconfirm`ではprovider inputをstdinから読まず、candidateを自動選択しません。
  未選択のambiguous providerはfail-closedで停止します。
- `moguet -S --select [--needed] <query>`はofficial repositoryとAURからsource-awareなroot
  package candidateを検索します。interactive TTYではpackage番号、複数番号、inclusive range、
  表示済みofficial groupの`@group` selectorを受理します。candidateが1件でもdefaultは
  ありません。empty input、`q`、`quit`、`cancel`、EOFは取消とし、invalid inputは同じ
  candidate一覧に対して再入力します。
- root package discoveryはnon-TTY stdinまたは`--noconfirm`ではcandidate queryもpromptも
  開始しません。selectionとstatic preflightの完了後、selected repository rootをexactな
  `repository/package`の1 transactionとして先に実行し、成功した場合だけselected AUR
  rootを既存source-build lifecycleへ渡します。後続AUR failureは完了済みrepository
  transactionをrollbackしません。
- provider choiceは現在のinvocation内だけで所有します。`deps` / `plan`はselectedと
  ambiguous providerを区別し、selected AUR providerのPackageBaseはfetch / build planへ
  渡し、selected repository providerはofficial `repository/package` dependencyとして
  導入します。`deps --recursive`ではprovided dependencyのうちuser-selected AUR
  providerだけをさらに辿り、unique providerとselected repository providerは終端の
  まま表示します。
- constraint resultはprovider candidateのfilter、sort、番号変更、recommend、default、
  auto-selectを行わず、source fallbackの根拠にもなりません。selected AUR provider metadataを
  refreshした場合はcurrent matching capabilityを再評価し、古いresultを再利用しません。
- source-build routeはroute固有のlifecycleを維持します。standalone official
  repository buildは`PackageBaseSet`、registered official sourceは
  `OnlyIfUpdated` preparation後の`PackageBaseSet`を使います。sync repository
  routeは既存`--needed`付きの`SingularCompatibility`、registered AUR sourceは
  既存provider / split-package guard付きの`SingularCompatibility`のままです。
- official repository source identityはconfigured libalpmのexact snapshotを
  authorityとします。confirmed `NotFound`だけがAUR fallbackを許し、query、config、
  metadata failureでは停止します。standalone / registered buildはauthoritativeな
  PackageBaseを1回buildし、requested `Explicit` childだけをinstallします。sibling / debug
  outputはunselectedかつnot installedとして保持します。
- registered AUR routeは、candidateがすべてofficial repository由来の場合だけprovider
  selectionを行います。AUR providerを含むcandidate setは、このsingular compatibility
  routeでそのproviderのPackageBaseをscheduleできないためambiguousのままsystem / source
  execution前に停止します。
- 未解決dependency、未選択のambiguous provider、cycle、blockingなconflict / replacement
  assessment、証明できないartifact identityは、対応するmutation前に拒否します。
- `--noconfirm`は対話停止を避ける指定であり、「すべてyes」ではありません。source
  selection、plan、identity、conflict、ownershipのguardを突破せず、自動削除や自動置換を
  許可しません。
- Moguet-owned boolean confirmationは、`[Y/n]`をYes default、`[y/N]`をNo default、
  `[y/n]`をdefaultなしとして扱います。fixedかつcase-insensitiveなASCII tokenとして
  `y` / `yes`、`n` / `no`、`q` / `quit` / `cancel`を受理します。`[y/n]`でのEnterは
  approvalではなくwarning後の再promptです。`--noconfirm`は宣言済みdefaultだけを使い、
  non-TTY inputはsafe Noだけを利用できます。どちらもapproval-requiredなdefaultなし
  promptをYesへ変えません。question固有のDeclined、current operationのCancelled、actual
  input / command / internal failureは互いに区別します。cancellationは後続処理を停止しますが、
  完了済みphaseをrollbackしません。詳細は[interactive confirmation contract](https://github.com/seekerkrt/moguet/blob/develop/docs/contracts/interactive-confirmation.md)を
  参照してください。
- 複数phaseのupgradeは単一atomic transactionではありません。failure時は後続処理を
  止めますが、完了済みpackage transactionをrollbackしません。install成功後にcleanup
  だけ失敗した場合、packageはinstall済みの可能性があるため、結果を確認せず再試行
  しないでください。`upgrade-all`のprovider selectionはfiltered AUR phaseのclone、build、
  pacman、sudoより前に行いますが、それ以前のphaseは完了済みの場合があります。
- exact target-less `moguet -Syu`もsequentialです。official repository system updateを
  完了してから、freshなinstalled foreign / AUR inventoryを取得し、normal AUR updateを
  実行します。repository failure時はAUR phaseを開始しません。repository完了後のblocker、
  execution failure、cleanup failureはnon-zeroのpartial outcomeとして報告し、完了済み
  repository transactionをrollbackしません。

詳細なcompatibility / routing契約は
[docs/COMPATIBILITY.md](https://github.com/seekerkrt/moguet/blob/develop/docs/COMPATIBILITY.md)、
採用済み設計判断は
[docs/DECISIONS.md](https://github.com/seekerkrt/moguet/blob/develop/docs/DECISIONS.md)を
参照してください。

<!-- parity:installation -->
## インストール

### Build requirements

- `base-devel`を事前導入したArch build環境。現在の構成packageがC++ toolchain、
  `pkgconf`、GNU gettext development toolを提供します
- `cmake` 3.18以降。optionalなtracked developer presetには3.19以降が必要です
- `pacman`、`pacman-conf`、libalpm development metadata
- `git`
- `curl`
- `nlohmann-json`
- `tomlplusplus`

development treeは次のようにbuildして確認できます。

```bash
git clone https://github.com/seekerkrt/moguet.git
cd moguet
make
./moguet --help
```

C++ build / install targetのauthorityはCMake、C++ test registration / executionの
authorityはCTestです。root `Makefile`は従来のdeveloper shortcutとrepository固有validationを
維持します。通常の`make`は`BUILD_TESTING=OFF`の`build/cmake-production`、`make test`は
`BUILD_TESTING=ON`の`build/cmake-testing`を使い、CTestの後にrepository validation layerを
実行します。既存のfocused入口も`make test-<area>`として利用できます。

再現可能なdebug / editor設定にはtracked developer presetのpost-success frontendを使います。

```bash
make cmake-dev-configure
cmake --build build/cmake-testing
ctest --test-dir build/cmake-testing --output-on-failure
```

このpresetは`BUILD_TESTING=ON`、Debug build、`CMAKE_EXPORT_COMPILE_COMMANDS=ON`を有効にし、
生成したdatabaseをrepository rootの`compile_commands.json` symlinkからclangd等のeditor / analysis
toolへ公開します。`make cmake-dev-configure`は`cmake --preset dev-debug`を実行し、configureと
generateを含むprocess全体が成功した後だけlinkをpublishします。configure / generate failureでは
以前のvalidなpublicationを維持し、first failureでは何もpublishしません。rawな
`cmake --preset dev-debug`はconfigure-onlyのCMake入口として残り、root linkをpublishしません。
`make clean`はwrapper所有build treeとroot linkを削除します。CMake Presetsの利用にはCMake
3.19以降が必要です。optionalなpreset CLIを使わないdirect configureでは、projectが宣言する
CMake 3.18 minimumを維持します。

live filesystemへ書き込まないpackaging用dry runは、payloadを一時directoryへstageします。

```bash
stage_dir=$(mktemp -d)
make PREFIX=/usr DESTDIR="$stage_dir" install
find "$stage_dir" -type f -print
```

`make install` / `make uninstall`はcanonical CMake install graphとexactな
`install_manifest.txt`へのfrontendです。上記destination overrideは別のMake install recipeではなく、
同じCMake graphへmappingされます。

current development packageはprivate implementation helper
`/usr/libexec/moguet/moguet-alpm-receipt-helper`と
`/usr/libexec/moguet/moguet-source-artifact-install-helper`もinstallします。両者はowner / protocol /
stateを分離したpackage-owned root transaction authorityで、public commandではありません。`PATH`外で
man pageを持たず、executable、state root、destination pathを引数に取らず、source / build treeのhelperで
置き換えてはなりません。source-artifact helperはwrite-sealedなvalidated artifact bytesだけをprivateな
root-owned transaction stateへstageしてからfixed `pacman -U`へ渡します。current public source-buildの
`--rmdeps`は引き続きunsupported / fail-closedであり、どちらのhelperのinstallもdependency cleanupを
有効化しません。

current development treeは、これらowner-specific helperを単一のclosed remote AUR lifecycle内部で使い、
internal cleanup-candidate assessmentを構築します。full invocationとcurrent metadata / policy observationが
成功し、exactにcorrelateされたactual dependency `Install`だけがinternal `Eligible`へ到達できます。
このassessmentはpublic preview、prompt、removalへ接続せず、makepkg sync dependencyのownershipも
独立した未解決authorityのままです。

v2.0.0のpackage名と唯一のexecutableは`moguet`で、`/usr/bin/jpacker`をinstall
しません。payloadはjpacker v1.16.0 packageと重複しないため、metadataには
`jpacker`への`provides`、`conflicts`、`replaces`を意図的に設定しません。manual
migrationとrollbackを検証する間、両packageはcoexistできます。source-preference
storeは共有せず、Moguetはuser所有のXDG config authorityだけを使い、legacy
`/etc/jpacker/package.build/` storeを参照・変更しません。Moguet packageはuser XDG dataも
legacy directoryも作成・所有しません。

package runtime dependencyは`curl`、`git`、`libalpm.so`、`libarchive`、`nano`、
`pacman`、`sudo`です。package metadataへ記録するexactな`makedepends` setは
`cmake`、`nlohmann-json`、`tomlplusplus`です。Arch package buildは`base-devel`の事前導入を
前提とし、現在の構成packageがGNU gettextと`pkgconf`を提供するため、`base-devel`、
`gettext`、`pkgconf`は`makedepends`へ含めません。`git`はruntime dependencyのまま
とし、重複して列挙しません。gettextはcatalog build toolを提供し、runtime binaryは
独立したlibintl dependencyを持ちません。

Moguet v2.0.0にはAUR publicationを含めません。AUR URLを作り上げたり、development
payloadをlive systemへinstallしたりしないでください。installed systemを変更する前に
[v1からv2へのMigration Guide](docs/migration/v1-to-v2.ja.md)を確認してください。

### `PKGBUILD`によるpackage installation

repository rootの`PKGBUILD`は、checkout済みのworking treeではなく公開済みreleaseを
package化します。`pkgver()`はrepository自身の`VERSION` fileからversionを読み取り、
対応する公開済みGit tag（`v<version>`）をbuild sourceとして取得するため、未release
のworking tree commitをpackage化することはありません。

```bash
git clone https://github.com/seekerkrt/moguet.git
cd moguet
makepkg -si
```

`makepkg -si`は、そのtag付きreleaseをbuildし、同じ操作で`pacman -U`によってlive
systemへinstallします。これは、development treeをその場でbuild・確認するだけで
何もinstallしない、上記の`make`や`./moguet --help`とは異なります。`PKGBUILD`はcanonicalな
production CMake build / install consumerとして`BUILD_TESTING=OFF`を指定し、115個のdeveloper
C++ test-ledger executable、1個の`EXCLUDE_FROM_ALL` installed transport fixture harness、146件のCTest
registrationはhost / CI / release validation側で扱います。
この`PKGBUILD`は
repository同梱のpackaging経路であり、AUR submissionではありません。Moguetはまだ
AUR pageを公開していません。

<!-- parity:usage -->
## 基本的な使い方

現在のcommand / option surfaceは`moguet --help`をruntime authorityとします。command
tokenとoption tokenはlocaleによって変わりません。

Moguet-owned / interceptedのclosed grammarは次のとおりです。

<!-- CLI CANONICAL GRAMMAR BEGIN -->
```text
build <pkg> [V=K...]
build --local <directory> [V=K...]
upgrade
upgrade-aur
upgrade-all
clean
deps [--recursive] <pkg>...
plan <pkg>...
fetch <pkg>...
add-src <item>...
edit-src <pkg>...
list-src
del-src <pkg>...
revert <pkg>...
-G <pkg> [--output-dir=DIR]
-Gp <pkg>
-S --select [--needed] <query>
-Syu [--needed]
-Syu --repo [--needed]
```
<!-- CLI CANONICAL GRAMMAR END -->

2つのexact target-less `-Syu` formはMoguetがinterceptするsemantic routeです。
repository-only formはcompatibleなdelegated pacman tailを引き続き受理します。
その他のpacman operation formは、Moguetのallowlistではなくdelegated open grammarのまま
です。closed grammarはremote / local `build`の2つ目のbare operandを拒否し、`upgrade`、
`upgrade-aur`、`upgrade-all`、`clean`、`list-src`のtarget operandを拒否します。`...`を
示したinspection / source-maintenance formはmulti-target behaviorを維持します。

```bash
# packageのinstall、search、info表示
moguet -S <pkg>
moguet -S --select [--needed] <query>
moguet -Ss <query>
moguet -Si <pkg>

# 通常のAUR helper update: official repositoryの後にnormal installed AUR
moguet -Syu

# repository-only system upgrade
moguet -Syu --repo

# configured source package、installed AUR package、または両方を更新
moguet upgrade
moguet upgrade-aur
moguet upgrade-all

# remote package 1件、またはlocal PKGBUILD root 1件をbuild・install
moguet build <pkg> [V=K...]
moguet build --local <directory> [V=K...]

# buildせずAUR dependencyとbuild orderを調査
moguet deps --recursive <pkg>...
moguet plan <pkg>...

# build / installせずbuild repositoryを取得
moguet fetch <pkg>...

# source-build preferenceを1件以上maintenance
moguet add-src <item>...
moguet edit-src <pkg>...
moguet list-src
moguet del-src <pkg>...
moguet revert <pkg>...

# PackageBase checkoutをexport、またはPKGBUILDだけを表示
moguet -G wezterm-git
moguet -G wezterm-git --output-dir=./exports
moguet -G wezterm-git --output-dir="$HOME/src/aur"
moguet -G wezterm-git --output-dir=/home/user/src/aur
moguet -Gp <pkg>

# persistent stateを変更せず、対応する全mutating routeを観測
moguet --dry-run -S <pkg>
moguet --dry-run -Syu
moguet --dry-run -Syu --repo
moguet --dry-run fetch <pkg>...
moguet --dry-run build <pkg>
moguet --dry-run build --local <directory>
moguet --dry-run upgrade
moguet --dry-run upgrade-aur
moguet --dry-run upgrade-all
```

`-G`は`--output-dir`未指定時、command開始時current directoryへexportします。
`--output-dir=DIR`はそのinvocationだけの既存parent directoryを指定し、relative valueは
command開始時current directoryを基準に解決します。validated PackageBaseが常にdirect-childの
destination nameです。Moguetはparentを作成せず、指定pathのsymlink componentをfollowせず、
existing destinationを上書きせず、独自のtilde expansionも行いません。
`--output-dir=~/src/aur`ではなく、`--output-dir="$HOME/src/aur"`またはabsolute pathを
使用してください。このoptionは`-Gp`や他operationではunsupportedで、attached formだけを
受理します。

`--dry-run`は、Moguet-owned `-S` install / system-update route、`fetch`、remote / local
`build`、`upgrade`、`upgrade-aur`、`upgrade-all`のglobal observation modifierです。
`deps`、`plan`、`-Ss`、`-Si`、`clean`、generic pacman pass-through routeではpacmanへ
転送せず明示的に拒否します。rendererは`Ready` / `NoOp`を終了code 0、`Blocked`を
non-zeroで報告します。

dry-runはproduction preflightと同じread-only filesystem / network discoveryと、exact
allowlist済みのpacman discovery queryを実行し得ますが、state logやpersistent stateを書かず、
cache、workspace、worktreeを作成せず、Git clone / fetch / checkout mutation、
`makepkg --printsrcinfo`その他のlocal metadata評価、build output、sudo、pacman transactionの
開始・mutation、pacman transaction lock、package install、cleanup mutationへ進みません。そのためlocal
buildでmetadata評価が必要な場合はreadyを推測せず`Blocked`になります。観測結果をapproval
token、execution capability、cached provider choiceとして再利用せず、後のactual invocationは
current stateを再validationします。v2.2.0のsurfaceはhuman-readableだけで、JSONその他の
machine-readable plan schemaは追加しません。

exact target-less `moguet --dry-run -Syu`では、repository system-update intentと後続の
normal-AUR transaction intentを分けて表示します。AUR assessmentはrepository transactionを
仮に実行した後のstateではなく、現在インストールされている状態に基づきます。actual
`moguet -Syu`はこのobservationやprepared capabilityを再利用せず、repository upgrade成功後に
freshなinstalled-foreign inventory、AUR metadata、plan、provider decision、preflightを
取り直します。`moguet --dry-run -Syu --repo`はrepository intentだけを表示し、AURや
source-build preferenceへqueryしません。

human-readable diagnosticはtyped stateのprojectionであり、classificationを決める
authorityではありません。英日ともnormal summary、attention-required detail、route-owned
necessary detailの順を保ちます。operation outcomeとpackage state observationを分け、plan
construction、completeness、execution readinessを独立して表示します。successfulだが
unverifiedな観測はrequired check付きのsuccessとして維持し、`Unknown`を`NoOp`へ書き換えず、
severity、blocking、exit-status effectも別dimensionとして扱います。

**upgrade commandの選択:** 通常のAUR helper updateにはexact target-less
`moguet -Syu`を使います。official repository system upgradeを完了した後、installed
foreign / AUR stateを再評価し、normal AUR updateを実行します。このrouteはsaved
source-build preferenceを列挙・readせず、PackageBase fallback preferenceも参照せず、
preference dataをAUR work environmentへ適用しません。valid、invalid、unreadable、その他
failureとなるsaved preferenceのいずれも、このrouteへ影響しません。

明示的な`upgrade*` commandはMoguetのsource-aware workflowであり、saved preferenceを
Strictに扱います。`upgrade`はrepository system updateとconfigured source-build
preference、`upgrade-aur`はinstalled AUR packageだけを更新しつつ、対応するsaved
source-build preferenceを確認・適用します。`upgrade-aur`はrepository system updateを
実行しません。`upgrade-all`はrepository update、configured-source lifecycle、remaining
AUR updateを実行します。これらは通常の`-Syu`の別名ではありません。

combined routeへ入るのはexact target-less canonical `-Syu` tokenだけです。`-Sy`、`-Su`、
modifierのalternate / separated spelling、target-bearing `-Syu <pkg>`、unknown modifier
formは既存routingを維持し、installed-AUR sweepを開始しません。Auto combined `-Syu`で
initially対応するpacman semantic optionは`--needed`だけで、repository transactionだけへ
適用します。他のpacman semantic optionやunsupported argument formはrepository mutation前に
失敗し、`moguet -Syu --repo`を案内します。repository-only formはsemantic selectorを
pacmanへ渡さず、compatibleなpacman pass-through surfaceを維持し、AUR inventory、AUR RPC、
preference、cache、Git、makepkgへ到達しません。`moguet -Syu --aur`はunsupportedです。
AUR-only source-aware updateには`moguet upgrade-aur`を使います。`--noconfirm`はprovider、
conflict / replacement、required `RequiresCheck`等のsafety guardを突破せず、未検証の
devel updateを承認しません。

Moguet v2.5.0では、exact AUR packageとして解決できるinstalled packageについて、
`-git`、`-svn`、`-hg`、`-bzr`、`-cvs`、`-darcs`というconventionalなsuffix
根拠をdevel candidateとして扱います。normal AUR versionが新しくなく、suffix根拠しか
ない場合、`moguet -Qua`はsilentに最新扱いせず、package、PackageBase / child根拠、
reason付きの`RequiresCheck`を表示します。normal AUR versionが新しい場合は既存の
update candidate authorityを優先します。

`RequiresCheck`は`UpToDate`でもautomatic rebuild decisionでもありません。通常のexact
target-less `-Syu`では、independentなtargetだけをnon-blocking warning付きでskipし、
unrelatedなnormal AUR updateを継続してaggregate successを許します。update statusは
未検証のままです。別のupdate candidateがdependency、provider、required-child relationとして
そのtargetを必要とする場合は、affected rootをblockしてoperationをnon-zeroにします。
`upgrade-aur`、そのdry-run、`upgrade-all`のfresh AUR phaseは、strictなwhole-operation
blocker semanticsを維持します。non-TTYや`--noconfirm`でもpromptを追加せずrebuildを承認しません。

v2.5.0ではupstream VCS revisionのquery / 比較やdevel build provenanceのpublicationを
行いません。current development treeには
[Issue #475](https://github.com/seekerkrt/moguet/issues/475)のtrusted HTTPS Git remote revision
observer foundationが入り、default HEAD / exact branchとcompleteなSHA-1 / SHA-256 resultだけへ
限定されています。[Issue #476](https://github.com/seekerkrt/moguet/issues/476) Slice 7-Bのinternal coordinatorは、
provenance tipとfresh installed / reviewed stateを照合した後だけremoteを観測し、local stateを再確認してから
`UpdateAvailable` / `UpToDate`を返します。Slice 7-Dではnormal AUR update、registered AUR update、
`-Qua`、対応するdry-runをこのproducerへ接続します。normal AUR versionが新しい場合はGit queryなしで
既存のversion authorityを優先し、same/olderの場合だけvalidated Git assessmentで補完します。
Git revision差分はpackage version変更と区別して表示します。

initial authoritative executionは実際のreviewed pinと既存のsingle-child HTTPS Git subsetを要求し、
S4/S5/S6を一度だけconsumeします。開始後にlegacyへfallbackしません。install成功とprovenance publication失敗は
別のpartial outcomeとして保持し、non-zeroにします。registered RequiresCheckはdefault-Noの明示rebuild確認へ進みますが、
それがsource reviewを代替することはありません。`--noconfirm`からreview authorityを作りません。
詳細は[normal devel route contract](https://github.com/seekerkrt/moguet/blob/develop/docs/contracts/devel-normal-routes.md)を参照してください。
provenanceは別XDG state namespaceのschema v1を維持します。unknown/future schema、corrupt/unsafe historyは
fail closedとし、updaterはrecord修復、external history adoption、missing baselineの自動生成を行いません。
baselineには明示的にreviewした対応build、実install、publication成功が必要です。同version reinstallでも
installed artifact bindingが変わればhistorical provenanceは無効です。詳細は
[devel tracking / migration contract](https://github.com/seekerkrt/moguet/blob/develop/docs/contracts/devel-tracking.md)を参照してください。
これはv2.7.0向けdevelopment candidateの説明であり、v2.7.0のrelease済み機能とは扱いません。

`--aur`は対応する`-S`、`-Ss`、`-Si`をAURへ限定します。`--repo`はこれらのformを
official binary repositoryへ限定し、exact target-less `-Syu`ではrepository-only selectorに
なります。`--aur`は`-Syu`で受理しません。両selectorの併用はexternal commandやAUR query
より前に失敗します。pacman-only routeではcompatibleなpacman optionを保持し、source-build
routeで意味を維持できないoptionは黙って無視せず拒否します。

`-S --select [--needed] <query>`はinteractiveなsource-aware discovery形式です。source selectorを
指定しなければofficial repositoryとAURの両方を検索し、`--aur`または`--repo`でcandidate
sourceを限定します。両selected routeで同じ意味を持つoptionは`--needed`だけです。
non-TTYと`--noconfirm`ではqueryやpackage選択を行わず失敗します。

source-build preferenceはmulti-targetの`add-src`、`edit-src`、`del-src`、`revert`と、
target-lessの`list-src`で管理します。一時的な`build <pkg> [V=K...]`はremote packageを
解決し、preferenceを保存しません。

### Reviewed AUR source workflow

AUR Git source buildでは、最後に明示acceptしたexact upstream commitをPackageBaseごとの
persistent XDG stateとして保持します。fetch / clone後は1つのexact target commitをpinします。
このworkflowより前から存在するcacheを含め、reviewed stateがなければ、最初に対象となる
PackageBaseのtracked file全体をfull reviewします。後続targetはprevious reviewed revisionから
reviewし、同じtargetなら新しいpromptもstate writeも不要です。old commit objectが利用できない
場合、cache checkoutへfallbackせずfull rebaseline reviewを提示します。invalid、corrupted、
source-mismatched stateにはexplicitなfull rebind reviewが必要で、future / unsafe stateは
fail-closedで停止します。

review inventoryはextension allowlistではなく、全tracked fileのadd、modify、delete、rename、
type changeを扱います。root `PKGBUILD`とtop-level `*.install`にはreview-sensitive guidanceを
付けますが、patch、service unit、helper script、local source / config、binary change、その他の
tracked contentも表示します。`.SRCINFO`はlower-priorityなgenerated metadataとして残し、
source reviewの代用にはしません。

`--diff`はreviewed-source prompt policyを選びますが、completeなreview後にinteractive
`y` / `yes`を明示入力した場合だけ保存revisionを進めます。`--nodiff`、review decline、
`--noconfirm`、non-TTY inputでは既存のcompatibility build behaviorを維持し得ますが、reviewed
stateは進めません。cancellation、EOF、input failure、unsupported review、unsafe / future stateは、
stateを進めず停止します。

accept済みbuildはexact target commitへcheckoutし、mutableなcheckout HEAD、branch、remote refを
build authorityにしません。publicationはcompare-and-swap guardを使い、並行processのreviewを
上書きしません。後続build / install failureでも、正常にacceptされたrevisionをrollbackしません。
`--edit` / `--noedit`はinvocation-localなPKGBUILD / `.install`編集を制御するもので、upstream
acceptanceではありません。editor changeはreviewed commit上の別overlayです。official repository
と`build --local` routeはこのstateを作りません。詳細は
[reviewed AUR source state contract](https://github.com/seekerkrt/moguet/blob/develop/docs/contracts/reviewed-source-state.md)を
参照してください。

### Package単位のbuild customization

既存v2.xの`[V=K...]`とsource-build preferenceを使うと、system-wideな
`/etc/makepkg.conf`を編集せず、1つのpackageだけbuild environmentを調整できます。
complete replacementと、system-wide valueを引き継いでから局所変更する方法は別の
patternです。どちらかを常にdefaultとせず、目的に応じて選びます。

one-offでcomplete overrideする例:

```bash
moguet build example-package \
  CFLAGS="-O3 -pipe" \
  CXXFLAGS="-O3 -pipe"
```

指定した各variableは、このpackage build向けの明示値へ置き換わり、対応する
system-wide valueとはmergeされません。問題の切り分けのためにflagsを意図的に
単純化する場合や、package固有の設定を試す場合に使えます。ここでの`-O3`は説明用で、
performance recommendationではありません。

現在のsystem settingを引き継ぎ、一部分だけ変更する例:

```bash
source /etc/makepkg.conf

moguet build obs-studio \
  CFLAGS="${CFLAGS/-O2/-O3}" \
  CXXFLAGS="${CXXFLAGS/-O2/-O3}"
```

ここで`source`は`/etc/makepkg.conf`をbaselineとしてcurrent shellへ読み込むだけで、
file自体を変更しません。各substitutionはその他の既存flagsを維持し、現在の値に
`-O2`が含まれる場合だけその部分を変更します。含まれなければ値は変わりません。
これは万能なoptimization recipeではなく、`-O3`も説明用の値です。

C / C++ mixed packageでは`CFLAGS`と`CXXFLAGS`の両方が必要になり得ますが、
C-only / C++-only packageでは必要なvariableが異なり得ます。実際にどのenvironment flagsを
利用するかはpackageの`PKGBUILD`とupstream build systemが決めるため、Moguetはこれらの
variableがcompiler invocationへ作用することを保証しません。

`build`はone-off operationのままで、これらのassignmentを保存しません。設定を確認後、
`add-src`を使うと、そのpackageのsource-build preferenceとして保存できます。
complete overrideを保存する例:

```bash
moguet add-src example-package \
  CFLAGS="-O3 -pipe" \
  CXXFLAGS="-O3 -pipe"
```

system-wide valueを引き継いで局所変更した結果を保存する例:

```bash
source /etc/makepkg.conf

moguet add-src obs-studio \
  CFLAGS="${CFLAGS/-O2/-O3}" \
  CXXFLAGS="${CXXFLAGS/-O2/-O3}"
```

`build --local <directory> [V=K...]`は、代わりにuser所有directoryを
exactly oneのlocal PackageBase sourceとして扱います。pathらしいpackage operandからlocal
rootを推測せず、そのrootをAURへqueryしません。

local routeは安全な`.SRCINFO`を変更せずに読みます。metadataがmissing、invalid、または
known-staleの場合、PKGBUILD reviewとdefaultなしの明示同意を終えてから
`makepkg --printsrcinfo`を実行します。non-TTY inputや`--noconfirm`は評価を許可せず停止
します。Moguetはinvocation-owned source snapshotからbuildし、user-owned treeを変更せず、
採用metadataが宣言するvalidかつuniqueな全`pkgname` childをexplicit rootとしてinstall
します。dependency artifactはdependency install reasonを保持し、既にexplicitなinstalled
packageをdependencyへ降格しません。runtime stateを使うpackage-name completion等の高度な
補完はfuture workであり、同梱completionはpublic CLI schemaに限定します。

<!-- parity:configuration -->
## 設定

Moguetはuser所有のoptionalなTOML fileを1つ読みます。

```text
$XDG_CONFIG_HOME/moguet/config.toml
fallback: ~/.config/moguet/config.toml
```

repository checkoutではcanonical copyを`sample/config.toml`として参照できます。
standard Arch packageのinstall後は、同じfileを
`/usr/share/doc/moguet/examples/config.toml`で参照できます。編集前に
`$XDG_CONFIG_HOME/moguet/config.toml`へcopyし、`XDG_CONFIG_HOME`が未設定の場合だけ
上記fallbackを使用してください。enum valueの`prompt`、`skip`、`normal`、`rebuild`、
`clean`はTOML stringなのでquoteが必要で、canonical sampleではdouble quoteを使用します。

v2.0.0の最小schemaとbuilt-in defaultは次のとおりです。

```toml
schema_version = 1

[review]
pkgbuild = "prompt"
diff = "prompt"

[build]
mode = "normal"
```

値は次の順で合成します。

```text
built-in default -> user config -> CLI override
```

canonical CLI overrideは`--edit` / `--noedit`、`--diff` / `--nodiff`、
`--build-mode=normal|rebuild|clean`です。`--rebuild`と`--cleanbuild`は対応する
build modeのcompatibility aliasです。conflicting overrideはlast-one-winsにせず、
external mutation前に失敗します。

`review.diff`と`--diff` / `--nodiff`はAUR reviewed-source prompt policyを選びます。
promptをskipしてもreviewed stateは進みません。`review.pkgbuild`と`--edit` / `--noedit`は
invocation-localなPKGBUILD / `.install` editor behaviorを選び、upstream review acceptanceには
なりません。

config fileがない状態は正常です。fileが存在する場合は`schema_version = 1`が必須で、
invalid TOML、unknown key、type error、invalid enum、未対応future schema versionは
invocationを停止します。Moguetはfileを自動作成・rewrite・migrationせず、
`/etc/moguet`をsystem-wide config layerとして使用しません。

source-build preferenceは、次のpackage別fileです。

```text
${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/<package-name>
```

TOML configではありません。`add-src`、`edit-src`、`list-src`、`del-src`、`revert`と、
source-aware build / 明示的な`upgrade*`側の全readerがこの1つのauthorityを使います。
exact target-less `-Syu`とそのdry-runは、このdirectoryのsnapshot、列挙、readを意図的に
行わず、child名 / PackageBase名のfallback preferenceも適用しません。Strict readerでは
storeまたはentryがない場合だけを保存済みpreferenceなしとして扱い、invalid name、unsafe
entry、permission error、I/O failureはhard errorです。read / list operationはdirectoryを
作成しません。storageを最初に必要とする`add-src`または`edit-src`だけがmanaged directoryを
mode `0700`、entryをmode `0600`で作成します。package install / reinstall / uninstallは
XDG preferenceもlegacy dataもcreate、migrate、removeしません。

<!-- parity:xdg -->
## XDG config・source preference・state・cache

| 責務 | XDG path | fallback |
| --- | --- | --- |
| user config | `$XDG_CONFIG_HOME/moguet/` | `~/.config/moguet/` |
| source-build preference | `$XDG_CONFIG_HOME/moguet/source-build.d/` | `~/.config/moguet/source-build.d/` |
| 永続runtime state、reviewed AUR revision、log | `$XDG_STATE_HOME/moguet/` | `~/.local/state/moguet/` |
| 再生成可能cache | `$XDG_CACHE_HOME/moguet/` | `~/.cache/moguet/` |

default logはstate directory内の`moguet.log`です。cacheはauthorityではなく再生成可能で、
cache削除によってconfigやpersistent stateを失ってはいけません。directoryはcommandが
必要としたときだけ作成し、help / version表示では作成しません。

accept済みAUR revisionは`$XDG_STATE_HOME/moguet/reviewed-sources/aur/`以下へ保存し、
HOME fallbackでは`~/.local/state/moguet/`以下を使います。これはcache metadataではなく
PackageBase stateであり、cache削除やreclone後も保持します。read-only lookupはstoreを
作成しません。

source-preference accessでは、`XDG_CONFIG_HOME`がunsetまたはemptyならHOME fallbackを
使います。明示的な`XDG_CONFIG_HOME`はabsoluteで、既存かつownership、type、permissionの
検証に成功する必要があり、Moguetはwrite commandが最初に必要としたときだけ配下のmanaged
`moguet/source-build.d`を作成します。HOME fallbackでは同じsource-preference safety
boundaryが必要なconfig階層を作成できます。relative path等の安全でないXDG pathはfail
closedとします。rootでMoguetを実行した場合はroot自身のXDG contextを使い、`SUDO_USER`等
から別userを推測してそのhomeへ書き込みません。このpath規則はAUR source buildのroot実行を
許可するものではありません。

<!-- parity:localization -->
## Localization

英語textをsource authorityかつbuilt-in fallbackとし、日本語を正式対応します。
Moguetはstandard GNU gettext behaviorによりprocess locale（`LC_ALL`、
`LC_MESSAGES`、`LANG`、`LANGUAGE`）へ従い、`--lang`やTOMLのlanguage settingを
独自実装しません。

英語で再現したい場合は`LC_ALL=C`を使えます。catalogやentryが欠けても、意味が
完結した英語messageへfallbackします。command / option token、package / repository
identity、path、TOML key / enum、machine-readable field、external programの出力は
翻訳しません。

英語manと日本語manはstandard locale-specific layoutへ配置され、適切なlocaleでは
`man moguet`が日本語pageを選び、存在しないlocaleでは英語へfallbackします。

<!-- parity:compatibility -->
## Compatibilityとmigration

Moguetはpacman-firstですが、すべてのsource-build routeで完全なpacman互換を宣言しません。
pacmanだけで完結するoperationはMoguetが消費しないoptionをpass-throughします。AUR /
source-build routeをMoguetが所有する場合は、対応関係を明示したoptionだけを保持し、
意味を維持できないものはmutation前に拒否します。

Moguet v2.6.0では、exact target-less `moguet -Syu`をrepository-onlyから通常のAUR helper
behaviorへ変更します。repository system updateの後にnormal installed-AUR updateを行い、
saved source-build preferenceはread・適用しません。以前のrepository-only behaviorが必要な
利用者は`moguet -Syu --repo`へmigrationしてください。saved source-build preferenceを確認・
適用する場合は`moguet upgrade`、`moguet upgrade-aur`、`moguet upgrade-all`を使います。
new combined routeはsequentialでatomicではありません。後続AUR failure時も完了済み
repository transactionをrollbackせず、non-zeroのpartial completionとして報告します。

reviewed-source stateが存在する前からのAUR cacheに手動migrationは不要です。recordがない状態は
正常で、最初に対象となるPackageBaseを一度initial full reviewします。legacy checkout HEAD、
branch、remote ref、build artifactからreviewed revisionを捏造しません。invalid、corrupted、
source-mismatched、future、unsafe stateをmissingとして扱わず、reviewed-source contractに従って
fail-closedを維持します。

Moguetは`/etc/jpacker/jpacker.conf`を通常config layerとして読まず、
`/etc/jpacker/package.build/`をsource-preference fallbackとして使用しません。
`/etc/jpacker`を自動copy・merge・rewrite・deleteせず、root-owned legacy dataの移行先userを
推測しません。[English Migration Guide](docs/migration/v1-to-v2.md)または
[日本語Migration Guide](docs/migration/v1-to-v2.ja.md)に従い、v1 stateをbackupし、対象user
ごとに理解したentryだけを手動で再作成してrollback dataを保持してください。

正式かつpackageが提供する唯一のv2 commandは`moguet`で、`jpacker` binary aliasは
ありません。Moguetとjpacker v1.16.0のpackage fileは重複せず、transition / rollback用に
同時installできます。preference storeは共有しませんが、両helperのpackage-mutating
operationを同時実行しないでください。Moguetはlegacy storeを無視して保持し、明示的な
per-user migrationだけがXDG authorityを変更します。

<!-- parity:development -->
## 開発

canonical development repositoryは
[GitHub](https://github.com/seekerkrt/moguet)、backup mirrorは
[GitLab](https://gitlab.com/seekerkrt/moguet)です。質問、不具合かもしれない相談、機能要望は
[GitHub Discussions](https://github.com/seekerkrt/moguet/discussions)を最初の入口にしてください。
[GitHub Issues](https://github.com/seekerkrt/moguet/issues)はmaintainerが管理する具体的な作業を
追跡します。十分な再現・観測情報があるbugは、
[Bug Issue Form](https://github.com/seekerkrt/moguet/issues/new?template=bug-report.yml)から
直接報告できます。pull requestはGitHubで管理します。

変更を提案する前に
[CONTRIBUTING.md](https://github.com/seekerkrt/moguet/blob/develop/CONTRIBUTING.md)を
確認してください。Security-sensitiveな内容をpublicなDiscussionやIssueへ投稿せず、
[SECURITY.md](https://github.com/seekerkrt/moguet/blob/develop/SECURITY.md)に従って
[非公開で報告してください](https://github.com/seekerkrt/moguet/security/advisories/new)。

active integration branchは`develop`、stable releaseは`main`です。
[docs/DEVELOPMENT.md](https://github.com/seekerkrt/moguet/blob/develop/docs/DEVELOPMENT.md)、
[docs/VERSIONING.md](https://github.com/seekerkrt/moguet/blob/develop/docs/VERSIONING.md)を
参照してください。Moguet v2.xではAUR helper
機能を段階的に追加し、高度なruntime-aware completionと将来のbuild profile systemは
別作業として扱います。

<!-- parity:license -->
## License

Moguet releaseとjpacker v1.15.0からv1.16.0は、`GPL-3.0-or-later`で提供します。
jpacker v1.14.0以前のreleaseはMIT Licenseで提供されました。これらhistorical releaseは
元のlicenseのまま利用でき、Moguet renameによってtag、release、付与済みpermissionは
変わりません。

- GNU GPL version 3全文: [LICENSE](https://github.com/seekerkrt/moguet/blob/develop/LICENSE)
- version境界と配布方針: [docs/LICENSING.md](docs/LICENSING.md)
- link / compile対象とexternal program: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- v1.14.0以前のhistorical MIT本文: [LICENSES/jpacker-MIT-legacy.txt](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/jpacker-MIT-legacy.txt)

Moguetはlibalpmとlibcurlへ直接dynamic linkし、systemのnlohmann-jsonとtoml++ headerを
binaryへcompileします。pacman、pacman-conf、makepkg、git、vercmp、およびnoticeに記載した
他のprogramはprocess boundary越しに実行する別programです。
