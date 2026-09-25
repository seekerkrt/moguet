# Source-aware package identity contract

## 文書の位置づけ

この文書は、Moguet v3のprofile / snapshot / patch workflowへ先行する、package child、PackageBase、source、source revision、package release、architectureの共通identity contractである。規範上の正本は日本語本文である。

- Origin Issue: [#355](https://github.com/seekerkrt/moguet/issues/355)
- Parent design: [#59](https://github.com/seekerkrt/moguet/issues/59)
- Parent roadmap: [#344](https://github.com/seekerkrt/moguet/issues/344)
- Prerequisites: [#217](https://github.com/seekerkrt/moguet/issues/217)、[#271](https://github.com/seekerkrt/moguet/issues/271)
- Related contract: [PackageBase build / required-child selection](packagebase-child-selection.md)
- Related contract: [Reviewed AUR source state](reviewed-source-state.md)
- Related upper decisions: [decision 1](../decisions.md#decision-1)、[decision 2](../decisions.md#decision-2)、[decision 3](../decisions.md#decision-3)、[decision 4](../decisions.md#decision-4)、[decision 6](../decisions.md#decision-6)、[decision 7](../decisions.md#decision-7)

このfoundationはinternal non-breakingであり、public profile / patch command、storage schema、source mutation、source revision queryを有効化しない。既存production modelを置換せず、既存authorityからcommon valueへのread-only projectionだけを提供する。

## Current identity inventory

current `develop`では、必要なidentityはrouteごとのownerへ分散している。

| Boundary | Current authority | Retained identity | Common projection上の制約 |
| --- | --- | --- | --- |
| repository root candidate / selected root | `RepositoryRootPackageIdentity` | repository name、package child name | PackageBaseとsource locationを持たないため、完全なpackage child identityへ推測投影しない |
| AUR root candidate / selected root | `AurRootPackageIdentity` | package child name、PackageBase | source location、revision、architectureを持たない |
| repository root source-build | `ResolvedRepositorySourceBuildIdentity` | configured repository exact snapshot、requested child、PackageBase、Git URL | source revisionを持たない |
| AUR root source-build | `ResolvedAurSourceBuildIdentity` / `SourceCheckoutIdentity` | requested child、PackageBase、canonical source key、Git URL | source revisionを持たない |
| local root | `LocalSourceRoot` / `LocalPackageMetadata` | canonical path、filesystem identity、PKGBUILD snapshot、PackageBase、ordered children、epoch/pkgver/pkgrel、architecture | Git repositoryをauthorityにせず、source revisionはlocal route上`Inapplicable` |
| dependency / BuildPlan | `RootTargetIdentity`、`PlannedPackageTarget`、`ResolvedDependencyCandidate` | root input order、child、PackageBase、source-specific resolved candidate、composite version evidence | `BuildPlan::order`と`package_targets`だけではsource kindを一意に復元しない |
| update target | `AurUpdateRemotePackage` / `AurUpdateExecutionTarget` | installed child/version、AUR child、PackageBase、remote composite version、plan attribution | source revisionとarchitectureを持たない |
| registered / upgrade-all source | `RegisteredSourcePreferenceSnapshot` / `UpgradeAllExplicitSourceIdentity` | package name、resolved PackageBase、source kind、repository exact identity、derived canonical key | canonical keyをparseしてsource identityを逆算しない |
| expected artifact | `ExpectedPackageArtifactPath` / set | ordered path、workspace/context lineage | package metadata identityを持たない |
| required artifact | `RequiredPackageArtifactTarget` | PackageBase、package child、install reason | sourceとreleaseを持たない |
| actual artifact | `ArtifactPackageIdentity` | package child、composite full version、archive内PackageBase、architectureと各metadata state | source / revisionを持たない |
| correlated artifact | `PackageBaseArtifactIdentitySelectionSuccess` | actual PackageBase / architectureを含むselected / unselected child identity、full version、stable artifact index | source / revisionはupper projectionから別途必要 |
| relation observation | `PackageRelationObservedPackage` / `PackageRelationSourceIdentity` | repository / AUR / local source、child、optional PackageBase、version、runtime filesystem provenance | conflict/replaces観測固有であり、durable profile identityへ直接置換しない |

Issue #355のgeneric projectionへ入力されるGit checkout / fetch modelはPackageBase、remote URL、branch / remote-refを扱うが、common source package identityとしてexact commit object IDを保持しない。したがって、既存repository / AUR modelからのgeneric projectionはrevisionを`Known`にしてはならない。

Issue #411のreviewed-source lifecycleは、AUR source reviewとbuild continuationに限ってexact target OIDを別authorityとして保持する。これはgeneric projection inputの拡張ではない。reviewed-source persistent / build capabilityからexact OIDを`source_package_identity_projection`へ注入したり、common projectionの`Unknown`を`Known`へ昇格させたりしない。

## Implemented internal boundary

Issue #355のinternal foundationは次の責務別moduleで完成している。

| Module | Responsibility |
| --- | --- |
| `source_package_identity.hpp` | closed common value、field/state validation、structural equality |
| `source_package_identity_projection.hpp` | root、resolved source-build、local、dependency、artifact、AUR updateからのread-only projectionとtyped incomplete/failure |
| `source_package_compatibility.hpp` | dimension別compatibility state、typed mismatch reason、aggregate classification |

projection successはordered nonempty aggregateである。local sourceはaccepted metadata child orderを保ち、単一境界はsize 1を返す。1件でもcore identityを証明できなければfailure armだけを返し、partial aggregateを公開しない。AUR上のconfirmed absenceは`SourceNotFound`、query / metadata failureは`SourceMetadataUnavailable`として別issueを保持する。

compatibility evaluatorは`ExactMatch`、`SamePackageChild`、`SamePackageBase`、`Incompatible`、`Indeterminate`を返す。source、PackageBase、child、revision、version、architectureの各dimension stateとreasonを同時に保持し、localized stringからclassificationを復元しない。

read-only projectionの一部は、Issue #485のinternal production pathへ限定接続されている。`project_dependency_source_package_identity()`は`SourceArtifactInstall`のtrusted bindingとinvocation-owned cleanup correlation / evidenceへ、`project_artifact_source_package_identity()`はtrusted bindingのartifact整合とreceipt evidenceへtyped identity / correlation evidenceを渡す。これは既存のtrusted ownerが確定したidentityをread-onlyに投影する接続であり、common modelがsource-build routing、artifact identity、`SourceArtifactInstall` trusted transport、invocation-owned cleanupのauthorityを所有または置換することを意味しない。

generic compatibility evaluatorは引き続きproduction callerを持たず、public production workflow / routing decisionへ未接続である。Issue #411は`PackageBaseIdentity` / `SourceRevisionIdentity`のvalidated valueを別のreviewed-source capability内で再利用するが、reviewed-source authorityをgeneric projectionへ移さず、evaluatorをproduction decisionへ接続しない。public profile workflow、generic revision authority、generic compatibility-driven routing、v3 source-build / profile architectureは後続workであり、#355自体はpublic behaviorを追加しない。
Issue #363の[local recipe patch consumer](patch-customization.md)はvalidated identity valueをassociation keyへ使うが、
適用許可は別のstrict acquisition・明示selection・fresh candidate guardで確認し、generic compatibility evaluatorへ委ねない。

## Contract本文（日本語normative source of truth）

### Identity hierarchy

identityは次の階層を保つ。

```text
PackageSourceIdentity
  └─ PackageBaseIdentity (source + PackageBase)
       └─ PackageChildIdentity (PackageBase identity + package child name)

SourceAwarePackageIdentity
  ├─ PackageChildIdentity
  ├─ SourceRevisionIdentity
  ├─ PackageVersionIdentity
  └─ PackageArchitectureIdentity
```

- package child nameはinstall / selection対象である。
- PackageBaseはclone / fetch / build / workspaceのsource build unitである。
- source revisionはsource treeのrevision evidenceであり、PackageBase名でもpackage releaseでもない。
- package releaseはpackage metadataのepoch/pkgver/pkgrelまたはauthorityが返したcomposite version evidenceである。
- architectureはpackage metadata / artifactのarchitecture evidenceである。

package child、PackageBase、source revision、pkgver/pkgrelを一つのstring keyへflattenしない。split packageのsiblingsは同じPackageBase identityを共有できるが、異なるpackage child identityである。

### Source kind、repository、source location

`PackageSourceIdentity`は次のclosed alternativeだけを持つ。

| Source kind | Required source field | Location kind |
| --- | --- | --- |
| `Repository` | nonempty repository name | `GitRemote` |
| `Aur` | repository nameなし | `GitRemote` |
| `Local` | repository nameなし | `LocalPath` |

repository nameはlogical repository identityである。configured repository orderはread-phase provenanceとselection authorityであり、durable repository identityへ含めない。projection時はorder整合を既存ownerで検証してからrepository nameをcommon modelへ渡す。

source locationは`Known`、`Unknown`、`Unavailable`を区別する。

- `Known`: exactなremote URLまたはabsolute local display pathを保持する。
- `Unknown`: location conceptは適用されるが、入力authorityが観測値を保持していない。
- `Unavailable`: 観測を試みたがauthority unavailable、observation failure、invalid observationで値を得られない。

`Unknown`をempty locationへ、`Unavailable`を`Unknown`やknown default URLへ変換しない。local pathはserialization-readyなabsolute UTF-8 display valueであり、runtime filesystem safetyやsame-object proofを代替しない。device / inode / descriptor / ownershipは`LocalSourceRoot`等のruntime capability ownerへ残す。

### Source revision state

`SourceRevisionIdentity`は次を区別する。

| State | Meaning |
| --- | --- |
| `Known` | exact Git commit object IDを保持する |
| `Unknown` | revision conceptは適用されるが、このobservationでは取得・保持していない |
| `Absent` | authoritative observationがrevision不在を確認した |
| `Unavailable` | observationを試みたがauthority unavailable、failure、invalid observationで確定できない |
| `Inapplicable` | このsource contractではGit revisionをidentity authorityにしない |

Git commitはabbreviationではなくcomplete object IDとする。初期contractはcanonical lowercase hexadecimalのSHA-1 40桁またはSHA-256 64桁を受理し、object formatをtyped fieldとして保持する。empty、短縮、uppercase、non-hex valueはknown commitとして構築しない。

Issue #355のcurrent repository / AUR projection modelはcommitを観測していないため`Unknown`へprojectする。Issue #411のreviewed-source lifecycleが別boundaryでexact target OIDを保持していても、このgeneric projection ruleは変わらない。current local routeはGit repositoryを要求せず、PKGBUILD content / filesystem provenanceをauthorityとするため`Inapplicable`へprojectする。directory内に偶然`.git`が存在することから`Known`を推測しない。

`Absent`、`Unknown`、`Unavailable`、`Inapplicable`はいずれもknown revision matchではない。同じstate同士のstructural equalityをpatch適用やprofile compatibilityの成功条件にしない。

### Package versionとarchitecture

package releaseはsource revisionと別dimensionで保持する。

- `Composite`: AUR RPC、libalpm、artifact metadata等が返したfull version stringを、そのauthorityの値として保持する。common modelでepoch/pkgver/pkgrelへ再parseしない。
- `PkgverPkgrel`: `.SRCINFO`等が明示的に分離して提供したoptional epoch、pkgver、pkgrelを保持し、serialization用full versionも`[epoch:]pkgver-pkgrel`として保持する。
- `Unknown`: version conceptは適用されるが未観測である。
- `Unavailable`: observation failure等で確定できない。

version ordering、constraint satisfaction、equivalent version判定は既存`vercmp` / libalpm / typed constraint ownerへ委ねる。common identityは独自version solverを実装しない。

architectureはnonemptyなknown value set、`Unknown`、`Unavailable`を区別する。known setはserializationのためbyte-orderでcanonicalizeし、duplicateを拒否する。PackageBase-level architectureをchild overrideへ暗黙適用する判断は`.SRCINFO` metadata ownerが行い、common modelは受け取ったeffective evidenceだけを保持する。

### Field validityとconstruction

common modelはdefault constructionとpublic aggregate constructionを許さず、factoryが次を検証した値だけを作る。

- package child / PackageBase: existing Arch package identifier contractを満たし、valid single-line UTF-8である。
- repository name: nonempty single-line UTF-8である。
- Git remote location: nonempty single-line UTF-8でraw whitespaceを含まない。
- local location: nonempty absolute single-line UTF-8 pathである。
- composite version、pkgver、pkgrel、architecture: nonempty single-line UTF-8でraw whitespaceを含まない。
- epoch: 指定時は1文字以上のASCII digitである。
- known Git commit: complete canonical object IDである。

control character、invalid UTF-8、empty required field、source kindとlocation kindの不一致、known stateとpayloadの不一致、unavailable stateとreasonの欠落を表現できるconstructorを公開しない。

### Equality semantics

`operator==`はserialization-ready valueのstructural equalityである。

- source equality: source kind、repository name、location kind / state / value / unavailable reasonが同じ。
- PackageBase equality: source equalityとPackageBase nameが同じ。
- package child equality: PackageBase equalityとpackage child nameが同じ。
- revision equality: state、object format / commit、unavailable reasonが同じ。
- version equality: state、representation、full value、structured component、unavailable reasonが同じ。
- architecture equality: state、canonical value set、unavailable reasonが同じ。
- source-aware package equality: 上記全fieldがstructurally equalである。

structural equalityはcompatibility authorizationではない。特に`Unknown == Unknown`、`Unavailable == Unavailable`、`Absent == Absent`は、known source revision matchやpatch適用可能性を証明しない。

### Compatibility semanticsとmismatch reason

compatibility evaluatorは、少なくとも次をtyped resultとして区別する。

- exact source-aware match
- same PackageBaseだがdifferent child
- same package childだがrelease / revision / architecture evidenceが異なる
- source drift（source kind、repository、location）
- PackageBase mismatch
- package child mismatch
- known commit mismatch
- package release mismatch
- architecture mismatch
- identity evidence unknown / absent / unavailable / inapplicableによるindeterminateまたはunsupported

mismatch reasonはlocalized stringから逆算せず、source → PackageBase → child → revision → release → architectureの各dimensionをtyped evidenceとして保持する。複数dimensionが異なる場合に、後段の差異を成功へ丸めない。known同士のexact value matchだけがそのdimensionのmatchを証明する。revision `Inapplicable`を許可できるworkflowは、そのworkflow固有contractで明示し、generic evaluatorがknown commit matchへ昇格させない。

### Read-only projection rules

projectionは既存ownerのauthorityを再実装しない。

1. source-specific variantやroute contextを使い、package nameからsourceを再推定しない。
2. PackageBaseを保持しないrepository root candidateは、typed incomplete / unavailable resultとし、package nameからPackageBaseを推測しない。
3. `BuildPlan::order` / `package_targets`だけからsource kindを逆算せず、`ResolvedDependencyCandidate`、route、local root等のauthoritative contextと相関する。
4. artifactはarchiveからread-only libalpmで得たactual child、full version、PackageBase、architectureを、required target / correlated selection / upper source contextと一緒に使う。PackageBase / architectureが`Missing` / `Malformed` / `Unavailable`の場合はupper contextで補完せずtyped failureとし、known expected valueとのexact correlationだけをcompleteとする。filename、actual child名、full versionからPackageBaseやsourceを推測しない。architecture compatibility solverはこのprojectionで再実装しない。
5. derived canonical source keyをparseしてsource kind / PackageBaseへ戻さない。keyを生成したtyped source snapshotからprojectする。
6. current repository / AUR revisionは`Unknown`、current local revisionは`Inapplicable`とし、自動commit queryやreviewed-source exact OIDの注入をgeneric projectionへ追加しない。
7. partial projectionをcomplete identityとして公開しない。failure armは欠けたfieldとauthorityをtyped reasonで保持する。

## Non-scope

- public profile / patch command、profile schema、binding、storage、migration。
- PKGBUILD patch生成・preview・適用、source patch、user-owned tree mutation。
- generic projectionによるsource commitの自動取得、reviewed-source authorityの取り込み、Git working tree update、build history database。
- repository / AUR solver、package rename追跡、version solver。
- existing root / dependency / artifact / update / relation modelの置換。
- compatibility evaluatorのproduction decisionへの接続。

## Compatibility

このfoundation自体はCLI、selection、build、install、update、output、exit status、config、filesystem layoutを変更しない。Issue #411のreviewed-source exact OIDはこのgeneric foundationのrevision projectionを変更しない。利用者向けの要約は[`compatibility.md`のcommon identity section](../compatibility.md#compat-common-source-identity)を参照する。
