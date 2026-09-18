#include "aur_devel_update.hpp"
#include <cstdlib>
namespace aur_devel_update_test_stub {
unsigned registered_calls = 0;
void reset_registered_calls() {
    registered_calls = 0;
}
unsigned registered_call_count() {
    return registered_calls;
}
} // namespace aur_devel_update_test_stub
std::vector<AurDevelUpdateObservation> refine_aur_devel_updates(AurUpdatePlan& plan) {
    const char* value = std::getenv("MOGUET_TEST_INSPECTION_SCENARIO");
    const std::string scenario = value ? value : "";
    if(!scenario.starts_with("foreign-authoritative-")) return {};
    for(auto& entry : plan.entries) {
        if(entry.classification != AurUpdateClassification::UpToDate) continue;
        entry.devel_assessment_origin = AurDevelAssessmentOrigin::CurrentObservation;
        entry.devel_assessment = scenario.ends_with("different") ? DevelUpdateAssessment::update_available() : scenario.ends_with("unknown")      ? DevelUpdateAssessment::unknown(DevelUnknownReason::RemoteObservationFailed)
                                                                                                           : scenario.ends_with("requires-check") ? DevelUpdateAssessment::requires_check(DevelRequiresCheckReason::ProvenanceMissing)
                                                                                                           : scenario.ends_with("unsupported")    ? DevelUpdateAssessment::unsupported(DevelUnsupportedReason::UnsupportedVcs)
                                                                                                                                                  : DevelUpdateAssessment::up_to_date();
    }
    return {};
}
AurUpdateQueryResult query_registered_aur_devel_update(const PackageBaseIdentity& base, const std::string& child) {
    ++aur_devel_update_test_stub::registered_calls;
    AurUpdateQueryResult result{make_aur_update_plan({AurUpdatePlanInput{child, "0-1", InstalledPackageReason::Explicit, AurUpdateRemotePackage{child, base.package_base(), "0-1", AurVersionRelation::SameAsInstalled}}}), {}, {}};
    if(const char* value = std::getenv("MOGUET_TEST_REGISTERED_DEVEL_STATE")) {
        const std::string state = value;
        auto& entry = result.plan.entries.front();
        entry.devel_assessment_origin = AurDevelAssessmentOrigin::CurrentObservation;
        entry.devel_assessment = state == "different" ? DevelUpdateAssessment::update_available() : state == "unknown" ? DevelUpdateAssessment::unknown(DevelUnknownReason::RemoteObservationFailed)
                                                                                                : state == "check"     ? DevelUpdateAssessment::requires_check(DevelRequiresCheckReason::ProvenanceMissing)
                                                                                                                       : DevelUpdateAssessment::up_to_date();
    }
    return result;
}

std::vector<RegisteredAurDevelObservation> observe_registered_aur_devel_updates(const SystemSourceUpgradeProjectionAuthority&) {
    return {};
}

void observe_aur_devel_bootstrap_candidates(AurUpdateQueryResult&, const AppConfig&) {
    // No source/installed authority in this legacy-only query fixture.
}
AurDevelUpdateContextObservation observe_aur_devel_update_context() {
    return {};
}

bool revalidate_devel_tracking_bootstrap(const DevelTrackingBootstrapTrial&) {
    throw std::logic_error("Legacy query fixture has no bootstrap observation authority.");
}
