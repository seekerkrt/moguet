#include "review_recipe_patch_save.hpp"

#include "app_config.hpp"
#include "source_build.hpp"
#include "source_install.hpp"
#include "reviewed_devel_source_route.hpp"
#include "local_source_metadata_evaluation.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "terminal_safe_text.hpp"

#include <algorithm>
#include <iostream>

namespace {
namespace fs = std::filesystem;

[[noreturn]] void stop_confirmation(ConfirmationResult result) {
    Logger::warn(confirmation_stop_diagnostic(result));
    throw ConfirmationOperationStopped(std::move(result));
}

[[noreturn]] void fail_save(RecipePatchSaveFailure failure) {
    std::string message;
    switch(failure.stage) {
        case RecipePatchSaveStage::UnsupportedDevel:
            message = localization::translate_message("Saving patch customization for authoritative devel builds is unsupported; build stopped.");
            break;
        case RecipePatchSaveStage::Destination:
            message = localization::translate_message("Patch directory is empty, unsafe or unavailable; build stopped.");
            message += " " + localization::translate_message("Use an existing patch directory with an absolute path or a path relative to the command's starting directory; '~' is not expanded.");
            break;
        case RecipePatchSaveStage::Generation:
            message = localization::translate_message("Recipe patch generation or exact reproduction verification failed; build stopped.");
            break;
        case RecipePatchSaveStage::IdentityValidation:
            message = localization::translate_message("Accepted recipe identity validation failed; build stopped.");
            break;
        case RecipePatchSaveStage::Publication:
            message = localization::translate_message("Generated patch publication or integrity verification failed; build stopped.");
            break;
        case RecipePatchSaveStage::Registration:
            message = localization::translate_message("Patch association registration failed; build stopped.");
            break;
    }
    if(failure.association) {
        const auto& reason = *failure.association;
        std::string name;
        switch(reason.kind) {
            case PatchAssociationFailureKind::Missing: name = localization::translate_message("missing patch material or association"); break;
            case PatchAssociationFailureKind::Changed: name = localization::translate_message("patch material digest changed"); break;
            case PatchAssociationFailureKind::Unsafe: name = localization::translate_message("unsafe patch customization"); break;
            case PatchAssociationFailureKind::Corrupt: name = localization::translate_message("corrupt patch association"); break;
            case PatchAssociationFailureKind::Unsupported: name = localization::translate_message("unsupported patch association"); break;
            case PatchAssociationFailureKind::InvalidMaterial: name = localization::translate_message("invalid patch material"); break;
            case PatchAssociationFailureKind::InvalidIdentity:
            case PatchAssociationFailureKind::AssociationMismatch: name = localization::translate_message("patch association identity mismatch"); break;
            case PatchAssociationFailureKind::ConcurrentChange: name = localization::translate_message("patch association changed during the operation"); break;
            case PatchAssociationFailureKind::AlreadyExists: name = localization::translate_message("patch material or association already exists"); break;
            case PatchAssociationFailureKind::ToolFailure: name = localization::translate_message("recipe identity evaluation failed"); break;
            case PatchAssociationFailureKind::PublicationUncertain: name = localization::translate_message("patch registration commit is uncertain"); break;
            case PatchAssociationFailureKind::IoFailure: name = localization::translate_message("patch customization I/O failure"); break;
        }
        message += " " + localization::format_translated_message("Patch customization failure: {} ({}).", name,
                                                                 terminal_safe_text::escape_utf8(reason.path.string()));
    }
    if(!failure.retained_material.empty()) {
        const auto path = terminal_safe_text::escape_utf8(failure.retained_material.string());
        message += " " + (failure.registration_completed
                              ? localization::format_translated_message("Patch material and registration are complete at {}. Current build continuation stopped.", path)
                              : localization::format_translated_message("Published patch material remains at {}. Registration is incomplete or uncertain.", path));
    }
    for(const auto& material : failure.retained_materials) {
        if(material == failure.retained_material) continue;
        const auto path = terminal_safe_text::escape_utf8(material.string());
        message += " " + (failure.registration_completed
                              ? localization::format_translated_message("Patch material and registration are complete at {}. Current build continuation stopped.", path)
                              : localization::format_translated_message("Published patch material remains at {}. Registration is incomplete or uncertain.", path));
    }
    throw RecipePatchSaveError(std::move(failure), message);
}

fs::path request_patch_directory() {
    std::cout << localization::translate_message("Patch directory (absolute path or relative to the command's starting directory; '~' is not expanded):") << " " << std::flush;
    std::string value;
    if(!std::getline(std::cin, value)) {
        if(std::cin.bad() || (!std::cin.eof() && std::cin.fail())) stop_confirmation(ConfirmationInputFailure{});
        stop_confirmation(ConfirmationCancelled{ConfirmationCancellationReason::EndOfInput});
    }
    if(std::holds_alternative<ConfirmationCancelled>(parse_confirmation_input(value, ConfirmationDefault::None)))
        stop_confirmation(ConfirmationCancelled{ConfirmationCancellationReason::ExplicitToken});
    if(value.size() > 4096 || value.find_first_not_of(" \t\r\n") == std::string::npos)
        fail_save({RecipePatchSaveStage::Destination});
    return fs::path(value);
}

void require_accepted_recipe(const ReviewRecipeEditCorrelation& edit, const SourceBuildRequest& request,
                             const ValidatedCachePath& checkout) {
    static_cast<void>(revalidate_trusted_cache_path(checkout, CachePathRequirement::ExistingDirectory));
    if(checkout.device() != edit.checkout_device() || checkout.inode() != edit.checkout_inode() ||
       request.aur_review_identity != std::optional<PackageBaseIdentity>{edit.identity().package_base()} ||
       request.checkout_name != edit.identity().package_base().package_base() ||
       request.git_url != edit.identity().canonical_git_remote())
        fail_save({RecipePatchSaveStage::IdentityValidation});
    auto root = open_local_source_root(checkout.canonical_path(), true);
    if(snapshot_supported_recipe_files(root) != edit.accepted_recipe())
        fail_save({RecipePatchSaveStage::IdentityValidation});
    root.require_unchanged_identity();
}
} // namespace

