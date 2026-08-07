// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/chromime_fonts/chromime_font_manager.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "crypto/sha2.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFontArguments.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkFontStyle.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/ports/SkTypeface_fontations.h"

namespace chromime_fonts {

namespace {

constexpr size_t kMaximumConfigBytes = 1024 * 1024;
constexpr size_t kMaximumManifestBytes = 16 * 1024 * 1024;
constexpr char kChromimeFontConfigSwitch[] = "chromime-font-config";

std::string LowerASCII(std::string_view value) {
  return base::ToLowerASCII(value);
}

bool ReadTextFile(const base::FilePath& path,
                  size_t maximum_bytes,
                  std::string* contents,
                  std::string* error) {
  std::optional<int64_t> size = base::GetFileSize(path);
  if (!size || *size < 0 || static_cast<uint64_t>(*size) > maximum_bytes) {
    *error =
        "File is missing or exceeds the size limit: " + path.AsUTF8Unsafe();
    return false;
  }
  if (!base::ReadFileToString(path, contents)) {
    *error = "Could not read file: " + path.AsUTF8Unsafe();
    return false;
  }
  return true;
}

std::optional<base::Value> ParseJson(std::string_view json,
                                     const base::FilePath& path,
                                     std::string* error) {
  auto parsed =
      base::JSONReader::ReadAndReturnValueWithError(json, base::JSON_PARSE_RFC);
  if (!parsed.has_value()) {
    *error = "Invalid JSON in " + path.AsUTF8Unsafe() + ": " +
             parsed.error().message;
    return std::nullopt;
  }
  return std::move(parsed.value());
}

bool IsSafeRelativePath(const base::FilePath& path) {
  return !path.empty() && !path.IsAbsolute() && !path.ReferencesParent();
}

bool ParseCoverageRange(std::string_view value,
                        std::pair<SkUnichar, SkUnichar>* range) {
  if (!value.starts_with("U+")) {
    return false;
  }
  value.remove_prefix(2);
  const size_t separator = value.find('-');
  const std::string_view first = value.substr(0, separator);
  const std::string_view last =
      separator == std::string_view::npos ? first : value.substr(separator + 1);
  uint32_t first_codepoint = 0;
  uint32_t last_codepoint = 0;
  if (first.empty() || last.empty() ||
      !base::HexStringToUInt(first, &first_codepoint) ||
      !base::HexStringToUInt(last, &last_codepoint) ||
      first_codepoint > last_codepoint || last_codepoint > 0x10FFFF) {
    return false;
  }
  *range = {static_cast<SkUnichar>(first_codepoint),
            static_cast<SkUnichar>(last_codepoint)};
  return true;
}

bool IsCjkCharacter(SkUnichar character) {
  return (character >= 0x2E80 && character <= 0x33FF) ||
         (character >= 0x3400 && character <= 0x4DBF) ||
         (character >= 0x4E00 && character <= 0x9FFF) ||
         (character >= 0xAC00 && character <= 0xD7AF) ||
         (character >= 0xF900 && character <= 0xFAFF) ||
         (character >= 0x20000 && character <= 0x323AF);
}

bool IsEmojiCharacter(SkUnichar character) {
  return (character >= 0x1F000 && character <= 0x1FAFF) ||
         (character >= 0x2600 && character <= 0x27BF);
}

bool IsMathCharacter(SkUnichar character) {
  return (character >= 0x2190 && character <= 0x22FF) ||
         (character >= 0x27C0 && character <= 0x2AFF) ||
         (character >= 0x1D400 && character <= 0x1D7FF);
}

std::string Sha256Hex(const base::FilePath& path, std::string* error) {
  base::MemoryMappedFile mapping;
  if (!mapping.Initialize(path)) {
    *error = "Could not map file for verification: " + path.AsUTF8Unsafe();
    return {};
  }
  return base::HexEncodeLower(crypto::SHA256Hash(mapping.bytes()));
}

struct FontFaceRecord {
  std::string family;
  std::string subfamily;
  SkFontStyle style;
  struct FontFileRecord {
    explicit FontFileRecord(base::FilePath file_path)
        : path(std::move(file_path)) {}

