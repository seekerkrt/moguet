# Moguet 設計ポリシー / Design Policy

[日本語](#日本語) | [English](#english)

---

## 日本語

### 文書の位置づけ

この文書は、現在のMoguetへ適用する普遍的な上位設計原則とlicense / third-party complianceの上位原則の詳細な正本である。CLI挙動、provider選択、solverの利用、fallback、自動化、安全境界について新しい判断を行うときは、このポリシーを基準にする。

Issue別に増えるproduction contractの全文はこの文書へ追加せず、[docs/contracts/](contracts/README.md)の各安定contractを参照する。現在のcommand routingと利用者向けcompatibility summaryは[docs/COMPATIBILITY.md](COMPATIBILITY.md)を参照する。`DECISIONS.md`は上位原則の正本であり、個別contractの実装詳細を独立した正本として重複保持しない。

現在のproject名はMoguetである。Moguet v2.0.0はjpacker v1.16.0の実行基盤を継承するが、current identityはMoguetとし、旧名称はversion、migration、storage等の明示されたlegacy contextだけで使用する。

この文書は、現在の全CLI挙動を列挙する互換性仕様でも、未実装機能を実装済みとみなす保証でもない。個別仕様を追加・変更するときは、この上位ポリシーに沿って、対象となるbehaviorと検証範囲をcontract側へ明示する。

<a id="decision-1"></a>

### 1. 一貫性

同じ種類の状態、結果、失敗は、経路や内部実装が違っても同じ境界と規則で扱う。

* package absence、metadata query failure、invalid input、unsupported decision は意味が異なる。これらを単一の bool、empty result、または success へ flatten しない。
* 観測できなかった状態と、観測した結果として存在しない状態を区別する。判断できない状態を「対象なし」や「処理済み」に置き換えない。
* 同じ option や operation は、pacman-only、AUR、source-build など経路が違っても、対応可能な範囲で同じ意味を保つ。
* 経路ごとに同じ意味を安全に保てない場合は、黙って無視したり別の意味へ変換したりせず、未対応であることを示して実行前に停止する。

<a id="decision-2"></a>

### 2. 透明性

何を観測し、何を判断し、どの command を実行し、なぜ停止したかを、利用者が追える形にする。

* query や外部 command の failure を package absence、empty result、または正常終了として隠さない。
* partial completion が可能な処理では、成功した対象、失敗した対象、未実行の対象を区別し、diagnostic と exit status に反映する。
* 利用者に影響する主要な外部 command と副作用は、実行前に見える形にする。特に pacman / makepkg に渡す主要 option を隠さない。
* 安全境界で停止した場合は、拒否した判断や不足している情報を示し、利用者が次に確認すべき対象を分かるようにする。

<a id="decision-3"></a>

### 3. 既存操作と挙動の尊重

pacman、makepkg、git、および既存 Moguet CLI の操作、責務、挙動は、明確な理由と独立した設計判断なしに変更しない。

* package metadata の内部取得を libalpm などへ移行しても、利用者から見た operation、transaction owner、build owner、判断結果は可能な限り維持する。
* behavior change を refactor、metadata migration、内部 API の置換へ暗黙に混ぜない。変更する場合は、目的、互換性への影響、失敗時の扱い、検証方法を独立して説明する。
* wrapper として扱える operation は、元 command の操作感、引数の意味、主要な副作用、終了状態を可能な限りなぞる。
* この原則は完全互換の宣言ではない。現在未対応の command や edge case を保証するのではなく、差異を意図せず増やさないための判断基準である。

<a id="decision-4"></a>

### 4. 任せる部分は任せる

Moguetは、既存componentが所有するpackage-management機能の不完全な再実装を増やさない。各componentのauthoritativeな結果と既存の責務境界を利用し、その上で必要なorchestrationを行う。

* Arch 固有の package metadata、dependency、provider、conflict などは、対応可能な範囲で libalpm を authoritative source とする。
* system package transaction と検証済みsource artifactのinstall transactionは pacman へ任せる。
* source package artifact の build は makepkg へ任せる。
* source repository の取得と更新は git へ任せる。
* Moguet は、orchestration、source-build policy、execution order、安全境界、diagnostic、および user intent の保全を所有する。

重要: 現在採用している libalpm の scope は read-only package metadata に限る。Moguet は libalpm transaction を開始、準備、commit せず、system package transaction の owner は引き続き pacman である。metadata の authority を libalpm へ寄せることは、transaction ownership の移行を意味しない。

<a id="decision-5"></a>

### 5. ユーザーが自然に想像する意図

これは、利便性の好みではなく、自動化と安全境界を決める独立した中核原則である。

> Moguet は、command 名、引数、既存 tool の慣習から利用者が自然に想像する目的と結果を尊重する。内部実装の都合や過剰な自動化によって、意外な選択、副作用、fallback、挙動変更を持ち込まない。

user intent は、command 名、指定された target と option、元 tool の慣習、文書化済みの既存挙動から判断する。内部で実装しやすいことや、技術的に自動化できることだけを根拠に拡張しない。

* 最終的な判断基準は、その command を実行した利用者が、自然にどの対象、結果、副作用を期待するかである。
* `--noconfirm` は、対応する経路で確認を省略し非対話で処理するための option であり、「すべてを自動承認する」という意味ではない。未解決依存、provider 選択、conflict、replacement、削除、source selection などの曖昧さや危険を承認する根拠にしない。
* 複数 provider、conflict、replacement、削除、source selection など、利用者の意図が一意に決まらないものを、候補順や内部都合だけで勝手に選ばない。
* authoritative な判断材料を取得できない場合は、failure を absence に置き換えたり推測で補ったりして mutation へ進まず、安全に停止して理由を示す。
* 対象同士が独立し、安全境界を保ったまま処理できる場合は、失敗対象を隔離して残りを継続してよい。その場合は partial failure として、成功、失敗、未実行を区別して示す。
* 「驚きが少ないこと」と「元 command から自然に予想できること」を、compatibility と UX の一部として扱う。

<a id="decision-6"></a>

### 6. 責務境界

| Component | 所有する責務 |
| --- | --- |
| libalpm | 対応範囲内の authoritative な Arch package metadata と package relationships。現在は read-only metadata に限り、transaction は所有しない |
| pacman | system package transactionsと検証済みsource artifactのinstall transactions |
| makepkg | source package artifactsのbuild |
| git | source repository retrieval と update |
| Moguet | orchestration、source-build policy、execution order、safety boundaries、diagnostics、user intent の保全 |

Moguet が外部 component を呼び出すための順序、事前条件、停止条件、表示を設計することは orchestration の責務である。ただし、それを理由に各 component の solver、transaction、build、repository operation を独自実装へ置き換えない。

<a id="decision-7"></a>

### 7. 判断ルール

新しい自動化、fallback、solver 利用、または behavior change を検討するときは、最低限、次を確認する。

* 既存 command の意味を変えないか。
* command 名、引数、既存 tool の慣習から利用者が自然に予想する対象、結果、副作用と一致するか。
* authoritative component へ任せられる判断や処理を、Moguet 側で再実装していないか。
* failure と absence、および unsupported decision を区別できるか。
* 実行する command、判断理由、副作用、partial completion を説明できるか。
* 判断が曖昧な場合や authoritative な情報を取得できない場合に、mutation より前に安全に停止できるか。
* behavior change を独立した decision として説明し、既存挙動との差分を検証できるか。

これらを満たす説明や検証方法がない場合は、自動化を既定動作へ組み込まない。read-only の観測や plan と、build、install、remove、repository update などの mutation を分け、必要な判断材料を利用者へ示すことを優先する。

<a id="decision-8"></a>

### 8. Licenseとthird-party compliance

Moguet releaseとjpacker v1.15.0以降は`GPL-3.0-or-later`で提供する。jpacker v1.14.0以前のreleaseはMIT Licenseのまま維持し、過去のtag、release、permissionを書き換えない。

licenseとnoticeの監査では、同じprogramへ組み込まれるdirect linked / header-compiled componentと、command line・stdin/stdout・exit statusを介するexternal programを分離して扱う。libraryのvendor、static link、binary bundle、新規linked/compiled dependencyを追加する場合は、配布前にlicense、notice、Corresponding Sourceを再監査する。

version boundary、配布policy、component別の詳細は[docs/LICENSING.md](LICENSING.md)をsource of truthとする。

---

## English

### Status and authority

This document is the detailed canonical source for the high-level design policy applied to the current Moguet project. Decisions 1 through 7 are universal design principles, and decision 8 is the high-level license and third-party compliance principle. New decisions about CLI behavior, provider selection, solver use, fallback, automation, and safety boundaries must be evaluated against this policy.

Issue-specific production contracts are not duplicated here. Use the [contract index](contracts/README.md) for their Japanese normative source and [docs/COMPATIBILITY.md](COMPATIBILITY.md) for current routing and user-visible compatibility summaries. This document is the source of truth for the high-level principles.

The current project name is Moguet. Moguet v2.0.0 inherits the jpacker v1.16.0 execution base, but Moguet is the current identity; the former name is used only in explicit legacy contexts such as versions, migration, and storage.

This document is not a compatibility specification enumerating all current CLI behavior, and it does not claim that unimplemented features already exist. Specific behavior contracts must state their affected behavior and verification scope in the contract documents while following this policy.

<a id="decision-1-en"></a>

### 1. Consistency

States, results, and failures of the same kind must follow the same boundaries and rules even when they pass through different routes or internal implementations.

* Package absence, metadata query failure, invalid input, and an unsupported decision have different meanings. They must not be flattened into a single boolean, an empty result, or success.
* A state that could not be observed must remain distinct from a state observed to be absent. An undecidable state must not be rewritten as “no target” or “completed.”
* The same option or operation should retain the same meaning, where supported, across pacman-only, AUR, and source-build routes.
* If a route cannot safely preserve that meaning, Moguet must not silently ignore the option or translate it into a different meaning. It must report the unsupported case and stop before execution.

<a id="decision-2-en"></a>

### 2. Transparency

Users must be able to follow what was observed, what was decided, which command will run, and why processing stopped.

* A query or external-command failure must not be hidden as package absence, an empty result, or normal completion.
* When partial completion is possible, successful, failed, and unattempted targets must be distinguished and reflected in diagnostics and exit status.
* Major external commands and side effects that affect users must be visible before execution. In particular, important options passed to pacman or makepkg must not be hidden.
* When a safety boundary stops processing, diagnostics must identify the rejected decision or missing information so users know what to inspect next.

<a id="decision-3-en"></a>

### 3. Preserve existing commands and behavior

The operations, responsibilities, and behavior of pacman, makepkg, git, and the existing Moguet CLI must not change without a clear reason and a separate design decision.

* Moving internal package-metadata access to libalpm or another component should preserve the user-visible operation, transaction owner, build owner, and decision result wherever possible.
* A behavior change must not be hidden inside a refactor, metadata migration, or internal API replacement. Its purpose, compatibility impact, failure handling, and verification method must be explained independently.
* An operation that can remain a wrapper should follow the original command's interaction model, argument meaning, major side effects, and completion status wherever possible.
* This principle is not a declaration of complete compatibility. It does not guarantee commands or edge cases that are currently unsupported; it is a rule against introducing unintended differences.

<a id="decision-4-en"></a>

### 4. Delegate to authoritative components

Moguet must not accumulate incomplete reimplementations of package-management capabilities owned by existing components. It should use their authoritative results and established responsibility boundaries, then provide the necessary orchestration around them.

* Arch-specific package metadata, dependencies, providers, conflicts, and related information use libalpm as the authoritative source where supported.
* System package transactions and validated source-artifact installation transactions remain delegated to pacman.
* Source package artifact builds remain delegated to makepkg.
* Source repository retrieval and updates remain delegated to git.
* Moguet owns orchestration, source-build policy, execution order, safety boundaries, diagnostics, and preservation of user intent.

Important: the currently adopted libalpm scope is limited to read-only package metadata. Moguet does not initiate, prepare, or commit libalpm transactions; pacman remains the owner of system package transactions. Treating libalpm as the metadata authority does not transfer transaction ownership to libalpm.

<a id="decision-5-en"></a>

### 5. Natural user intent

This is an independent core principle for automation and safety boundaries, not merely a preference about convenience.

> Moguet respects the purpose and result that users would naturally expect from a command name, its arguments, and existing tool conventions. Internal implementation convenience or excessive automation must not introduce surprising selections, side effects, fallback, or behavior changes.

User intent is inferred from the command name, explicit targets and options, conventions of the original tool, and documented existing behavior. An implementation must not expand behavior merely because an action is technically automatable or internally convenient.

* The final criterion is which targets, results, and side effects a user running that command would naturally expect.
* `--noconfirm` suppresses confirmation and enables non-interactive handling only within a supported route; it does not mean “approve everything automatically.” It does not authorize ambiguity or risk involving unresolved dependencies, provider selection, conflicts, replacements, removals, or source selection.
* Multiple providers, conflicts, replacements, removals, source selection, and other decisions for which user intent is not unique must not be resolved arbitrarily from candidate order or implementation convenience.
* If authoritative decision inputs cannot be obtained, Moguet must not replace failure with absence or guess its way into mutation. It must stop safely and explain why.
* When targets are independent and can be processed without weakening safety boundaries, a failed target may be isolated while the remaining targets continue. The result must be reported as a partial failure that distinguishes success, failure, and unattempted work.
* “Low surprise” and behavior naturally predictable from the original command are part of compatibility and user experience.

<a id="decision-6-en"></a>

### 6. Responsibility boundary

| Component | Owned responsibility |
| --- | --- |
| libalpm | Authoritative Arch package metadata and package relationships where supported. Its current scope is read-only metadata; it does not own transactions |
| pacman | System package transactions and validated source-artifact installation transactions |
| makepkg | Source package artifact builds |
| git | Source repository retrieval and updates |
| Moguet | Orchestration, source-build policy, execution order, safety boundaries, diagnostics, and preservation of user intent |

Designing the order, preconditions, stop conditions, and presentation around calls to external components is part of Moguet orchestration. It is not a reason to replace each component's solver, transaction, build, or repository operations with a custom implementation.

<a id="decision-7-en"></a>

### 7. Decision rule

Before introducing new automation, fallback, solver use, or a behavior change, verify at least the following:

* Does it preserve the meaning of existing commands?
* Does it match the targets, results, and side effects users would naturally expect from the command name, its arguments, and existing tool conventions?
* Could an authoritative component own the decision or operation instead of Moguet reimplementing it?
* Can it distinguish failure, absence, and an unsupported decision?
* Can it explain the command to run, the reason for the decision, side effects, and any partial completion?
* Can it stop safely before mutation when a decision is ambiguous or authoritative information cannot be obtained?
* Can the behavior change be explained as an independent decision and verified against existing behavior?

If these questions cannot be answered with a clear explanation and verification method, the automation must not become default behavior. Prefer separating read-only observation and planning from mutations such as build, install, removal, and repository update, and expose the decision inputs users need.

<a id="decision-8-en"></a>

### 8. Licensing and third-party compliance

Moguet releases and jpacker v1.15.0 or later are distributed under `GPL-3.0-or-later`. jpacker releases through v1.14.0 remain under the MIT License; their historical tags, releases, and granted permissions are not rewritten.

License and notice audits distinguish direct linked or header-compiled components incorporated into the program from external programs communicating through command-line arguments, stdin/stdout, and exit status. Adding vendored libraries, static links, binary bundles, or a new linked/compiled dependency requires a new license, notice, and Corresponding Source audit before distribution.

[docs/LICENSING.md](LICENSING.md) is the source of truth for the version boundary, distribution policy, and component-level details.

---

## Issue別production contract index

decision 9〜15として旧`DECISIONS.md`に記載していた全文contractは、次の安定filenameへ移動した。旧decision番号は追跡用の互換識別子であり、新しいcontractのauthorityは各ファイルの日本語本文である。

| 旧decision | 現行contract | behavior / safety boundary |
| --- | --- | --- |
| 9 | [PackageBase build / required-child selection](contracts/packagebase-child-selection.md) | PackageBase build unitとrequired child install selectionの分離 |
| 10 | [separated source-build `--rmdeps`](contracts/source-build-rmdeps.md) | cleanup ownershipを証明できないsource-buildではfail closed、pacman-onlyでは消費してno-op |
| 11 | [XDG cache cutover safety](contracts/xdg-cache-safety.md) | cache filesystem identity、symlink、root escape、legacy cache非変更 |
| 12 | [source-build preference XDG authority](contracts/source-build-preference-xdg.md) | source preferenceのuser XDG authorityと安全なfilesystem操作 |
| 13 | [ambiguous provider selection](contracts/ambiguous-provider-selection.md) | invocation-localな明示provider選択とmutation前preflight |
| 14 | [root package selection](contracts/root-package-selection.md) | `-S --select`によるsource-aware root selectionとroute固定 |
| 15 | [local PKGBUILD](contracts/local-pkgbuild.md) | `build --local`のlocal root identity、metadata、source tree非破壊境界 |

### v3 foundation contract

Issue #355で、public profile / patch workflowより前に利用する[source-aware package identity contract](contracts/source-package-identity.md)を追加した。このcontractはpackage child、PackageBase、repository / AUR / local source、source location、source revision、package release、architectureを分離し、既存production modelからのread-only projectionだけを許可する。CLI、storage、profile、patch適用、source commit取得を有効化するdecisionではない。

### 上位原則とcontractの読み分け

decision 1〜7は全contractへ適用する普遍原則であり、decision 8はlicense / third-party complianceの上位原則である。個別contractはこれらの原則を特定のbehaviorやsafety boundaryへ適用したもので、実装module、type、capability plumbingを恒久固定するものではない。利用者向けのroute差分、pass-through、対応 / 非対応一覧は`COMPATIBILITY.md`を参照する。

## Legacy decision anchors and move notices

旧decision 9〜15を参照する外部linkやhistorical auditのため、旧番号と旧見出し相当のcompatibility anchorを残す。旧contract全文は保持せず、移動先だけを案内する。

<a id="decision-9"></a>
<a id="9-packagebase-buildとrequired-child-selectionの分離"></a>

* 旧decision 9: [PackageBase build / required-child selection contract](contracts/packagebase-child-selection.md)へ移動。

<a id="decision-10"></a>
<a id="10-separated-source-build上のrmdepsはunsupportedとする"></a>

* 旧decision 10: [separated source-build `--rmdeps` contract](contracts/source-build-rmdeps.md)へ移動。

<a id="decision-11"></a>
<a id="11-xdg-cache-cutoverの安全契約と実装の比例性"></a>

* 旧decision 11: [XDG cache cutover safety contract](contracts/xdg-cache-safety.md)へ移動。

<a id="decision-12"></a>
<a id="12-source-build-preferenceのxdg authorityとv2.0.1-patch例外"></a>

* 旧decision 12: [source-build preference XDG authority contract](contracts/source-build-preference-xdg.md)へ移動。

<a id="decision-13"></a>
<a id="13-ambiguous-providerはinvocation-localな明示選択とする"></a>

* 旧decision 13: [ambiguous provider selection contract](contracts/ambiguous-provider-selection.md)へ移動。

<a id="decision-14"></a>
<a id="14-root-package-discoveryは-s-selectでsource-awareな明示選択とする"></a>

* 旧decision 14: [root package selection contract](contracts/root-package-selection.md)へ移動。

<a id="decision-15"></a>
<a id="15-local-pkgbuildはbuild-localで明示しremote-package-identityと分離する"></a>

* 旧decision 15: [local PKGBUILD contract](contracts/local-pkgbuild.md)へ移動。
