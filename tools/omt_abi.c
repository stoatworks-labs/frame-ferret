// Prints the real sizes and field offsets of OMTMediaFrame.
//
// Same discipline as tools/ndi_abi.c, and for the same reason: this repo does
// not vendor libomt.h, so src/transports/omt_abi.h is a hand-written mirror and
// a wrong offset there reads the wrong field rather than crashing.
//
// Build (needs libomt.h, which we do not vendor):
//   cc -I$HOME/.local/lib/omt tools/omt_abi.c -o build/omt_abi
//
// Then check the numbers against the static_asserts in
// src/transports/omt_abi.h. Never update those by reasoning — run this.

#include <stdio.h>
#include <stddef.h>

#include "libomt.h"

#define SHOW_SIZE(T) printf("%-24s size %3zu\n", #T, sizeof(T))
#define SHOW_OFF(T, F) \
  printf("  %-24s @ %3zu  (size %zu)\n", #F, offsetof(T, F), sizeof(((T*)0)->F))

int main(void) {
  printf("pointer %zu, int %zu, long long %zu, float %zu, enum %zu\n\n",
         sizeof(void*), sizeof(int), sizeof(long long), sizeof(float),
         sizeof(OMTFrameType));

  printf("omt_receive_t size %zu, omt_send_t size %zu\n\n",
         sizeof(omt_receive_t), sizeof(omt_send_t));

  SHOW_SIZE(OMTMediaFrame);
  SHOW_OFF(OMTMediaFrame, Type);
  SHOW_OFF(OMTMediaFrame, Timestamp);
  SHOW_OFF(OMTMediaFrame, Codec);
  SHOW_OFF(OMTMediaFrame, Width);
  SHOW_OFF(OMTMediaFrame, Height);
  SHOW_OFF(OMTMediaFrame, Stride);
  SHOW_OFF(OMTMediaFrame, Flags);
  SHOW_OFF(OMTMediaFrame, FrameRateN);
  SHOW_OFF(OMTMediaFrame, FrameRateD);
  SHOW_OFF(OMTMediaFrame, AspectRatio);
  SHOW_OFF(OMTMediaFrame, ColorSpace);
  SHOW_OFF(OMTMediaFrame, SampleRate);
  SHOW_OFF(OMTMediaFrame, Channels);
  SHOW_OFF(OMTMediaFrame, SamplesPerChannel);
  SHOW_OFF(OMTMediaFrame, Data);
  SHOW_OFF(OMTMediaFrame, DataLength);
  SHOW_OFF(OMTMediaFrame, CompressedData);
  SHOW_OFF(OMTMediaFrame, CompressedLength);
  SHOW_OFF(OMTMediaFrame, FrameMetadata);
  SHOW_OFF(OMTMediaFrame, FrameMetadataLength);
  printf("\n");

  printf("FrameType none=%d meta=%d video=%d audio=%d\n", OMTFrameType_None,
         OMTFrameType_Metadata, OMTFrameType_Video, OMTFrameType_Audio);
  printf("Codec VMX1=0x%08X FPA1=0x%08X UYVY=0x%08X BGRA=0x%08X NV12=0x%08X\n",
         OMTCodec_VMX1, OMTCodec_FPA1, OMTCodec_UYVY, OMTCodec_BGRA,
         OMTCodec_NV12);
  printf("Quality default=%d low=%d medium=%d high=%d\n", OMTQuality_Default,
         OMTQuality_Low, OMTQuality_Medium, OMTQuality_High);
  printf("PreferredVideoFormat UYVY=%d UYVYorBGRA=%d BGRA=%d\n",
         OMTPreferredVideoFormat_UYVY, OMTPreferredVideoFormat_UYVYorBGRA,
         OMTPreferredVideoFormat_BGRA);
  printf("ColorSpace undefined=%d 601=%d 709=%d\n", OMTColorSpace_Undefined,
         OMTColorSpace_BT601, OMTColorSpace_BT709);
  printf("VideoFlags none=%d interlaced=%d alpha=%d premul=%d hibit=%d\n",
         OMTVideoFlags_None, OMTVideoFlags_Interlaced, OMTVideoFlags_Alpha,
         OMTVideoFlags_PreMultiplied, OMTVideoFlags_HighBitDepth);
  printf("ReceiveFlags none=%d preview=%d incComp=%d compOnly=%d\n",
         OMTReceiveFlags_None, OMTReceiveFlags_Preview,
         OMTReceiveFlags_IncludeCompressed, OMTReceiveFlags_CompressedOnly);
  return 0;
}
