# --- Project frontend ---
TARGET := moguet
PACKAGE_NAME := moguet
CMAKE ?= cmake
CTEST ?= ctest
CMAKE_COMPILER_PREFLIGHT := cmake/MoguetCompilerPreflight.cmake
CMAKE_COMPILE_COMMANDS_PUBLISHER := \
	cmake/MoguetPublishCompileCommands.cmake
CMAKE_DEV_CONFIGURE_ARGS ?=
DOCKER ?= docker
ARCH_VALIDATION_IMAGE ?= moguet-arch-validation:local
ARCH_LIVE_VALIDATION_IMAGE ?= moguet-arch-live-validation:local
ARCH_LIVE_AUR_VALIDATION_IMAGE ?= moguet-arch-live-aur-validation:local
ARCH_LIVE_LOCAL_VALIDATION_IMAGE ?= moguet-arch-live-local-validation:local
ARCH_RECEIPT_VALIDATION_IMAGE ?= moguet-arch-receipt-validation:local

VERSION_FILE := VERSION
VERSION := $(strip $(shell cat $(VERSION_FILE) 2>/dev/null))
ifeq ($(VERSION),)
VERSION := unknown
endif

BUILD_DIR := build
CMAKE_PRODUCTION_BUILD_DIR := $(BUILD_DIR)/cmake-production
CMAKE_CTEST_BUILD_DIR := $(BUILD_DIR)/cmake-testing
CMAKE_CTEST_BINARY_DIR := $(CMAKE_CTEST_BUILD_DIR)/tests
CMAKE_CLI_AUTHORITY_EXPORTER := \
	$(CMAKE_CTEST_BUILD_DIR)/moguet-cli-authority-exporter
COMPILE_COMMANDS_LINK := compile_commands.json

# These names are developer-facing compatibility aliases. CMake owns each
# alias's exact build-target and CTest selection mapping.
CMAKE_FOCUSED_ALIASES := \
	test-application-identity \
	test-interactive-confirmation \
	test-localization \
	test-xdg-paths \
	test-xdg-directory-safety \
	test-xdg-generation-store \
	test-xdg-state-log \
	test-trusted-cache \
	test-runtime-identity \
	test-app-config \
	test-provider-selection \
	test-provider-installed-state \
	test-dependency-constraint \
	test-cross-source-version-lock \
	test-package-relation \
	test-package-relation-observation \
	test-package-relation-assessment \
	test-package-constraint-metadata \
	test-aur-constraint-metadata \
	test-root-package-candidate \
	test-root-package-search \
	test-root-package-selection \
	test-root-package-route-projection \
	test-local-package-metadata \
	test-local-source-root \
	test-local-dependency-plan-projection \
	test-local-source-workspace \
	test-local-source-build \
	test-user-config \
	test-package-identifier \
	test-source-package-identity \
	test-installed-artifact-binding \
	test-devel-build-provenance \
	test-devel-build-provenance-store \
	test-git-remote-revision-observer \
	test-source-package-identity-projection \
	test-source-package-compatibility \
	test-invocation-owned-cleanup-model \
	test-invocation-owned-source-build-context \
	test-evaluated-devel-source-build \
	test-evaluated-devel-source-artifact-transport \
	test-remote-aur-cleanup-collector \
	test-source-artifact-install-trusted-transport \
	test-reviewed-source-state \
	test-reviewed-source-state-store \
	test-reviewed-source-lifecycle \
	test-reviewed-source-acceptance \
	test-reviewed-source-pinned-build \
	test-reviewed-source-production-connection \
	test-reviewed-source-projection \
	test-reviewed-source-review \
	test-reviewed-source-patch \
	test-reviewed-source-presentation \
	test-reviewed-source-git \
	test-package-metadata \
	test-package-metadata-integration \
	test-shell-words \
	test-source-environment \
	test-artifact-workspace \
	test-multiple-artifact-workspace \
	test-makepkg-assignment-precedence \
	test-makepkg-devel-phase-characterization \
	test-artifact-identity \
	test-multiple-artifact-identity \
	test-package-base-artifact-install-plan \
	test-artifact-install-executor \
	test-package-base-artifact-install-executor \
	test-separated-source-build \
	test-separated-package-base-source-build \
	test-production-source-build \
	test-process-capture \
	test-bounded-process \
	test-aur-update-plan \
	test-upgrade-all-plan \
	test-system-source-upgrade \
	test-aur-update-query \
	test-aur-update-command \
	test-upgrade-all-command \
	test-aur-update-execution-preflight \
	test-aur-update-execution-preflight-integration \
	test-aur-update-execution-preparation \
	test-aur-update-execution-runner \
	test-aur-update-operation-result \
	test-filtered-aur-update-operation \
	test-upgrade-all-operation \
	test-cli-diagnostic-model \
	test-runtime-cli-connection \
	test-dependency-plan-model \
	test-build-plan-artifact-target-projection \
	test-unified-plan-observation \
	test-observation-contract-gate \
	test-unified-plan-projection \
	test-projection-fixture-gate \
	test-unified-plan-renderer \
	test-repository-query \
	test-artifact-install-plan \
	test-artifact-selection-model \
	test-artifact-identity-selection \
	test-conflicts-replaces \
	test-aur-rpc-validation \
	test-cli-parser \
	test-dry-run-command \
	test-commands-inspect \
	test-commands-source-maintenance \
	test-commands-sync \
	test-pacman-routing \
	test-build-cache-symlink \
	test-source-build \
	test-source-selection \
	test-needed-contract \
	test-pkgbuild-export

