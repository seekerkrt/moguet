# Read-only devel package assessment

## Scope / input authority

Issue #476 Slice 7-Bのread-only coordinatorは
`assess_current_devel_package(const DevelPackageAssessmentTarget&)`だけをproduction入口にする。
targetは解決済みPackageBase/AUR source identity、同じbaseに属するinstalled child集合、既知develのhintを保持する。
normal query callerがsource resolutionとinventory groupingを所有する。空集合、複数child、source/base不一致はremote前に拒否する。
hintはMissing時の保守的な分類にだけ使用し、network authorityを与えない。

callerはprovenance、current installed observation、reviewed state、raw OID、parser resultを入力できない。
historical provenanceからexpected sourceをコピーする自己比較は行わない。
local DBはAUR URLを証明せず、#411 recordもevaluated Git URL/branchを保存していない。

initial subsetは既存schema v1のS4/S6 projectionに限定する。
exact reviewed recipeから得たone-pkgname/one-floating-Git-source、HTTPS、DefaultHeadまたはexact Branch、
architecture-independent sourceと、今回のsingleton installed targetを照合する。
S4で許可されたcontained local ancillary filesの意味は変更しない。
split/multi-floating-source/architecture-qualified/non-Git/tag/fixed/SSH/file/localへ拡張せず、PKGBUILD再評価もしない。
schema v1で表現できないsource形態は通常provenance decode時点でInvalidとして拒否される。

## P0 / I0 / R0 local gates

1. P0: `read_devel_build_provenance`でPackageBaseのcurrent tipだけを読む。
2. I0: 7-A `observe_current_installed_artifact_binding`でfresh DB observationを取り、
   P0のhistorical installed bindingと既存pure comparatorでexact比較する。
3. R0: `read_reviewed_source_state`でfresh current #411 stateを読み、
   P0のexact reviewed recipe/generation/raw-document digest bindingと既存pure comparatorで比較する。
4. P/I/R exact後だけ、P0に保存されたevaluated Git sourceをinstalled artifactの追跡sourceとして使用する。
   既存#475 HTTPS validatorとexact branch validatorでrequestを準備する。

store readerによるchain全体の安全性検証と、historical baselineの選択は別の責務である。
tipがI/Rと不一致ならRequiresCheckで止め、古いmatching generationを探して採用しない。
repair、republish、rebase、auto baseline更新は行わない。

`DevelPackageAssessmentAuthority`のcomplete private declarationを#475 granting headerから参照する。
approved sourceのconstructorを呼べる新ownerは、このlocal I/Oを所有するcoordinatorだけ。
result、target、persistent decoderへreverse friendは与えない。

## Remote / post-check

local/source gates成立後、`observe_git_remote_revision`を1回呼ぶ。
process/parser、fixed Git profile、timeout、capture cap、HTTPS/credential isolationは#475の既存実装を使用する。
branch validation failureはrequest gate failureであり、network observationとは数えない。

remote failureはUnknownとoriginal typed resultを返し、P1/I1/R1の再読は行わない。
remote成功の場合だけ、P1/I1/R1を各1回fresh readする。P1がfailureでも、この1組のI1/R1観測を保持する。

- P1はP0とexact observed record identity（generation、leaf、filesystem identity、raw contents）とsemantic provenanceが一致すること。
- I1はP0 historical bindingとexact一致し、I0/I1のtrusted DB worldも同じこと。
- R1はP0 historical #411 bindingとexact一致すること。

どれかが失敗/変化していればpositiveにせず、remoteの再query、retry、history adoptionを行わない。
全post-check成功後だけ7-Aのtyped Git comparatorを呼ぶ。
SameRevisionだけUpToDate、DifferentRevisionだけUpdateAvailable（basis=GitRevision）。
Differentはnewer/descendantの証明ではない。SourceMismatch/ObjectFormatMismatch/InvalidRevisionはpositiveにしない。

## State / diagnostic mapping

top-level stateは既存`DevelUpdateAssessmentState`を再利用し、pure factoriesも維持する。
`DevelPackageAssessment`は公開のpure decision/diagnostic productであって、mint capabilityではない。

