# Moguet v2.7.0

This tracked file is the source of truth for release bodies. The English and
Japanese sections for each release describe the same scope.

## English

Moguet v2.7.0 adds authoritative Git revision tracking for a limited set of
installed devel packages. It ties update assessment to the source that Moguet
actually built and installed, and makes unavailable package metadata explicit
instead of treating it as confirmed absence.

### Authoritative devel Git tracking

- Supported devel packages can be assessed for upstream Git changes even when
  their package version has not increased. Normal AUR updates, registered AUR
  updates, `-Qua`, and the corresponding dry-run routes share this assessment.
- A newer AUR RPC package Version retains precedence without a Git query.
  For the same or an older RPC Version, Git assessment can refine the result
  only after validating the provenance tip, fresh installed binding, and
  current reviewed recipe/source identity. Local state is checked again after
  a successful remote observation.
- Matching remote and actually built Git OIDs mean `UpToDate`; a differing OID
  produces `UpdateAvailable` with a Git-revision basis. Git OID equality does
  not imply semantic-version ordering, ancestry, or a package-version increase.
  `-Qua` and plan output distinguish Git changes from version changes.
- Missing or invalid local proof remains `RequiresCheck`; a remote observation
  failure remains `Unknown`. Neither authorizes an automatic build. Registered
  AUR `OnlyIfUpdated` evaluates this state before a version-only shortcut;
  its `RequiresCheck` rebuild confirmation defaults to No and does not replace
  source review.

### Supported scope and migration

- The initial authoritative route requires an explicitly reviewed and pinned
  AUR recipe, no observed editor overlay, and exactly one floating,
  architecture-independent HTTPS Git source using default HEAD or an exact
  branch. Additional sources must be reviewed, tracked regular local files.
- It supports a non-split package with exactly one package name, selected
  child, produced artifact, and actually installed child. Extra debug-package
  output is outside this subset; Moguet does not silently override makepkg's
  debug policy. Other VCS implementations/transports, full split provenance,
  arbitrary PKGBUILD shell proofs, and external build-history adoption are
  outside this release's authoritative tracking scope.
- Provenance uses schema v1 in a separate XDG state namespace from reviewed
  source state. Existing valid records still require current-state
  revalidation. Future schemas and corrupt or unsafe history fail closed;
  the updater does not repair records, adopt history, or create a missing
  baseline from cache, installed metadata, or another helper's records.
- A first baseline requires an explicitly reviewed supported build, an actual
  installation, and successful provenance publication. `--noconfirm` and
  non-interactive use do not supply review authority. An external reinstall
  can invalidate historical provenance even at the same package version.

### Installation and provenance safety

- Reviewed recipe identity, actual upstream source revision, built artifact,
  installed binding, and persistent provenance remain distinct. The build
  uses invocation-owned recipe and build directories, evaluates source
  metadata in that context, and proves the actual source and artifact instead
  of treating a planning-time remote OID as the built revision.
- Publication requires an actual selected-artifact Install/Upgrade receipt
  and a fresh installed binding after the transaction; `pacman -U` exit 0
  alone is insufficient. A `--needed` skip does not manufacture a new proof
  or publish a new baseline. Once authoritative execution starts, it does not
  fall back to a legacy build route.
- Installation, proof, publication, and cleanup outcomes remain separate.
  Installation success followed by failed or uncertain publication, or a
  cleanup failure, is retained as a non-zero partial outcome. It does not
  erase the successful installation or claim a rollback; later work remains
  unattempted.
- Dry-run uses the same read-only assessment without checkout, PKGBUILD
  evaluation, build, installation, or publication. Repository-only `-Syu`,
  standalone `plan`, and `deps` do not acquire a new devel Git query or
  execution side effect.

### Metadata failure handling

- Auto `-Si` falls back to AUR only on confirmed repository `NotFound`.
  Unavailable metadata is reported as failure; the affected target is not
  sent to AUR or included in later pacman operands. Independent targets can
  continue through their existing routes.
- `revert` checks each target's repository metadata before deleting its saved
  source-build preference. Failed targets retain their preferences and are
  excluded from the grouped binary reinstall. Independent successful targets
  continue, with failures reflected in the final non-zero result.

### Diagnostics and presentation

- Terminal-facing opaque text in plans, runtime diagnostics, and reviewed
  source output follows a shared UTF-8 policy. Valid ordinary Unicode is
  preserved; invalid bytes, control characters, backslashes, line/paragraph
  separators, bidirectional controls, and U+FEFF are rendered as visible
  uppercase `\xHH` bytes. Display escaping does not change package identity,
  path operations, dependency decisions, subprocess arguments, or exit status.
- AUR info displays an unavailable installed state with a warning instead of
  asserting `no`. Auto search adds `[installed]` only from a successfully read
  foreign inventory; unavailable inventory produces a warning and omits the
  annotation. These optional-display failures do not change the underlying
  search/info success policy. AUR-only search avoids installed/repository
  metadata queries.
- Reviewed-source generation status uses clearer user-facing wording.

### Developer, build, and validation changes

- CMake policy choices are explicit while the minimum remains 3.18:
  CMP0200 uses NEW where available, and CMP0156/CMP0181 retain OLD semantics.
  This preserves the existing external linker-flag and library-ordering
  contracts without a blanket policy-version increase.
- The installed acceptance fixture now has a host compile/link gate included
  in repository validation. It catches declaration drift before container
  acceptance while retaining `EXCLUDE_FROM_ALL`; fixture runtime and actual
  transaction/publication acceptance remain owned by the container lanes.
- `.editorconfig` and `.gitattributes` establish editor and line-ending
  conventions without changing production behavior.

## 日本語

Moguet v2.7.0は、限定したinstalled devel packageにauthoritativeなGit revision
trackingを追加します。Moguetが実際にbuild/installしたsourceへupdate assessmentを
束縛し、package metadataの取得不能を確認済みの不在として扱わず、明示します。

### Authoritative devel Git tracking

- 対応devel packageでは、package versionが増加していなくてもupstream Gitの変更を
  評価できます。normal AUR update、registered AUR update、`-Qua`、対応するdry-runは
  このassessmentを共有します。
- AUR RPC package Versionがnewerなら、Git queryを行わず通常のVersion authorityを
  優先します。same/olderの場合だけ、provenance tip、fresh installed binding、current
  reviewed recipe/source identityの一致を確認してGit assessmentで補完します。
  remote observation成功後にはlocal stateを再確認します。
- remote OIDと実際にbuildしたGit OIDが一致すれば`UpToDate`、異なればGit revisionを
  根拠とする`UpdateAvailable`です。Git OIDの一致・差分をsemantic versionの大小、
  ancestry、package version増加へ読み替えません。`-Qua`とplan出力もGit差分と
  version変更を区別します。
- local proof不足・不正は`RequiresCheck`、remote observation failureは`Unknown`として
  保持し、どちらもautomatic buildを許可しません。registered AUR `OnlyIfUpdated`は
  version-only shortcutより先にこの状態を評価します。`RequiresCheck`のrebuild確認は
  default-Noであり、source reviewを代替しません。

### 対応範囲とmigration

- 初期authoritative routeは、明示的にreview/pinしたAUR recipe、observed editor overlayなし、
  architecture-independentなfloating HTTPS Git source 1件を要求します。selectorはdefault
  HEADまたはexact branchだけです。追加sourceはreviewed・trackedのregular local fileに限ります。
