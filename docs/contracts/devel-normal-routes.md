# Authoritative devel normal routes

## Scope / decision owners

Issue #476 Slice 7-Dは既存7-B assessmentと7-C executionをnormal routeへ接続する。
新しいbuild/installed/remote approval/publication proofは定義しない。全体のpublic contract・migrationは
[devel tracking](devel-tracking.md)、final acceptanceは[VALIDATION](../VALIDATION.md)を参照する。

`aur_update_query` → `aur_devel_update` → `assess_current_devel_package`を共通read-only ownerとする。
normal AUR RPC VersionとGit revisionは独立したauthorityである。

- RPC Version newerは通常のUpdateAvailableを優先し、7-B/#475を呼ばない。
- Same/Olderだけを7-Bでrefineする。PackageBase/sourceはAUR resolutionから作り、provenance自身からコピーしない。
- configured pacman inventoryが7-Bのfixed trusted system DB worldと一致することを確認する。
- full installed inventoryを読み、同baseの全childをtargetへ渡す。selected/foreign subsetからsingletonを捏造しない。
- 不完全なgrouping、別DB contextはpositiveを作らない。各childのP/I/Rを照合する。元のworld/inventory evidenceをquery resultへ保持する。
- suffixはMissingをNotApplicableへ落とさないhintだけ。Git approvalは7-BのP/I/R gatesだけが所有する。
- current tipのみ。history fallback/adoption、repair、global cache、retryは追加しない。

`AurUpdatePlanEntry.devel_assessment_origin`はproducerを区別するpure routing metadataであり、
CurrentObservation自体がauthority成立を意味しない。decision、元の7-B product、target contextを別に保持する。
copyはsnapshot copyであり、fresh query/leaseではない。
`aur_update_basis`はVersion/GitRevisionを保持する。DifferentRevisionはnewer/descendant/package version増加を意味しない。

## Route policies

| State | ordinary target-less -Syu AUR | upgrade-aur / upgrade-all AUR | registered AUR update |
| --- | --- | --- | --- |
| UpToDate | candidate除外 | candidate除外 | current develならsource buildをskip |
| UpdateAvailable / GitRevision | normal preflight後にreviewed execution | 同左 | version-only shortcutを迂回してreviewed execution |
| RequiresCheck | independentはwarning/skip。初回Missingだけは下記の明示bootstrapを提示可能。required dependency/provider/childへ再entryならblock | whole-operation block/nonzero | default-Noの明示rebuild確認。declineはtyped Incomplete、acceptedでも別途normal reviewed pinが必要 |
| Unknown | hard observation failure、build0 | block/nonzero、build0 | failure/nonzero、automatic build0 |
| Unsupported | auto candidateにしない | block/nonzero | automatic build0。explicit supported legacy intentは別経路 |
| NotApplicable | normal version policy | normal version policy | 既存registered version/source-baseline policy |

repository-only -Syu、standalone plan/depsにGit queryを追加しない。
actual upgrade-allのAUR queryは先行system/source phase後にfreshに実行する。
registered sourceはexecution時のpost-system installed stateを使い、既存.SRCINFO version-only skipより先に共通queryを使う。
未installとして観測済みのconfigured sourceは、既存explicit new-source intentを維持する。

7-Bはpackageごとにlocal gates→remote1→success時local recheckを行う。
7-Dはexecution前の念のためremote queryを追加せず、planning OIDをbuild inputへ渡さない。
phaseを跨ぐ観測は再利用しない。複数baseで同じGit sourceを使う場合のdedupはfollow-upとし、global cacheはない。

## Execution selection / outcomes

normal `source_build::finalize_aur_checkout_authority`がtyped pinを得た位置から
`select_normal_reviewed_source_execution`へ渡す。選択はLegacy / AuthoritativeDevel / Reject。
Legacyだけが既存type erasureへ進む。Reject/authoritative failureからlegacyへfallbackしない。

GitRevisionのautomatic candidateは対応するroot childにだけauthoritative selection intentを付ける。
そのdependencyへ同じflagを無差別伝播しない。flagはproofでもOIDでもない。
explicit buildはreviewed .SRCINFOのsyntaxをselection hintにできるが、evaluated source proofは既存S4だけが作る。
no-overlay、one floating HTTPS Git、DefaultHead/Branch、architecture-independentを維持する。ordinary split集合対応は下記に従う。
needed/rmdeps/AsDeps/promotion等の非対応intentをS5 Defaultへ偽装しない。

