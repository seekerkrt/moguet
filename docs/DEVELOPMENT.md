# Development Workflow

Moguetは、`main` / `develop` / `feature/*` / `fix/*` / `docs/*` / `release/*`を使った軽量Git Flow型の運用を行う。canonical repositoryは[GitHub](https://github.com/seekerkrt/moguet)、backup mirrorは[GitLab](https://gitlab.com/seekerkrt/moguet)である。

branch / tag同期のownerはGitHub Actionsのmirror workflowとする。同じrefをGitHubとGitLabへ二重に手動pushせず、GitHubをauthority、GitLabをmirror destinationとして扱う。

バージョン番号の付け方は [VERSIONING.md](VERSIONING.md) を参照する。developmentからrelease
candidateまでのvalidation selection、approval evidence、evidence reuse / invalidation、review closureは
[VALIDATION.md](VALIDATION.md)をpolicy authorityとする。

## Branches

### main

`main` は最新安定版を表す。

- GitHub の default branch とする
- `VERSION` / `PKGBUILD` / man page / Git tag / GitHub Release と整合している状態を保つ
- 通常の機能追加や修正作業は直接取り込まない
- release branch からの PR のみを取り込む

### develop

`develop` は次リリース候補を集約する integration branch とする。

- 普段の Issue 対応や小修正は `develop` から分岐する
- feature / fix / docs branch は原則 `develop` へ PR する
- 未リリース変更を含んでよい
- 次の release branch は `develop` から分岐する

### feature / fix / docs branches

Issue ごとの作業ブランチ。

例:

- `feature/issue-XX-aur-deps`
- `fix/issue-XX-search-exit-code`
- `docs/issue-XX-branch-workflow`

原則として `develop` から分岐し、`develop` へ PR する。

### release branches

リリース準備用ブランチ。

例:

- `release/v2.1.0`
- `release/v2.0.1`

`develop` から分岐し、以下を整える。

- `VERSION`
- generated man page
- `PKGBUILD` / package metadata
- English / Japanese sectionを持つtracked `RELEASE_NOTES.md`
- 必要な README / docs 更新

準備が完了したら`main`へPRする。merge後にrelease merge commitへannotated Git tagを作成し、tagをGitHubへだけpushする。GitHub Release本文はtracked `RELEASE_NOTES.md`をauthorityとし、GitLabのbranch / tag / Releaseは各mirror workflowで同期する。

## Typical flow

### Normal development

    git switch develop
    git pull --ff-only origin develop
    git switch -c feature/issue-XX-topic

実装中は`VALIDATION.md`のrisk classificationに従い、incremental buildとaffected / focused
targetを使う。例:

    env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target test-<affected-area>

Slice completionでは変更contractのfocused supersetと必要なhost / deterministic regressionを確認する。
PR / merge approvalのcanonical host gateは次の1回である。同じcandidateの有効なevidenceがある場合は、
`VALIDATION.md`のinvalidation ruleに従って不要な再実行を避ける。

    env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target test-host-release
    git diff --check
    git status --short

    git add .
    git commit -m "..."
    git push -u origin feature/issue-XX-topic

    gh pr create --base develop --head feature/issue-XX-topic

PR merge 後:

    git switch develop
    git pull --ff-only origin develop

GitLab側の同一refはmirror workflowの完了後に確認する。通常flowで手動pushしない。

### CMake / CTest build authority

Issue #463の最終状態では、CMakeがproject-ownedなC++ compile / link / build / install graph、
CTestがC++ test registration / executionを所有する。Makefileはdeveloper shortcutとrepository
validation frontendであり、production / test source list、C++ standard、warning、compile definition、
include / link graph、negative compile recipeを所有しない。

用途別build treeは次の2つを基本とする。

| Tree | Policy | Consumer |
| --- | --- | --- |
| `build/cmake-production` | `BUILD_TESTING=OFF` | 通常の`make`、install / uninstall、production smoke |
| `build/cmake-testing` | `BUILD_TESTING=ON` | developer、CTest、host / release validation、focused test |

通常の`make`はproduction treeだけから`moguet`をbuildし、107個のC++ test-ledger executable、
1個の`EXCLUDE_FROM_ALL` installed transport fixture harness、132件のCTest registrationを不用意にbuildしない。
`make test`はtesting treeをbuildし、CTestを実行してから
gettext、shell、docs、packaging等のrepository-specific validationを実行する。`make test-<area>`は
互換entrypointとして残るが、exact target / CTest selectionは
`cmake/MoguetFocusedTests.cmake`が所有する。

#### Developer debug preset / compile database

tracked `CMakePresets.json`の`dev-debug` presetは、既存testing treeをDebug、
`BUILD_TESTING=ON`、`CMAKE_EXPORT_COMPILE_COMMANDS=ON`でconfigureし、fresh treeのcompilerを
Make frontendと同じ`g++`で初期化する。

    make cmake-dev-configure
    cmake --build build/cmake-testing
    ctest --test-dir build/cmake-testing --output-on-failure

`make cmake-dev-configure`はtracked presetでconfigureし、そのprocessがconfigure / generateを
含めてexit 0となった後だけpost-success publisherを実行する。repository rootの
`compile_commands.json`は
`build/cmake-testing/compile_commands.json`を指すgenerated symlinkとなる。presetの再configureはexactな
root artifactをcurrent treeへ更新する。configure-phaseまたはgenerate-phaseで失敗した場合は
以前のvalidなlinkを維持し、first failureではroot artifactを公開しない。rawな
`cmake --preset dev-debug`はCMakeのconfigure-only入口であり、process終了後のpublicationを所有しないため
root linkを更新しない。`make clean`はbuild treeとroot linkを削除する。package / release buildはroot
compile databaseを必要とせず、`PKGBUILD`もこのdeveloper optionを有効にしない。

CMake PresetsはCMake 3.19で導入されたため、`dev-debug`には3.19以降が必要である。project本体の
`cmake_minimum_required(VERSION 3.18)`は維持し、3.18ではpresetを使わずdirect configureする。
Ninjaはoptional generatorであり、必要な環境では別treeで次のように確認できる。

    cmake -S . -B build/cmake-ninja -G Ninja -DBUILD_TESTING=ON
    cmake --build build/cmake-ninja
    ctest --test-dir build/cmake-ninja --output-on-failure

`build/cmake-ninja`はvalidation時の一時的な別backendであり、恒常的な第三のauthorityではない。
Ninjaを`PKGBUILD`のmandatory dependencyへ追加しない。

#### External toolchain inputs

project-ownedなstrict C++20、`-Wall -Wextra`、default `-O2 -pipe`、version macro、include、libraryは
CMake target propertyが所有する。`CPPFLAGS`、`CXXFLAGS`、`LDFLAGS`、`CCACHE`、compiler selectionは
外部toolchain inputであり、Make / PKGBUILD frontendはconfigureごとに明示的に同期する。direct
CMakeはfrontend syncを既定で無効とし、explicit `-D` cache authorityと通常のCMake初期化規則を
尊重する。

Make frontendでは、未定義の`CXXFLAGS`だけがCMake-owned defaultを要求する。command lineまたは
environmentで明示した空値はdefaultを抑止する。persistent treeで値をA、B、explicit emptyへ変更しても、
configureごとにcurrent valueへ同期し、stale flagやlauncherを残さない。

ccacheを導入済みの環境ではcompile launcherを明示できる。

    make CCACHE=ccache -j8 --output-sync=target test
    ccache --show-stats
    make CCACHE= -j8 --output-sync=target test

launcherはcompile commandだけへ入り、link commandへは入らない。ccacheはruntime / package dependencyでも
defaultでもない。optional linkerも外部`LDFLAGS`から指定できるが、mold等をproject defaultやmandatory
dependencyにしない。

Make / PKGBUILDはexisting treeのcached compilerとrequested compilerを、CMake-compatibleな
executableとimmutable required argumentsへ分けてconfigure前に比較する。`g++`、`/usr/bin/g++`、
同一実体へのsymlinkは許可し、`CXX='g++ -m64'`のようなrequired argumentもfresh treeとsame-argument
reuseで維持する。異なるcompiler実体、argumentの変更、argumentの削除はcacheやtreeを変更する前に
停止する。frontendからraw spellingを毎回`-DCMAKE_CXX_COMPILER`へ再注入しない。

#### CMake policy contract

`cmake/MoguetPolicies.cmake`はminimumを3.18に保ち、target作成前に次のbehaviorを明示する。
policy version rangeの一括引き上げは、未検証のpolicyまでNEWにするため行わない。

| Policy | Behavior | Moguetで維持する契約 |
| --- | --- | --- |
| [CMP0200](https://cmake.org/cmake/help/latest/policy/CMP0200.html) | NEW（利用可能なCMake） | `IMPORTED_CONFIGURATIONS`をavailable configurationsのauthorityとする。current ArchのCURLはgeneric / RELEASEとも同じshared library、ALPM / JSONはconfiguration-independentなINTERFACE targetであり、選択名が変わってもcompile / link inputsは変わらない。 |
| [CMP0156](https://cmake.org/cmake/help/latest/policy/CMP0156.html) | OLD（利用可能なCMake） | static libraryの必要な反復と、shared libraryの最後の出現を残す既存orderingを維持する。current GNU ldではNEWでもlink commandは同じだが、外部linker指定時のde-duplication strategyやCMP0179の選択まで暗黙に変更しない。 |
| [CMP0181](https://cmake.org/cmake/help/latest/policy/CMP0181.html) | OLD（利用可能なCMake） | `CMAKE_*_LINKER_FLAGS`を既存command fragmentとして消費する。`LDFLAGS`には`-Wl,...`等のcompiler-driver形式を使用し、`LINKER:` prefixへの移行や再引用をこの整理へ含めない。 |

JSONのexportは内部の`cmake_policy(VERSION ...)`でCMP0200を未設定へ戻すため、その
`find_package`呼び出し中だけ`CMAKE_POLICY_DEFAULT_CMP0200=NEW`を与え、終了後に元の値／未定義へ
戻す。header-onlyなJSON usage requirementsにはconfiguration選択に依存するlocationやdefinitionが
なく、このdefaultで意味は変わらない。packageが明示的に選んだpolicyは上書きしない。

Issue #530のCMake 4.4.3 / GCC 16.2.1 / GNU ld 2.47でのfresh reproductionでは、通常configureの
対象warningは0件だった。`--trace-expand`を付けるとCMP0200はimported target selection、
CMP0156はMoguetのlibrary de-duplication、CMP0181は`CMAKE_CXX_CREATE_CONSOLE_EXE`の
command fragmentを起点にwarningが出た。CMP0156 / CMP0181は単なる未設定では通常warnせず、
[trace / debugによるpolicy診断の有効化](https://cmake.org/cmake/help/latest/variable/CMAKE_POLICY_WARNING_CMPNNNN.html)
が発生条件だった。completion frontend fixtureも同じpolicy moduleを読み、standalone projectの
policy未設定を持ち込まない。warning suppression optionは使用しない。

外部flagのauthorityは上記External toolchain inputsのままとする。Moguet自身が同期するのは
`CMAKE_EXE_LINKER_FLAGS`だけで、shared / module flagsとconfiguration別flagsはCMakeの
environment初期化、cache、toolchainに委ねる。CMP0156 / CMP0181のNEWへの移行は、必要になった
時点でexternal flagsとlinker別のgenerated command / symbol closureを検証して決める。

#### Test composition / link firewall

`cmake/MoguetTests.cmake`、`MoguetTestTargets.cmake`、`MoguetTestRegistrations.cmake`が次のfail-closed
inventoryを所有する。

| Inventory | Expected |
| --- | ---: |
| C++ test executables | 115 |
| installed transport fixture harnesses (`EXCLUDE_FROM_ALL`) | 1 |
| support / stub translation units | 32 |
| link firewalls | 50 |
| firewall descriptors | 50 |
| CTest registrations | 146 |

stub / real implementation exclusion、replacement ABI、ALPM stub、exact source closureをtarget-localに
維持する。単一production libraryを全testへ無条件linkしない。negative compileはCTest registrationから
effective CMake compiler / launcher / compile optionを取得し、GNU Make recursive compileへ戻さない。
Make focused aliasとCMake focused targetは各126件で一致し、missing / unexpectedを0に保つ。

`make test-installed-fixture-compile`は既存のinstalled transport fixture全体をcompile/linkする
host gateであり、fixtureを実行しない。`make test`のrepository validationにも含め、production headerと
fixture内のreplacement definitionとのsignature driftをcontainer acceptanceより前に検出する。
対象source、test-only macro、include/link profileは`MoguetTestTargets.cmake`の既存targetをそのまま使う。
`EXCLUDE_FROM_ALL`を維持し、通常のproduction/testing `all`やinstall payloadには追加しない。
fixture runtimeのownerは引き続きinstalled container laneであり、host compile PASSはactual S5/S6の
transaction/publication acceptanceを代替しない。

completion生成が使う`moguet-cli-authority-exporter`もCMake targetであり、Python generatorはcompilerを
直接起動しない。このtargetは`EXCLUDE_FROM_ALL`なので通常のproduction/package buildへ混ざらず、
repository validationが必要時だけ明示buildする。tracked completionのcanonical write入口は
`make generate-completions`であり、exporterへ依存するCMake targetがcurrent sourceに対してbuildした後、
Pythonのstdout rendererを実行して3つのtracked fileをpublishする。
`scripts/generate_completions.py`はcheck-only / stdout-onlyでtracked write modeを持たず、callerが
environment markerを自称してもこのfreshness boundaryを代替できない。

#### Install / package consumer

CMake install graphと`install_manifest.txt`がinstall / uninstall payloadのcanonical authorityである。
Makeの`PREFIX`、`BINDIR`、internal `LIBEXECDIR`、completion、man、license、doc、locale destination overrideはCMake cacheへ
mappingし、別のMake install graphを持たない。`PKGBUILD`はgenerator-neutralなCMake configure / build /
install consumerで、`BUILD_TESTING=OFF`を指定する。package payload / permission / layout validationは
repository validation側で維持し、通常のpackage buildへfull CTestを追加しない。current internal payloadには
`/usr/libexec/moguet/moguet-alpm-receipt-helper`と
`/usr/libexec/moguet/moguet-source-artifact-install-helper` mode `0755`を含む。両者をpublic commandとして
扱わず、owner-specific production root hookはconfigure / install graphが確定した対応するabsolute
helper pathだけを使用する。

### Host validation execution graph

`test`はfull host A–Dを所有し、`release-check-exclusive`はversion、license、packaging、tracked
Markdownのrelease固有4 checkerだけを所有する。`test-host-release`は同じtop-level runで`test`を
完了してから`release-check-exclusive`を1回実行するため、A–DとGを重複なく構成できる。
実行段階とevidenceの扱いは[VALIDATION.md](VALIDATION.md)を正とする。

既存`release-check`のstandalone互換性は維持し、従来のA–D subset prerequisiteを完了してから同じ
`release-check-exclusive`へ委譲する。`release-check`単独をfull A–Dへ拡張したものではない。

### Arch Linux container validation

実機Arch Linuxでの最終smoke testより前に、official `archlinux:latest`を使う隔離laneを
明示的に実行できる。Docker CLI、起動中のDocker daemon、およびcommitとして解決できる
local `v1.16.0` tagを用意し、repository rootで次を実行する。

    make test-container

image buildではnetwork利用を許可し、`--pull`でbase imageを確認し、cache miss時はArch
repositoryからdependency packageを取得する。Dockerのlayer cacheは再利用し得るため、このlaneは
base imageとpackage repositoryの現在状態に対するvalidationであり、長期固定された再現imageでは
ない。test containerの実行時は`--network=none`とし、public AUR、real Git clone、actual package
transactionを行わない。

default build contextからsource snapshotだけをcontainer内へcopyし、host worktreeをbind mount
しない。`.git`、host build artifact、XDG data、credential、Docker socket、host pacman database /
config / cacheは共有せず、`--privileged`も使用しない。package transition testのlegacy sourceは
local `v1.16.0` tagからtemporary archiveとして生成し、Git metadataとは分離したnamed build
contextで渡す。build、test、release validationはcontainer固有のHOME / XDG directoryを使う
一般userとして実行する。image buildがclean production buildを1回所有し、runtimeはそのbinaryが
存在することを確認してfull host A–Dとrelease固有Gを1回ずつ実行する。

image build:

    env -u MAKEFLAGS -u MFLAGS make clean
    env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target

runtime:

    env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target test-host-release

Docker command、daemon、image build、またはvalidation stepが失敗した場合、diagnosticとnon-zero
statusをhostへ返す。実行containerは成功時・失敗時とも`--rm`で破棄し、temporary legacy archiveも
削除する。build cacheとlocal image `moguet-arch-validation:local`は後続実行で再利用できるよう残す。

このlaneはhostの通常build、`make test`、`make release-check`を置き換えず、それらから再帰的に
呼び出さない。release前にはhost validationとcontainer validationを別々に確認する。

Issue #404 Slice 3.6のroot trust / ALPM receipt boundaryは、追加のsecurity-specific installed fixtureで
確認する。

    make test-container-receipt

このtargetは既存のlocal `moguet-arch-validation:local` imageをdependency/toolchain baseとしてreuseし、
current sourceをhost bindではなくstandalone Docker contextからcopyする。image buildとruntimeはいずれも
`--network=none`で、CMake install graphが配置したroot-owned
`/usr/libexec/moguet/moguet-alpm-receipt-helper`だけをhookから実行する。actual Install、Upgrade non-match、
solver-introduced dependency、transaction failure / ABORTをephemeral container package databaseで確認し、
host package database、host `/run`、development-tree helperを共有しない。このtargetはsecurity Slice evidenceであり、
host A–D、offline E、actual provider / AUR / local Fを相互に代替しない。

Issue #485 Slice 2のSourceArtifactInstall boundaryは、selected-provider laneを上書きせず、別targetで確認する。

    make test-container-source-artifact-receipt

同じstandalone image infrastructureをreuseするが、実行owner、helper、protocol、`/run` state、assertionは
`SourceArtifactInstall`専用である。write-sealed artifact bytesからroot-owned stagingを作り、actual
`pacman -U`のInstall、Upgrade、same-version reinstall、downgrade、`--needed` skip、failure、
multi-artifact、replay / cross-owner isolationをephemeral container package databaseで確認する。単一artifact
matrixはproduction C++ transportから`SourceArtifactInstallReceiptObservation`とcausal evidenceまで通す。
`test-container-receipt`のselected-provider evidenceとは相互に代替しない。

Issue #476 Slice 1のinstalled binding feasibilityは、同じnetworkless imageを使うがproduction helperへ
接続しない専用characterizationで確認する。

    make test-container-installed-binding-characterization

runtimeはhost `/var/lib/pacman`を共有せず、自動破棄されるanonymous volumeだけをpacman root / local DB
authorityにする。Install、Upgrade、`--needed` skip、same-version different artifact reinstall、identical artifact
reinstall、identical same-second reinstallをactual `pacman -U`で実行し、fresh `desc` / `files` / `mtree`と
通常userから再取得したfilesystem-scoped opaque file handleを比較する。`name_to_handle_at(2)`をsupportしない
filesystem/runtime、またはidentical same-second replacementを区別できない環境はPASSへ丸めずfail closedする。
このtargetはfeasibility evidenceであり、provenance publication、host A–D、offline E、actual Fを代替しない。

Issue #476 Slice 2のstorage foundationは、host focused target
`test-xdg-generation-store`と`test-devel-build-provenance-store`で確認する。前者は#411から抽出した
raw immutable-generation/CAS機械層、後者は別namespace、strict codec、exact #411 binding、future-schema
refusal、typed lookup/publicationを所有する。normal queryは7-Bからreadし、7-C→S6がpublicationする。
このfocused evidenceはinstalled characterizationやcontainer transaction evidenceを代替しない。

Issue #476 Slice 3のbuild-context foundationは、host focused target
`test-invocation-owned-source-build-context`で確認する。`PinnedReviewedSourceBuild`だけをproduction mint入口とし、
exact reviewed Git treeから`.git`を含まないprivate recipe snapshotを作り、同じ一意なownerへprivate
`PKGDEST` / `BUILDDIR` / `SRCDEST`と固定`/usr/bin/makepkg` identityを束縛する。tracked symlink / gitlink、
editor overlay、dirty / untracked drift、unsafe path / root、shared fallbackはfail closedとする。fixed `/tmp`
parentはrootまたはeffective user所有だけを許し、group / other writableならsticky bitを必須とし、retained
descriptorとnamed identityをcontext lifetime中も再検証する。partial construction failureはretain済みのroot / child
だけをdescriptor-relativeにcleanupし、abort cleanup failureをprimary creation failureと別のtyped consequenceへ
保持する。current normal source-buildは7-Cからこのcontextを作り、S4のmakepkg phaseへ渡す。
context producer自体はinstall/publicationやdevel comparisonを呼ばない。

Issue #476 Slice 4のactual-build proofは、host focused target
`test-evaluated-devel-source-build`で確認する。exact reviewed snapshotと同じcontext内のprivate working recipeを
分離し、retained `/usr/bin/makepkg` FD、raw/evaluated source一致、`--nobuild`後のprivate mirror/worktree、
pre/post-build complete Git OID、dynamic version、fresh one-artifact inventory、retained-FD libalpm metadata、
archive / ALPM-MTREE SHA-256を一つのmove-only capabilityへ束縛する。pre-build stale packagelist、source shape drift、
ambiguous workspace、PKGDEST contamination、artifact replacement/mismatch、cross-context compositionはfail closedする。
このproofはcontextとartifactをSlice 5向けに保持するが、current source-build / install / CLI、provenance
publication、#475 observationを直接呼ばない。S5-A/B producerがretained artifactをtrusted transactionへ渡し、
S6-Bは完成したS5 result内のsemantic valuesだけを読む。詳細は
[`evaluated-devel-source-build-proof.md`](contracts/evaluated-devel-source-build-proof.md)を正とする。

Issue #476 S5-Bのpurpose/operation protocol、root helper publication、fresh local DB/generation、live mintの結合は
`test-exact-artifact-transaction-protocol`、`test-exact-artifact-transaction-receipt`、
`test-installed-package-record-observation`、`test-exact-installed-binding`で確認する。
最後のtargetは既存Slice 4/transport fixture executableの専用modeを使用し、通常host DBのtransactionは行わない。
canonical negative compileにはsame-name observer spoof、raw generation/binding/path/fd/tuple、historical decodeからの
fresh mint、private entry/receipt constructorの拒否を登録する。

    make test-container-exact-installed-binding

このinstalled acceptanceはnetworkless Dockerとanonymous volumeのDBを使用し、actual Slice 4 proofから
first Install、Upgrade、same-version reinstall、downgradeを通す。fixed pacman-confで解決した同じDB world、
PostTransaction anchor、通常userのnew ALPM handle、raw MTREE、opaque record generation、live bindingを確認する。
S5-Cの`InstalledDevelSourceBuildProof`完成までを確認し、XDG provenanceが作られないことを要求する。
host package DBはmount/変更しない。S5 laneはpublicationなしを維持する。
Slice 6-Bの内部publisherは`test-devel-build-provenance-publication`と`test-devel-build-provenance-publication-result`で
deterministic S4/S5 fixtureから検証する。6-Cのactual publicationは同じinstalled laneの別modeで確認する。

    make test-container-devel-publication

1つのfresh anonymous DB volumeで4 transactionsを順次実行し、各S5 receipt/fresh binding/final proofを
先に確認してからproduction publisherを呼ぶ。store世代1→2→3→4とopaque installed generationを
別々に記録する。same-version reinstallでも新世代へ進み、downgradeでもpublication順は逆行しない。
raw persistent bytesのSHA-256、schema v1/27 keys、final proofとの全field一致、旧世代の保存と
contiguous historyを確認する。runnerはcopied source hashesとraw documentsをstdoutへ出し、
検証者はcurrent candidateとの照合とrepository外へのevidence保存を行う。
S5-only targetはpublication-noneを引き続き要求する。#475 comparisonは7-B coordinator内だけに接続し、
7-Dがnormal routeから7-B/7-Cへ接続する。Slice 8の最終契約とmigration判断は
[devel tracking contract](contracts/devel-tracking.md)、final acceptanceの選択とevidenceは
[VALIDATION](VALIDATION.md)を参照する。S4/S5/S6のowner contractは変更しない。
詳細は[`exact-installed-artifact-binding.md`](contracts/exact-installed-artifact-binding.md)を正とする。

S5-Cのfinal construction/lineage/N=1は`test-installed-devel-source-build-proof`、lossless aggregateとcleanup consequenceは
`test-devel-source-artifact-install-result`で確認する。同じS5-B fixtureの出力を消費し、41-case matrixは複製しない。
同一fixture executableをbuildするfocused targetは別invocationで実行し、同じbuild outputへの重複buildを避ける。
finalizerのcomplete private authority、raw tuple/decoded binding/contradictory resultのconstruction firewallも
canonical negative compileへ含める。contractは[`installed-devel-source-build-proof.md`](contracts/installed-devel-source-build-proof.md)を正とする。

Issue #476 Slice 7-Aは`test-current-installed-artifact-binding`と`test-devel-git-revision-comparison`で確認する。
current observerは各callのfresh DB observationであり、S5 transaction proofではない。
Git comparatorはsource/algorithmを先に照合するpure value comparisonで、networkやassessmentを実行しない。
7-Bだけがこのcomparison foundationを消費し、normal routeは7-Dを参照する。詳細は
[`current installed observation contract`](contracts/current-installed-artifact-observation.md)を参照する。

Issue #476 Slice 7-Bは`test-devel-package-assessment`でtip-only P/I/R gates、#475 remote mapping、
remote成功後のone-time local recheckとcall countを確認する。normal routeは7-Dから接続し、assessmentからS6 publisherを呼ばない。
approved-source mintはown-I/O coordinatorだけで、canonical negative compileにS7-B firewallを追加する。
詳細は[`read-only assessment contract`](contracts/devel-package-assessment.md)を正とする。

Issue #476 Slice 7-Cは`test-reviewed-devel-source-build-execution`でtyped pinからS4/S5/S6を結合する。
normal ownerがtype erasure前に選べる専用variantを提供するが、7-Dがnormal finalizerからLegacy/AuthoritativeDevelを選択する。
S5/S6 contractを変更せず、partial install/publication/cleanup outcomesとlive context lifetimeを保持する。
詳細は[`reviewed devel execution bridge`](contracts/reviewed-devel-source-build-execution.md)を正とする。

Issue #485 Slice 5のclosed lifecycle / authoritative candidate gateは、同じnetworkless installed imageを
使う専用targetで確認する。

    make test-container-cleanup-authority

このtargetはsynthetic remote AUR rootとproduction collectorを使い、session mint、mutation前baseline、
actual `SourceArtifactInstall` dependency transaction、Install-only receipt、full successful invocation、
post-success current / policy query、correlation、aggregate、classifierまでを通してexact candidateが
`Eligible`になることを確認する。同じrunner内のUpgrade / reinstall / downgrade / `--needed` / failure / multi / owner isolation
matrixはnegative authorityを維持する。public `--rmdeps`、preview、prompt、removalを実行せず、host package DB / `/run`を共有しない。

Issue #372のlive aggregate gateは、provider selection、real AUR install、real local
PKGBUILD build / installを単一のfail-fast recipeから別containerで順に実行する。parallel makeの
contextでも後続laneを並行開始せず、providerまたはAUR failure後は残りのlaneを開始しない。
current Arch repository、public AUR、
container内のactual package transactionを使うため、`make test` / `make release-check`へ
actual executionを混ぜない。release candidateでは通常のhost / offline validation後に、
明示的に次を実行し、three live laneの結果を個別に確認する。

    make test-container-live

`release-check`はlive targetのisolation、standalone Dockerfile、aggregate compositionを
static `test-live-contract`として確認するが、networkやcontainer runtimeが必要なこのgateの
成功を代替しない。

### Release flow

    git switch develop
    git pull --ff-only origin develop
    git switch -c release/vX.Y.Z

リリース準備後:

    env -u MAKEFLAGS -u MFLAGS make clean
    env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target
    env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target test-host-release
    env -u MAKEFLAGS -u MFLAGS make test-container
    env -u MAKEFLAGS -u MFLAGS make test-container-live
    git diff --check

ccache / mold parityは必要なreleaseでの追加validationであり、上記default gateの代替にしない。
それぞれのexact compile / link scopeとclean / incremental条件を`VALIDATION.md`に従って記録する。

    git status --short

    git add -- \
        VERSION \
        README.md \
        README.ja.md \
        RELEASE_NOTES.md \
        docs/DEVELOPMENT.md \
        man/moguet.1 \
        man/ja/moguet.1 \
        po/moguet.pot \
        po/ja.po

    git diff --cached --name-only | LC_ALL=C sort
    git status --short
    git commit -m "vX.Y.Z release準備"
    git push -u origin release/vX.Y.Z

    gh pr create --base main --head release/vX.Y.Z

上記の`git add`は、v2.7.1 release preparationでstage対象とする9 pathsを1件ずつ明示した
current release用のexact path setです。`git add .`や代表pathだけのpartial listへ置き換えません。
commit前にcached path一覧をactual diffと再照合し、release scopeのunstaged / untracked pathや
unrelatedなstaged pathがないことを確認します。

root `VERSION`、README EN/JA、`RELEASE_NOTES.md`、generated man EN/JA、gettext metadataを同期します。
`docs/DEVELOPMENT.md`自身は今回のexact path setとその理由を保持します。v2.7.1はv2.7.0の
maintenance PATCHであり、`docs/COMPATIBILITY.md`や`docs/contracts/devel-tracking.md`に
development-candidateからreleased contractへの新しい状態遷移はありません。

`scripts/check_public_documentation.py`はroot `VERSION`からcurrent release sectionを動的に求め、
`tests/test-public-documentation-checker.py`のversion文字列はその動作を検証する独立fixtureです。
そのため今回のrelease versionを複製する変更は行いません。過去releaseの導入versionも書き換えません。

`PKGBUILD`はroot `VERSION`を動的に読み、published tagへprojectするためcontent changeはありません。
man templateは`@VERSION@`と既存の`September 2026`を維持するため変更しません。
`po/POTFILES.in`はsource extraction inventory変更なし、completionはversion independentです。
Make / CMake、production source、container Dockerfile / runner、fixture package metadata、その他testsには
release metadata preparationによる変更contractがないため、current listへ含めません。
v2.1.0固有の履歴は下記の`v2.1.0 post-release closure`として別に扱います。将来のreleaseでは、このlistを
流用せず、そのreleaseで監査済みのexact path setへ置き換えます。

merge 後:

    git switch main
    git pull --ff-only origin main

    git tag -a vX.Y.Z -m "vX.Y.Z"
    git push origin vX.Y.Z

    if ! release_notes_payload=$(mktemp); then
        printf '%s\n' 'release notes payload: unable to create temporary file; release was not created' >&2
        exit 1
    fi
    trap 'rm -f "$release_notes_payload"' EXIT HUP INT TERM

    if ! sh scripts/extract-release-notes.sh >"$release_notes_payload"; then
        printf '%s\n' 'release notes extraction failed; release was not created' >&2
        exit 1
    fi

    printf '%s\n' 'Inspect the release notes payload before creating the release:'
    if ! cat "$release_notes_payload"; then
        printf '%s\n' 'release notes payload inspection failed; release was not created' >&2
        exit 1
    fi

    if [ ! -s "$release_notes_payload" ]; then
        printf '%s\n' 'release notes payload is empty; release was not created' >&2
        exit 1
    fi

    if ! version=$(tr -d '[:space:]' < VERSION); then
        printf '%s\n' 'VERSION could not be read; release was not created' >&2
        exit 1
    fi
    if ! grep -Fqx "# Moguet v$version" "$release_notes_payload"; then
        printf '%s\n' 'release notes payload verification failed; release was not created' >&2
        exit 1
    fi

    gh release create vX.Y.Z --title "vX.Y.Z" --notes-file "$release_notes_payload"

tag mirrorとGitLab Release mirrorの完了を確認する。uploaded assetがないreleaseではGitLab asset linkが0件でも正常とする。

`RELEASE_NOTES.md`は過去releaseの履歴も保持するため、そのまま`--notes-file`へ渡さない。
`sh scripts/extract-release-notes.sh`は`VERSION`と一致するcurrent top-level sectionだけを出力し、
current headingが0件または複数なら失敗する。GitHub Release作成前にこの出力を確認し、実際に
渡すpayloadがcurrent release sectionだけであることを確認する。

その後、`main`から`develop`への回収PRを作成する。protected branchをlocal mergeやdirect pushで更新しない。

    gh pr create --base develop --head main

回収PRのmergeとmirror完了後、local branchを更新する。

    git switch develop
    git pull --ff-only origin develop

### v2.1.0 post-release closure

mainからdevelopへの回収PRとGitLab mirror完了後、次の順にreleaseを閉じる。GitHubを
authorityとして扱い、GitLabへ同じrefを手動pushしない。

1. GitHub authority上の`release/v2.1.0`を削除する。
2. GitLab mirror側で同branchの削除/pruneが完了したことを確認する。
3. local、GitHub、GitLabの`main` SHAが一致することを確認する。
4. local、GitHub、GitLabの`develop` SHAが一致することを確認する。
5. local、GitHub、GitLabのannotated `v2.1.0` tag objectが一致することを確認する。
6. annotated `v2.1.0` tagのpeeled commitが一致することを確認する。
7. local、GitHub、GitLabに`release/v2.1.0`が存在しないことを確認する。
8. local worktreeがcleanであることを確認する。

削除操作そのものはGitHub authorityで完了させる。以下は削除後のread-only確認例であり、
placeholderの`<tag-object-sha>`には手順5で確認したGitHub tag object SHAを使う。

    git rev-parse main develop v2.1.0 v2.1.0^{}
    git ls-remote origin refs/heads/main refs/heads/develop refs/tags/v2.1.0 'refs/tags/v2.1.0^{}'
    git ls-remote gitlab refs/heads/main refs/heads/develop refs/tags/v2.1.0 'refs/tags/v2.1.0^{}'
    gh api --method GET repos/seekerkrt/moguet/git/ref/heads/main
    gh api --method GET repos/seekerkrt/moguet/git/ref/heads/develop
    gh api --method GET repos/seekerkrt/moguet/git/ref/tags/v2.1.0
    gh api --method GET repos/seekerkrt/moguet/git/tags/<tag-object-sha>
    git branch --list release/v2.1.0
    git ls-remote --heads origin release/v2.1.0
    git ls-remote --heads gitlab release/v2.1.0
    git status --short --branch

## Notes

- `main` は完成品を置く棚として扱う
- `develop` は普段の作業机として扱う
- `main` / `develop`はprotected branchとして扱い、更新はPR経由とする
- 実装後はPR作成・merge・branch削除・mirror結果・clean確認まで締める
- published tagは移動・再作成せず、通常recoveryでReleaseを削除・unpublishしない
- 大きなリネームや破壊的変更は major release で扱う

Issue #476 Slice 7-Dのnormal routingは`test-aur-devel-route`、normal finalizerのS4/S5/S6結合は
`test-normal-reviewed-devel-execution`で確認する。normal routeの既存query/preflight/runner/reducer/CLI/dry-run tests、
construction firewalls、frontendを併用する。contractは[devel normal routes](contracts/devel-normal-routes.md)。
