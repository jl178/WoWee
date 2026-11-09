#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::addons {

struct TocFile {
    std::string addonName;
    std::string basePath;

    std::unordered_map<std::string, std::string> directives;
    std::vector<std::string> files;

    [[nodiscard]] std::string getTitle() const;
    [[nodiscard]] bool isLoadOnDemand() const;
    [[nodiscard]] std::vector<std::string> getSavedVariables() const;
    [[nodiscard]] std::vector<std::string> getSavedVariablesPerCharacter() const;
    /// The addons this one cannot run without: ## RequiredDeps, ## Dependencies
    /// and any directive starting "Dep", which the client treats alike.
    [[nodiscard]] std::vector<std::string> getRequiredDeps() const;
    /// ## OptionalDeps: loaded first when they are present, ignored when not.
    [[nodiscard]] std::vector<std::string> getOptionalDeps() const;
    /// ## LoadWith (a load-on-demand addon): load this one as soon as any of
    /// these has loaded.
    [[nodiscard]] std::vector<std::string> getLoadWith() const;
};

std::optional<TocFile> parseTocFile(const std::string& tocPath);

} // namespace wowee::addons
