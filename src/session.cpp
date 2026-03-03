//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#include "pch.h"
#include "session.h"
#include "logging.h"

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

    // Initialise NVAPI stereo presenter
    m_stereo = std::make_unique<NvapiStereoPresenter>();
    if (!m_stereo->Init(m_cfg.width, m_cfg.height,
                        m_cfg.separation.load(),
                        m_cfg.convergence.load()))
    {
        LOG_ERROR("Failed to initialise NVAPI stereo presenter");
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
void Session::PollConfigThread() {
    while (!m_pollStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        PollConfigReload(m_cfg);

        if (m_stereo && m_stereo->IsInitialised()) {
            m_stereo->SetSeparation(m_cfg.separation.load());
            m_stereo->SetConvergence(m_cfg.convergence.load());
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
    if (!IsRunning()) return XR_ERROR_SESSION_NOT_RUNNING;

    std::lock_guard<std::mutex> lk(m_frameMtx);

    m_predictedDisplayTime   = m_timer.WaitAndGetNextDisplayTime();
    state->predictedDisplayTime         = m_predictedDisplayTime;
    state->predictedDisplayPeriod       = static_cast<XrDuration>(1'000'000'000LL / m_cfg.frameRate);
    state->shouldRender                 = XR_TRUE;

    return XR_SUCCESS;
}

XrResult Session::BeginFrame(const XrFrameBeginInfo* /*info*/) {
    if (!IsRunning()) return XR_ERROR_SESSION_NOT_RUNNING;
    std::lock_guard<std::mutex> lk(m_frameMtx);
    if (m_frameBegun) return XR_ERROR_CALL_ORDER_INVALID;
    m_frameBegun = true;
    return XR_SUCCESS;
}

XrResult Session::EndFrame(const XrFrameEndInfo* info) {
    if (!IsRunning()) return XR_ERROR_SESSION_NOT_RUNNING;

    {
        std::lock_guard<std::mutex> lk(m_frameMtx);
        if (!m_frameBegun) return XR_ERROR_CALL_ORDER_INVALID;
        m_frameBegun = false;
    }

    if (!info || info->layerCount == 0) return XR_SUCCESS; // shouldRender was false

    // Find left and right projection layer swapchains
    for (uint32_t l = 0; l < info->layerCount; ++l) {
        if (!info->layers[l]) continue;
        if (info->layers[l]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;

        const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(
            info->layers[l]);

        if (proj->viewCount < 2) continue;

        m_leftSwapchain  = proj->views[0].subImage.swapchain;
        m_rightSwapchain = proj->views[1].subImage.swapchain;
        break;
    }

    PresentFrame(info);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
bool Session::PresentFrame(const XrFrameEndInfo* /*info*/) {
    Swapchain* left  = GetSwapchain(m_leftSwapchain);
    Swapchain* right = GetSwapchain(m_rightSwapchain);

    if (!left || !right) {
        LOG_VERBOSE("EndFrame: missing left/right swapchain handles");
        return false;
    }

    const auto* leftImg  = left->GetLatestImage();
    const auto* rightImg = right->GetLatestImage();

    if (!leftImg || !rightImg) return false;

    return m_stereo->PresentStereoFrame(
        leftImg->srv.Get(),
        rightImg->srv.Get(),
        m_d3d11Dev.Get());
}

// ---------------------------------------------------------------------------
// Swapchains
// ---------------------------------------------------------------------------
XrResult Session::CreateSwapchain(const XrSwapchainCreateInfo* ci,
                                   XrSwapchain*                 out)
{
    auto sc = std::make_unique<Swapchain>();
    if (!sc->Init(m_d3d11Dev.Get(), *ci))
        return XR_ERROR_RUNTIME_FAILURE;

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
// Views — identity pose, no head tracking
// ---------------------------------------------------------------------------
XrResult Session::LocateViews(const XrViewLocateInfo* /*info*/,
                               XrViewState*             viewState,
                               uint32_t                 cap,
                               uint32_t*                count,
                               XrView*                  views)
{
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2)  return XR_ERROR_SIZE_INSUFFICIENT;

    viewState->viewStateFlags =
        XR_VIEW_STATE_POSITION_VALID_BIT    |
        XR_VIEW_STATE_ORIENTATION_VALID_BIT |
        XR_VIEW_STATE_POSITION_TRACKED_BIT  |
        XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;

    float halfIpd = m_cfg.ipd * 0.5f;

    // Hardcoded 90° horizontal FoV stereo pair
    static const float kFov = 0.7854f; // 45°

    for (int eye = 0; eye < 2; ++eye) {
        views[eye].type = XR_TYPE_VIEW;
        views[eye].next = nullptr;

        // Identity orientation
        views[eye].pose.orientation = {0.f, 0.f, 0.f, 1.f};

        // Eye offset on X axis
        float offset = (eye == 0) ? -halfIpd : halfIpd;
        views[eye].pose.position = {offset, 0.f, 0.f};

        views[eye].fov = {-kFov, kFov, kFov, -kFov};
    }
    return XR_SUCCESS;
}

} // namespace xr3dv
