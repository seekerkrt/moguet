#include "package_relation_presentation.hpp"

#include "localization.hpp"

#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view AUR_SOURCE_NAME = "AUR";
constexpr std::string_view PACKAGE_BASE_FIELD_NAME = "PackageBase";

std::string observed_value_display(std::string_view value) {
    return value.empty() ? localization::translate_message("not observed")
                         : std::string(value);
}

std::string join_display_values(
    const std::vector<std::string>& values, std::string_view separator) {
    std::ostringstream joined;
    for(std::size_t index = 0; index < values.size(); ++index) {
        if(index != 0) joined << separator;
        joined << values[index];
    }
    return joined.str();
}

std::string relation_kind_display(PackageRelationKind kind) {
    switch(kind) {
        case PackageRelationKind::Conflict:
            return localization::translate_message("conflict");
        case PackageRelationKind::Replacement:
            return localization::translate_message("replacement");
    }
    return localization::translate_message("invalid relation kind");
}

std::string observation_role_display(PackageRelationObservationRole role) {
    switch(role) {
        case PackageRelationObservationRole::Installed:
            return localization::translate_message("installed");
        case PackageRelationObservationRole::PlannedTarget:
            return localization::translate_message("planned");
        case PackageRelationObservationRole::RepositoryCandidate:
            return localization::translate_message("repository-context");
    }
    return localization::translate_message("unknown-role");
}

std::string observation_completeness_display(
    PackageRelationObservationCompleteness completeness) {
    switch(completeness) {
        case PackageRelationObservationCompleteness::Complete:
            return localization::translate_message("complete");
        case PackageRelationObservationCompleteness::Partial:
            return localization::translate_message("partial");
        case PackageRelationObservationCompleteness::Unavailable:
            return localization::translate_message("unavailable");
        case PackageRelationObservationCompleteness::Invalid:
            return localization::translate_message("invalid");
    }
    return localization::translate_message("invalid");
}

std::string source_identity_display(
    const PackageRelationSourceIdentity& source) {
    return std::visit(
        [](const auto& identity) -> std::string {
            using Identity = std::decay_t<decltype(identity)>;
            if constexpr(std::is_same_v<
                             Identity,
                             PackageRelationInstalledDatabaseIdentity>) {
                return localization::translate_message(
                    "installed package database");
            } else if constexpr(std::is_same_v<
                                    Identity,
                                    ConfiguredRepositoryIdentity>) {
                return localization::format_translated_message(
                    "configured repository {} (order #{})",
                    observed_value_display(identity.repository_name),
                    identity.configured_order + 1);
            } else if constexpr(std::is_same_v<
                                    Identity,
                                    PackageRelationAurSourceIdentity>) {
                return localization::format_translated_message(
                    // TRANSLATORS: The first and third placeholders are
                    // the literal source and package-base field names.
                    "{} source {} ({}: {})", AUR_SOURCE_NAME,
                    observed_value_display(identity.package_name),
                    PACKAGE_BASE_FIELD_NAME,
                    observed_value_display(identity.package_base));
            } else {
                return localization::translate_message("local source");
            }
        },
        source);
}

std::string root_attribution_display(
    const std::vector<PackageRelationRootAttribution>& roots) {
    std::vector<std::string> values;
    values.reserve(roots.size());
    for(const auto& root : roots) {
        values.push_back(localization::format_translated_message(
            "input #{} requested {}", root.invocation_index + 1,
            observed_value_display(root.requested_name)));
    }
    return join_display_values(values, ", ");
}

std::string package_identity_display(
    const PackageRelationObservedPackage& package) {
    const std::string package_name =
        observed_value_display(package.package_name);
    if(!package.package_base.has_value() ||
       package.package_base->empty() ||
       package.package_base.value() == package.package_name) {
        return package_name;
    }
    return localization::format_translated_message(
        // TRANSLATORS: The second placeholder is the literal package-base
        // field name.
        "{} ({}: {})", package_name, PACKAGE_BASE_FIELD_NAME,
        package.package_base.value());
}

std::string package_context_display(
    const PackageRelationObservedPackage& package) {
    std::vector<std::string> context = {
        localization::format_translated_message(
            "source: {}", source_identity_display(package.source))};
    if(!package.roots.empty()) {
        context.push_back(localization::format_translated_message(
            "roots: {}", root_attribution_display(package.roots)));
    }
    return localization::format_translated_message(
        "{} [{}]", package_identity_display(package),
        join_display_values(context, "; "));
}

