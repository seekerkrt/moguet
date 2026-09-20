# Ambiguous provider selection contract

## 文書の位置づけ

この文書は、dependencyに複数provider候補が残る場合のinvocation-localな明示選択と、選択前のmutation禁止を定めるnormative production contractである。文書の規範上の正本は日本語本文である。

- Origin Issue: [#272](https://github.com/seekerkrt/moguet/issues/272)
- Related Issues: [#97](https://github.com/seekerkrt/moguet/issues/97)、[#217](https://github.com/seekerkrt/moguet/issues/217)、[#268](https://github.com/seekerkrt/moguet/issues/268)、[#271](https://github.com/seekerkrt/moguet/issues/271)、[#266](https://github.com/seekerkrt/moguet/issues/266)、[#267](https://github.com/seekerkrt/moguet/issues/267)
- Related Issues: #388、#351
- Related PRs: #341（#272 provider selection）、#277（typed provider origin）
- Update history: Issue #373で旧decision 13の本文から安定contractへ分離。Issue #388 Slice 1でinstalled stateのauthority/presentation契約を追加。Issue #351 Slice 5でconstraint preflight、partial-source、installed exact fallbackのproduction semanticsへ同期。
- Related upper decisions: [decision 1](../decisions.md#decision-1)、[decision 2](../decisions.md#decision-2)、[decision 4](../decisions.md#decision-4)、[decision 5](../decisions.md#decision-5)、[decision 6](../decisions.md#decision-6)、[decision 7](../decisions.md#decision-7)

## Contract本文（日本語normative source of truth）

### Resolution orderとcandidate identity

dependency `bar`の解決では、pacman-firstの既存順序を維持する。

1. official repositoryのexact packageをsource-aware libalpm adapterで確認する。
2. repo exact packageがあればrepo dependencyとし、repository identityとpacman / pacman.confのconfigured orderを保持する。先行sourceの`Present`は後続sourceの`SourceFailure`で消さない。
3. repo exact packageがなく、source failureが残る場合はtyped `Unknown`とし、AURへfallbackしない。全configured sourceが`Absent`の場合だけAUR exact packageを確認する。
4. AUR exact packageがあればAUR dependencyとする。AUR exact query failureはtyped `Unknown`とし、providerへfallbackしない。
5. exact packageがなければproviderを探す。repository provider setを先に確認し、そのsetがcompleteかつ空の場合だけAUR provider setを確認する。
6. provider candidate setがpartialならvalid observationをdiagnostic用に保持したtyped `Unknown(PartialSourceFailure)`とし、selection promptや別source fallbackへ進まない。
7. completeなprovider setが1件ならauto-resolveできる。
8. completeなprovider setが複数ならambiguousとし、候補順で先頭を選ばない。
9. completeなrepository / AUR provider setがともに0件の場合だけinstalled exact packageを確認する。
10. installed exactが`Present`なら`Installed` sourceとして解決し、`Absent`ならunresolved、local DBの`QueryFailure`ならtyped `Unknown`とする。

candidateはsource kind、package name、repository packageならrepository name、AUR packageならPackageBase、provided dependency name、取得可能なversion / constraint metadataを保持する。candidate順はconfigured repository orderと既存AUR aggregation orderを維持し、Moguet独自のscoreやdefault candidateを導入しない。

constraint evaluationはcandidateのfilter、sort、番号、default、recommend、auto-selectionを変更しない。`Unsatisfied` / `Unknown`はpresentation-only warningとしてchoice契約を維持し、`Invalid` / `Conflicting`はprompt開始前にfail-closedとする。constraintを理由に別sourceへfallbackしない。selection後にAUR provider metadataをrefreshした場合はcurrent matching capabilityを再取得・再評価し、selection前のprovided version / resultを再利用しない。source kind、package name、PackageBase、matching provided capability、provided capability versionの変化はidentity mismatchとしてfail-closedとする。

### Installed exact fallbackとsource identity

completeなexact / provider lookupの後に行うinstalled exact fallbackは、provider candidateへの`[installed]`注記とは別のresolution phaseである。

- installed exact candidateのsourceは`Installed`であり、official repositoryやAURへ分類しない。
- foreign installed packageも`deps`では`Installed dependencies`へ表示し、`Official repo dependencies`へ表示しない。
- local DBのconfirmed absenceだけを`Absent`とし、initialization / database / query failureをabsenceへ丸めない。
- query failureはfailure reasonを保持したtyped `Unknown`とし、read-only routeではwarning、mutation routeではmutation前blockへ接続する。
- installed stateはrepository source provenanceを証明しない。

### Interactive selection

複数providerの選択はinteractive TTYの番号入力だけで受け付ける。候補を番号付きで表示し、defaultを設けず、validな番号1件を明示入力として受理する。empty input、`q`、`quit`、`cancel`、EOFは取消とする。invalidまたはout-of-range inputは再入力を求める。

non-TTYではpromptを開始せず、stdin pipeをprovider selection inputとして暗黙使用しない。`--noconfirm`でも先頭候補やdefault候補を選ばず、ambiguous errorとしてfail closedする。cancel、EOF、non-TTY、`--noconfirm`はmutation可能なrouteをnon-zeroで停止させる。

### Interactive candidate presentation

Normalは番号、`repository/package`または`aur/package`、versionを主情報として表示する。
名前が`aur`のconfigured repositoryにはlocalized `[repository]`を添え、AUR sourceと区別する。
PackageBaseがpackage名と異なる場合はNormalでも補助表示し、同一なら繰り返さない。
provider capabilityは`[provides: ...]`へまとめ、version付きspecificationを保持する。
presenterはpromptのdependency contextを所有しないため、unversioned capabilityも一度表示する。
componentとspecificationの名前が異なる場合はcomponentも補助表示する。
installed stateの注記・warningとlookup lifetimeは従来の契約を維持する。
既存`PresentationDetail::Detailed`はfixed metadata fieldsを保持する。
`--details`のroute supportはCLI authorityに従い、候補表示のために拡張しない。
NormalのTTY配色はAUR `-Ss`のsource・package・version・installedのstyle primitiveを共有し、
候補出力がnon-TTYならANSIを追加しない。集合・順序・番号・入力・選択policyは表示modeに依存しない。

### Installed stateの表示契約（read-only）

provider selection前に、候補の`Installed`状態はread-only補助情報として表示する。installed stateは以下の挙動には影響を与えてはならない。

- candidate identity
- candidate ordering
- candidate numbering
- candidate filtering
- duplicate candidate削除
- 自動選択
- default choice
- 選択推奨
- empty input
- EOF
- cancel
- invalid input retry
- non-TTY
- `--noconfirm`
- invocation-local choice reuse
- incompatible provider identity guard
- BuildPlan policy
- provider routing

installed stateの表示は決定を誘導する情報であり、候補のsort、partition、filter、deduplicateは不可。

### Installed state authority

authorityは`pacman` contextのlibalpm local package database read-only照会を参照し、次の既存境界内でのみ成立する。

- human-readableな`pacman -Q`出力はparseしない。
- commandの終了ステータスをinstalled stateの真偽にそのまま圧縮しない。
- raw libalpm pointerをcandidate modelやpresentationへ公開しない。
- query結果はowned snapshotまたはpresentation-only observationへ変換する。
- query失敗を`NotInstalled`へ丸めない。

`Installed`は次の場合のみ意味を持つ。

- `candidate.package_name`と同名のpackageが、照会したlocal package databaseに存在する。

以下は証明しない（明示的に未証明とする）。

- installed packageがcandidateのrepository由来か否か
- installed packageがAUR由来か否か
- PKGBUILD、PackageBase、source commitなどのprovenance
- installed versionとcandidate versionの一致
- version constraint satisfiability
- provided dependency specificationの一致
- explicit / dependency install reason

repository candidateとAUR candidateが同package nameを持つ場合、両者が`Installed`と注記される可能性がある。これは両candidateが参照する同名packageがlocal DBに存在するという、name-onlyのobservationに限定する。

`NotInstalled`は次の場合だけとする。

- package metadata sessionの初期化に成功
- local DBのvalidity確認とcache preloadに成功
- 対象package nameのqueryがtyped `PackageNotFound`を返した

null、command failure、configuration failure、query failureを`NotInstalled`へ変換してはならない。

`Unknown`は、local DB authorityの取得、初期化、照会、検証が一貫して完了せず、`Installed`/`NotInstalled`を断言できない状態とする。

`Unknown`への投影は次を含む。

- configuration unavailable
- configuration malformed
- initialization failed
- local database unavailable
- local database invalid
- cache preload failure
- package query failed
- malformed returned metadata

failure reasonとdiagnosticは保持し、失わず保持する。

`Unknown`は`NotInstalled`の別名ではない。

### Strict failure

次の条件は`Unknown`として隠蔽せず、strict failureとして停止する。

- invalid candidate package name
- unexpected result variant
- moved-from misuse
- internal invariant failure
- expected metadata failureではないexception

`invalid candidate package name`ではprompt前に停止し、無効なnameをlibalpmへ渡してはならない。

### Availability policy

installed stateはpresentation補助であり、metadata取得の不足を理由に、従来成立していたproviderの明示選択を禁止しない。

- expected metadata failureはUnknown表示
- candidateを削除しない
- validな番号選択を継続可能
- query failureを`NotInstalled`へ丸めない
- identity / safety invariant failureのみstrict停止

### Model boundary

installed stateは`ProvidedDependency`へ直接保持しない。

- `ProvidedDependency`はcandidate identityとresolution metadataのauthorityである。
- default equality、vector equality、plan fixture、selection reuse近傍でpresentation stateが混入しうる。
- `same_provider_identity()`はinstalled stateから独立して維持する必要がある。

推奨構造（実装詳細は後続Sliceで確定）:

```text
raw ProvidedDependency candidates
    ├─ selection policy / identity authority
    └─ presentation-only candidate + installed-state projection
```

installed stateはidentity modelやBuildPlanへ流入しない境界を維持する。

### Installed state lifetime / cache

- provider choice/cancellation は既存通りinvocation-local
- installed state lookup はprovider selection phase-local
- lookupはinteractive candidate list表示時のみ開始
- non-TTY、`--noconfirm`、既存choice reuse、cancelled dependency reuse、candidate数1以下ではlookupを開始しない
- 一つのphaseでlocal DB sessionは原則1回だけopen
- 同じpackage nameはphase内で1回だけquery
- cache keyはpackage name
- name cacheをsource identity cacheとして使用しない
- mutationを跨いだinvocation-wide metadata cacheを作らない

### Presentation contract

将来のproduction presentationとして固定する。

- Installed: `[installed]`
- NotInstalled: suffixなし
- Unknown: `[installed state unknown]`

補足:

- `[installed]`とUnknown tagはlocalization対象
- fixed metadata labelsは従来どおりtranslation対象外
- Unknownを無表示にしてNotInstalledと区別不能にしない
- failure diagnosticはcandidate metadata fieldへ埋め込まず、別のwarningとして表示可能にする
- duplicate candidate / same package nameで同一diagnosticを不必要に繰り返さない
- candidate lineはmachine-readable formatとして宣言しない
- NotInstalledの既存lineはsuffix追加なしで維持する

### Current implementation status

- Issue #388でprovider candidateのinstalled-state annotationをproduction presentationへ接続済み。
- Issue #351 Slice 2〜4のtyped constraint model、source-aware repository/local adapter、AUR metadata projectionをproduction resolver edgeのauthorityとする。
- Issue #351 Slice 5ではinvocation-wide aggregation、prompt前の`Invalid` / `Conflicting` guard、partial-source `Unknown`、selected provider refresh、installed exact fallbackを同じBuildPlan / preflight ownerへ接続する。

### Ownership、plan、route

provider choiceはcanonical dependency identityごとにinvocation内だけで共有する。同じdependencyが複数edgeから要求された場合は同じchoiceを使い、incompatible choiceまたはconstraint conflictはmutation前に停止する。choiceをconfigやcacheへ永続化しない。

selected providerはdependency edgeへsource-aware identityとselection provenanceを保持する。selected AUR providerのPackageBaseはBuildPlanのdependency edge、build order、`fetch`対象へ渡す。selected repository providerはAUR repositoryとして取得・buildせず、exactな`repository/package`をofficial dependencyとしてpacman / makepkg側の経路へ渡す。

selected repository providerの`pacman -S --asdeps --needed`成功だけでは、実際にpackage stateが変更されたことを断言しない。transaction outcomeはsource work itemと分離して保持し、authoritativeな変更証拠を取得できないpackage stateは`Unknown`としてaggregateへ伝播する。

対象phaseの全provider choiceとstatic preflightが完了するまで、dependency transaction、Git checkout、makepkg、source-artifact install、pacman、sudoを開始しない。transaction failureはsource mutationへ進む根拠にならず、すでに完了した別phaseやpackage transactionをrollbackしたと報告しない。

`--noconfirm`はprovider selectionを自動化しない。root package discovery、split artifact selection、conflicts / replaces、complete version solverとは別責務である。

## Non-scope / implementationを固定しない範囲

- provider choiceのconfig / cache永続化。
- complete version constraint solver、conflicts / replaces solver、cross-source atomic transaction。
- root package selection、split package artifact selection、fuzzy finder、GUI / TUI。
- candidate model、session、callback、prompt表示の具体実装を恒久固定すること。

## Compatibility

provider順序、TTY / non-TTY、`--noconfirm`、cancel / EOF、selection-before-mutation、selected repository / AUR routeの要約は、[`compatibility.md`のdependency provider section](../compatibility.md#compat-ambiguous-provider)を参照する。
