// [POST_PASS 28.09.2026] Portiert aus dem RE4-Fork (afw, vr/d3d12/SharpenPass.cpp):
// eigene Nachbearbeitung auf dem fertigen Augenbild -- Schaerfe (LumaSharpen-Art),
// Saettigung (PostPass) und SMAA 1x ULTRA (SmaaPass). Der alte CAS-SharpenPass
// des RE4-Forks ist hier bewusst NICHT dabei.
#include <algorithm>
#include <climits>
#include <cstring>
#include <string>

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include "PostPass.hpp"
#include "SmaaData.inl"

#pragma comment(lib, "d3dcompiler")

namespace d3d12 {
// ============================================================================
// [POST_PASS 28.09.2026] Schaerfe + Saettigung auf den fertigen Augenbildern.
// ============================================================================
namespace {
// Schaerfe nach Art von ReShade "LumaSharpen": Differenz zum 4er-Kreuz-Mittel,
// nur ueber die Helligkeit (keine Farbsaeume), begrenzt auf +-CLAMP_MAX.
// Danach Saettigung um die Helligkeit herum. Liest die Quelle ggf. als SRGB
// (dann linear) und schreibt bei g_srgb_out zurueck in SRGB-Kodierung, weil
// die Zwischentextur UNORM ist (SRGB-UAVs gibt es nicht).
constexpr char POST_SHADER[] = R"(
Texture2D<float4>   g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);

cbuffer Params : register(b0) {
    float g_sharp;      // 0 = aus .. 3
    float g_sat;        // 1 = unveraendert, 0 = grau, 2 = doppelt
    uint  g_width;
    uint  g_height;
    uint  g_srgb_out;   // 1 = Ergebnis nach SRGB kodieren
    float g_gamma;      // [BRIGHTNESS 29.09.2026] Exponent, 1 = unveraendert, <1 heller
    float g_contrast;   // [CONTRAST 29.09.2026] Faktor um Mittelgrau, 1 = unveraendert
};

static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
static const float  CLAMP_MAX = 0.08;

float4 tap4(int2 p) {
    p = clamp(p, int2(0, 0), int2(g_width - 1, g_height - 1));
    return g_src.Load(int3(p, 0));
}

float3 to_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_width || id.y >= g_height) {
        return;
    }

    const int2 p = int2(id.xy);
    const float4 c4 = tap4(p);
    float3 c = c4.rgb;

    if (g_sharp > 0.0) {
        const float3 blur = (tap4(p + int2(0, -1)).rgb + tap4(p + int2(0, 1)).rgb
                           + tap4(p + int2(-1, 0)).rgb + tap4(p + int2(1, 0)).rgb) * 0.25;
        float d = dot(c - blur, LUMA) * g_sharp;
        d = clamp(d, -CLAMP_MAX, CLAMP_MAX);
        c += d;
    }

    if (g_sat != 1.0) {
        const float l = dot(c, LUMA);
        c = max(lerp(float3(l, l, l), c, g_sat), 0.0);
    }

    // Ab hier in Anzeige-Kodierung (bei SRGB-Quelle erst zurueckkodieren),
    // damit Helligkeit/Kontrast so wirken, wie man es am Bildschirm erwartet.
    if (g_srgb_out != 0) {
        c = to_srgb(c);
    }

    // [BRIGHTNESS 29.09.2026] Gamma-artig: Schwarz und Weiss bleiben fest.
    if (g_gamma != 1.0) {
        c = pow(max(c, 0.0), g_gamma);
    }

    // [CONTRAST 29.09.2026] Spreizung um Mittelgrau 0.5.
    if (g_contrast != 1.0) {
        c = max((c - 0.5) * g_contrast + 0.5, 0.0);
    }

    g_dst[id.xy] = float4(c, c4.a);
}
)";

DXGI_FORMAT post_uav_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return f;
    default:
        return DXGI_FORMAT_UNKNOWN;   // typeless u. a. -- lieber nicht anfassen
    }
}

bool post_is_srgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

void post_barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}
}

