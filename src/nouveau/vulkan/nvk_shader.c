
#include "nvk_device.h"
#include "nvk_shader.h"
#include "nvk_physical_device.h"
#include "nvk_pipeline_layout.h"
#include "nvk_nir.h"

#include "nouveau_bo.h"
#include "nouveau_context.h"
#include "vk_shader_module.h"

#include "nir.h"
#include "nir_builder.h"
#include "compiler/spirv/nir_spirv.h"

#include "nv50_ir_driver.h"

static void
shared_var_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size = glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length, *align = comp_size;
}

static inline enum pipe_shader_type
pipe_shader_type_from_mesa(gl_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      return PIPE_SHADER_VERTEX;
   case MESA_SHADER_TESS_CTRL:
      return PIPE_SHADER_TESS_CTRL;
   case MESA_SHADER_TESS_EVAL:
      return PIPE_SHADER_TESS_EVAL;
   case MESA_SHADER_GEOMETRY:
      return PIPE_SHADER_GEOMETRY;
   case MESA_SHADER_FRAGMENT:
      return PIPE_SHADER_FRAGMENT;
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      return PIPE_SHADER_COMPUTE;
   default:
      unreachable("bad shader stage");
   }
}

const nir_shader_compiler_options *
nvk_physical_device_nir_options(const struct nvk_physical_device *pdevice,
                                gl_shader_stage stage)
{
   enum pipe_shader_type p_stage = pipe_shader_type_from_mesa(stage);
   return nv50_ir_nir_shader_compiler_options(pdevice->dev->chipset, p_stage);
}

static const struct spirv_to_nir_options spirv_options = {
   .caps = {
      .image_write_without_format = true,
   },
   .ssbo_addr_format = nir_address_format_64bit_global_32bit_offset,
   .ubo_addr_format = nir_address_format_64bit_global_32bit_offset,
   .shared_addr_format = nir_address_format_32bit_offset,
};

const struct spirv_to_nir_options *
nvk_physical_device_spirv_options(const struct nvk_physical_device *pdevice)
{
   return &spirv_options;
}

static bool
lower_load_global_constant_offset_instr(nir_builder *b, nir_instr *instr,
                                        UNUSED void *_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   if (intrin->intrinsic != nir_intrinsic_load_global_constant_offset)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_ssa_def *val =
      nir_build_load_global(b, intrin->dest.ssa.num_components,
                            intrin->dest.ssa.bit_size,
                            nir_iadd(b, intrin->src[0].ssa,
                                        nir_u2u64(b, intrin->src[1].ssa)),
                            .access = nir_intrinsic_access(intrin),
                            .align_mul = nir_intrinsic_align_mul(intrin),
                            .align_offset = nir_intrinsic_align_offset(intrin));
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, val);

   return true;
}

static int
count_location_slots(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static void
assign_io_locations(nir_shader *nir)
{
   nir_assign_var_locations(nir, nir_var_shader_in, &nir->num_inputs,
                            count_location_slots);
   nir_assign_var_locations(nir, nir_var_shader_out, &nir->num_outputs,
                            count_location_slots);
}

void
nvk_lower_nir(struct nvk_device *device, nir_shader *nir,
              const struct nvk_pipeline_layout *layout)
{
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);

   NIR_PASS(_, nir, nir_split_struct_vars, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   NIR_PASS(_, nir, nir_lower_system_values);

   nir_lower_compute_system_values_options csv_options = {
   };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv_options);

   /* Vulkan uses the separate-shader linking model */
   nir->info.separate_shader = true;

   /* Lower push constants before lower_descriptors */
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);

   NIR_PASS(_, nir, nvk_nir_lower_descriptors, layout, true);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo,
            spirv_options.ssbo_addr_format);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,
            spirv_options.ubo_addr_format);
   NIR_PASS(_, nir, nir_shader_instructions_pass,
            lower_load_global_constant_offset_instr,
            nir_metadata_block_index | nir_metadata_dominance, NULL);

   if (!nir->info.shared_memory_explicit_layout) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types,
               nir_var_mem_shared, shared_var_info);
   }
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
            nir_address_format_32bit_offset);

   NIR_PASS(_, nir, nir_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);

   if (nir->info.stage != MESA_SHADER_COMPUTE)
      assign_io_locations(nir);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

