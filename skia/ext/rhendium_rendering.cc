// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// This low-level Skia row blitter operates on caller-owned pixel buffers.
#pragma allow_unsafe_buffers
#endif

#include "skia/ext/rhendium_rendering.h"

#include "third_party/skia/include/private/SkFeatures.h"
#include "third_party/skia/src/core/SkBlitRow.h"
#include "third_party/skia/src/core/SkMSAN.h"

#if defined(SK_ARM_HAS_NEON)
#include <arm_neon.h>
#endif

namespace skia {
namespace {

#if defined(SK_ARM_HAS_NEON)

uint8x8_t MulDiv255LikeX86(uint8x8_t inverse_alpha, uint8x8_t destination) {
  // SSE2 and AVX2 approximate dst * (255 - alpha) / 255 as
  // dst * (256 - alpha) >> 8. Use the same integer operation on NEON so that
  // transparent edges do not differ by one between x86 and ARM screenshots.
  uint16x8_t scale = vaddq_u16(vmovl_u8(inverse_alpha), vdupq_n_u16(1));
  uint16x8_t product = vmulq_u16(scale, vmovl_u8(destination));
  return vshrn_n_u16(product, 8);
}

uint8x8x4_t SourceOver8(uint8x8x4_t destination, uint8x8x4_t source) {
  uint8x8_t inverse_alpha = vmvn_u8(source.val[3]);
  return {
      vqadd_u8(source.val[0],
               MulDiv255LikeX86(inverse_alpha, destination.val[0])),
      vqadd_u8(source.val[1],
               MulDiv255LikeX86(inverse_alpha, destination.val[1])),
      vqadd_u8(source.val[2],
               MulDiv255LikeX86(inverse_alpha, destination.val[2])),
      vqadd_u8(source.val[3],
               MulDiv255LikeX86(inverse_alpha, destination.val[3])),
  };
}

uint8x8_t SourceOver2(uint8x8_t destination, uint8x8_t source) {
  const uint8x8_t alpha_indices = vcreate_u8(0x0707070703030303);
  uint8x8_t inverse_alpha = vmvn_u8(vtbl1_u8(source, alpha_indices));
  return vqadd_u8(source, MulDiv255LikeX86(inverse_alpha, destination));
}

void RhendiumBlitRowS32AOpaque(SkPMColor* destination,
                               const SkPMColor* source,
                               int length,
                               U8CPU alpha) {
  SkASSERT(alpha == 0xFF);
  sk_msan_assert_initialized(source, source + length);

  while (length >= 8) {
    vst4_u8(reinterpret_cast<uint8_t*>(destination),
            SourceOver8(vld4_u8(reinterpret_cast<const uint8_t*>(destination)),
                        vld4_u8(reinterpret_cast<const uint8_t*>(source))));
    source += 8;
    destination += 8;
    length -= 8;
  }

  while (length >= 2) {
    vst1_u8(reinterpret_cast<uint8_t*>(destination),
            SourceOver2(vld1_u8(reinterpret_cast<const uint8_t*>(destination)),
                        vld1_u8(reinterpret_cast<const uint8_t*>(source))));
    source += 2;
    destination += 2;
    length -= 2;
  }

  if (length != 0) {
    uint8x8_t result =
        SourceOver2(vcreate_u8(static_cast<uint64_t>(*destination)),
                    vcreate_u8(static_cast<uint64_t>(*source)));
    vst1_lane_u32(destination, vreinterpret_u32_u8(result), 0);
  }
}

#endif  // defined(SK_ARM_HAS_NEON)

}  // namespace

void EnableRhendiumCrossPlatformBlending() {
#if defined(SK_ARM_HAS_NEON)
  SkOpts::blit_row_s32a_opaque = RhendiumBlitRowS32AOpaque;
#endif
}

}  // namespace skia
