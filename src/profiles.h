#pragma once
#include "core.h"

namespace novelty {
struct SetupProfile {
    QString id, title, preparation, description, preferredProtonVersion;
};
struct SetupRule {
    QString gameIdentity, releaseIdentity, engine, profileId;
};
struct SetupRecommendation {
    QString profileId, source;
};
const QList<SetupProfile> &setupProfiles();
const SetupProfile *findSetupProfile(const QString &id);
const SetupProfile &resolveSetupProfile(const Game &);
// Used at import or by an explicit reset. Existing records never follow changing defaults.
QString defaultSetupProfile(const QString &engine);
SetupRecommendation recommendSetupProfile(const QString &engine, const QString &gameIdentity = {},
                                           const QString &releaseIdentity = {},
                                           const QList<SetupRule> &verifiedRules = {});
QString preferredProtonVersion(const QString &profileId);
}
