# Trusted devel provenance publication

## Scope

Issue #476 Slice 6-Bは、[S5 installation result](installed-devel-source-build-proof.md)をconsumeし、
historical semantic provenanceを[既存store](devel-build-provenance-store.md)へpublicationする内部producerである。
normal CLI/source/upgrade/#476 routeは未接続。6-Cは既存publisherのactual/container acceptanceだけを追加する。
current binding re-observation/#475 comparisonは7-A/7-Bが、typed pinからの専用execution bridgeは
[7-C](reviewed-devel-source-build-execution.md)が所有する。normal route activationの7-DとSlice 8は未実装である。

```text
transaction success != receipt success != proof success != publication success
InstalledDevelSourceBuildProof != DevelBuildProvenance
decoded provenance != fresh live proof
generic store Published DTO != trusted publication Complete
publication failure != transaction rollback != installed binding invalid
```

## Ownership / construction

入口は`publish_installed_devel_source_build(DevelSourceArtifactInstallResult, request)`だけで、
S5 result全体をmoveする。invalid/moved-from inputはstoreを呼ばずnulloptを返す。
`DevelBuildProvenancePublicationAuthority`のcomplete private declarationをresult headerから参照し、
callerのraw provenance/binding/generation/path/decoded valueやstore Published DTOをmint inputにしない。
既存S5 proof/model/decoderへ新friendやmutable extractionを追加しない。

`DevelBuildProvenancePublicationResult`はdefault/copy不可、move-only、private construction。
元S5 resultをそのまま所有し、const `installation()`経由でoperation、pacman status、transport result、
receipt/issue、proof/binding issue、privileged/source cleanupを保持する。
move元はinvalidとなり、diagnostic accessを拒否する。pointer/referenceはownerのlifetimeに従うborrowである。

`NotRequested`もone-shotのterminal outcomeであり、後からpublicationを再開するhandleではない。
Failed、OutcomeUnknown、CAS conflict後のretry/rebase/republish APIは存在しない。
診断のためのproof保持と、再publicationの許可は別である。

## Projection / identity

内部ではconst final proofからだけsemantic valueを取得し、`make_devel_build_provenance`を
persistent consistency checkerとして使用する。S5のlineage/token/anchor correlationをコピー実装しない。

| Source authority | Persistent fields |
| --- | --- |
| built proof PackageBase / AUR source | source_kind、package_base、aur_git_remote |
| built reviewed binding | reviewed_recipe_oid、reviewed_state_generation、reviewed_state_document_sha256 |
| evaluated source / selector | evaluated_vcs_kind、evaluated_source_location、evaluated_selector_kind/value、evaluated_architecture_scope |
| actual built revision | actual_built_git_oid |
| retained artifact evidence | artifact_child/base/full_version/architecture、archive SHA-256、raw MTREE SHA-256 |
| final installed binding | installed_child/base/full_version/architecture、raw MTREE、raw desc/files digest、opaque generation scheme/identity |

schema v1 / 27 keysを維持する。transaction token、stage/hook/Post anchor DTO、temporary context/path、
retained FD/lease、PID、ALPM pointer/session、shared_ptr lineageは保存しない。

exact encoded bytesとそのSHA-256、PackageBase identity storageをstore write前に確保する。
既存store APIは同じimmutable semantic valueを同じdeterministic v1 codecでencodeする。
この限定されたencodeの重複により、raw documentを受ける新しいstore/mint APIを作らない。
CompleteのidentityはPackageBase、verified store generation、exact encoded document SHA-256である。
identity自体はcopyableなhistorical diagnosticsであり、raw identityからtrusted resultをconstructできない。

## Historical installed binding

documentはS5 transaction後の観測事実を保存する。publication直前のstill-current predicateは付けない。
publisherはworkspace/archive/stage/hook/installed DBを再openしない。
proof Aの後に別transaction Bでinstalled recordが変わっても、Aのbindingを保存し、Bへ推定更新しない。
storeのcurrent tipはpublication順であり、package transactionの全順序ではない。
7-Bがcurrent installed/#411/source identityを別途照合する。