#ifndef NDEBUG
static void
nvk_shader_dump(struct nvk_shader *shader)
{
   unsigned pos;

   if (shader->stage != MESA_SHADER_COMPUTE) {
      _debug_printf("dumping HDR for %s shader\n",
                    _mesa_shader_stage_to_string(shader->stage));
      for (pos = 0; pos < ARRAY_SIZE(shader->hdr); ++pos)
         _debug_printf("HDR[%02"PRIxPTR"] = 0x%08x\n",
                      pos * sizeof(shader->hdr[0]), shader->hdr[pos]);
   }
   _debug_printf("shader binary code (0x%x bytes):", shader->code_size);
   for (pos = 0; pos < shader->code_size / 4; ++pos) {
      if ((pos % 8) == 0)
         _debug_printf("\n");
      _debug_printf("%08x ", ((uint32_t *)shader->code_ptr)[pos]);
   }
   _debug_printf("\n");
}
#endif

#include "tgsi/tgsi_ureg.h"

/* NOTE: Using a[0x270] in FP may cause an error even if we're using less than
 * 124 scalar varying values.
 */
static uint32_t
nvc0_shader_input_address(unsigned sn, unsigned si)
{
   switch (sn) {
   case TGSI_SEMANTIC_TESSOUTER:    return 0x000 + si * 0x4;
   case TGSI_SEMANTIC_TESSINNER:    return 0x010 + si * 0x4;
   case TGSI_SEMANTIC_PATCH:        return 0x020 + si * 0x10;
   case TGSI_SEMANTIC_PRIMID:       return 0x060;
   case TGSI_SEMANTIC_LAYER:        return 0x064;
   case TGSI_SEMANTIC_VIEWPORT_INDEX:return 0x068;
   case TGSI_SEMANTIC_PSIZE:        return 0x06c;
   case TGSI_SEMANTIC_POSITION:     return 0x070;
   case TGSI_SEMANTIC_GENERIC:      return 0x080 + si * 0x10;
   case TGSI_SEMANTIC_FOG:          return 0x2e8;
   case TGSI_SEMANTIC_COLOR:        return 0x280 + si * 0x10;
   case TGSI_SEMANTIC_BCOLOR:       return 0x2a0 + si * 0x10;
   case TGSI_SEMANTIC_CLIPDIST:     return 0x2c0 + si * 0x10;
   case TGSI_SEMANTIC_CLIPVERTEX:   return 0x270;
   case TGSI_SEMANTIC_PCOORD:       return 0x2e0;
   case TGSI_SEMANTIC_TESSCOORD:    return 0x2f0;
   case TGSI_SEMANTIC_INSTANCEID:   return 0x2f8;
   case TGSI_SEMANTIC_VERTEXID:     return 0x2fc;
   case TGSI_SEMANTIC_TEXCOORD:     return 0x300 + si * 0x10;
   default:
      assert(!"invalid TGSI input semantic");
      return ~0;
   }
}