- non-splitで、pkgname、selected child、produced artifact、actually installed childが
  それぞれ1件のpackageに対応します。追加debug package outputは対象外であり、Moguetは
  makepkgのdebug policyを暗黙に上書きしません。他VCS/transport、full split provenance、
  arbitrary PKGBUILD shell proof、external build history adoptionは今回のauthoritative
  tracking scopeに含みません。
- provenanceはreviewed-source stateと別のXDG state namespaceにschema v1で保存します。
  既存のvalid recordにもcurrent stateの再検証が必要です。future schema、corrupt/unsafe
  historyはfail closedとし、updaterはrecord修復、history adoption、cache・installed metadata・
  他helperのrecordからのmissing baseline自動生成を行いません。
- 初回baselineには、明示的にreviewした対応build、実install、provenance publication成功が
  必要です。`--noconfirm`やnon-interactive実行からreview authorityは作りません。
  external reinstallは同じpackage versionでもhistorical provenanceを失効させることがあります。

### Installとprovenanceの安全境界

- reviewed recipe identity、actual upstream source revision、built artifact、installed binding、
  persistent provenanceを別々に保持します。buildはinvocation-ownedなrecipe/build directoryを
  使い、同じcontextでsource metadataを評価してactual source/artifactを証明します。
  planning時のremote OIDをbuilt revisionへ流用しません。
- publicationには、selected artifactのactual Install/Upgrade receiptとtransaction後のfresh
  installed bindingが必要です。`pacman -U`のexit 0だけでは不十分です。`--needed` skipから
  新proofやbaseline publicationを捏造しません。authoritative execution開始後はlegacy build
  routeへfallbackしません。
- install、proof、publication、cleanupの結果を別に保持します。install成功後のpublication
  failure・outcome unknownやcleanup failureは、non-zeroのpartial outcomeです。成功したinstallを
  消したりrollbackを主張したりせず、後続workは未実行として保持します。
- dry-runは同じread-only assessmentを使い、checkout、PKGBUILD評価、build、install、publicationを
  行いません。repository-only `-Syu`、standalone `plan`、`deps`へ新しいdevel Git queryやexecutionの
  副作用を追加しません。

### Metadata failureの扱い

- Auto `-Si`はrepositoryでconfirmed `NotFound`の場合だけAURへfallbackします。
  metadata取得不能はfailureとして報告し、対象をAURへ送らず、後段pacman operandにも含めません。
  独立targetは既存routeで続行できます。
- `revert`は各targetのrepository metadataを確認してからsaved source-build preferenceを
  削除します。失敗targetのpreferenceを保持し、grouped binary reinstallから除外します。
  独立した成功targetは続行し、失敗は最終non-zero resultへ反映します。

### 診断と表示

- plan、runtime diagnostic、reviewed-source出力のterminal-facingなopaque textで、共通の
  UTF-8 policyを使います。通常のvalid Unicodeを維持し、invalid byte、control character、
  backslash、line/paragraph separator、bidi control、U+FEFFをuppercase `\xHH` byteとして
  可視化します。表示escapeをpackage identity、path操作、dependency判断、subprocess引数、
  exit statusのauthorityへ戻しません。
- AUR infoはinstalled stateの取得不能を`no`と断定せず、取得不能表示とwarningを出します。
  Auto searchの`[installed]`は成功したforeign inventoryだけを根拠とし、取得不能時はwarningと
  ともにannotationを省略します。これらoptional表示の失敗はsearch/info本体の成功規則を
  変更しません。AUR-only searchはinstalled/repository metadataをqueryしません。
- reviewed-source generation statusの利用者向けwordingを明確にします。

### 開発・build・validationの変更

- CMake minimum 3.18を維持し、利用可能な場合にCMP0200=NEW、CMP0156/CMP0181=OLDを
  明示します。policy versionの一括引き上げを行わず、external linker flagsとlibrary orderingの
  既存契約を保持します。
- installed acceptance fixtureのhost compile/link gateをrepository validationへ追加します。
  container acceptanceより前にdeclaration driftを検出し、`EXCLUDE_FROM_ALL`を維持します。
  fixture runtimeとactual transaction/publication acceptanceのownerはcontainer laneのままです。
- `.editorconfig`と`.gitattributes`でeditor・改行規約を整備し、production behaviorは変更しません。

# Moguet v2.6.0

This tracked file is the source of truth for release bodies. The English and
Japanese sections for each release describe the same scope.

## English

Moguet v2.6.0 includes a behavior-changing compatibility correction for the
exact target-less `moguet -Syu` command. It now follows ordinary AUR-helper
expectations while keeping Moguet's saved source-build preferences behind the
explicit source-aware `upgrade*` workflows.

### Ordinary AUR-helper `-Syu`

- Before v2.6.0, exact target-less `moguet -Syu` performed only the official
  repository system update.
- Starting with v2.6.0, it performs the official repository system update and,
  only after success, obtains a fresh installed-foreign/AUR inventory and runs
  the normal installed-AUR update.
- The combined meaning applies only to the exact canonical target-less token.
  Alternate or separated modifiers and target-bearing forms retain their
  existing routing and do not start an installed-AUR sweep.
- `--needed` is initially the only supported pacman semantic option for Auto
  combined `-Syu` and applies only to the repository transaction. Other
  pacman semantic options or unsupported argument forms fail before repository
  mutation instead of silently running only the repository phase.
- `moguet -Syu --aur` remains unsupported. The AUR-only source-aware operation
  remains `moguet upgrade-aur`.

### Saved source-build preference boundary

- Actual and dry-run `moguet -Syu` do not snapshot, enumerate, or read the
  source-preference directory, attempt a PackageBase fallback preference, or
  apply a preference-derived environment. Valid, invalid, unreadable, or
  failing saved preferences do not affect the normal AUR phase.
- `moguet upgrade`, `moguet upgrade-aur`, and `moguet upgrade-all` remain
  explicit source-aware workflows and continue to check and apply saved
  source-build preferences strictly.
- `upgrade-aur` remains AUR-only in the sense that it does not run the
  repository system update; it does not ignore saved preferences.

### Independent devel `RequiresCheck` localization

- In ordinary exact target-less `moguet -Syu`, an independent devel target
  whose update status is `RequiresCheck` is skipped with a non-blocking
  warning. It does not prevent unrelated normal AUR updates from continuing,
  and the aggregate can exit successfully when no other hard failure exists.
- `RequiresCheck` remains an unverified package-state observation. It is not
  rewritten as `UpToDate` or `NoChange`, and it never authorizes an automatic
  update, including with `--noconfirm`.
- If another update candidate requires that target through a dependency,
  provider, or required-child relation, the affected root retains a typed
  blocker and the operation remains non-zero.
- `upgrade-aur` and `upgrade-all`, including dry-run, retain their strict
  whole-operation `RequiresCheck` blocker behavior.

### Sequential outcome and dry-run freshness

- Repository and AUR work are separate sequential transactions, not one atomic
  transaction. A repository failure leaves AUR work unattempted. A later AUR
  blocker, execution failure, or cleanup failure is non-zero partial
  completion and does not roll back the completed repository transaction.
- Cleanup state remains distinct from installation outcome, inconsistent
  aggregate results fail closed, and AUR `NoUpdates` alone does not make the
  whole repository-bearing operation `NoOp`.
- `moguet --dry-run -Syu` displays the repository intent separately from the
  normal-AUR intents. Its AUR assessment is based on the current installed
  state; actual execution does not reuse that observation and re-evaluates AUR
  authority after the repository upgrade succeeds.

### Migration

- To keep the former repository-only behavior, use `moguet -Syu --repo`. The
  semantic selector is not forwarded to pacman, compatible repository
  pass-through is preserved, and no AUR, preference, cache, Git, makepkg, or
  source-runner work is started.
