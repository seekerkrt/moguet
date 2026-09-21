# AGENTS.md

## 位置づけ

この文書は、Moguet repositoryで作業するときの入口・SSOT地図・固有の作業境界を定める。
言語非依存の共通契約はCodexのグローバル`AGENTS.md`、C/C++共通規約は`cpp-conventions` Skillを基準とし、ここでは再掲しない。

Moguet固有の指示、`docs/coding-conventions.md`、実際のbuild設定が共通規約と矛盾する場合は、より具体的なrepository側の契約を優先する。

## Repository概要と優先事項

Moguetは、pacman、makepkg、AUR、gitの既存契約を尊重しながらArch package操作を補助するhosted C++ CLIである。

このrepositoryでは、特に次を優先する。

- pacman-firstを維持し、upstream toolの責務を独自再実装しない
- plan / deps / fetchと、build / install等の副作用境界を明確にする
- CLI互換性、終了code、出力、設定、管理directoryの意味を不用意に変えない
- external commandと権限昇格を利用者から見える形に保つ

## 最初に読む文書

- `README.md`: 現行CLIと利用者向け契約
- `docs/decisions.md`: transaction、ownership、主要な設計判断
- `docs/project-stance.md`: projectの立場と非目標
- `docs/compatibility.md`: 互換性境界
- `docs/development.md`: branch、PR、mirror、release運用
- `docs/validation.md`: 段階別validation、approval evidence、再利用 / 無効化、review closure
- `docs/versioning.md`: version policy
- `docs/LICENSING.md`: dependencyと配布物のlicense契約
- `docs/coding-conventions.md`: Moguet固有のC++追加・上書き規約

設計判断の詳細をこの文書やコーディング規約へ複製しない。変更対象に対応する正式文書を正とする。

## 重要な責務境界

- plan / depsは調査・表示の層であり、clone、build、installを混ぜない。
- fetchは取得段階であり、既存cloneでは`git fetch origin`までを境界とする。working tree更新、pull、merge、reset、build、installを暗黙に追加しない。
- package transactionとsystem databaseのauthorityはpacman / libalpmへ委ねる。
- PKGBUILD評価とpackage buildはmakepkg / Arch packaging契約を尊重する。
- Moguet本体は通常userで動作し、必要なpacman操作だけを明示的なsudo境界へ渡す。
- AUR/network側のfailure、local validation failure、Moguet内部failureをCLI上で区別する。
- `--noconfirm`は対応経路のprompt省略であり、未解決dependency、ambiguous provider、conflict / replacement、削除、source selection等のguardを突破する許可ではない。

## Skill routing

- C/C++の生成・編集・レビューでは`cpp-conventions`を使い、続けて`docs/coding-conventions.md`を必ず読む。
- read-onlyの責務監査、unused判定、docs整合確認では`audit`を使う。
- 非自明な変更後のbuild / test / CLI確認では`verify`を使う。
- commit前の差分整理では`commit-prep`、GitHub操作では`github`を使う。
- handoffは依頼された保存方式に対応するhandoff系Skillへroutingする。

## Build・testの入口

- `make`: binaryとman pageの基本build
- `make test`: full host A–D
- `make test-<領域>`: 変更責務に対応する限定test
- `make test-host-release`: PR / mergeのcanonical host gateとなるA–D + G
- `make release-check`: standalone互換target。full host A–Dではない
- `make test-container`: offline/current Arch Docker E
- `make test-container-live`: actual provider / AUR / local F
- `make release-validate`: final RCのhost / offline/current Arch / live validationをcandidate identity付きで直列実行するoperator入口。policyは`docs/validation.md`
- `git diff --check`: docs-onlyを含む差分の基本確認

C++の生成・編集後は、`docs/coding-conventions.md`のchanged-file workflowを正とし、通常のvalidation前に
`scripts/format-changed-cpp.sh --write`、続けて`--check`を実行する。対象検出の失敗をrepository-wide
formatへfallbackせず、untrackedまたは今回と無関係なC++を暗黙に整形しない。

実行段階、approvalへ十分なevidence、再実行が必要な変更は`docs/validation.md`を正とする。
CLI出力や終了codeを変えた場合は対象commandを直接確認する。pacman、makepkg、sudo、system package databaseへ影響する確認は通常testと同列に実行せず、対象と副作用を明示した依頼に基づいて行う。

## Branch・remote・mirror

- GitHubの`origin`がcanonical、GitLabの`gitlab`がbackup mirrorである。
- `main`は最新安定版、`develop`は次releaseのintegration branchである。
- 通常の`feature/*`、`fix/*`、`docs/*`は`develop`から派生し、PRも`develop`をtargetとする。
- release branch、main反映、tag、GitHub Release、mirror更新の手順は`docs/development.md`を正とする。
- Issue、PR、commit messageは日本語を主文とする。release noteはtracked `RELEASE_NOTES.md`でEnglish / Japanese sectionを同期する。

## Repository固有の慎重領域

- package transaction、dependency plan、provider / conflict / replaces判定
- source-build preferenceと`${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/`
- shell quoting、package name / environment key validation
- temporary directory、current directory、partial clone / buildのcleanup
- sudo境界と利用者へ表示するcommand
- CLI option、出力、終了code、man pageの互換性

## Repository固有のnon-goal

- pacman、makepkg、git、AUR APIの独自再実装
- plan / deps / fetchへ暗黙の副作用を追加すること
- convenienceだけを理由にした汎用wrapperやglobal state
- project文書を越えてrelease / branch policyを再定義すること
- file分割、namespace導入、整形自体を目的にした一括変更
