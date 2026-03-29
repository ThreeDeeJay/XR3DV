//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#include "pch.h"
#include "session.h"
#include "logging.h"
#include <d3d12.h>   // ID3D12Device — COM vtable only, no d3d12.lib required

namespace xr3dv {

// ---------------------------------------------------------------------------
// Supported swapchain formats (preferred first)
// ---------------------------------------------------------------------------
static const int64_t kSupportedFormats[] = {
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_D32_FLOAT,
    DXGI_FORMAT_D16_UNORM,
    DXGI_FORMAT_D24_UNORM_S8_UINT,
};

// ---------------------------------------------------------------------------
Session::Session(Config& cfg)
    : m_cfg(cfg)
    , m_timer(cfg.frameRate)
{
}

Session::~Session() {
    m_pollStop = true;
    if (m_pollThread.joinable()) m_pollThread.join();
}

// ---------------------------------------------------------------------------
XrResult Session::InitD3D11(const XrGraphicsBindingD3D11KHR* binding) {
    if (!binding || !binding->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;

    m_d3d11Dev = binding->device;
    m_d3d11Dev->GetImmediateContext(&m_d3d11Ctx);

    // Initialise the windowed D3D9 stereo presenter (packed-surface approach).
    if (!m_presenter.Init(
            m_cfg.width, m_cfg.height,
            m_cfg.monitorRate,
            m_cfg.separation.load(),
            m_cfg.convergence.load(),
            m_cfg.swapEyes,
            m_cfg.gameIniPath))
    {
        LOG_ERROR("Failed to initialise NvapiStereoPresenter");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    m_timer.SetTargetHz(m_cfg.frameRate);

    // Start background config-poll thread
    m_pollThread = std::thread([this]{ PollConfigThread(); });

    m_state = XR_SESSION_STATE_READY;
    LOG_INFO("Session initialised (D3D11)");
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
XrResult Session::InitD3D12(void* d3d12DeviceRaw, void* /*d3d12QueueRaw*/)
{
    if (!d3d12DeviceRaw) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    m_graphicsApi  = GraphicsApi::D3D12;
    m_d3d12Device  = d3d12DeviceRaw;

    // Create our own D3D11 device on the same adapter.
    // Swapchain::InitShared will create NT-shared textures so the app's D3D12
    // device can render into them while we read them back via D3D11 SRVs.
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    0, nullptr, 0, D3D11_SDK_VERSION,
                                    &m_d3d11Dev, &fl, &m_d3d11Ctx);
    if (FAILED(hr)) {
        LOG_ERROR("InitD3D12: D3D11CreateDevice failed 0x%08X", (unsigned)hr);
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    LOG_INFO("InitD3D12: internal D3D11 device fl=0x%04X", (unsigned)fl);

    if (!m_presenter.Init(m_cfg.width, m_cfg.height, m_cfg.monitorRate,
                           m_cfg.separation.load(), m_cfg.convergence.load(),
                           m_cfg.swapEyes, m_cfg.gameIniPath)) {
        LOG_ERROR("InitD3D12: failed to initialise NvapiStereoPresenter");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    m_timer.SetTargetHz(m_cfg.frameRate);
    m_pollThread = std::thread([this]{ PollConfigThread(); });
    m_state = XR_SESSION_STATE_READY;
    LOG_INFO("Session initialised (D3D12 → shared D3D11)");
    return XR_SUCCESS;
}

// --------------------------------------------------------------------------- {
    while (!m_pollStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        bool changed = PollConfigReload(m_cfg);
        if (changed && m_presenter.IsInitialised()) {
            m_presenter.SetSeparation(m_cfg.separation.load());
            m_presenter.SetConvergence(m_cfg.convergence.load());
        }
    }
}

// ---------------------------------------------------------------------------
bool Session::IsRunning() const {
    auto s = m_state.load();
    return s == XR_SESSION_STATE_SYNCHRONIZED ||
           s == XR_SESSION_STATE_VISIBLE       ||
           s == XR_SESSION_STATE_FOCUSED;
}

// ---------------------------------------------------------------------------
XrResult Session::Begin(const XrSessionBeginInfo* bi) {
    if (m_state != XR_SESSION_STATE_READY) return XR_ERROR_SESSION_NOT_READY;
    if (bi->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;

    m_state = XR_SESSION_STATE_FOCUSED;
    LOG_INFO("Session begun");
    return XR_SUCCESS;
}

XrResult Session::End() {
    if (!IsRunning()) return XR_ERROR_SESSION_NOT_RUNNING;
    m_state = XR_SESSION_STATE_STOPPING;
    // Signal stopping then idle asynchronously — real runtime would drain frames first
    m_state = XR_SESSION_STATE_IDLE;
    LOG_INFO("Session ended");
    return XR_SUCCESS;
}

XrResult Session::RequestExit() {
    // Mark session as exiting — WaitFrame will return XR_SESSION_LOSS_PENDING
    m_state = XR_SESSION_STATE_EXITING;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------
XrResult Session::WaitFrame(const XrFrameWaitInfo* /*info*/, XrFrameState* state) {
    LOG_TRACE("WaitFrame: enter (state=%d)", (int)m_state.load());
    if (!IsRunning()) {
        LOG_ERROR("WaitFrame: session not running (state=%d)", (int)m_state.load());
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    LOG_TRACE("WaitFrame: calling WaitAndGetNextDisplayTime...");
    int64_t displayTime = m_timer.WaitAndGetNextDisplayTime();
    LOG_TRACE("WaitFrame: displayTime=%" PRId64, displayTime);

    {
        std::lock_guard<std::mutex> lk(m_frameMtx);
        m_predictedDisplayTime = displayTime;
    }

    state->predictedDisplayTime   = displayTime;
    state->predictedDisplayPeriod = static_cast<XrDuration>(1'000'000'000LL / m_cfg.frameRate);
    state->shouldRender           = XR_TRUE;

    LOG_TRACE("WaitFrame: return XR_SUCCESS");
    return XR_SUCCESS;
}

XrResult Session::BeginFrame(const XrFrameBeginInfo* /*info*/) {
    LOG_TRACE("BeginFrame: enter");
    if (!IsRunning()) {
        LOG_ERROR("BeginFrame: session not running (state=%d)", (int)m_state.load());
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    std::lock_guard<std::mutex> lk(m_frameMtx);
    if (m_frameBegun) {
        LOG_ERROR("BeginFrame: XR_ERROR_CALL_ORDER_INVALID (frame already begun)");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    m_frameBegun = true;
    LOG_TRACE("BeginFrame: return XR_SUCCESS");
    return XR_SUCCESS;
}

XrResult Session::EndFrame(const XrFrameEndInfo* info) {
    LOG_TRACE("EndFrame: enter layerCount=%u", info ? info->layerCount : 0u);
    if (!IsRunning()) {
        LOG_ERROR("EndFrame: session not running");
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    {
        std::lock_guard<std::mutex> lk(m_frameMtx);
        if (!m_frameBegun) {
            LOG_ERROR("EndFrame: XR_ERROR_CALL_ORDER_INVALID (BeginFrame not called)");
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        m_frameBegun = false;
    }

    if (!info || info->layerCount == 0) {
        LOG_TRACE("EndFrame: no layers, skipping present");
        return XR_SUCCESS;
    }

    // Find left and right projection layer swapchains
    for (uint32_t l = 0; l < info->layerCount; ++l) {
        if (!info->layers[l]) continue;
        if (info->layers[l]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;

        const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(
            info->layers[l]);

        if (proj->viewCount < 2) continue;

        m_leftSwapchain  = proj->views[0].subImage.swapchain;
        m_rightSwapchain = proj->views[1].subImage.swapchain;
        LOG_TRACE("EndFrame: left SC=0x%p right SC=0x%p",
                  (void*)m_leftSwapchain, (void*)m_rightSwapchain);
        break;
    }

    LOG_TRACE("EndFrame: calling PresentFrame...");
    bool ok = PresentFrame(info);
    LOG_TRACE("EndFrame: PresentFrame returned %s", ok ? "true" : "false");
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
bool Session::PresentFrame(const XrFrameEndInfo* /*info*/) {
    LOG_TRACE("PresentFrame: enter");
    Swapchain* left  = GetSwapchain(m_leftSwapchain);
    Swapchain* right = GetSwapchain(m_rightSwapchain);

    if (!left || !right) {
        LOG_VERBOSE("PresentFrame: missing left/right swapchain handles");
        return false;
    }
    if (left->IsDepth() || right->IsDepth()) {
        LOG_ERROR("PresentFrame: projection view swapchain is depth — skipping");
        return false;
    }

    const auto* leftImg  = left->GetLatestImage();
    const auto* rightImg = right->GetLatestImage();
    if (!leftImg || !rightImg) {
        LOG_VERBOSE("PresentFrame: no acquired image yet — skipping");
        return false;
    }

    LOG_TRACE("PresentFrame: leftSRV=%p rightSRV=%p",
              (void*)leftImg->srv.Get(), (void*)rightImg->srv.Get());

    // Call the windowed D3D9 presenter directly.
    // It blits left+right into a packed W×(H*2+1) surface and presents via
    // the NVAPI packed-stereo StretchRect technique.
    LOG_TRACE("PresentFrame: calling PresentStereoFrame...");
    bool ok = m_presenter.PresentStereoFrame(
        leftImg->srv.Get(), rightImg->srv.Get(), m_d3d11Dev.Get());
    LOG_TRACE("PresentFrame: PresentStereoFrame returned %s", ok ? "true" : "false");
    return ok;
}

// ---------------------------------------------------------------------------
// Swapchains
// ---------------------------------------------------------------------------
XrResult Session::CreateSwapchain(const XrSwapchainCreateInfo* ci,
                                   XrSwapchain*                 out)
{
    auto sc = std::make_unique<Swapchain>();
    bool ok;
    if (m_graphicsApi == GraphicsApi::D3D12 && m_d3d12Device) {
        ok = sc->InitShared(m_d3d11Dev.Get(),
                            static_cast<ID3D12Device*>(m_d3d12Device), *ci);
    } else {
        ok = sc->Init(m_d3d11Dev.Get(), *ci);
    }
    if (!ok) return XR_ERROR_RUNTIME_FAILURE;

    std::lock_guard<std::mutex> lk(m_scMtx);
    uint64_t h = m_nextScHandle++;
    m_swapchains[h] = std::move(sc);
    *out = reinterpret_cast<XrSwapchain>(h);
    return XR_SUCCESS;
}

XrResult Session::DestroySwapchain(XrSwapchain sc) {
    std::lock_guard<std::mutex> lk(m_scMtx);
    auto h = reinterpret_cast<uint64_t>(sc);
    auto it = m_swapchains.find(h);
    if (it == m_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    m_swapchains.erase(it);
    return XR_SUCCESS;
}

XrResult Session::EnumerateSwapchainFormats(uint32_t cap, uint32_t* cnt,
                                             int64_t* fmts)
{
    uint32_t n = static_cast<uint32_t>(std::size(kSupportedFormats));
    *cnt = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n)  return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < n; ++i) fmts[i] = kSupportedFormats[i];
    return XR_SUCCESS;
}

Swapchain* Session::GetSwapchain(XrSwapchain handle) {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lk(m_scMtx);
    auto it = m_swapchains.find(reinterpret_cast<uint64_t>(handle));
    return (it == m_swapchains.end()) ? nullptr : it->second.get();
}

// ---------------------------------------------------------------------------
// Views — mouse-look head pose with IPD offset
// ---------------------------------------------------------------------------
// Mouse sensitivity: pixels → radians.  Tune via xr3dv.ini [Input] if added.
static constexpr float kMouseSensitivity = 0.001f; // rad/pixel
static constexpr float kPitchLimit       = 1.48f;  // ~85 degrees

// Multiply two quaternions (Hamilton product)
static XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

// Rotate a vector by a quaternion: v' = q * (0,v) * q^-1
static XrVector3f QRotate(const XrQuaternionf& q, XrVector3f v) {
    // Using the optimised formula: v' = v + 2q.w*(q.xyz × v) + 2*(q.xyz × (q.xyz × v))
    float tx = 2.f * (q.y*v.z - q.z*v.y);
    float ty = 2.f * (q.z*v.x - q.x*v.z);
    float tz = 2.f * (q.x*v.y - q.y*v.x);
    return {
        v.x + q.w*tx + (q.y*tz - q.z*ty),
        v.y + q.w*ty + (q.z*tx - q.x*tz),
        v.z + q.w*tz + (q.x*ty - q.y*tx)
    };
}

XrResult Session::LocateViews(const XrViewLocateInfo* /*info*/,
                               XrViewState*             viewState,
                               uint32_t                 cap,
                               uint32_t*                count,
                               XrView*                  views)
{
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2)  return XR_ERROR_SIZE_INSUFFICIENT;

    // Consume mouse deltas from the presenter
    int32_t dx = 0, dy = 0; bool recenter = false;
    if (m_presenter.IsInitialised())
        m_presenter.ConsumeDelta(dx, dy, recenter);

    if (recenter) {
        m_yaw = m_pitch = 0.f;
        LOG_INFO("Head pose recentered");
    } else {
        m_yaw   -= static_cast<float>(dx) * kMouseSensitivity;
        m_pitch -= static_cast<float>(dy) * kMouseSensitivity;
        if (m_pitch >  kPitchLimit) m_pitch =  kPitchLimit;
        if (m_pitch < -kPitchLimit) m_pitch = -kPitchLimit;
    }

    // Build head orientation: yaw around Y (world-up), then pitch around X (local-right)
    float hy = sinf(m_yaw   * 0.5f), hw = cosf(m_yaw   * 0.5f); // Y-axis rotation
    float px = sinf(m_pitch * 0.5f), pw = cosf(m_pitch * 0.5f); // X-axis rotation
    XrQuaternionf qYaw   = {0.f, hy, 0.f, hw};
    XrQuaternionf qPitch = {px,  0.f, 0.f, pw};
    XrQuaternionf headQ  = QMul(qYaw, qPitch); // yaw applied first (world space)

    viewState->viewStateFlags =
        XR_VIEW_STATE_POSITION_VALID_BIT    |
        XR_VIEW_STATE_ORIENTATION_VALID_BIT |
        XR_VIEW_STATE_POSITION_TRACKED_BIT  |
        XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;

    float halfIpd = m_cfg.ipd * 0.5f;
    static const float kFov = 0.7854f; // 45° each side

    for (int eye = 0; eye < 2; ++eye) {
        views[eye].type = XR_TYPE_VIEW;
        views[eye].next = nullptr;
        views[eye].pose.orientation = headQ;
        // Eye position = head origin + head-rotated IPD offset
        float sign = (eye == 0) ? -1.f : 1.f;
        XrVector3f localOffset = {sign * halfIpd, 0.f, 0.f};
        views[eye].pose.position = QRotate(headQ, localOffset);
        views[eye].fov = {-kFov, kFov, kFov, -kFov};
    }
    return XR_SUCCESS;
}

} // namespace xr3dv