## Publication product / ordering

public stateはNotAttempted、Complete、Failed、OutcomeUnknown。
issue/stage、projection checker failure、original store read result、original store publish resultを別に保持する。
storeのCasConflict、OverwriteRefused、FutureSchemaOverwriteRefused、UnsafeHistory、AuthorityUnavailable、
StoreFailure/ResourceFailure/InternalFailure、PublishedUncertainを上位のpacman failureへflattenしない。

| Installation / request | Publication |
| --- | --- |
| operation NotAttempted | NotAttempted(NotExecuted) |
| operation OutcomeUnknown | NotAttempted(OperationUnknown) |
| operation Failed(status) | NotAttempted(OperationFailed) |
| receipt non-Complete | NotAttempted(ReceiptUnavailable) |
| proof incomplete/invalid、fileless/unsupported/mismatch/cardinality failure | NotAttempted(ProofIncomplete) |
| eligible + NotRequested | NotAttempted(NotRequested) |
| local projection/serialization/resource failure | Failed(local cause/stage)、S5保持 |
| abnormal predecessor read | Failed(StoreReadRejected)、original typed read result保持 |
| verified store Published | Complete(identity) |
| definite store refusal/failure | Failed(StorePublicationFailed)、original typed store result保持 |
| store PublishedUncertain | OutcomeUnknown(StorePublicationUncertain)、commit evidence保持 |

eligibleになるまではstore read/prepare/publishを呼ばず、managed namespaceを作らない。
S5 ownership移動後にprojection、serialization/digest、predecessor readの順に進む。
publisherのpredecessor readは1回だけで、Missingはnullopt、Loadedはexact observed tokenを使用する。
existing store内部のsemantic preflight / locked CAS reproofはそのまま維持し、失敗後のread/rebaseは行わない。
同payloadでも正しいpredecessorでのpublicationはnew generationになる。same bytesによる成功adoptはない。

6-A storeがcommit phaseをtyped resultで閉じる。publisherはstore return後にcommitを推定し直さず、
nothrow moveとprimitive assignmentで結果を保持する。
通常bad_alloc/length_errorを含むlocal failuresでも、先にownedしたS5 resultは失わない。
process termination/std::terminate一般へのtyped return保証は含めない。

## Cleanup / lifetime

proof CompleteであればRetirementFailed/PrivateStageCleanupFailedでもpublicationできる。
必要な証拠はS5のowned sealed valuesであり、残ったprivileged pathを再採用しない。
publication Completeでもcleanup Failedは独立して残る。
source contextはS6 resultがS5 resultを持つ間Retainedで、最後のowner破棄時に既存local RAIIが動く。
destructor結果を事前にCompleteとしない。destructorにprivileged transactionやpublicationを追加しない。

## Validation

- `test-devel-build-provenance-publication`: real S4/S5 fixture outputからのprojection、historical drift、existing generation、move/one-shot。
- `test-devel-build-provenance-publication-result`: no-publication gates、store/refusal/resource/uncertainty mapping、cleanup failure。
- `test-container-devel-publication`: isolated actual Install/Upgrade/reinstall/downgradeからpublication Complete、raw document/readback/historyとstore世代1→2→3→4。installed世代とversion orderingはstore世代へ混ぜない。S5-only laneのpublication-noneは維持する。
- canonical negative compile: raw/decoded values、store DTO、private entry、same-name/reverse friend/late include、copy/default、contradictory aggregateを拒否。
- `test-build-authority-closure`: normal routeと#475からのdisconnection。

S5 publication-none testsは残す。今回のtestsはisolated temporary HOME/XDG_STATE_HOMEとexisting trusted helper seamを使う。
actual package transaction/container publicationは6-Cで別途検証し、このcontractのdeterministic PASSをその代替にしない。
