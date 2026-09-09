# Moguet production contracts

## 文書の位置づけ

このdirectoryは、Issue別に確定したproduction contractを、存続するbehavior / safety boundaryごとに管理する。Issue番号は来歴を示すmetadataであり、filenameと現在のauthorityは契約の意味を表す安定した名前にする。

contractの規範上の正本は各文書の日本語本文である。英語利用者向けにこのindexが示す説明はinformativeであり、英語全文の別正本は作成しない。上位原則は日本語・英語併記の[`docs/DECISIONS.md`](../DECISIONS.md)を参照する。利用者向けのroute差分、pass-through policy、対応 / 非対応一覧は[`docs/COMPATIBILITY.md`](../COMPATIBILITY.md)を参照する。

## Contract index

| Contract | Origin Issue | Main boundary |
| --- | --- | --- |
| [Source-aware package identity](source-package-identity.md) | #355 | source、PackageBase、package child、revision、release、architectureの分離とread-only projection |
| [Reviewed AUR source state](reviewed-source-state.md) | #411 | PackageBase単位のexact reviewed revision、explicit acceptance、CAS publication、pinned build、legacy migration |
| [Trusted Git remote revision observer](git-remote-revision-observer.md) | #475 | authority-approved request、HTTPS-only Git observation、bounded process、strict SHA-1 / SHA-256 result、mutation-free boundary |
| [Installed devel Git tracking](devel-tracking.md) | #476 S8 | authority chain、identity invariants、schema/migration、compatibility、final acceptance |
| [Devel build provenance store foundation](devel-build-provenance-store.md) | #476 | 別XDG namespace、exact #411 binding、strict schema、immutable-generation CAS |
| [Evaluated devel source build proof](evaluated-devel-source-build-proof.md) | #476 | reviewed/evaluated source、actual makepkg Git workspace、dynamic version、fresh artifactのactual build proof |
| [Trusted source artifact transport sealing](trusted-source-artifact-transport.md) | #485 / #476 | trusted-root threat model、exact archive/signature digest、staged generation、privileged final reproofとpathname handoff |
| [Exact transaction receipt and fresh installed binding](exact-installed-artifact-binding.md) | #476 S5-B | cleanup/exact purpose分離、Install/Upgrade hook receipt、Post anchor、fresh descriptor-bound local DB、opaque generation、live mint |
| [Installed devel source build proof / Slice 5 result](installed-devel-source-build-proof.md) | #476 S5-C | closed final producer、same-lineage correlation、N=1、lossless operation/receipt/proof/cleanup、non-publication |
| [Trusted devel provenance publication](devel-build-provenance-publication.md) | #476 S6-B | sealed S5 resultのone-shot historical publication、lossless outcome、7-C専用publisher |
| [Current installed artifact observation / pure Git comparison](current-installed-artifact-observation.md) | #476 S7-A | fresh current bindingとtyped pure OID比較、transaction/network/normal routeから独立 |
| [Read-only devel package assessment](devel-package-assessment.md) | #476 S7-B | tip-only P/I/R local authority、#475 remote observation、one-time post-check、normal query接続 |
| [Normal authoritative devel routes](devel-normal-routes.md) | #476 S7-D | RPC/Git precedence、normal assessment/execution、registered/dry-run/-Qua |
| [Reviewed devel source execution bridge](reviewed-devel-source-build-execution.md) | #476 S7-C | typed pinからS4→S5→S6、lossless live result、legacy分離、normal reviewed owner接続 |
| [PackageBase build / required-child selection](packagebase-child-selection.md) | #268 | PackageBase build unitとrequired child install selectionの分離 |
| [separated source-build `--rmdeps`](source-build-rmdeps.md) | #269 / #404 | current source-buildのfail-closed、pacman-only no-op、future causal ownership / interaction boundary |
| [XDG cache cutover safety](xdg-cache-safety.md) | #305 | cache filesystem identityとlegacy cache非変更 |
| [source-build preference XDG authority](source-build-preference-xdg.md) | #335 | user XDG authority、ownership、atomic filesystem操作 |
| [interactive confirmation](interactive-confirmation.md) | #431 | boolean confirmationのsuffix、fixed token、typed outcome、exit / cancellation境界 |
| [ambiguous provider selection](ambiguous-provider-selection.md) | #272 | invocation-localな明示選択とmutation前preflight |
| [root package selection](root-package-selection.md) | #217 | `-S --select`のsource-aware root selectionとroute固定 |
| [local PKGBUILD](local-pkgbuild.md) | #271 | local root identity、metadata authority、source tree非破壊境界 |

## 英語利用者向けの読み方

- contract本文は日本語がnormative source of truthである。
- 英語の短い説明や表はinformativeであり、日本語本文の意味を変更しない。
- Moguet全体に適用する上位原則は[`DECISIONS.md`](../DECISIONS.md)の日英併記を参照する。
- route別の実際のcompatibility summaryとmatrixは[`COMPATIBILITY.md`](../COMPATIBILITY.md)を参照する。

## Authority flow

```text
DECISIONS.md (上位原則)
        ↓ 適用
docs/contracts/*.md (日本語normative production contract)
        ↓ 利用者向け要約・route差分
COMPATIBILITY.md (summary / matrix / pass-through)
```

Contractは、実装moduleやtypeの恒久的な固定ではない。implementationを変更する場合も、本文に記載されたfail-closed boundary、ownership、authority、transaction owner、partial completionの意味を弱めてはならない。
