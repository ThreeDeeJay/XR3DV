//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  All spaces return identity transforms — this runtime has no tracking.

#include <openxr/openxr.h>
#include "runtime.h"
#include "logging.h"

// Space handle just encodes the reference space type as a pointer value.
// Fine for a non-tracking runtime.

extern "C" {

XrResult xrCreateReferenceSpace(XrSession session,
                                 const XrReferenceSpaceCreateInfo* createInfo,
                                 XrSpace* space)
{
    (void)session;
    if (!createInfo || !space) return XR_ERROR_VALIDATION_FAILURE;
    // Encode the space type as a fake handle value
    *space = reinterpret_cast<XrSpace>(
        static_cast<uintptr_t>(createInfo->referenceSpaceType + 1));
    LOG_VERBOSE("xrCreateReferenceSpace type=%d", createInfo->referenceSpaceType);
    return XR_SUCCESS;
}

XrResult xrCreateActionSpace(XrSession session,
                              const XrActionSpaceCreateInfo* createInfo,
                              XrSpace* space)
{
    (void)session; (void)createInfo;
    // Action spaces always return identity
    *space = reinterpret_cast<XrSpace>(0xFF);
    return XR_SUCCESS;
}

XrResult xrLocateSpace(XrSpace space, XrSpace baseSpace,
                        XrTime time, XrSpaceLocation* location)
{
    (void)space; (void)baseSpace; (void)time;
    if (!location) return XR_ERROR_VALIDATION_FAILURE;

    location->locationFlags =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT   |
        XR_SPACE_LOCATION_POSITION_VALID_BIT       |
        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT  |
        XR_SPACE_LOCATION_POSITION_TRACKED_BIT;

    location->pose.orientation = {0.f, 0.f, 0.f, 1.f};
    location->pose.position    = {0.f, 0.f, 0.f};
    return XR_SUCCESS;
}

XrResult xrDestroySpace(XrSpace /*space*/) {
    return XR_SUCCESS;
}

XrResult xrEnumerateReferenceSpaces(XrSession session,
                                    uint32_t spaceCapacityInput,
                                    uint32_t* spaceCountOutput,
                                    XrReferenceSpaceType* spaces)
{
    (void)session;
    static const XrReferenceSpaceType kSpaces[] = {
        XR_REFERENCE_SPACE_TYPE_VIEW,
        XR_REFERENCE_SPACE_TYPE_LOCAL,
        XR_REFERENCE_SPACE_TYPE_STAGE,
    };
    *spaceCountOutput = 3;
    if (spaceCapacityInput == 0) return XR_SUCCESS;
    if (spaceCapacityInput < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 3; ++i) spaces[i] = kSpaces[i];
    return XR_SUCCESS;
}

XrResult xrGetReferenceSpaceBoundsRect(XrSession session,
                                        XrReferenceSpaceType referenceSpaceType,
                                        XrExtent2Df* bounds)
{
    (void)session; (void)referenceSpaceType;
    // Unbounded — report a reasonable stage size
    bounds->width  = 5.0f;
    bounds->height = 5.0f;
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

} // extern "C"
