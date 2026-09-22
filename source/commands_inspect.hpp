#pragma once

#include "dependency_plan.hpp"
#include "presentation_detail.hpp"

#include <string>
#include <vector>

struct AppConfig;
struct PkgbuildExportInvocation;

// fetchがclone/fetchへ進む前に確定するproduction plan。metadata riskを
// install blockerへ読み替えず、route-specific projectionとactual guardが
// 同じBuildPlanを参照する。
struct FetchPreparation {
    BuildPlan plan;
    std::string invocation_targets;
};

int cmd_deps(
    const std::vector<std::string>& targets,
    const std::vector<std::string>& flags,
    const AppConfig& config);

int cmd_plan(
    const std::vector<std::string>& targets,
    const std::vector<std::string>& flags,
    const AppConfig& config);

int cmd_fetch(
    const std::vector<std::string>& targets,
    const std::vector<std::string>& flags,
    const AppConfig& config);

FetchPreparation prepare_fetch_operation(
    const std::vector<std::string>& targets,
    const AppConfig& config);

int cmd_export_pkgbuild_tree(const PkgbuildExportInvocation& invocation);
int cmd_print_pkgbuild(const PkgbuildExportInvocation& invocation);
int cmd_query_foreign_updates(PresentationDetail detail = PresentationDetail::Normal);