static uint32_t
nvc0_shader_output_address(unsigned sn, unsigned si)
{
   switch (sn) {
   case TGSI_SEMANTIC_TESSOUTER:     return 0x000 + si * 0x4;
   case TGSI_SEMANTIC_TESSINNER:     return 0x010 + si * 0x4;
   case TGSI_SEMANTIC_PATCH:         return 0x020 + si * 0x10;
   case TGSI_SEMANTIC_PRIMID:        return 0x060;
   case TGSI_SEMANTIC_LAYER:         return 0x064;
   case TGSI_SEMANTIC_VIEWPORT_INDEX:return 0x068;
   case TGSI_SEMANTIC_PSIZE:         return 0x06c;
   case TGSI_SEMANTIC_POSITION:      return 0x070;
   case TGSI_SEMANTIC_GENERIC:       return 0x080 + si * 0x10;
   case TGSI_SEMANTIC_FOG:           return 0x2e8;
   case TGSI_SEMANTIC_COLOR:         return 0x280 + si * 0x10;
   case TGSI_SEMANTIC_BCOLOR:        return 0x2a0 + si * 0x10;
   case TGSI_SEMANTIC_CLIPDIST:      return 0x2c0 + si * 0x10;
   case TGSI_SEMANTIC_CLIPVERTEX:    return 0x270;
   case TGSI_SEMANTIC_TEXCOORD:      return 0x300 + si * 0x10;
   case TGSI_SEMANTIC_VIEWPORT_MASK: return 0x3a0;
   case TGSI_SEMANTIC_EDGEFLAG:      return ~0;
   default:
      assert(!"invalid TGSI output semantic");
      return ~0;
   }
}

static int
nvc0_vp_assign_input_slots(struct nv50_ir_prog_info_out *info)
{
   unsigned i, c, n;

   for (n = 0, i = 0; i < info->numInputs; ++i) {
      switch (info->in[i].sn) {
      case TGSI_SEMANTIC_INSTANCEID: /* for SM4 only, in TGSI they're SVs */
      case TGSI_SEMANTIC_VERTEXID:
         info->in[i].mask = 0x1;
         info->in[i].slot[0] =
            nvc0_shader_input_address(info->in[i].sn, 0) / 4;
         continue;
      default:
         break;
      }
      for (c = 0; c < 4; ++c)
         info->in[i].slot[c] = (0x80 + n * 0x10 + c * 0x4) / 4;
      ++n;
   }

   return 0;
}

static int
nvc0_sp_assign_input_slots(struct nv50_ir_prog_info_out *info)
{
   unsigned offset;
   unsigned i, c;

   for (i = 0; i < info->numInputs; ++i) {
      offset = nvc0_shader_input_address(info->in[i].sn, info->in[i].si);

      for (c = 0; c < 4; ++c)
         info->in[i].slot[c] = (offset + c * 0x4) / 4;
   }

   return 0;
}

static int
nvc0_fp_assign_output_slots(struct nv50_ir_prog_info_out *info)
{
   unsigned count = info->prop.fp.numColourResults * 4;
   unsigned i, c;

   /* Compute the relative position of each color output, since skipped MRT
    * positions will not have registers allocated to them.
    */
   unsigned colors[8] = {0};
   for (i = 0; i < info->numOutputs; ++i)
      if (info->out[i].sn == TGSI_SEMANTIC_COLOR)
         colors[info->out[i].si] = 1;
   for (i = 0, c = 0; i < 8; i++)
      if (colors[i])
         colors[i] = c++;
   for (i = 0; i < info->numOutputs; ++i)
      if (info->out[i].sn == TGSI_SEMANTIC_COLOR)
         for (c = 0; c < 4; ++c)
            info->out[i].slot[c] = colors[info->out[i].si] * 4 + c;

   if (info->io.sampleMask < NV50_CODEGEN_MAX_VARYINGS)
      info->out[info->io.sampleMask].slot[0] = count++;
   else
   if (info->target >= 0xe0)
      count++; /* on Kepler, depth is always last colour reg + 2 */

   if (info->io.fragDepth < NV50_CODEGEN_MAX_VARYINGS)
      info->out[info->io.fragDepth].slot[2] = count;

   return 0;
}

static int
nvc0_sp_assign_output_slots(struct nv50_ir_prog_info_out *info)
{
   unsigned offset;
   unsigned i, c;

   for (i = 0; i < info->numOutputs; ++i) {
      offset = nvc0_shader_output_address(info->out[i].sn, info->out[i].si);

      for (c = 0; c < 4; ++c)
         info->out[i].slot[c] = (offset + c * 0x4) / 4;
   }

   return 0;
}