bool PostPass::setup(ID3D12Device* device) {
    if (device == nullptr || m_failed) {
        return false;
    }

    if (m_pso != nullptr) {
        return true;
    }

    m_failed = true;   // wird am Ende bei Erfolg zurueckgenommen

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(D3DCompile(POST_SHADER, sizeof(POST_SHADER) - 1, nullptr, nullptr, nullptr, "main",
                          "cs_5_0", 0, 0, &shader, &errors))) {
        spdlog::error("[PostPass] Shader liess sich nicht uebersetzen: {}",
                      errors != nullptr ? (const char*)errors->GetBufferPointer() : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 7;   // [29.09.2026] +Helligkeit +Kontrast
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors))
        || FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                              IID_PPV_ARGS(&m_root_sig)))) {
        spdlog::error("[PostPass] Root Signature fehlgeschlagen");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        spdlog::error("[PostPass] CreateComputePipelineState fehlgeschlagen");
        m_root_sig.Reset();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        spdlog::error("[PostPass] CreateDescriptorHeap fehlgeschlagen");
        m_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;

    spdlog::info("[PostPass] bereit (Schaerfe + Saettigung, Compute, in place)");

    return true;
}

int32_t PostPass::ensure_target(ID3D12Device* device, ID3D12Resource* src) {
    const auto desc = src->GetDesc();

    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        auto& s = m_slots[i];

        if (s.src == src && s.target != nullptr && s.width == (uint32_t)desc.Width
            && s.height == desc.Height && s.format == desc.Format) {
            return (int32_t)i;
        }
    }

    const auto uav_format = post_uav_format(desc.Format);

    if (uav_format == DXGI_FORMAT_UNKNOWN || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || desc.SampleDesc.Count != 1) {
        return -1;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = uav_format;

    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))
        || (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        return -1;
    }

    int32_t index = -1;

    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        if (m_slots[i].src == src || m_slots[i].target == nullptr) {
            index = (int32_t)i;
            break;
        }
    }

    if (index < 0) {
        index = (int32_t)(m_next_slot % SLOT_COUNT);
        m_next_slot = (m_next_slot + 1) % SLOT_COUNT;
    }

    auto& slot = m_slots[index];
    slot.target.Reset();
    slot.src = nullptr;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = desc.Width;
    tex.Height = desc.Height;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = uav_format;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&slot.target)))) {
        spdlog::error("[PostPass] Zwischentextur konnte nicht angelegt werden");
        return -1;
    }

    slot.target->SetName(L"RE4VR PostPass target");
    slot.src = src;
    slot.width = (uint32_t)desc.Width;
    slot.height = desc.Height;
    slot.format = desc.Format;
    slot.uav_format = uav_format;

    return index;
}

