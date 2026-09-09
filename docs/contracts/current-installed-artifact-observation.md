# Current installed artifact observation / pure Git comparison

## Scope

Issue #476 Slice 7-Aのproduction-disconnected foundation。
current observationはtransaction receipt、S5 `FreshInstalledArtifactBinding`、final build proofではない。
7-B [assessment coordinator](devel-package-assessment.md)がこのfoundationを消費する。
7-A自体はnetwork approval/queryを所有しない。7-C execution bridgeは別ownerが担い、7-D normal routeは未接続。

## Current installed owner

入口は`observe_current_installed_artifact_binding(const PackageChildIdentity&)`。
callerは解決済みのAUR child/PackageBase/source identityを渡す。
unknown source、空になったmoved-from identity、不正なchild/baseはExpectedIdentityで拒否する。
local pacman DBが証明するのはchild/base、version、architecture、installed recordのbytes/generationであり、
AUR URLのauthenticityではない。sourceはcallerのcorrelation contextを保持する。
provenanceからsourceをコピーする自己比較や、historical provenanceの採否はこのownerの責務ではない。

各callで次を行う。

1. fixed trusted DB worldを解決する。
2. `observe_installed_package_record`を呼び、新しいALPM handleとretained raw DB descriptorsで観測する。
3. actual child/baseとexpectedを照合し、version/architecture/MTREE/DB digest/generationのsemantic valueを準備する。
4. worldを再解決し、descriptor/named lineage identityが同じことを確認する。
5. ObservedまたはAbsentを返す。Absentでもworld再確認を省略しない。

recordのfreshness boundaryは既存raw readerの最終generation/descriptor reproofである。
追加のworld確認はDB worldに対する確認であり、worldとrecordをglobal atomic snapshotにしたり、
観測後のrecord不変を保証したりしない。remoteを挟む再確認は7-B coordinatorが所有し、7-Aには追加しない。

pacman transaction lock、S5 receipt/Post anchor/transaction lineageを追加しない。
XDG store lookup/publication/repair、namespace作成、PKGBUILD評価、checkout、Git queryを行わない。
raw readerの既存protocol/enum/mint境界は変更しない。

## Result / ownership / firewall

`CurrentInstalledArtifactBindingObservation`はObserved / Absent / Failureのvariant。
Observed armはprivate construction、const binding/world accessのみ。
semantic snapshotとしてcopy/move constructionとassignmentを許す。copyは観測を更新せず、
moved-from valueは通常のC++ value semanticsに従い、freshness capabilityとして使用しない。
copy assignmentは一時snapshotの完成後だけdestinationを置き換え、allocation failureでbinding/world pairを部分更新しない。
AbsentとFailureは単なるdiagnostic valueであり、公開aggregateを禁止しない。

Failureはstageとcauseを保持する。

| Cause | Meaning |
| --- | --- |
| existing `InstalledRecordObservationIssue` | low-level issueをそのまま保持。RecordChanged、DatabaseWorldMismatch、DatabaseLoadFailure、UnsupportedGeneration、ReadFailure、ResourceFailure等 |
| InvalidExpectedIdentity | resolved AUR contextとして使用できないinput |
| MalformedBinding | projectionのvalue validation failure |
| InternalFailure | producer内の予期しないexception |

bad_alloc/length_errorはstage付きResourceFailure。failure returnにdiagnostic allocationを要求しない。
low-level enumにないConcurrentReplacement/InternalFailureをlow-levelの観測結果だったと捏造しない。

`CurrentInstalledArtifactBindingObserver`のcomplete private declarationを、binding/current-resultの各granting headerから参照。
friendはown-I/O entryと必要なprivate value constructionだけ。
raw snapshot/generation/digest/path tuple、decoded bindingからObservedをconstructするAPIはない。
current resultからS5 fresh binding/receipt/final proof/S6 publication resultをconstructできない。

## Installed comparison

既存`compare_installed_artifact_binding`はpureのまま。
child → base → source → full version → architecture → MTREE → DB digest → generationの順で比較する。
同versionでも後半3 fieldの差をexact matchへ丸めない。
opaque generationはequality/inequalityだけで扱い、numeric/timestamp/monotonic orderingを仮定しない。

## Pure Git revision comparison

`compare_devel_git_revision(built, remote)`は2つの`UpstreamGitRevision` semantic valuesだけを受ける。
役割の由来をcallerが所有し、この関数はactual build proofやremote observationをmintしない。

判定順はInvalidRevision → SourceMismatch → ObjectFormatMismatch → SameRevision/DifferentRevision。
SourceMismatchはVcsSourceIdentity全体（location/selector/architectureを含む）の不一致。
SHA-1/SHA-256は既存SourceRevisionIdentityのtyped formatを比較し、同formatのtyped valueだけを比較する。
InvalidRevisionは空になったmoved-from等の不完全valueをSameと誤判定しないためのdefensive outcome。

raw OID string overload、pkgver/vercmp/AUR RPC、ancestor/newer判定、network query、
UpToDate/UpdateAvailable assessmentはない。differentは単に異なるrevisionである。

## Validation

- `test-current-installed-artifact-binding`: fresh ALPM/raw projection、Absent、world/record race、same-version drift、typed failure、read-no-create。
- `test-devel-git-revision-comparison`: SHA-1/SHA-256 same/different、reverse、source/format mismatch、moved value。
- `test-installed-artifact-binding`: existing pure mismatch taxonomy/order。
- `test-installed-package-record-observation`: existing low-level descriptor/generation/raw parsing regression。
- `test-reviewed-source-pinned-build`: S5/S6 existing negativesとS7-A current-owner construction negatives。
- `test-build-authority-closure`: new current observer/comparatorをnormal routeから切り離す。

#475 regressionは既存deterministic loopback fixtureを使う。new S7-A targets自体には#475 observer symbolをlinkしない。
