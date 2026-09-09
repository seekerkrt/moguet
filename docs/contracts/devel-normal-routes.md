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
- 不完全なgrouping、別DB context、splitはpositiveを作らない。元のworld/inventory evidenceをquery resultへ保持する。
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
| RequiresCheck | independentはwarning/skip、required dependency/provider/childへ再entryならblock | whole-operation block/nonzero | default-Noの明示rebuild確認。declineはtyped Incomplete、acceptedでも別途normal reviewed pinが必要 |
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
no-overlay、one pkgname/artifact/installed child、one floating HTTPS Git、DefaultHead/Branch、architecture-independentを維持する。
needed/rmdeps/AsDeps/promotion等の非対応intentをS5 Defaultへ偽装しない。

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
