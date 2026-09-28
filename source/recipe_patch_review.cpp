#include "recipe_patch_review.hpp"
#include "app_config.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "terminal_safe_text.hpp"
#include <sstream>
#include <stdexcept>

std::optional<ConfirmationResult> offer_patch_recipe_review(const LocalSourceFileSnapshot& recipe, const AppConfig& config) {
    if(config.user_config.review.pkgbuild == ReviewPolicy::Skip) return std::nullopt;
    const auto show = request_confirmation(localization::format_translated_message("Show {} for read-only review?", "PKGBUILD"),
                                           ConfirmationDefault::No, config.no_confirm);
    if(std::holds_alternative<ConfirmationDeclined>(show)) return std::nullopt;
    if(!std::holds_alternative<ConfirmationAccepted>(show)) return show;
    constexpr std::size_t MAX_INLINE_RECIPE_BYTES = 65536;
    if(recipe.contents.size() > MAX_INLINE_RECIPE_BYTES) {
        Logger::error(localization::format_translated_message("{} is too large for inline review. Inspect it externally and decline the preview on the next invocation.", "PKGBUILD"));
        throw std::runtime_error("local-recipe-review-limit");
    }
    Logger::info(localization::format_translated_message("Read-only {} candidate:", "PKGBUILD"));
    std::istringstream lines(recipe.contents);
    std::string line;
    // NO_TRANSLATE: Indented user recipe content, escaped for terminal display.
    while(std::getline(lines, line))
        Logger::info("  | " + terminal_safe_text::escape_utf8(line));
    return std::nullopt;
}
