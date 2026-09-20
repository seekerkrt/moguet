# Authoritative reviewed devel source execution bridge

## Scope / normal owner boundary

Issue #476 Slice 7-Cは、typed reviewed pinから既存S3/S4/S5/S6をconsumeする専用execution branchである。
`prepare_reviewed_production_source_execution`は、normal ownerの
`finalize_aur_checkout_authority`が`PinnedReviewedSourceBuild`を得た直後に使用できる選択API。
7-Dのnormal finalizerは`select_normal_reviewed_source_execution`からこの選択APIを使う。
7-C自体は既存sealed producersのbridgeを維持し、normal route policyは[7-D](devel-normal-routes.md)が所有する。

```text
typed PinnedReviewedSourceBuild + normal execution intent
  ├─ Legacy → existing ProductionArtifactSourceTree
  ├─ AuthoritativeDevel → PreparedReviewedDevelSourceBuildExecution
  └─ unsupported/invalid → ReviewedDevelSourceBuildRejected
```

Legacy armだけが既存factoryでpinを`shared_ptr<void>`のlifetimeに包む。
authoritative armはtyped pinをmove-owned stateへ保持し、type erasureを通らない。
raw `ProductionSourceBuildProvenance`、path、OID、`shared_ptr<void>`からpin/proofを再構築するAPIはない。
選択は明示的で、拒否/失敗から別armへ自動fallbackしない。

`ReviewedDevelSourceBuildIntent`は既存SourceBuildRequest、required targets、resolved DB paths、
rm-deps/no-confirmを保持するordinary intentであり、build proofではない。
provider/dependency/conflict/source-preferenceの準備は既存normal callerの責務を維持する。
7-B assessmentやplanning remote OIDをexecution inputにしない。
Git UpdateAvailableをpackage version constraint成立の根拠にしない。

## Invocation-local command presentation

Issue #595 Slice 1は既存`PresentationDetail`を、normal source-build callerの
`AppConfig.presentation_detail`から`execute_normal_reviewed_devel`、
`execute_reviewed_devel_source_build`、`acquire_pinned_submodule_closure`、root-tag command ownerへ
明示引数で渡す。同期実行中だけのpresentation dependencyであり、ordinary intent、prepared authority、
execution result、returned closure、source/tag/snapshot identityやpersistent provenanceには保存しない。
modeはexecution choice、validity、argv、timeout、failure分類を決定しない。

CLI relationの追加はremote `build --details`だけとし、local buildやplain/delegated pacman route等の
既存rejectを維持する。Slice 1ではNormal / Detailedとも従来のcommand文字列を`Logger::raw_cmd`へ渡し、
terminal表示とstate-log `EXEC`を維持する。compact表示とexact loggingの分離は後続Sliceの責務とする。

## Initial subset / intent policy

authoritative preparationはvalid typed pinと同じcheckout identity、AUR source/base、required childを照合する。
editor overlay、empty/duplicate required children、needed、rm-deps、未解決OnlyIfUpdated intentは開始前に拒否する。
Legacyを明示選択した場合は既存factory/compatibility semanticsを維持し、S6 publicationを開始しない。

exact declared/output child setsとone-floating-Git-source、HTTPS、DefaultHead/exact Branch、
architecture-independent/no-overlayは既存S3/S4契約に委ねる。bridgeはPKGBUILDやcurrent source definitionを
別に再評価せず、S4の既存evaluation/build protocolをそのまま使う。
S4がunsupported shapeを返した後にlegacy buildへretryする経路はない。

S4 artifact set取得後は既存N-artifact selectorでrequired Tとsealed Bを照合する。
trusted fixed DB worldとnormal intentのDB pathsが一致する場合だけ、fresh PackageMetadataSessionで
selected child名ごとにinstalled reason/versionを観測し、既存`map_installed_artifact_policy_state`と
`resolve_install_reason_directive`を適用する。

S5の`needed=false / PreserveExistingReason`を変更せず、reducerがDefaultを返す場合だけS5へ進む。