# release-check is a historical repository-validation subset. CTest still
# owns runtime registration and execution; this list only requests artifacts
# required by that subset before applying its stable CTest selection.
CMAKE_RELEASE_CTEST_TARGETS := \
	localization-test \
	localization-missing-catalog-test \
	moguet-cli-localization-test \
	application-identity-test \
	xdg-paths-test \
	xdg-directory-safety-test \
	source-environment-test \
	xdg-state-log-test \
	trusted-cache-test \
	moguet \
	moguet-root-execution-identity-test \
	moguet-app-config-test \
	moguet-test \
	moguet-aur-rpc-validation-test

MANPAGE_EN := man/moguet.1
MANPAGE_EN_IN := man/moguet.1.in
MANPAGE_JA := man/ja/moguet.1
MANPAGE_JA_IN := man/ja/moguet.1.in
MANPAGES := $(MANPAGE_EN) $(MANPAGE_JA)
BASH_COMPLETION := completions/moguet.bash
ZSH_COMPLETION := completions/_moguet
FISH_COMPLETION := completions/moguet.fish
COMPLETION_FILES := $(BASH_COMPLETION) $(ZSH_COMPLETION) $(FISH_COMPLETION)

# --- Canonical CMake install destination mapping ---
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec/$(PACKAGE_NAME)
COMPDIR ?= /usr/share/bash-completion/completions
ZSHCOMPDIR ?= /usr/share/zsh/site-functions
FISHCOMPDIR ?= /usr/share/fish/vendor_completions.d
MANDIR ?= $(PREFIX)/share/man/man1
JAMANDIR ?= $(PREFIX)/share/man/ja/man1
LICENSEDIR ?= $(PREFIX)/share/licenses/$(PACKAGE_NAME)
DOCDIR ?= $(PREFIX)/share/doc/$(PACKAGE_NAME)
LOCALEDIR ?= /usr/share/locale

# --- Repository-owned localization and documentation validation ---
GETTEXT_DOMAIN := moguet
PO_DIR := po
POTFILES_FILE := $(PO_DIR)/POTFILES.in
LINGUAS_FILE := $(PO_DIR)/LINGUAS
POT_FILE := $(PO_DIR)/$(GETTEXT_DOMAIN).pot
LINGUAS := $(strip $(shell sed 's/[[:space:]]*\#.*//' $(LINGUAS_FILE) 2>/dev/null))
LOCALE_BUILD_DIR := $(BUILD_DIR)/locale
MO_FILES := $(foreach locale,$(LINGUAS),$(LOCALE_BUILD_DIR)/$(locale)/LC_MESSAGES/$(GETTEXT_DOMAIN).mo)

PROJECT_LICENSE_FILES := \
	LICENSE \
	LICENSES/jpacker-MIT-legacy.txt \
	LICENSES/curl.txt \
	LICENSES/nlohmann-json-MIT.txt \
	LICENSES/tomlplusplus-MIT.txt \
	LICENSES/bjoern-hoehrmann-utf8-MIT.txt
COMPLIANCE_DOC_FILES := \
	THIRD_PARTY_NOTICES.md \
	docs/LICENSING.md
PUBLIC_DOC_FILES := \
	README.md \
	README.ja.md \
	docs/migration/v1-to-v2.md \
	docs/migration/v1-to-v2.ja.md

