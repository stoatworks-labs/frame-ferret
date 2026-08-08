// Prints libsrt's real option numbers and payload constants.
//
// Same discipline as tools/ndi_abi.c and tools/omt_abi.c. It earned its keep
// immediately: SRTO_SNDTIMEO and SRTO_RCVTIMEO were mirrored from memory as 38
// and 37 and are really 13 and 14, and SRTO_STREAMID has no explicit value in
// the header at all — it is positional, so it cannot be read off by eye.
//
// Build: cc -I/opt/homebrew/include tools/srt_abi.c -o build/srt_abi
#include <stdio.h>
#include <srt/srt.h>

int main(void) {
  printf("SRTO_SNDSYN      %d\n", SRTO_SNDSYN);
  printf("SRTO_RCVSYN      %d\n", SRTO_RCVSYN);
  printf("SRTO_SNDTIMEO    %d\n", SRTO_SNDTIMEO);
  printf("SRTO_RCVTIMEO    %d\n", SRTO_RCVTIMEO);
  printf("SRTO_LATENCY     %d\n", SRTO_LATENCY);
  printf("SRTO_PASSPHRASE  %d\n", SRTO_PASSPHRASE);
  printf("SRTO_STREAMID    %d\n", SRTO_STREAMID);
  printf("SRTO_TRANSTYPE   %d\n", SRTO_TRANSTYPE);
  printf("SRTO_PAYLOADSIZE %d\n", SRTO_PAYLOADSIZE);
  printf("SRTT_LIVE        %d\n", SRTT_LIVE);
  printf("SRT_ERROR        %d\n", SRT_ERROR);
  printf("SRT_INVALID_SOCK %d\n", SRT_INVALID_SOCK);
  printf("SRT_LIVE_DEF_PLSIZE %d\n", SRT_LIVE_DEF_PLSIZE);
  printf("sizeof(SRT_TRACEBSTATS) %zu\n", sizeof(SRT_TRACEBSTATS));
  printf("SRT_ETIMEOUT     %d\n", SRT_ETIMEOUT);
  printf("SRT_EASYNCRCV    %d\n", SRT_EASYNCRCV);
  printf("SRT_ECONNLOST    %d\n", SRT_ECONNLOST);
  printf("SRT_ENOCONN      %d\n", SRT_ENOCONN);
  printf("SRT_EINVSOCK     %d\n", SRT_EINVSOCK);
  return 0;
}
