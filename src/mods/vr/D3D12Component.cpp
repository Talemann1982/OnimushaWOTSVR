#include <openvr.h>
#include <utility/ScopeGuard.hpp>

#include "../VR.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>

#include "D3D12Component.hpp"
#include "AFW.hpp"   // [AFW 29.09.2026]

namespace vrmod {
// [POST_PASS 28.09.2026 -- Ansage des Users, portiert aus dem RE4-Fork] Eigene
// Nachbearbeitung wie ReShade: erst SMAA, dann Schaerfe/Saettigung -- auf dem
// fertigen Augenbild, bevor es in die OpenXR-/OpenVR-Swapchain kopiert wird.
// Onimusha rendert per AFR (ein Auge pro Frame), ohne Upscaler; `eye` ist der
// Backbuffer (8 Bit, PRESENT) oder die konvertierte 8-Bit-Textur
// (PIXEL_SHADER_RESOURCE). Der Backbuffer taugt nicht als SRV -> Arbeitskopie:
// eye -> m_post_work, Passes an Ort und Stelle auf m_post_work, zurueck nach eye.
void D3D12Component::apply_post_pass(VR* vr, ID3D12Device* device, ID3D12Resource* eye, D3D12_RESOURCE_STATES eye_state, uint64_t frame_count) {
    if (vr == nullptr || device == nullptr || eye == nullptr) {
        return;
    }

    const float sharp = vr->post_sharpness();
    const float sat = vr->post_saturation();
    const float bright = vr->post_brightness();   // [29.09.2026]
    const float contrast = vr->post_contrast();   // [29.09.2026]
    const bool smaa = vr->post_smaa();

    if (!smaa && sharp <= 0.0f && sat == 1.0f && bright == 1.0f && contrast == 1.0f) {
        return;
    }

    auto desc = eye->GetDesc();

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 || desc.DepthOrArraySize != 1) {
        return;
    }