XGETTEXT ?= xgettext
MSGMERGE ?= msgmerge
MSGCMP ?= msgcmp
MSGFMT ?= msgfmt
MSGGREP ?= msggrep
XGETTEXT_OPTIONS := \
	--language=C++ \
	--from-code=UTF-8 \
	--keyword=translate_message:1 \
	--keyword=translate_plural_message:1,2 \
	--keyword=format_translated_message:1 \
	--keyword=format_translated_plural_message:1,2 \
	--keyword=add_reduction_issue:4 \
	--keyword=add_localized_operation_issue:3 \
	--keyword=make_localized_execution_issue:2 \
	--keyword=make_localized_preparation_issue:2 \
	--keyword=retain_localized_build_unit_selection_issue:3 \
	--flag=format_translated_message:1:c++-format \
	--flag=format_translated_plural_message:1:c++-format \
	--flag=format_translated_plural_message:2:c++-format \
	--add-comments=TRANSLATORS: \
	--package-name=Moguet \
	--package-version=$(VERSION) \
	--no-location \
	--no-wrap

# CXX and the standard toolchain variables are external frontend inputs, not
# project compile/link authority. CMake owns defaults, target options, source
# closure, definitions, includes, and libraries. An undefined CXXFLAGS asks
# CMake for its default; an explicitly empty value remains explicitly empty.
CXX ?= g++
ifeq ($(origin MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS),undefined)
MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS := \
	$(if $(filter undefined,$(origin CXXFLAGS)),ON,OFF)
endif
export CPPFLAGS CXXFLAGS LDFLAGS CCACHE
export MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS

.PHONY: \
	all \
	cmake-production-configure \
	cmake-test-configure \
	cmake-dev-configure \
	cmake-test-build \
	cmake-cli-authority-exporter-build \
	cmake-release-build \
	test-cmake \
	clean \
	install \
	uninstall
.PHONY: \
	catalogs \
	check-catalogs \
	check-localization-config \
	check-pot \
	update-po \
	update-pot
.PHONY: \
	test-repository \
	test-host-release \
	test-release-compat \
	release-check \
	release-check-exclusive \
	test-internal-identity \
	test-cli-localization-surface \
	test-catalog-metadata-gate \
	test-artifact-identity-real-pacman \
	test-command-stub-contract \
	test-cmake-frontend-contract \
	test-build-authority-closure \
	test-validation-status \
	test-format-changed-cpp \
	test-markdown-links \
	test-completion-schema \
	generate-completions \
	check-completion-freshness \
	test-public-documentation \
	test-fixture-authority \
	test-live-contract \
	test-run-with-pty \
	test-install-layout \
	test-package-transition \
	test
.PHONY: \
	test-container \
	test-container-live \
	test-container-live-provider \
	test-container-live-aur \
	test-container-live-local \
	test-container-receipt \
	test-container-cleanup-authority \
	test-container-source-artifact-receipt \
	test-container-installed-binding-characterization
.PHONY: check-reviewed-source-pinned-build-authority $(CMAKE_FOCUSED_ALIASES)

all: $(TARGET) $(MANPAGES)

# CXX initializes a fresh CMake tree. After preflight accepts an equivalent
# spelling, an existing tree keeps its cached CMAKE_CXX_COMPILER value; the
# raw spelling is not re-passed as a cache override.
cmake-production-configure:
	$(CMAKE) \
		"-DMOGUET_COMPILER_PREFLIGHT_BUILD_DIR=$(CMAKE_PRODUCTION_BUILD_DIR)" \
		"-DMOGUET_REQUESTED_CXX=$(CXX)" \
		-P $(CMAKE_COMPILER_PREFLIGHT)
	CXX="$(CXX)" \
	$(CMAKE) -S . -B $(CMAKE_PRODUCTION_BUILD_DIR) \
		"-DCMAKE_INSTALL_PREFIX=$(PREFIX)" \
		"-DCMAKE_INSTALL_BINDIR=$(BINDIR)" \
		"-DMOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY=$(LIBEXECDIR)" \
		"-DMOGUET_INSTALL_BASH_COMPLETION_DIRECTORY=$(COMPDIR)" \
		"-DMOGUET_INSTALL_ZSH_COMPLETION_DIRECTORY=$(ZSHCOMPDIR)" \
		"-DMOGUET_INSTALL_FISH_COMPLETION_DIRECTORY=$(FISHCOMPDIR)" \
		"-DMOGUET_INSTALL_MAN1_DIRECTORY=$(MANDIR)" \
		"-DMOGUET_INSTALL_JA_MAN1_DIRECTORY=$(JAMANDIR)" \
		"-DMOGUET_INSTALL_LICENSE_DIRECTORY=$(LICENSEDIR)" \
		"-DMOGUET_INSTALL_DOCUMENT_DIRECTORY=$(DOCDIR)" \
		"-DMOGUET_LOCALE_DIRECTORY=$(LOCALEDIR)" \
		"-DMOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS=$(MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS)" \
		-DMOGUET_SYNC_EXTERNAL_BUILD_INPUTS=ON \
		-DBUILD_TESTING=OFF
	$(CMAKE) \
		"-DMOGUET_COMPILE_COMMANDS_BUILD_DIR=$(abspath $(CMAKE_PRODUCTION_BUILD_DIR))" \
		-P $(CMAKE_COMPILE_COMMANDS_PUBLISHER)

