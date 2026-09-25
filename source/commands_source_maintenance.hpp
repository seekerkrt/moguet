#pragma once

#include "cli_routing.hpp"
#include "local_source_root.hpp"
#include "local_source_workspace.hpp"

#include <string>
#include <utility>
#include <vector>

struct AppConfig;

// Pre-log route preparationでlexical CLI requestとdescriptor-first local
// root inspectionを束ねる。missing/unsafe pathはstate/cache作成前に停止する。
struct PreparedLocalSourceBuildRoute {
    LocalSourceBuildInvocation invocation;
    LocalSourceRoot source_root;
};

struct RemoteSourceBuildInvocation {
    std::string package_name;
    SourceBuildEnvironment source_environment;
    bool use_source_preference = false;
};

PreparedLocalSourceBuildRoute prepare_local_source_build_route(
    LocalSourceBuildInvocation invocation,
    const AppConfig& config);

void require_executable_local_source_build_route(
    const PreparedLocalSourceBuildRoute& route);

RemoteSourceBuildInvocation require_remote_source_build_invocation(
    const ParsedCliArguments& parsed);

std::string local_source_workspace_failure_diagnostic(
    const LocalSourceWorkspaceFailure& failure);

int cmd_build_local(
    PreparedLocalSourceBuildRoute route,
    const AppConfig& config);

int cmd_build(
    RemoteSourceBuildInvocation invocation,
    const AppConfig& config);

int cmd_add_src(const std::vector<std::string>& args);

int cmd_edit_src(
    const std::vector<std::string>& targets,
    const AppConfig& config);

void cmd_list_src();

int cmd_del_src(const std::vector<std::string>& targets);

void cmd_revert(
    const std::vector<std::string>& targets,
    const AppConfig& config);

int cmd_clean(const AppConfig& config);

int cmd_upgrade(const AppConfig& config);