bool PostPass::dispatch_inplace(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                ID3D12Resource* src, D3D12_RESOURCE_STATES src_state,
                                float sharpness, float saturation, float brightness, float contrast) {
    if (device == nullptr || cmd == nullptr || src == nullptr) {
        return false;
    }

    // Beides neutral -> gar nichts tun, das Bild bleibt bitgenau, wie es war.
    if (sharpness <= 0.0f && saturation == 1.0f && brightness == 1.0f && contrast == 1.0f) {
        return false;
    }

    // Nur ganze Texturen ohne Mips/Arrays -- sonst passt CopyResource nicht.
    {
        const auto d = src->GetDesc();

        if (d.MipLevels != 1 || d.DepthOrArraySize != 1) {
            return false;
        }
    }

    if (!setup(device)) {
        return false;
    }

    const auto slot_index = ensure_target(device, src);

    if (slot_index < 0) {
        return false;
    }

    auto& slot = m_slots[slot_index];

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * 2) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * 2) * m_descriptor_size;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = slot.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = slot.uav_format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav{cpu.ptr + m_descriptor_size};
    device->CreateUnorderedAccessView(slot.target.Get(), nullptr, &uav, cpu_uav);

    post_barrier(cmd, src, src_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);

    const float sh = std::clamp(sharpness, 0.0f, 3.0f);
    const float sa = std::clamp(saturation, 0.0f, 2.0f);
    const uint32_t srgb_out = post_is_srgb(slot.format) ? 1u : 0u;
    cmd->SetComputeRoot32BitConstants(1, 1, &sh, 0);
    cmd->SetComputeRoot32BitConstants(1, 1, &sa, 1);
    cmd->SetComputeRoot32BitConstants(1, 1, &slot.width, 2);
    cmd->SetComputeRoot32BitConstants(1, 1, &slot.height, 3);
    cmd->SetComputeRoot32BitConstants(1, 1, &srgb_out, 4);

    const float br = std::clamp(brightness, 0.25f, 4.0f);
    const float co = std::clamp(contrast, 0.0f, 3.0f);
    cmd->SetComputeRoot32BitConstants(1, 1, &br, 5);
    cmd->SetComputeRoot32BitConstants(1, 1, &co, 6);

    cmd->Dispatch((slot.width + 7) / 8, (slot.height + 7) / 8, 1);

    // Ergebnis zurueck in die Quelle; die Kopie laeuft ueber die
    // Format-Familie (SRGB <-> UNORM ist fuer CopyResource erlaubt).
    post_barrier(cmd, slot.target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    post_barrier(cmd, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    cmd->CopyResource(src, slot.target.Get());

    post_barrier(cmd, slot.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    post_barrier(cmd, src, D3D12_RESOURCE_STATE_COPY_DEST, src_state);

    return true;
}

void PostPass::reset() {
    for (auto& s : m_slots) {
        s.target.Reset();
        s.src = nullptr;
        s.width = 0;
        s.height = 0;
        s.format = DXGI_FORMAT_UNKNOWN;
        s.uav_format = DXGI_FORMAT_UNKNOWN;
    }

    m_heap.Reset();
    m_pso.Reset();
    m_root_sig.Reset();
    m_ring = 0;
    m_next_slot = 0;
    m_failed = false;
}

// ============================================================================
// [POST_PASS SMAA 28.09.2026] SMAA 1x auf den fertigen Augenbildern.
// ============================================================================
namespace {
// Vorspann: SMAA im "Custom"-Modus, Sampler/Texturen fest auf Register gelegt,
// die RT-Metrik als Konstante (b0), Qualitaet ULTRA ([28.09.2026] User: HIGH packt zu wenig zu).
constexpr char SMAA_PREFIX[] = R"(
cbuffer SmaaParams : register(b0) { float4 g_rt_metrics; };
#define SMAA_RT_METRICS g_rt_metrics
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_ULTRA 1
SamplerState LinearSampler : register(s0);
SamplerState PointSampler  : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
)";

// Nachspann: die drei Durchgaenge als Vollbild-Dreieck (ohne Vertexpuffer).
constexpr char SMAA_SUFFIX[] = R"(
Texture2D g_color  : register(t0);
Texture2D g_edges  : register(t1);
Texture2D g_blend  : register(t2);
Texture2D g_area   : register(t3);
Texture2D g_search : register(t4);

void fullscreen(uint id, out float4 pos, out float2 uv) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

struct VSEdges { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 o0 : TEXCOORD1; float4 o1 : TEXCOORD2; float4 o2 : TEXCOORD3; };

VSEdges vs_edges(uint id : SV_VertexID) {
    VSEdges o;
    fullscreen(id, o.pos, o.uv);
    float4 off[3];
    SMAAEdgeDetectionVS(o.uv, off);
    o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2];
    return o;
}

float2 ps_edges(VSEdges i) : SV_Target {
    float4 off[3] = { i.o0, i.o1, i.o2 };
    return SMAALumaEdgeDetectionPS(i.uv, off, g_color);
}

struct VSWeights { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o0 : TEXCOORD2; float4 o1 : TEXCOORD3; float4 o2 : TEXCOORD4; };

VSWeights vs_weights(uint id : SV_VertexID) {
    VSWeights o;
    fullscreen(id, o.pos, o.uv);
    float4 off[3];
    SMAABlendingWeightCalculationVS(o.uv, o.pix, off);
    o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2];
    return o;
}

float4 ps_weights(VSWeights i) : SV_Target {
    float4 off[3] = { i.o0, i.o1, i.o2 };
    return SMAABlendingWeightCalculationPS(i.uv, i.pix, off, g_edges, g_area, g_search, float4(0.0, 0.0, 0.0, 0.0));
}

struct VSBlend { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };

VSBlend vs_blend(uint id : SV_VertexID) {
    VSBlend o;
    fullscreen(id, o.pos, o.uv);
    SMAANeighborhoodBlendingVS(o.uv, o.o);
    return o;
}

float4 ps_blend(VSBlend i) : SV_Target {
    return SMAANeighborhoodBlendingPS(i.uv, i.o, g_color, g_blend);
}
)";

bool smaa_compile(const std::string& src, const char* entry, const char* profile, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> errors{};

    if (FAILED(D3DCompile(src.data(), src.size(), "SMAA", nullptr, nullptr, entry, profile,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors))) {
        spdlog::error("[SmaaPass] {} liess sich nicht uebersetzen: {}", entry,
                      errors != nullptr ? (const char*)errors->GetBufferPointer() : "?");
        return false;
    }

    return true;
}