通常Auto更新の`ordinary_devel_package_base` intentでAuthoritativeDevelが選択された場合は、
既存のexact closure取得・別途明示承認・SourceReady workspaceをcommon S4へ渡す。
GitRevision更新でも既存P/RをMissingへ戻さず、通常のrecipe reviewとsource snapshot acceptanceを分離する。
この分岐は既存のordinary execution scope内だけで、standalone/registered routeへ波及させない。
UpToDateでは実行・source reviewを開始しない。closure未承認・失敗からplain/legacy経路へfallbackしない。

既存#411のinteraction policyを維持する。--noconfirm/non-TTY/explicit diff bypassからreviewed pinをfabricateしない。
Git automatic branchでcompatibility checkoutしか得られない場合はreject。
explicit supported compatibility buildは従来Legacyであり、S6 publicationを起こさない。

normal package-base executionはlegacy resultと`ReviewedDevelExecutionSnapshot`のvariantを返す。
後者はpure diagnostic adapterで、元のmove-only 7-C resultをimmutable shared handleで所有する。
handle copyは同じproductのlifetimeを共有するだけで、S4/S5/S6をcopy/remint/extractできない。
実行slotはside effect前に確保し、元の7-C productをnothrow moveで格納してからfallible diagnosticsを投影する。
投影allocation failureでもownerを失わず、complete successへ丸めない。

operation/receipt/proof/publication/privileged cleanupを独立保持する。
install success + publication Failed/OutcomeUnknown、cleanup Failedはpartial nonzero。
transaction Unknownやknown nonzeroにpackage effect不存在を推定しない。publication failureからrollbackを主張しない。
last owner destructionまでS6→S5→S4 contextを保持し、destructorでtransaction/publication retryをしない。

## Dry-run / query / presentation

normal AUR queryとdry-runは同じ7-B producerを使う。registered upgrade/upgrade-all dry-runも同じcurrent queryを読む。
read-only registered observationsはnormal source attributionへ照合し、RequiresCheck/Unknown/Unsupportedをtyped blockersへ投影する。
dry-runはexplicit rebuild確認、source checkout、PKGBUILD評価、build/install/publicationをしない。
registered sourceのfuture OnlyIfUpdated intentとcurrent assessmentは別観測であり、actual post-system stateと一致する保証はない。

-Quaとunified planはGit revision differenceを明示する。same-version Git updateに架空のversion arrowを出さない。
UnknownをRequiresCheckのreasonへflattenしない。strict routeのfailureは先行phaseの確定結果を消さない。

## Closure / validation

- #475 direct caller: 7-B coordinatorのみ。
- S6 publisher direct caller: 7-C ownerのみ。
- current installed observer direct caller: 7-B coordinatorのみ。
- 7-B caller: normal devel query adapter。
- 7-C prepare/execute caller: normal reviewed route adapter。normal source finalizer/executorだけがそのadapterを呼ぶ。

`test-aur-devel-route`はreal isolated P/I/R/ALPMと#475 seamをnormal RPC queryへ結合する。
`test-normal-reviewed-devel-execution`はTTYのnormal finalizerから既存S4/S5/S6を通す。
legacy-only unit profilesのstubはlive authorityをmintせず、authoritative executionへ入ると失敗する。
既存firewallsとnormal route regression、-Qua、dry-run/unified projection、frontend gateも併用する。

## Normal invocation partial outcome (#476 S7D-AUD-01)

`execute_prepared_source_build_invocation`は通常returnされたauthoritative partialを
`ProductionSourceBuildWorkItemStatus::AuthoritativePartial`として返す。例外の`Failed`とは別であり、
`failure_exception`を捏造せず、既存exception-only constructorも使用しない。
whole 7-C productとoperation/receipt/proof/publication/cleanupはaggregateの`devel_execution`に残る。
後続work-itemはNotAttempted、`is_success()`はfalse、`command_exit_status()`は1。
callerはこれを確認して後続buildを開始しない。install成功をrollbackや未実行へ読み替えない。