- For Moguet's source-aware update behavior, use `moguet upgrade`,
  `moguet upgrade-aur`, or `moguet upgrade-all` according to the required
  repository/AUR scope.

## 日本語

Moguet v2.6.0では、exact target-less `moguet -Syu`にbehavior-changingな
compatibility correctionを行います。通常のAUR helperとして期待されるupdateへ合わせつつ、
Moguetのsaved source-build preferenceは明示的なsource-aware `upgrade*` workflowだけで
扱います。

### 通常のAUR helper `-Syu`

- v2.6.0より前のexact target-less `moguet -Syu`はofficial repository system updateだけを
  実行していました。
- v2.6.0以降はofficial repository system updateを実行し、その成功後にだけfreshな
  installed-foreign / AUR inventoryを取得してnormal installed-AUR updateを実行します。
- combined semanticsを持つのはexact canonical target-less tokenだけです。alternate /
  separated modifierやtarget-bearing formはexisting routingを維持し、installed-AUR sweepを
  開始しません。
- Auto combined `-Syu`でinitially対応するpacman semantic optionは`--needed`だけで、
  repository transactionだけへ適用します。他のpacman semantic optionやunsupported
  argument formはrepository mutation前にfail closedし、repository phaseだけを黙って実行
  しません。
- `moguet -Syu --aur`はunsupportedのままです。AUR-onlyのsource-aware operationは
  `moguet upgrade-aur`です。

### saved source-build preferenceの境界

- actual / dry-runの`moguet -Syu`はsource-preference directoryのsnapshot、列挙、read、
  PackageBase fallback preference、preference-derived environment適用を行いません。valid、
  invalid、unreadable、failureとなるsaved preferenceはnormal AUR phaseへ影響しません。
- `moguet upgrade`、`moguet upgrade-aur`、`moguet upgrade-all`は明示的なsource-aware
  workflowのままで、saved source-build preferenceを引き続きStrictに確認・適用します。
- `upgrade-aur`のAUR-onlyはrepository system updateを行わないことを意味し、saved
  preferenceを無視する意味ではありません。

### independent devel `RequiresCheck`の局所化

- 通常のexact target-less `moguet -Syu`では、update statusが`RequiresCheck`の
  independent devel targetをnon-blocking warning付きでskipします。unrelatedなnormal AUR
  updateは継続し、他にhard failureがなければaggregateはsuccess / exit 0になれます。
- `RequiresCheck`は未検証のpackage-state observationのままです。`UpToDate` / `NoChange`へ
  書き換えず、`--noconfirm`を含めautomatic updateのauthorityにはしません。
- 別のupdate candidateがdependency、provider、required-child relationとしてそのtargetを
  必要とする場合は、affected rootのtyped blockerを維持してoperationをnon-zeroにします。
- `upgrade-aur` / `upgrade-all`とそのdry-runは、strictなwhole-operation
  `RequiresCheck` blocker behaviorを維持します。

### sequential outcomeとdry-run freshness

- repository / AUR workは別のsequential transactionであり、単一atomic transactionでは
  ありません。repository failure時はAUR workを開始しません。後続AUR blocker、execution
  failure、cleanup failureはnon-zeroのpartial completionで、完了済みrepository transactionを
  rollbackしません。
- cleanup stateをinstall outcomeへflattenせず、inconsistent aggregateはfail closedです。
  AUR `NoUpdates`だけを根拠にrepository intentを持つoperation全体を`NoOp`と呼びません。
- `moguet --dry-run -Syu`はrepository intentとnormal-AUR intentを分けて表示します。AUR
  assessmentは現在インストールされている状態に基づき、actual executionはobservationを
  再利用せず、repository upgrade成功後にAUR authorityをfreshに再評価します。

### Migration

- 以前のrepository-only behaviorを維持する場合は`moguet -Syu --repo`を使います。semantic
  selectorはpacmanへforwardせず、compatibleなrepository pass-throughを維持し、AUR、
  preference、cache、Git、makepkg、source runnerへ到達しません。
- Moguetのsource-aware updateには、必要なrepository / AUR scopeに応じて
  `moguet upgrade`、`moguet upgrade-aur`、`moguet upgrade-all`を使います。

# Moguet v2.5.0

This tracked file is the source of truth for release bodies. The English and
Japanese sections for each release describe the same scope.

## English

Moguet v2.5.0 is a minor release focused on safer update classification and
cross-source diagnostics. Conventional VCS/devel AUR packages no longer fall
through to a false `UpToDate` result when authoritative revision tracking is
not yet available, and `upgrade-all` can provide a targeted diagnostic for
repo/AUR exact-version dependency locks after a failed system transaction.

This release also establishes substantial invocation-owned dependency-cleanup
authority without enabling unsafe production removal, and makes the repository
C++ formatting authority and changed-file workflow reproducible.

### Conservative VCS/devel AUR tracking

- Conventional devel-package suffixes such as `-git`, `-svn`, `-hg`, `-bzr`,
  `-cvs`, and `-darcs` are treated as candidate evidence rather than proof of
  an available update or an up-to-date state.
- When normal AUR version comparison says an exact AUR package is current but
  PackageBase or installed-child evidence identifies a conventional devel
  candidate, Moguet reports `RequiresCheck` instead of `UpToDate`.
- `moguet -Qua` exposes the package and reason without presenting the state as
  an ordinary version-update arrow.
- `upgrade-aur`, dry-run, and the AUR phase of `upgrade-all` keep
  `RequiresCheck` as a blocker before mutation. `--noconfirm` and non-TTY use
  do not turn that state into an automatic rebuild or implicit approval.
- A normal authoritative AUR version update remains an update candidate even
  when devel evidence is incomplete.
- v2.5.0 does not perform Git remote revision queries, checkout/fetch/reset,
  PKGBUILD evaluation, or build-provenance publication for this classification.
  Authoritative remote revision observation and installed-build provenance are
  deferred to follow-up work.

### Repo/AUR exact-version lock diagnostics

- `upgrade-all` now models cross-source version-lock observations separately
  from a generic system transaction failure.
- After a failed system phase, Moguet can correlate the repository target,
  installed blocker, dependency relation, and available AUR observations and
  present a dedicated diagnostic when the evidence supports that conclusion.
- Missing, incompatible, ambiguous, or failed AUR observations are not
  promoted to a compatible coordinated update.
- Moguet does not bypass pacman dependency checks, use `--nodeps`, choose a
  partial upgrade, or automatically remove/reinstall an AUR package to force
  the transaction through.
- The completed v2.5.0 scope is diagnostic and fail-closed; automatic
  cross-source transaction orchestration is not implied.

### Invocation-owned dependency cleanup foundation

- The cleanup design now explicitly preserves the rule
  `NewlyObserved != InvocationOwned`: a package merely appearing between
  snapshots is not sufficient proof that the current Moguet invocation owns
  its installation.
- A typed cleanup classifier, install-reason-aware local snapshot, BuildPlan
  and lifecycle correlation, transaction identity/outcome records, and
  machine-readable receipt infrastructure now form the cleanup foundation.
- A trusted ALPM receipt path exists for the selected repository-provider
  transaction boundary, while makepkg-internal sync dependency transactions
  and remaining policy/route authority are intentionally deferred.
- Pre-existing, explicit, shared, unverified, or otherwise unprovable packages
  are never promoted to cleanup candidates by guesswork.
- Broad orphan sweeps are not introduced, and `--noconfirm` is not cleanup
  approval.
