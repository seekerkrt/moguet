# PackageBase build / required-child selection contract

## 文書の位置づけ

この文書は、source-build upper projectionがauthoritativeに確定したPackageBase build unitとrequired package childのinstall-selection unitを分離するsource-neutralなnormative production contractである。文書の規範上の正本は日本語本文であり、Issue本文や英語要約は来歴・説明として扱う。

- Origin Issue: [#268](https://github.com/seekerkrt/moguet/issues/268)
- Related Issues: [#98](https://github.com/seekerkrt/moguet/issues/98)、[#218](https://github.com/seekerkrt/moguet/issues/218)、[#242](https://github.com/seekerkrt/moguet/issues/242)、[#266](https://github.com/seekerkrt/moguet/issues/266)、[#267](https://github.com/seekerkrt/moguet/issues/267)、[#406](https://github.com/seekerkrt/moguet/issues/406)
- Related PRs: #291〜#296（#268 production slice）、#241、#257〜#261（#242 artifact lifecycle）、#412〜#414（#406 repository projection / integration）
- Update history: Issue #373で旧decision 9の本文から安定contractへ分離。Issue #406でofficial repositoryのstandalone / registered upper projectionへ適用範囲を拡張。
- Related upper decisions: [decision 1](../DECISIONS.md#decision-1)、[decision 2](../DECISIONS.md#decision-2)、[decision 4](../DECISIONS.md#decision-4)、[decision 5](../DECISIONS.md#decision-5)、[decision 6](../DECISIONS.md#decision-6)、[decision 7](../DECISIONS.md#decision-7)

## Contract本文（日本語normative source of truth）

### Identityとbuild unit

source buildでは、PackageBaseをrepository、build、workspace、package transactionの単位とし、source-build upper projectionが必要とするpackage childをinstall-selectionの単位とする。upper projectionはroute固有のauthorityを使う。AURはBuildPlan、official repositoryはconfigured repository順のstrict libalpm exact snapshotからrequired targetを確定し、PackageBase identityとchild identityを単一のpackage nameへflattenしてはならない。official repositoryのconfirmed `NotFound`だけをAUR fallbackとして扱い、query / config / metadata failureからabsenceやPackageBaseを推測しない。

official repositoryのstandalone / registered upper projectionは、single / multiple outputに関係なく`{authoritative PackageBase, requested repository child, Explicit}`をrequired targetとする`PackageBaseSet`を使う。registered routeは`OnlyIfUpdated` preparationを先に閉じる。sync repositoryの`SingularCompatibility` + existing `--needed`、registered AURの`SingularCompatibility` + existing provider / split guardはこのcontract適用で拡張しない。

1. 1つのPackageBaseは、1つのinvocation-owned fresh artifact workspaceで1回だけbuildする。expected outputは`makepkg --packagelist`からordered aggregateとして取得する。
2. upper projectionが要求するchildはrequired-target orderで保持する。childのartifactはfilenameの推測やPackageBase名ではなく、build後のpackage metadata identityでexactly one選択する。
3. expectedだがrequiredでないsibling、debug、その他のartifactはunselected result dataとして保持する。これらへinstall input、update target attribution、install outcome、install reasonを付与しない。
4. selected childrenはPackageBaseごとに1回のpacman transactionへ渡す。childごとのdesired install reasonをそのtransactionで表現できない場合は、部分的にinstallせずmutation前にfail closedとする。

### Artifactとfilesystemの安全境界

expected artifactとactual artifactについて、次をinstall前に証明する。

- expected identityとactual package metadata identityが一致すること。
- required childごとにmatching artifactがexactly oneであること。
- artifactがregular fileであり、invocation-owned workspaceのcontainment内にあること。
- freshness、ownership、filesystem identityを検証できること。
- missing、duplicate、unexpected、unknown identity、containment violationを推測で補わないこと。

PackageBaseとchildのidentity相関、workspaceのcontainment、artifactのfreshnessを証明できない場合は、`--noconfirm`指定でもinstallへ進まない。filenameの規則、directory layout、内部type、module分割はこのcontractが固定するauthorityではない。

### 検査したarchive bytesとtransaction input

prepareはarchiveと存在するdetached signatureをretained FDからwrite-sealed snapshotへ固定し、
そのarchive snapshotをlibalpmで検査してrequired childを選ぶ。元のnamed entry / inode / owner / containmentの
検証に加え、prepare完了時とtransaction直前に元archive・署名をsnapshotとbyte単位で照合する。
same-inode / same-size / mtime復元を含む内容変更は拒否する。unselected childの相関も保持し、
transactionへ渡すbytesはselected childのarchiveと署名だけとする。

legacy executorはfixed sudoからinstalled source-artifact helperの`install-legacy`を呼び、同じsealed bytesと
保存したname/version・SHA-256をroot-owned private stagingへ渡す。helperは元workspace pathnameを再openせず、
kernelの`/proc/PID/fd/FD`から一度開いたsealed inputを検証してcopyする。標準入力はpacmanのprompt用に維持する。
最終的なarchive/signature digest、stage generation、parent / named / retained identityの再証明とpacmanへのexecは
同じprivileged ownerが行う。clientの最終照合後に元pathnameが変わっても、transaction inputは検査済みsnapshotから変わらない。
root / kernelをtrustedとする境界とfinal reproofは[trusted transport contract](trusted-source-artifact-transport.md)に従う。

packageと`.sig`は通常の隣接pathnameでpacmanへ渡し、SigLevel、confirmation、install reason、`--needed`を変更しない。
空の`.sig`もabsenceへ潰さず、legacy専用schemaでSHA-256(empty)を保持してpacmanの既存署名policyへ渡す。
legacy selectionが要求しないoptional PackageBase / architectureを新しい選択条件にしない。
legacy専用purpose / prepared schemaはcleanup receiptや#476のexact installed bindingへ昇格できない。
helperは同じchildの終了をwaitし、hookが走らない完全な`--needed` skipも既存どおり成功として扱う。
zeroは検査済みinputに対する既存operation resultの根拠であり、fresh installed DB / exact receiptの新規保証ではない。
childの終了が不明なら成功とせず、stage / leaseを保持する。既知のtransaction終了後のstage cleanup failureは
警告として残し、実行済みtransactionを未実行・rollback済みへ変換しない。

snapshotは既存transportのresource上限（archive 4 GiB、署名16 MiB、aggregate 8 GiB）を超える場合に拒否する。
sealed snapshotとselected transfer inputを保持するため追加のmemory / swapを使い、作成・copy・sealの失敗はinstall前に停止する。

### Transaction、failure、cleanup

transaction failureはpackageごとのpartial successを証明しない。failed transaction後にchild successを推測せず、safeなattempt identity / versionをfailure evidenceとしてsuccessful outcomeから分離する。

workspace cleanupはtransaction success後に限る。cleanup failureはtransaction failureへflattenせず、すでにcompletedした全childの正確なinstalled / skipped-as-needed outcomeとunselected identityを保持するpartial successとして報告する。先行して完了したpackageをrollbackしたと報告せず、未実行のchildを成功扱いしない。

### `--noconfirm`とselection

`--noconfirm`は確認を省略するoptionであり、曖昧なartifact selection、mixed install reason、missing metadata、failure、unsafe filesystem stateを突破する許可ではない。requested childとmetadata identityから一意に決まるselectionは対話判断ではないが、identityを証明できないselectionを自動選択してはならない。

## Non-scope / implementationを固定しない範囲

- arbitraryなmultiple-output、sibling、debug artifactを明示なしに全installすること。
- provider選択、conflicts / replaces solver、version constraint solver、package group selection。
- artifact reuse、durable manifest、automatic rollback、unified transaction。
- PackageBase selection、artifact validation、cleanupを特定のmodule、type、syscall、directory layoutへ恒久固定すること。

## Compatibility

利用者向けのsplit package、PackageBase、selected / unselected artifact、`--noconfirm`の要約とroute差分は、[`COMPATIBILITY.md`のPackageBase / child section](../COMPATIBILITY.md#compat-packagebase-child-selection)を参照する。
