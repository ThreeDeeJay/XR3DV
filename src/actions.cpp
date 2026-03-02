//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Action/input stubs.  XR3DV has no physical controllers; all inputs
//  return "inactive / zero" states so applications continue to run.

#include <openxr/openxr.h>
#include "logging.h"

extern "C" {

// ---- Action sets ---------------------------------------------------------
XrResult xrCreateActionSet(XrInstance /*instance*/,
                            const XrActionSetCreateInfo* /*info*/,
                            XrActionSet* actionSet)
{
    static uint64_t s_id = 1;
    *actionSet = reinterpret_cast<XrActionSet>(s_id++);
    return XR_SUCCESS;
}

XrResult xrDestroyActionSet(XrActionSet /*actionSet*/) { return XR_SUCCESS; }

// ---- Actions -------------------------------------------------------------
XrResult xrCreateAction(XrActionSet /*actionSet*/,
                         const XrActionCreateInfo* /*info*/,
                         XrAction* action)
{
    static uint64_t s_id = 1;
    *action = reinterpret_cast<XrAction>(s_id++);
    return XR_SUCCESS;
}

XrResult xrDestroyAction(XrAction /*action*/) { return XR_SUCCESS; }

// ---- Bindings ------------------------------------------------------------
XrResult xrSuggestInteractionProfileBindings(
    XrInstance /*instance*/,
    const XrInteractionProfileSuggestedBinding* /*info*/)
{
    // Accept any profile without complaint
    return XR_SUCCESS;
}

XrResult xrAttachSessionActionSets(XrSession /*session*/,
                                   const XrSessionActionSetsAttachInfo* /*info*/)
{
    return XR_SUCCESS;
}

// ---- Sync / query --------------------------------------------------------
XrResult xrSyncActions(XrSession /*session*/,
                        const XrActionsSyncInfo* /*info*/)
{
    return XR_SUCCESS;
}

XrResult xrGetCurrentInteractionProfile(
    XrSession /*session*/,
    XrPath /*topLevelUserPath*/,
    XrInteractionProfileState* state)
{
    state->interactionProfile = XR_NULL_PATH; // no controller connected
    return XR_SUCCESS;
}

XrResult xrGetActionStateBoolean(XrSession /*session*/,
                                  const XrActionStateGetInfo* /*info*/,
                                  XrActionStateBoolean* state)
{
    state->currentState     = XR_FALSE;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime   = 0;
    state->isActive         = XR_FALSE;
    return XR_SUCCESS;
}

XrResult xrGetActionStateFloat(XrSession /*session*/,
                                const XrActionStateGetInfo* /*info*/,
                                XrActionStateFloat* state)
{
    state->currentState     = 0.f;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime   = 0;
    state->isActive         = XR_FALSE;
    return XR_SUCCESS;
}

XrResult xrGetActionStateVector2f(XrSession /*session*/,
                                   const XrActionStateGetInfo* /*info*/,
                                   XrActionStateVector2f* state)
{
    state->currentState     = {0.f, 0.f};
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime   = 0;
    state->isActive         = XR_FALSE;
    return XR_SUCCESS;
}

XrResult xrGetActionStatePose(XrSession /*session*/,
                               const XrActionStateGetInfo* /*info*/,
                               XrActionStatePose* state)
{
    state->isActive = XR_FALSE;
    return XR_SUCCESS;
}

XrResult xrEnumerateBoundSourcesForAction(
    XrSession /*session*/,
    const XrBoundSourcesForActionEnumerateInfo* /*info*/,
    uint32_t /*cap*/,
    uint32_t* count,
    XrPath* /*sources*/)
{
    *count = 0; // No bindings
    return XR_SUCCESS;
}

XrResult xrGetInputSourceLocalizedName(
    XrSession /*session*/,
    const XrInputSourceLocalizedNameGetInfo* /*info*/,
    uint32_t /*cap*/,
    uint32_t* count,
    char* /*buf*/)
{
    *count = 0;
    return XR_SUCCESS;
}

// ---- Haptics (no-op) -----------------------------------------------------
XrResult xrApplyHapticFeedback(XrSession /*session*/,
                                const XrHapticActionInfo* /*info*/,
                                const XrHapticBaseHeader* /*haptic*/)
{
    return XR_SUCCESS; // silently ignore
}

XrResult xrStopHapticFeedback(XrSession /*session*/,
                               const XrHapticActionInfo* /*info*/)
{
    return XR_SUCCESS;
}

// ---- Path utilities ------------------------------------------------------
static uint64_t s_pathCounter = 1;

XrResult xrStringToPath(XrInstance /*instance*/,
                          const char* pathString,
                          XrPath* path)
{
    (void)pathString;
    // All paths are accepted but stored as incrementing handles.
    // A production runtime would maintain a bidirectional string↔id map.
    *path = s_pathCounter++;
    return XR_SUCCESS;
}

XrResult xrPathToString(XrInstance /*instance*/,
                          XrPath /*path*/,
                          uint32_t /*cap*/,
                          uint32_t* count,
                          char* /*buf*/)
{
    *count = 0;
    return XR_SUCCESS;
}

} // extern "C"
