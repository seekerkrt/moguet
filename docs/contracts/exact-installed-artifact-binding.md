# Exact transaction receipt and fresh installed binding

## Authorityとscope

Issue #476 Slice 5 S5-Bは、[S5-Aのretained artifact transport](trusted-source-artifact-transport.md)から、
actual Install/Upgrade receiptとfresh installed DB bindingを作る内部producerを定める。
normal CLI/source route、S5-Cのfinal proof/composition、Slice 6のpublication、#475 comparisonには接続しない。

```text
execution witness != exact transaction receipt != fresh installed binding
PostTransaction operation receipt != PostTransaction DB anchor
fresh observation != same transaction causality
version equality != artifact identity
transaction success != binding proof success
persistent decoded binding != fresh live binding
```

kernel、root/admin、fixed pacman/libalpm、installed helperをtrustedとするThreat Model Aを維持する。
package transactionはpacmanが所有し、observerはtransaction/lockを開始しない。

## Purposeとactual operation receipt

既存`CleanupInstallOnly`はInstall-only receipt v2、cleanup candidate、reason attributionを維持する。
`Upgrade`、reinstall、downgradeをcleanup候補へ追加しない。

`EvaluatedDevelSourceArtifactTransport::execute_exact`だけが、元のSlice 4 proofを保持したまま
`ExactInstalledBinding` purposeを選ぶ。通常の`execute`と同じsnapshot/sealing/privileged execution engineを共有し、
同じownerの二度目の実行やmove元からの実行を拒否する。

exact prepared/prepare-response、operation fragment、private record、DB world/record、outer evidenceにはそれぞれ
独立したversion 1のgrammarを持たせる。execution-status v3とは別contractである。
exact prepared documentはpurposeと全selected artifactのraw MTREE hashを必須とし、内部のlegacy artifact projectionを
含む全bytesをmanifest identityへ束縛する。legacy v2単独をexact purposeへ補完/adoptしない。

