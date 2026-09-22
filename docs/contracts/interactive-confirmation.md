# Interactive confirmation contract

## 文書の位置づけ

この文書は、Moguetが所有するboolean confirmationについて、prompt suffix、固定input token、default、interaction gate、結果分類、終了status、停止時のside-effect境界を定めるnormative production contractである。文書の規範上の正本は日本語本文である。

- Origin Issue: [#431](https://github.com/seekerkrt/moguet/issues/431)
- Related Issues: [#134](https://github.com/seekerkrt/moguet/issues/134)、[#151](https://github.com/seekerkrt/moguet/issues/151)、[#217](https://github.com/seekerkrt/moguet/issues/217)、[#272](https://github.com/seekerkrt/moguet/issues/272)
- Update history: Issue #431で重複していたboolean confirmation helperを共通化し、Declined / Cancelled / Unavailable / InputFailureを区別するpublic contractとして追加。
- Related upper decisions: [decision 1](../decisions.md#decision-1)、[decision 2](../decisions.md#decision-2)、[decision 3](../decisions.md#decision-3)、[decision 5](../decisions.md#decision-5)、[decision 7](../decisions.md#decision-7)

## Contract本文（日本語normative source of truth）

### 対象と固定input grammar

このcontractはMoguet自身が解釈するboolean confirmationだけを対象とする。dependency provider selection、root package selection、およびpacman / makepkg / editor等のexternal programが所有する対話grammarは、それぞれのownerと既存contractを維持する。

boolean confirmationが受理するresponse tokenは、ASCII whitespaceを除いた後にASCII範囲でcase-insensitiveに比較する、次の固定値だけである。

- Accepted: `y`、`yes`
- Declined: `n`、`no`
- Cancelled: `q`、`quit`、`cancel`

これらはlocale-neutralなcommand inputであり、日本語の「はい」「いいえ」等をaccepted tokenへ追加しない。question、warning、diagnostic等のhuman-readable textはlocalization対象だが、固定tokenとsuffixは翻訳しない。interactive TTYで上記以外を入力した場合はwarningを表示し、同じquestionを再promptする。

### Suffix、default、interaction gate

| Suffix | empty input | `y` / `yes` | `n` / `no` | `q` / `quit` / `cancel` | invalid input | `--noconfirm` | non-TTY stdin |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `[Y/n]` | declared Yes defaultによるAccepted | Accepted | Declined | Cancelled | warning + re-prompt | declared Yes defaultによるAccepted | Unavailable。implicit Yesへ進まない |
| `[y/N]` | declared No defaultによるDeclined | Accepted | Declined | Cancelled | warning + re-prompt | safe default NoによるDeclined | safe default NoによるDeclined |
| `[y/n]` | defaultなし。warning + re-prompt | Accepted | Declined | Cancelled | warning + re-prompt | Unavailable | Unavailable |

`[y/n]`のempty inputはapprovalでもcancellationでもない。accidental Enterを暗黙のYesへ変換せず、formal cancellationはq-familyが担うため、明示responseを得るまで再promptする。

`--noconfirm`は宣言済みdefaultだけを利用できる。defaultなしのapprovalを作り出さない。non-TTY stdinはside effectを伴い得る`[Y/n]`のYes defaultを選ばず、`[y/N]`のsafe Noだけを利用できる。

### EOFとinput failure

interactive inputがclean EOFで終わった場合は、`Cancelled`の`EndOfInput`としてcurrent Moguet operationを停止する。streamの`badbit`等、実際のread / I/O failureは`InputFailure`とする。clean EOF cancellationとactual input failureを同じfailureへflattenせず、どちらもimplicit Yesへ変換しない。

### Outcomeの意味とpresentation

- `Accepted`: questionで求めたpositive answer。explicit tokenまたはcontract上許されたdefaultから生じる。
- `Declined`: question固有のnegative answer。optional questionではnegative branchを取り、commandを継続または正常skipできる。
- `Cancelled`: q-familyまたはclean EOFにより、current Moguet operationをその時点で明示的に停止する。
- `Unavailable`: interaction gate上、安全にanswerを得られない状態。approvalとして扱わない。
- `InputFailure`: actual input stream failure。user cancellationとは区別する。

`Declined`と`Cancelled`は同義ではない。たとえば`Edit PKGBUILD?`で`n`はeditorを起動せず処理を続け、`q`はcurrent operationを停止する。`Proceed with build?`で`n`はrequired continuationへのnegative answer、`q`はoperation cancellationである。required operationが完了しない場合はどちらもnon-zeroになり得るが、actual build / external / internal failureとはpresentation上区別する。

### Exit status（Option C）

diagnosticのseverity / classificationとprocess exit statusは独立したdimensionである。user cancellationをactual production failureと区別することは、常にexit 0を返すことを意味しない。route固有の既存contractを維持し、次の原則で投影する。

exit 0となり得る結果:

- optionalまたはdefault Noの後も、要求commandが正常完了した場合
- normal no-opまたはroute-owned normal skip
- inspection commandが既存status contract上、cancelled / ambiguous stateを正常な観測結果として表示する場合
- registered sourceのunknown-updateに対する`n`またはsafe default Noの既存per-target skip

non-zeroとなる結果:

- required approvalのDeclinedにより要求operationが未完了
- explicit CancelledまたはEOF cancellationにより要求operationが未完了
- approval Unavailable
- InputFailure
- external command、validation、metadata、internal等のactual failure

したがって「cancelは必ずexit 0」でも「non-zeroは必ずinternal error」でもない。caller / route ownerはtyped outcomeを保持したまま、要求operationが完了したかというpublic command contractに従ってstatusを決める。

### Cancellationとside-effect境界

cancellationは、その時点以降のcurrent Moguet operationを停止する。invocation全体に副作用がなかったこと、または既に完了したphaseをrollbackしたことを意味しない。現行routeの停止境界は次を維持する。

- existing-cache diff cancellation: `git fetch origin`後、working tree reset / makepkg前
- build-mode cancellation: checkout update / reset後、makepkg前
- `Proceed with build?` cancellation: editor後、makepkg前
- unknown-update cancellation: 完了済みsystem / Git phaseを保持し、makepkg前
- `clean` cancellation: pacman clean phaseが完了済みの場合があり、Moguet cache deletion前

diagnosticは先行phaseが完了済みであり得ることを隠さない。cross-phase atomic transactionやautomatic rollbackは、このcontractから導入しない。

## Non-scope / implementationを固定しない範囲

- dependency provider / root package selectorのinput grammar、candidate model、selection policyの変更。
- pacman、makepkg、git、editor等、external programが所有するprompt grammarの再実装。
- package transaction、dependency plan、provider / conflict / replaces、source selectionのsemantics変更。
- shared parser、variant、diagnostic module、exception wiring等の具体的なC++型やmoduleを恒久固定すること。
- cancellationをinvocation-wide rollbackまたはatomicity guaranteeへ拡張すること。

## Compatibility

suffix、fixed token、non-TTY / `--noconfirm`、outcome、exit status、side-effect境界の利用者向け要約は、[`compatibility.md`のinteractive confirmation section](../compatibility.md#compat-interactive-confirmation)を参照する。
