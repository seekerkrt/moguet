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

#### Test composition / link firewall

`cmake/MoguetTests.cmake`、`MoguetTestTargets.cmake`、`MoguetTestRegistrations.cmake`が次のfail-closed
inventoryを所有する。

| Inventory | Expected |
| --- | ---: |
| C++ test executables | 107 |
| installed transport fixture harnesses (`EXCLUDE_FROM_ALL`) | 1 |
| support / stub translation units | 30 |
| link firewalls | 50 |
| firewall descriptors | 50 |
| CTest registrations | 132 |

stub / real implementation exclusion、replacement ABI、ALPM stub、exact source closureをtarget-localに
維持する。単一production libraryを全testへ無条件linkしない。negative compileはCTest registrationから
effective CMake compiler / launcher / compile optionを取得し、GNU Make recursive compileへ戻さない。
Make focused aliasとCMake focused targetは各111件で一致し、missing / unexpectedを0に保つ。

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

Issue #476 Slice 2のproduction-disconnected storage foundationは、host focused target
`test-xdg-generation-store`と`test-devel-build-provenance-store`で確認する。前者は#411から抽出した
raw immutable-generation/CAS機械層、後者は別namespace、strict codec、exact #411 binding、future-schema
refusal、typed lookup/publicationを所有する。production sourceへcompileされてもnormal CLIからのstore
lookup/publication callerは持たず、installed characterizationやcontainer transaction evidenceを代替しない。

Issue #476 Slice 3のproduction-disconnected build-context foundationは、host focused target
`test-invocation-owned-source-build-context`で確認する。`PinnedReviewedSourceBuild`だけをproduction mint入口とし、
exact reviewed Git treeから`.git`を含まないprivate recipe snapshotを作り、同じ一意なownerへprivate
`PKGDEST` / `BUILDDIR` / `SRCDEST`と固定`/usr/bin/makepkg` identityを束縛する。tracked symlink / gitlink、
editor overlay、dirty / untracked drift、unsafe path / root、shared fallbackはfail closedとする。fixed `/tmp`
parentはrootまたはeffective user所有だけを許し、group / other writableならsticky bitを必須とし、retained
descriptorとnamed identityをcontext lifetime中も再検証する。partial construction failureはretain済みのroot / child
だけをdescriptor-relativeにcleanupし、abort cleanup failureをprimary creation failureと別のtyped consequenceへ
保持する。このfoundationはproduction sourceへcompileされるが、current source-build caller、makepkg phase、
provenance publication、devel comparisonへは接続しない。

Issue #476 Slice 4のproduction-disconnected actual-build proofは、host focused target
`test-evaluated-devel-source-build`で確認する。exact reviewed snapshotと同じcontext内のprivate working recipeを
分離し、retained `/usr/bin/makepkg` FD、raw/evaluated source一致、`--nobuild`後のprivate mirror/worktree、
pre/post-build complete Git OID、dynamic version、fresh one-artifact inventory、retained-FD libalpm metadata、
archive / ALPM-MTREE SHA-256を一つのmove-only capabilityへ束縛する。pre-build stale packagelist、source shape drift、
ambiguous workspace、PKGDEST contamination、artifact replacement/mismatch、cross-context compositionはfail closedする。
このproofはcontextとartifactをSlice 5向けに保持するが、current source-build / install / CLI、pacman、provenance
publication、#475 observationへは接続しない。詳細は
[`evaluated-devel-source-build-proof.md`](contracts/evaluated-devel-source-build-proof.md)を正とする。

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
        containers/arch-live-validation/run-aur-install.sh \
        docs/DEVELOPMENT.md \
        man/moguet.1.in \
        man/ja/moguet.1.in \
        man/moguet.1 \
        man/ja/moguet.1 \
        po/moguet.pot \
        po/ja.po \
        scripts/check_public_documentation.py \
        tests/test-live-contract.sh

    git diff --cached --name-only | LC_ALL=C sort
    git status --short
    git commit -m "vX.Y.Z release準備"
    git push -u origin release/vX.Y.Z

    gh pr create --base main --head release/vX.Y.Z

上記の`git add`は、現在のv2.6.0 release preparationでstage対象とするpathを1件ずつ明示した
current release用のexact path setです。`git add .`や代表pathだけのpartial listへ置き換えません。commit前に
cached path一覧をこのreleaseのdiffと再照合し、release scopeのunstaged / untracked pathや
unrelatedなstaged pathがないことを確認します。現在のv2.6.0 release preparationでは、上記の
14-path listがstage対象のcurrent release scopeのauthorityです。`PKGBUILD`はroot `VERSION`を動的に
読み、published tagへprojectするためcontent changeはありません。
`containers/arch-validation/Dockerfile`にはv2.6.0 release metadataとしての変更contractがなく、
`po/POTFILES.in`はsource extraction inventory変更なし、completionはversion independent、CMake、
source、その他testsはrelease metadata preparationでは変更しないため、current listへ含めません。man
templateは`@VERSION@` authorityを維持しつつrelease dateを更新し、generated pageとともにcurrent
listへ含めます。`scripts/check_public_documentation.py`はrelease-noteのcurrent version assertionをroot
`VERSION`から導出し、formal RCのSSOT blockerを解消するためcurrent listへ含めます。
`containers/arch-live-validation/run-aur-install.sh`と`tests/test-live-contract.sh`は、Formal RCで検出した
AUR live artifact-evidence authority driftをcurrent production ownerへ同期するvalidation-side fixとして
current listへ含めます。v2.1.0固有の履歴は、下記の
`v2.1.0 post-release closure`として別に扱い、current listの根拠にはしません。将来のrelease
では、このlistを流用せず、そのreleaseで監査済みのexact path setへ置き換えます。

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
