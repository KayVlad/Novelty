#pragma once
#include <QtCore/qstring.h>
class QWidget;
namespace novelty {
class Library;
struct Game;
// Empty result means cancelled/unfinished; caller applies a runner only when settings are saved.
QString provisionGe(QWidget *parent, const QString &libraryRoot, const QString &helper = {}, bool startImmediately = false,
                    const QString &suggestedVersion = {});
QString provisionUmu(QWidget *parent, const QString &libraryRoot, const QString &helper = {}, bool startImmediately = false);
QString requiredSteamRuntime(const QString &runner);
bool provisionSteamRuntime(QWidget *parent, const Library &library, const Game &game);
// The automatic first-Play path uses one cancellable dialog for missing GE, umu and runtime steps.
bool provisionFirstPlay(QWidget *parent, Library &library, Game &game, const QString &geVersion = {},
                        const QString &geHelper = {}, const QString &umuHelper = {});
}