root helperのfixed `record-install`と`record-upgrade`は、別のInstall-only/Upgrade-only PostTransaction hookから呼ぶ。
各entry自身がkindを固定し、stdinはNeedsTargetsのpackage名だけを受ける。
同名packageが両kindへ現れる場合やduplicate invocationはInvalidであり、sort/uniqueで修復しない。
reinstall/downgradeのkindも、[ALPM hook semantic](https://man.archlinux.org/man/alpm-hooks.5)に従って`Upgrade`のまま保持する。

fragmentは次を束縛する。

- token、owner、exact purpose、manifest SHA-256、staged identity-record SHA-256
- actual operation kind
- manifest由来のselected index/name、archive SHA-256、PackageBase/full version/architecture、built raw MTREE SHA-256

indexはvector位置ではない。manifestが持つ非連続indexを保持し、serialization順もmanifestから決める。
generic protocol/helperは複数artifactとmixed Install/Upgradeを扱い、selected index/nameの全単射を要求する。
一部skip・missing・extra・duplicate recordをexit 0やexecution markerから補完しない。

private `exact/` subtreeとimmutable `world`はprepare時のstaged identityへ含める。
operation/baseline/anchor recordはO_EXCL/O_NOFOLLOWでpartialをclaimし、retained FDのidentity、token、purpose、
manifest、stage、leafをself envelopeへ保存してno-replace renameでpublishする。bounded read前後にnamed/FDを照合し、
同bytesの別inodeも拒否する。partialはCompleteへ修復せず、completeとの共存も拒否する。

Post hookはactual operation fragmentをdurableにした後、独立したDB anchorを取得する。
anchorのMTREE/generation/DB/resource failureはcore receiptを消さない。anchorだけからoperation recordは作らない。

## Transaction worldとfresh observation

root helperはfixed `/usr/bin/pacman-conf --config /etc/pacman.conf --verbose RootDir DBPath`を使用する。
executable/configと親directoryはretained descriptor、owner、mode、named identityで検証し、PATH上の同名commandを使わない。
RootDir `/`をsupportし、DBPathとそのdescriptor ancestryをsealed worldへ保存する。
supported worldではfixed pacmanのargvへそのRootDir/DBPathを明示し、実行時にも利用者へ表示する。
`/var/lib/pacman`を無条件のtransaction authorityにはしない。

unsupported worldはtyped proof failureとして保存する。world観測失敗を理由にpacmanのtransaction policyを変更しない。
hook/outerのfixed world再解決がsealed worldと一致しなければ`DatabaseWorldMismatch`等となる。
caller-selected config/root/dbpathをlive mint APIへ渡す入口はない。

PreTransactionでは独立したexecution witnessのpublish後、同じworldから`KnownAbsent`またはold record snapshotを取得する。
baselineはreceiptでもtransaction成功でもない。observerに`AbortOnFail`を追加しない。

Post hook anchorとouter observationは、各call内で新しいALPM handleを作る。
既存`PackageMetadataSession`、cached local DB、`alpm_pkg_t*`を受け取らず、lazy metadataをscope内でowned valueへcopyする。
cache preload failureとpackage absenceを分け、関連recordのmalformed/duplicate core fieldsをabsenceに丸めない。

descriptor pathはDB root→local→exact package record→desc/files/mtreeとする。
record候補のdesc/files/mtreeをすべてnofollowでopen/retainしてから、retained descのbounded readとcore fieldによる選択を行う。
selected childを後から初めてpath openしてadoptせず、required childのidentityをdirectory timestampへ委譲しない。
同じrequired child FDsを最後まで保持し、read-close-reopenしない。
type、owner、writability、regular file nlink=1、親、named entry、読取り前後のidentityを照合する。
root/DB/localの他transactionによる内容変更と、selected recordのidentity変更を区別する。

raw descのsize/structural admissionとraw filesのbounded readを、ALPM session内のlazy metadata利用より前に行う。
empty noncore sectionは許容するが、次sectionをvalueへ吸収しない。NAME/BASE/VERSION/ARCHは一意なnonempty単一値を要求し、
重複・矛盾・欠落はgetter側の正常値で補完しない。XDATAは各行の`key=value` framingを事前確認する。

libalpmのlocal DESC lazy loadは、途中failureでも既取得getter値を残し、errnoへfailureを常に伝播するわけではない。
各getter直後のerrorとhandle-local error callbackに加え、DESC取得後に同じfresh handleで初めてpublic FILES cacheを要求する。
local DESC loadがerrorを保持すると未loadのFILES cacheへ進めないため、nonempty file listの取得を独立したDESC completion観測とする。
このlistはraw digest、installed generation、payload identityのproducerには使用しない。
空file listは正常なfileless packageとload failureを区別できないので、bindingは保守的に`DatabaseLoadFailure`でno-proofとする。
transaction成功と既成立exact receiptは保持する。private libalpm struct/flagへaccessするAPIやtransaction処理は追加しない。

## Raw identityとgeneration

raw desc/filesには4 MiB、raw MTREEには64 MiBの上限を設ける。
desc/filesのNULは拒否し、digest preimageを`"desc\0" || raw_desc || "files\0" || raw_files`とする。
binary MTREEへtextのNUL拒否を適用せず、whitespace、field順、改行、gzip headerをnormalizeしない。
parsed MTREE entriesをraw identity producerには使わない。

fresh recordのname、PackageBase、full version、architectureはselected built artifactとexact一致させる。
required metadataのmissing/unknownやraw descとALPM getterの矛盾はproof failureである。
`any`をhost architectureへ変換しない。

次をすべて要求する。

```text
built.raw_mtree_sha256 == hook.raw_mtree_sha256 == fresh.raw_mtree_sha256
hook.world == fresh.world
hook.record_generation == fresh.record_generation
hook.raw_desc_files_sha256 == fresh.raw_desc_files_sha256
hook.descriptor_identity == fresh.descriptor_identity
```

generationはretained record FDへの`name_to_handle_at(fd, "", ..., AT_EMPTY_PATH)`から取得する。
size 0→EOVERFLOW sizing→1..128 byteのbounded allocation→retry→actual length exact一致を要求する。
EOPNOTSUPP、EPERM、zero/excessive size、changed length等は`UnsupportedGeneration`へ倒し、truncate/fallbackしない。

opaque encodingは`linux-name-to-handle-at-v1|fs=<16 hex>|fsid=<2 x 8 hex>|type=<8 hex>|length=<decimal>|handle=<hex>`。
filesystem magic、f_fsidの2つのnumeric word、handle type/length/bytesをcanonical順で保持する。
mount ID、inode、st_dev、ctime/mtime、install timeはpersistent generationの代わりにしない。
既存provenance schemaのopaque generation fieldを使用でき、XDG schema/storeを変更しない。

Installはbaseline absentと新record、Upgradeはbaseline presentとold/new generation不一致を要求する。
same-version reinstallでもversion equalityで省略せず、新generationを必要とする。
hook後のidentical reinstallはouter generation不一致で拒否する。

## Sealed component outputsとfailure

`ExactArtifactTransactionReceipt`はknown successful pacman outcomeとcomplete actual operation recordsが揃った場合だけ、
retained-proof ownerがprivate constructorで作る。Pre/Post execution markerやexit 0単独は代替入力にならない。

`InstalledArtifactBindingObserver`だけがnew live `InstalledArtifactBinding::make`とgeneration constructorへ到達できる。
friend付与元はcomplete private declarationを共有し、same-name spoofとraw path/fd/tuple factoryを拒否する。
observerの入力はsealed exact receiptと元のevaluated build proofであり、自身のfresh observationからのみmintする。
`FreshInstalledArtifactBinding`はhistorical bindingと別のmove-only component capabilityである。
persistent decodeはhistorical value復元のまま、fresh wrapperへの昇格経路を持たない。

`execute_exact`のtransaction resultと、owner上のreceipt/receipt issue、fresh binding/binding issueは別に保持する。
known success後のMTREE、generation、ALPM、world、resource failureで`pacman_exit_status=0`を取り消さない。
receiptがCompleteでもbindingがIncompleteであり得る。failed/OutcomeUnknown transactionではfresh live mintしない。
Unknownはprivate evidenceを保持し、abort/consume/retry/Completeへの推定を行わない。

このbindingが示すのは、exact trusted artifact transactionのPost anchorと一致する観測時点のlocal DB recordである。
全installed payload bytes、scriptlet後の全filesystem state、NoExtract/NoUpgradeの完全payload一致、未来の不変性、
malicious root/adminに対する耐性を保証しない。

S5-Cの`InstalledDevelSourceBuildProof`、元build proof/receipt/bindingのfinal sealed composition、lossless top-level aggregateは未実装。
Slice 6 publication、XDG provenance write、normal #476 route、#475 comparisonは未接続である。

## Validation入口

`test-exact-artifact-transaction-protocol`、`test-exact-artifact-transaction-receipt`、
`test-installed-package-record-observation`、`test-exact-installed-binding`がcomponent/結合matrixを所有する。
construction firewallは既存canonical CMake negative compileへ登録する。

`test-container-exact-installed-binding`はnetworkless Dockerのanonymous volume DBに対し、
実際のSlice 4 proof、installed helper、Install/Upgrade/same-version reinstall/downgrade、raw MTREE一致、
通常userのfresh live mintを検証する。host package DBをmount/変更しない。
既存source-artifact receipt/installed-binding characterization laneも独立したregression evidenceとして維持する。