    sk_sp<SkData> GetData() const {
      base::AutoLock auto_lock(lock);
      if (data) {
        return data;
      }
      auto mapping = std::make_unique<base::MemoryMappedFile>();
      if (!mapping->Initialize(path)) {
        return nullptr;
      }
      base::span<const uint8_t> bytes = mapping->bytes();
      data = SkData::MakeWithProc(
          bytes.data(), bytes.size(),
          [](const void*, void* context) {
            delete static_cast<base::MemoryMappedFile*>(context);
          },
          mapping.release());
      return data;
    }

    const base::FilePath path;
    mutable base::Lock lock;
    mutable sk_sp<SkData> data;
  };

  std::shared_ptr<FontFileRecord> file;
  int collection_index = 0;
  std::vector<std::pair<SkUnichar, SkUnichar>> coverage;
  mutable sk_sp<SkTypeface> typeface;

  bool Covers(SkUnichar character) const {
    auto range = std::lower_bound(coverage.begin(), coverage.end(), character,
                                  [](const auto& candidate, SkUnichar value) {
                                    return candidate.second < value;
                                  });
    return range != coverage.end() && character >= range->first;
  }

  sk_sp<SkTypeface> GetTypeface() const {
    base::AutoLock auto_lock(lock);
    if (!typeface) {
      sk_sp<SkData> data = file->GetData();
      if (data) {
        typeface = SkTypeface_Make_Fontations(
            std::move(data),
            SkFontArguments().setCollectionIndex(collection_index));
      }
    }
    return typeface;
  }

  bool HasTypefaceId(SkTypefaceID typeface_id) const {
    base::AutoLock auto_lock(lock);
    return typeface && typeface->uniqueID() == typeface_id;
  }

  mutable base::Lock lock;
};

class ChromimeFontStyleSet final : public SkFontStyleSet {
 public:
  explicit ChromimeFontStyleSet(
      std::vector<std::shared_ptr<FontFaceRecord>> faces)
      : faces_(std::move(faces)) {}

  int count() override { return static_cast<int>(faces_.size()); }

  void getStyle(int index, SkFontStyle* style, SkString* name) override {
    if (index < 0 || static_cast<size_t>(index) >= faces_.size()) {
      return;
    }
    if (style) {
      *style = faces_[index]->style;
    }
    if (name) {
      name->set(faces_[index]->subfamily.c_str());
    }
  }

  sk_sp<SkTypeface> createTypeface(int index) override {
    return index < 0 || static_cast<size_t>(index) >= faces_.size()
               ? nullptr
               : faces_[index]->GetTypeface();
  }

  sk_sp<SkTypeface> matchStyle(const SkFontStyle& pattern) override {
    return matchStyleCSS3(pattern);
  }