void save_review_recipe_edit(
    const ReviewRecipeEditCorrelation* edit, bool unsupported_devel_edit,
    const SourceBuildRequest& request, const ValidatedCachePath& checkout,
    const ValidatedCacheRoot& cache_root, const AppConfig& config,
    const ReviewedDevelSourceBuildIntent* intent) {
    if(!edit && !unsupported_devel_edit) return;
    auto answer = request_confirmation(localization::translate_message("Save this edit as patch customization?"),
                                       ConfirmationDefault::No, config.no_confirm);
    if(std::holds_alternative<ConfirmationDeclined>(answer)) return;
    const auto* accepted = std::get_if<ConfirmationAccepted>(&answer);
    if(!accepted || accepted->origin != ConfirmationDecisionOrigin::ExplicitToken) stop_confirmation(std::move(answer));
    if(unsupported_devel_edit || request.authoritative_devel_update || request.devel_tracking_bootstrap)
        fail_save({RecipePatchSaveStage::UnsupportedDevel});
    if(!edit) fail_save({RecipePatchSaveStage::IdentityValidation});
    try {
        require_accepted_recipe(*edit, request, checkout);
        if(edit->upstream_srcinfo_failure()) throw LocalSourceRootError(*edit->upstream_srcinfo_failure());
        if(intent && edit->upstream_srcinfo() && requires_authoritative_devel_recipe(*edit->upstream_srcinfo(), *intent))
            fail_save({RecipePatchSaveStage::UnsupportedDevel});
    } catch(const RecipePatchSaveError&) {
        throw;
    } catch(...) {
        RecipePatchSaveFailure stopped{RecipePatchSaveStage::IdentityValidation};
        stopped.exception = std::current_exception();
        fail_save(std::move(stopped));
    }
    const auto destination = request_patch_directory();
    auto destination_check = validate_generated_patch_destination(destination, config.command_start_directory,
                                                                  {cache_root.canonical_path(), checkout.canonical_path()});
    if(auto* failure = std::get_if<PatchAssociationFailure>(&destination_check)) {
        RecipePatchSaveFailure stopped{RecipePatchSaveStage::Destination};
        stopped.association = *failure;
        fail_save(std::move(stopped));
    }
    const auto duplicate = read_patch_association(edit->identity().package_base());
    if(const auto* failure = std::get_if<PatchAssociationFailure>(&duplicate)) {
        RecipePatchSaveFailure stopped{RecipePatchSaveStage::Registration};
        stopped.association = *failure;
        fail_save(std::move(stopped));
    }
    if(std::holds_alternative<LoadedPatchAssociation>(duplicate)) {
        RecipePatchSaveFailure stopped{RecipePatchSaveStage::Registration};
        stopped.association = PatchAssociationFailure{PatchAssociationFailureKind::AlreadyExists,
                                                      patch_association_record_path(edit->identity().package_base()),
                                                      {},
                                                      std::nullopt,
                                                      std::nullopt};
        fail_save(std::move(stopped));
    }
    auto generated = generate_recipe_patch(*edit);
    if(auto* failure = std::get_if<RecipePatchGenerationFailure>(&generated))
        fail_save({RecipePatchSaveStage::Generation, std::move(*failure)});
    if(!std::holds_alternative<GeneratedRecipePatch>(generated)) fail_save({RecipePatchSaveStage::Generation});

    RecipePatchSaveFailure failure{RecipePatchSaveStage::IdentityValidation};
    std::optional<LocalSourceWorkspace> candidate;
    bool cleanup_attempted = false;
    try {
        require_accepted_recipe(*edit, request, checkout);
        auto original = open_local_source_root(checkout.canonical_path(), true);
        auto evaluation = request_confirmation(localization::format_translated_message(
                                                   "Evaluate {} metadata with {}?", "PKGBUILD", "makepkg --printsrcinfo"),
                                               ConfirmationDefault::None, config.no_confirm);
        const auto* consent = std::get_if<ConfirmationAccepted>(&evaluation);
        if(!consent || consent->origin != ConfirmationDecisionOrigin::ExplicitToken) stop_confirmation(std::move(evaluation));
        // Fresh identity only, in an owned copy. No provider/dependency replan,
        // RPC refresh or future Apply consent is derived from Save Yes.
        candidate.emplace(materialize_local_source_workspace(original, cache_root));
        auto root = open_local_source_root(candidate->path(), true);
        const auto evaluation_recipe = snapshot_supported_recipe_files(root);
        if(!same_supported_recipe_content_and_mode(evaluation_recipe, edit->accepted_recipe()))
            fail_save({RecipePatchSaveStage::IdentityValidation});
        auto effective_environment = request.custom_environment;
        if(request.empty_value_policy == SourceEnvironmentEmptyValuePolicy::Omit)
            std::erase_if(effective_environment.ordered_assignments, [](const auto& assignment) { return assignment.value.empty(); });
        const auto metadata = evaluate_recipe_metadata(root, request.custom_environment,
                                                       resolve_local_source_effective_architecture(effective_environment), request.empty_value_policy);
        if(metadata.metadata().package_base != request.checkout_name)
            fail_save({RecipePatchSaveStage::IdentityValidation});
        std::vector<std::string> required;
        if(intent)
            for(const auto& target : intent->required_targets)
                required.push_back(target.package_name);
        else if(!request.package_name.empty())
            required.push_back(request.package_name);
        for(const auto& child : required)
            if(std::none_of(metadata.metadata().children.begin(), metadata.metadata().children.end(),
                            [&](const auto& value) { return value.name == child; })) fail_save({RecipePatchSaveStage::IdentityValidation});
        if(snapshot_supported_recipe_files(open_local_source_root(candidate->path(), true)) != evaluation_recipe)
            fail_save({RecipePatchSaveStage::IdentityValidation});
        original.require_unchanged_identity();
        cleanup_attempted = true;
        candidate->cleanup();
        candidate.reset();
        require_accepted_recipe(*edit, request, checkout);
    } catch(...) {
        const auto primary = std::current_exception();
        if(candidate && !cleanup_attempted) {
            cleanup_attempted = true;
            try {
                candidate->cleanup();
            } catch(const LocalSourceWorkspaceError& error) {
                failure.cleanup = error.failure();
            } catch(...) { /* Original failure remains primary. */
            }
        }
        if(cleanup_attempted) {
            try {
                std::rethrow_exception(primary);
            } catch(const LocalSourceWorkspaceError& error) {
                failure.cleanup = error.failure();
            } catch(...) {
            }
        }
        if(!failure.cleanup) {
            try {
                std::rethrow_exception(primary);
            } catch(const RecipePatchSaveError&) {
                throw;
            } catch(const ConfirmationOperationStopped&) {
                throw;
            } catch(...) {
            }
        }
        failure.exception = primary;
        fail_save(std::move(failure));
    }

    auto publication = publish_generated_recipe_patch(std::get<GeneratedRecipePatch>(generated), destination,
                                                      config.command_start_directory, {cache_root.canonical_path(), checkout.canonical_path()});
    if(auto* error = std::get_if<PatchAssociationFailure>(&publication)) {
        RecipePatchSaveFailure stopped{RecipePatchSaveStage::Publication};
        stopped.association = *error;
        if(error->published_material) stopped.retained_material = *error->published_material;
        stopped.retained_materials = error->published_materials;
        fail_save(std::move(stopped));
    }
    const auto& published = std::get<PublishedRecipePatch>(publication);
    const auto material_path = published.material_root / published.expected_entry.file;
    std::vector<fs::path> material_paths;
    for(const auto& entry : published.expected_entries)
        material_paths.push_back(published.material_root / entry.file);
    const ResolvedAurSourceBuildIdentity source(request.package_name.empty() ? request.checkout_name : request.package_name,
                                                request.checkout_name);
    auto registration = register_expected_aur_patch_association(source, published.material_root, published.expected_entries);
    if(auto* error = std::get_if<PatchAssociationFailure>(&registration)) {
        RecipePatchSaveFailure stopped{RecipePatchSaveStage::Registration};
        stopped.association = *error;
        stopped.retained_material = material_path;
        stopped.retained_materials = material_paths;
        fail_save(std::move(stopped));
    }
    // Same accepted bytes, never a stock or regenerated recipe, continue.
    try {
        require_accepted_recipe(*edit, request, checkout);
    } catch(...) {
        RecipePatchSaveFailure stopped{RecipePatchSaveStage::IdentityValidation};
        stopped.exception = std::current_exception();
        stopped.retained_material = material_path;
        stopped.retained_materials = material_paths;
        stopped.registration_completed = true;
        fail_save(std::move(stopped));
    }
    for(const auto& path : material_paths)
        Logger::info(localization::format_translated_message("Saved patch customization: {}", terminal_safe_text::escape_utf8(path.string())));
}
