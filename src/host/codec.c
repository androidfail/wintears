/* nn::codec hardware Opus decoder: upstream games/totk/runtime/opus_codec.c, compiled unchanged with libopus
   (externals/opus). Upstream's override table gives function pointers only; this table adds how each result goes
   back to the guest. */
#include "host/hos.h"

void *runtime_codec_override(const char *name);   /* upstream opus_codec.c */

static const struct { const char *name; int ret; } kinds[] = {
    {"_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", HOST_RET_U64},
    {"_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", HOST_RET_U32},
    {"_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", HOST_RET_VOID},
    {"_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", HOST_RET_U32},
    {"_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", HOST_RET_U32},
    {"_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", HOST_RET_U32},
    {"_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", HOST_RET_U32},
};
static host_fn table[sizeof kinds / sizeof *kinds];

const host_fn *host_codec_override(const char *name) {
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        if (strcmp(name, kinds[i].name)) continue;
        if (!table[i].fn) {
            table[i] = (host_fn){kinds[i].name, runtime_codec_override(name), kinds[i].ret};
            if (!table[i].fn) die("codec override %s is missing upstream", name);
        }
        return &table[i];
    }
    if (runtime_codec_override(name)) die("codec override %s has no result kind here (src/host/codec.c)", name);
    return NULL;
}
