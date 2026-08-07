// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/chromime_fonts/chromime_font_manager.h"

#include <string>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "crypto/sha2.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkFontStyle.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTypeface.h"

namespace chromime_fonts {
namespace {

std::string Sha256(std::string_view bytes) {
  return base::HexEncodeLower(crypto::SHA256Hash(base::as_byte_span(bytes)));
}

class ChromimeFontManagerTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_directory_.CreateUniqueTempDir());
    pack_directory_ =
        temp_directory_.GetPath().AppendASCII("packs").AppendASCII("test-pack");
    fonts_directory_ = pack_directory_.AppendASCII("fonts");
    ASSERT_TRUE(base::CreateDirectory(fonts_directory_));

    base::FilePath executable_directory;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_directory));
    source_font_ = executable_directory.AppendASCII("test_fonts")
                       .AppendASCII("Arimo-Regular.ttf");
    ASSERT_TRUE(base::PathExists(source_font_));
    font_path_ = fonts_directory_.AppendASCII("Arimo-Regular.ttf");
    ASSERT_TRUE(base::CopyFile(source_font_, font_path_));
    config_path_ = temp_directory_.GetPath().AppendASCII("active-profile.json");
  }

  void WriteProfile(bool antialiasing = true) {
    std::string font_bytes;
    ASSERT_TRUE(base::ReadFileToString(font_path_, &font_bytes));

    base::DictValue face;
    face.Set("collection_index", 0);
    face.Set("family", "Arimo");
    face.Set("subfamily", "Regular");
    face.Set("family_class", "primary");
    face.Set("weight", 400);
    face.Set("width", 5);
    base::ListValue coverage;
    coverage.Append("U+0020-007E");
    face.Set("coverage", std::move(coverage));

    base::ListValue faces;
    faces.Append(std::move(face));
    base::DictValue file;
    file.Set("path", "fonts/Arimo-Regular.ttf");
    file.Set("size", static_cast<int>(font_bytes.size()));
    file.Set("sha256", Sha256(font_bytes));
    file.Set("faces", std::move(faces));

    base::ListValue files;
    files.Append(std::move(file));
    base::DictValue manifest;
    manifest.Set("pack_id", "test-pack");
    manifest.Set("files", std::move(files));
    std::string manifest_json;
    ASSERT_TRUE(base::JSONWriter::WriteWithOptions(
        manifest, base::JSONWriter::OPTIONS_PRETTY_PRINT, &manifest_json));
    ASSERT_TRUE(base::WriteFile(pack_directory_.AppendASCII("manifest.json"),
                                manifest_json));

    base::DictValue font_pack;
    font_pack.Set("id", "test-pack");
    font_pack.Set("manifest", "packs/test-pack/manifest.json");
    font_pack.Set("manifest_sha256", Sha256(manifest_json));

    base::DictValue generic_families;
    for (const char* generic :
         {"standard", "serif", "sans_serif", "monospace", "system_ui",
          "ui_serif", "ui_sans_serif", "ui_monospace", "cursive", "fantasy",
          "math", "emoji", "fangsong"}) {
      generic_families.Set(generic, "Arimo");
    }

    base::DictValue aliases;
    aliases.Set("Arial", "Arimo");

    base::DictValue cjk;
    for (const char* language :
         {"zh-Hans", "zh-Hant", "zh-HK", "ja", "ko", "und"}) {
      base::ListValue families;
      families.Append("Arimo");
      cjk.Set(language, std::move(families));
    }
    base::ListValue classes;
    classes.Append("primary");
    base::DictValue fallback;
    fallback.Set("strategy", "manifest_coverage_order_v1");
    fallback.Set("default_text_family", "Arimo");
    fallback.Set("last_resort_family", "Arimo");
    fallback.Set("emoji_family", "Arimo");
    fallback.Set("math_family", "Arimo");
    fallback.Set("cjk_by_language", std::move(cjk));
    fallback.Set("ordered_family_classes", std::move(classes));

    base::DictValue rendering;
    rendering.Set("hinting", "slight");
    rendering.Set("antialiasing", antialiasing);
    rendering.Set("subpixel_positioning", true);
    rendering.Set("subpixel_rendering", "none");
    rendering.Set("embedded_bitmaps", true);
    rendering.Set("autohinter", false);
    rendering.Set("synthetic_bold", true);
    rendering.Set("synthetic_italic", true);

    base::DictValue config;
    config.Set("schema_version", 1);
    config.Set("profile_id", "test-profile");
    config.Set("font_pack", std::move(font_pack));
    config.Set("generic_families", std::move(generic_families));
    config.Set("aliases", std::move(aliases));
    config.Set("fallback", std::move(fallback));
    config.Set("rendering", std::move(rendering));
    std::string config_json;
    ASSERT_TRUE(base::JSONWriter::WriteWithOptions(
        config, base::JSONWriter::OPTIONS_PRETTY_PRINT, &config_json));
    ASSERT_TRUE(base::WriteFile(config_path_, config_json));
  }

  base::ScopedTempDir temp_directory_;
  base::FilePath pack_directory_;
  base::FilePath fonts_directory_;
  base::FilePath source_font_;
  base::FilePath font_path_;
  base::FilePath config_path_;
};