- Public source-build `--rmdeps` remains unsupported and fail-closed in
  v2.5.0. Production preview, confirmation, mutation-time revalidation, and
  exact removal continue in follow-up work.

### Repository C++ formatting authority

- Add a repository-root `.clang-format` as the project-owned C/C++ formatting
  authority instead of relying on a HOME-level configuration or formatter
  fallback.
- Normalize the tracked C++ baseline under `source/` and `tests/` to that
  authority without intentional semantic changes.
- Add `scripts/format-changed-cpp.sh` for the normal changed-file workflow.
  It formats or checks only tracked `.cpp` / `.hpp` files changed from `HEAD`.
- Untracked C++ is not modified, zero candidates are a normal no-op, and Git
  changed-file detection failure fails closed instead of falling back to a
  repository-wide format operation.
- Developer and AI-agent documentation now share the same changed-file
  formatting contract before normal validation.

## 日本語

Moguet v2.5.0は、update classificationとcross-source diagnosticの安全性を強化する
minor releaseです。authoritativeなrevision trackingがまだ成立していないconventionalな
VCS/devel AUR packageをfalse `UpToDate`へ流さず、`upgrade-all`ではsystem transaction
failure後にrepo/AUR exact-version dependency lockを根拠付きで診断できるようにします。

さらに、invocation-owned dependency cleanupのauthorityを大きく前進させつつ、安全条件が
揃っていないproduction removalは有効化せず、repository C++ formatting authorityと
changed-file workflowも再現可能な形へ統一します。

### conservativeなVCS/devel AUR tracking

- `-git`、`-svn`、`-hg`、`-bzr`、`-cvs`、`-darcs`等のconventionalな
  devel package suffixは、update / up-to-dateの証明ではなくcandidate evidenceとして扱います。
- normal AUR version comparisonではcurrentでも、exact AUR packageかつPackageBase /
  installed childにconventionalなdevel evidenceがある場合、`UpToDate`ではなく
  `RequiresCheck`を返します。
- `moguet -Qua`はpackageとreasonを明示し、通常のversion update arrowへ偽装しません。
- `upgrade-aur`、dry-run、`upgrade-all`のAUR phaseでは`RequiresCheck`をmutation前の
  blockerとして維持します。`--noconfirm`やnon-TTYからautomatic rebuildやimplicit
  approvalへ昇格させません。
- normal AUR version comparisonでauthoritativeなupdateが確定している場合は、その既存
  update authorityを維持します。
- v2.5.0のclassificationだけを理由にGit remote query、checkout / fetch / reset、
  PKGBUILD evaluation、build provenance publicationは行いません。authoritativeなremote
  revision observationとinstalled build provenanceはfollow-upへ分離しています。

### repo/AUR exact-version lock diagnostic

- `upgrade-all`でcross-source version-lock observationをgenericなsystem transaction
  failureとは別のtyped stateとして扱います。
- system phase失敗後、repository target、installed blocker、dependency relation、
  AUR側observationを相関し、evidenceが成立する場合は専用diagnosticを表示します。
- AUR candidateがmissing / incompatible / ambiguousな場合やquery failure時に、
  coordinated update可能と推測しません。
- pacman dependency checkを無視せず、`--nodeps`、partial upgrade、AUR packageの
  automatic remove/reinstallで強制突破しません。
- v2.5.0で完成するscopeはdiagnostic / fail-closedまでであり、cross-source transactionの
  automatic orchestrationを意味しません。

### invocation-owned dependency cleanup foundation

- cleanup authorityでは`NewlyObserved != InvocationOwned`を明文化し、snapshot間で新しく
  見えたことだけをcurrent invocation所有の証明にしません。
- typed cleanup classifier、install reason付きlocal snapshot、BuildPlan / lifecycle
  correlation、transaction identity / outcome record、machine-readable receipt基盤を
  整備しました。
- selected repository provider transactionにはtrusted ALPM receipt pathを構築しましたが、
  makepkg内部sync dependency transactionと残るpolicy / route authorityはfollow-upへ
  明示的に延期しています。
- pre-existing、Explicit、shared、unverified、その他proof不足のpackageを推測でcleanup
  candidateへ昇格させません。
- broad orphan sweepを追加せず、`--noconfirm`をcleanup approvalとして扱いません。
- v2.5.0でもpublic source-build `--rmdeps`はunsupported / fail-closedのままです。
  production preview、confirmation、mutation-time revalidation、exact removalは
  follow-upで完成させます。

### repository C++ formatting authority

- repository rootへproject-owned `.clang-format`を追加し、HOME側の個人設定やformatter
  fallbackではなくrepository自身をC/C++ format authorityとします。
- `source/` / `tests/`のtracked C++ baselineを、意図的なsemantic changeなしで同じ
  authorityへ統一しました。
- normal workflow用に`scripts/format-changed-cpp.sh`を追加し、`HEAD`との差分にある
  tracked `.cpp` / `.hpp`だけをformat / checkします。
- untracked C++は変更せず、candidate 0件はnormal no-op、Git changed-file detection
  failure時はrepository-wide formattingへfallbackせずfail closedします。
- developer / AI agent documentationも、通常validation前に同じchanged-file formatting
  contractを使用します。

# Moguet v2.4.1

This tracked file is the source of truth for release bodies. The English and
Japanese sections for each release describe the same scope.

## English

Moguet v2.4.1 is a patch maintenance release that makes the strict TOML
user-configuration syntax easier to discover and provides a canonical
configuration example in both the repository and installed documentation.
It does not change the configuration schema, built-in defaults, or
fail-closed handling of malformed configuration.

### Canonical configuration sample

- Add `sample/config.toml` containing the current schema-version 1 defaults:
  - `review.pkgbuild = "prompt"`
  - `review.diff = "prompt"`
  - `build.mode = "normal"`
- Install the example as:
  `/usr/share/doc/moguet/examples/config.toml` under the standard Arch package
  layout.
- README documentation points users to both the repository sample and
  installed example while retaining `$XDG_CONFIG_HOME` / HOME fallback
  ownership.

### Clear TOML string syntax in help

- `moguet --help` now shows string enum values with TOML quotes:
  - `review.pkgbuild = "prompt"|"skip"`
  - `review.diff = "prompt"|"skip"`
  - `build.mode = "normal"|"rebuild"|"clean"`
- This prevents the previous help presentation from suggesting invalid bare
  TOML string values.
- The parser remains strict: malformed existing configuration still fails
  closed rather than being silently accepted or replaced with defaults.

## 日本語

Moguet v2.4.1は、strict TOML user configurationの正しいsyntaxを見つけやすくし、
repositoryとinstalled documentationの双方へcanonicalなconfiguration exampleを提供する
patch / maintenance releaseです。configuration schema、built-in default、malformed configに
対するfail-closed behaviorは変更しません。

### canonical configuration sample

- current schema version 1 / defaultを示す`sample/config.toml`を追加。
- standard Arch package layoutでは
  `/usr/share/doc/moguet/examples/config.toml`としてinstall。
- README英日からrepository sample / installed exampleを案内し、
  `$XDG_CONFIG_HOME`とHOME fallbackのownershipを維持。

### helpのTOML string syntax明確化

- `moguet --help`のstring enumをquoted表記へ変更:
  - `review.pkgbuild = "prompt"|"skip"`
  - `review.diff = "prompt"|"skip"`
  - `build.mode = "normal"|"rebuild"|"clean"`
- bare TOML stringを書けるように見える旧presentationを解消。
- parser自体は変更せず、invalid existing configは引き続きfail closed。

# Moguet v2.4.0

This tracked file is the source of truth for release bodies. The English and
Japanese sections for each release describe the same scope.

