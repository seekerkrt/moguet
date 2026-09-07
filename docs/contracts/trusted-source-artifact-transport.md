# Trusted source artifact transport sealing

## Authority and threat model

この文書はIssue #476 F-S5-01 prerequisiteで強化した既存#485 SourceArtifactInstall transportの
契約を定める。S5-BのInstall/Upgrade receipt、fresh local DB、installed bindingは
[別contract](exact-installed-artifact-binding.md)を正とし、このsealing/lease/Unknown contractを維持する。

Threat Model Aは次をtrusted authorityとする。

- host kernel
- host root / system administrator
- Moguetのprivileged transaction helper
- 既存契約で検証されるfixed sudo / pacman / libalpm

防御対象は、unprivileged mutation、Moguet自身のvalidate-then-reopen TOCTOU、
stale pathname adoption、意図しないstage再作成、private parentのrebinding、
Moguetのtransaction flow内のprotocol/state driftである。
malicious root/admin、kernel compromise、外部privileged processの意図的tampering、
privileged debugger/ptraceは対象外である。hostile rootに対するsecurityを主張しない。

root ownershipだけをartifact identityとして扱わない。Moguetが証明したexact bytesとstaged generationを、
同じprivileged ownerによるfinal reproofからexecまで維持する。

## Transport identity

S5-Aでは`prepare_evaluated_devel_source_artifact_transport`がSlice 4の
`EvaluatedDevelSourceBuildProof`全体をmoveで受け取り、move-onlyの
`EvaluatedDevelSourceArtifactTransport`が元context / artifact FDを所有する。
default / copy / raw inputからのconstructionは不可で、completeなowner宣言を
friend付与元headerから参照する。normal CLI / source-build routeへは接続しない。

新ownerはexecute時にoriginal retained FDのidentity / size / named entryを再確認し、
同FDから再計算したarchive SHA-256をSlice 4保存digestと照合してからcopyする。
named entryは置換拒否のためだけに確認し、raw pathの再open、same bytesの新object採用、
旧workspace / prepared installの再構築は行わない。copy後にidentityを再確認し、seal済み
snapshot自身のSHA-256も保存digestと一致させる。signatureは明示的にsize 0 / `-`である。
exactly one childをindex 0として渡し、`PreserveExistingReason`、`needed=false`を固定する。
これはS5-Aの限定internal transport policyであり、通常routeのreason / skip policyは変更しない。

snapshot copy / sealとprivileged prepare→execute→execution-status→consume / abortは
既存coreを共有する。旧routeだけがcleanup expectation / observation / operation summaryを
生成し、新routeはそれらを空のまま返す。新routeの`Complete`は既存transport protocolの
完了区分であり、exact Install/Upgrade receiptやinstalled bindingを意味しない。
known outcomeは既存positive execution witnessを必須とし、exit 0後のconsume failureでも
`pacman_exit_status=0`を保持する。S5-Bは同ownerの別entry `execute_exact`からexact purposeを選び、
actual receiptとfresh bindingを別component outputへ保持する。S5-Cのfinal aggregate/compositionは未実装である。

新ownerはlocal rejectionを含めexecuteを一度だけ許し、move元と再実行を拒否する。
元FD / contextはowner破棄まで保持し、OutcomeUnknownでもtokenを診断用に保持する。
共有coreはUnknown時にabort / consume / retryせず、privileged stage / lifetime / evidenceを残す。
旧PreparedPackageBaseArtifactInstall routeのsnapshot失敗時のactive状態と、root prepare試行時の
consumption pointは維持する。

既存のPreparedPackageBaseArtifactInstallが保持するdescriptorからarchive SHA-256を取得する。
copy前のfile identityをcopy完了まで再確認し、write-sealed memfdからroot helperへ転送する。
hash implementationはSlice 4も使う既存XDG generation-store SHA-256であり、
共有実装を独立translation unitへ移した。algorithmとbyte semanticsは変更しない。

protocol v2はarchive digestを必須とする。signatureがあれば別のsignature digestとsizeを必須とし、
signatureがなければsize 0と明示的なabsence markerを要求する。MTREE、version、filename、sizeを
archive digestの代わりにしない。旧v1 prepared state / response / receipt、missing/malformed digest、
missing/old generation authorityをfail closedとする。

helper prepareはroot-owned private fileへexact copyした後、同じdescriptorのmetadataとSHA-256を確認する。
別のmandatory identity recordへ以下を束縛する。

- exact request bytesのdigest、token、schema
- runtime / private parent / transaction / artifact / hook directoryのdevice、inode、type、owner/group、mode
- prepared file、lifetime lease、hook、archive、signatureのdevice、inode、type、owner/group、mode、link count、size、mtime/ctime

regular fileのlink countは1を要求する。parent directoryのtimestamp/link countは別transactionや
receipt publicationで変化するためgeneration判定に使用しない。
このidentityはlive stagingのfilesystem observationであり、InstalledPackageRecordGenerationや
installed provenanceのpersistent generationへ流用しない。

