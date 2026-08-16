// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SKIA_EXT_RHENDIUM_RENDERING_H_
#define SKIA_EXT_RHENDIUM_RENDERING_H_

#include "third_party/skia/include/core/SkTypes.h"

namespace skia {

inline constexpr char kRhendiumCrossPlatformBlendingSwitch[] =
    "rhendium-cross-platform-blending";

// Selects architecture-independent source-over rounding for deterministic
// Rhendium renderer and GPU/Viz processes. This must be called after
// SkGraphics::Init().
SK_API void EnableRhendiumCrossPlatformBlending();

}  // namespace skia

#endif  // SKIA_EXT_RHENDIUM_RENDERING_H_