cmake-test-configure:
	$(CMAKE) \
		"-DMOGUET_COMPILER_PREFLIGHT_BUILD_DIR=$(CMAKE_CTEST_BUILD_DIR)" \
		"-DMOGUET_REQUESTED_CXX=$(CXX)" \
		-P $(CMAKE_COMPILER_PREFLIGHT)
	CXX="$(CXX)" \
	$(CMAKE) -S . -B $(CMAKE_CTEST_BUILD_DIR) \
		"-DMOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS=$(MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS)" \
		-DMOGUET_SYNC_EXTERNAL_BUILD_INPUTS=ON \
		-DBUILD_TESTING=ON
	$(CMAKE) \
		"-DMOGUET_COMPILE_COMMANDS_BUILD_DIR=$(abspath $(CMAKE_CTEST_BUILD_DIR))" \
		-P $(CMAKE_COMPILE_COMMANDS_PUBLISHER)

# CMake 3.19 presets cannot express a post-configure command.  Keep the
# tracked preset as configure authority and publish only after it exits 0.
cmake-dev-configure:
	$(CMAKE) --preset dev-debug -B $(CMAKE_CTEST_BUILD_DIR) $(CMAKE_DEV_CONFIGURE_ARGS)
	$(CMAKE) \
		"-DMOGUET_COMPILE_COMMANDS_BUILD_DIR=$(abspath $(CMAKE_CTEST_BUILD_DIR))" \
		-P $(CMAKE_COMPILE_COMMANDS_PUBLISHER)

cmake-test-build: cmake-test-configure
	+$(CMAKE) --build $(CMAKE_CTEST_BUILD_DIR)

cmake-cli-authority-exporter-build: cmake-test-configure
	+$(CMAKE) --build $(CMAKE_CTEST_BUILD_DIR) \
		--target moguet-cli-authority-exporter

cmake-release-build: cmake-cli-authority-exporter-build
	+$(CMAKE) --build $(CMAKE_CTEST_BUILD_DIR) \
		--target $(CMAKE_RELEASE_CTEST_TARGETS)

test-cmake: cmake-test-build
	$(CTEST) --test-dir $(CMAKE_CTEST_BUILD_DIR) --output-on-failure

# CMake owns the focused target/CTest mapping. This frontend carries no source
# closure, compile definition, include path, link library, or runtime recipe.
$(CMAKE_FOCUSED_ALIASES): cmake-test-configure
	$(CMAKE) --build $(CMAKE_CTEST_BUILD_DIR) \
		--target moguet-focus-$@

check-reviewed-source-pinned-build-authority: cmake-test-configure
	$(CMAKE) --build $(CMAKE_CTEST_BUILD_DIR) \
		--target moguet-focus-test-reviewed-source-pinned-build

$(TARGET): cmake-production-configure
	+$(CMAKE) --build $(CMAKE_PRODUCTION_BUILD_DIR) --target moguet
	$(CMAKE) -E copy_if_different \
		$(CMAKE_PRODUCTION_BUILD_DIR)/moguet $(TARGET)

$(MANPAGE_EN): $(MANPAGE_EN_IN) $(VERSION_FILE)
	@echo ":: Generating $@ (v$(VERSION))"
	sed 's/@VERSION@/$(VERSION)/g' $(MANPAGE_EN_IN) > $@

$(MANPAGE_JA): $(MANPAGE_JA_IN) $(VERSION_FILE)
	@echo ":: Generating $@ (v$(VERSION))"
	sed 's/@VERSION@/$(VERSION)/g' $(MANPAGE_JA_IN) > $@