same bytesでも新inodeへの置換は拒否する。digest equalityはstage generationの再採用許可ではない。

## Final reproof and execution

outer transportはprepare responseを根拠に直接pacmanを起動しない。
fixed sudoから同じinstalled root helperのone-shot executeを呼ぶ。

executeはprivate namespaceとstage descriptorsを保持し、saved generation、named/descriptor identity、
owner/mode/type/nofollow、archive/signature digestを再証明する。argv構築やtest-only race seamは
final reproofより前に置く。final proofからexecまでに同じexecute processが行うfilesystem writeは別のauthorization recordだけであり、
archive/signatureまたはそのparentを変更・再作成しない。
descriptorはatomic execまで保持し、fixed pacmanへ通常のstaged pathnameを渡す。

trusted rootが最終検証直後に意図的にraceするwindowは本threat modelの対象外である。
prepareはpublication前にtransaction直下へ必須の`lifetime` regular fileを1個だけ作る。
tokenを内容へ保存し、owner、transaction directoryと同じgroup、mode `0600`、link count 1、
exact generationを検証する。既存v2 stateでもleaseが欠落・置換されていれば再作成せずfail closedとする。
呼出側はlease pathを指定できず、nofollowで取得した同じdescriptorをgeneration reproofへ渡す。

executeとPostTransaction recordはこのlifetimeの共有flockを保持する。
one-shot `execution` claimは別fileの`O_EXCL`で取得し、lease解放後も再実行を拒否する。
abort/consumeとprepare publication後のfailure cleanupは、同じlifetimeの排他flockを
nonblockingで取得し、exact reproof、usedへのretirement、stage/authorityのunlink、最終syncまで保持する。
cleanup先行時はexecuteがclaim/authorizationへ進まず、execute/record先行時はcleanupが変更せず停止する。
busyは`TransactionLifetimeBusy`とerrnoで区別し、localized diagnosticをauthorityへ使わない。

Moguet自身による後続cleanupは、pacmanへ継承する共有leaseで拒否する。
helper所有FDのうちこのleaseだけの`FD_CLOEXEC`を意図的に解除し、他のnamespace/artifact FDはexecで閉じる。
outer waitがambiguousでも、実行中またはそのdescriptorを持つdescendantが残る間はearly cleanupしない。
leaseが残る場合はfail closedでprivate stateを保持し、blind retryや強制解除を行わない。
最後の継承FDが閉じられれば排他leaseを再取得できる。filenameの存在だけではbusyと判断しない。
exec failureはtyped refusalを残し、helper終了時にleaseを解放する。

recordはstageを再証明し、sealed execution authorizationとrefusalの不存在を確認する。
consumeも再証明し、failed sealingからComplete receiptを返さない。
Install-only hookの意味、dependency attribution、cleanup candidateの条件は変更しない。
Upgrade/reinstall/downgrade/needed skipをInstall receiptへ昇格しない。

## Signature and process behavior

### Execution outcome authority (F-R01)

`AUTHORIZED`はfinal reproofに基づく起動許可だけであり、exec完了やpackage-manager outcomeではない。
prepareは既存Install-only PostTransaction hookに加えて、Install/Upgradeをtriggerとする固定
PreTransaction hookをprivate hook directoryへ作り、両hookのbytes/generationをidentityへ束縛する。
この追加hookはfixed helperの`observe-execution <token>`だけを呼び、receiptを生成しない。
`AbortOnFail`を追加せず、Moguetの観測失敗でpacmanのsignature/transaction policyを変更しない。

