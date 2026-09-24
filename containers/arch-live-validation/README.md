# Live validation lane contract

このディレクトリは、Issue #372 Slice 1で固定した **live** lane契約、
Slice 2のprovider-selection lane、Slice 3のreal AUR install laneを収める。

- live laneは既存のoffline validation lane (`containers/arch-validation`) と**完全に別入口**で運用する。
- ベースイメージは `archlinux:latest` を利用する。
- 実行時ネットワークは**有効**。
- ホスト `worktree`、`.git`、credentials、`/var/run/docker.sock`、XDG state、pacman DB/config/cacheを**共有しない**。
- `--mount` などの bind mount は**必須化しない**。
- `docker run --privileged` を使用しない。
- source build は、非 root の検証ユーザーで実行する。
- sudo はlaneごとにroot-ownedなcanonical `/usr/bin/pacman`だけへ限定する。
- ライフサイクル終了時（成功/失敗含む）はコンテナを破棄する。
- live case については暗黙のフォールバックを許容せず、外部レビューを要求する。
- live lane は `make test` / `release-check` を再帰的に起動しない。

## Fixture authority boundary

network非依存のdeterministic fixtureは`tests/fixtures/`に置き、live provider / AUR /
local payloadはこのdirectoryの明示的live targetだけが消費する。`make test`はlive stateを
取得せず、`test-fixture-authority`でtracked PKGBUILDとindependentなexpected `.SRCINFO`、
small scenario contract、transport固有payload authorityの内部整合だけを検証する。

local scenarioで共有するpackage / PackageBase、version、architecture、dependency constraint、
reviewed provider、install reasonは`fixtures/local-package/contract.env`をauthorityとする。
PKGBUILD、`expected.srcinfo`、`payload-authority.tsv`はshell contractへ統合せず、それぞれ
Arch build input、tracked projection、archive payloadという固有contractを維持する。
AUR scenarioのidentity、dependency、pin、source、artifact architectureは`aur-cases.tsv`、
payload path/type/mode/static hashは`fixtures/aur/payload-authority.tsv`がauthorityで、
Dockerfile、runner、root gatewayは共通loaderを通してcase値を読む。expected projectionや
payload expectedをMoguet本体のparser / formatter / resolverから生成しない。

## Slice 2: live provider selection

`make test-container-live-provider`は、`archlinux:latest`をpullして独立imageを
buildし、production `./moguet`、real PTY、real Arch repository metadataを使って、
local PKGBUILDの`cargo` dependencyに対するambiguous provider selectionを検証する。
candidate identityは`extra/rust`と`extra/rustup`のexact集合として解釈し、表示順や
versionは固定しない。集合、origin、repository、重複、またはpresentationがdriftした
場合は、別dependencyや先頭候補へfallbackせずreview-required failureとして停止する。

実package transactionは行わない。imageのprovisioningとMoguet buildが完了した後、
container内のcanonical `/usr/bin/pacman`だけをroot-owned・non-writableなsentinelへ
置き換える。sentinelはproduction Moguetが生成したexactな
`sudo pacman -S --asdeps --needed [--noconfirm] -- extra/{rust,rustup}` argvと、
multiple選択caseに限るレビュー済み2件のargvをbyte-safeに記録し、固定statusで停止する。
元のreal pacmanはvalidation userから実行不能なroot-only pathへ隔離されるため、
rust / rustupのreal install、`pacman -U`、remove、upgrade、未承認の複数target、
unqualified targetは実行されない。

各caseはtracked PKGBUILDだけをfresh user-owned directoryへcopyし、そのcopyで
unprivileged `makepkg --printsrcinfo`を実行してtracked `expected.srcinfo`とexact比較する。
case sourceへcontractやexpected dataを混ぜず、generated `.SRCINFO`もtracked fixtureへ
追加しない。before / after checksum、package database manifest、sentinel log、source /
artifact workspace不在を照合する。Moguetとmakepkgはいずれもvalidation userで動作する。

runtime networkは有効だが、host worktreeや`.git`、credentials、Docker socket、XDG
state、pacman DB/config/cacheをmountしない。containerは`--rm`で破棄する。Dockerの
image / layer cacheはhost localに残り得るが、host systemへpackage mutationは行わない。

## Slice 3: real AUR install

`make test-container-live-aur`はprovider laneと別のstandalone imageをbuildし、tracked
AUR package **fetchfetchだけ**をreal public AURで検証する。production AUR RPC、
PackageBase identity、real AUR git clone、PKGBUILD / `.SRCINFO` review boundary、real
source archive fetch、unprivileged `makepkg` build、artifact validation、real
`pacman -U` installを順に通す。`fetchfetch`、PackageBase `fetchfetch`、version
`2.0.0-1`、AUR git HEAD、PKGBUILD / `.SRCINFO` hash、source filename / URL / SHA-256、
artifact architecture、payloadをtracked authorityへ固定し、いずれかのupstream driftは
別package、最新version、先頭候補へ追従せずreview-required failureとして停止する。

AUR imageは、provisioning完了後にreal pacmanをroot-owned libexec pathへcopyし、
canonical `/usr/bin/pacman`をAUR専用gatewayへ置換する。non-root queryはreal pacmanへ
委譲するが、sudoersはgatewayだけを許可し、real pacman path、shell、copy tool、archive
tool、Pythonを許可しない。root gatewayはcase identityとexactな
`pacman -U --noconfirm -- <one-artifact>`だけを受け付け、`-S` / `-R` / `-Syu`、
追加option、複数artifact、cache外pathをfail closedで拒否する。Slice 2のprovider
sentinelをreal install用に緩和・転用しない。

