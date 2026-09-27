# Shell completion closure (Issue #253 Slice 1–5)

completionはpublic CLI authorityのprojectionであり、runtime validationの代替ではない。
このcontractは実装した狭いcontext、表示、failure boundary、検証のownerを固定する。
installed/registered package候補、AUR、他pacman operationへの拡大を宣言しない。

## Authority and supported contexts

canonical flowはCLI exporter/schema → generator → Bash/Zsh/Fishである。
operation/form/operand、occurrence/conflict、allowed_values、fixed alias値は`cli_authority.hpp`、
表示文言は`completions/descriptions/en.json`、upstream spellingはsource-controlled raw snapshotを使う。
shell別のoption/value catalogue、upstream prose由来のarity/effect推測を作らない。

| candidate | supported context | provider |
|---|---|---|
| local sync package名 | plain `-S`の最初のbare prefix、追加option/operandなし | private prefix helper、1回/event |
| finite build mode | applicable formのattached `--build-mode=`、alias/final-value agreementを維持 | static allowed_values、0回 |
| upstream option spelling | exact `-Q`のinitial/known transparent tail | pinned token projection、0回 |
| current Moguet/static options | existing operation/form/conflict/occurrence authority | static、0回 |

`-Ss`はfree Query、`-S --select`もQueryであり、`--aur`へlocal sync候補を混ぜない。
alternate DB/root、pending value、marker、unknown modifier/tail、prior operand等ではpackage providerを呼ばない。
`-Q`はunknown/token-only arityやopaque operandの後でupstream option候補を止める。
詳細なscopeとbudgetは[local prefix](local-sync-package-prefix.md)と
[query spelling](pacman-query-completion.md)を正とする。

## Lexical boundary for every option candidate

shared `LEXICAL_VALUE` / `BOUNDARY`をoption、typed value、operation discoveryの候補にも適用する。
pending option value中と`--`以降ではoption候補を提示しない。current valueが`--ne`のように
optionに見えても値のままである。pending値として消費された`--`はboundaryにしない。
完成したinline valueはfollowing valueを待たない。これは既存lexical arityのprojectionだけであり、
unknown/open grammarのarityや値の妥当性を推測するものではない。
Bashの`=`等のwordbreak/quotesは既存logical-word処理で復元し、evalしない。

## Ownership presentation

explicit OptionContractのownershipはupstream collisionより優先し、tokenは1候補だけにする。
Zshはnative `_describe`のcategory tag/groupを使う。見出し表示は利用者のzstyle設定に従う。
Fishはdescription欄へcategoryを表示し、Bashはraw tokenを保持する。
package名とfinite valueにはoption ownershipを付けない。表示を変えてもsemantic候補集合を変えない。

## Failure and side effects

private helperのmissing/nonexecutable/nonzero/timeout、partial/newline/protocol不正、
count/byte超過ではdynamic package候補全体を破棄し、static候補を保持する。診断は候補にしない。
valid empty/truncated inventoryは成功であり、missing/corrupt DBやconfiguration failureと区別する。
snapshot missing/corrupt/hash/version/grammar/budget failureはgeneration失敗であり、
tracked旧outputをhidden fallback catalogueにしたりruntime host lookupへ切り替えたりしない。

Tab処理はnormal Moguet startup、pacman help/version、AUR/network、sudo、transaction、Git、
PKGBUILD/build/plan、config/state/persistent cache writeを開始しない。
private helper/workerとread-only pacman-conf、Fishのprotocol用sortだけがpackage経路のexternal transportである。
libalpm-linked library初期化が受動的なsocketを生成する場合と、network通信/requestを区別する。
runtime evidenceはconnect/packet送受信とcompletion起因のwriteを検査し、shell自身のstartup時の
config/cache初期化をcompletionの副作用と混同しない。
`__complete = UNNECESSARY`であり、public/hidden CLIを追加しない。

## Regression / evidence

- `test-completion-schema` / `test-public-documentation`: schemaとhelp/man/static/compatibility projectionのdrift。
- `test-dynamic-completion`: shared package/typed/query/lexical scenarios、helper count、failure fallback、実PTY。
  canonical machine-readable結果はbuild treeの`Testing/completion-semantic-results.json`へ出力する。
  expected値はexisting authorityから導出し、presentationとshell既存matching差を区別する。
- `test-completion-presentation`: semantic invariance、Zsh metadata/native listing、Fish description、raw PTY insertion。
- `test-repository-prefix-helper` / `test-package-metadata` / `test-package-metadata-integration`:
  supervisor、protocol/limits、configuration failure、real libalpm missing/corrupt DB。
- `test-pacman-query-projection`: raw fixture、bounded capture/parser、snapshot failure、collision/reproducibility。
  `test-pacman-query-host`は別の明示的read-only host validationで、通常生成には含めない。

generationとfreshnessを依存順に実行し、同じcandidateに対する再生成はbyte-identicalにする。
latencyはfirst/repeated observationsで記録し、cache coldnessを仮定せず、absolute SLAへ読み替えない。
PR-ready host evidenceは`docs/validation.md`の`test-host-release`であり、final RCとは別epochである。