| Observation | Assessment |
| --- | --- |
| non-AUR target | NotApplicable |
| Missing provenance、devel hint/suffix根拠もない | NotApplicable |
| Missing / Invalid / Corrupted / Future provenance | RequiresCheck(ProvenanceMissing / Invalid / Corrupted / FutureSchema) |
| P source/base mismatch | RequiresCheck(SourceIdentityChanged) |
| P unsafe/authority unavailable/store failure | RequiresCheck(BuildSourceProofUnavailable) |
| installed Absent / exact comparator mismatch | RequiresCheck(InstalledArtifactDrift) |
| installed observer failure | RequiresCheck(BuildSourceProofUnavailable)、original stage/cause保持 |
| reviewed Missing / generation・digest-only drift | RequiresCheck(NoAuthoritativeBuildProvenance) |
| reviewed recipe OID mismatch | RequiresCheck(AurRecipeAdvanced) |
| reviewed source/base mismatch | RequiresCheck(SourceIdentityChanged) |
| reviewed invalid/corrupt/future/unsafe/read failure | RequiresCheck(BuildSourceProofUnavailable) |
| invalid HTTPS / branch validation invalid・process failure | RequiresCheck(TransportRequiresCheck / SelectorRequiresCheck) |
| authoritative non-Git source（schema v1では通常decode拒否） | Unsupported(UnsupportedVcs) |
| #475 RefNotFound / Timeout | Unknown(RemoteRefNotFound / RemoteObservationTimedOut) |
| #475 ProcessFailure / GitExitFailure / CaptureLimitExceeded | Unknown(RemoteObservationFailed) |
| #475 MalformedOutput / AmbiguousOutput | Unknown(RemoteResultMalformed / RemoteResultAmbiguous) |
| P1 tip change | RequiresCheck(NoAuthoritativeBuildProvenance)、ProvenanceTipChanged detail |
| I0/I1 world change | RequiresCheck(InstalledArtifactDrift)、InstalledWorldChanged detail |
| Git source/object-format mismatch | RequiresCheck(SourceIdentityChanged)、original comparator detail |
| incomplete/moved Git revision | RequiresCheck(BuildSourceProofUnavailable)、InvalidRevision detail |

productはstage、P0/I0/R0、P1/I1/R1、各comparator、branch validation、remote original arm、post-check dispositionを保持する。
stderrからDNS/TLS/authentication failureを推測して分類しない。
resource/internal exceptionでproductを構築できなければ既存command例外境界へ伝播し、Unknownやpositiveへ偽装しない。

## Snapshot / side effects / disconnection

各readerが返したvalue snapshotを保持する。DB/XDG store lockをnetwork中に保持しない。
post-check成功も、return瞬間まで世界全体がatomic/permanently unchangedという意味ではない。
remoteを挟んだvalidated observation setであり、copyは追加queryをせず、transaction authorization/leaseにはならない。

coordinatorはprovenance/reviewed state publication、store repair、pacman transaction、S5 finalizer、S6 publisher、
PKGBUILD評価、checkout、build/installを呼ばない。
normal AUR RPC version-newer precedenceは7-Dのroute orchestration責務であり、このGit-only coordinatorには混ぜない。

production callerの境界:

- #475 remote observer、7-A current observer / Git comparatorの新consumerは7-B coordinatorだけ。
- coordinatorのnormal callerは7-DのAUR devel query adapter。
- S6 publisherのconsumerは別の7-C execution ownerだけで、assessmentからのcallは0。
- [7-D normal routing](devel-normal-routes.md)がassessment/policyと[7-C execution](reviewed-devel-source-build-execution.md)を結合する。coordinatorからbuild/install/publicationを呼ばない。
- public contractとmigrationは[devel tracking](devel-tracking.md)を参照する。

## Validation

`test-devel-package-assessment`はisolated HOME/XDG、real provenance/#411 stores、real libalpm temporary DB、
既存low-level generation seamsと#475 deterministic process seamを使う。
P/I/R local拒否のremote0、SHA1/SHA256、DefaultHead/Branch、remote全failure arms、tip-only history、
remote中P/I/R drift、post-check read counts、read-only inventory/bytesを確認する。
public upstreamへのlive queryは行わない。

canonical negative compileはS5/S6/S7-Aを維持し、S7-B narrow baselines3、positive1、negative17を追加する。
raw/parser/tuple bypass、approved-source constructor、same-name/reverse/late/namespace/inheritanceを拒否する。