std::string matched_component_display(
    const PackageRelationMatchEvidence& evidence,
    std::string_view target_component) {
    switch(evidence.identity_match) {
        case PackageRelationIdentityMatchKind::ExactPackage: {
            const std::string* version =
                evidence.observed_package.package_version.version();
            return version == nullptr
                       ? localization::format_translated_message(
                             "exact package component {} (version unavailable)",
                             observed_value_display(
                                 evidence.observed_package.package_name))
                       : localization::format_translated_message(
                             "exact package component {} (version {})",
                             observed_value_display(
                                 evidence.observed_package.package_name),
                             *version);
        }
        case PackageRelationIdentityMatchKind::ProvidedComponent:
            for(const auto& provided :
                evidence.provided_capability_evidence) {
                if(provided.provided_capability_index >=
                   evidence.observed_package.provides.size()) {
                    continue;
                }
                return localization::format_translated_message(
                    "provided component {}",
                    evidence.observed_package
                        .provides[provided.provided_capability_index]
                        .capability.raw_specification());
            }
            return localization::format_translated_message(
                "provided component {}",
                observed_value_display(target_component));
        case PackageRelationIdentityMatchKind::NoIdentityMatch:
            return localization::translate_message(
                "no matching component evidence");
        case PackageRelationIdentityMatchKind::InvalidInput:
            return localization::translate_message(
                "invalid component evidence");
    }
    return localization::translate_message("invalid component evidence");
}

std::string observation_failure_kind_display(
    PackageRelationObservationFailureKind kind) {
    switch(kind) {
        case PackageRelationObservationFailureKind::SourceUnavailable:
            return localization::translate_message("source unavailable");
        case PackageRelationObservationFailureKind::PartialSourceFailure:
            return localization::translate_message(
                "source observation incomplete");
        case PackageRelationObservationFailureKind::InvalidIdentity:
            return localization::translate_message("source identity invalid");
        case PackageRelationObservationFailureKind::MalformedMetadata:
            return localization::translate_message("metadata malformed");
    }
    return localization::translate_message("metadata malformed");
}

std::string match_invalid_reason_display(
    PackageRelationMatchInvalidReason reason) {
    switch(reason) {
        case PackageRelationMatchInvalidReason::InvalidDeclarationTarget:
            return localization::translate_message(
                "declaration target is invalid");
        case PackageRelationMatchInvalidReason::InvalidPackageIdentity:
            return localization::translate_message("package identity is invalid");
        case PackageRelationMatchInvalidReason::SourceRoleMismatch:
            return localization::translate_message("source role is inconsistent");
        case PackageRelationMatchInvalidReason::SourceIdentityMismatch:
            return localization::translate_message(
                "source identity is inconsistent");
        case PackageRelationMatchInvalidReason::MissingPackageBase:
            return localization::format_translated_message(
                "{} is missing", PACKAGE_BASE_FIELD_NAME);
        case PackageRelationMatchInvalidReason::InvalidPackageBase:
            return localization::format_translated_message(
                "{} is invalid", PACKAGE_BASE_FIELD_NAME);
        case PackageRelationMatchInvalidReason::VersionSourceMismatch:
            return localization::translate_message(
                "version source is inconsistent");
        case PackageRelationMatchInvalidReason::MalformedCapability:
            return localization::translate_message(
                "provided component is malformed");
        case PackageRelationMatchInvalidReason::InvalidVersionMetadata:
            return localization::translate_message("version metadata is invalid");
        case PackageRelationMatchInvalidReason::InvalidRootAttribution:
            return localization::translate_message("root attribution is invalid");
    }
    return localization::translate_message("observation is invalid");
}

