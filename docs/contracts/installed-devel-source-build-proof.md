# Installed devel source build proofとSlice 5 result

## Scopeとauthority

Issue #476 S5-Cは、[Slice 4 built proof](evaluated-devel-source-build-proof.md)と
[S5-B exact receipt / fresh binding](exact-installed-artifact-binding.md)を同一lineageで照合し、
`InstalledDevelSourceBuildProof`と`DevelSourceArtifactInstallResult`を作る内部producerである。
normal routeは7-CからS4→S5を実行し、このproof/resultをS6へ渡す。
S5自体はpublicationを行わない。[Slice 6-B](devel-build-provenance-publication.md)はこのresultをconsumeする別ownerである。
normal route policyと#475 comparisonは[7-D](devel-normal-routes.md)と7-Bの別責務である。

```text
ReviewedSourceState != built provenance
execution witness != transaction receipt != installed binding != final proof
operation receipt != DB anchor
fresh observation != same-transaction causality
package version != artifact identity
transaction success != binding/final proof success
persistent decoded binding != fresh installed capability
DevelBuildProvenance persistent value != InstalledDevelSourceBuildProof live capability
```

## Ownershipとclosed producer

入口は`prepare_evaluated_devel_source_artifact_transport(std::move(built))`、
`execute_exact(options)`、`finalize()`の順である。finalizeはtransactionやobserverを再実行しない。
`DevelSourceArtifactInstallAuthority`のcomplete private declarationを、friendを付与するnarrow headerから
参照する。同じcycle-free authority headerにtransportとtest fixtureのcomplete declarationも置き、
逆向きfriendもnarrow includeから閉じる。callerはraw path/FD/token/name/version/digest/generation、decoded binding、receipt-like struct、
別々に入手したcomponent tupleをfinal constructorへ渡せない。

元built proofにはbuildごとに独立したopaque in-memory identityがあり、proofとともにmoveする。
transportは実行前に別のtransaction identityとterminal stateを確保する。
receiptは元build identityとtransaction identityを保持し、fresh bindingはreceipt由来の同じtransaction identityと
stage identity、fresh descriptor-bound snapshotをprivateに保持する。公開tokenは既存diagnostic valueに限られ、
join/retry authorityではない。identityをpersistent schemaへ追加しない。

final producerは次を自身でも確認する。

- 元proofのopaque build identityとreceiptのbuild lineageが同じ。
- owner/receipt/freshのtransaction identity、token、purpose、stage identityが同じ。
- built artifact、receipt manifest/operation、fresh bindingが各1件。現行bridgeのselected indexは0。
- name、PackageBase、full version、architecture、source付きchild identityが一致。
- retained built archiveのSHA-256/sizeとreceipt inputが一致し、signatureは明示absence。
- built/receipt/anchor/freshのraw MTREE SHA-256が一致。
- fresh semantic bindingとPost anchorのopaque generation、raw desc/files digest、descriptor identityが一致。
- Installはbaseline absent、Upgradeはbaseline presentかつold/new generationが異なる。

generic receipt/transportのN>1対応は維持する。final producerのbuilt/receipt/fresh cardinalityが1以外なら
`UnsupportedCardinality`でno-proofとし、split package provenanceを実装済みと扱わない。

final proofはdefault/copy不可、move-only、private constructionである。元built proof、exact receipt、fresh binding、
context/retained artifact lifetimeを含む同じowned stateを引き取る。finalization時の追加allocationはない。
receipt/freshのmoveは元capabilityをinactiveにし、final proof/resultのmove元も使用を拒否する。
finalize後のtransportはstateを持たず、再finalize/executeできない。execute前のfinalizeはNotAttempted結果を返す。
legacy execute後のfinalizeは拒否し、legacy結果をexact proofへ変換しない。

## Lossless result

resultはclosedなmove-only型で、Complete armは`InstalledDevelSourceBuildProof`そのものを所有する。
元transport result、receipt/receipt issue、binding issueを保持し、operation/receipt/proof/cleanupを個別に読む。
callerがstatusとraw bindingを組み合わせてComplete armを作ることはできない。

