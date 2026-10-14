#include "core.h"
#include "recipes.h"
#include "runner_manager.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
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
              !QFileInfo::exists(recipePath(root, "engines", "renpy-bundled-v1")),
              "fresh libraries do not seed deprecated profile recipes");
        file(recipePath(root, "general", "legacy-profile"),
             recipe("legacy-profile", {{"category", "game"}}, {{"setup_profile", "generic-game-v1"}}));
        for (const auto &entry : loadRecipes(root)) if (entry.id == "legacy-profile")
            check(!entry.settings.contains("setup_profile"), "existing profile recipe loads without obsolete setting");
        rejects([&] { saveRecipe(root, "games", recipe("new-profile", {{"category", "game"}},
            {{"setup_profile", "generic-game-v1"}})); });
        QTemporaryDir older; check(older.isValid(), "old recipe library fixture");
        const auto oldRoot = older.path(), oldFolder = oldRoot + "/Visual Novel";
        file(oldRoot + "/.novelty/recipes/catalog-v2", "older catalog");
        file(recipePath(oldRoot, "general", "generic-game-v1"),
             recipe("generic-game-v1", {{"category", "game"}},
                    {{"backend", "umu"}, {"setup_profile", "generic-game-v1"}}));
        file(recipePath(oldRoot, "general", "generic-vn-v1"),
             recipe("generic-vn-v1", {{"inherits", "generic-game-v1"}, {"category", "visual_novel"}},
                    {{"setup_profile", "generic-proton-v1"}}));
        file(recipePath(oldRoot, "engines", "renpy-bundled-v1"),
             recipe("renpy-bundled-v1", {{"inherits", "generic-vn-v1"},
                 {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"type", "file"}, {"path", "renpy/__init__.py"}},
                                                       QJsonObject{{"type", "directory"}, {"path", "game"}}}}}}},
                    {{"setup_profile", "renpy-bundled-v1"}, {"save_strategy", "renpy"}}));
        file(oldFolder + "/renpy/__init__.py");
        QDir().mkpath(oldFolder + "/game");
        Library olderLibrary(oldRoot);
        const auto oldChoice = selectRecipe(oldRoot, oldFolder, {}, "generic-vn-v1");
        check(oldChoice.selected().id == "renpy-bundled-v1" &&
              !oldChoice.selected().settings.contains("setup_profile") &&
              oldChoice.selected().category == "visual_novel",
              "older inherited catalog remains usable without compiled profiles");
        const auto inherited = recipe("vn-engine", {{"inherits", "general-vn"},
            {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"type", "file"}, {"path", "Story.exe"}}}},
                                   {"any", QJsonArray{QJsonObject{{"type", "directory"}, {"path", "assets"}},
                                                      QJsonObject{{"type", "file"}, {"path", "marker.dat"}}}}}}},
            {{"engine", "unknown"}, {"environment", QJsonObject{{"TZ", "Asia/Tokyo"}}}});
        saveRecipe(root, "engines", inherited);
        check(!recipeMatches(loadRecipes(root).last(), folder, {folder + "/Story.exe"}), "any clause requires evidence");
        file(folder + "/marker.dat");
        auto selection = selectRecipe(root, folder, {folder + "/Story.exe"}, "general-game");
        check(selection.selected().id == "vn-engine" && selection.selected().category == "visual_novel" &&
              selection.selected().settings["engine"] == "unknown", "JSON category independent of engine");
        library.import({inspect(folder, root)}, {});
        auto game = library.games().first();
        check(game.category == "visual_novel" && game.engine == "unknown", "import persists JSON category");
        game.locale = "ja_JP.UTF-8"; game.environment["CUSTOM"] = "value"; library.save(game);
        game = library.game(game.id);
        check(game.recipeOverrides.contains("locale") && game.recipeOverrides.contains("environment"), "manual edits have explicit provenance");
        saveRecipe(root, "engines", recipe("vn-engine", {{"inherits", "general-vn"},
            {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"type", "file"}, {"path", "Story.exe"}}}}}}},
            {{"engine", "unknown"}, {"environment", QJsonObject{{"TZ", "Europe/Dublin"}}}}));
        const auto preview = library.previewRecipe(game.id, "vn-engine");
        check(preview.locale == "ja_JP.UTF-8" && preview.environment["CUSTOM"] == "value", "preview preserves user edits");
        library.applyRecipe(game.id, "vn-engine"); game = library.game(game.id);
        check(game.environment["CUSTOM"] == "value" && game.locale == "ja_JP.UTF-8", "applying new revision preserves overrides");
        QStringList omissions;
        const auto recipes = loadRecipes(root);
        const Recipe *parent = nullptr;
        for (const auto &entry : recipes) if (entry.id == "vn-engine") parent = &entry;
        check(parent, "parent recipe exists");
        const auto exported = gameRecipeDocument(game, "story-custom", root, parent, &omissions);
        check(exported["inherits"] == "vn-engine" && exported["settings"].toObject()["locale"] == "ja_JP.UTF-8",
              "portable game export records only changes");
        saveRecipe(root, "games", QJsonDocument(exported).toJson());
        check(!recipeMatches(loadRecipes(root).last(), folder, {folder + "/Story.exe"}), "export remains manual without reviewed match");
        QTemporaryDir second; check(second.isValid(), "second disposable library");
        const auto secondRoot = second.path(), secondFolder = secondRoot + "/Story";
        file(secondFolder + "/Story.exe", "game");
        file(secondFolder + "/marker.dat");
        Library another(secondRoot);
        QFile parentFile(recipePath(root, "engines", "vn-engine"));
        check(parentFile.open(QIODevice::ReadOnly), "read exported parent");
        saveRecipe(secondRoot, "engines", parentFile.readAll());
        saveRecipe(secondRoot, "games", QJsonDocument(exported).toJson());
        another.import({inspect(secondFolder, secondRoot)}, {});
        const auto transferred = another.games().first();
        another.applyRecipe(transferred.id, "story-custom");
        const auto roundTrip = another.game(transferred.id);
        check(roundTrip.category == game.category && roundTrip.engine == game.engine &&
              roundTrip.locale == game.locale && roundTrip.environment == game.environment,
              "portable game export applies on a second disposable library");
        rejects([&] { removeRecipe(root, "engines", "vn-engine"); });
        QFile::remove(recipePath(root, "engines", "vn-engine")); // Simulate a missing external source file.
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
        syncGeProvisioningPolicy(library, runner);
        check(readJson(root + "/.novelty/ge-proton.json")["version"] == version,
              "Settings pin updates the provisioning policy");
        library.setSettings({{"runner", runner}});
        check(!managedRunnerRemovalIssue(library, version).isEmpty(), "default pin blocks removal");
        syncGeProvisioningPolicy(library, {});
        check(!readJson(root + "/.novelty/ge-proton.json").contains("version"),
              "changing the default clears the old GE pin");
        library.setSettings({{"default_recipe", "general-game"}});
        removeManagedRunner(library, version);
        check(!QFileInfo::exists(runner) && QFileInfo::exists(folder + "/Story.exe"), "unused build removed without game data loss");
        std::printf("PASS: %d configuration assertions\n", checks); return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