std::string assessment_detail_display(
    const PackageRelationAssessment& assessment) {
    if(assessment.attributed_observation_failure.has_value()) {
        const PackageRelationObservationFailure& failure =
            assessment.attributed_observation_failure.value();
        const std::string kind =
            observation_failure_kind_display(failure.kind);
        if(failure.diagnostic.empty()) return kind;
        return localization::format_translated_message(
            "{}: {}", kind, failure.diagnostic);
    }
    if(assessment.attributed_package_evidence.has_value()) {
        const PackageRelationMatchEvidence& evidence =
            assessment.attributed_package_evidence.value();
        if(evidence.invalid_reason.has_value()) {
            return localization::format_translated_message(
                "package {}: {}",
                package_context_display(evidence.observed_package),
                match_invalid_reason_display(
                    evidence.invalid_reason.value()));
        }
        if(evidence.version_match ==
           PackageRelationVersionMatchKind::Unavailable) {
            return localization::format_translated_message(
                "version judgment unavailable for package {} through {}",
                package_context_display(evidence.observed_package),
                matched_component_display(
                    evidence,
                    assessment.declaration.target_component()));
        }
        if(evidence.version_match ==
           PackageRelationVersionMatchKind::Invalid) {
            return localization::format_translated_message(
                "version evidence invalid for package {} through {}",
                package_context_display(evidence.observed_package),
                matched_component_display(
                    evidence,
                    assessment.declaration.target_component()));
        }
    }
    return localization::format_translated_message(
        "observation completeness: {}",
        observation_completeness_display(
            assessment.active_evidence.observation_completeness));
}

std::string matched_package_context_display(
    const PackageRelationAssessment& assessment) {
    if(!assessment.attributed_package_evidence.has_value()) {
        return localization::translate_message("not observed");
    }
    return package_context_display(
        assessment.attributed_package_evidence->observed_package);
}

std::string matched_package_role_display(
    const PackageRelationAssessment& assessment) {
    if(!assessment.attributed_package_evidence.has_value()) {
        return localization::translate_message("current or planned");
    }
    return observation_role_display(
        assessment.attributed_package_evidence->observed_package.role);
}

std::string matched_package_component_display(
    const PackageRelationAssessment& assessment) {
    if(!assessment.attributed_package_evidence.has_value()) {
        return localization::translate_message("not observed");
    }
    return matched_component_display(
        assessment.attributed_package_evidence.value(),
        assessment.declaration.target_component());
}

} // namespace

std::string package_relation_assessment_summary_display(
    const PackageRelationAssessment& assessment) {
    const std::string relation = localization::format_translated_message(
        "{}: {} -> {}", relation_kind_display(assessment.declaration.kind()),
        package_identity_display(assessment.declaring_package),
        observed_value_display(assessment.declaration.raw_specification()));
    const std::string matched = assessment.attributed_package_evidence.has_value()
                                    ? package_identity_display(assessment.attributed_package_evidence->observed_package)
                                    : localization::translate_message("not observed");
    switch(assessment.kind) {
        case PackageRelationAssessmentKind::ConfirmedInstalledConflict:
            return localization::format_translated_message(
                "{}; installed conflict with {}. Build/install is blocked.",
                relation, matched);
        case PackageRelationAssessmentKind::ConfirmedPlannedTargetConflict:
            return localization::format_translated_message(
                "{}; planned conflict with {}. Build/install is blocked.",
                relation, matched);
        case PackageRelationAssessmentKind::PotentialReplacement:
            return localization::format_translated_message(
                "{}; potential replacement of {}. Review required; no automatic replacement. Build/install is blocked.",
                relation, matched);
        case PackageRelationAssessmentKind::ConfirmedNoMatchingCurrentOrPlannedTarget:
            return localization::format_translated_message(
                "{}; no matching installed or planned target.", relation);
        case PackageRelationAssessmentKind::Unknown:
        case PackageRelationAssessmentKind::Invalid: {
            std::string reason = observation_completeness_display(
                assessment.active_evidence.observation_completeness);
            if(assessment.attributed_observation_failure.has_value()) {
                const auto& failure = *assessment.attributed_observation_failure;
                reason = localization::format_translated_message(
                    "{}: {}", observation_role_display(failure.role),
                    observation_failure_kind_display(failure.kind));
            } else if(assessment.attributed_package_evidence.has_value() &&
                      assessment.attributed_package_evidence->invalid_reason.has_value()) {
                reason = match_invalid_reason_display(
                    *assessment.attributed_package_evidence->invalid_reason);
            } else if(assessment.attributed_package_evidence.has_value()) {
                const auto& evidence = *assessment.attributed_package_evidence;
                if(evidence.version_match == PackageRelationVersionMatchKind::Unavailable) {
                    reason = localization::format_translated_message(
                        "version judgment unavailable for {}", matched);
                } else if(evidence.version_match == PackageRelationVersionMatchKind::Invalid) {
                    reason = localization::format_translated_message(
                        "version evidence invalid for {}", matched);
                }
            }
            if(assessment.kind == PackageRelationAssessmentKind::Invalid) {
                return localization::format_translated_message(
                    "{}; invalid relation metadata or observation ({}). Build/install is blocked.",
                    relation, reason);
            }
            return localization::format_translated_message(
                "{}; relation judgment unavailable ({}). Build/install is blocked.",
                relation, reason);
        }
        case PackageRelationAssessmentKind::DeclaredRelation:
            return localization::format_translated_message(
                "{}; assessment incomplete. Build/install is blocked.", relation);
    }
    return localization::translate_message("Invalid relation metadata or observation.");
}