## English

Moguet v2.4.0 adds a reviewed-source workflow for AUR updates, an explicit
export parent for `moguet -G`, and clearer phase-level observations in
`upgrade-all`. It also completes the shared source-identity foundation and
repository build-infrastructure work: `source/` avoids the root `makepkg`
workspace collision, while CMake / CTest now own the C++ build and test graph.
These changes preserve pacman / makepkg / Git ownership, existing `make`
developer entrypoints, and fail-closed safety boundaries; reviewed state is
explicit acknowledgement, not automatic security certification.

### Reviewed AUR source revisions

- Moguet records the last AUR upstream revision explicitly accepted by the
  user as reviewed state at PackageBase scope. Fetched, reviewed, built, and
  installed revisions remain distinct, and reviewed state is independent of
  cache cleanup or recloning.
- For later updates, the review baseline is the previous reviewed commit to
  the exact fetched upstream commit, rather than the mutable cache `HEAD`.
  That pinned source revision is carried through review, safety preflight, and
  state publication.
- Review covers tracked repository content rather than a PKGBUILD-only
  extension whitelist. Added, changed, removed, and renamed content such as
  `.install` files, patches, service units, helper/config files, and other
  tracked files remains visible; binary or non-text changes are not treated as
  no change.
- Only explicit acceptance after successful review and preflight advances the
  reviewed state. Fetching, displaying a diff, building, installing,
  `--nodiff`, non-TTY operation, and `--noconfirm` do not manufacture review
  acceptance.
- When no reviewed state exists, including for existing users upgrading to
  this release, Moguet performs an initial full review from the empty tree to
  the exact target.
- If a recorded baseline object is unavailable, or current-schema state is
  invalid, corrupt, or source-mismatched, Moguet requires an explicit full
  rebaseline/rebind review rather than falling back to cache `HEAD`.
- Unsupported future state, unsafe history, store failures, and inconsistent
  observations fail closed instead of being treated as an empty diff or a
  reviewed result.
- This is a review-history and acknowledgement workflow for AUR source. It
  does not certify that an AUR package is safe or replace the user's review.

### Explicit export destination for `moguet -G`

- Existing `moguet -G <pkg>` behavior remains compatible: without an option,
  the PackageBase is exported under the command-start current directory.
- `moguet -G <pkg> --output-dir=DIR` selects an existing export parent for
  that invocation. The attached `--output-dir=DIR` form is the public syntax;
  relative paths use the command-start current directory, and the PackageBase
  remains the direct-child destination.
- Existing destinations are never replaced. Parent identity, symlink, path
  containment, and other export safety checks remain fail-closed, and the
  option is operation-local rather than a persistent or global setting.

### Clearer `upgrade-all` package-state observations

- Aggregate package-state semantics remain unchanged, while normal output now
  makes the system/source and AUR phase-level observations easier to
  distinguish alongside the aggregate result.
- A system/source `Changed` result with an AUR phase that is verified
  unchanged remains aggregate `Changed`, but is no longer as easy to read as
  an AUR update. Solver, update, and transaction semantics are unchanged.

### Common source identity foundation

- A shared typed identity model now relates package children, PackageBases,
  source kind, repository/AUR/local source distinctions, and source revisions
  without flattening those identities into package names.
- This foundation is reused by the reviewed-source workflow and later
  profile/patch work. It does not itself add a public profile or PKGBUILD
  patch command.

### Repository structure cleanup

- Moguet's production C++ tree moved from `src/` to `source/`, with repository
  references updated consistently.
- The structure-only refactor avoids collision with makepkg's root-level
  `src/` working directory and does not change runtime behavior or public CLI
  semantics.

### CMake / CTest build infrastructure

- CMake is now canonical for the project-owned C++ compile, link, build, and
  install graph; CTest owns C++ test registration and execution. The PKGBUILD
  consumes the canonical CMake build/install path, while repository-specific
  validation remains with the Make frontend and scripts.
- The Makefile remains a thin developer shortcut and validation frontend, so
  the normal `make` workflow is retained. CMake presets and compile-database
  support provide the developer-facing build tooling.
- The project CMake minimum remains 3.18; direct configure retains that
  compatibility, while the tracked `dev-debug` preset requires CMake 3.19+.
  Ninja is an optional CMake generator, not a mandatory package dependency,
  and this migration does not change the public CLI contract.

## 日本語

Moguet v2.4.0は、AUR updateのreview済みsource revision workflow、
`moguet -G`の明示的なexport親directory、`upgrade-all`のphase-level
observation明確化を追加します。さらに、common source identity foundationと
repository build infrastructureを整え、root `makepkg` work directoryとの衝突を
避ける`source/`構成と、C++ build/test graphのCMake / CTest authorityを完成させます。
pacman / makepkg / Gitのownership、既存の`make` developer entrypoint、fail-closed
safety boundaryは維持します。review済みstateは明示的なacknowledgementであり、
自動的なsecurity certificationではありません。

### AUR sourceのreview済みrevision

- 利用者が明示的に受理したAUR upstreamの最後のrevisionを、PackageBase単位の
  review済みstateとして保持します。fetched、reviewed、built、installedのrevisionは
  分離され、review済みstateはcache cleanupやrecloneにも依存しません。
- 後続updateでは、mutableなcache `HEAD`ではなく、前回review済みcommitから今回の
  exact fetched upstream commitまでをreview baselineとします。このpinned source
  revisionをreview、安全preflight、state publicationまで一貫して使います。
- review対象はPKGBUILDだけのextension whitelistではなく、tracked repository
  content全体です。`.install`、patch、service unit、helper/config fileなどの追加・
  変更・削除・renameとその他tracked fileを保持し、binary / non-textの変更を変更なし
  として扱いません。
- reviewとpreflightが成功した後の明示的なacceptanceだけがreview済みstateを進めます。
  fetch、diff表示、build、install、`--nodiff`、non-TTY operation、`--noconfirm`から
  review acceptanceを暗黙に生成しません。
- review済みstateが存在しない場合は、既存利用者のupgrade後も含め、empty treeから
  exact targetまでのinitial full reviewを行います。
- 記録済みbaseline objectが利用不能な場合や、current-schema stateがinvalid / corrupt /
  source-mismatchedな場合は、cache `HEAD`へfallbackせず明示的なfull rebaseline /
  rebind reviewを要求します。
- unsupported future state、unsafe history、store failure、inconsistent observationは、
  空のdiffやreview済み結果へ丸めずfail-closedで停止します。
- これはAUR sourceのreview history / acknowledgement workflowです。AUR packageの安全性を
  certifyするものでも、利用者自身のreviewを置き換えるものでもありません。

### `moguet -G`の明示的なexport先

- 既存の`moguet -G <pkg>` behaviorは互換性を維持します。optionを指定しなければ、
  PackageBaseをcommand-start current directoryの下へexportします。
- `moguet -G <pkg> --output-dir=DIR`で、そのinvocationだけの既存export parentを選べます。
  公開syntaxはattached formの`--output-dir=DIR`で、relative pathはcommand-start current
  directory基準、PackageBaseは常にdirect-childのdestinationです。
- existing destinationは置換しません。parent identity、symlink、path containmentなどの
  export safety checkはfail-closedを維持し、このoptionはpersistent / global settingではなく
  operation-localです。

### `upgrade-all`のpackage-state observation明確化

- aggregate package-state semanticsは変更せず、normal outputでsystem/sourceとAURの
  phase-level observationをaggregate resultと並べて区別しやすくします。
