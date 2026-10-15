#pragma once
#include <QtCore/qstring.h>
#include <QtCore/qstringlist.h>
#include <QtCore/qregularexpression.h>
namespace novelty {
// Recipe and Manage configuration are data, not a host loader/shell configuration.
// Extend this reviewed list when another game option is needed. Application-specific
// data variables may use NOVELTY_APP_*; never allow arbitrary executable/path controls.
inline bool supportedGameEnvironment(const QString &name, const QString &value) {
    static const QRegularExpression variable("^[A-Z][A-Z0-9_]{0,79}$");
    if (!variable.match(name).hasMatch() || value.contains(QChar::Null) || value.size() > 8192) return false;
    if (name.startsWith("NOVELTY_APP_") || name.startsWith("DXVK_") || name.startsWith("VKD3D_") ||
        name.startsWith("LC_")) return true;
    static const QStringList supported{
        "TZ", "LANG", "WINEDEBUG", "WINEDLLOVERRIDES", "WINEESYNC", "WINEFSYNC", "WINE_FULLSCREEN_FSR",
        "WINE_FULLSCREEN_FSR_STRENGTH", "WINE_FULLSCREEN_FSR_MODE", "WINE_FULLSCREEN_FSR_CUSTOM_MODE",
        "PROTON_LOG", "PROTON_LOG_DIR", "PROTON_NO_ESYNC", "PROTON_NO_FSYNC", "PROTON_USE_WINED3D",
        "PROTON_NO_D3D11", "PROTON_NO_D3D10", "PROTON_NO_D3D9", "PROTON_NO_D3D12", "PROTON_ENABLE_NVAPI",
        "PROTON_HIDE_NVIDIA_GPU", "PROTON_ENABLE_AMD_AGS", "PROTON_FORCE_LARGE_ADDRESS_AWARE",
        "PROTON_HEAP_DELAY_FREE", "PROTON_ENABLE_WAYLAND", "PROTON_USE_WOW64", "PROTON_SET_GAME_DRIVE",
        "PROTON_OLD_GL_STRING", "PROTON_USE_SECCOMP", "PROTON_NO_WRITE_WATCH", "PROTON_FSR4_UPGRADE",
        "MANGOHUD", "MANGOHUD_CONFIG", "DXVK_HUD", "DXVK_ASYNC", "WINE_CPU_TOPOLOGY"
    };
    return supported.contains(name);
}
}
