# Delegated query option spelling (Issue #253 Slice 3)

generated Bash / Zsh / Fish completionはexact `-Q`の安全なoption positionに限り、
upstream pacman query helpから抽出したoption tokenを提示する。CLI/runtimeのopen delegated
grammarを閉じず、pacman互換性のauthorityや別parserを作らない。

## Reproducible input

canonical inputは`completions/upstream/pacman-query/`のraw `help.txt`、`version.txt`、
`capture.json`である。これは実`/usr/bin/pacman`の`LC_ALL=C` stdoutを改変せず保存したsnapshotで、
手で選別したoption catalogueやsemantic oracleではない。version outputのcopyright/license noticeも保持する。
初期snapshotはpacman 7.1.0 / libalpm 16.0.1。

通常のgenerator、freshness、build/installはsnapshotを読むだけで、installed pacmanを起動しない。
tracked outputをdeveloper hostのversionへ暗黙に依存させない。snapshotが欠落、不正、hash不一致、
未対応format/versionならgenerationは失敗し、既存tracked outputをfallback catalogueにしない。

明示的な`scripts/pacman_query_completion.py --capture <new-directory>`だけがhostを取得する。
既存directoryへ上書きしない。target pacmanの更新時はoperatorがtarget/system上でraw captureを取得し、
source inputの差分をreviewして明示更新する。package/releaseでdeveloper hostから自動更新しない。
`make test-pacman-query-host`はread-onlyのactual capture/parser確認で、snapshotのtoken projectionとの
一致を検査する。absent executable、capture failure、unsupported major/format、token driftは明確に失敗する。
same-format/versionの違いでも入力identityを表示し、対応範囲全体の互換性を保証したとは扱わない。

## Extraction / budgets

parserはsupported pacman major 7のquery usage headerと固定description column 23を検査し、
syntax columnのsingle-letter short / long tokenと直接のshort-long pairingだけを抽出する。
long-onlyのwrapped descriptionも扱う。prose、operand placeholder、説明内のoption引用は捨てる。
bytewise token/record順序を決定し、duplicate token/alias、ambiguous syntax、unexpected indentation、
control/non-ASCII inputをrejectする。

- inputごと16 KiB、64 option records、128 tokens以内。
- stdout/stderr captureはそれぞれ16 KiB、processごと2秒deadline。
- executableは固定absolute `/usr/bin/pacman`、shell-free argv、`LC_ALL=C`、非特権。
- 不成功、diagnostic、budget超過時はpartial captureをauthorityとしてpublishしない。

arity、enum/default、route compatibility、forwarding/effect、conflict、repeatability、source-build safety、
Moguet ownershipをhelp prose/placeholderから推測しない。

## Moguet precedence / context

CLI exporterの既存OPTIONとdelegated relationが先にauthorityとなる。既知tokenはそのownership、
canonical identity、applicabilityを保つ。例: upstreamにもある`--noconfirm`はMoguet-ownedのまま1候補。
source-build専用の既知tokenが仮にhelpへ現れても、generic pacman tokenへ降格して`-Q`へ加えない。
未定義tokenだけにupstream-delegated categoryを付ける。pairingはspelling identityで、Moguet aliasの
semanticsを増設する意味ではない。既存delegated候補も保持し、tokenごとにdedupする。

initial option positionと、Moguetが既に知るtransparent delegated controls / lexical value optionsの
完成したtailだけを扱う。pending value、`--`、unknown option、bare operand、token-onlyでarity未確認の
新tokenの後は候補を抑制する。後続argvをhelpから推測せずopaque/openとして扱う。
既存LEXICAL_VALUEに載るoptionのseparate valueとlong optionのinline `=value`だけはその既存arityを使う。
short attached/inlineの未投影syntaxはopaqueへ残す。
combined modifiers (`-Qs`等)、他operation、source/AUR routeへ拡大しない。

runtimeはstatic projectionだけでpacman help/versionを呼ばない。package prefix helperも`-Q`では0 calls。
Slice 1bのplain `-S` package provider、Slice 2のfinite valuesは別authorityのまま維持する。
挿入tokenにcategory/prose/placeholderを混ぜない。rich ownership UXはSlice 4。
