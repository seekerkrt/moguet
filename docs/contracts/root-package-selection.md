# Root package selection contract

## 文書の位置づけ

この文書は、official repositoryとAURの候補からroot install targetを明示選択する`-S --select` routeのnormative production contractである。文書の規範上の正本は日本語本文である。

- Origin Issue: [#217](https://github.com/seekerkrt/moguet/issues/217)
- Related Issues: [#171](https://github.com/seekerkrt/moguet/issues/171)、[#217](https://github.com/seekerkrt/moguet/issues/217)、[#268](https://github.com/seekerkrt/moguet/issues/268)、[#271](https://github.com/seekerkrt/moguet/issues/271)、[#272](https://github.com/seekerkrt/moguet/issues/272)、[#86](https://github.com/seekerkrt/moguet/issues/86)、[#152](https://github.com/seekerkrt/moguet/issues/152)、[#168](https://github.com/seekerkrt/moguet/issues/168)
- Related PRs: #346、#348、#365〜#367（#217 model、route、production surface）
- Update history: Issue #373で旧decision 14の本文から安定contractへ分離。
- Related upper decisions: [decision 1](../decisions.md#decision-1)、[decision 2](../decisions.md#decision-2)、[decision 4](../decisions.md#decision-4)、[decision 5](../decisions.md#decision-5)、[decision 6](../decisions.md#decision-6)、[decision 7](../decisions.md#decision-7)

## Contract本文（日本語normative source of truth）

### CLI入口とsource-aware identity

root package discoveryの正式入口は`moguet -S --select [--needed] <query>`である。通常のpacman-compatible `-Ss`は非対話のsearch / presentationとして維持し、install selectionへ入らない。operation omissionや新しいbare operationは追加せず、unknown bare tokenはunknown-operation errorとする。

`-S`はinstall intent、`--select`はexact targetではなく検索候補から選ぶintentを表す。候補が1件でもdefault選択しない。repository candidateはsource kind、package name、repository name、root roleを保持し、AUR candidateはsource kind、package name、PackageBase、root roleを保持する。candidateとselected rootをpackage nameだけへflattenせず、同名でもsource identityが異なる候補は別に扱う。

`--needed`はselection自体をskipせず、planningやbuildもskipしない。supported routeでselectionとpreflightを完了した後、そのrouteが所有するfinal installにだけ適用する。

official searchとArch package groupはread-only libalpm metadata、AUR searchはtyped AUR responseをauthorityとする。pacmanのhuman-readable search outputやsync database formatをroot candidateへparseしない。Autoで一方のsource queryがfailureした場合、failureをempty resultへflattenした不完全なsnapshotからselectionを続行しない。

### Search candidate ranking

candidate membershipは既存repository / AUR searchとvalidation / dedupの結果をauthorityとする。
集約後、次のmatch quality順でsnapshotを並べ、全候補へ表示番号を付ける（Issue #436）。

1. package nameの完全一致。
2. package nameの先頭、または`-`、`_`、`.`、`+`、`@`直後からの一致。末尾境界は要求しない。
3. package nameのその他の部分一致。
4. AUR PackageBaseの部分一致。完全一致もこの分類とし、package name一致より下位に置く。
5. その他。description-only、native regex / group match、literal一致を説明できない候補もここへ残す。

CLI入口の既存ASCII空白trim後、providerへ渡したquery全体をliteralとして比較する。
比較時だけASCII `A`–`Z`を`a`–`z`と同一視し、queryやcandidate identityを上書きしない。
ranking独自のtrim、tokenize、regex評価、Unicode normalizationは行わない。
repositoryのregex searchとAURのname/description searchの成立条件は各providerに委ね、
descriptionの照合・長さ・出現回数を順位の追加根拠にしない。
root repository candidateにはPackageBase metadataがないため、ranking目的の追加取得はしない。
AUR split childはそれぞれのpackage nameとPackageBaseを保持し、base一致による展開や集約は行わない。

同点では既存canonical orderingを維持する。package nameのunsigned bytewise順、同名なら
repositoryからAURの順、同名repository間はconfigured repository順（同rankではrepository名の
bytewise順）、AURではPackageBaseのbytewise順とする。native返却順には依存せず、同じqueryと
candidate metadata / configured repository orderから同じ順序を得る。
このsource tie-breakは表示順の既存規則であり、sourceの推薦や自動選択を意味しない。
version、description、group metadata、source identity、dedup semanticsは順位付けで変更しない。
番号の解決とretryはこの並べ替え済みsnapshotを使い、表示modeで再sortしない。

候補のdrop、top-N、pagination、popularity、外部ranking API、fuzzy search、provider resolverや
dependency provider rankingへの適用は行わない。`-Ss`、候補表示format、plan / deps表示は別責務とする。

### Selection grammarとinteractive gate

interactive stdinでだけ、番号、複数番号、inclusive range、および表示済みofficial groupを表す`@group` selectorを受け付ける。empty input、`q`、`quit`、`cancel`、EOF、non-TTY、`--noconfirm`では選択せずnon-zeroで停止する。candidateが1件でも明示selectionを要求し、Enterや先頭候補をdefaultにしない。

invalidなselection expressionは一部だけを採用せず、同じcandidate snapshotに対してretryする。複数sourceの同名candidateを同時に選んだ場合はalternative source conflictとしてline全体を不採用にする。表示順はselection indexを固定するpresentation policyであり、sourceを暗黙決定するpriorityではない。

### Interactive candidate presentation

Normalは番号、`repository/package`または`aur/package`、versionを主情報として表示する。
名前が`aur`のconfigured repositoryにはlocalized `[repository]`を添え、AUR sourceと区別する。
PackageBaseがpackage名と異なる場合はNormalでも補助表示し、同一なら繰り返さない。
root candidateのdescriptionはidentity直下へindentし、選択可能な`@group`も表示する。
root candidateにinstalled stateを追加照会しない。
既存`PresentationDetail::Detailed`はfixed metadata fieldsを保持する。
`--details`のroute supportはCLI authorityに従い、候補表示のために拡張しない。
NormalのTTY配色はAUR `-Ss`のsource・package・version・installedのstyle primitiveを共有し、
候補出力がnon-TTYならANSIを追加しない。集合・順序・番号・入力・選択policyは表示modeに依存しない。

### Selection-before-mutationとroute projection

全selected rootのselection、identity validation、repository / AUR route projection、全static preflightが完了するまで、pacman、sudo、clone、git fetch、makepkg、artifact install、cache / workspace mutationを開始しない。

selected repository rootはexactな`repository/package`のbinary routeへ明示的にprojectする。selected AUR rootはpackage nameとPackageBaseを保持したAUR routeへprojectする。package nameだけをAuto routingへ戻し、sourceを再推定してはならない。

mixed selectionではrepository rootsとAUR rootsをcandidate orderを保ったまま分ける。repository transaction failureではAUR rootsを未実行とし、AUR failureでは完了済みrepository transactionをrollbackしない。cross-source unified transaction、automatic rollbackは追加しない。完了済み、失敗、未実行を区別し、partial completionやfailureをsuccessへflattenしない。

`#272`のprovider selectionとはTTY gate、cancel / retry / EOF、no-default、selection-before-mutationだけを共有し、dependency provider固有のexactly-one sessionやchoice cacheをroot selectionへ混ぜない。#268のsplit artifact selection、conflicts / replaces、version solverも別責務として維持する。

## Non-scope / implementationを固定しない範囲

- operation omission、先頭候補の自動選択、machine-readable non-interactive selection。
- provider choiceの永続化、provider solver、split artifact solver、conflicts / replaces solver。
- cross-source atomic transaction、automatic rollback、fuzzy finder、GUI / TUI。
- libalpm session、candidate adapter、selection parser、route projectionの具体実装を恒久固定すること。

## Compatibility

`-Ss`との差、`-S --select`入口、source identity、selection grammar、TTY / `--noconfirm`、route matrixは、[`compatibility.md`のroot package selection section](../compatibility.md#compat-root-package-selection)を参照する。
