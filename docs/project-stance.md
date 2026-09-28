# Moguet のproject stance

## 概要

この文書は、Moguetの目的、引き受ける責任、correctness / safety / review / provenance、
非目標、customizationとmajor versionの姿勢の規範上の正本である。v2で確立した姿勢を、今後の判断にも適用する。

[README](../README.md)は利用者向けの入口、[設計ポリシー](decisions.md)は具体的な設計判断に
適用する原則と根拠、[個別contract](contracts/README.md)はbehaviorごとの保証を所有する。
route別の対応範囲は[compatibility](compatibility.md)、証拠の扱いは
[validation policy](validation.md)を正とする。current release scopeは
[v2.11.0 milestone](https://github.com/seekerkrt/moguet/milestone/32)、current capabilityはdevelopの実装、
public help/man、現行contractで確認する。closedの[roadmap #344](https://github.com/seekerkrt/moguet/issues/344)は
historical customization / release planningの記録であり、current status authorityではない。

## 日常的なAUR利用を扱う

MoguetはArch Linux上で日常的なAUR利用を扱うpacman-first AUR helperである。
検索、依存関係の解決、取得、review、build、install、updateをworkflowとして扱う。
特殊なsecurity scannerやaudit utilityを主目的とせず、まず一般的なAUR helperとして
日常利用できることをv2の土台とする。

[#606のsupport audit](https://github.com/seekerkrt/moguet/issues/606#issuecomment-5769277841)では、
ordinary split、provider、repo+AUR compositionを含む主要workflowの対応とevidenceを確認し、
新しいv2 blockerは0だった。これは全AUR packageや全dependency topologyへの対応宣言ではない。
対応済み、制限付き対応、意図した非対応を区別し、safe rejectをそれだけで未完成とは扱わない。

ordinary sourceの取得・評価・buildをmakepkgへ委譲する経路と、Moguetが高度なdevel tracking /
provenanceを確立する範囲は別である。後者の[対応するGit source範囲](contracts/devel-tracking.md)を、
非Git source全般のbuild可否へ読み替えない。#606のF-04はordinary SVN / Hg / FTP /
architecture-qualified source等のdelegated backend evidence不足であり、新たなproduct BUGを
実証したものではない。F-05はArchWiki live revision取得の外部制約で、product機能の不足ではない。
いずれもnon-blocking limitationとして保持し、未検証範囲を検証済みとも扱わない。

## 既存authorityとMoguetの責任

package transactionはpacmanへ委ねる。Moguetのlibalpm利用は対応するpackage metadataと
relationshipのread-only取得であり、libalpm transactionを開始・準備・commitしない。
PKGBUILD評価、source handling、package buildはmakepkg、Git object identityとtransportはGitの
契約を尊重する。Moguetは各toolの間で「何を対象に」「どの順序で」「どの根拠・確認をもって」
進めるかを組み立て、review、artifact検証、利用者の意図と結果の保全を担う。
具体的な分界は[設計ポリシーの責務境界](decisions.md#decision-6)と各contractに従う。

## Correctness / safety / review / provenance

- **Correctness**: 間違ったpackage、PackageBase、source、version、dependency、install reason、
  stateを正しいものとして扱わない。
- **Safety**: 分からない状態を分かったことにして進まない。
- **Review**: 利用者の判断が必要な境界を勝手に越えない。
- **Provenance**: 判断したrecipe / source / revisionと実際のbuild / installの対応を失わない。
  対応範囲外のbuildを、追跡済みのprovenanceとして扱わない。

安全に自動判断できることは自動で進める。利用者の確認・選択で必要な判断を確立できる場合は
確認を求める。それでもcorrectnessを確立できなければ、理由を示してその判断に依存する処理を
停止する（fail-closed）。独立targetの継続、取消、先行phaseの完了は各routeの契約に従って区別する。

review済みであることもprovenanceがあることも、upstream codeやpackageの安全性そのものを
保証しない。これらは判断と実行の対応を保つための仕組みであり、security認証ではない。
品質の根拠はautomated regression、deterministic / controlled integration、container、
実packageのdogfoodなどで積み重ねる。検証した範囲と未確認範囲を分け、bug-freeとは主張しない。

## Public UX

内部modelの厳密さを、通常利用者へ常時見せる複雑さにしない。Normal表示ではoperationの意図と
重要なstateを簡潔に示す。一方、確認、選択、warning、`RequiresCheck`、unsupported、blocked、
partial failureを隠したり、単純な成功・失敗へ潰したりしない。

必要なdiagnosticへ到達できる導線を保つ。たとえば`moguet --details plan <pkg>`と
`moguet --details deps <pkg>`は詳細な判断材料を表示するが、`--details`が全routeに共通するとは
扱わない。表示密度で判断・承認・実行可否は変わらない。具体的な表示責務は
[plan / depsのpresentation policy](compatibility.md#compat-plan-deps-presentation)等のroute別契約に従う。

他のAUR helperではそのまま進む操作でも、追加の確認・選択や理由を伴う停止が起こり得る。
これは利用者の意図とcorrectnessを保つための境界であり、重要な判断を省略することを簡潔なUXとは呼ばない。

## 推測で通さない境界

本来扱う領域でも、必要なidentity、state、authority、user decision、表現可能なtransactionを
確立できず、利用者確認でもcorrectnessを回復できなければ通さない。代表例は次のとおりである。

- ambiguous providerを非対話時や`--noconfirm`で勝手に選ばない。未解決dependencyやcycleを
  無理にbootstrapせず、conflicts / replacesから自動removeを推測しない。
- stale / changed sourceを以前の判断で通さず、source identity不明のままprovenanceを作らない。
  観測と再検証は各contractが定める境界で行い、任意の改変の常時検出を意味しない。
- selected childrenのinstall reasonを1つのPackageBase transactionで正しく表現できない場合、
  黙って妥協したり部分的にinstallしたりしない。

これらは[provider選択](contracts/ambiguous-provider-selection.md)、
[reviewed source](contracts/reviewed-source-state.md)、[devel tracking](contracts/devel-tracking.md)、
[PackageBase / child selection](contracts/packagebase-child-selection.md)等の既存契約に基づく。
safe rejectはその入力・状態に対する結果であり、領域全体を永久に対象外とする宣言ではない。

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

現在の`V=K`とsource-build preferenceはpackage単位のbuild environmentを扱い、
`/etc/makepkg.conf`を自動編集しない。上記のflagは説明例であり最適化の推奨ではない。
assignmentを実際に利用するかはpackage / upstream build systemにも依存する。
patch差分の明示保存・再適用は別authorityの実装済みExperimental機能であり、generic profile abstractionとは区別する。

## 既存 AUR helper への敬意

Moguet は既存 AUR helper と同じ判断や操作感をすべて追うものではない。

先行する既存ツールには、それぞれ長年の実績・思想・利用者がある。
日常的なAUR利用を実用的に扱うことと、他helperの全機能や判断をコピーすることは別である。

既存 AUR helper の機能を参考にするのは、機能を真似るためではなく、AUR helper として実用上どのような判断材料・安全導線・操作感が必要になるかを学ぶためである。

## 責任を広げすぎない

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

## 意図的なnon-goalsと拡張判断

「実装できること」だけを要件にせず、current requirementへ必要な最小構成を選ぶ。
AI等で実装可能でも、rare edge caseのためにgeneric frameworkを先回りして増やさない。
次はv2の完成条件にしない。

- pacman / libalpm、makepkg / PKGBUILD evaluator、Gitその他VCS implementationの再実装
- 全AUR package / 全PKGBUILD syntaxの対応保証、他helperの全機能の複製
- arbitrary / general-purpose dependency solver、conflicts / replacementsの全面自動解決、
  cyclic dependencyの汎用bootstrap
- 全VCS backendへの同等な高度provenance tracking
- AUR submission / hosting / moderation tool
- upstream source codeの安全性そのものの保証

現行solverは#606で確認したordinary usageの主要範囲を扱う。扱えない現実的なtopologyが
継続して報告された場合は、その具体的use caseと責任境界を評価し、必要ならfocused extensionを
検討する。万能solverを将来必ず作るというroadmapにも、永久に拡張しないという宣言にもしない。

## Current capabilityとcustomizationの姿勢

current released stableはv2.10.1、next planned releaseはv2.11.0である。以下はdevelopの現在地であり、
公開stableの収録内容とは区別する。

- **Implemented current:** ordinary AUR-helperの主要workflow、provider選択、source-aware routing、
  package単位のone-off environmentとpersistent preference、Bash / Zsh / Fishのcontext-aware completion。
  `build <pkg> V=K...`は今回限り、`add-src` / `edit-src`は永続管理、`build <pkg> --use-preference`は明示再利用。
  `build <pkg> V=K... --save-preference`はremoteのbuild/install lifecycle成功後、explicit assignmentだけを
  既存preferenceへ上書きせず昇格する。plain buildの自動保存ではなく、patch保存とも別authorityである。
  詳細は[source preference contract](contracts/source-build-preference-xdg.md)を正とする。
  completionはlocal sync prefix、typed `--build-mode=`、bounded delegated `-Q` token等に限定し、
  refreshやTab時AUR network検索をしない。[completion contract](contracts/shell-completion.md)の範囲を越えて主張しない。
- **Implemented Experimental:** local patch association / listing、AUR association、upgrade-family discoveryと明示Apply、
  reviewed ordinary AUR editからの明示Save。既存root PKGBUILDと既存top-level `*.install`のcontent editを、
  changed-file-per-patchのordered logical seriesとして保存・strict whole-series replayできる。
  旧PKGBUILD-only recordとの互換性を維持する。local consumerはPKGBUILD-onlyである。
  Experimentalはpublic surface / schema / UXが変わり得ることを示し、未実装やunsafeという意味ではない。
  詳細は[patch customization contract](contracts/patch-customization.md)を正とする。
- **Future enhancement:** customization対応のauthoritative devel proof / provenance、現在のcompletion contextを越える拡張等は
  別follow-upとして具体的な需要とauthorityを評価する。特定releaseへの収録は約束しない。
- **Non-goal / intentionally unsupported or unclaimed:** generic profile abstractionはcurrent requirementに採用しない。
  persistent patchのadd/delete/rename/type/mode変更、任意tracked payload、nested / binary / non-text customizationは非対応。
  pacman完全互換、全AUR topology対応、任意pacman semantics推測、全operationのdynamic completion、fuzzy finder、
  arbitrary profile completion、利用者の明示意図なしのpersistent config書き込みも主張しない。

## Major versionの判断

customization workはv2.x lineで続ける。過去のv3 labelだけを理由に次majorを予約しない。
profile / patch customizationというテーマやExperimental機能の実装自体はmajor bumpを要求しない。
具体的なbreaking change、rebranding、generation reset、同等の互換性上の理由が生じた時点で、
[version policy](versioning.md)に従いmajorを改めて判断する。
過去のplanningや公開release記録は当時の判断として保持し、current requirementや収録保証へ読み替えない。

## 判断基準

今後の機能追加で迷った場合は、以下を判断基準にする。

- pacman / makepkg に任せるべきことを奪っていないか
- Arch の基本運用を壊していないか
- AUR helper として必要な判断材料や安全導線になっているか
- 危険な操作が暗黙に実行されないか
- 明示オプション、確認、default の関係が分かりやすいか
- 既存 AUR helper の単なる模倣になっていないか
- 自分の Arch / AUR 運用を安全に支える道具になっているか
