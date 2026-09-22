#pragma once

#include "package_relation_assessment.hpp"

#include <string>

// Public wording is projected only from the completed typed assessment. This
// owner does not parse relation metadata, query package state, or decide
// readiness.
std::string package_relation_assessment_diagnostic_display(
    const PackageRelationAssessment& assessment);

// Compact result summary; the same assessment remains the diagnostic authority.
std::string package_relation_assessment_summary_display(
    const PackageRelationAssessment& assessment);