static int
nvc0_program_assign_varying_slots(struct nv50_ir_prog_info_out *info)
{
   int ret;

   if (info->type == PIPE_SHADER_VERTEX)
      ret = nvc0_vp_assign_input_slots(info);
   else
      ret = nvc0_sp_assign_input_slots(info);
   if (ret)
      return ret;

   if (info->type == PIPE_SHADER_FRAGMENT)
      ret = nvc0_fp_assign_output_slots(info);
   else
      ret = nvc0_sp_assign_output_slots(info);
   return ret;
}

static inline void
nvk_vtgs_hdr_update_oread(struct nvk_shader *vs, uint8_t slot)
{
   uint8_t min = (vs->hdr[4] >> 12) & 0xff;
   uint8_t max = (vs->hdr[4] >> 24);

   min = MIN2(min, slot);
   max = MAX2(max, slot);

   vs->hdr[4] = (max << 24) | (min << 12);
}

static int
nvk_vtgp_gen_header(struct nvk_shader *vs, struct nv50_ir_prog_info_out *info)
{
   unsigned i, c, a;

   for (i = 0; i < info->numInputs; ++i) {
      if (info->in[i].patch)
         continue;
      for (c = 0; c < 4; ++c) {
         a = info->in[i].slot[c];
         if (info->in[i].mask & (1 << c))
            vs->hdr[5 + a / 32] |= 1 << (a % 32);
      }
   }

   for (i = 0; i < info->numOutputs; ++i) {
      if (info->out[i].patch)
         continue;
      for (c = 0; c < 4; ++c) {
         if (!(info->out[i].mask & (1 << c)))
            continue;
         assert(info->out[i].slot[c] >= 0x40 / 4);
         a = info->out[i].slot[c] - 0x40 / 4;
         vs->hdr[13 + a / 32] |= 1 << (a % 32);
         if (info->out[i].oread)
            nvk_vtgs_hdr_update_oread(vs, info->out[i].slot[c]);
      }
   }

   for (i = 0; i < info->numSysVals; ++i) {
      switch (info->sv[i].sn) {
      case TGSI_SEMANTIC_PRIMID:
         vs->hdr[5] |= 1 << 24;
         break;
      case TGSI_SEMANTIC_INSTANCEID:
         vs->hdr[10] |= 1 << 30;
         break;
      case TGSI_SEMANTIC_VERTEXID:
         vs->hdr[10] |= 1 << 31;
         break;
      case TGSI_SEMANTIC_TESSCOORD:
         /* We don't have the mask, nor the slots populated. While this could
          * be achieved, the vast majority of the time if either of the coords
          * are read, then both will be read.
          */
         nvk_vtgs_hdr_update_oread(vs, 0x2f0 / 4);
         nvk_vtgs_hdr_update_oread(vs, 0x2f4 / 4);
         break;
      default:
         break;
      }
   }

   vs->vs.clip_enable = (1 << info->io.clipDistances) - 1;
   vs->vs.cull_enable =
      ((1 << info->io.cullDistances) - 1) << info->io.clipDistances;
   for (i = 0; i < info->io.cullDistances; ++i)
      vs->vs.clip_mode |= 1 << ((info->io.clipDistances + i) * 4);

   if (info->io.genUserClip < 0)
      vs->vs.num_ucps = 8 + 1; /* prevent rebuilding */

   vs->vs.layer_viewport_relative = info->io.layer_viewport_relative;

   return 0;
}

static int
nvk_vs_gen_header(struct nvk_shader *vs, struct nv50_ir_prog_info_out *info)
{
   vs->hdr[0] = 0x20061 | (1 << 10);
   vs->hdr[4] = 0xff000;

   return nvk_vtgp_gen_header(vs, info);
}

#define NVC0_INTERP_FLAT          (1 << 0)
#define NVC0_INTERP_PERSPECTIVE   (2 << 0)
#define NVC0_INTERP_LINEAR        (3 << 0)
#define NVC0_INTERP_CENTROID      (1 << 2)

