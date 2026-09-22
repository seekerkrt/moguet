#pragma once

#include "presentation_detail.hpp"
#include "provider_selection.hpp"
#include "user_config.hpp"

#include <memory>
#include <string>

// typed user configとinvocation-only optionを1回の実行で参照する境界。
struct AppConfig {
    UserConfig user_config;
    bool no_confirm = false;
    bool rm_deps = false;
    std::string editor = "nano";
    std::shared_ptr<ProviderSelectionSession> provider_selection;
    ProviderCandidatePresenterFactory provider_candidate_presenter_factory;
    PresentationDetail presentation_detail = PresentationDetail::Normal;
};

// load / composition済みのfinal configをproduction consumer向けに一度だけ束ねる。
AppConfig make_app_config(
    UserConfig final_user_config, bool no_confirm, bool rm_deps,
    PresentationDetail presentation_detail = PresentationDetail::Normal);

// null sessionをempty callbackへ畳み、consumerへownership判定を漏らさない。
ProviderSelectionCallback provider_selection_callback(const AppConfig& config);
