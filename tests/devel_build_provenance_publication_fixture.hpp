#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class DevelSourceArtifactInstallResult;

std::vector<std::string> devel_publication_fixture_cases(bool projection);
std::string devel_publication_fixture_install_mode(std::string_view scenario);
void check_devel_publication_fixture(std::string_view scenario, DevelSourceArtifactInstallResult installation,
                                     const std::function<void()>& replace_installed_record = {});
