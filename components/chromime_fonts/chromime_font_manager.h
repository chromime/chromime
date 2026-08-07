// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_CHROMIME_FONTS_CHROMIME_FONT_MANAGER_H_
#define COMPONENTS_CHROMIME_FONTS_CHROMIME_FONT_MANAGER_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "third_party/skia/include/core/SkRefCnt.h"
#include "third_party/skia/include/core/SkTypeface.h"

class SkFontMgr;

namespace chromime_fonts {

// The result of resolving and validating a Chromime font configuration.
// `enabled` is false only when no configuration was requested. If `enabled`
// is true and `font_manager` is null, callers must fail closed and must not
// fall back to host fonts.
struct ConfiguredFontManager {
  bool enabled = false;
  sk_sp<SkFontMgr> font_manager;
  base::FilePath config_path;
  std::string error;
};

struct FontFileReference {
  base::FilePath path;
  int collection_index = 0;
};

// Loads the absolute path supplied by --chromime-font-config. No host font
// manager is consulted while loading or matching the resulting manager.
ConfiguredFontManager LoadFromCommandLine();

// Loads a profile directly. Exposed separately for tests and embedders.
ConfiguredFontManager LoadFromConfigFile(const base::FilePath& config_path);

// Returns the verified pack file backing a typeface created by a Chromime font
// manager. Other managers and unknown typefaces return nullopt.
std::optional<FontFileReference> GetFontFileReference(
    const SkFontMgr& font_manager,
    SkTypefaceID typeface_id);

}  // namespace chromime_fonts

#endif  // COMPONENTS_CHROMIME_FONTS_CHROMIME_FONT_MANAGER_H_