 private:
  const std::vector<std::shared_ptr<FontFaceRecord>> faces_;
};

class ChromimeFontManager;

struct FontManagerRegistry {
  base::Lock lock;
  std::map<const SkFontMgr*, const ChromimeFontManager*> managers;
};

FontManagerRegistry& GetFontManagerRegistry() {
  static base::NoDestructor<FontManagerRegistry> registry;
  return *registry;
}

class ChromimeFontManager final : public SkFontMgr {
 public:
  ChromimeFontManager(
      std::vector<std::shared_ptr<FontFaceRecord>> faces,
      std::map<std::string, std::string> aliases,
      std::vector<std::string> ordered_family_classes,
      std::map<std::string, std::string> family_classes,
      std::map<std::string, std::vector<std::string>> cjk_by_language,
      std::string default_family,
      std::string last_resort_family,
      std::string emoji_family,
      std::string math_family)
      : aliases_(std::move(aliases)),
        cjk_by_language_(std::move(cjk_by_language)),
        default_family_(std::move(default_family)),
        last_resort_family_(std::move(last_resort_family)),
        emoji_family_(std::move(emoji_family)),
        math_family_(std::move(math_family)),
        faces_(std::move(faces)) {
    FontManagerRegistry& registry = GetFontManagerRegistry();
    base::AutoLock lock(registry.lock);
    registry.managers.emplace(this, this);
    for (const std::shared_ptr<FontFaceRecord>& face : faces_) {
      const std::string key = LowerASCII(face->family);
      family_lookup_.try_emplace(key, face->family);
      faces_by_family_[key].push_back(face);
    }
    for (const auto& [key, family] : family_lookup_) {
      family_names_.push_back(family);
    }
    std::ranges::sort(family_names_, [](const auto& left, const auto& right) {
      return LowerASCII(left) < LowerASCII(right);
    });

    std::set<std::string> appended;
    auto append_family = [&](const std::string& family) {
      std::string resolved = ResolveFamily(family);
      if (!resolved.empty() && appended.insert(LowerASCII(resolved)).second) {
        fallback_families_.push_back(std::move(resolved));
      }
    };
    append_family(default_family_);
    for (const std::string& family_class : ordered_family_classes) {
      std::vector<std::string> class_families;
      for (const auto& [family, actual_class] : family_classes) {
        if (actual_class == family_class) {
          class_families.push_back(family);
        }
      }
      std::ranges::sort(class_families,
                        [](const std::string& left, const std::string& right) {
                          return LowerASCII(left) < LowerASCII(right);
                        });
      for (const std::string& family : class_families) {
        append_family(family);
      }
    }
    append_family(last_resort_family_);
  }

  ~ChromimeFontManager() override {
    FontManagerRegistry& registry = GetFontManagerRegistry();
    base::AutoLock lock(registry.lock);
    registry.managers.erase(this);
  }

  bool HasFamily(std::string_view family) const {
    return !ResolveFamily(family).empty();
  }

  bool HasValidConfiguredFamilies() const {
    if (!HasFamily(default_family_) || !HasFamily(last_resort_family_) ||
        !HasFamily(emoji_family_) || !HasFamily(math_family_)) {
      return false;
    }
    for (const auto& [alias, family] : aliases_) {
      if (!HasFamily(family)) {
        return false;
      }
    }
    for (const auto& [language, families] : cjk_by_language_) {
      for (const std::string& family : families) {
        if (!HasFamily(family)) {
          return false;
        }
      }
    }
    return true;
  }

  std::optional<FontFileReference> GetFontFileReference(
      SkTypefaceID typeface_id) const {
    for (const std::shared_ptr<FontFaceRecord>& face : faces_) {
      if (face->HasTypefaceId(typeface_id)) {
        return FontFileReference{face->file->path, face->collection_index};
      }
    }
    return std::nullopt;
  }

 private:
  std::string ResolveFamily(std::string_view requested) const {
    std::string key = LowerASCII(requested);
    auto alias = aliases_.find(key);
    if (alias != aliases_.end()) {
      key = LowerASCII(alias->second);
    }
    auto family = family_lookup_.find(key);
    return family == family_lookup_.end() ? std::string() : family->second;
  }

  sk_sp<SkTypeface> MatchFamily(const std::string& family,
                                const SkFontStyle& style,
                                std::optional<SkUnichar> character) const {
    std::string resolved = ResolveFamily(family);
    if (resolved.empty()) {
      return nullptr;
    }
    auto family_faces = faces_by_family_.find(LowerASCII(resolved));
    if (family_faces == faces_by_family_.end()) {
      return nullptr;
    }
    std::vector<std::shared_ptr<FontFaceRecord>> candidates;
    for (const std::shared_ptr<FontFaceRecord>& face : family_faces->second) {
      if (!character || face->Covers(*character)) {
        candidates.push_back(face);
      }
    }
    return candidates.empty()
               ? nullptr
               : sk_make_sp<ChromimeFontStyleSet>(std::move(candidates))
                     ->matchStyle(style);
  }