aggregateのwork-item storageは実行前に用意し、terminal ownerをnothrow moveしてから表示用copyを試みる。
copy allocation failureはprojection_failed/partialになり、live ownerを保持する。
S4/S5/S6のproof・lifetime契約と、本当にthrowされたlegacy failureのexception契約は変更しない。
`test-normal-reviewed-devel-execution`はsingular/PackageBaseSetの各4 partial casesを実集約ループへ通し、
real reviewed finalizer/S4/S5/S6、one-shot、nonzero、last-owner cleanup、persistent allocation denialを確認する。
そのprofileだけにあるdispatch seamはhost cache/provider操作を省き、raw Completeを注入しない。

## Explicit initial tracking bootstrap (#553)

有効化するのはexact target-less ordinary `-Syu`のactual Auto coordinatorだけ。
query / -Qua / dry-run、standalone upgrade-aur、upgrade-allはbootstrap intentを生成しない。
source-buildやtarget grammarの一般policyを変更しない。

- `is_initial_devel_bootstrap_observation`はCurrentObservation、initial Provenance stage、beforeの
  StoreMissing、ProvenanceMissing reason、exact target correlationを要求する。remote後の消失や他reasonは除外する。
- `observe_aur_devel_bootstrap_candidates`は元query index/evidence/contextを維持してtrial intentを付ける。
  RequiresCheckやVersion/GitRevision basisを変更しない。
- trialはP/I/Rとfull installed groupingをread-onlyで観測し、recipe HEAD OIDをrepositoryless Gitで取得し、
  そのexact idのAUR cgit metadataをboundedに読む。old persistent recipe checkoutは観測しない。
  complete declared/installed child集合、exactly one floating HTTPS Git source、DefaultHead/Branch、source qualifierなしに限定する。
  source全件を分類し、合計1..64件、Git以外はrecipe直下のrenameなしlocal basenameを最大63件まで候補にできる。
  local名は255 bytes以下のASCII英数字・`_`・`-`・`+`・`.`に限定し、dot始まり、`..`を含む名前、`PKGBUILD`を拒否する。
  `.SRCINFO`、Git metadata、private `.moguet-*` inputsもdot始まりとして除外する。extension whitelistは設けない。
  duplicate名とmakepkg Git destination（明示alias、またはURL leafの`.git`以降を除去）とのcollisionを拒否する。
  second Git、remote supplemental、nested/renamed local、他VCS、dynamic declarationは対象外。
  localのtracked/regular/no-symlink/exact bytesはtrialでは証明せず、full review → exact reviewed tree → S3 → S4へ委譲する。
  localはreviewed build inputであり、scalar baseline revisionは引き続きone floating Gitだけが所有する。
  packageのmultiple declared archは既存S4契約に従い、architecture-qualified sourceとは区別する。
  source evaluation、cache作成、checkout、review authority、publicationはこの観測で発生しない。
- trial failure/unsupported/判定不能は従来skip。P/R invalid、corrupt、future、unsafeをMissingへ丸めない。
  trial network failureは既存valid provenanceに対する#475 Unknownと別の試行適格性観測である。
  recipe HEAD応答は256 bytes・30秒の既存bound内で、完全な`<OID><TAB>HEAD<LF>` recordだけを解析する。
  同一OID・同一HEADの完全同一recordは一意化するが、異なるOID、別ref、不正・未完・余剰bytesは拒否する。
  この正規化はrecipe trial専用で、#475 upstream observerのduplicate rejectionは変更しない。
  HEAD process failure/timeout/overflow、malformed/conflicting response、HTTP metadata unavailable、
  metadata parse failure、unsupported source/count/local shape、multiple tracking roots、destination collisionは
  `DevelTrackingBootstrapUnavailableReason`で区別し、collectorが
  `AurDevelUpdateObservation.bootstrap_unavailable`へ保持する。元のassessmentは変更せず、現行presentationは
  従来のgeneric warning/skipへまとめる。typed reason自体はbuild/review/provenance authorityにならない。
- 同じcombined BuildPlanを一度解決し、既存required relation projectionを適用する。
  bootstrapの自分自身のsingular Root artifactだけをそのtrial intentとして区別し、cross-rootの
  dependency/provider/child/shared-base relationは従来どおりblockする。
- runnerのbootstrap decision phaseはcache activation/shared provider transactionより前。
  query index順にdefault-No確認を行う。declined/changed/unavailable rootだけに属するwork itemとproviderを
  実行対象から除き、元のindex/root attributionとtyped skipを結果へ残す。名前で再検索/replanningしない。