std::string package_relation_assessment_diagnostic_display(
    const PackageRelationAssessment& assessment) {
    const std::string declaring_package =
        package_context_display(assessment.declaring_package);
    const std::string raw_relation = observed_value_display(
        assessment.declaration.raw_specification());
    const std::string target_component = observed_value_display(
        assessment.declaration.target_component());

    switch(assessment.kind) {
        case PackageRelationAssessmentKind::ConfirmedInstalledConflict:
            return localization::format_translated_message(
                "Installed conflict confirmed: declaring package {} declares conflict {} for target component {}; matched installed package {} through {}; build/install is blocked before mutation.",
                declaring_package, raw_relation, target_component,
                matched_package_context_display(assessment),
                matched_package_component_display(assessment));
        case PackageRelationAssessmentKind::ConfirmedPlannedTargetConflict:
            return localization::format_translated_message(
                "Planned-target conflict confirmed: declaring package {} declares conflict {} for target component {}; matched planned package {} through {}; build/install is blocked before mutation.",
                declaring_package, raw_relation, target_component,
                matched_package_context_display(assessment),
                matched_package_component_display(assessment));
        case PackageRelationAssessmentKind::PotentialReplacement:
            return localization::format_translated_message(
                "Potential replacement impact: declaring package {} declares replacement {} for target component {}; matched {} package {} through {} is a replacement candidate; review is required and no automatic replacement is performed; build/install is blocked before mutation.",
                declaring_package, raw_relation, target_component,
                matched_package_role_display(assessment),
                matched_package_context_display(assessment),
                matched_package_component_display(assessment));
        case PackageRelationAssessmentKind::
            ConfirmedNoMatchingCurrentOrPlannedTarget:
            return localization::format_translated_message(
                "Confirmed no matching current or planned target: declaring package {} declares {} {} for target component {}; complete current/planned observation found no matching package or provided component; this relation does not block build/install.",
                declaring_package,
                relation_kind_display(assessment.declaration.kind()),
                raw_relation, target_component);
        case PackageRelationAssessmentKind::Unknown:
            return localization::format_translated_message(
                "Relation judgment unavailable: declaring package {} declares {} {} for target component {}; current/planned observation is {}; {}; this is not a confirmed absence, so build/install is blocked.",
                declaring_package,
                relation_kind_display(assessment.declaration.kind()),
                raw_relation, target_component,
                observation_completeness_display(
                    assessment.active_evidence.observation_completeness),
                assessment_detail_display(assessment));
        case PackageRelationAssessmentKind::Invalid:
            return localization::format_translated_message(
                "Invalid relation metadata or observation: declaring package {} declares {} {} for target component {}; {}; invalid input is fail-closed, so build/install is blocked.",
                declaring_package,
                relation_kind_display(assessment.declaration.kind()),
                raw_relation, target_component,
                assessment_detail_display(assessment));
        case PackageRelationAssessmentKind::DeclaredRelation:
            return localization::format_translated_message(
                "Declared relation awaiting assessment: declaring package {} declares {} {} for target component {}; current/planned assessment is incomplete, so build/install remains blocked under the fail-closed policy.",
                declaring_package,
                relation_kind_display(assessment.declaration.kind()),
                raw_relation, target_component);
    }
    return localization::translate_message(
        "Invalid relation metadata or observation.");
}