positive evidenceの意味は「libalpmがこのtokenのPreTransaction hookを呼んだ」ことである。
signature validation完了やInstall成功を、このmarkerから推定しない。
既存PostTransaction recordも、その後段へ到達した事実を独立に証明できる。
PreTransaction markerが不在ならPostTransaction producerは自身のphaseを記録できるが、
partial/stale markerを修復したり、receiptの存在だけからmarkerを再作成したりしない。
PostTransaction hookはtransactionが完了しなければ走らないため、それ以前のfailureの証明には使わない。
phaseとNeedsTargetsの意味は[ALPM hook contract](https://man.archlinux.org/man/alpm-hooks.5.en)へ従う。

markerはroot-owned private transaction内の`execution-observed`である。
installed helperだけがSH lifetime下で、claim/authorization/refusalとstaged generationを再証明して作る。
callerはphase、marker path、marker contentsをCLIから指定できず、helperのroot境界を維持する。
token、exact identity recordのSHA-256（transaction/lease/archive/signature generationを含む）、
marker自身のdevice/inode/type/owner/group/link countを内容へ束縛する。
nofollow、0600、nlink=1、transactionと同じgroup、bounded read前後のmetadataを確認する。
別transaction、同bytes/new inode、stale stage/lease、partial/future schemaのmarkerは採用しない。
これは既存trusted root hook producerのauthorityであり、unprivileged fileやcallerのbooleanではない。

execution-statusとprivate claim/authorization/refusal recordsは明示的なv3とし、
`EXECUTION = Unobserved | PreTransaction | PostTransaction`を必須にする。
旧v1/v2・future schema・新field欠落を拒否する。prepare/response/Install receiptのv2 artifact契約は不変だが、
追加hookのない旧staged generationを再開・自動修復しない。
pre-exec record自身のEXECUTIONは常にUnobserved。positive phaseはseparate markerをroot側で再証明した結果だけに付ける。
claim欠落、authorizationなしのmarker、refusalとmarkerの共存等は矛盾として拒否する。

outerは終了codeを解釈する前にv3 statusを取得する。
valid typed helper refusalならArtifactSealingFailed、valid positive phaseとknown process outcomeが揃えば
package-manager outcomeとして扱う。nonzeroはPacmanFailedで数値を保持し、zero後は既存record/consumeへ進む。
positive evidenceが不足・破損・矛盾した場合は、zeroを含めOutcomeUnknownとしてprivate stateを保持する。
hook到達前の実際のpacman failureや、transactionがなくhookが走らない完全な`--needed` skipもUnknownとなる。
この保守的な区分はretry、consume、automatic abort、Complete、operation successを許さない。
Upgrade/reinstall/downgradeのPreTransaction markerは実行phaseだけを証明し、Install receiptへは昇格しない。

exec-error pipeのEOFはpre-exec terminationでも生じるためpositive authorityには使わない。
supervisor/ptraceによる全exec観測は追加せず、既存pacman hook authorityと未証明結果の保持にscopeを限定する。
refusal publication failure、pre-positive signal、same numeric helper/package exitは、authorizationやrefusal不在で推定しない。

packageとdetached signatureは通常のpathnameで隣接させる。
package inputを/proc/self/fd/Nへ変更せず、SigLevelを変更せず、署名承認をMoguetへ移さない。
pacmanは同じsudo entryが構築した環境を引き継ぎ、既存の署名policyとpromptを所有する。
利用者へpacmanのoptions/pathsとprivileged helper invocationを表示する。

helper execution refusalはroot-private structured statusから取得する。
package-manager exit codeとの数値一致やlocalized stdout/stderrから原因を推定しない。
prepared / generation / digest / replacement / protocol / executable launch failureはtyped causeを保持する。
nonzero outer outcomeの後、execution-status query/capture failure、malformed/truncated/旧schema、
token不一致、status欠落、または`authorized=false`でtrusted refusalのない応答は`OutcomeUnknown`とする。
この経路はabort/consume、retry、transaction restartを自動実行せず、private namespaceとtoken、
execution/authorization/refusal、stage、digest/generation、hook/receipt evidenceをそのまま保持する。
残存stateは診断・fail-closed recoveryのためであり、outer Complete receiptや新しいtrusted conclusionをmintしない。
validなtyped helper refusalとauthorizedな既知pacman failureには、既存のexact cleanupを適用できる。
pacmanが既にexit 0を返した後のconsume sealing failureは、既存operation successを保持したまま
receipt authorityを拒否する。これはfresh installed bindingを証明するものではない。

## Validation and scope

focused targetはtest-source-artifact-install-trusted-transportである。
S5-Aの`test-evaluated-devel-source-artifact-transport`はactual Slice 4 fixtureからreal helper stateへ
接続し、privileged process / pacman execだけをtest seamで置き換える。保存digest mismatch、
same-size / same-bytes replacement、hash→copy race、move / double consumption / execution、
FD lifetime、signature absence、Unknown保持、consume failure、final reproof refusalを確認する。
sealed input→helper stage→reproof→test-only execution replacementを通し、
same-metadata/different-digest、same-bytes/new-inode、metadata後/最終reproof前/record前/consume前の置換、
symlink、hardlink、mode、private parent、schema/digest欠落、signature drift、in-flight cleanupを確認する。
pipeで順序を固定したfork regressionはabort/consumeの両方向、double execute、recordとの共存、
real execとdescendantへのflock継承、他FDのclose、終了後cleanupを検証する。
status failure matrixはreal private stateを使い、Unknown時の全evidence保持、abort/retryなし、Completeなしを確認する。

signature regressionは、missing optional signature、present invalid signatureのALPM拒否、
signature-check-disabled policyでの受理、隣接signature bytes保持をread-only ALPMで確認する。
cryptographically valid署名を持つactual Installのlive evidenceへ読み替えない。
hostのpacman transactionはこのfocused targetでは実行しない。

normal #476 route、final Slice 5 proof、provenance publication、#475 comparisonは未接続・未実装のままである。
S5-Bのexact purposeだけにInstall/Upgrade receipt、fresh local DB observer、live InstalledArtifactBinding mintを追加し、
既存cleanup Install-only routeを拡張しない。

Refs #476
