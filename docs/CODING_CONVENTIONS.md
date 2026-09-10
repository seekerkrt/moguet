# Moguet固有 C++ コーディング規約

## 位置づけと優先順位

C/C++共通規約は`cpp-conventions` Skillを基準とする。
この文書はMoguet固有の追加・上書き規約だけを定める。

矛盾する場合は、この文書と実際に使用されるbuild設定を優先する。ここに書かれていない共通規則は`cpp-conventions`へ従う。

transaction、互換性、project stance等の設計判断は対応する正式docsを正とし、この文書へ詳細を複製しない。

## 言語・実行環境

- CMake target propertyをC++ compile / link authorityとし、hostedなstrict C++20
  （`CXX_STANDARD_REQUIRED=YES`、extensionなし）を使用する。Makefileはcompilerを直接起動しない。
- compiler selectionはdirect CMakeのtoolchain/cache、またはMake / PKGBUILD frontendの外部`CXX`
  inputとして扱う。project側で特定compiler実体へ固定せず、persistent treeではCMake-compatibleに
  分解したcompiler実体とimmutable required argumentsのidentity preflightを維持する。
- standard library、libcurl、libalpmを既存の責務境界に従って利用できる。
- 例外は内部の失敗伝播に使用できる。CLI境界で`std::exception`等を捕捉し、利用者向けerrorと終了codeへ変換する。
- destructorから例外を外へ出さない。cleanup failureを無視する場合は、その契約が重要なら理由を残す。
- RTTI / `dynamic_cast`は現在の設計前提ではない。新しく必要になった場合は、型判別よりinterfaceやdata modelで表せないかを先に確認する。
- `reinterpret_cast`を通常のapplication logicへ導入しない。外部C APIとの境界で必要なら、狭いadapterに閉じて前提を明示する。

## File構成と分割

現在の実装は`source/moguet.cpp`だけの単一構成ではなく、CLI、設定、package metadata、plan、source build、process実行等を責務ごとの`.hpp` / `.cpp`へ分けている。

- 新しい非自明な型や複数箇所から使うinterfaceは、既存moduleと同様に宣言を`.hpp`、定義を`.cpp`へ分ける。
- entry pointとtop-level CLI wiringは`source/moguet.cpp`へ置き、domain実装を戻して肥大化させない。
- 既存の責務pairへ収まる変更では、新しいgeneric moduleやwrapperを増やさない。
- file分割そのものを目的に既存moduleを一括移動しない。
- testは対象moduleと既存`tests/`の構成に対応させる。
- production translation unitは`cmake/MoguetSources.cmake`、test source / stub / definition / include /
  link profileは`cmake/MoguetTestTargets.cmake`を正とし、Makefileやvalidation scriptへ二重列挙しない。

## 命名

- 型、class、struct、`enum class`はPascalCaseを基本とする。
- 新しい`enum class`のvalueもPascalCaseを基本とし、既存enumでは周辺の形式を優先する。
- free function、method、private / internal helper、local variable、argumentは既存コードに合わせてsnake_caseを基本とする。
- 新しいclass memberは`_`接尾辞、単純なdata aggregateのstruct memberは接尾辞なしを基本とする。
- translation-unit localなhelper、定数、補助型は無名namespaceへ閉じる。
- boolは`is_`、`has_`、`should_`、`can_`、`needs_`等、真偽の意味が読める名前を優先する。
- path / directory / file、status / exit_code / failedは値の意味が区別できる名前にする。
- pacman、makepkg、AUR、PKGBUILD、PackageBase等の既存domain用語の表記を揺らさない。
- repository全体をnamespaceや命名へ揃えるだけの変更は行わない。

## Functionとerrorの境界

- metadata取得、dependency計画、表示、fetch、build、install、cleanupを1つの関数へ詰め込まない。
- dry-run / plan生成と副作用実行を別の責務に保つ。
- exceptionを投げる関数、`bool`、`std::optional`、exit codeを返す関数のfailure contractを混ぜない。
- exception messageはCLI境界で利用者へ表示される可能性を前提に、失敗内容と対象を含める。
- network / AUR failure、external command failure、validation failure、internal invariantを同じerror表現へ潰さない。

## pacman・makepkg・git・AUR境界