| 状態 | operation | receipt | installed proof |
| --- | --- | --- | --- |
| 実行なし | NotAttempted | NotAttempted | NotAttempted |
| execution不明 | OutcomeUnknown | NotAttempted | NotAttempted |
| known nonzero | Failed(exact status) | NotAttempted | NotAttempted |
| known zero、receipt欠落 | Succeeded | Missing | NotAttempted |
| known zero、receipt不正 | Succeeded | Invalid | NotAttempted |
| known zero、receipt処理resource failure | Succeeded | Incomplete | NotAttempted |
| receipt成立、binding不可 | Succeeded | Complete | Incomplete(binding issue保持) |
| receipt/binding成立、final照合不一致 | Succeeded | Complete | Incomplete(final issue保持) |
| 全chain成立 | Succeeded | Complete | Complete(final proof) |

正常fileless packageの保守的`DatabaseLoadFailure`、`UnsupportedGeneration`、world/metadata/read/resource failureは
S5-Bのtyped issueをそのまま保持する。operation成功やreceipt成立を取り消さない。
known nonzeroはpartial filesystem/package DB effectsの不存在を意味しない。
OutcomeUnknownではprivate evidenceを保持し、consume/abort/retry、後付けDB観測、Complete推定を行わない。

operationのenumとnumeric statusはpositive execution witness確認直後、receipt/abortの処理前に確定する。
通常allocation failureでdiagnostic/result copyが失敗しても、確定済みoutcomeはallocation不要のterminal armへ残る。
final correlationはtyped issueを返し、既知成功をUnknown/PacmanFailedへ書き換えない。
process terminationや`std::terminate`までtyped returnを保証する契約ではない。

## Cleanupとの独立性

exact helperはretained evidenceのvalidationを終え、bounded response全体をowned bytesへ確保してから
active→used retirementとprivate stage cleanupを行う。outer evidence protocol v2の固定幅`CLEANUP` fieldは
`Complete`、`RetirementFailed`、`PrivateStageCleanupFailed`を区別し、failure記録に追加allocationを必要としない。
legacy cleanup-purpose protocolを変更せず、v1 exact outer evidenceをv2へ推定adoptしない。

outerはこのowned evidenceからreceiptをmintし、新しいDB observationを行い、finalizeでfinal proofを作る。
privileged stage/hook pathを後で読み直すことはない。retirement/cleanup失敗でもvalidation済みevidenceと
known successは残り、binding/final照合が成立すればproof Completeかつcleanup Failedとなる。
prepare/consume response欠落・不正でcleanup完了を確認できない場合はcleanup OutcomeUnknownを保持する。
helper nonzeroやabort failureも独立したcause/statusとして残す。cleanup failureをrollbackと扱わない。

source contextはresult/final proofが引き続き所有するため`Retained`である。現在のSlice 4 lifetime契約に従い、
最後のowner破棄時にlocal contextのRAII cleanupを行う。将来のdestructor cleanup成功を事前に主張しない。
final proofのdestructorはprivileged token/state、installed DB、provenance storeを変更しない。

## Validation

- `test-installed-devel-source-build-proof`: actual Slice 4/S5-B fixtureからのfinal proof、別valid build/receipt/bindingの混入、semantic/raw/generation/lineage/cardinality mismatch、move/one-shot。
- `test-devel-source-artifact-install-result`: 既存S5-B fixtureを再利用するfocused aggregate matrix、fileless/unsupported/no-proof、known nonzeroとcleanup failure。
- canonical negative compile: granting narrow headers、same-name authority spoof、raw component/tuple/decoded binding、copy/default、contradictory aggregateを拒否。
- `test-container-exact-installed-binding`: isolated anonymous volume上の実Install/Upgrade/reinstall/downgradeからfinal proofまで。host package DB非共有、publicationなし。
- `test-build-authority-closure`: source/link境界、S5 producerからstoreへのdirect callなし、7-C経由のnormal connection。