- system/sourceが`Changed`でAUR phaseが変更なしを確認済みの場合もaggregate `Changed`は
  維持しますが、AUR updateがあったようには読みにくくなります。solver、update、transaction
  semanticsは変更しません。

### common source identity基盤

- package child、PackageBase、source kind、repository / AUR / local sourceの区別、source
  revisionの関係を、package nameへflattenしないshared typed identity modelで共通化します。
- このfoundationはreviewed-source workflowと後続のprofile / patch workで再利用しますが、
  それ自体でpublic profile commandやPKGBUILD patch commandを追加するものではありません。

### repository structureの整理

- Moguetのproduction C++ treeを`src/`から`source/`へ移し、repository内の参照を一貫して更新しました。
- structure-onlyのrefactorにより、makepkgがroot levelで使う作業用`src/`との名前衝突を避けます。
  runtime behaviorやpublic CLI semanticsは変更しません。

### CMake / CTest build infrastructure

- project-ownedなC++ compile、link、build、install graphのcanonical authorityをCMakeへ移し、
  C++ testのregistration / executionをCTestが所有します。PKGBUILDはcanonicalなCMake
  build/install pathを利用し、repository固有validationはMake frontendとscriptsに残します。
- Makefileはthinなdeveloper shortcut / validation frontendとして維持し、通常の`make` workflow
  を残します。CMake presetとcompile database supportにより、developer向けbuild toolingも整えます。
- projectのCMake minimumは3.18のままで、direct configureは3.18 compatibilityを維持します。
  tracked `dev-debug` presetはCMake 3.19+を必要とします。NinjaはoptionalなCMake generatorであり、
  mandatory package dependencyではありません。このmigrationによるpublic CLI contractの変更もありません。

# Moguet v2.3.2

This tracked file is the source of truth for release bodies. The English and
Japanese sections for each release describe the same scope.

## English

Moguet v2.3.2 is a patch maintenance release that bounds file descriptor use
during large local-source workspace cleanup and corrects the presentation of
already-current split AUR targets in `upgrade-all`. The cleanup change preserves
the existing fail-closed safety invariants, and the presentation change remains
limited to authoritative typed `UpToDate` evidence.

### Bounded local-source cleanup

- Large local source trees could build successfully but fail during cleanup
  with `Too many open files` under a constrained `RLIMIT_NOFILE`.
- Cleanup now uses bounded descriptor traversal instead of retaining one file
  descriptor per descendant. Ownership, symlink, filesystem-boundary, and
  concurrent-replacement checks remain fail-closed; opaque filesystem
  generation identity continues to reject inode-reuse ABA replacement.
- Regression coverage exercises successful cleanup of a large tree under a
  constrained file-descriptor limit.

### Correct UpToDate split AUR presentation

- `upgrade-all` now projects a normal skip with authoritative typed
  `AurUpdateExecutionReason::UpToDate` evidence as `VerifiedUnchanged`, rather
  than `NotObserved` / `ObservationNotPrepared`.
- The requested split child and its `PackageBase` identity remain intact, but
  that identity difference alone no longer makes an authoritative `UpToDate`
  target attention-required.
- Genuine split `Updated`, `Unsupported`, `Incomplete`, failure, and other
  attention states remain visible, while standalone `upgrade-aur` typed-skip
  presentation is unchanged. This does not treat every skipped target as
  `VerifiedUnchanged`.

## 日本語

Moguet v2.3.2は、大規模local source workspace cleanupのfile descriptor使用量を
boundedにし、`upgrade-all`で既に最新のsplit AUR targetを正しく表示するpatch / maintenance
releaseです。cleanupの既存fail-closed safety invariantを維持し、presentation修正は
authoritativeなtyped `UpToDate` evidenceを持つ場合だけに限定します。

### local source cleanupのFD bounded化

- 大規模local source treeではbuildが成功しても、制約された`RLIMIT_NOFILE`の下でcleanupが
  `Too many open files`により失敗することがありました。
- cleanupはdescendantごとにfile descriptorを保持せず、boundedなdescriptor traversalを
  使います。ownership、symlink、filesystem boundary、concurrent replacementに対する
  fail-closed checkを維持し、opaqueなfilesystem generation identityによってinode再利用の
  ABA replacementも引き続き拒否します。
- 制約されたfile descriptor limitの下で大規模treeを正常にcleanupするregressionを
  coverageへ含めています。

### UpToDate split AUR presentationの修正

- `upgrade-all`では、authoritativeなtyped
  `AurUpdateExecutionReason::UpToDate` evidenceを持つnormal skipを、
  `NotObserved` / `ObservationNotPrepared`ではなく`VerifiedUnchanged`へprojectします。
- requested split childと`PackageBase`のidentityは維持し、そのidentity差だけを理由に
  authoritativeな`UpToDate` targetをattention-requiredにしません。
- 実際のsplit `Updated`、`Unsupported`、`Incomplete`、failure、その他attentionが必要な
  stateは引き続き表示し、standalone `upgrade-aur`のtyped skip presentationも変えません。
  すべてのskipped targetを`VerifiedUnchanged`にする変更ではありません。

# Moguet v2.3.1

## English

Moguet v2.3.1 is a maintenance release that makes trusted source checkouts and
installed package-relation observations more reliable, gives Moguet-owned
confirmation and cancellation one consistent contract, and establishes
Discussions-first community routing. It preserves the existing ownership of
Git, pacman, libalpm, and makepkg and keeps ambiguous or malformed states
fail-closed.

### Trusted Git fetch stability

- Managed source-repository fetches pass `--no-auto-maintenance` to Git so
  background automatic maintenance cannot race trusted-checkout revalidation
  after `git fetch` returns.
- Trusted cache, repository binding, descendant, and concurrent-replacement
  validation remain in place; the change does not relax checkout trust.

### Valid libalpm Provides projection

- Installed and repository `Provides` projection accepts valid legacy ALPM
  SONAME v1 metadata instead of misclassifying the whole relation inventory as
  malformed.
- Unversioned and ordinary package-version equality semantics remain intact.
  Missing, unsupported, or genuinely malformed relation data still blocks
  unsafe planning and mutation.

### Consistent confirmation and cancellation

- Moguet-owned `[Y/n]`, `[y/N]`, and `[y/n]` prompts share one grammar for
  affirmative, negative, cancel, empty-input, and EOF handling.
- Declined, cancelled, unavailable-input, input-failure, and actual operation
  failure outcomes remain distinct. Required declines and cancellations stay
  non-zero, while existing optional-skip behavior is preserved.

### Discussions-first community entry

- README and contribution guidance route questions, suspected bugs, feature
  ideas, and design or workflow proposals to GitHub Discussions first, while
  Issues remain maintainer-managed concrete work items.
- A dedicated Bug Issue Form remains available for well-observed reproducible
  bugs, and a low-barrier Bug Discussion Form supports earlier triage.
- Security-sensitive reports continue to use GitHub Private vulnerability
  reporting rather than public Discussions or Issues. English and Japanese
  guidance describe the same routing.

## 日本語

Moguet v2.3.1は、trusted source checkoutとinstalled package relationの観測を
より確実にし、Moguetが所有するconfirmation / cancellationを一貫した契約へ統一し、
Discussions-firstのcommunity routingを整備するmaintenance releaseです。Git、pacman、
libalpm、makepkgの既存ownershipを維持し、ambiguousまたはmalformedなstateでは
fail-closedを保ちます。

### trusted Git fetchの安定化

- managed source repository fetchではGitへ`--no-auto-maintenance`を渡し、`git fetch`
  return後のbackground automatic maintenanceとtrusted checkout再検証のraceを防ぎます。
