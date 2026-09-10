# Installed devel Git tracking contract

## 位置づけ

Issue #476で完成したauthority chainと、そのpublic compatibility / migration境界をまとめる。
Moguet v2.7.0で導入するauthoritative devel trackingの対応範囲と安全境界を定める。
各producerの詳細は下記の既存contractを正とし、新しいproof、store、transaction semanticsを定義しない。

## Authority chain

```text
#411 reviewed exact AUR recipe / typed pin
  → same-context evaluated source
  → invocation-owned recipe / PKGDEST / BUILDDIR / SRCDEST
  → actual pre/post-build Git revision (S4)
  → actual dynamic-version artifact identity / archive + MTREE digest (S4)
  → exact selected-artifact Install / Upgrade receipt (S5)
  → fresh transaction-bound installed binding (S5)
  → InstalledDevelSourceBuildProof (S5)
  → persistent historical provenance (S6)
  → current provenance / installed / reviewed (P/I/R) revalidation (7-B)
  → #475 remote observation → P/I/R one-time recheck
  → typed Git revision assessment
  → normal route policy (7-D)
  → authoritative reviewed execution (7-C)
  → new S4 → S5 → S6 publication
```

図のevaluated sourceとbuild contextは独立inputではない。S3でpinからcontextを作り、S4がその内部で
sourceを評価する。[S4](evaluated-devel-source-build-proof.md)がactual build authority、
[S5 receipt/binding](exact-installed-artifact-binding.md)と[final proof](installed-devel-source-build-proof.md)が
実install authority、[S6](devel-build-provenance-publication.md)がhistorical publicationを所有する。
[assessment](devel-package-assessment.md)のsnapshotはbuild authorizationやleaseではない。
[normal execution](reviewed-devel-source-build-execution.md)はexecution時のtyped reviewed pinから始め、
planning remote OIDをactual built OIDへ注入しない。

## Identity invariants

| 区別するidentity / outcome | 意味と検証owner |
| --- | --- |
| installed version != AUR RPC Version | normal RPC version precedenceとGit basisを分離。`test-aur-devel-route` |
| reviewed recipe OID != upstream Git OID | #411はrecipe、S4はactual working source。`test-evaluated-devel-source-build` |
| remote observed OID != actual built OID | observationは後日のref、build proofは同一invocationの実source。S4と`test-devel-package-assessment` |
| pacman exit 0 != installed binding proof | exact receipt、Post anchor、fresh DBとgenerationが別途必要。`test-exact-installed-binding` |
| current installed snapshot != transaction proof | 7-Aはcurrent observation、S5はtransaction-bound proof。`test-current-installed-artifact-binding`、canonical negatives |
| persistent provenance != fresh current authority | tip-only P/I/Rを再確認し、history fallbackしない。`test-devel-package-assessment` |
| transaction success != publication success | install成功＋publication Failed/OutcomeUnknownをpartial nonzeroで保持。`test-normal-reviewed-devel-execution` |
| store generation != installed-record generation | publication順とpacman record replacementは別。`test-container-devel-publication` |
| Git revision different != semver/newer/descendant | 同じsource/object formatのOID equalityだけを比較。`test-devel-git-revision-comparison` |

値が偶然一致してもauthorityを代替できない。installed versionが変わらなくてもMTREE、raw DB digest、
opaque record generationのいずれかが変わればhistorical bindingは無効となる。
identical artifactのsame-version / same-second reinstallもgenerationで区別する。安定したgenerationを
証明できないfilesystem/runtimeではtimestampへfallbackせずfail closedする。
installed versionがAUR/.SRCINFOと等しくても、全P/I/R gatesが成立しremote OIDがactual built OIDと異なれば
`UpdateAvailable(GitRevision)`となる。

## Initial authoritative subset / compatibility

AUR、valid #411 exact reviewed pin、observed editor overlayなし、architecture-independentなone floating
HTTPS Git source、DefaultHead/exact Branch、one pkgname / selected child / produced artifact / actual installed childに
限定する。追加sourceはreviewed tracked regular local fileだけ。full split provenance、他VCS/transport、
arbitrary PKGBUILD shell proof、external history adoptionは含まない。

単一artifact制約はpost-preparationの`--packagelist`とactual inventoryの両方へ適用する。
makepkgのdebug設定による追加outputも対象外で、Moguetがdebug policyを暗黙に上書きすることはない。
deterministic positive fixtureはreviewed recipeに`options=('!debug')`を明示し、Archのglobal defaultへ依存しない。

