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

## Initial subset / intent policy

authoritative preparationはvalid typed pinと同じcheckout identity、AUR source/base、required childを照合する。
editor overlay、multiple required children、needed、rm-deps、未解決OnlyIfUpdated intentは開始前に拒否する。
Legacyを明示選択した場合は既存factory/compatibility semanticsを維持し、S6 publicationを開始しない。

one-pkgname/one-produced-artifact/one-floating-Git-source、HTTPS、DefaultHead/exact Branch、
architecture-independent/no-overlayは既存S3/S4契約に委ねる。bridgeはPKGBUILDやcurrent source definitionを
別に再評価せず、S4の既存evaluation/build protocolをそのまま使う。
S4がunsupported shapeを返した後にlegacy buildへretryする経路はない。

S4 artifact取得後はrequired childとsealed artifact identityを照合する。
trusted fixed DB worldとnormal intentのDB pathsが一致する場合だけ、fresh PackageMetadataSessionで
installed reason/versionを観測し、既存`map_installed_artifact_policy_state`と
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
| install/proof success + publication Failed | install successを維持、将来routeではpartial nonzero |
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
- normal routingとdry-run/queryは7-Dを参照する。Slice 8はNOT STARTED。

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