- trusted cache、repository binding、descendant、concurrent replacementのvalidationは
  維持し、checkout trustを緩和しません。

### 有効なlibalpm Provides projection

- installed / repositoryの`Provides` projectionは、有効なlegacy ALPM SONAME v1
  metadataをrelation inventory全体のmalformedとして誤判定せず受理します。
- unversioned relationと通常package-version equalityのsemanticsを維持します。
  missing、unsupported、または実際にmalformedなrelation dataは、unsafeなplanや
  mutationへ進む前に従来どおり停止します。

### 一貫したconfirmationとcancellation

- Moguetが所有する`[Y/n]`、`[y/N]`、`[y/n]` promptは、yes、no、cancel、empty input、
  EOFを1つのgrammarで扱います。
- Declined、Cancelled、input unavailable、input failure、実際のoperation failureを
  区別します。requiredなdecline / cancelはnon-zeroを維持し、既存のoptional skip
  behaviorも変えません。

### Discussions-firstのcommunity入口

- READMEとcontribution guidanceは、質問、不具合かもしれない相談、機能要望、設計・
  workflow提案をまずGitHub Discussionsへ案内し、Issueはmaintainerが管理する具体的な
  work itemとして維持します。
- 観測・再現情報が十分なbugには専用Bug Issue Formを残し、早いtriageには投稿負担を
  抑えたBug Discussion Formを用意します。
- security-sensitiveな報告はpublic Discussion / Issueではなく、引き続きGitHub Private
  vulnerability reportingへ案内します。English / Japaneseで同じroutingを示します。

# Moguet v2.3.0

## English

Moguet v2.3.0 strengthens release validation, unifies public CLI diagnostics,
and makes source builds safer for repository PackageBases and per-package
customization. It preserves pacman / libalpm as transaction authority and
stops fail closed when Moguet cannot establish a safe pre-transaction result.

### Validation reliability and release-gate clarity

- Validation commands preserve producer failures instead of allowing later
  formatting or status work to mask them.
- Focused and incremental validation tracks dependencies and build signatures
  so stale binaries are not accepted as current evidence.
- The canonical release gate retains full host coverage without duplicating
  release-only checks, while host, offline/current-Arch container, and live
  provider / AUR / local lanes have explicit responsibilities.

### Coherent public CLI and transaction diagnostics

- The parser, help, man pages, and Bash / Zsh / Fish completions derive their
  public operation and option surface from a shared authority.
- Source-aware diagnostics project typed readiness and outcomes through a
  consistent hierarchy of summary, attention, and necessary detail.
- `Conflicts` and `Replaces` receive a typed pre-transaction assessment against
  installed and planned packages, provided components, and applicable version
  relations. Unknown, invalid, or incomplete observations fail closed rather
  than being treated as absence.
- A matching replacement is shown as potential impact that requires review.
  Moguet does not automatically remove packages, choose replacements, or
  resolve conflicts; pacman / libalpm remain transaction authority.

### Repository PackageBase artifact-selection safety

- Official-repository source builds require a strict repository package and
  PackageBase identity, build each PackageBase once, and validate every
  produced artifact.
- Only requested `Explicit` child packages are selected for installation;
  sibling and debug artifacts are not installed implicitly.
- Unknown, duplicate, or otherwise unsafe artifact identity fails closed. This
  allows multiple-artifact PackageBases such as OBS Studio to be handled
  without broadening install selection.

### Effective per-package build customization

- `build <pkg> [V=K...]` supports either a package-specific complete override
  or a local modification layered on the existing `makepkg.conf` baseline.
- A tested one-off assignment can be carried into a persistent `add-src`
  preference without rewriting the system-wide `makepkg.conf`.
- Assignments are applied after makepkg loads its configuration so they remain
  effective overrides.
- Whether variables such as `CFLAGS` or `CXXFLAGS` affect a result still
  depends on the PKGBUILD and upstream build system; this release does not
  recommend any optimization flag.

## 日本語

Moguet v2.3.0はrelease validationを強化し、public CLI diagnosticを一貫させ、
repository PackageBaseとpackage単位customizationのsource buildをより安全にする
releaseです。package transactionのauthorityはpacman / libalpmに維持し、安全な
transaction前判定を確立できない場合はfail-closedで停止します。

### validation reliabilityとrelease gateの明確化

- validation commandはproducer failureを保持し、後続のformattingやstatus処理に
  failureをmaskさせません。
- focused / incremental validationはdependencyとbuild signatureを追跡し、stale binaryを
  current evidenceとして受け入れません。
- canonical release gateはrelease-only checkを重複させずにfull host coverageを維持し、
  host、offline/current Arch container、live provider / AUR / local laneの責務を
  明確に分けます。

### 一貫したpublic CLIとtransaction前diagnostic

- parser、help、man page、Bash / Zsh / Fish completionは、public operation / option
  surfaceをshared authorityから導出します。
- source-aware diagnosticは、typed readinessとoutcomeをsummary、attention、必要なdetailの
  一貫した階層へ投影します。
- `Conflicts` / `Replaces`は、installed / planned package、provided component、適用可能な
  version relationに対してtransaction前にtyped評価します。Unknown、invalid、または
  incompleteなobservationはabsenceとして扱わずfail-closedにします。
- matching replacementはreviewが必要なpotential impactとして表示します。Moguetはpackageの
  remove、replacementの選択、conflict解決を自動化せず、pacman / libalpmをtransaction
  authorityとして維持します。

### repository PackageBaseのartifact選択安全性

- official repository source-buildではstrictなrepository package / PackageBase identityを
  必須とし、各PackageBaseを1回buildして、生成された全artifactを検証します。
- install対象に選ぶのはrequested `Explicit` child packageだけで、sibling / debug artifactを
  暗黙にinstallしません。
- unknown、duplicate、その他unsafeなartifact identityはfail-closedになります。これにより
  OBS Studioのようなmultiple-artifact PackageBaseでもinstall選択を広げずに扱えます。

### 実際に有効になるpackage単位build customization

- `build <pkg> [V=K...]`では、package単位のcomplete override、または既存
  `makepkg.conf` baselineを継承したlocal modificationを指定できます。
- one-offで確認したassignmentをpersistentな`add-src` preferenceへ移行でき、system-wideの
  `makepkg.conf`自体は書き換えません。
- assignmentはmakepkgがconfigを読み込んだ後に適用するため、effective overrideとして残ります。
- `CFLAGS` / `CXXFLAGS`等が実際の結果へ反映されるかはPKGBUILDとupstream build systemに
  依存し、このreleaseは特定のoptimization flagを推奨しません。

# Moguet v2.2.0

## English

Moguet v2.2.0 extends package planning and execution safety while preserving
its pacman-first and fail-closed boundaries. It makes ambiguity visible,
validates version constraints across source observations, and provides one
human-readable plan for supported no-mutation workflows.

### #388 — Ambiguous provider installed-state visibility

- Ambiguous provider candidates now show `[installed]` when a same-name
  installed package is observed.
- If installed-state lookup is unavailable or cannot be trusted, the candidate
  shows `[installed state unknown]` with a warning; an authoritative absence
  remains unmarked.
- Installed-state text is presentation-only. Provider selection policy,
  candidate order, numbering, explicit choice, and routing are unchanged; no
  provider is selected automatically.

### #351 — Version constraint satisfiability

- Consumer requirements and provider capabilities are kept separate and
  evaluated as typed results: `Unconstrained`, `Satisfied`, `Unsatisfied`,
  `Unknown`, `Invalid`, or `Conflicting`.
- Repository, AUR, provider, and local observations retain their source-aware
  identity so version compatibility is not inferred from a name alone.