    // Typeless-Backbuffer: Arbeitskopie im passenden UNORM/FLOAT-Format
    // (CopyResource erlaubt das innerhalb derselben Format-Familie).
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    default: break;
    }

    if (m_post_work != nullptr) {
        const auto w = m_post_work->GetDesc();

        if (w.Width != desc.Width || w.Height != desc.Height || w.Format != desc.Format) {
            m_post_work.Reset();
        }
    }

    if (m_post_work == nullptr) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC tex{};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = desc.Width;
        tex.Height = desc.Height;
        tex.DepthOrArraySize = 1;
        tex.MipLevels = 1;
        tex.Format = desc.Format;
        tex.SampleDesc.Count = 1;
        tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tex.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&m_post_work)))) {
            spdlog::error("[PostPass] Arbeitskopie konnte nicht angelegt werden");
            return;
        }

        m_post_work->SetName(L"ONI PostPass work");
    }

    auto& commands = m_post_commands[frame_count % m_post_commands.size()];

    if (commands.cmd_list == nullptr) {
        return;
    }

    commands.wait(INFINITE);

    auto* cmd = commands.cmd_list.Get();
    auto* work = m_post_work.Get();

    const auto barrier = [cmd](ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
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
    };

    // eye -> Arbeitskopie (work ruht in COPY_DEST)
    barrier(eye, eye_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(work, eye);

    bool any = false;

    if (smaa) {
        any |= m_smaa_pass.dispatch_inplace(device, cmd, work, D3D12_RESOURCE_STATE_COPY_DEST);
    }

    any |= m_post_pass.dispatch_inplace(device, cmd, work, D3D12_RESOURCE_STATE_COPY_DEST, sharp, sat, bright, contrast);

    // Arbeitskopie -> eye (nur wenn wirklich etwas bearbeitet wurde)
    if (any) {
        barrier(work, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(eye, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(eye, work);
        barrier(eye, D3D12_RESOURCE_STATE_COPY_DEST, eye_state);
        barrier(work, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    } else {
        barrier(eye, D3D12_RESOURCE_STATE_COPY_SOURCE, eye_state);
    }

    commands.has_commands = true;
    commands.execute();
}

// [AFW 29.09.2026 -- nach RE9-Fork D3D12Component::on_frame "Frame Warp Module"]
// TextureDesc je Ressource einmal beim Plugin anmelden (Views), danach aus dem Cache.
static pd::TextureDesc& afw_desc_for(pd::D3D12RendererAPI* r, std::array<pd::TextureDesc, 8>& cache, size_t& next,
                                     ID3D12Resource* res, D3D12_RESOURCE_STATES state) {
    for (auto& d : cache) {
        if (d.pTexture == res && d.initialState == state) {
            return d;
        }
    }

    auto& d = cache[next++ % cache.size()];
    d = pd::TextureDesc{};
    d.pTexture = res;
    d.initialState = state;
    r->SetupTextureDesc(d);
    return d;
}

bool D3D12Component::afw_frame(VR* vr, ID3D12CommandQueue* command_queue, ID3D12Resource* eye_texture,
                               D3D12_RESOURCE_STATES eye_state, UINT backbuffer_index, vr::EVRCompositorError& e_out) {
    e_out = vr::VRCompositorError_None;

    auto* r = vr->afw_renderer();
    auto& st = afw::state();

    if (r == nullptr || eye_texture == nullptr) {
        return false;
    }

    auto runtime = vr->get_runtime();
    const auto eye_desc = eye_texture->GetDesc();

    // Ausgabepuffer des Plugins in Augen-Groesse/-Format (einmal pro Groesse versuchen).
    if (st.buf_w != (UINT)eye_desc.Width || st.buf_h != eye_desc.Height || st.buf_fmt != eye_desc.Format) {
        st.buf_w = (UINT)eye_desc.Width;
        st.buf_h = eye_desc.Height;
        st.buf_fmt = eye_desc.Format;

        pd::FrameWarpInitParams init{};
        init.hmdWidth = (int)eye_desc.Width;
        init.hmdHeight = (int)eye_desc.Height;
        init.eyeFormat = eye_desc.Format;
        st.buffers = pd::InitFrameWarp(init);
    }

    auto& buf_l = st.buffers.eyeFrameBuffers[0].color;
    auto& buf_r = st.buffers.eyeFrameBuffers[1].color;

    if (buf_l.pTexture == nullptr || buf_r.pTexture == nullptr) {
        return false;
    }

    auto& left_ctx = m_openvr.get_left();
    auto& right_ctx = m_openvr.get_right();

    if (left_ctx.texture == nullptr || right_ctx.texture == nullptr) {
        return false;
    }

    static std::array<pd::TextureDesc, 8> s_color_cache{};
    static size_t s_color_next{0};
    static std::array<pd::TextureDesc, 8> s_out_cache{};
    static size_t s_out_next{0};

    auto& color = afw_desc_for(r, s_color_cache, s_color_next, eye_texture, eye_state);

    // Tiefe (on_pre_end_rendering) und daraus die Groesse der MV-Textur.
    auto* depth = st.depth_tex.load();

    if (depth != st.depth_desc.pTexture) {
        st.depth_desc = pd::TextureDesc{};
        st.depth_desc.pTexture = depth;
        st.depth_desc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (depth != nullptr) {
            r->SetupTextureDesc(st.depth_desc);
            st.depth_desc.type = pd::Depth;
        }
    }

    if (depth != nullptr) {
        const auto dd = depth->GetDesc();

        if (st.mv_desc.pTexture == nullptr || st.mv_desc.pTexture->GetDesc().Width != dd.Width || st.mv_desc.pTexture->GetDesc().Height != dd.Height) {
            r->CreateTexture((int)dd.Width, (int)dd.Height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, st.mv_desc, true);
        }
    }

    // UI-Puffer: das Plugin legt das UI unverzerrt drueber statt es mitzuwarpen.
    auto* ui = vr->m_ui_buffer_tex.load();

    if (ui != st.ui_desc.pTexture) {
        st.ui_desc = pd::TextureDesc{};
        st.ui_desc.pTexture = ui;
        st.ui_desc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (ui != nullptr) {
            r->SetupTextureDesc(st.ui_desc);
        }
    }

    const int eye = (vr->m_render_frame_count % 2 == vr->m_left_eye_interval) ? 0 : 1;
    auto* cmd_list = r->BeginCommandList((int)backbuffer_index);

    if (depth != nullptr && st.mv_desc.pTexture != nullptr) {
        static pd::FrameBufferDesc s_in{};
        s_in.color = color;
        s_in.depth = st.depth_desc;
        s_in.motionVectors = st.mv_desc;

        pd::FrameWarpEvaluateParams params{};
        params.InCmdList = cmd_list;
        params.InEyeFrameBuffer = &s_in;
        params.InUIColorAlpha = st.ui_desc.pTexture != nullptr ? &st.ui_desc : nullptr;
        params.IsHudlessColor = st.ui_desc.pTexture == nullptr;
        params.MotionVectorsType = vr->is_fix_dlss() ? pd::Normal : pd::FromOtherEye;
        params.InMotionScale[0] = (float)eye_desc.Width;
        params.InMotionScale[1] = (float)eye_desc.Height;
        params.Mode = pd::CombinedWarping;
        params.EyeIndex = eye == 0 ? pd::EyeLeft : pd::EyeRight;
        params.ClearBeforeWarping = false;
        params.CameraData = &st.camera[eye];
        params.IgnoreMotionThreshold = 2.5f;
        params.Debug = false;

        pd::EvaluateFrameWarp(params);
    } else {
        // Noch keine Tiefe: nur das gerenderte Auge weiterreichen (anderes Auge = letztes Bild).
        r->Blit(cmd_list, eye == 0 ? buf_l : buf_r, color);
    }

    // Plugin-Ergebnis in unsere Augen-Texturen (Format/Groesse gleicht der Blit an).
    r->Blit(cmd_list, afw_desc_for(r, s_out_cache, s_out_next, left_ctx.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), buf_l);
    r->Blit(cmd_list, afw_desc_for(r, s_out_cache, s_out_next, right_ctx.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), buf_r);

    r->EndCommandList((int)backbuffer_index);

    if (runtime->is_openxr() && vr->m_openxr->ready()) {
        m_openxr.copy(0, left_ctx.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_openxr.copy(1, right_ctx.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    if (runtime->is_openvr()) {
        vr::D3D12TextureData_t left{left_ctx.texture.Get(), command_queue, 0};
        vr::Texture_t left_eye{(void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

        auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &vr->m_left_bounds);

        if (e != vr::VRCompositorError_None) {
            e_out = e;
            return true;
        }

        vr::D3D12TextureData_t right{right_ctx.texture.Get(), command_queue, 0};
        vr::Texture_t right_eye{(void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

        e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &vr->m_right_bounds);

        if (e != vr::VRCompositorError_None) {
            e_out = e;
            return true;
        }

        vr->m_submitted = true;
        ++m_openvr.texture_counter;
    }

    return true;
}

vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    if (m_openvr.left_eye_tex[0].texture == nullptr || m_force_reset) {
        setup();
    }

    auto& hook = g_framework->get_d3d12_hook();
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};

    const auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer");
        return vr::VRCompositorError_None;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    if (!m_backbuffer_is_8bit) {
        auto command_list = m_backbuffer_copy.commands.cmd_list.Get();
        m_backbuffer_copy.commands.wait(INFINITE);

        // Copy current backbuffer into our copy so we can use it as an SRV.
        m_backbuffer_copy.commands.copy(backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

        float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
        m_backbuffer_copy.commands.clear_rtv(m_converted_eye_tex, clear_color, D3D12_RESOURCE_STATE_PRESENT);

        // Convert the backbuffer to 8-bit.
        render_srv_to_rtv(command_list, m_backbuffer_copy, m_converted_eye_tex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        m_backbuffer_copy.commands.execute();
    }

    auto eye_texture = m_backbuffer_is_8bit ? backbuffer : m_converted_eye_tex.texture;

    auto runtime = vr->get_runtime();
    const auto frame_count = vr->m_render_frame_count;

    // [POST_PASS 28.09.2026] SMAA/Schaerfe/Saettigung auf das fertige Auge, bevor es
    // unten in die Swapchain kopiert wird. Zustand: Backbuffer PRESENT, konvertierte
    // Textur PIXEL_SHADER_RESOURCE (render_srv_to_rtv oben).
    apply_post_pass(vr, device, eye_texture.Get(),
                    m_backbuffer_is_8bit ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    frame_count);

    // [AFW 29.09.2026] Warp + beide Augen jeden Frame; sonst der normale AFR-Weg darunter.
    vr::EVRCompositorError afw_error = vr::VRCompositorError_None;
    const bool afw_done = vr->is_using_afw()
        && afw_frame(vr, command_queue, eye_texture.Get(),
                     m_backbuffer_is_8bit ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     backbuffer_index, afw_error);

    if (afw_error != vr::VRCompositorError_None) {
        return afw_error;
    }

    // If m_frame_count is even, we're rendering the left eye.
    if (afw_done) {
        // [AFW] schon abgegeben
    } else if (frame_count % 2 == vr->m_left_eye_interval) {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            m_openxr.copy(0, eye_texture.Get());
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture (m_left_eye_tex0 holds the intermediate frame).
        if (runtime->is_openvr()) {
            m_openvr.copy_left(eye_texture.Get());

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::Texture_t left_eye{(void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &vr->m_left_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
        }
    } else {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            m_openxr.copy(1, eye_texture.Get());
        }

        // OpenVR texture
        // Copy the back buffer to the right eye texture.
        if (runtime->is_openvr()) {
            m_openvr.copy_right(eye_texture.Get());

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::Texture_t right_eye{(void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &vr->m_right_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }

            ++m_openvr.texture_counter;
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (frame_count % 2 == vr->m_right_eye_interval || afw_done) {   // [AFW] jeden Frame
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->ready() && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE) {
            runtime->synchronize_frame();

            if (!runtime->got_first_poses) {
                runtime->update_poses();
            }
        }

        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

            auto result = vr->m_openxr->end_frame();

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame();
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        if (vr->m_desktop_fix->value()) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                auto& copier = m_generic_copiers[frame_count % m_generic_copiers.size()];
                copier.wait(INFINITE);
                copier.copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                copier.execute();
            }
        }
    }

    // [ONI_UIBUF] UI-Puffer der Engine leeren (PureDark RE9AFW D3D12Component::on_frame). Die
    // Overlay-Schicht, die das sonst macht, laeuft bei "Allow Engine Overlays" AUS nicht.
    if (auto ui_buf = vr->m_ui_buffer_tex.load(); ui_buf != nullptr) {
        if (ui_buf != m_ui_buffer_src) {
            m_ui_buffer.reset();
            m_ui_buffer_src = ui_buf;
            m_ui_buffer_failed = !m_ui_buffer.setup(device, ui_buf, std::nullopt, std::nullopt, L"ONI UI buffer");
        }

        if (!m_ui_buffer_failed) {
            const float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
            m_ui_buffer.commands.wait(INFINITE);
            m_ui_buffer.commands.clear_rtv(m_ui_buffer, clear_color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_ui_buffer.commands.execute();
        }
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

void D3D12Component::on_post_present(VR* vr) {
}

void D3D12Component::on_reset(VR* vr) {
    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& copier : m_generic_copiers) {
        copier.reset();
    }

    // [POST_PASS 28.09.2026]
    for (auto& commands : m_post_commands) {
        commands.reset();
    }

    m_post_pass.reset();
    m_smaa_pass.reset();
    m_post_work.Reset();
    
    m_prev_backbuffer.Reset();
    m_backbuffer_copy.reset();
    m_converted_eye_tex.reset();
    m_ui_buffer.reset();         // [ONI_UIBUF]
    m_ui_buffer_src = nullptr;
    m_ui_buffer_failed = false;
    vr->m_ui_buffer_tex = nullptr;

    if (runtime->is_openxr() && runtime->loaded) {
        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height()) {
            m_openxr.create_swapchains();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_openvr.texture_counter = 0;
}

void D3D12Component::setup() {
    spdlog::info("[VR] Setting up d3d12 textures...");
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return;
    }

    const auto backbuffer_desc = backbuffer->GetDesc();

    m_backbuffer_is_8bit = backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;

    auto backbuffer_srv_desc = backbuffer_desc;
    backbuffer_srv_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_srv_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    // Create copy of backbuffer to use as SRV to convert from HDR to 8bit
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> backbuffer_copy{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_srv_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(backbuffer_copy.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up backbuffer copy texture RTV/SRV.");
        }
    }

    auto rt_desc = backbuffer_desc;

    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    spdlog::info("[VR] D3D12 Backbuffer width: {}, height: {}", backbuffer_desc.Width, backbuffer_desc.Height);

    // Create converted eye texture
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create converted eye texture.");
            return;
        }

        if (!m_converted_eye_tex.setup(device, eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up converted eye texture RTV/SRV.");
        }
    }

    for (auto& ctx : m_openvr.left_eye_tex) {
        ComPtr<ID3D12Resource> left_eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(left_eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create left eye texture.");
            return;
        }

        left_eye_tex->SetName(L"OpenVR Left Eye Texture");
        if (!ctx.setup(device, left_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up left eye texture RTV/SRV.");
        }
    }
 
    for (auto& ctx : m_openvr.right_eye_tex) {
        ComPtr<ID3D12Resource> right_eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(right_eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create right eye texture.");
            return;
        }

        right_eye_tex->SetName(L"OpenVR Right Eye Texture");
        if (!ctx.setup(device, right_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up right eye texture RTV/SRV.");
        }
    }

    for (auto& copier : m_generic_copiers) {
        copier.setup();
    }

    // [POST_PASS 28.09.2026]
    for (auto& commands : m_post_commands) {
        commands.setup(L"Post Pass Commands");
    }

    setup_sprite_batch_pso(rt_desc.Format);

    m_backbuffer_size[0] = rt_desc.Width;
    m_backbuffer_size[1] = rt_desc.Height;

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;
}

void D3D12Component::setup_sprite_batch_pso(DXGI_FORMAT output_format) {
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    DirectX::RenderTargetState output_state{output_format, DXGI_FORMAT_UNKNOWN};
    DirectX::SpriteBatchPipelineStateDescription pd{output_state};

    m_sprite_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");
}

void D3D12Component::render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    const auto dst_desc = dst.texture->GetDesc();
    const auto src_desc = src.texture->GetDesc();
    
    auto& batch = m_sprite_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)dst_desc.Width;
    viewport.Height = (float)dst_desc.Height;
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)dst_desc.Width;
    scissor_rect.bottom = (LONG)dst_desc.Height;

    // Transition dst to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst.texture.Get();

    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = src_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
    }

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { dst.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height };

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { src.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(src.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)src_desc.Width, (uint32_t)src_desc.Height },
        dest_rect,
        DirectX::Colors::White);

    batch->End();

    // Transition dst to dst_state
    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = dst_state;
        command_list->ResourceBarrier(1, &barrier);
    }
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();
    this->contexts.resize(openxr->views.size());
    
    for (auto i = 0; i < openxr->views.size(); ++i) {
        spdlog::info("[VR] Creating swapchain for eye {}", i);
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        backbuffer_desc.Width = vr->get_hmd_width();
        backbuffer_desc.Height = vr->get_hmd_height();
        backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        // Create the swapchain.
        XrSwapchainCreateInfo swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_create_info.arraySize = 1;
        swapchain_create_info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        swapchain_create_info.width = backbuffer_desc.Width;
        swapchain_create_info.height = backbuffer_desc.Height;
        swapchain_create_info.mipCount = 1;
        swapchain_create_info.faceCount = 1;
        swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
        swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains.push_back(swapchain);

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        spdlog::info("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR Commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

	if (this->contexts.empty()) {
        return;
    }

    spdlog::info("[VR] Destroying swapchains.");

    for (auto i = 0; i < this->contexts.size(); ++i) {
        auto& ctx = this->contexts[i];
        ctx.texture_contexts.clear();

        auto result = xrDestroySwapchain(VR::get()->m_openxr->swapchains[i].handle);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to destroy swapchain {}.", i);
        } else {
            spdlog::info("[VR] Destroyed swapchain {}.", i);
        }

        ctx.textures.clear();
    }

    this->contexts.clear();
    VR::get()->m_openxr->swapchains.clear();
}

void D3D12Component::OpenXR::copy(uint32_t swapchain_idx, ID3D12Resource* resource, D3D12_RESOURCE_STATES src_state) {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->m_openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);
            texture_ctx->commands.copy(
                resource, 
                ctx.textures[texture_index].texture, 
                src_state,   // [AFW 29.09.2026] war fest PRESENT 
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
        }
    }
}
} // namespace vrmod