bool smaa_format_ok(ID3D12Device* device, DXGI_FORMAT f) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = f;

    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))) {
        return false;
    }

    return (fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0
        && (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
}

ComPtr<ID3D12Resource> smaa_texture(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT f,
                                    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = w;
    tex.Height = h;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = f;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Flags = flags;

    ComPtr<ID3D12Resource> res{};

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, state, nullptr,
                                               IID_PPV_ARGS(&res)))) {
        return nullptr;
    }

    res->SetName(name);

    return res;
}

bool smaa_graphics_pso(ID3D12Device* device, ID3D12RootSignature* rs, ID3DBlob* vs, ID3DBlob* ps,
                       DXGI_FORMAT rtv_format, ComPtr<ID3D12PipelineState>& out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.DepthStencilState.StencilEnable = FALSE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv_format;
    d.SampleDesc.Count = 1;

    return SUCCEEDED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)));
}
}

bool SmaaPass::setup(ID3D12Device* device) {
    if (device == nullptr || m_failed) {
        return false;
    }

    if (m_pso_edges != nullptr && m_pso_weights != nullptr) {
        return true;
    }

    m_failed = true;   // bei Erfolg am Ende zurueckgenommen

    std::string src{SMAA_PREFIX};

    for (const auto* chunk : smaa_data::SHADER_CHUNKS) {
        src += chunk;
    }

    src += SMAA_SUFFIX;

    if (!smaa_compile(src, "vs_edges", "vs_5_0", m_vs_edges) || !smaa_compile(src, "ps_edges", "ps_5_0", m_ps_edges)
        || !smaa_compile(src, "vs_weights", "vs_5_0", m_vs_weights) || !smaa_compile(src, "ps_weights", "ps_5_0", m_ps_weights)
        || !smaa_compile(src, "vs_blend", "vs_5_0", m_vs_blend) || !smaa_compile(src, "ps_blend", "ps_5_0", m_ps_blend)) {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = SRV_PER_DRAW;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (auto i = 0; i < 2; ++i) {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 2;
    rs_desc.pStaticSamplers = samplers;

    ComPtr<ID3DBlob> rs_blob{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors))
        || FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                              IID_PPV_ARGS(&m_root_sig)))) {
        spdlog::error("[SmaaPass] Root Signature fehlgeschlagen");
        return false;
    }

    if (!smaa_graphics_pso(device, m_root_sig.Get(), m_vs_edges.Get(), m_ps_edges.Get(), DXGI_FORMAT_R8G8_UNORM, m_pso_edges)
        || !smaa_graphics_pso(device, m_root_sig.Get(), m_vs_weights.Get(), m_ps_weights.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, m_pso_weights)) {
        spdlog::error("[SmaaPass] Grafik-PSO fehlgeschlagen");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = RING_COUNT * SRV_PER_DRAW;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = SLOT_COUNT * 3;

    if (FAILED(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&m_srv_heap)))
        || FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&m_rtv_heap)))) {
        spdlog::error("[SmaaPass] Deskriptor-Heaps fehlgeschlagen");
        return false;
    }

    m_srv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    m_area = smaa_texture(device, 160, 560, DXGI_FORMAT_R8G8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, L"RE4VR SMAA AreaTex");
    m_search = smaa_texture(device, 64, 16, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_COPY_DEST, L"RE4VR SMAA SearchTex");

    if (m_area == nullptr || m_search == nullptr) {
        spdlog::error("[SmaaPass] Lookup-Texturen fehlgeschlagen");
        return false;
    }

    m_failed = false;
    spdlog::info("[SmaaPass] bereit (SMAA 1x ULTRA)");

    return true;
}

bool SmaaPass::ensure_pso_out(ID3D12Device* device, DXGI_FORMAT format) {
    if (m_pso_out != nullptr && m_pso_out_format == format) {
        return true;
    }

    m_pso_out.Reset();

    if (!smaa_graphics_pso(device, m_root_sig.Get(), m_vs_blend.Get(), m_ps_blend.Get(), format, m_pso_out)) {
        spdlog::error("[SmaaPass] Ausgabe-PSO fuer Format {} fehlgeschlagen", (int)format);
        m_pso_out_format = DXGI_FORMAT_UNKNOWN;
        return false;
    }

    m_pso_out_format = format;

    return true;
}

