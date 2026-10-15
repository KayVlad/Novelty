#include "core.h"
#include "recipes.h"
#include "runner_manager.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qtemporarydir.h>
#include <functional>
#include <stdexcept>
#include <cstdio>

using namespace novelty;
namespace {
int checks = 0;
void check(bool okay, const char *why) { ++checks; if (!okay) throw std::runtime_error(why); }
void rejects(const std::function<void()> &work) {
    bool failed = false; try { work(); } catch (const std::exception &) { failed = true; }
    check(failed, "invalid configuration was accepted");
}
void file(const QString &path, const QByteArray &bytes = {}) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile output(path);
    check(output.open(QIODevice::WriteOnly), "fixture file open"); output.write(bytes);
}
QByteArray recipe(const QString &id, const QJsonObject &extra, const QJsonObject &settings = {}) {
    auto result = QJsonObject{{"schema", 1}, {"id", id}, {"name", id}, {"settings", settings}};
    for (auto it = extra.begin(); it != extra.end(); ++it) result[it.key()] = it.value();
    return QJsonDocument(result).toJson();
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary; check(temporary.isValid(), "temporary library");
        const auto root = temporary.path(), folder = root + "/Story";
        file(root + "/.novelty/recipes/default/legacy.json", recipe("legacy", {{"category", "game"}}));
        file(folder + "/Story.exe", "game");
        Library library(root);
        check(QFileInfo::exists(root + "/.novelty/recipes/general/legacy.json") &&
              !QFileInfo::exists(root + "/.novelty/recipes/default/legacy.json"), "old directory migrated without deleting recipe");
        check(!QFileInfo::exists(recipePath(root, "general", "generic-game-v1")) &&
              !QFileInfo::exists(root + "/.novelty/recipes/engines"),
              "fresh libraries create only general and game recipe tiers");
        const auto general = loadRecipes(root);
        for (const auto &entry : general) {
            if (entry.id == "general-game")
                check(entry.name == "Game" && entry.settings["backend"] == "umu" &&
                      !entry.settings.contains("runner") && entry.settings["locale"] == "system",
                      "game general preset uses the supplied defaults");
            if (entry.id == "general-vn")
                check(entry.name == "Visual Novel" && entry.inherits == "general-game" &&
                      !entry.settings.contains("runner") && entry.settings["locale"] == "ja_JP.UTF-8" &&
                      entry.settings["environment"].toObject()["TZ"] == "Asia/Tokyo",
                      "visual novel general preset inherits the game defaults and sets locale and time zone");
        }
        rejects([&] { saveRecipe(root, "engines", recipe("new-engine", {}, {})); });
        file(recipePath(root, "general", "legacy-profile"),
             recipe("legacy-profile", {{"category", "game"}}, {{"setup_profile", "generic-game-v1"}}));
        for (const auto &entry : loadRecipes(root)) if (entry.id == "legacy-profile")
            check(!entry.settings.contains("setup_profile"), "existing profile recipe loads without obsolete setting");
        rejects([&] { saveRecipe(root, "games", recipe("new-profile", {{"category", "game"}},
            {{"setup_profile", "generic-game-v1"}})); });
        rejects([&] { saveRecipe(root, "games", recipe("new-strategy", {{"category", "game"}},
            {{"save_strategy", "renpy"}})); });
        QTemporaryDir older; check(older.isValid(), "old recipe library fixture");
        const auto oldRoot = older.path(), oldFolder = oldRoot + "/Visual Novel";
        file(oldRoot + "/.novelty/recipes/catalog-v2", "older catalog");
        file(recipePath(oldRoot, "general", "generic-game-v1"),
             recipe("generic-game-v1", {{"category", "game"}},
                    {{"backend", "umu"}, {"setup_profile", "generic-game-v1"}}));
        file(recipePath(oldRoot, "general", "generic-vn-v1"),
             recipe("generic-vn-v1", {{"inherits", "generic-game-v1"}, {"category", "visual_novel"}},
                    {{"setup_profile", "generic-proton-v1"}}));
        const auto legacyEngine = oldRoot + "/.novelty/recipes/engines/renpy-bundled-v1.json";
        file(legacyEngine,
             recipe("renpy-bundled-v1", {{"inherits", "generic-vn-v1"},
                 {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"type", "file"}, {"path", "renpy/__init__.py"}},
                                                       QJsonObject{{"type", "directory"}, {"path", "game"}}}}}}},
                    {{"setup_profile", "renpy-bundled-v1"}, {"save_strategy", "renpy"},
                     {"save_candidates", QJsonArray{QJsonObject{{"scope", "game"}, {"path", "game/saves"}}}}}));
        file(oldFolder + "/renpy/__init__.py");
        QDir().mkpath(oldFolder + "/game");
        Library olderLibrary(oldRoot);
        const auto oldChoice = selectRecipe(oldRoot, oldFolder, {}, "generic-vn-v1");
        check(oldChoice.selected().id == "generic-vn-v1" && QFileInfo::exists(legacyEngine),
              "older engine recipes remain on disk but do not match automatically");
        file(oldRoot + "/.novelty/recipes/engines/unused.json", "invalid old recipe");
        check(selectRecipe(oldRoot, oldFolder, {}, "generic-vn-v1").selected().id == "generic-vn-v1",
              "unused older engine files do not affect recipe loading");
        bool legacyVisible = false;
        for (const auto &entry : loadRecipes(oldRoot)) if (entry.id == "renpy-bundled-v1") legacyVisible = true;
        check(!legacyVisible, "older engine recipes are not selectable");
        saveRecipe(oldRoot, "games", recipe("legacy-game", {{"inherits", "renpy-bundled-v1"},
            {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"file", "renpy/__init__.py"}}}}}}}, {}));
        const auto inheritedChoice = selectRecipe(oldRoot, oldFolder, {}, "generic-vn-v1");
        check(inheritedChoice.selected().id == "legacy-game" &&
              !inheritedChoice.selected().settings.contains("save_strategy") &&
              inheritedChoice.selected().settings["save_candidates"].toArray().size() == 1 &&
              inheritedChoice.selected().category == "visual_novel",
              "older recipes retain explicit save paths without the obsolete strategy");
        const auto inherited = recipe("vn-game", {{"inherits", "general-vn"},
            {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"type", "file"}, {"path", "Story.exe"}}}},
                                   {"any", QJsonArray{QJsonObject{{"type", "directory"}, {"path", "assets"}},
                                                      QJsonObject{{"type", "file"}, {"path", "marker.dat"}}}}}}},
            {{"engine", "unknown"}, {"environment", QJsonObject{{"TZ", "Asia/Tokyo"}}}});
        saveRecipe(root, "games", inherited);
        check(!recipeMatches(loadRecipes(root).last(), folder, {folder + "/Story.exe"}), "any clause requires evidence");
        file(folder + "/marker.dat");
        auto selection = selectRecipe(root, folder, {folder + "/Story.exe"}, "general-game");
        check(selection.selected().id == "vn-game" && selection.selected().category == "visual_novel" &&
              selection.selected().settings["engine"] == "unknown", "JSON category independent of engine");
        library.import({inspect(folder, root)}, {});
        auto game = library.games().first();
        check(game.category == "visual_novel" && game.engine == "unknown", "import persists JSON category");
        game.locale = "en_US.UTF-8"; game.environment["CUSTOM"] = "value"; library.save(game);
        game = library.game(game.id);
        check(game.recipeOverrides.contains("locale") && game.recipeOverrides.contains("environment"), "manual edits have explicit provenance");
        saveRecipe(root, "games", recipe("vn-game", {{"inherits", "general-vn"},
            {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"type", "file"}, {"path", "Story.exe"}}}}}}},
            {{"engine", "unknown"}, {"environment", QJsonObject{{"TZ", "Europe/Dublin"}}}}));
        const auto preview = library.previewRecipe(game.id, "vn-game");
        check(preview.locale == "en_US.UTF-8" && preview.environment["CUSTOM"] == "value", "preview preserves user edits");
        library.applyRecipe(game.id, "vn-game"); game = library.game(game.id);
        check(game.environment["CUSTOM"] == "value" && game.locale == "en_US.UTF-8", "applying new revision preserves overrides");
        QStringList omissions;
        const auto recipes = loadRecipes(root);
        const Recipe *parent = nullptr;
        for (const auto &entry : recipes) if (entry.id == "vn-game") parent = &entry;
        check(parent, "parent recipe exists");
        const auto exported = gameRecipeDocument(game, "story-custom", root, parent, &omissions);
        check(exported["inherits"] == "vn-game" && exported["settings"].toObject()["locale"] == "en_US.UTF-8",
              "portable game export records only changes");
        saveRecipe(root, "games", QJsonDocument(exported).toJson());
        bool exportedMatches = false;
        for (const auto &entry : loadRecipes(root)) if (entry.id == "story-custom")
            exportedMatches = recipeMatches(entry, folder, {folder + "/Story.exe"});
        check(!exportedMatches, "export remains manual without reviewed match");
        QTemporaryDir second; check(second.isValid(), "second disposable library");
        const auto secondRoot = second.path(), secondFolder = secondRoot + "/Story";
        file(secondFolder + "/Story.exe", "game");
        file(secondFolder + "/marker.dat");
        Library another(secondRoot);
        QFile parentFile(recipePath(root, "games", "vn-game"));
        check(parentFile.open(QIODevice::ReadOnly), "read exported parent");
        saveRecipe(secondRoot, "games", parentFile.readAll());
        saveRecipe(secondRoot, "games", QJsonDocument(exported).toJson());
        another.import({inspect(secondFolder, secondRoot)}, {});
        const auto transferred = another.games().first();
        another.applyRecipe(transferred.id, "story-custom");
        const auto roundTrip = another.game(transferred.id);
        check(roundTrip.category == game.category && roundTrip.engine == game.engine &&
              roundTrip.locale == game.locale && roundTrip.environment == game.environment,
              "portable game export applies on a second disposable library");
        rejects([&] { removeRecipe(root, "games", "vn-game"); });
        QFile::remove(recipePath(root, "games", "vn-game")); // Simulate a missing external source file.
        check(library.game(game.id).executable == game.executable && library.game(game.id).locale == game.locale,
              "saved launch config survives missing recipe");
        QFile::remove(recipePath(root, "games", "story-custom"));
        const auto global = libraryPresetDocument({{"default_recipe", "general-game"}, {"backend", "umu"},
            {"runner", "/home/user/private/GE"}, {"token", "secret"}}, "portable", root, &omissions);
        const auto bytes = QJsonDocument(global).toJson();
        check(!bytes.contains("secret") && !bytes.contains("/home/") && !bytes.contains("token"), "global export omits secrets and paths");
        check(parseLibraryPreset(bytes) == global, "library preset round trips");
        rejects([&] { parseLibraryPreset(QJsonDocument(QJsonObject{{"schema", 1}, {"id", "bad"},
            {"settings", QJsonObject{{"server_token", "secret"}}}}).toJson()); });
        rejects([&] { saveRecipe(root, "games", recipe("bad-exe", {{"category", "game"}},
            {{"executable", "../outside.exe"}})); });
        rejects([&] { saveRecipe(root, "games", recipe("bad-env", {{"category", "game"}},
            {{"environment", QJsonObject{{"WINEPREFIX", "/tmp/evil"}}}})); });
        rejects([&] { saveRecipe(root, "games", recipe("bad-key", {{"category", "game"}},
            {{"script", "touch /tmp/evil"}})); });
        const auto catalog = parseGeCatalog(QJsonDocument(QJsonArray{
            QJsonObject{{"tag_name", "GE-Proton11-7"}, {"published_at", "2026-01-01T00:00:00Z"},
                        {"assets", QJsonArray{QJsonObject{{"name", "GE-Proton11-7.tar.gz"}, {"size", 1234}}}}},
            QJsonObject{{"tag_name", "bad"}}}).toJson());
        check(catalog.size() == 1 && catalog.first().version == "GE-Proton11-7", "official catalog parse filters unsupported tags");
        check(!fetchGeCatalog(root, QUrl("file:///nonexistent-catalog"), 100).error.isEmpty(), "unavailable catalog is visible");
        const auto version = QString("GE-Proton99-1"), runner = root + "/.novelty/runners/" + version;
        file(runner + "/proton", "#!/bin/sh\n");
        QFile::setPermissions(runner + "/proton", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        file(runner + "/version", "test");
        writeJson(runner + "/novelty-install.json", {{"schema", 1}, {"version", version}, {"architecture", "x86_64"},
            {"sha512", QString(128, QLatin1Char('a'))}});
        check(managedGeRunners(library).size() == 1 && managedGeRunners(library).first().verified, "managed receipt checked");
        {
            QLockFile maintenance(root + "/.novelty/mutation.lock"); maintenance.setStaleLockTime(0);
            check(maintenance.tryLock(), "maintenance lock fixture");
            const auto before = library.settings();
            rejects([&] { saveRecipe(root, "games", recipe("during-maintenance", {}, {{"runner", version}})); });
            rejects([&] { removeRecipe(root, "general", "legacy"); });
            rejects([&] { library.setSettings({{"runner", runner}}); });
            rejects([&] { syncGeProvisioningPolicy(library, runner); });
            check(library.settings() == before && !QFileInfo::exists(recipePath(root, "games", "during-maintenance")) &&
                  QFileInfo::exists(recipePath(root, "general", "legacy")) &&
                  !QFileInfo::exists(root + "/.novelty/ge-proton.json"),
                  "maintenance lock leaves recipes, Settings and GE policy unchanged");
        }
        syncGeProvisioningPolicy(library, runner);
        check(readJson(root + "/.novelty/ge-proton.json")["version"] == version,
              "Settings pin updates the provisioning policy");
        library.setSettings({{"runner", runner}});
        check(!managedRunnerRemovalIssue(library, version).isEmpty(), "default pin blocks removal");
        syncGeProvisioningPolicy(library, {});
        check(!readJson(root + "/.novelty/ge-proton.json").contains("version"),
              "changing the default clears the old GE pin");
        library.setSettings({{"default_recipe", "general-game"}});
        saveRecipe(root, "games", recipe("pinned-runner", {}, {{"runner", version}}));
        check(!managedRunnerRemovalIssue(library, version).isEmpty(), "recipe pin blocks runner removal");
        rejects([&] { removeManagedRunner(library, version); });
        removeRecipe(root, "games", "pinned-runner");
        removeManagedRunner(library, version);
        check(!QFileInfo::exists(runner) && QFileInfo::exists(folder + "/Story.exe"), "unused build removed without game data loss");
        {
            QTemporaryDir provenance; check(provenance.isValid(), "runner provenance library");
            const auto root = provenance.path(), gameFolder = root + "/Runner Story";
            file(gameFolder + "/Story.exe", "game");
            const auto a = QString("GE-Proton90-1"), b = QString("GE-Proton90-2"), c = QString("GE-Proton90-3");
            for (const auto &version : {a, b, c}) file(root + "/.novelty/runners/" + version + "/proton");
            QString id;
            {
                Library runners(root);
                saveRecipe(root, "games", recipe("runner-a", {{"match", QJsonObject{{"all", QJsonArray{
                    QJsonObject{{"file", "Story.exe"}}}}}}}, {{"runner", a}}));
                runners.import({inspect(gameFolder, root)}, {});
                auto game = runners.games().first(); id = game.id;
                check(game.runnerSelector == a && game.runner.isEmpty(), "recipe A imports as a selector without a resolved path");
                game.runner = resolveRunnerSelector(root, a); // First Play resolves the recipe's runner.
                runners.save(game); game = runners.game(id);
                check(game.runner == root + "/.novelty/runners/" + a && !game.recipeOverrides.contains("runner"),
                      "automatic first-Play resolution does not become an override");
                {
                    Library reopenedAutomatic(root);
                    check(reopenedAutomatic.game(id).runnerSelector == a &&
                          !reopenedAutomatic.game(id).recipeOverrides.contains("runner"),
                          "automatic runner provenance survives reopening the library");
                }
                game.recipeOverrides["runner"] = game.runner; // Receipt written by an older Novelty build.
                runners.save(game);
                check(!runners.game(id).recipeOverrides.contains("runner"), "legacy automatic runner override is recognized");
                saveRecipe(root, "games", recipe("runner-b", {}, {{"runner", b}}));
                runners.applyRecipe(id, "runner-b"); game = runners.game(id);
                check(game.runner.isEmpty() && game.runnerSelector == b && !game.recipeOverrides.contains("runner"),
                      "applying recipe B releases the automatically resolved runner A");
                game.runner = resolveRunnerSelector(root, b); runners.save(game);
                game = runners.game(id); game.selectRunner(resolveRunnerSelector(root, c)); runners.save(game);
                game = runners.game(id);
                check(game.runner == root + "/.novelty/runners/" + c &&
                      game.recipeOverrides["runner"] == game.runner && game.runnerSelector.isEmpty(),
                      "explicit Manage runner C is recorded separately from the recipe selector");
                runners.applyRecipe(id, "runner-a"); game = runners.game(id);
                check(game.runner == root + "/.novelty/runners/" + c && game.recipeOverrides["runner"] == game.runner,
                      "explicit runner C survives a recipe change");
                game.environment = {{"TZ", "Asia/Tokyo"}, {"CUSTOM", "portable"}, {"API_TOKEN", "secret-value-123"},
                    {"HTTP_AUTHORIZATION", "Bearer secret-header-456"}, {"CACHE_DIR", "/home/deck/private-cache"},
                    {"WINDOWS_CACHE", "C:\\Users\\deck\\Private"}, {"OPTIONS", "--cache=/home/deck/private"},
                    {"EXTRA_FLAGS", "Authorization: Bearer secret-flags-789"},
                    {"CDN_URL", "https://example.invalid/assets"}};
                runners.save(game); game = runners.game(id);
                QStringList omissions;
                const auto exported = gameRecipeDocument(game, "portable-runner", root, nullptr, &omissions);
                const auto bytes = QJsonDocument(exported).toJson();
                const auto environment = exported["settings"].toObject()["environment"].toObject();
                check(environment["TZ"] == "Asia/Tokyo" && environment["CUSTOM"] == "portable" &&
                      environment["CDN_URL"] == "https://example.invalid/assets", "portable environment survives export");
                check(!environment.contains("API_TOKEN") && !environment.contains("HTTP_AUTHORIZATION") &&
                      !environment.contains("CACHE_DIR") && !environment.contains("WINDOWS_CACHE") &&
                      !environment.contains("OPTIONS") && !environment.contains("EXTRA_FLAGS") &&
                      !bytes.contains("secret-value-123") && !bytes.contains("secret-flags-789") &&
                      !bytes.contains("secret-header-456") && !bytes.contains("/home/deck") &&
                      omissions.join(' ').contains("API_TOKEN") && omissions.join(' ').contains("CACHE_DIR") &&
                      !omissions.join(' ').contains("secret-value-123"),
                      "secrets and machine paths are omitted with value-free diagnostics");
                check(exported["settings"].toObject()["runner"] == c &&
                      parseRecipe(bytes, {}, "games").settings["environment"].toObject() == environment,
                      "explicit runner and sanitized environment round trip as a valid recipe");
                Recipe privateParent; privateParent.id = "private-parent";
                privateParent.settings = {{"environment", QJsonObject{{"API_TOKEN", "parent-secret"}}}};
                QStringList parentOmissions;
                const auto standalone = gameRecipeDocument(game, "standalone-private", root, &privateParent, &parentOmissions);
                check(!standalone.contains("inherits") && !QJsonDocument(standalone).toJson().contains("parent-secret") &&
                      parentOmissions.join(' ').contains("standalone recipe"),
                      "export does not inherit a parent that would restore omitted secrets");
            }
            Library reopened(root);
            auto game = reopened.game(id);
            check(game.runner == root + "/.novelty/runners/" + c && game.recipeOverrides["runner"] == game.runner,
                  "explicit runner provenance survives reopening the library");
            reopened.applyRecipe(id, "runner-b"); game = reopened.game(id);
            check(game.runner == root + "/.novelty/runners/" + c && game.recipeOverrides["runner"] == game.runner,
                  "reopened explicit runner survives another recipe change");
            game.selectRunner({}); reopened.save(game); game = reopened.game(id);
            check(game.runner.isEmpty() && game.runnerSelector == b && !game.recipeOverrides.contains("runner"),
                  "clearing a Manage runner selection returns to the recipe selector");
        }
        std::printf("PASS: %d configuration assertions\n", checks); return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
