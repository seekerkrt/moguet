#pragma once
#include "interactive_confirmation.hpp"
#include "local_source_root.hpp"
#include <optional>
struct AppConfig;
std::optional<ConfirmationResult> offer_patch_recipe_review(const LocalSourceFileSnapshot& recipe, const AppConfig& config);
