#pragma once

struct DevelPackageAssessmentTarget;
struct DevelPackageAssessment;

// Complete at the #475 granting header. Only the own-I/O entrance may invoke
// this owner; neither diagnostic products nor persistent values are friends.
class DevelPackageAssessmentAuthority final {
    DevelPackageAssessmentAuthority() = delete;
    friend DevelPackageAssessment assess_current_devel_package(
        const DevelPackageAssessmentTarget& target);

    [[nodiscard]] static DevelPackageAssessment assess(
        const DevelPackageAssessmentTarget& target);
};
