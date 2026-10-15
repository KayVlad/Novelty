#include "core.h"
#include "recipes.h"
#include "runner_manager.h"
#include "runner_fixture.h"
#include "app_settings.h"
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
void settingsFiles(const QString &base) {
    const auto initial = loadAppSettings();
    check(initial == defaultAppSettings() && readJson(appSettingsPath()) == initial,
          "fresh app settings are populated from the bundled JSON");
    auto edited = initial; edited["libraryView"] = "list"; edited["autoGeUpdates"] = false;
    writeJson(appSettingsPath(), edited);
    check(loadAppSettings()["libraryView"] == "list" && !loadAppSettings()["autoGeUpdates"].toBool(),
          "direct JSON edits control application preferences");
    setAppSetting("gamesRoot", base);
    check(readJson(appSettingsPath())["gamesRoot"] == base && readJson(appSettingsPath())["libraryView"] == "list",
          "UI setting writes update the same JSON and retain other preferences");
    rejects([&] { setAppSetting("autoGeUpdates", "false"); });
    rejects([&] { setAppSetting("unsupported", true); });
    check(!readJson(appSettingsPath())["autoGeUpdates"].toBool(), "rejected settings do not overwrite valid JSON");
    const auto valid = readJson(appSettingsPath());
    file(appSettingsPath(), "{broken JSON");
    rejects([&] { loadAppSettings(); });
    QFile broken(appSettingsPath()); check(broken.open(QIODevice::ReadOnly) && broken.readAll() == "{broken JSON",
                                         "invalid user configuration remains available for correction"); broken.close();
    writeJson(appSettingsPath(), valid);
    const auto firstRoot = base + "/Configured library", secondRoot = base + "/Independent library";
    QDir().mkpath(firstRoot); QDir().mkpath(secondRoot);
    Library first(firstRoot), second(secondRoot);
    auto libraryConfig = readJson(first.dataRoot() + "/settings.json");
    check(libraryConfig == defaultLibrarySettings(), "each library gets its own opinionated settings JSON");
    libraryConfig["default_recipe"] = "general-vn"; libraryConfig["runner"] = "ge-proton-latest";
    writeJson(first.dataRoot() + "/settings.json", libraryConfig);
    check(first.settings()["default_recipe"] == "general-vn" && first.settings()["runner"] == "ge-proton-latest" &&
          second.settings()["default_recipe"] == "general-game", "file edits are authoritative and library configurations are independent");
    auto settings = first.settings(); settings["umu"] = "/custom/umu-run"; first.setSettings(settings);
    check(readJson(first.dataRoot() + "/settings.json")["umu"] == "/custom/umu-run", "library Settings writes the authoritative JSON");
    Library reopened(firstRoot);
    check(reopened.settings() == first.settings(), "library preferences survive reopening");
    file(first.dataRoot() + "/settings.json", "{broken JSON");
    rejects([&] { first.settings(); });
    writeJson(first.dataRoot() + "/settings.json", libraryConfig);
    setLocalSetting("externalProtonPaths", QJsonArray{base + "/external"});
    check(!loadAppSettings().contains("externalProtonPaths") &&
          loadLocalSettings()["externalProtonPaths"].toArray().size() == 1, "external registrations have separate machine-local storage");
    setLocalSetting("externalProtonPaths", QJsonArray{});
}
QByteArray recipe(const QString &id, const QJsonObject &extra, const QJsonObject &settings = {}) {
    auto result = QJsonObject{{"schema", 1}, {"id", id}, {"name", id}, {"settings", settings}};
    for (auto it = extra.begin(); it != extra.end(); ++it) result[it.key()] = it.value();
    return QJsonDocument(result).toJson();
}
void recipeContract(const QString &base) {
    const auto root = base + "/Recipe contract", folder = root + "/Game";
    file(folder + "/game.exe"); Library library(root);
    const auto older = library.dataRoot() + "/runners/GE-Proton96-1", newer = library.dataRoot() + "/runners/GE-Proton96-2";
    managedRunnerFixture(older); managedRunnerFixture(newer);
    library.setSettings({{"runner", newer}});
    auto bytes = recipe("configured-game", {{"inherits", "general-game"},
        {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"file", "game.exe"}}}}}}},
        {{"runner", "GE-Proton96-1"}, {"locale", "ja_JP.UTF-8"}, {"arguments", QJsonArray{"recipe argument"}},
         {"environment", QJsonObject{{"NOVELTY_APP_RECIPE", "original"}}}, {"umu_launcher", "umu-latest"},
         {"umu_game_id", "umu-recipe"}, {"store", "gog"}, {"steam_id", "123"}});
    saveRecipe(root, "games", bytes);
    library.import({inspect(folder, root)}, newer);
    auto game = library.games().first();
    check(game.runnerSelector == "GE-Proton96-1" && game.runner.isEmpty() && game.umuLauncher == "umu-latest" &&
          game.umuGameId == "umu-recipe" && game.store == "gog" && game.steamId == "123", "recipe defines launch configuration ahead of library defaults");
    game.locale = "en_US.UTF-8"; game.arguments = {"manual argument"}; game.environment = {{"NOVELTY_APP_MANUAL", "yes"}};
    game.umuGameId = "umu-custom"; game.store = "itchio"; game.steamId = "456"; game.umuLauncher.clear();
    game.selectRunner("ge-proton-latest"); library.save(game);
    QFile original(recipePath(root, "games", "configured-game"));
    check(original.open(QIODevice::ReadOnly) && original.readAll() == bytes, "Manage overrides leave the source recipe byte-for-byte intact"); original.close();
    game = library.game(game.id);
    for (const auto &key : {"locale", "arguments", "environment", "runner", "umu_launcher", "umu_game_id", "store", "steam_id"})
        check(game.recipeOverrides.contains(key), "all changed launch settings have saved overrides");
    auto document = QJsonDocument::fromJson(bytes).object(); auto settings = document["settings"].toObject();
    settings["locale"] = "fr_FR.UTF-8"; settings["umu_game_id"] = "umu-updated"; document["settings"] = settings;
    saveRecipe(root, "games", QJsonDocument(document).toJson());
    library.applyRecipe(game.id, "configured-game"); game = library.game(game.id);
    check(game.locale == "en_US.UTF-8" && game.arguments == QStringList{"manual argument"} &&
          game.environment == QMap<QString, QString>{{"NOVELTY_APP_MANUAL", "yes"}} && game.umuLauncher.isEmpty() &&
          game.umuGameId == "umu-custom" && game.store == "itchio" && game.steamId == "456" && game.runner == newer,
          "reapplying an edited recipe preserves explicit overrides including cleared values");
    Recipe parent;
    for (const auto &entry : loadRecipes(root)) if (entry.id == "configured-game") parent = entry;
    const auto exported = gameRecipeDocument(game, parent.id, root, &parent);
    check(!exported.contains("inherits") && exported["match"] == parent.match &&
          exported["settings"].toObject()["umu_game_id"] == "umu-custom", "export can explicitly replace the source with complete settings and its matching rules");
    saveRecipe(root, "games", QJsonDocument(exported).toJson());
    Game fresh; fresh.folder = folder;
    for (const auto &entry : loadRecipes(root)) if (entry.id == parent.id) applyRecipeToGame(fresh, entry);
    check(fresh.locale == game.locale && fresh.store == game.store && fresh.umuGameId == game.umuGameId &&
          fresh.steamId == game.steamId && fresh.arguments == game.arguments && fresh.environment == game.environment,
          "exported effective configuration recreates the customized launch settings");
    // Resolving a library default is not a user override, even after editing another field.
    const auto delegatedRoot = base + "/Delegated runner", delegatedFolder = delegatedRoot + "/Game";
    file(delegatedFolder + "/game.exe"); Library delegated(delegatedRoot);
    managedRunnerFixture(delegated.dataRoot() + "/runners/GE-Proton96-1");
    delegated.setSettings({{"runner", "ge-proton-latest"}});
    delegated.import({inspect(delegatedFolder, delegatedRoot)}, "ge-proton-latest");
    auto inherited = delegated.games().first(); inherited.locale = "en_US.UTF-8"; delegated.save(inherited);
    check(!delegated.game(inherited.id).recipeOverrides.contains("runner"), "resolved library runner does not become a per-game override on Save");
    saveRecipe(delegatedRoot, "games", recipe("exact-runner", {{"inherits", "general-game"}}, {{"runner", "GE-Proton96-1"}}));
    delegated.applyRecipe(inherited.id, "exact-runner");
    check(delegated.game(inherited.id).runnerSelector == "GE-Proton96-1" && delegated.game(inherited.id).locale == "en_US.UTF-8",
          "a later recipe runner takes effect while an unrelated explicit override survives");
    // A standalone recipe must not accidentally retain fields from a previously applied recipe.
    applyRecipeToGame(fresh, parseRecipe(recipe("neutral", {}, {}), {}, "games"));
    check(fresh.locale.isEmpty() && fresh.arguments.isEmpty() && fresh.environment.isEmpty() &&
          fresh.umuGameId.isEmpty() && fresh.store.isEmpty() && fresh.steamId.isEmpty(), "recipe application assigns neutral values for omitted launch fields");
}

}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary; check(temporary.isValid(), "temporary library");
        qputenv("NOVELTY_TEST_APP_DATA_DIR", QFile::encodeName(temporary.path() + "/app"));
        settingsFiles(temporary.path());
        recipeContract(temporary.path());
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
                      entry.settings["runner"] == "" && entry.settings["locale"] == "system",
                      "game general preset uses the supplied defaults");
            if (entry.id == "general-vn")
                check(entry.name == "Visual Novel" && entry.inherits == "general-game" &&
                      entry.settings["runner"] == "" && entry.settings["locale"] == "ja_JP.UTF-8" &&
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
        game.locale = "en_US.UTF-8"; game.environment["NOVELTY_APP_CUSTOM"] = "value"; library.save(game);
        game = library.game(game.id);
        check(game.recipeOverrides.contains("locale") && game.recipeOverrides.contains("environment"), "manual edits have explicit provenance");
        saveRecipe(root, "games", recipe("vn-game", {{"inherits", "general-vn"},
            {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"type", "file"}, {"path", "Story.exe"}}}}}}},
            {{"engine", "unknown"}, {"environment", QJsonObject{{"TZ", "Europe/Dublin"}}}}));
        const auto preview = library.previewRecipe(game.id, "vn-game");
        check(preview.locale == "en_US.UTF-8" && preview.environment["NOVELTY_APP_CUSTOM"] == "value", "preview preserves user edits");
        library.applyRecipe(game.id, "vn-game"); game = library.game(game.id);
        check(game.environment["NOVELTY_APP_CUSTOM"] == "value" && game.locale == "en_US.UTF-8", "applying new revision preserves overrides");
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
        for (const auto &key : {"LD_PRELOAD", "LD_LIBRARY_PATH", "PYTHONPATH", "PYTHONHOME", "BASH_ENV",
                                "ENV", "PATH", "WINELOADER", "PROTON_REMOTE_DEBUG_CMD", "SDL_DYNAMIC_API"})
            rejects([&] { saveRecipe(root, "games", recipe("bad-host-env", {{"category", "game"}},
                {{"environment", QJsonObject{{key, "unsafe"}}}})); });
        saveRecipe(root, "games", recipe("supported-env", {{"category", "game"}},
            {{"environment", QJsonObject{{"TZ", "Europe/Dublin"}, {"DXVK_HUD", "fps"},
                {"PROTON_NO_ESYNC", "1"}, {"WINEDLLOVERRIDES", "example=b"}}}}));
        rejects([&] { saveRecipe(root, "games", recipe("bad-key", {{"category", "game"}},
            {{"script", "touch /tmp/evil"}})); });
        const auto catalog = parseGeCatalog(QJsonDocument(QJsonArray{
            QJsonObject{{"tag_name", "GE-Proton11-7"}, {"published_at", "2026-01-01T00:00:00Z"},
                        {"assets", QJsonArray{QJsonObject{{"name", "GE-Proton11-7.tar.gz"}, {"size", 1234}},
                                              QJsonObject{{"name", "GE-Proton11-7.sha512sum"}}}}},
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
        // A registered game that has never launched need not have a state directory.
        const auto untouchedRoot = root + "/Never launched";
        file(untouchedRoot + "/game.exe", {});
        library.import({inspect(untouchedRoot)}, {});
        for (const auto &registered : library.games()) {
            const auto lockParent = QFileInfo(library.gamePath(registered.id, "launch.lock")).absolutePath();
            if (QDir(lockParent).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty())
                QDir().rmdir(lockParent);
        }
        removeManagedRunner(library, version);
        check(!QFileInfo::exists(runner) && QFileInfo::exists(folder + "/Story.exe"), "unused build removed without game data loss");
        {
            QTemporaryDir provenance; check(provenance.isValid(), "runner provenance library");
            const auto root = provenance.path(), gameFolder = root + "/Runner Story";
            file(gameFolder + "/Story.exe", "game");
            const auto a = QString("GE-Proton90-1"), b = QString("GE-Proton90-2"), c = QString("GE-Proton90-3");
            for (const auto &version : {a, b, c}) managedRunnerFixture(root + "/.novelty/runners/" + version);
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
                game.environment = {{"TZ", "Asia/Tokyo"}, {"NOVELTY_APP_CUSTOM", "portable"}, {"API_TOKEN", "secret-value-123"},
                    {"HTTP_AUTHORIZATION", "Bearer secret-header-456"}, {"CACHE_DIR", "/home/deck/private-cache"},
                    {"WINDOWS_CACHE", "C:\\Users\\deck\\Private"}, {"OPTIONS", "--cache=/home/deck/private"},
                    {"EXTRA_FLAGS", "Authorization: Bearer secret-flags-789"},
                    {"NOVELTY_APP_CDN_URL", "https://example.invalid/assets"}};
                runners.save(game); game = runners.game(id);
                QStringList omissions;
                const auto exported = gameRecipeDocument(game, "portable-runner", root, nullptr, &omissions);
                const auto bytes = QJsonDocument(exported).toJson();
                const auto environment = exported["settings"].toObject()["environment"].toObject();
                check(environment["TZ"] == "Asia/Tokyo" && environment["NOVELTY_APP_CUSTOM"] == "portable" &&
                      environment["NOVELTY_APP_CDN_URL"] == "https://example.invalid/assets", "portable environment survives export");
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
