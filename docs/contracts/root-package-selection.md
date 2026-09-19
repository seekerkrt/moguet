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

### Selection grammarとinteractive gate

interactive stdinでだけ、番号、複数番号、inclusive range、および表示済みofficial groupを表す`@group` selectorを受け付ける。empty input、`q`、`quit`、`cancel`、EOF、non-TTY、`--noconfirm`では選択せずnon-zeroで停止する。candidateが1件でも明示selectionを要求し、Enterや先頭候補をdefaultにしない。

invalidなselection expressionは一部だけを採用せず、同じcandidate snapshotに対してretryする。複数sourceの同名candidateを同時に選んだ場合はalternative source conflictとしてline全体を不採用にする。表示順はselection indexを固定するpresentation policyであり、sourceを暗黙決定するpriorityではない。

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