  UNSAFE_BUFFER_USAGE std::vector<std::string> CjkCandidates(
      const char* bcp47[],
      int bcp47_count) const {
    for (int index = 0; index < bcp47_count; ++index) {
      const char* language_tag = UNSAFE_BUFFERS(bcp47[index]);
      if (!language_tag) {
        continue;
      }
      std::string language = language_tag;
      auto exact = cjk_by_language_.find(language);
      if (exact != cjk_by_language_.end()) {
        return exact->second;
      }
      size_t separator = language.find('-');
      if (separator != std::string::npos) {
        auto base_language =
            cjk_by_language_.find(language.substr(0, separator));
        if (base_language != cjk_by_language_.end()) {
          return base_language->second;
        }
      }
    }
    auto unspecified = cjk_by_language_.find("und");
    return unspecified == cjk_by_language_.end() ? std::vector<std::string>()
                                                 : unspecified->second;
  }

  int onCountFamilies() const override {
    return static_cast<int>(family_names_.size());
  }

  void onGetFamilyName(int index, SkString* family_name) const override {
    if (index < 0 || static_cast<size_t>(index) >= family_names_.size()) {
      family_name->reset();
      return;
    }
    family_name->set(family_names_[index].c_str());
  }

  sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const override {
    if (index < 0 || static_cast<size_t>(index) >= family_names_.size()) {
      return nullptr;
    }
    return onMatchFamily(family_names_[index].c_str());
  }

  sk_sp<SkFontStyleSet> onMatchFamily(
      const char requested_family[]) const override {
    if (!requested_family) {
      return nullptr;
    }
    std::string family = ResolveFamily(requested_family);
    if (family.empty()) {
      return nullptr;
    }
    auto faces = faces_by_family_.find(LowerASCII(family));
    return faces == faces_by_family_.end()
               ? nullptr
               : sk_make_sp<ChromimeFontStyleSet>(faces->second);
  }

  sk_sp<SkTypeface> onMatchFamilyStyle(
      const char requested_family[],
      const SkFontStyle& style) const override {
    if (!requested_family) {
      return nullptr;
    }
    return MatchFamily(requested_family, style, std::nullopt);
  }

  UNSAFE_BUFFER_USAGE sk_sp<SkTypeface> onMatchFamilyStyleCharacter(
      const char requested_family[],
      const SkFontStyle& style,
      const char* bcp47[],
      int bcp47_count,
      SkUnichar character) const override {
    if (requested_family) {
      if (sk_sp<SkTypeface> match =
              MatchFamily(requested_family, style, character)) {
        return match;
      }
    }
    if (IsEmojiCharacter(character)) {
      if (sk_sp<SkTypeface> match =
              MatchFamily(emoji_family_, style, character)) {
        return match;
      }
    }
    if (IsMathCharacter(character)) {
      if (sk_sp<SkTypeface> match =
              MatchFamily(math_family_, style, character)) {
        return match;
      }
    }
    if (IsCjkCharacter(character)) {
      for (const std::string& family : CjkCandidates(bcp47, bcp47_count)) {
        if (sk_sp<SkTypeface> match = MatchFamily(family, style, character)) {
          return match;
        }
      }
    }
    for (const std::string& family : fallback_families_) {
      if (sk_sp<SkTypeface> match = MatchFamily(family, style, character)) {
        return match;
      }
    }
    return nullptr;
  }

  sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData> data,
                                   int ttc_index) const override {
    return SkTypeface_Make_Fontations(
        std::move(data), SkFontArguments().setCollectionIndex(ttc_index));
  }

  sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset> stream,
                                          int ttc_index) const override {
    return SkTypeface_Make_Fontations(
        std::move(stream), SkFontArguments().setCollectionIndex(ttc_index));
  }

  sk_sp<SkTypeface> onMakeFromStreamArgs(
      std::unique_ptr<SkStreamAsset> stream,
      const SkFontArguments& args) const override {
    return SkTypeface_Make_Fontations(std::move(stream), args);
  }

  sk_sp<SkTypeface> onMakeFromFile(const char path[],
                                   int ttc_index) const override {
    std::unique_ptr<SkStreamAsset> stream = SkStream::MakeFromFile(path);
    return stream ? onMakeFromStreamIndex(std::move(stream), ttc_index)
                  : nullptr;
  }

  sk_sp<SkTypeface> onLegacyMakeTypeface(const char requested_family[],
                                         SkFontStyle style) const override {
    if (requested_family) {
      if (sk_sp<SkTypeface> match =
              onMatchFamilyStyle(requested_family, style)) {
        return match;
      }
    }
    return MatchFamily(default_family_, style, std::nullopt);
  }

  const std::map<std::string, std::string> aliases_;
  const std::map<std::string, std::vector<std::string>> cjk_by_language_;
  const std::string default_family_;
  const std::string last_resort_family_;
  const std::string emoji_family_;
  const std::string math_family_;
  const std::vector<std::shared_ptr<FontFaceRecord>> faces_;
  std::map<std::string, std::string> family_lookup_;
  std::map<std::string, std::vector<std::shared_ptr<FontFaceRecord>>>
      faces_by_family_;
  std::vector<std::string> family_names_;
  std::vector<std::string> fallback_families_;
};

bool ReadStringMap(const base::DictValue& dictionary,
                   std::map<std::string, std::string>* output,
                   std::string* error) {
  for (const auto [key, value] : dictionary) {
    if (!value.is_string() || value.GetString().empty()) {
      *error = "Expected a non-empty string for key: " + key;
      return false;
    }
    output->insert_or_assign(LowerASCII(key), value.GetString());
  }
  return true;
}

ConfiguredFontManager Fail(const base::FilePath& config_path,
                           std::string error) {
  ConfiguredFontManager result;
  result.enabled = true;
  result.config_path = config_path;
  result.error = std::move(error);
  return result;
}

bool ValidateRenderingProtocol(const base::DictValue* rendering,
                               std::string* error) {
  const std::string* hinting =
      rendering ? rendering->FindString("hinting") : nullptr;
  const std::string* subpixel_rendering =
      rendering ? rendering->FindString("subpixel_rendering") : nullptr;
  if (!rendering || !hinting || *hinting != "slight" ||
      rendering->FindBool("antialiasing") != true ||
      rendering->FindBool("subpixel_positioning") != true ||
      !subpixel_rendering || *subpixel_rendering != "none" ||
      rendering->FindBool("embedded_bitmaps") != true ||
      rendering->FindBool("autohinter") != false ||
      rendering->FindBool("synthetic_bold") != true ||
      rendering->FindBool("synthetic_italic") != true) {
    *error =
        "Rendering must use the Chromime cross-platform rendering protocol";
    return false;
  }
  return true;
}

}  // namespace

