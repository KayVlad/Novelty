#include "profiles.h"
#include <stdexcept>

namespace novelty {
const QList<SetupProfile> &setupProfiles() {
    // IDs are immutable versions. Bump preparation when prefix actions or defaults change.
    // Both baselines currently share the same preparation and preserve generic receipts.
    static const QList<SetupProfile> profiles{
        {"generic-proton-v1", "Generic VN · v1", "generic-proton-v1",
         "Prepare a dedicated Proton prefix with GE-Proton11-7 as the default runner. No extra dependencies or compatibility overrides. Your locale and environment settings are kept.", "GE-Proton11-7"},
        {"generic-game-v1", "Generic game · v1", "generic-proton-v1",
         "Prepare a dedicated Proton prefix for a general Windows game with GE-Proton11-7 as the default runner. No extra dependencies or compatibility overrides. Your locale and environment settings are kept.", "GE-Proton11-7"},
        {"renpy-bundled-v1", "Ren’Py bundled runtime · v1", "generic-proton-v1",
         "Use the game's bundled Ren’Py runtime with generic Proton preparation. No extra fonts, codecs or renderer overrides. GE-Proton gameplay validation is still pending.", {}}
    };
    return profiles;
}
const SetupProfile *findSetupProfile(const QString &id) {
    for (const auto &profile : setupProfiles()) if (profile.id == id) return &profile;
    return nullptr;
}
const SetupProfile &resolveSetupProfile(const Game &g) {
    if (const auto profile = findSetupProfile(g.setupProfile)) return *profile;
    throw std::runtime_error(("Unknown setup profile: " + g.setupProfile +
        ". Choose a supported profile in Manage → Runtime and save.").toStdString());
}
QString defaultSetupProfile(const QString &engine) {
    return recommendSetupProfile(engine).profileId;
}
SetupRecommendation recommendSetupProfile(const QString &engine, const QString &gameIdentity,
                                           const QString &releaseIdentity,
                                           const QList<SetupRule> &verifiedRules) {
    for (const auto &rule : verifiedRules)
        if (!findSetupProfile(rule.profileId) ||
            (rule.gameIdentity.isEmpty() != rule.releaseIdentity.isEmpty()) ||
            (rule.gameIdentity.isEmpty() && rule.engine.isEmpty()))
            throw std::runtime_error("A setup recipe is incomplete or names an unknown profile.");
    for (const auto &rule : verifiedRules)
        if (!rule.gameIdentity.isEmpty() && !rule.releaseIdentity.isEmpty() &&
            rule.gameIdentity == gameIdentity && rule.releaseIdentity == releaseIdentity) {
            return {rule.profileId, "verified game and release recipe"};
        }
    for (const auto &rule : verifiedRules)
        if (rule.gameIdentity.isEmpty() && !rule.engine.isEmpty() && rule.engine == engine) {
            return {rule.profileId, "verified engine recipe"};
        }
    if (engine == "Ren'Py") return {"renpy-bundled-v1", "bundled Ren'Py engine baseline"};
    if (engine == "Generic" || engine == "Unknown" || engine.isEmpty())
        return {"generic-game-v1", "generic game baseline"};
    return {"generic-proton-v1", "generic VN baseline"};
}
QString preferredProtonVersion(const QString &profileId) {
    if (const auto profile = findSetupProfile(profileId)) return profile->preferredProtonVersion;
    return {};
}
}
