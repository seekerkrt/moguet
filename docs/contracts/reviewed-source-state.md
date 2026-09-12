# Reviewed AUR source state contract

## 文書の位置づけ

この文書は、AUR Git sourceについて、最後に利用者が明示的に確認したexact revisionを
PackageBase単位で保持し、そのrevisionから次のbuild targetまでをreviewするproduction
contractである。規範上の正本は日本語本文である。

- Origin Issue: [#411](https://github.com/seekerkrt/moguet/issues/411)
- Parent roadmap: [#344](https://github.com/seekerkrt/moguet/issues/344)
- Follow-up to: [#151](https://github.com/seekerkrt/moguet/issues/151)
- Related Issues: [#59](https://github.com/seekerkrt/moguet/issues/59)、[#355](https://github.com/seekerkrt/moguet/issues/355)、[#359](https://github.com/seekerkrt/moguet/issues/359)、[#363](https://github.com/seekerkrt/moguet/issues/363)
- Related contracts: [source-aware package identity](source-package-identity.md)、[interactive confirmation](interactive-confirmation.md)、[XDG cache safety](xdg-cache-safety.md)
- Related upper decisions: [decision 1](../DECISIONS.md#decision-1)、[decision 2](../DECISIONS.md#decision-2)、[decision 4](../DECISIONS.md#decision-4)、[decision 5](../DECISIONS.md#decision-5)、[decision 6](../DECISIONS.md#decision-6)、[decision 7](../DECISIONS.md#decision-7)

## Contract本文（日本語normative source of truth）

### Scopeとauthority

このcontractの対象は、canonical AUR Git remoteから取得するsource-buildだけである。
official repository source-buildと`build --local`のlocal PKGBUILD routeには、reviewed-source
stateを追加しない。

source lifecycleでは、少なくとも次を別authorityとして扱う。

```text
fetched revision
reviewed revision
built revision
installed revision
```

このcontractが所有するpersistent authorityは`reviewed revision`だけである。fetch、review
表示、build、installの成功だけからreviewed revisionを推測しない。built / installed stateを
reviewed stateへ昇格させず、後段のbuild / install結果をreview済みかどうかの証拠にしない。

### PackageBase単位とpersistent state

reviewed-source stateの単位はPackageBaseである。split packageのchildrenは同じAUR
PackageBase stateを共有し、childごとの重複recordを作らない。identityは少なくともsource kind
`Aur`、canonical Git remote、PackageBase、exact reviewed commitを保持する。package child、
cache path、checkout HEAD、branch、remote ref、`.SRCINFO`、artifact version、canonical source
key、URL leafから不足fieldを推測しない。

stateはcacheと分離したXDG state authorityに保存する。

```text
${XDG_STATE_HOME:-$HOME/.local/state}/moguet/reviewed-sources/aur/
```

cacheは再生成可能であり、cacheのcleanup、削除、recloneからreviewed-source stateを独立させる。
read-only lookupはstate directoryやrecordを作成しない。recordはcurrent schema、source identity、
complete Git object IDを検証し、unsafe file type、owner、mode、hardlink、symlink、lineage、I/O
failureを正常なabsenceへ丸めない。

### Lifecycle

exact target commitをpinした後、persistent observationとGit object availabilityから次を選ぶ。

| Observation | Review lifecycle | State publication |
| --- | --- | --- |
| stateなし（`Missing`） | empty treeからexact targetまでのinitial full review | explicit acceptance後に最初のrecordを作る |
| valid reviewed revision == exact target | already reviewed | promptもstate rewriteも行わない |
| valid reviewed revision != exact target | previous reviewed revisionからexact targetまでのupdate review | explicit acceptance後だけtargetへ進める |
| valid baseline objectがmissingまたはcommitではない | cache HEADへfallbackせず、empty treeからexact targetまでをfull rebaseline review | explicit rebaseline acceptance後だけ置換する |
| current-schema stateがinvalid / corrupted / source-mismatched | reviewed済みと推測せず、empty treeからexact targetまでをfull rebind / rebaseline review | reasonを保持したexplicit acceptance後だけ置換する |
| unsupported future schema、unsafe history、store failure、inconsistent observation | fail closed | overwriteしない |

update baselineがtargetのancestorでない場合も、cache HEADへfallbackしない。exact baseline treeと
exact target treeの差分を示し、history divergenceを明示する。baseline unavailableのfallbackは
empty-tree full reviewであり、mutable checkoutの状態ではない。

`Invalid`、`Corrupted`、`SourceMismatch`、`UnsupportedFuture`を`Missing`へ丸めない。
current-schemaの異常stateはreasonを保持した明示full rebind reviewを要求する。future schema、
fork / gap等のunsafe history、authorityを証明できないstore stateはcompatibility buildへも
fallbackせず停止し、既存recordをdowngrade overwriteしない。

### Materialized reviewとreview-sensitive source

review eligibilityのauthorityはAUR Git treeのtracked file全体である。extension whitelistを
使わず、added、modified、deleted、renamed、type-changedを保持する。path、mode、object ID、
取得可能なsize、text / non-text classificationを表示し、binary、NUL-containing content、
Gitlink、mode-only changeを「変更なし」へ丸めない。

line-reviewable contentはexact Git blobからboundedにmaterializeし、text patchはstrict replayで
検証してからterminal-safeに表示する。raw Git path / patch / blob bytesをそのままterminalへ
流さない。完全なcontent reviewを表示できないnon-text sourceはmanual inspection requiredとし、
root `PKGBUILD`またはtop-level `*.install`のreview-sensitive contentを安全に表示できない場合は、
より強いfailureとしてacceptanceとbuildを停止する。

root `PKGBUILD`とtop-level `*.install`はreview-sensitive guidanceとして強調するが、これらだけを
review対象にしない。patch、service unit、sysusers / tmpfiles、helper script、local config / source、
その他のtracked fileもinventoryから除外しない。`.SRCINFO`はgenerated metadataとしてinventoryに
残すが、lower-priority表示とし、PKGBUILDやtracked sourceのreview authorityにはしない。

### Explicit acceptance

reviewのmaterialize / 表示成功とacceptanceは別eventである。表示しただけではstateを進めない。
reviewed-source acceptance promptはdefaultを持たず、interactive TTYで利用者が明示的に入力した
`y` / `yes`だけがacceptance authorityになる。generic confirmation result、default Yes、
`--noconfirm`、non-TTY inputをacceptanceへ昇格させない。

次の経路はreviewed stateをadvanceしない。

- `--nodiff`または`review.diff = "skip"`
- explicitなreview declineまたはsafe default No
- `--noconfirm`
- non-TTY input
- `q` / `quit` / `cancel`
- EOF、input failure
- materialization / presentation failure
- manual inspection requiredまたはreview-sensitive source unrenderable
- lifecycle / Git / checkout / state-store failure

current compatibilityを維持できる明示skip、decline、safe No、`--noconfirm`、non-TTY経路は、
reviewed authorityを持たないcompatibility-only buildとして継続し得る。ただしstateは据え置く。
q-family、EOF、input failure、unsupported review、unsafe / future / inconsistent state等、operation
stopに分類された経路はbuildへ進まない。

### Initial devel tracking bootstrapのfull review

ordinary exact target-less `-Syu`の明示bootstrap intentは、通常のreview lifecycleとは別purposeで
full inventory reviewを要求する。valid reviewed stateが同一revisionでもAlreadyReviewedへ短絡せず、
異なるrevisionでも差分reviewだけにはしない。元のstore observationとexact CAS predecessorを保持し、
Missingへの偽装・削除・修復は行わない。bootstrapの開始確認と、このfull reviewの明示acceptanceは別である。
このpurposeはexisting valid provenanceのnormal updateやexplicit source-buildへ伝播しない。

### CAS publicationとstate advancement

publicationはreview開始時に読んだexact record identityとraw contentsをguardにするCAS semanticsを
持つ。別processが先に新しいstateをpublishした場合、stale writerはlast-writer-winsで上書きしない。
current stateが同じexact targetへ既に進んでいる場合だけidempotent same-target successとして扱い、
別target、future schema、unsafe history、観測不能なconflictをretry writeへ変えない。

acceptance tokenを得た直後にはpublishしない。exact targetのdetached checkout materialization、
checkout safety revalidation、editor boundary、build-mode confirmation等、makepkg前の既存preflightを
完了してからpublishする。publicationはmakepkg開始前に完了させ、definite failure、CAS conflict、
post-commit uncertaintyをreview成功へflattenしない。

正常にaccept / publishされたreviewed revisionは、後続のmakepkg、artifact validation、install、
cleanup failureによってrollbackしない。reviewed、built、installedは別authorityであり、結果表示も
それぞれを分ける。publication後にcheckout proofを失った場合はbuildを開始しないが、既にpublish
されたstateを「なかったこと」にしない。

### Exact pinned build authority

fetchまたはclone後にtarget refをcomplete Git commit OIDへresolveし、accepted / already-reviewed
continuationではreview projection、acceptance、exact checkout、state publication、build continuationへ
同じpinned targetを渡す。後段でbranch、`origin/<branch>`、checkout HEAD等のmutable refを再resolveして
reviewed build authorityにしない。

reviewed buildは、exact targetへdetached checkoutし、残留worktree / index stateを除去して、
PackageBase leaseとcheckout filesystem identityを保持する。makepkg開始直前とexternal command後にも
exact checkout / overlay proofを再検証する。mutable checkout HEAD、branch、remote ref、cache path
stringだけではbuild continuationを作らない。

このexact pinned ruleはreviewed-source authorityを持つcontinuationの契約である。明示skip / decline等で
維持するcompatibility-only buildはreviewed authorityを持たず、legacy checkout continuationを
「review済み」またはpinned reviewed buildへ昇格させない。compatibility routeからstate publicationや
reviewed upstream revision provenanceを作らない。

### Editor overlayの分離

`--edit` / `--noedit`と`review.pkgbuild`はPKGBUILD / detected top-level `*.install`を開く
invocation-local editor policyであり、upstream reviewed-source acceptanceではない。editor前後の
exact worktree projectionから変更の有無を観測し、変更がある場合はreviewed upstream commit上の
invocation-local overlayとしてbuild provenanceへ保持する。

editor overlayはupstream reviewed revisionと別authorityである。overlay bytesをpersistent
reviewed stateへ書かず、overlayをacceptance token、upstream commit、将来invocationのpatch、
またはgeneric source identityへ昇格させない。user-authored patchの保存・再適用は#59 / #359 /
#363の別責務である。

### Legacy migration

Issue #411より前の利用者が持つ既存AUR cacheにはreviewed-source stateがない。この状態は異常では
なく、次のnormal lifecycleとして扱う。

```text
no reviewed state
  -> Missing
  -> InitialFullReview
```

legacy cacheのcheckout HEAD、branch、remote ref、file timestamp、build artifactからreviewed revisionを
捏造しない。legacy cacheを「既にreview済み」と推測せず、最初に対象となったAUR PackageBaseは
一度full reviewとexplicit acceptanceを必要とする。userによるstate file作成、cache変換、手動
migrationは不要である。

このnormal migration ruleは、本当にstateがない場合だけに適用する。invalid、corrupted、
source-mismatched、unsupported future、unsafe / inconsistent stateをMissingとして扱うmigrationは
行わない。

### Official repository / local routeのnon-scope

official repository source-buildはconfigured repository / libalpm snapshotのauthorityを維持し、
reviewed-source stateをread / writeしない。local PKGBUILD routeはuser-owned filesystem / content
provenanceとinvocation-owned snapshotをauthorityとし、偶然Git repositoryであってもreviewed-source
stateを作らない。これらのrouteをpackage nameやGit URLからAUR reviewed routeへ再分類しない。

### Issue #355 common identityとの関係

Issue #355のgeneric source-aware identity projectionと、Issue #411のreviewed-source persistent /
build authorityは別boundaryである。

```text
generic source identity projection
  !=
reviewed-source persistent/build authority
```

#355のcurrent repository / AUR projectionは、projection元のgeneric modelがexact commitを保持しない
ため、revisionを引き続き`Unknown`とする。#411はlifecycle内でexact target OIDを取得し、既存の
`PackageBaseIdentity` / `SourceRevisionIdentity` valueをreviewed-source capabilityとして再利用するが、
そのOIDを`source_package_identity_projection`へ注入してgeneric projectionを`Known`へ昇格させない。

reviewed-source state、accepted target、pinned checkoutは、#355のpartial common identityを補完する
fallbackでも、generic compatibility evaluatorのsuccess tokenでもない。両boundaryは同じvalue型を
共有しても、取得元、lifetime、consumer、authorizationを混在させない。

## Non-scope / implementationを固定しない範囲

- user-authored PKGBUILD patchの保存・再適用、patch conflictの自動解決。
- arbitrary source codeのsemantic analysis、malware detection、maintainer trust score。
- reviewed revisionからのautomatic rollback、build / install history database。
- review skipの全面禁止、cross-package atomic transaction。
- state recordのC++ class名、internal capability名、renderer layout、Git command列を恒久固定すること。

## Compatibility

利用者向けのworkflow、option、migration、route差分は、
[`COMPATIBILITY.md`のreviewed AUR source section](../COMPATIBILITY.md#compat-reviewed-source-state)を
参照する。