| Intent / current state | Bridge policy |
| --- | --- |
| new explicit install | Default、対応 |
| existing explicit → explicit update | Default、対応 |
| existing dependency → dependency update | Default、対応 |
| existing explicit requested as dependency | Default、explicitを維持 |
| new dependency install | AsDependencyが必要なので拒否 |
| existing dependency → explicit promotion | AsExplicitが必要なので拒否 |
| needed=true | preparationで拒否、強制reinstallに変換しない |

このsnapshot-based policyは既存artifact install reducerと同じ意味であり、DB全世界をlockする保証ではない。
実transaction/installed proofのauthorityは引き続きS5/pacmanが所有する。

## Execution / no fallback

`execute_reviewed_devel_source_build`はprepared stateを一度moveでconsumeする。
moved-from inputはnulloptで、context/build/transactionを再試行しない。

```text
create_invocation_owned_source_build_context(typed pin)
→ same-context make_makepkg_environment
→ build_evaluated_devel_source
→ required child / existing install-policy checks
→ prepare_evaluated_devel_source_artifact_transport(entire S4 proof)
→ execute_exact
→ transport.finalize
→ publish_installed_devel_source_build(entire S5 result)
```

actual workspace Git proof、archive hashing/correlation、receipt validation、fresh installed binding、
S5 final correlation、S6 projection/publicationをbridgeでコピー実装しない。
planning remote OIDをactual built OIDへ流用しない。publicationのOIDはS4 actual proofからS6が得る。

authoritative context creation以降は、失敗してもlegacy/shared mirror/別artifactへのfallbackを行わない。
S4 failureはoriginal typed failureを保持してS5へ進まない。
S5実行後は同じtransportを一度finalizeする。diagnostic returnの例外があっても再executeせず、
S5が保持する固定operation factsをfinalize/publisherへ渡す。

## Initial Missing bootstrapのclosure接続（Issue #564 4B2）