- bootstrap確認前、Yes後、source実行開始時に既存local/source観測を再検証する。Missing snapshotをleaseとしない。
  source reviewはtrialと同じexact recipeを要求し、変化時はfail closed。新global lockやretryは作らない。
- accepted intentは専用full-review purposeを要求する。元R observed record/CASを保持し、same/changed revisionでも
  full inventoryを提示する。--noeditはeditor省略だけ。review bypass/declineからlegacy buildへfallbackしない。
- actual build/install/publicationは既存7-C→S4→S5→S6。trial metadataはproofにならず、actual built OIDはS4だけが所有する。
- declineはIndependentDevelRequiresCheck skip。cancel/EOFは既存AurUpdateExecutionCancelled→FilteredAurUpdateCancelledで
  partial resultを運ぶ。decision phaseのcancelでは他work itemは未実行、review中のcancelでは完了済みprefixを保持する。
- accepted後のfailure/partialは既存first-failure stop。S6 Failed/OutcomeUnknownをinstall outcomeと分離し、retry/rollbackしない。

`test-aur-devel-route`が試行適格性/reason/local drift、`test-reviewed-source-lifecycle`がfull-review purposeとCAS保持、
`test-devel-tracking-bootstrap`がactual route/confirmation/review/S4/S5/S6と同一fixture再assessmentを検証する。

## Isolated recipe acquisition foundation (#564 Slice 3A)

`acquire_invocation_owned_recipe(const DevelTrackingBootstrapTrial&)`は、initial migration用の
取得foundationである。Slice 3Bでは上記initial Missing migrationのexplicit Yes後だけに接続する。
clean/dirtyを問わずfresh acquisitionを使い、trialと再検証はold PackageBase checkoutのclean gateへ依存しない。

入力は既存typed trialのcanonical PackageBase、事前観測済みexact recipe OID、cgit exact-idの
raw `.SRCINFO` bytes。canonical AUR URLの検証は既存`AurReviewedSourceReviewIdentity`を再利用する。
RPCのPackageBase観測、recipe HEAD観測、cgit metadataは別の役割を維持する。
raw path、arbitrary URL/OID tuple、old cache HEAD/origin、installed versionからownerを構成できない。

- fixed `/tmp`の安全なparentをdescriptorで保持し、128-bit random名を最大32回試行する。
  新規0700 rootの下に`moguet/<PackageBase>`を作る。preexisting collision、stale rootの再利用・採用・GCはない。
  pure `EnvironmentSnapshot`から既存XDG resolver / directory preparation / validated cache bridgeを使う。
  processのHOME/XDGは書き換えない。leaf==PackageBaseという既存pin契約を維持する。
- root / parent / checkout / `.git`のidentity、owner/modeを相関し、root配下は
  `openat2`のBENEATH / NO_SYMLINKS / NO_XDEVで開く。regular metadataのhardlink、gitfile、
  unsafe permission、replacement、alternate/commondirを拒否する。
  既存strict Git metadata validatorはacquisition専用checkpoint付き入口でdeadline / visit数を制限する。
  Git configのallowlistは既存trusted Git parserを共用し、検証後のconfig byte driftも拒否する。
- trusted Gitのcomplete envpとobserver由来のHTTPS-only profileを使用する。
  inherited Git routing/config/object/template environment、HOME/global/system Git config、askpass、
  terminal auth、SSH、credential helper、hooks、fsmonitorを取得authorityにしない。
  proxy / absolute custom CAだけは既存policyのrouting例外を維持する。
- init前はretained checkout FDから`.git`の不存在をno-followで検証する。型にかかわらず既存entryを
  `Initialization / UnsafeFilesystem`で拒否し、Git childを起動しない。不在以外の検査errorも停止する。
  init後は`.git` directoryの存在と既存strict metadata検査を必須にする。preexisting gitfileの内容を
  cleanup authorityにせず、自己作成root内のregular fileとしてのみ扱う。
- fresh `git init --template= --object-format=<expected format>`の後、canonical URLへexact expected OIDを
  fetchする。checkout、submodule update、auto-maintenance、shallow/partial/reference取得はしない。
  `FETCH_HEAD`も書かない。remoteがXからYへ進んでもexpected Xの取得だけを試みる。
  fetch非zeroは元のprocess outcome付きfailureであり、stderrから「Xが存在しない」と推定しない。
  reobserve / Y差替え / cache fallback / retryはない。
