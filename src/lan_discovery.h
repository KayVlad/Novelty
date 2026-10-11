#pragma once
#include "devices.h"
#include <QtCore/qobject.h>
#include <QtCore/qtimer.h>
#include <functional>

namespace novelty {
// Announcements are hints only. Callers must authenticate TLS before remembering
// an endpoint or showing a catalogue. No titles or library paths are broadcast.
class LanDiscovery : public QObject {
public:
    LanDiscovery(Library &, quint16 servicePort, QObject *parent = nullptr);
    ~LanDiscovery() override;
    std::function<void(const TrustedPeer &)> onPeer;
    void announce();
private:
    void poll();
    Library &library_;
    QString fingerprint_;
    quint16 port_;
    int socket_ = -1;
    QTimer beacon_, poll_;
};
}
