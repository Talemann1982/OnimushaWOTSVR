#pragma once

// [AFW 29.09.2026] Alternate Frame Warping nach dem RE9-Fork (PureDark). Pro Frame
// rendert das Spiel wie bei AFR nur EIN Auge; das Plugin (PDAFWPlugin.dll) warpt das
// andere Auge aus dem letzten Bild per Tiefe + Bewegungsvektoren (aus dem DLSS-Hook),
// und beide Augen gehen jeden Frame raus.
// Nur in VR.cpp und D3D12Component.cpp einbinden: der Plugin-Header zieht
// "using namespace pd" nach sich.

#include <atomic>
#include <d3d12.h>

#include "PDAFWPlugin.h"

namespace afw {
struct State {
    pd::EyeFrameBuffers buffers{};          // Ausgabe des Plugins (links/rechts)
    UINT buf_w{0};
    UINT buf_h{0};
    DXGI_FORMAT buf_fmt{DXGI_FORMAT_UNKNOWN};

    pd::TextureDesc mv_desc{};              // eigene Kopie der Spiel-MVs (Tiefen-Groesse)
    pd::TextureDesc depth_desc{};
    pd::TextureDesc ui_desc{};
    std::atomic<ID3D12Resource*> depth_tex{nullptr};

    pd::CameraData camera[2]{};             // pro gerendertem Auge, in VR::on_present
};

inline State& state() {
    static State s{};
    return s;
}
}