TEST_F(ChromimeFontManagerTest, LoadsAliasesAndDeterministicFallback) {
  WriteProfile();
  ConfiguredFontManager configured = LoadFromConfigFile(config_path_);
  ASSERT_TRUE(configured.font_manager) << configured.error;

  sk_sp<SkTypeface> alias =
      configured.font_manager->matchFamilyStyle("Arial", SkFontStyle::Normal());
  ASSERT_TRUE(alias);
  SkString family;
  alias->getFamilyName(&family);
  EXPECT_STREQ(family.c_str(), "Arimo");

  const char* languages[] = {"en"};
  sk_sp<SkTypeface> fallback =
      configured.font_manager->matchFamilyStyleCharacter(
          "Missing host family", SkFontStyle::Normal(), languages, 1, 'A');
  ASSERT_TRUE(fallback);
  EXPECT_EQ(fallback->uniqueID(), alias->uniqueID());
  EXPECT_FALSE(configured.font_manager->matchFamilyStyleCharacter(
      "Missing host family", SkFontStyle::Normal(), languages, 1, 0x10FFFF));

  std::optional<FontFileReference> reference =
      GetFontFileReference(*configured.font_manager, alias->uniqueID());
  ASSERT_TRUE(reference);
  EXPECT_EQ(reference->path, font_path_);
  EXPECT_EQ(reference->collection_index, 0);
}

TEST_F(ChromimeFontManagerTest, RejectsModifiedFontBytes) {
  WriteProfile();
  ASSERT_TRUE(base::WriteFile(font_path_, "tampered"));
  ConfiguredFontManager configured = LoadFromConfigFile(config_path_);
  EXPECT_TRUE(configured.enabled);
  EXPECT_FALSE(configured.font_manager);
  EXPECT_NE(configured.error.find("size does not match"), std::string::npos);
}

TEST_F(ChromimeFontManagerTest, RejectsUnlistedFontFile) {
  WriteProfile();
  ASSERT_TRUE(base::CopyFile(source_font_,
                             fonts_directory_.AppendASCII("unlisted.ttf")));
  ConfiguredFontManager configured = LoadFromConfigFile(config_path_);
  EXPECT_FALSE(configured.font_manager);
  EXPECT_NE(configured.error.find("unlisted file"), std::string::npos);
}

TEST_F(ChromimeFontManagerTest, RejectsHostDependentRendering) {
  WriteProfile(/*antialiasing=*/false);
  ConfiguredFontManager configured = LoadFromConfigFile(config_path_);
  EXPECT_FALSE(configured.font_manager);
  EXPECT_NE(configured.error.find("cross-platform rendering protocol"),
            std::string::npos);
}

}  // namespace
}  // namespace chromime_fonts