- Moguetはpacman-first wrapperである。system package transactionやdatabase authorityを独自に持たない。
- libalpmはquery / transaction契約に従って使用し、CLI都合の重複modelを増やさない。
- makepkgのPKGBUILD評価、build、package artifact生成の意味を隠しすぎない。
- git操作はsource取得に必要な範囲へ限定し、fetchとworking tree mutationを分ける。
- AUR metadata、dependency、provider、conflictの事実と、Moguetが作るplanを区別する。
- `${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/`はsource-build preferenceの唯一のruntime authorityであり、意味やlayoutの変更を互換性変更として扱う。`/etc/jpacker`へのfallback、merge、自動migrationを追加しない。

## External command・CLI出力

- external commandはargvを構造として保持できる既存経路を優先する。shell stringが避けられない場合は、既存のquoting方針とvalidation経路に従い、未検証の値を直接連結しない。
- package nameやenvironment keyは、対応する既存validatorを通す。
- sudo pacman、makepkg、git等、利用者に影響する主要commandとoptionは実行前に見える形で表示する。
- Moguet全体をrootで動かす前提にせず、必要なpacman操作だけをsudo境界へ渡す。
- user-facing failureは`Logger::error`、継続可能な注意は`Logger::warn`、通常進行は`Logger::info`という既存区分を維持する。
- CLI option、output wording、machine-consumed output、終了codeを変える場合は、README / man page / testsへの影響を確認する。

## Resource管理

- curl global stateとhandle、file、process result、temporary directory、current directory変更は、既存の`CurlGlobal`、`CurlHandle`、`WorkDirGuard`等のRAII型または一意なownerへ束ねる。
- partial clone / build / install前処理のfailure pathで、どのartifactを残し、どれを掃除するかを明示する。
- rollbackやcleanupで対象pathを組み立てる場合、base directoryと対象identityをvalidationしてから扱う。

## 書式とtool設定

- repository rootの`.clang-format`をMoguetのC/C++ format authorityとする。repository配下で
  `clang-format --style=file`を使うとこの設定を解決し、IDE、AI agent、developerが同じ
  repository-owned設定を参照する。HOME側の個人設定はMoguetの正式authorityとして扱わない。
- repository rootの`.editorconfig`はUTF-8、LF、final newline等の基本editor policyを定義する。
  C/C++の具体的なformat policyは`.clang-format`をauthorityとし、`.editorconfig`では重複定義しない。
- repository rootの`.gitattributes`はtracked textのLF normalizationを定義する。
  binary / special fileに例外が必要になった場合は、実際のtracked contentを根拠に明示する。
- CMakeのproject build contractは`-Wall -Wextra`を含む。新しいwarningを無視するcastやsuppressionを
  安易に追加しない。
- C++実装を終えたら、通常のvalidationより前に次を順に実行する。

      scripts/format-changed-cpp.sh --write
      scripts/format-changed-cpp.sh --check

  helperは`HEAD`に対するindex + working treeの現在差分から、trackedまたはstagedの`.cpp` / `.hpp`
  だけをNUL-safeに選び、repositoryの`.clang-format`を`--style=file`で適用する。untracked C++は
  暗黙に対象とせず、新規fileはtracked / stagedになってからhelperを使うか、untrackedの間は明示的に
  `clang-format --style=file`を実行する。helperは`git add`を行わない。
- changed-file検出が失敗した場合はそこで停止し、repository-wide formatへfallbackしない。
  通常workflowでrepository全体や今回と無関係なC++へ`clang-format -i`を実行しない。
- repository全体を整形する変更は、機能変更やformat authority変更と分けた明示的な作業にする。
  `.clang-format`の追加を既存sourceの一括format許可とは扱わない。

## Project固有の確認入口

- 基本build: `make`
- 全test: `make test`
- 限定test: 対応する`make test-<領域>`
- PR / merge用host gate: `make test-host-release`
- historical release subset: `make release-check`
- preset CMake / CTest: `make cmake-dev-configure`後に`cmake --build build/cmake-testing`、
  `ctest --test-dir build/cmake-testing --output-on-failure`

CLI挙動を変えた場合は対象command、error、終了codeも確認する。pacman / makepkg / sudoを伴うsystem mutationは通常testに含めず、明示された安全な環境とscopeでだけ実施する。
