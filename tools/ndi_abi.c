// Prints the real sizes and field offsets of the NDI structs we mirror.
//
// The fleet learned this the hard way: hand arithmetic got
// NDIlib_audio_frame_v3_t wrong (it is 64 bytes, not 56), and a wrong layout
// does not crash — it silently reads the wrong field, so a receiver "connects"
// and delivers rubbish.
//
// Offsets matter as much as sizes: two same-typed adjacent fields swapped give
// an identical total size and completely different behaviour.
//
// Build (needs the NDI SDK headers, which we do not vendor):
//   cc -I"/Library/NDI SDK for Apple/include" tools/ndi_abi.c -o build/ndi_abi
//
// Then check the numbers against the static_asserts in
// src/transports/ndi_abi.h. Never update those asserts by reasoning — run this.

#include <stdio.h>
#include <stddef.h>

#include "Processing.NDI.Lib.h"

#define SHOW_SIZE(T) printf("%-34s size %3zu\n", #T, sizeof(T))
#define SHOW_OFF(T, F) \
  printf("  %-30s @ %3zu  (size %zu)\n", #F, offsetof(T, F), sizeof(((T*)0)->F))

int main(void) {
  printf("pointer size %zu, bool size %zu\n\n", sizeof(void*), sizeof(bool));

  SHOW_SIZE(NDIlib_source_t);
  SHOW_OFF(NDIlib_source_t, p_ndi_name);
  SHOW_OFF(NDIlib_source_t, p_url_address);
  printf("\n");

  SHOW_SIZE(NDIlib_video_frame_v2_t);
  SHOW_OFF(NDIlib_video_frame_v2_t, xres);
  SHOW_OFF(NDIlib_video_frame_v2_t, yres);
  SHOW_OFF(NDIlib_video_frame_v2_t, FourCC);
  SHOW_OFF(NDIlib_video_frame_v2_t, frame_rate_N);
  SHOW_OFF(NDIlib_video_frame_v2_t, frame_rate_D);
  SHOW_OFF(NDIlib_video_frame_v2_t, picture_aspect_ratio);
  SHOW_OFF(NDIlib_video_frame_v2_t, frame_format_type);
  SHOW_OFF(NDIlib_video_frame_v2_t, timecode);
  SHOW_OFF(NDIlib_video_frame_v2_t, p_data);
  SHOW_OFF(NDIlib_video_frame_v2_t, line_stride_in_bytes);
  SHOW_OFF(NDIlib_video_frame_v2_t, p_metadata);
  SHOW_OFF(NDIlib_video_frame_v2_t, timestamp);
  printf("\n");

  SHOW_SIZE(NDIlib_audio_frame_v3_t);
  SHOW_OFF(NDIlib_audio_frame_v3_t, sample_rate);
  SHOW_OFF(NDIlib_audio_frame_v3_t, no_channels);
  SHOW_OFF(NDIlib_audio_frame_v3_t, no_samples);
  SHOW_OFF(NDIlib_audio_frame_v3_t, timecode);
  SHOW_OFF(NDIlib_audio_frame_v3_t, FourCC);
  SHOW_OFF(NDIlib_audio_frame_v3_t, p_data);
  SHOW_OFF(NDIlib_audio_frame_v3_t, channel_stride_in_bytes);
  SHOW_OFF(NDIlib_audio_frame_v3_t, p_metadata);
  SHOW_OFF(NDIlib_audio_frame_v3_t, timestamp);
  printf("\n");

  SHOW_SIZE(NDIlib_find_create_t);
  SHOW_OFF(NDIlib_find_create_t, show_local_sources);
  SHOW_OFF(NDIlib_find_create_t, p_groups);
  SHOW_OFF(NDIlib_find_create_t, p_extra_ips);
  printf("\n");

  SHOW_SIZE(NDIlib_send_create_t);
  SHOW_OFF(NDIlib_send_create_t, p_ndi_name);
  SHOW_OFF(NDIlib_send_create_t, p_groups);
  SHOW_OFF(NDIlib_send_create_t, clock_video);
  SHOW_OFF(NDIlib_send_create_t, clock_audio);
  printf("\n");

  SHOW_SIZE(NDIlib_recv_create_v3_t);
  SHOW_OFF(NDIlib_recv_create_v3_t, source_to_connect_to);
  SHOW_OFF(NDIlib_recv_create_v3_t, color_format);
  SHOW_OFF(NDIlib_recv_create_v3_t, bandwidth);
  SHOW_OFF(NDIlib_recv_create_v3_t, allow_video_fields);
  SHOW_OFF(NDIlib_recv_create_v3_t, p_ndi_recv_name);
  printf("\n");

  printf("FourCC UYVY=0x%08X BGRA=0x%08X BGRX=0x%08X\n",
         (unsigned)NDIlib_FourCC_video_type_UYVY,
         (unsigned)NDIlib_FourCC_video_type_BGRA,
         (unsigned)NDIlib_FourCC_video_type_BGRX);
  printf("audio FourCC FLTP=0x%08X\n", (unsigned)NDIlib_FourCC_audio_type_FLTP);
  printf("recv_color_format BGRX_BGRA=%d UYVY_BGRA=%d fastest=%d best=%d\n",
         (int)NDIlib_recv_color_format_BGRX_BGRA,
         (int)NDIlib_recv_color_format_UYVY_BGRA,
         (int)NDIlib_recv_color_format_fastest,
         (int)NDIlib_recv_color_format_best);
  printf("bandwidth highest=%d lowest=%d audio_only=%d metadata_only=%d\n",
         (int)NDIlib_recv_bandwidth_highest,
         (int)NDIlib_recv_bandwidth_lowest,
         (int)NDIlib_recv_bandwidth_audio_only,
         (int)NDIlib_recv_bandwidth_metadata_only);
  printf("frame_type none=%d video=%d audio=%d metadata=%d error=%d\n",
         (int)NDIlib_frame_type_none, (int)NDIlib_frame_type_video,
         (int)NDIlib_frame_type_audio, (int)NDIlib_frame_type_metadata,
         (int)NDIlib_frame_type_error);
  printf("frame_format progressive=%d interleaved=%d field0=%d field1=%d\n",
         (int)NDIlib_frame_format_type_progressive,
         (int)NDIlib_frame_format_type_interleaved,
         (int)NDIlib_frame_format_type_field_0,
         (int)NDIlib_frame_format_type_field_1);
  return 0;
}
