#pragma once

#include "core.h"
#include <QtCore/qjsonobject.h>

namespace novelty {

struct Recipe {
    QString id, name, tier, path;
    QJsonObject match, settings;
    QByteArray revision;
    QString inherits;
    bool runnerExplicit = false;
};

struct RecipeSelection {
    QList<Recipe> matches;
    QString tier;
    bool ambiguous() const { return matches.size() > 1; }
    bool empty() const { return matches.isEmpty(); }
    Recipe selected() const { return matches.isEmpty() ? Recipe{} : matches.first(); }
};

QString recipeRoot(const QString &libraryRoot);
void ensureRecipeCatalog(const QString &libraryRoot);
QList<Recipe> loadRecipes(const QString &libraryRoot);
Recipe parseRecipe(const QByteArray &json, const QString &path, const QString &tier);
RecipeSelection selectRecipe(const QString &libraryRoot, const QString &gameFolder,
                             const QStringList &executables, const QString &defaultId);
bool recipeMatches(const Recipe &, const QString &gameFolder, const QStringList &executables);
QString recipePath(const QString &libraryRoot, const QString &tier, const QString &id);
void saveRecipe(const QString &libraryRoot, const QString &tier, const QByteArray &json);
void removeRecipe(const QString &libraryRoot, const QString &tier, const QString &id);
void applyRecipeToGame(Game &, const Recipe &);
QStringList recipeExecutables(const Recipe &, const QString &gameFolder, const QStringList &found);
QString resolveRunnerSelector(const QString &libraryRoot, const QString &selector);
QString resolveUmuSelector(const QString &libraryRoot, const QString &selector);

} // namespace novelty
