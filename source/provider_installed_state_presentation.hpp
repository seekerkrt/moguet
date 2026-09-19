#pragma once

#include "provider_installed_state.hpp"
#include "provider_selection.hpp"

// phase-local lookupを借用し、installed stateをcandidate presentationだけへ
// 投影する。候補identity、selection result、BuildPlanへstateを返さない。
ProviderCandidatePresenter make_provider_installed_state_candidate_presenter(
    ProviderInstalledStateLookup& lookup,
    PresentationDetail detail = PresentationDetail::Normal);

// production callbackがphaseごとにlookupとpresenterを生成するfactory。
ProviderCandidatePresenterFactory
make_provider_installed_state_candidate_presenter_factory();