check-localization-config:
	@case '$(LOCALEDIR)' in \
		/*) ;; \
		*) echo "error: LOCALEDIR must be an absolute path: $(LOCALEDIR)" >&2; exit 1 ;; \
	esac
	@test -n "$(LINGUAS)" || { \
		echo "error: $(LINGUAS_FILE) must declare at least one locale" >&2; \
		exit 1; \
	}
	@set -e; for locale in $(LINGUAS); do \
		case "$$locale" in \
			.|..|*[!A-Za-z0-9_.@-]*) \
				echo "error: invalid locale token in $(LINGUAS_FILE): $$locale" >&2; \
				exit 1 ;; \
		esac; \
		test -f "$(PO_DIR)/$$locale.po" || { \
			echo "error: missing catalog source: $(PO_DIR)/$$locale.po" >&2; \
			exit 1; \
		}; \
	done

catalogs: check-localization-config $(MO_FILES)

$(LOCALE_BUILD_DIR)/%/LC_MESSAGES/$(GETTEXT_DOMAIN).mo: $(PO_DIR)/%.po
	@mkdir -p $(dir $@)
	@echo ":: Compiling message catalog $<"
	@set -eu; \
		tmp_file='$@.tmp'; \
		rm -f "$$tmp_file"; \
		trap 'rm -f "$$tmp_file"' EXIT HUP INT TERM; \
		$(MSGFMT) --check --check-format --check-domain \
			--output-file="$$tmp_file" "$<"; \
		mv "$$tmp_file" "$@"

check-catalogs: check-localization-config $(POT_FILE)
	@mkdir -p $(BUILD_DIR)/po-check/catalogs
	@set -eu; \
		metadata_dir="$(BUILD_DIR)/po-check/catalogs"; \
		pot_utf8="$$metadata_dir/$(GETTEXT_DOMAIN)-utf8.pot"; \
		pot_format_messages="$$metadata_dir/$(GETTEXT_DOMAIN)-c++-format.pot"; \
		po_format_messages=; \
		trap 'rm -f "$$pot_utf8" "$$pot_format_messages" "$$po_format_messages"' EXIT HUP INT TERM; \
		sed '/^"Content-Type: /s/charset=CHARSET/charset=UTF-8/' \
			"$(POT_FILE)" > "$$pot_utf8"; \
		$(MSGGREP) --sticky-flag=c++-format --force-po --no-location --no-wrap \
			--output-file="$$pot_format_messages" "$$pot_utf8"; \
		for locale in $(LINGUAS); do \
			po_file="$(PO_DIR)/$$locale.po"; \
			if ! $(MSGCMP) --no-fuzzy-matching \
					"$$po_file" "$$pot_utf8"; then \
				echo "error: $$po_file has untranslated or fuzzy messages required by $(POT_FILE); run 'make update-po' and complete the translations" >&2; \
				exit 1; \
			fi; \
			po_format_messages="$$metadata_dir/$$locale-c++-format.po"; \
			rm -f "$$po_format_messages"; \
			$(MSGGREP) --sticky-flag=c++-format --force-po --no-location --no-wrap \
				--output-file="$$po_format_messages" "$$po_file"; \
			if ! $(MSGCMP) --no-fuzzy-matching --use-fuzzy --use-untranslated \
					"$$po_format_messages" "$$pot_format_messages"; then \
				echo "error: c++-format metadata missing from $$po_file for messages required by $(POT_FILE); run 'make update-po'" >&2; \
				exit 1; \
			fi; \
			if ! $(MSGCMP) --no-fuzzy-matching --use-fuzzy --use-untranslated \
					"$$pot_format_messages" "$$po_format_messages"; then \
				echo "error: unexpected c++-format metadata in $$po_file compared with $(POT_FILE); run 'make update-po'" >&2; \
				exit 1; \
			fi; \
			rm -f "$$po_format_messages"; \
			po_format_messages=; \
			$(MSGFMT) --check --check-format --check-domain \
				--output-file=/dev/null "$$po_file"; \
		done

update-pot: $(POTFILES_FILE) $(VERSION_FILE)
	@mkdir -p $(BUILD_DIR)/po
	@echo ":: Updating $(POT_FILE)"
	@set -eu; \
		tmp_file='$(BUILD_DIR)/po/$(GETTEXT_DOMAIN).pot.tmp'; \
		rm -f "$$tmp_file"; \
		trap 'rm -f "$$tmp_file"' EXIT HUP INT TERM; \
		$(XGETTEXT) $(XGETTEXT_OPTIONS) \
			--files-from=$(POTFILES_FILE) --output="$$tmp_file"; \
		mv "$$tmp_file" "$(POT_FILE)"

update-po: update-pot check-localization-config
	@set -e; for locale in $(LINGUAS); do \
		$(MSGMERGE) --update --backup=none --no-wrap \
			"$(PO_DIR)/$$locale.po" "$(POT_FILE)"; \
	done

check-pot: $(POT_FILE) $(POTFILES_FILE) $(VERSION_FILE)
	@mkdir -p $(BUILD_DIR)/po-check
	@echo ":: Checking $(POT_FILE) extraction drift"
	@set -eu; \
		candidate='$(BUILD_DIR)/po-check/$(GETTEXT_DOMAIN).pot'; \
		candidate_normalized="$$candidate.normalized"; \
		tracked_normalized='$(BUILD_DIR)/po-check/tracked-$(GETTEXT_DOMAIN).pot.normalized'; \
		$(XGETTEXT) $(XGETTEXT_OPTIONS) \
			--files-from=$(POTFILES_FILE) --output="$$candidate"; \
		sed '/^"POT-Creation-Date:/d' "$$candidate" > "$$candidate_normalized"; \
		sed '/^"POT-Creation-Date:/d' "$(POT_FILE)" > "$$tracked_normalized"; \
		if ! cmp -s "$$tracked_normalized" "$$candidate_normalized"; then \
			diff -u "$$tracked_normalized" "$$candidate_normalized" || true; \
			echo "error: $(POT_FILE) is stale; run 'make update-pot'" >&2; \
			exit 1; \
		fi

clean:
	@echo ":: Cleaning up"
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET)
	rm -f $(COMPILE_COMMANDS_LINK)

test-internal-identity: $(MANPAGES)
	python3 scripts/check-internal-identity.py

test-cli-localization-surface: check-pot $(POT_FILE) $(POTFILES_FILE) scripts/check-cli-localization-surface.py
	python3 scripts/check-cli-localization-surface.py

test-catalog-metadata-gate: $(PO_DIR)/ja.po $(POT_FILE) $(LINGUAS_FILE) $(POTFILES_FILE)
	sh tests/test-catalog-metadata-gate.sh \
		"$(MAKE)" \
		"$(abspath .)" \
		"$(abspath $(PO_DIR))" \
		"$(XGETTEXT)" \
		"$(MSGCMP)" \
		"$(MSGFMT)" \
		"$(MSGGREP)"

test-artifact-identity-real-pacman:
	sh tests/test-artifact-identity-real-pacman.sh

test-command-stub-contract:
	sh tests/test-command-stub-contract.sh

test-cmake-frontend-contract: \
	test-public-documentation \
	test-install-layout \
	test-package-transition \
	test-build-authority-closure
	sh tests/test-cmake-frontend-contract.sh "$(CMAKE)"

test-build-authority-closure: cmake-test-configure
	sh tests/test-build-authority-closure.sh \
		"$(CMAKE)" \
		"$(CMAKE_CTEST_BUILD_DIR)" \
		$(CMAKE_FOCUSED_ALIASES)

test-validation-status:
	sh tests/test-validation-status.sh

test-format-changed-cpp: \
	scripts/format-changed-cpp.sh \
	tests/test-format-changed-cpp.sh \
	.clang-format
	bash tests/test-format-changed-cpp.sh \
		"$(abspath scripts/format-changed-cpp.sh)"

test-markdown-links:
	sh tests/test-markdown-links.sh \
		$(abspath scripts/check-markdown-links.sh)

test-completion-schema: cmake-cli-authority-exporter-build scripts/generate_completions.py tests/test-completion-schema-validator.py
	MOGUET_CLI_AUTHORITY_EXPORTER="$(abspath $(CMAKE_CLI_AUTHORITY_EXPORTER))" \
	PYTHONDONTWRITEBYTECODE=1 \
	python3 tests/test-completion-schema-validator.py

generate-completions: cmake-test-configure
	+$(CMAKE) --build $(CMAKE_CTEST_BUILD_DIR) \
		--target moguet-generate-completions

check-completion-freshness: cmake-cli-authority-exporter-build
	MOGUET_CLI_AUTHORITY_EXPORTER="$(abspath $(CMAKE_CLI_AUTHORITY_EXPORTER))" \
	PYTHONDONTWRITEBYTECODE=1 \
	python3 scripts/generate_completions.py --check

test-public-documentation: cmake-release-build check-completion-freshness test-completion-schema $(MANPAGES) $(COMPLETION_FILES) tests/test-help-man-completion.sh tests/test-public-documentation-checker.py tests/test-static-completion.sh
	MOGUET_CLI_AUTHORITY_EXPORTER="$(abspath $(CMAKE_CLI_AUTHORITY_EXPORTER))" \
	PYTHONDONTWRITEBYTECODE=1 \
	python3 tests/test-public-documentation-checker.py
	MOGUET_CLI_AUTHORITY_EXPORTER="$(abspath $(CMAKE_CLI_AUTHORITY_EXPORTER))" \
	sh tests/test-help-man-completion.sh \
		$(abspath $(CMAKE_CTEST_BINARY_DIR)/moguet-cli-localization-test)
	bash tests/test-static-completion.sh $(abspath $(BASH_COMPLETION))

test-fixture-authority:
	sh tests/test-fixture-authority.sh

test-live-contract:
	sh tests/test-live-contract.sh

test-run-with-pty:
	sh tests/test-run-with-pty.sh

test-install-layout: cmake-cli-authority-exporter-build $(TARGET) $(MANPAGES) $(COMPLETION_FILES) $(PROJECT_LICENSE_FILES) $(COMPLIANCE_DOC_FILES) $(PUBLIC_DOC_FILES)
	sh tests/test-install-layout.sh

test-package-transition: cmake-cli-authority-exporter-build $(TARGET) $(MANPAGES) $(COMPLETION_FILES) $(PROJECT_LICENSE_FILES) $(COMPLIANCE_DOC_FILES) $(PUBLIC_DOC_FILES)
	sh tests/test-package-transition.sh

test-container:
	@set -eu; \
		if ! git -C "$(CURDIR)" rev-parse --verify \
			'refs/tags/v1.16.0^{commit}' >/dev/null 2>&1; then \
			printf '%s\n' \
				'error: local tag v1.16.0 is required for container validation' >&2; \
			exit 1; \
		fi; \
		fixture_dir=$$(mktemp -d); \
		legacy_archive=$$fixture_dir/jpacker-v1.16.0-source.tar; \
		cleanup() { \
			rm -f -- "$$legacy_archive"; \
			rmdir -- "$$fixture_dir"; \
		}; \
		trap cleanup EXIT; \
		trap 'exit 129' HUP; \
		trap 'exit 130' INT; \
		trap 'exit 143' TERM; \
		git -C "$(CURDIR)" archive --format=tar \
			--output="$$legacy_archive" v1.16.0; \
		printf '%s\n' ':: Building Arch validation image'; \
		$(DOCKER) build --pull \
			--build-context "moguet-transition-fixture=$$fixture_dir" \
			--tag "$(ARCH_VALIDATION_IMAGE)" \
			--file containers/arch-validation/Dockerfile \
			.; \
		printf '%s\n' ':: Running Arch validation container'; \
		$(DOCKER) run --rm --network=none "$(ARCH_VALIDATION_IMAGE)"

test-container-live-provider:
	@set -eu; \
		printf '%s\n' ':: Building Arch live provider-validation image'; \
		$(DOCKER) build --pull \
			--tag "$(ARCH_LIVE_VALIDATION_IMAGE)" \
			--file containers/arch-live-validation/Dockerfile \
			.; \
		printf '%s\n' ':: Running Arch live provider-validation container'; \
		$(DOCKER) run --rm "$(ARCH_LIVE_VALIDATION_IMAGE)"

test-container-live-aur:
	@set -eu; \
		printf '%s\n' ':: Building Arch live AUR-validation image'; \
		$(DOCKER) build --pull \
			--tag "$(ARCH_LIVE_AUR_VALIDATION_IMAGE)" \
			--file containers/arch-live-validation/Dockerfile.aur \
			.; \
		printf '%s\n' ':: Running Arch live AUR-validation container'; \
		$(DOCKER) run --rm "$(ARCH_LIVE_AUR_VALIDATION_IMAGE)"

test-container-live-local:
	@set -eu; \
		printf '%s\n' ':: Building Arch live local-PKGBUILD validation image'; \
		$(DOCKER) build --pull \
			--tag "$(ARCH_LIVE_LOCAL_VALIDATION_IMAGE)" \
			--file containers/arch-live-validation/Dockerfile.local \
			.; \
		printf '%s\n' ':: Running Arch live local-PKGBUILD validation container'; \
		$(DOCKER) run --rm "$(ARCH_LIVE_LOCAL_VALIDATION_IMAGE)"

test-container-receipt:
	@set -eu; \
		printf '%s\n' ':: Building trusted ALPM receipt validation image'; \
		$(DOCKER) build --network=none \
			--tag "$(ARCH_RECEIPT_VALIDATION_IMAGE)" \
			--file containers/arch-receipt-validation/Dockerfile \
			.; \
		printf '%s\n' ':: Running trusted ALPM receipt validation container'; \
		$(DOCKER) run --rm --network=none \
			"$(ARCH_RECEIPT_VALIDATION_IMAGE)"

test-container-source-artifact-receipt:
	@set -eu; \
		printf '%s\n' ':: Building source-artifact receipt validation image'; \
		$(DOCKER) build --network=none \
			--tag "$(ARCH_RECEIPT_VALIDATION_IMAGE)" \
			--file containers/arch-receipt-validation/Dockerfile \
			.; \
		printf '%s\n' ':: Running source-artifact receipt validation container'; \
		$(DOCKER) run --rm --network=none \
			"$(ARCH_RECEIPT_VALIDATION_IMAGE)" \
			/usr/bin/python3 \
			containers/arch-receipt-validation/run-installed-source-artifact-receipt.py

test-container-installed-binding-characterization:
	@set -eu; \
		printf '%s\n' ':: Building installed-binding characterization image'; \
		$(DOCKER) build --network=none \
			--tag "$(ARCH_RECEIPT_VALIDATION_IMAGE)" \
			--file containers/arch-receipt-validation/Dockerfile \
			.; \
		printf '%s\n' ':: Running installed-binding characterization container'; \
		$(DOCKER) run --rm --network=none \
			--mount type=volume,destination=/var/lib/moguet-installed-binding-characterization,volume-nocopy \
			"$(ARCH_RECEIPT_VALIDATION_IMAGE)" \
			/usr/bin/python3 \
			containers/arch-receipt-validation/run-installed-binding-characterization.py

test-container-cleanup-authority:
	@set -eu; \
		printf '%s\n' ':: Building closed cleanup-authority validation image'; \
		$(DOCKER) build --network=none \
			--tag "$(ARCH_RECEIPT_VALIDATION_IMAGE)" \
			--file containers/arch-receipt-validation/Dockerfile \
			.; \
		printf '%s\n' ':: Running closed cleanup-authority positive and installed transport matrix'; \
		$(DOCKER) run --rm --network=none \
			"$(ARCH_RECEIPT_VALIDATION_IMAGE)" \
			/usr/bin/python3 \
			containers/arch-receipt-validation/run-installed-source-artifact-receipt.py; \
		for scenario in later-failed later-not-attempted; do \
			printf '%s\n' ":: Running closed cleanup-authority validation container: $$scenario"; \
			$(DOCKER) run --rm --network=none \
				"$(ARCH_RECEIPT_VALIDATION_IMAGE)" \
				/usr/bin/python3 \
				containers/arch-receipt-validation/run-installed-source-artifact-receipt.py \
				--cleanup-authority-scenario "$$scenario"; \
		done

test-container-live:
	+@set -eu; \
		$(MAKE) test-container-live-provider; \
		$(MAKE) test-container-live-aur; \
		$(MAKE) test-container-live-local

test-repository: \
	check-pot \
	check-catalogs \
	test-catalog-metadata-gate \
	test-cli-localization-surface \
	test-internal-identity \
	test-artifact-identity-real-pacman \
	test-command-stub-contract \
	test-cmake-frontend-contract \
	test-build-authority-closure \
	test-validation-status \
	test-format-changed-cpp \
	test-markdown-links \
	test-public-documentation \
	test-fixture-authority \
	test-live-contract \
	test-run-with-pty \
	test-install-layout \
	test-package-transition

test:
	+$(MAKE) --no-print-directory test-cmake
	+$(MAKE) --no-print-directory test-repository

test-host-release: test
	+@$(MAKE) --no-print-directory release-check-exclusive

test-release-compat: cmake-release-build
	$(CTEST) --test-dir $(CMAKE_CTEST_BUILD_DIR) --output-on-failure \
		--tests-regex '^(localization\.contract|cpp\.(application_identity|xdg_paths|xdg_directory_safety|source_environment|xdg_state_log|trusted_cache)|cli\.(runtime_identity|dry_run))$$'

release-check: check-pot check-catalogs test-catalog-metadata-gate test-cli-localization-surface test-internal-identity test-release-compat test-public-documentation test-install-layout test-package-transition test-fixture-authority test-live-contract test-validation-status
	+@$(MAKE) --no-print-directory release-check-exclusive

release-check-exclusive:
	@echo ":: Checking release version consistency"
	sh scripts/check-release-version.sh
	@echo ":: Checking license compliance"
	sh scripts/check-license-compliance.sh
	@echo ":: Checking packaging metadata and payload"
	sh scripts/check-packaging-metadata.sh
	@echo ":: Checking tracked Markdown links"
	sh scripts/check-markdown-links.sh

install: check-completion-freshness $(TARGET) $(MANPAGES) $(COMPLETION_FILES) $(PROJECT_LICENSE_FILES) $(COMPLIANCE_DOC_FILES) $(PUBLIC_DOC_FILES)
	@echo ":: Installing canonical CMake payload..."
	DESTDIR="$(DESTDIR)" $(CMAKE) --install $(CMAKE_PRODUCTION_BUILD_DIR)

uninstall: cmake-production-configure
	@echo ":: Removing canonical CMake payload..."
	DESTDIR="$(DESTDIR)" $(CMAKE) --build $(CMAKE_PRODUCTION_BUILD_DIR) \
		--target uninstall