既存typed `devel_tracking_bootstrap` intentを持つ初期Missing executionは、S3とrecipe acquisition cleanup後に
initial selection→4A exact closure→4B0 explicit closure review→4B1 SourceReady→common S4を通す。
explicit migration acceptanceとrecipe full review/acceptanceをこのbridgeで短絡しない。
SourceReady consumerは同一selectionを保持し、prepared/post-build closure reproofとroot X相関だけを
workspace-specific branchとして加える。root tag mappingの取得・明示承認・projection・phase-point reproofも
同じowner chainで保持する（[Issue #589 contract](pinned-submodule-closure.md#root-tag-authority-issue-589)）。別build pipelineやS4 proofを作らない。

4A acquisition/review failureは`closure_failure()` / `closure_review_failure()`に元のprocess/cancel/cleanupを保持する。
SourceReady以降は既存build failureへnarrow closure detailを追加する。失敗からlegacyや別revisionへfallbackしない。
closure reviewのq/EOFは元の理由を`ConfirmationCancelled`へ戻し、既存runnerのformal cancellation処理へ渡す。
Noは既存recipe bootstrap reviewと同じ`Acceptance / ReviewOperationStopped / NonExplicitAcceptance`へ投影する。
required review未完了のためaggregateはnonzeroとなるが、actual build/internal failureとはtyped detailと診断で区別する。
これらのstopではbuild/installはNotAttemptedとし、live owner、先行R publication、cleanup consequenceを保持する。
runner/reducerのpure link境界を保つため、snapshotはownerのreview failureと既存required-review stop valueだけを追加で保持する。
S4取得後のartifact correlation、transport、install policy、S5/S6は上記のcommon executionをそのまま使う。
SourceReady whole ownerもS6→S5→S4 resultの寿命まで保持する。

初期Missingのmigration activationはexact target-less ordinary `-Syu` / `-Su` Autoだけに保つ。
通常Auto更新で既存selectorがAuthoritativeDevelを選んだexecutionも、既存
`ordinary_devel_package_base` intentにより同じselection / closure review / SourceReady / common S4へ接続する。
このintentは経路選択であり、Missing trial、upstream acceptance、built proofを生成しない。
既存baselineやreviewed recipe lineageを削除・再初期化せず、normal recipe review後に取得したexact upstream
snapshotを別途明示承認する。planning OIDやcacheを承認済みsourceとして採用しない。
No / q / EOFでは既存closure stopを返し、build / install / publicationへ進まない。
recipe revisionが同じなら既存Rを維持し、成功時だけS4/S5を根拠にS6が既存Pの後継generationを保存する。
同一revisionのUpToDateは従来どおり実行前に除外され、新しい承認promptを出さない。
non-devel、upgrade-aur/all、dry-run、explicit target、registered sourceには、このordinary activationを広げない。
provenance v1 / 27 keysは不変。ordinary split groupのselected child / install reason契約も維持する。

## Lossless result / lifetime

`ReviewedDevelSourceBuildExecutionResult`はmove-onlyでprivate construction。
default/copy/raw S5 result/raw S6 identityから構築できない。
complete private authority declarationを両granting headersから参照し、S4/S5/S6へnew friendは追加しない。

resultはnormal review outcome snapshot、stage、producer-local issue、S3/S4 failure、
fixed world/policy observation/directive、live S6 publication productを保持する。
`Finished`はpipeline終端であり、全成功という意味ではない。
`ReviewedProductionSourceExecutionResult`はlegacy resultとこのlive resultのdistinct variantであり、
旧`PackageBaseSourceBuildExecutionResult`のprivate constructorへraw successを押し込まない。

live S6 productからoriginal S5 operation、pacman status、transport result、receipt、binding/proof issue、
privileged cleanup、source context cleanupをconst診断として参照できる。

| Outcome | Preserved result |
| --- | --- |
| S4/context/policy failure | typed failure、S5/S6未開始 |
| known transaction nonzero | operation Failed(status)、publication NotAttempted |
| transaction OutcomeUnknown | original Unknown、publication NotAttempted、retryなし |
| install success + proof不足 | Succeededとproof/receipt issue、publication NotAttempted |
| install/proof success + publication Failed | install successを維持、normal routeへpartial nonzeroを伝播 |
| PublishedUncertain | publication OutcomeUnknown、adopt/rebase/retryなし |
| proof/publication Complete + privileged cleanup Failed | cleanup failureを独立保持、rollbackと扱わない |

outer storageとintent/source-provenance copiesはS3/S4開始前に確保する。
S5 finalize後のS6 ownership transferとouter result返却はnothrow move/primitive assignmentだけ。
S6内のfallible projection/publicationは既存S6 resultがS5 factsを保持したまま処理する。

S6→S5→S4 context lifetimeをそのまま維持する。source contextはlive resultの間Retainedで、
最後のowner destructionで既存local RAII cleanupが動く。destructor cleanup成功を事前にCompleteとはしない。
`owned_root()`は診断pathのみで、cleanup/reopen authorityではない。
destructorへprivileged cleanup、transaction/publication retry、store mutationを追加しない。

## Disconnection / escalation boundary

- new preparation/execution APIのnormal consumerは7-D reviewed route adapter。
- S6 publisherの新consumerは7-C execution ownerだけ。
- 7-B assessmentからexecutionへのdirect callは0。7-Dがpolicyとtyped reviewed selectionを結合する。
- normal routingとdry-run/queryは7-D、public contractとmigrationは[devel tracking](devel-tracking.md)を参照する。

S5 receipt/protocol/FreshInstalledArtifactBinding、S5 final proof authority、S6 publication/result/lifetimeは変更しない。
bridge外での新たなsource再評価や、既存sealed authorityを置換する新proof定義は追加しない。
これらの変更が必要になった場合は7-C内で拡張せず、Astra-xhigh architecture re-auditへ戻す。

## Validation

`test-reviewed-devel-source-build-execution`は既存S4 real Git/makepkg/archive fixtureと、
S5 isolated trusted helper state/process seamを使用する。raw S5/S6 Completeをmintしない。
typed pinからproduction bridgeを通してS6 readback、partial outcomes、reason policy、overlay/legacy分離、
one-shot、last-owner cleanup、publication確定後のallocation denialを検証する。

S5のshared fixture executableを使う既存focused targetsは別invocationで実行する。
normal CLI activationやactual host package DB transactionをこのdeterministic acceptanceに含めない。
