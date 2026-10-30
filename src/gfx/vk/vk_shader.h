/* Shaders: the game's Maxwell programs translated to SPIR-V at run time (vk_shader.c, after upstream totk_shader_rt.m).
   A translation ("variant") depends on answers from the bound state (texture targets per slot, image formats, vertex
   attribute and colour output component classes); the replay picks the variant whose recorded dependencies match. */
#pragma once
#include "gfx/vk/vk.h"

enum { RB_SUPPORT, RB_C1, RB_DRV0, RB_DRV2, RB_CBUF, RB_SBUF, RB_UBOPTR, RB_SBUFUBO, RB_TEX, RB_SAMP, RB_IMG, RB_NONE };
enum { DT_UBO, DT_SSBO, DT_CIS, DT_SI, DT_SAMP, DT_UTB, DT_SIMG, DT_STB };
typedef struct { uint16_t binding, count; uint8_t type, kind, rw, view; int16_t reg, sreg; uint16_t off; uint8_t dyn; } vs_bind;   /* dyn: first dynamic offset index (uniform buffers of a ndyn shader) */   /* view: VkImageViewType */
typedef struct vk_shader {
    char name[112]; int stage;   /* NVN stage: 0 vertex, 1 fragment, 5 compute */
    VkShaderModule mod; VkDescriptorSetLayout dsl;
    int ndyn;   /* uniform buffers are dynamic: their offsets are given at bind time (0: plain) */
    int early;  /* stage_set may reuse its last set when no binding but uniforms changed: 0 not decided, 1 yes, 2 no */
    int nb; vs_bind *b;
    uint32_t in_mask, out_mask, local[3]; int writes_depth;
    VkBuffer c1_buf; VkDeviceSize c1_off; uint32_t c1_size;   /* the shader's own constants (bank c1), in the c1 arena */
    int ndep; struct dep { uint8_t kind; uint8_t slot; char val[30]; } *dep;
} vk_shader;

typedef struct vk_prog vk_prog;
vk_prog *vs_prog(const prog_t *p);              /* NULL when the program cannot be used (logged once) */
vk_shader *vs_stage(vk_prog *pr, int stage);    /* the fitting variant of that stage, or NULL (absent / failed) */
int vs_has_stage(const vk_prog *pr, int stage);
void vs_prog_release(uint64_t serial);          /* nvnProgramFinalize / re-initialised: the next use reads it again */
void vs_init(void);