- 全object inventory、raw object type==commit、resolved complete OID、storage object formatを検証し、
  expected commit treeのregular `.SRCINFO` blobをtrial bytesと完全一致で比較する。
  generated `.SRCINFO`のsemantic authorityはS4に残る。recipe OIDをupstream built OIDへ転用しない。

取得全体のdeadlineは90秒。各bounded childは残時間を使い、200msのtermination grace、
process-group終了/reap、既存parent-death behaviorを維持する。init/fetchのstdout/stderrは
同一pipeの合計上限（4KiB / 64KiB）でcaptureし、object/metadata queryは個別のstdout上限を持ち
stderrを破棄する。`BoundedCapturedProcessResult::cancellation_signal`はchild outcomeから独立し、
親cancelを観測した後にchildがexit 0を返してもacquisition successを作らない。
他のbounded callerのoutcome解釈は3Aでは変更しない。

verification budgetはfilesystem 32,768 entries / depth 64 / observed regular bytes合計256MiB、
Git objects 32,768件 / individual expanded size 32MiB / expanded size合計256MiB、
`.SRCINFO` 256KiB、local config 8KiB。object inventory streamは32,768×96 bytes以下。
これらはrecipe historyの取得検証budgetであり、S4のbuild/source budgetとは独立している。
**hard transport byte quota / disk quotaではない**。fetch中にこれを超えるpackが一時的に書かれる可能性があり、
超過を検証した結果はfailureとなる。OSのblocking filesystem syscallや同UID hostile writerに対する
hard realtime / sandbox保証も提供しない。

success ownerはmove-onlyで、証明するのはexpected identityのprivate workspaceへの取得・照合まで。
`checkout()`はborrowであり、そのcapabilityのcopyはworkspace寿命を延長しない。
callerはownerをfull review → separate explicit acceptance → exact pin → S3 final recipe reproofまで保持する。
既存full review / R CAS / pin / S3のfactoryを通り、raw acquisitionからreview/R/S3/S4/Pをmintしない。
S3成功後はsource bytesがS3へ移っているため、acquisitionをcleanupしてからbuildへ進める。

`cleanup()`は5秒の走査deadlineと上記filesystem budgetで、全treeをpreflightしてから
ancestor/leaf identityを再照合し、自己作成rootだけを削除する。primary failureとcleanup consequenceは別fieldであり、
abort cleanupが失敗した場合は`abandoned_root`を診断用に保持する。explicit cleanupのfailureはsuccessへ丸めず、
同じownerで再試行しない。noexcept destructorは未実施cleanupのbackstopで、callerは結果を必要とする経路で
必ずexplicit cleanupを使う。SIGKILL/power loss後のdestructor実行、同UIDによるmkdir/open間の置換や
最後のidentity check/unlink間の原子的なcompare-and-unlinkは保証しない。残存rootは次invocationで採用しない。
同PackageBaseの各ownerは別root/repo/refs/indexを持ち、old cache leaseやPackageBase-wide lockを取得しない。

`test-invocation-owned-recipe-acquisition`はreal typed bootstrap observer、isolated installed DB、
exact-id metadata observation seam、offline Git transport substitutionを使う。productionのcanonical HTTPS
argv/envpを検査した後、test binaryだけで取得先をlocal fixtureへ置換する。実HTTPS/AUR server policyの
検証ではない。SHA-1/SHA-256、remote advance、actual full review/pin/S3、two-owner isolation、
process/cancel/resource/config/filesystem/cleanup failureを覆う。通常routeのactivationは下記の別fixtureで検証する。

## Bootstrap integration / old cache decoupling (#564 Slice 3B)

runnerの既存decision順序、Yes前 / Yes後 / source実行前のP/I/R・exact recipe・metadata・source shape再検証を維持する。
No/default-No/cancel/EOFでは取得workspaceもGit acquisitionも作らない。source実行前の最後の再検証後にremoteが
XからYへ進んでも、取得・full review・pinはtrialのXだけを使う。X取得失敗は停止し、Yへの再観測・差替えを行わない。

migrationの`source_build`はold `<cache>/moguet/<PackageBase>`を開く前に3A factoryへ分岐する。
old contents/HEAD/refs/origin/configをeligibility・review・recipe inputに使わず、fetch/reset/clean/checkout/
remove/reclone/config rewriteも行わない。clean cacheだけを旧経路へ戻す分岐はない。
共有XDG cache root activationとprivate artifact root capabilityの既存preflightは別責務として維持する。
通常valid provenance更新、non-devel、legacy/compatibilityのpersistent checkout契約は変更しない。