bool SmaaPass::upload_luts(ID3D12Device* device, ID3D12GraphicsCommandList* cmd) {
    if (m_luts_ready) {
        return true;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]{};
    UINT rows[2]{};
    UINT64 row_bytes[2]{};
    UINT64 sizes[2]{};

    const auto area_desc = m_area->GetDesc();
    const auto search_desc = m_search->GetDesc();

    device->GetCopyableFootprints(&area_desc, 0, 1, 0, &fp[0], &rows[0], &row_bytes[0], &sizes[0]);

    const UINT64 search_offset = (sizes[0] + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)
                                 & ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);

    device->GetCopyableFootprints(&search_desc, 0, 1, search_offset, &fp[1], &rows[1], &row_bytes[1], &sizes[1]);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = search_offset + sizes[1];
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&m_upload)))) {
        spdlog::error("[SmaaPass] Upload-Puffer fehlgeschlagen");
        return false;
    }

    uint8_t* mapped = nullptr;

    if (FAILED(m_upload->Map(0, nullptr, (void**)&mapped)) || mapped == nullptr) {
        return false;
    }

    // Zeilenweise, weil die Zeilen im Upload-Puffer auf 256 Byte ausgerichtet sind.
    for (UINT y = 0; y < rows[0]; ++y) {
        memcpy(mapped + fp[0].Offset + (UINT64)y * fp[0].Footprint.RowPitch, smaa_data::AREA_TEX + (size_t)y * 160 * 2, 160 * 2);
    }

    for (UINT y = 0; y < rows[1]; ++y) {
        memcpy(mapped + fp[1].Offset + (UINT64)y * fp[1].Footprint.RowPitch, smaa_data::SEARCH_TEX + (size_t)y * 64, 64);
    }

    m_upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{}, srcl{};
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcl.pResource = m_upload.Get();

    dst.pResource = m_area.Get();
    srcl.PlacedFootprint = fp[0];
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);

    dst.pResource = m_search.Get();
    srcl.PlacedFootprint = fp[1];
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);

    post_barrier(cmd, m_area.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    post_barrier(cmd, m_search.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_luts_ready = true;

    return true;
}

int32_t SmaaPass::ensure_slot(ID3D12Device* device, ID3D12Resource* src) {
    const auto desc = src->GetDesc();

    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        auto& s = m_slots[i];

        if (s.src == src && s.out != nullptr && s.width == (uint32_t)desc.Width && s.height == desc.Height
            && s.format == desc.Format) {
            return (int32_t)i;
        }
    }

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1
        || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || !smaa_format_ok(device, desc.Format)) {
        return -1;
    }

    int32_t index = -1;

    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        if (m_slots[i].src == src || m_slots[i].out == nullptr) {
            index = (int32_t)i;
            break;
        }
    }

    if (index < 0) {
        index = (int32_t)(m_next_slot % SLOT_COUNT);
        m_next_slot = (m_next_slot + 1) % SLOT_COUNT;
    }

    auto& slot = m_slots[index];
    slot = Slot{};

    const auto w = (uint32_t)desc.Width;
    const auto h = desc.Height;
    const auto rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto psr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    slot.edges = smaa_texture(device, w, h, DXGI_FORMAT_R8G8_UNORM, rt, psr, L"RE4VR SMAA edges");
    slot.blend = smaa_texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, rt, psr, L"RE4VR SMAA blend");
    slot.out = smaa_texture(device, w, h, desc.Format, rt, psr, L"RE4VR SMAA out");

    if (slot.edges == nullptr || slot.blend == nullptr || slot.out == nullptr) {
        spdlog::error("[SmaaPass] Zwischentexturen fehlgeschlagen");
        slot = Slot{};
        return -1;
    }

    slot.src = src;
    slot.width = w;
    slot.height = h;
    slot.format = desc.Format;
    slot.rtv_base = (uint32_t)index * 3;

    auto rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)slot.rtv_base * m_rtv_size;
    device->CreateRenderTargetView(slot.edges.Get(), nullptr, rtv);
    rtv.ptr += m_rtv_size;
    device->CreateRenderTargetView(slot.blend.Get(), nullptr, rtv);
    rtv.ptr += m_rtv_size;
    device->CreateRenderTargetView(slot.out.Get(), nullptr, rtv);

    return index;
}

