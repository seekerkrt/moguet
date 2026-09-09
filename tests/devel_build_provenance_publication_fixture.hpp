#pragma once

#include "devel_build_provenance_store.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class DevelSourceArtifactInstallResult;

std::vector<std::string> devel_publication_fixture_cases(bool projection);
std::string devel_publication_fixture_install_mode(std::string_view scenario);
void check_devel_publication_fixture(std::string_view scenario, DevelSourceArtifactInstallResult installation,
                                     const std::function<void()>& replace_installed_record = {});

// Actual S5 result only. History is readback evidence, never a mint input.
void check_installed_devel_publication(std::string_view label, DevelSourceArtifactInstallResult installation,
                                       std::vector<DevelBuildProvenanceStoreLoaded>& history);