取得したexact identityを既存`BootstrapFullReview`へ渡す。migration Yesと別のexplicit review acceptance、
既存R publication/CAS、trial `.SRCINFO`/pin OID相関を経て、move-only acquisition ownerを
`PreparedReviewedDevelSourceBuildExecution`のstateへ移す。borrowed pathだけでは寿命を延ばさない。
既存S3 producerがpinを消費しfinal reproofを終えた後、取得ownerを明示cleanupしてからS4へ進む。
S3失敗でも取得cleanupを実行する。S4が観測するupstream Git OIDはrecipe OIDとは別identityのままである。

取得failureは`BootstrapRecipeAcquisitionError`でgeneric exception boundaryを越え、runner / operation resultの
`recipe_acquisition_failure`へstage/reason/process/errno/boundary/cleanup/residueを保持する。
review失敗にcleanup失敗が伴う場合は元のreview/confirmation exceptionをprimaryとして保つ。
S3後cleanup失敗は`RecipeCleanupFailure`でS4前に停止し、S3 failureがあればそのprimaryも残す。
cleanupを理由にRをrollbackせず、retry/automatic repair/old-cache fallbackを行わない。

acquisitionの親signal cancellationは元の`cancellation_signal`とchild outcomeを保持し、child exit 0でも
runnerのCancelled → operation cancellationへ投影する。q/EOFの`ConfirmationCancelled`は生成しない。
accepted work itemのfirst failure/cancelはnonzero、completed prefixを保持し、suffixはNotAttemptedとなる。

`test-devel-tracking-bootstrap`はold cacheとは独立して作ったauthoritative recipeとexact-id metadata seamを使う。
actual Git取得、production full reviewと別Yes、pin/S3、actual makepkg/archive、既存S5 transport/installed observation
fixtureからS6 Completeまでを通す。old cacheの全entry/regular bytes/HEAD/refs/configを前後照合し、old pathへのGit
呼出0を確認する。dirty tracked PKGBUILD、untracked/ignored residue、wrong HEAD/origin/config、patch/config collision、
remote advance、取得failure/cancel、review stop、S3/cleanup failureを対象とする。
同remoteの再assessmentと2回目ordinary updateではUpToDate・取得/build/install追加0を確認する。
このdeterministic evidenceを実AUR通信・host package DB installやSlice 6の全closureへ読み替えない。

## Ordinary split activation（Issue #564 Slice 5）

- Dはexact recipeの全declared children、I_dbはtrusted installed DBの同base children、IはDとのintersection。
  staleなI_db−Dは黙って捨てずtrialを拒否する。Tはexact RPC entryと既存request/update eligibility/required attribution。
- 同じbaseのMissing candidatesは1 trialを共有する。D/I_db/Missing候補のchild observationsを別保持する。
  最終Tは既存required targetsが所有し、同baseの独立したVersion/GitRevision更新候補も含められる。
  shared migrationをdeclineした場合、Missing候補は従来skip、通常更新候補はNotAttemptedとしてnonzeroを保持する。
  entryのない未installed siblingをTへ入れず、invalid/corrupt/futureをbootstrapへ昇格しない。
- 1 migration decision → 1 acquisition/full review/S3 → 必要な1 closure review/SourceReady →
  1 common S4でBを証明 → Tだけの1 transaction → selected child別S5/S6とする。
- requestのordinary_devel_package_baseは既存SkipIndependentTarget policyのpreparationから付与・再照合する。
  split execution activationはordinary updateと既存typed bootstrapに限定する。new explicit bootstrap、
  standalone upgrade-aur/upgrade-allのsplit execution拡張、dry-run再設計、#554を追加しない。
- required dependency/provider/child blockerとexact self-root exemptionを維持する。shared baseだけでcross-root relationを免除しない。
- 1 work itemがN selected child結果とshared execution ownerを保持する。install成功＋S5 incomplete/S6 partialはlosslessなnonzero。
  先行group完了→後続groupのformal cancel→suffix NotAttemptedを保持し、rollbackしない。

`test-devel-tracking-bootstrap.py --split`がpartial/both installed、same-remote steady state、
nonzero transaction、partial binding/publication、#545 cancellationをproduction-connected fixtureで確認する。