bool SmaaPass::dispatch_inplace(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                ID3D12Resource* src, D3D12_RESOURCE_STATES src_state) {
    if (device == nullptr || cmd == nullptr || src == nullptr) {
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto slot_index = ensure_slot(device, src);

    if (slot_index < 0 || !ensure_pso_out(device, m_slots[slot_index].format) || !upload_luts(device, cmd)) {
        return false;
    }

    auto& slot = m_slots[slot_index];

    // Deskriptoren: t0 Farbe, t1 Kanten, t2 Gewichte, t3 Area, t4 Search.
    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_srv_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_srv_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * SRV_PER_DRAW) * m_srv_size;
    gpu.ptr += (UINT64)(ring * SRV_PER_DRAW) * m_srv_size;

    ID3D12Resource* srvs[SRV_PER_DRAW]{src, slot.edges.Get(), slot.blend.Get(), m_area.Get(), m_search.Get()};

    for (uint32_t i = 0; i < SRV_PER_DRAW; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + (SIZE_T)i * m_srv_size};
        device->CreateShaderResourceView(srvs[i], nullptr, h);
    }

    auto rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)slot.rtv_base * m_rtv_size;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_edges{rtv.ptr};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_blend{rtv.ptr + m_rtv_size};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_out{rtv.ptr + 2 * (SIZE_T)m_rtv_size};

    const auto psr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto rts = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const float zero[4]{0.0f, 0.0f, 0.0f, 0.0f};

    post_barrier(cmd, src, src_state, psr);

    ID3D12DescriptorHeap* heaps[]{m_srv_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_root_sig.Get());
    cmd->SetGraphicsRootDescriptorTable(0, gpu);

    const float metrics[4]{1.0f / (float)slot.width, 1.0f / (float)slot.height, (float)slot.width, (float)slot.height};
    cmd->SetGraphicsRoot32BitConstants(1, 4, metrics, 0);

    D3D12_VIEWPORT vp{0.0f, 0.0f, (float)slot.width, (float)slot.height, 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, (LONG)slot.width, (LONG)slot.height};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 1) Kanten
    post_barrier(cmd, slot.edges.Get(), psr, rts);
    cmd->ClearRenderTargetView(rtv_edges, zero, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtv_edges, FALSE, nullptr);
    cmd->SetPipelineState(m_pso_edges.Get());
    cmd->DrawInstanced(3, 1, 0, 0);
    post_barrier(cmd, slot.edges.Get(), rts, psr);

    // 2) Mischgewichte
    post_barrier(cmd, slot.blend.Get(), psr, rts);
    cmd->ClearRenderTargetView(rtv_blend, zero, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtv_blend, FALSE, nullptr);
    cmd->SetPipelineState(m_pso_weights.Get());
    cmd->DrawInstanced(3, 1, 0, 0);
    post_barrier(cmd, slot.blend.Get(), rts, psr);

    // 3) Nachbarn mischen -> out
    post_barrier(cmd, slot.out.Get(), psr, rts);
    cmd->OMSetRenderTargets(1, &rtv_out, FALSE, nullptr);
    cmd->SetPipelineState(m_pso_out.Get());
    cmd->DrawInstanced(3, 1, 0, 0);

    // Ergebnis zurueck in die Quelle
    post_barrier(cmd, slot.out.Get(), rts, D3D12_RESOURCE_STATE_COPY_SOURCE);
    post_barrier(cmd, src, psr, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(src, slot.out.Get());
    post_barrier(cmd, slot.out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, psr);
    post_barrier(cmd, src, D3D12_RESOURCE_STATE_COPY_DEST, src_state);

    return true;
}

void SmaaPass::reset() {
    for (auto& s : m_slots) {
        s = Slot{};
    }

    m_srv_heap.Reset();
    m_rtv_heap.Reset();
    m_pso_edges.Reset();
    m_pso_weights.Reset();
    m_pso_out.Reset();
    m_pso_out_format = DXGI_FORMAT_UNKNOWN;
    m_root_sig.Reset();
    m_vs_edges.Reset();
    m_ps_edges.Reset();
    m_vs_weights.Reset();
    m_ps_weights.Reset();
    m_vs_blend.Reset();
    m_ps_blend.Reset();
    m_area.Reset();
    m_search.Reset();
    m_upload.Reset();
    m_luts_ready = false;
    m_ring = 0;
    m_next_slot = 0;
    m_failed = false;
}
}