| Route / state | Current behavior |
| --- | --- |
| normal RPC Version newer | Version precedence。Git queryなしで既存candidateを維持 |
| valid Git same | UpToDate、automatic buildなし |
| valid Git different | GitRevision candidate。normal preflightとreviewed executionを通す |
| RequiresCheck | automatic buildなし。ordinary -Syuのindependent targetはwarning/skip、required relationとstrict routeはblock |
| Unknown | remote observation failureを保持、automatic buildなし／nonzero |
| Unsupported / unsupported devel source | automatic authoritative buildなし。suffixだけでVCSを確定しない。local proof不足はRequiresCheck |
| ordinary non-devel AUR | normal version policy。missing provenanceからGit baselineを生成しない |
| overlay / split PackageBase / legacy source build | eligibleなauthoritative routeへ偽装しない。explicit supported legacy intentは既存route、publicationなし |
| registered AUR OnlyIfUpdated | version-only shortcutより先に共通current assessment。RequiresCheckはdefault-Noの明示rebuild確認、source reviewは別途必要 |
| registered repository / local source | 既存source/version policy、#476 provenanceへ昇格しない |
| repo-only -Syu / standalone plan・deps | #476 Git query、build、publicationを追加しない |
| dry-run | normalと同じread-only assessment。checkout、PKGBUILD評価、execution、publicationは0 |
| -Qua | Version/GitRevisionの根拠を区別。同version Git差分にfake version arrowを出さない |

authoritative branch開始後にlegacyへfallbackしない。`--needed` skipをinstall済みとしてpublishしない。
unsupported needed/rmdeps/AsDeps/promotion intentをDefaultへ丸めず、既存install reason policyを守る。
route別の詳細は[normal route contract](devel-normal-routes.md)を参照する。

## Schema / migration decision

current [provenance store](devel-build-provenance-store.md)はschema v1 / 27 keysを維持する。
Slice 8でschema変更もmigration commandも追加しない。

```text
${XDG_STATE_HOME:-$HOME/.local/state}/moguet/devel-build-provenance/aur/<PackageBase>/
```

#411 reviewed-source stateとはnamespace、schema、CAS、publication timingが異なる。
serialized dataはhistorical evidenceであり、parser resultからlive proofをmintできない。
transaction token、FD、Post anchor、ALPM session、build-context lifetimeは永続化しない。

- known valid v1 recordはそのまま読むが、current authorityには必ずP/I/R revalidationを要求する。
- unknown/future schemaはfail closed。future versionを現行schemaとしてparse、downgrade、overwriteしない。
- corrupt/invalid/unsafe history、fork/gap/orphan、source mismatchをrepair/adoptしない。
- missing baselineはRequiresCheck（devel根拠なしならNotApplicable）。ordinary updaterは自動生成しない。
- #411、既存cache、AUR RPC、external helper履歴、installed snapshotからrecordを変換・昇格しない。
- 初回baselineは明示的にreviewした対応buildがS4→S5→S6を完了した場合だけ作る。
  `--noconfirm`、non-TTY、diff bypassはreview authorityを作らない。
- 有効tipの更新はexact predecessorのCASだけ。失敗後のretry/rebase、古いmatching recordへのfallbackはしない。

利用者によるstate fileの作成や編集は移行手順ではない。異常recordの自動修復やhistory adoptionは提供しない。
publication順がtransaction順と一致する保証はなく、後のcurrent readでinstalled bindingが合わなければ拒否する。

## Partial outcome / construction boundary

install成功＋publication Failed、install成功＋publication OutcomeUnknown、cleanup Failed＋publication Complete、
publication後の表示projection allocation failureはlive S4/S5/S6 productとtyped factsを保持し、partial nonzeroを返す。
後続work itemはNotAttempted。retry、rollback claim、exception-only aggregateへの偽装は行わない。
元のproduct/contextは最後のownerまで生存する。

actual production objectsのdirect callerは次だけである。

| Protected producer | Sole caller |
| --- | --- |
| observe_git_remote_revision | 7-B devel_package_assessment |
| observe_current_installed_artifact_binding | 7-B devel_package_assessment |
| publish_installed_devel_source_build | 7-C reviewed_devel_source_build_execution |

normal routeはこれらを直接呼ばない。S5-A/G9、S5-B、S5-C、S6-B、S7-A、S7-B、S7-Cのcanonical
construction negativesと意味のあるcompiler diagnosticsを維持する。raw DTO、persistent decode、parser resultは
live proofのconstructorへ到達できない。test seamは専用CMake profileだけに閉じる。

## Acceptance evidence boundary

[VALIDATION](../VALIDATION.md)のfinal acceptanceを同一candidateで実行する。
deterministic normal route/partial acceptance、actual Git/makepkg/archive、loopback HTTPS、actual isolated pacman
transactionを区別して記録する。S5-only laneはpublicationなし、S6 laneはraw document SHA-256・27 keys・
exact predecessor chain・actual S4 OID / artifact / S5 bindingとのreadback一致を要求する。
public provider/AUR/local live acceptanceやrelease approvalをdeterministic seamから推定しない。