gatewayはroot branch開始時に`PATH=/usr/bin`とし、Python caller environmentを除去した
うえで`/usr/bin/python3 -I`のroot-owned helperとfixed environmentのreal pacmanだけを
実行する。user-owned artifactをそのままroot pacmanへ渡さず、invocation-owned artifact
workspace内のregular fileを`O_NOFOLLOW`で固定し、root-owned stagingへcopyして
source-before / copy / staged / source-after SHA-256とsource inode/metadataの安定性を照合する。

image buildでは、固定AUR commit・PKGBUILD / `.SRCINFO`・source SHA-256から別workspaceの
reference packageをunprivileged `makepkg`でbuildする。同じimage filesystem/toolchainで作る
root-owned manifestには、binaryを含むpayload path/type/mode/owner/group/content hashと、stableな
`.PKGINFO` fieldのsorted duplicate-aware TSVを残し、reference source・workspace・package artifact
自体は削除する。static authorityはsource-carriedのREADME / LICENSE hashだけをrepositoryへ固定し、
generated binary hashはrepositoryへpinしない。gatewayはFD-based copyと4点のhash照合後、root-owned
staged artifactをdirect libarchive helperで全entry走査し、POSIX/default/NFSv4 ACL、任意xattr、file
flagsを拒否する。`bsdtar -tvf`はpath/type/mode/owner/group検査だけに使用し、permission listingを
ACL/xattr不在のauthorityには使わない。`.PKGINFO`はhard-coded allowlistとtransaction field denylistを
維持しつつ、`builddate`と`packager`だけを構文検証するvolatile fieldとして扱い、その他のidentity
field（`pkgdesc`、`url`、`license`、`size`を含む）はroot-owned manifestとexact multiset照合する。

binary-content、transaction field、xattr、ACL、`pkgdesc` authority driftの各negative artifactは、
独立workspaceでrepackしてgatewayへ渡す。いずれもstatus 97で拒否され、real pacmanへ到達せず、package
inventoryとpositive one-shot evidenceを変えない。accepted argv、original/staged path、hash、identity、
payload、direct metadata inspection、timestampはroot-owned evidenceとして残し、productionがoriginal
artifact workspaceをcleanupした後も診断authorityにする。

install結果は`fetchfetch 2.0.0-1`かつreason **Explicit** に固定する。imageに既存の
`glibc`、`gcc`、`make`はversion/reasonのbefore/afterが不変でなければ失敗し、package
database全体もfetchfetch 1件の追加以外のversion/reason変化を許可しない。containerは
`--rm`で破棄し、host worktree、`.git`、credentials、Docker socket、XDG state、pacman
DB/config/cacheをmountしないため、host package mutationは起こさない。runtime networkは
real AUR/RPC/git/source downloadのため有効であり、Dockerのimage / layer cacheだけは
host localに残り得る。official minimal imageの`NoExtract = usr/share/doc/*`に対しては、
root-owned container configへ`!usr/share/doc/fetchfetch/README.md`だけを追加し、tracked
payloadのREADMEをexactに展開する。host pacman configは参照・共有しない。

## Slice 4: real local PKGBUILD install and release gate

`make test-container-live-local`はprovider / AUR laneと別のstandalone imageをbuildし、
tracked local fixtureはvalidation-user buildより前にroot-owned read-only authorityへ固定し、
runtimeではPKGBUILDだけをfresh user-owned directoryへcopyしてproduction
`moguet --noedit build --local <directory>`で検証する。fixtureのmissing
`.SRCINFO`はexplicit evaluation gateを通り、real Arch repository metadataから選択した
`extra/rust` providerをdependencyとしてinstallした後、invocation-owned source snapshot、
unprivileged `makepkg` build、artifact identity validation、root gateway経由のreal
`pacman -U`によるexplicit installまでを通す。authorityとcase-local copyのmanifest、owner、
modeをbuild前に照合し、case-local root、root-owned fixture authority、host worktreeを変更しない。
root artifact transactionはroot-owned staging copy、four-way SHA-256照合、
contract由来の`.PKGINFO` identity、tracked payload path/mode/static marker hashの検証を
終えてからだけ許可する。

install reasonはlive package databaseで検証する。local root
`moguet-live-fixture 1.0.0-1`はExplicit、選択した`rust`と同時に導入されるprovider
closureはDependencyでなければ失敗する。container start時のpackage version / reasonは
不変でなければならず、root gatewayは`-Syu`、remove、unqualified provider、cache外
artifact、wrong install reasonをfail closedで拒否する。runtime networkはofficial
repository transactionのため有効だが、containerは`--rm`で破棄され、host package
database / config / cacheは共有しない。

`make test-container-live`は単一のfail-fast recipeからprovider、AUR、local PKGBUILDの三つの
live targetを順に再帰実行するrelease gateである。callerがparallel makeでもlaneを並行開始せず、
先行laneのfailure後は後続laneを開始しない。network、current Arch repository、public AUR、real package
transactionを含むため、`make test`と`make release-check`からactual live executionへ
再帰的に含めない。`release-check`は`test-live-contract`を通じてtargetのisolationとgate
compositionだけをstaticに検証する。release candidateでは通常のhost / offline
validationとは別に、明示的にaggregate targetを実行して結果を確認する。

Slice 4のnon-goalはselected repository providerのinstall reason preservationに必要な修正以外の
production C++、CLI、help、man、completionの変更、provider selection / AUR laneの緩和、
複数local root、clean chroot、package signing、host install、CI常時実行、既存offline laneの変更である。
