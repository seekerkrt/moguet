# Moguet の立ち位置メモ

## 概要

この文書は、Moguetの現在と将来の発展に共通する立ち位置を整理するためのメモである。

製品として目指す能力と引き受ける責任範囲を示す。設計上の規範は[設計ポリシー](decisions.md)、
具体的な保証は[個別contract](contracts/README.md)、その証拠は[validation policy](validation.md)へつなぐ。

## Arch の流儀を尊重する

Moguet は、Arch Linux を別のディストリビューションのように変えることを目的としない。

Arch の基本は、公式リポジトリのバイナリパッケージを pacman で素直に使うことにある。
必要に応じて PKGBUILD / makepkg / AUR を使って調整できる柔軟さも Arch の魅力だが、それはシステム全体を常時ソースビルド化することとは異なる。

Moguet は、pacman や makepkg を置き換えるのではなく、それらの流れを尊重しながら、必要な部分を補助する道具として育てる。

## Gentoo 化を目指さない

Gentoo 的な細かい制御やソースビルド中心の思想には魅力がある。
しかし、Moguet は Arch を Gentoo のように使うためのツールではない。

すべてのパッケージを自前でビルドしたり、システム全体を細かく最適化したりすることは目的にしない。
基本は Arch の公式バイナリを使い、例外的に自分で調整したいパッケージだけを扱う。

## 過剰な最適化を目的にしない

Moguet は、性能向上やベンチマークのために過剰な最適化を行うツールではない。

`-march=native`、LTO、PGO、全体的な再ビルドなどにはロマンがあるが、日常運用ではビルド時間、保守コスト、不具合リスクに見合わない場合も多い。

目指すのは、速さのための過剰最適化ではなく、必要な変更を安全に記録し、再適用しやすくすることである。

## makepkg.conf の代替を目指さない

Moguet は、`makepkg.conf` の代替を目指すものではない。

`CFLAGS` / `CXXFLAGS` など、makepkg 全体に適用する基本設定は `makepkg.conf` に任せる。  
Moguet が扱うべきなのは、全体設定ではなく、パッケージ単位の例外である。

たとえば、普段は `makepkg.conf` で `-O3 -march=native` を使っていても、特定のパッケージだけ安全性や安定性を優先して `-O2` に抑えたい場合がある。

このような場合に、Moguet は以下を記録・再適用できる道具として育てたい。

- このパッケージだけ全体設定から外す理由
- パッケージ単位の build option
- patch や PKGBUILD 差分
- 更新時に再適用すべき変更
- 差分が当たらなくなった場合に安全に停止する導線

つまり、Moguet は過剰最適化のための道具ではなく、必要な例外を安全に管理するための道具でもある。

## 既存 AUR helper への敬意

Moguet は既存 AUR helper と同じ判断や操作感をすべて追うものではない。

先行する既存ツールには、それぞれ長年の実績・思想・利用者がある。
Moguet がそれらと同じ立場を名乗る必要はない。

既存 AUR helper の機能を参考にするのは、機能を真似るためではなく、AUR helper として実用上どのような判断材料・安全導線・操作感が必要になるかを学ぶためである。

## Moguet の当面の役割

Moguet の当面の役割は、pacman / makepkg の流れを尊重しつつ、AUR package を安全に扱いやすくすることである。

重視すること:

- AUR package を安全に build / install できること
- AUR RPC の情報を使って判断材料を表示すること
- 導入済み、out-of-date、PackageBase などの状態を分かりやすくすること
- root / sudo 実行など危険な使い方を防ぐこと
- `--noconfirm`、対話プロンプト、default selection の挙動を一貫させること
- pacman / makepkg に任せるべきことを Moguet 側で抱え込みすぎないこと

能力の広さと責任範囲の広さは同じではない。ordinary/common AUR package topologyは、正しい根拠を持って
扱える範囲で普通に動くことを目指す。未対応・不明・不整合を観測し、必要なauthorityを確立できない場合は、
定義された境界でfail closedし、理由を示して停止する。

Moguetは必要なauthorityを、必要なphaseで、必要な期間だけ保持する。source / revision / artifact /
install identity、provenance、明示承認、破壊操作のcontainment、利用者所有sourceの非破壊、
failure・cancellation・partial outcomeの保全は、その境界で守るべき正しさである。
same-UID userやreview済みbuild processの常時監視、phase間の一時改変から復元までの完全検出、
全descendantのsecurity mediation、汎用sandboxや汎用network policyは引き受けない。

**検出は自動、復旧は明示操作（Detection is automatic. Recovery is explicit.）**とする。
これは定義済みの観測でderived stateの不整合を検出する責務であり、任意の手動改変の追跡、
自動修復、修復後に同じ失敗transactionを継続する保証ではない。対応する復旧もMoguet-ownedな
disposable stateへ狭く限定し、ownershipとcontainmentを確認する。unknown / external pathや
利用者所有sourceの削除、durable provenance / review stateの消去、万能resetを意味しない。

## 将来のbuild tuningで広げたい方向

将来のMoguetでbuild tuningを発展させる場合も、Archの流儀を壊さないことを前提にする。

Moguet が扱うべきなのは、Arch 全体をソースビルド化することではなく、例外的に調整したいパッケージの差分を管理することである。

たとえば:

- 公式 PKGBUILD に小さな patch を当てる
- 一部の build option を記録する
- kernel config などの差分を保存する
- 更新時に自分の差分を再適用する
- 差分が当たらなくなった場合は安全に停止して確認する

これは「Arch を別物にする」ためではなく、Arch の仕組みに乗ったまま、必要な部分だけを調律するための機能である。

## 判断基準

今後の機能追加で迷った場合は、以下を判断基準にする。

- pacman / makepkg に任せるべきことを奪っていないか
- Arch の基本運用を壊していないか
- AUR helper として必要な判断材料や安全導線になっているか
- 危険な操作が暗黙に実行されないか
- 明示オプション、確認、default の関係が分かりやすいか
- 既存 AUR helper の単なる模倣になっていないか
- 自分の Arch / AUR 運用を安全に支える道具になっているか

## まとめ

Moguet は、pacman を置き換えるものではない。
Arch を Gentoo 化するものでもない。
既存 AUR helper と同じ判断や操作感をすべて追うものでもない。

Arch の流儀を尊重しながら、AUR や一部のカスタムビルドを安全に扱いやすくするための道具として育てる。