- Mutation routes perform the same production preflight before clone, fetch,
  build, install, or transaction work. Constraints that cannot be proven safe
  fail closed instead of being guessed or silently routed elsewhere.

### #352 — Unified plan and global dry-run

- A unified human-readable plan describes the observations and decisions for
  supported Moguet-owned routes.
- Global `--dry-run` is supported for:
  `-S` install / system-update, `fetch`, remote build, local build, `upgrade`,
  `upgrade-aur`, and `upgrade-all`.
- Dry-run is no-mutation and unsupported routes fail closed. An actual
  execution revalidates the current state instead of reusing an observation as
  approval or execution capability.

## 日本語

Moguet v2.2.0は、pacman-firstかつfail-closedの境界を保ったまま、package
planningとexecution safetyを拡張するreleaseです。曖昧さを表示し、source
observationをまたぐversion constraintを検証し、supportedなno-mutation workflowを
1つのhuman-readable planで確認できるようにしました。

### #388 — ambiguous providerのinstalled state表示

- ambiguous provider candidateと同名のinstalled packageが観測された場合、候補に
  `[installed]`を表示します。
- installed-state lookupが利用できない、または信頼できない場合は、候補に
  `[installed state unknown]`とwarningを表示します。authoritativeに未installと判定できる
  場合はsuffixを付けません。
- installed-state表示はpresentation-onlyです。provider selection policy、候補順、番号、
  明示選択、routingは変更せず、providerを自動選択しません。

### #351 — version constraint satisfiability

- consumer requirementとprovider capabilityを分離し、`Unconstrained`、`Satisfied`、
  `Unsatisfied`、`Unknown`、`Invalid`、`Conflicting`のtyped resultとして評価します。
- repository、AUR、provider、localのobservationはsource-awareなidentityを保ち、nameだけ
  からversion compatibilityを推測しません。
- mutation routeではclone、fetch、build、install、transactionへ進む前に同じproduction
  preflightを実行します。安全を証明できないconstraintは推測や別経路への黙ったfallbackをせず、
  fail-closedになります。

### #352 — unified planとglobal dry-run

- supportedなMoguet-owned routeのobservationとdecisionを、統一human-readable planで表示します。
- global `--dry-run`は、次のrouteに対応します。
  - `-S` install / system-update
  - `fetch`
  - remote build
  - local build
  - `upgrade`
  - `upgrade-aur`
  - `upgrade-all`
- dry-runはno-mutationで、unsupported routeはfail-closedです。actual executionはobservationを
  approvalやexecution capabilityとして再利用せず、current stateから再validationします。

# Moguet v2.1.0

The following historical notes describe the published v2.1.0 release.

## English

Moguet v2.1.0 expands Moguet's source-aware AUR workflows while preserving its
pacman-first and fail-closed boundaries. It makes ambiguous choices explicit,
adds a local `PKGBUILD` entry point, and strengthens the validation lanes that
protect these workflows.

### Package workflows

- When an AUR dependency has several providers, an interactive TTY now lists
  the source-aware candidates by number and requires one explicit choice. No
  provider is selected by default; non-TTY, EOF, `--noconfirm`, and cancellation
  remain fail-closed.
- `moguet -S --select <query>` provides interactive package discovery across
  official repositories and AUR. It supports explicit package numbers,
  multiple selections, inclusive ranges, and displayed official groups without
  guessing a default choice.
- Selected repository roots are installed first through one exact pacman
  transaction. Only then do selected AUR roots enter the source-build workflow,
  so source-aware install routing stays visible and a later AUR failure does
  not hide an already completed repository transaction.
- `moguet build --local <directory> [V=K...]` builds an explicitly selected
  local PackageBase without treating a path-like remote package operand as a
  local source. It preserves the user-owned source tree and validates the local
  metadata before build and installation.
- Local `PKGBUILD` builds can resolve their AUR dependencies, build the
  required PackageBases, and install the validated dependency and local package
  artifacts through the established build/install boundaries.

### Validation and maintenance

- The Arch Docker offline lane validates a clean source snapshot without
  runtime network access. The live lane separately exercises real provider
  selection, AUR build/install, and local `PKGBUILD` end-to-end flows.
- The heavyweight integration-test binaries now use target-isolated object
  builds with dependency tracking and link firewalls, improving incremental
  validation without changing runtime behavior.
- Developers can opt into `ccache` for compilation and an optional linker such
  as mold through existing Make overrides; neither is a runtime or package
  requirement.
- TTY and locale authority handling is aligned across interactive selection,
  diagnostics, and gettext fallback, so non-interactive calls do not consume
  input intended for prompts.
- Obsolete production artifacts from the jpacker v1.16.0 transition have been
  removed. Historical migration guidance, fixtures, and license evidence remain
  available where they document the supported transition.

### Repositories

- Canonical GitHub repository: <https://github.com/seekerkrt/moguet>
- GitLab mirror: <https://gitlab.com/seekerkrt/moguet>

## 日本語

Moguet v2.1.0は、pacman-firstかつfail-closedの境界を保ったまま、source-awareな
AUR workflowを拡張するreleaseです。曖昧な選択を明示化し、local `PKGBUILD`の入口を追加し、
これらのworkflowを守るvalidation laneを強化しました。

### Package workflow

- AUR dependencyに複数providerがある場合、interactive TTYはsource-awareなcandidateを
  番号付きで表示し、1つの明示選択を求めます。default選択はなく、non-TTY、EOF、
  `--noconfirm`、cancelは引き続きfail-closedです。
- `moguet -S --select <query>`はofficial repositoryとAURをまたぐinteractive package
  discoveryを提供します。package番号、複数選択、inclusive range、表示済みofficial groupを
  明示的に選択でき、defaultを推測しません。
- 選択したrepository rootは、まず正確に1つのpacman transactionでinstallします。その後に
  選択したAUR rootだけがsource-build workflowへ入るため、source-awareなinstall routingは
  見通しを保ち、後続AUR failureが完了済みrepository transactionを隠すことはありません。
- `moguet build --local <directory> [V=K...]`は、pathのように見えるremote package operandを
  local sourceとして扱わず、明示したlocal PackageBaseをbuildします。user所有のsource treeを
  保持し、local metadataを検証してからbuild / installします。
- local `PKGBUILD` buildではAUR dependencyを解決し、必要なPackageBaseをbuildし、確立済みの
  build / install boundaryに従って検証済みのdependencyとlocal package artifactをinstallできます。

### Validationとmaintenance

- Arch Docker offline laneはruntime network accessなしでcleanなsource snapshotを検証します。
  live laneはreal provider selection、AUR build / install、local `PKGBUILD`のend-to-end flowを
  個別に実行します。
- 重量級integration test binaryは、targetごとに分離したobject build、dependency tracking、
  link firewallを使用するようになり、runtime behaviorを変えずにincremental validationを改善しました。
- developerは既存のMake overrideにより、compileへ`ccache`、linkerへmoldなどを任意で使用できます。
  どちらもruntime / package requirementではありません。
- TTYとlocaleのauthorityをinteractive selection、diagnostic、gettext fallbackで整合させ、
  non-interactive callがprompt用inputを消費しないようにしました。
- jpacker v1.16.0移行に由来するobsoleteなproduction artifactを削除しました。supported
  transitionを説明するhistorical migration guidance、fixture、license evidenceは必要な範囲で
  保持しています。

### Repository

- canonical GitHub repository: <https://github.com/seekerkrt/moguet>
- GitLab mirror: <https://gitlab.com/seekerkrt/moguet>