static uint8_t
nvk_hdr_interp_mode(const struct nv50_ir_varying *var)
{
   if (var->linear)
      return NVC0_INTERP_LINEAR;
   if (var->flat)
      return NVC0_INTERP_FLAT;
   return NVC0_INTERP_PERSPECTIVE;
}


static int
nvk_fs_gen_header(struct nvk_shader *fs, struct nv50_ir_prog_info_out *info)
{
   unsigned i, c, a, m;

   /* just 00062 on Kepler */
   fs->hdr[0] = 0x20062 | (5 << 10);
   fs->hdr[5] = 0x80000000; /* getting a trap if FRAG_COORD_UMASK.w = 0 */

   if (info->prop.fp.usesDiscard)
      fs->hdr[0] |= 0x8000;
   if (!info->prop.fp.separateFragData)
      fs->hdr[0] |= 0x4000;
   if (info->io.sampleMask < 80 /* PIPE_MAX_SHADER_OUTPUTS */)
      fs->hdr[19] |= 0x1;
   if (info->prop.fp.writesDepth) {
      fs->hdr[19] |= 0x2;
      fs->flags[0] = 0x11; /* deactivate ZCULL */
   }

   for (i = 0; i < info->numInputs; ++i) {
      m = nvk_hdr_interp_mode(&info->in[i]);
      if (info->in[i].sn == TGSI_SEMANTIC_COLOR) {
         fs->fs.colors |= 1 << info->in[i].si;
         if (info->in[i].sc)
            fs->fs.color_interp[info->in[i].si] = m | (info->in[i].mask << 4);
      }
      for (c = 0; c < 4; ++c) {
         if (!(info->in[i].mask & (1 << c)))
            continue;
         a = info->in[i].slot[c];
         if (info->in[i].slot[0] >= (0x060 / 4) &&
             info->in[i].slot[0] <= (0x07c / 4)) {
            fs->hdr[5] |= 1 << (24 + (a - 0x060 / 4));
         } else
         if (info->in[i].slot[0] >= (0x2c0 / 4) &&
             info->in[i].slot[0] <= (0x2fc / 4)) {
            fs->hdr[14] |= (1 << (a - 0x280 / 4)) & 0x07ff0000;
         } else {
            if (info->in[i].slot[c] < (0x040 / 4) ||
                info->in[i].slot[c] > (0x380 / 4))
               continue;
            a *= 2;
            if (info->in[i].slot[0] >= (0x300 / 4))
               a -= 32;
            fs->hdr[4 + a / 32] |= m << (a % 32);
         }
      }
   }
   /* GM20x+ needs TGSI_SEMANTIC_POSITION to access sample locations */
   if (info->prop.fp.readsSampleLocations && info->target >= NVISA_GM200_CHIPSET)
      fs->hdr[5] |= 0x30000000;

   for (i = 0; i < info->numOutputs; ++i) {
      if (info->out[i].sn == TGSI_SEMANTIC_COLOR)
         fs->hdr[18] |= 0xf << (4 * info->out[i].si);
   }

   /* There are no "regular" attachments, but the shader still needs to be
    * executed. It seems like it wants to think that it has some color
    * outputs in order to actually run.
    */
   if (info->prop.fp.numColourResults == 0 && !info->prop.fp.writesDepth)
      fs->hdr[18] |= 0xf;

   fs->fs.early_z = info->prop.fp.earlyFragTests;
   fs->fs.sample_mask_in = info->prop.fp.usesSampleMaskIn;
   fs->fs.reads_framebuffer = info->prop.fp.readsFramebuffer;
   fs->fs.post_depth_coverage = info->prop.fp.postDepthCoverage;

   /* Mark position xy and layer as read */
   if (fs->fs.reads_framebuffer)
      fs->hdr[5] |= 0x32000000;

   return 0;
}

VkResult
nvk_compile_nir(struct nvk_physical_device *device, nir_shader *nir,
                struct nvk_shader *shader)
{
   struct nv50_ir_prog_info *info;
   struct nv50_ir_prog_info_out info_out = {};
   int ret;