ConfiguredFontManager LoadFromConfigFile(const base::FilePath& config_path) {
  if (!config_path.IsAbsolute()) {
    return Fail(config_path,
                "Chromime font configuration path must be absolute");
  }

  std::string error;
  std::string config_json;
  if (!ReadTextFile(config_path, kMaximumConfigBytes, &config_json, &error)) {
    return Fail(config_path, std::move(error));
  }
  std::optional<base::Value> parsed_config =
      ParseJson(config_json, config_path, &error);
  if (!parsed_config || !parsed_config->is_dict()) {
    return Fail(config_path, error.empty()
                                 ? "Font configuration must be a JSON object"
                                 : std::move(error));
  }
  const base::DictValue& config = parsed_config->GetDict();
  if (config.FindInt("schema_version") != 1 ||
      !config.FindString("profile_id") ||
      config.FindString("profile_id")->empty() ||
      !ValidateRenderingProtocol(config.FindDict("rendering"), &error)) {
    return Fail(config_path,
                error.empty() ? "Invalid profile identity" : std::move(error));
  }
  const base::DictValue* font_pack = config.FindDict("font_pack");
  const base::DictValue* generic_families = config.FindDict("generic_families");
  const base::DictValue* fallback = config.FindDict("fallback");
  if (!font_pack || !generic_families || !fallback) {
    return Fail(
        config_path,
        "Configuration requires font_pack, generic_families, and fallback");
  }

  const std::string* pack_id = font_pack->FindString("id");
  const std::string* manifest_value = font_pack->FindString("manifest");
  const std::string* manifest_hash = font_pack->FindString("manifest_sha256");
  if (!pack_id || !manifest_value || !manifest_hash ||
      manifest_hash->size() != 64) {
    return Fail(config_path, "Invalid font_pack identity or manifest hash");
  }
  base::FilePath manifest_relative =
      base::FilePath::FromUTF8Unsafe(*manifest_value).NormalizePathSeparators();
  if (!IsSafeRelativePath(manifest_relative)) {
    return Fail(config_path, "Manifest path must be relative and contained");
  }
  base::FilePath manifest_path =
      config_path.DirName().Append(manifest_relative);

  std::string manifest_json;
  if (!ReadTextFile(manifest_path, kMaximumManifestBytes, &manifest_json,
                    &error)) {
    return Fail(config_path, std::move(error));
  }
  if (!base::EqualsCaseInsensitiveASCII(base::HexEncodeLower(crypto::SHA256Hash(
                                            base::as_byte_span(manifest_json))),
                                        *manifest_hash)) {
    return Fail(config_path, "Manifest SHA-256 does not match the profile");
  }

  std::optional<base::Value> parsed_manifest =
      ParseJson(manifest_json, manifest_path, &error);
  if (!parsed_manifest || !parsed_manifest->is_dict()) {
    return Fail(config_path, error.empty()
                                 ? "Font manifest must be a JSON object"
                                 : std::move(error));
  }
  const base::DictValue& manifest = parsed_manifest->GetDict();
  const std::string* actual_pack_id = manifest.FindString("pack_id");
  const base::ListValue* files = manifest.FindList("files");
  if (!actual_pack_id || *actual_pack_id != *pack_id || !files ||
      files->empty()) {
    return Fail(config_path, "Manifest pack identity or file list is invalid");
  }

  std::set<base::FilePath> verified_font_paths;
  std::map<base::FilePath, std::shared_ptr<FontFaceRecord::FontFileRecord>>
      font_files;
  std::map<std::string, std::string> family_classes;
  std::vector<std::shared_ptr<FontFaceRecord>> font_faces;
  for (const base::Value& file_value : *files) {
    if (!file_value.is_dict()) {
      return Fail(config_path, "Manifest file entry must be an object");
    }
    const base::DictValue& file = file_value.GetDict();
    const std::string* relative_value = file.FindString("path");
    const std::string* expected_hash = file.FindString("sha256");
    std::optional<int> expected_size = file.FindInt("size");
    const base::ListValue* faces = file.FindList("faces");
    if (!relative_value || !expected_hash || expected_hash->size() != 64 ||
        !expected_size || *expected_size < 0 || !faces) {
      return Fail(config_path, "Manifest file entry is incomplete");
    }
    base::FilePath relative_path =
        base::FilePath::FromUTF8Unsafe(*relative_value)
            .NormalizePathSeparators();
    if (!IsSafeRelativePath(relative_path)) {
      return Fail(config_path, "Manifest contains an unsafe font path");
    }
    base::FilePath font_path = manifest_path.DirName().Append(relative_path);
    std::optional<int64_t> actual_size = base::GetFileSize(font_path);
    if (!actual_size || *actual_size != *expected_size) {
      return Fail(config_path,
                  "Font size does not match manifest: " + *relative_value);
    }
    std::string actual_hash = Sha256Hex(font_path, &error);
    if (actual_hash.empty() ||
        !base::EqualsCaseInsensitiveASCII(actual_hash, *expected_hash)) {
      return Fail(config_path, error.empty() ? "Font SHA-256 does not match: " +
                                                   *relative_value
                                             : std::move(error));
    }
    verified_font_paths.insert(font_path);
    auto [font_file_it, inserted] = font_files.try_emplace(font_path);
    if (inserted) {
      font_file_it->second =
          std::make_shared<FontFaceRecord::FontFileRecord>(font_path);
    }
    std::shared_ptr<FontFaceRecord::FontFileRecord> font_file =
        font_file_it->second;

    for (const base::Value& face_value : *faces) {
      if (!face_value.is_dict()) {
        return Fail(config_path, "Manifest face entry must be an object");
      }
      const std::string* family = face_value.GetDict().FindString("family");
      const std::string* subfamily =
          face_value.GetDict().FindString("subfamily");
      const std::string* family_class =
          face_value.GetDict().FindString("family_class");
      std::optional<int> collection_index =
          face_value.GetDict().FindInt("collection_index");
      std::optional<int> weight = face_value.GetDict().FindInt("weight");
      std::optional<int> width = face_value.GetDict().FindInt("width");
      const base::ListValue* coverage =
          face_value.GetDict().FindList("coverage");
      if (!family || family->empty() || !subfamily || subfamily->empty() ||
          !family_class || family_class->empty() || !collection_index ||
          *collection_index < 0 || !weight || *weight < 1 || *weight > 1000 ||
          !width || *width < 1 || *width > 9 || !coverage ||
          coverage->empty()) {
        return Fail(config_path, "Manifest face family metadata is incomplete");
      }
      family_classes.emplace(*family, *family_class);

      auto record = std::make_shared<FontFaceRecord>();
      record->family = *family;
      record->subfamily = *subfamily;
      record->file = font_file;
      record->collection_index = *collection_index;
      const std::string lower_subfamily = LowerASCII(*subfamily);
      const SkFontStyle::Slant slant =
          lower_subfamily.find("oblique") != std::string::npos
              ? SkFontStyle::kOblique_Slant
          : lower_subfamily.find("italic") != std::string::npos
              ? SkFontStyle::kItalic_Slant
              : SkFontStyle::kUpright_Slant;
      record->style = SkFontStyle(*weight, *width, slant);
      for (const base::Value& coverage_value : *coverage) {
        std::pair<SkUnichar, SkUnichar> range;
        if (!coverage_value.is_string() ||
            !ParseCoverageRange(coverage_value.GetString(), &range)) {
          return Fail(config_path, "Manifest has invalid coverage metadata");
        }
        record->coverage.push_back(range);
      }
      std::ranges::sort(record->coverage);
      font_faces.push_back(std::move(record));
    }
  }

  base::FilePath fonts_directory = manifest_path.DirName().AppendASCII("fonts");
  base::FileEnumerator enumerator(fonts_directory, true,
                                  base::FileEnumerator::FILES);
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    const base::FilePath::StringType extension = path.Extension();
    if ((base::FilePath::CompareEqualIgnoreCase(extension,
                                                FILE_PATH_LITERAL(".ttf")) ||
         base::FilePath::CompareEqualIgnoreCase(extension,
                                                FILE_PATH_LITERAL(".ttc")) ||
         base::FilePath::CompareEqualIgnoreCase(extension,
                                                FILE_PATH_LITERAL(".otf")) ||
         base::FilePath::CompareEqualIgnoreCase(extension,
                                                FILE_PATH_LITERAL(".otc"))) &&
        !verified_font_paths.contains(path)) {
      return Fail(config_path, "Font directory contains an unlisted file: " +
                                   path.AsUTF8Unsafe());
    }
  }

  if (font_faces.empty()) {
    return Fail(config_path, "No font faces could be loaded from the manifest");
  }

  std::map<std::string, std::string> aliases;
  if (const base::DictValue* configured_aliases = config.FindDict("aliases")) {
    if (!ReadStringMap(*configured_aliases, &aliases, &error)) {
      return Fail(config_path, std::move(error));
    }
  }
  for (const auto [generic, value] : *generic_families) {
    if (!value.is_string()) {
      return Fail(config_path, "Generic family mapping must be a string");
    }
    std::string css_generic = generic;
    std::ranges::replace(css_generic, '_', '-');
    aliases.insert_or_assign(LowerASCII(css_generic), value.GetString());
  }

  const std::string* default_family =
      fallback->FindString("default_text_family");
  const std::string* last_resort_family =
      fallback->FindString("last_resort_family");
  const std::string* emoji_family = fallback->FindString("emoji_family");
  const std::string* math_family = fallback->FindString("math_family");
  const base::ListValue* ordered_classes =
      fallback->FindList("ordered_family_classes");
  const base::DictValue* configured_cjk = fallback->FindDict("cjk_by_language");
  const std::string* fallback_strategy = fallback->FindString("strategy");
  if (!default_family || !last_resort_family || !emoji_family || !math_family ||
      !ordered_classes || !configured_cjk || !fallback_strategy ||
      *fallback_strategy != "manifest_coverage_order_v1") {
    return Fail(config_path, "Fallback configuration is incomplete");
  }
  std::vector<std::string> ordered_family_classes;
  for (const base::Value& value : *ordered_classes) {
    if (!value.is_string()) {
      return Fail(config_path, "Fallback family class must be a string");
    }
    ordered_family_classes.push_back(value.GetString());
  }
  std::map<std::string, std::vector<std::string>> cjk_by_language;
  for (const auto [language, value] : *configured_cjk) {
    if (!value.is_list()) {
      return Fail(config_path, "CJK fallback mapping must be a list");
    }
    std::vector<std::string> families;
    for (const base::Value& family : value.GetList()) {
      if (!family.is_string()) {
        return Fail(config_path, "CJK fallback family must be a string");
      }
      families.push_back(family.GetString());
    }
    cjk_by_language.emplace(language, std::move(families));
  }

  sk_sp<ChromimeFontManager> font_manager = sk_make_sp<ChromimeFontManager>(
      std::move(font_faces), std::move(aliases),
      std::move(ordered_family_classes), std::move(family_classes),
      std::move(cjk_by_language), *default_family, *last_resort_family,
      *emoji_family, *math_family);
  if (!font_manager->HasValidConfiguredFamilies()) {
    return Fail(config_path,
                "A configured font family is not present in the pack");
  }

  ConfiguredFontManager result;
  result.enabled = true;
  result.font_manager = std::move(font_manager);
  result.config_path = config_path;
  return result;
}

ConfiguredFontManager LoadFromCommandLine() {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (!command_line.HasSwitch(kChromimeFontConfigSwitch)) {
    return {};
  }
  return LoadFromConfigFile(
      command_line.GetSwitchValuePath(kChromimeFontConfigSwitch));
}

std::optional<FontFileReference> GetFontFileReference(
    const SkFontMgr& font_manager,
    SkTypefaceID typeface_id) {
  FontManagerRegistry& registry = GetFontManagerRegistry();
  base::AutoLock lock(registry.lock);
  auto manager = registry.managers.find(&font_manager);
  return manager == registry.managers.end()
             ? std::nullopt
             : manager->second->GetFontFileReference(typeface_id);
}

}  // namespace chromime_fonts