   info = CALLOC_STRUCT(nv50_ir_prog_info);
   if (!info)
      return false;

   info->type = pipe_shader_type_from_mesa(nir->info.stage);
   info->target = device->dev->chipset;
   info->bin.nir = nir;

   for (unsigned i = 0; i < 3; i++)
      shader->cp.block_size[i] = nir->info.workgroup_size[i];

   info->bin.smemSize = shader->cp.smem_size;
   info->dbgFlags = debug_get_num_option("NV50_PROG_DEBUG", 0);
   info->optLevel = debug_get_num_option("NV50_PROG_OPTIMIZE", 3);
   info->io.auxCBSlot = 15;
   info->io.uboInfoBase = 0;
   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      info->io.auxCBSlot = 1;
      info->prop.cp.gridInfoBase = 0;
   } else {
      info->assignSlots = nvc0_program_assign_varying_slots;
   }
   ret = nv50_ir_generate_code(info, &info_out);
   if (ret)
      return VK_ERROR_UNKNOWN;

   shader->stage = nir->info.stage;
   shader->code_ptr = (uint8_t *)info_out.bin.code;
   shader->code_size = info_out.bin.codeSize;

   if (info_out.target >= NVISA_GV100_CHIPSET)
      shader->num_gprs = MIN2(info_out.bin.maxGPR + 5, 256); //XXX: why?
   else
      shader->num_gprs = MAX2(4, (info_out.bin.maxGPR + 1));
   shader->cp.smem_size = info_out.bin.smemSize;
   shader->num_barriers = info_out.numBarriers;

   switch (info->type) {
   case PIPE_SHADER_VERTEX:
      ret = nvk_vs_gen_header(shader, &info_out);
      break;
   case PIPE_SHADER_FRAGMENT:
      ret = nvk_fs_gen_header(shader, &info_out);
      break;
   case PIPE_SHADER_COMPUTE:
      break;
   default:
      unreachable("Invalid shader stage");
      break;
   }
   assert(ret == 0);

   if (info_out.bin.tlsSpace) {
      assert(info_out.bin.tlsSpace < (1 << 24));
      shader->hdr[0] |= 1 << 26;
      shader->hdr[1] |= align(info_out.bin.tlsSpace, 0x10); /* l[] size */
      shader->need_tls = true;
   }

   if (info_out.io.globalAccess)
      shader->hdr[0] |= 1 << 26;
   if (info_out.io.globalAccess & 0x2)
      shader->hdr[0] |= 1 << 16;
   if (info_out.io.fp64)
      shader->hdr[0] |= 1 << 27;

   return VK_SUCCESS;
}

void
nvk_shader_upload(struct nvk_device *dev, struct nvk_shader *shader)
{
   uint32_t hdr_size = 0;
   if (shader->stage != MESA_SHADER_COMPUTE) {
      if (dev->ctx->eng3d.cls >= 0xc597)
         hdr_size = TU102_SHADER_HEADER_SIZE;
      else
         hdr_size = GF100_SHADER_HEADER_SIZE;
   }

   /* TODO: The I-cache pre-fetches and we don't really know by how much.  So
    * throw on a bunch just to be sure.
    */
   uint32_t total_size = shader->code_size + hdr_size;
   shader->bo = nouveau_ws_bo_new(nvk_device_physical(dev)->dev,
                                  total_size + 4096, 256,
                                  NOUVEAU_WS_BO_LOCAL | NOUVEAU_WS_BO_MAP);

   void *ptr = nouveau_ws_bo_map(shader->bo, NOUVEAU_WS_BO_WR);

   assert(hdr_size <= sizeof(shader->hdr));
   memcpy(ptr, shader->hdr, hdr_size);
   memcpy(ptr + hdr_size, shader->code_ptr, shader->code_size);

#ifndef NDEBUG
   if (debug_get_bool_option("NV50_PROG_DEBUG", false))
      nvk_shader_dump(shader);
#endif
}