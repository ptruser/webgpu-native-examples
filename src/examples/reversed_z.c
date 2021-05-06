﻿#include "example_base.h"
#include "examples.h"

#include <string.h>

#include "../webgpu/imgui_overlay.h"

/* -------------------------------------------------------------------------- *
 * WebGPU Example - Reversed Z
 *
 * This example shows the use of reversed z technique for better utilization of
 * depth buffer precision. The left column uses regular method, while the right
 * one uses reversed z technique. Both are using depth32float as their depth
 * buffer format. A set of red and green planes are positioned very close to
 * each other. Higher sets are placed further from camera (and are scaled for
 * better visual purpose). To use reversed z to render your scene, you will need
 * depth store value to be 0.0, depth compare function to be greater, and remap
 * depth range by multiplying an additional matrix to your projection matrix.
 *
 * Related reading:
 * https://developer.nvidia.com/content/depth-precision-visualized
 * https://thxforthefish.com/posts/reverse_z/
 *
 * Ref:
 * https://github.com/austinEng/webgpu-samples/blob/main/src/pages/samples/reversedZ.ts
 * -------------------------------------------------------------------------- */

#define DEFAULT_CANVAS_WIDTH 600
#define DEFAULT_CANVAS_HEIGHT 600

#define X_COUNT 1
#define Y_COUNT 5
#define NUM_INSTANCES (X_COUNT * Y_COUNT)
#define MATRIX_FLOAT_COUNT sizeof(mat4)

// Two planes close to each other for depth precision test
static const uint32_t geometry_vertex_size
  = 4 * 8; // Byte size of one geometry vertex.
static const uint32_t geometry_position_offset = 0;
static const uint32_t geometry_color_offset
  = 4 * 4; // Byte offset of geometry vertex color attribute.
static const uint32_t geometry_draw_count = 6 * 2;

static const float d = 0.0001f; // half distance between two planes
static const float o
  = 0.5f; // half x offset to shift planes so they are only partially overlaping

static const uint32_t default_canvas_width  = (uint32_t)DEFAULT_CANVAS_WIDTH;
static const uint32_t default_canvas_height = (uint32_t)DEFAULT_CANVAS_HEIGHT;

static const uint32_t viewport_width = default_canvas_width / 2;

const uint32_t x_count            = (uint32_t)X_COUNT;
const uint32_t y_count            = (uint32_t)Y_COUNT;
const uint32_t num_instances      = (uint32_t)NUM_INSTANCES;
const uint32_t matrix_float_count = (uint32_t)MATRIX_FLOAT_COUNT; // 4x4 matrix
const uint32_t matrix_stride      = 4 * matrix_float_count;

static mat4 model_matrices[NUM_INSTANCES]                          = {0};
static float mvp_matrices_data[NUM_INSTANCES * MATRIX_FLOAT_COUNT] = {0};
static mat4 depth_range_remap_matrix                               = {
  {1.0f, 0.0f, 0.0f, 0.0f},  //
  {0.0f, 1.0f, 0.0f, 0.0f},  //
  {0.0f, 0.0f, -1.0f, 0.0f}, //
  {0.0f, 0.0f, 1.0f, 1.0f},  //
};
static mat4 tmp_mat4 = GLM_MAT4_IDENTITY_INIT;

static const WGPUTextureFormat depth_buffer_format
  = WGPUTextureFormat_Depth32Float;

// Vertex buffer and attributes
static struct vertices_t {
  WGPUBuffer buffer;
  uint32_t count;
} vertices = {0};

static WGPURenderPipeline depth_pre_pass_pipelines[2] = {0};
static WGPURenderPipeline precision_pass_pipelines[2] = {0};
static WGPURenderPipeline color_pass_pipelines[2]     = {0};
static WGPURenderPipeline texture_quad_pass_pipeline;

static texture_t depth_texture;
static texture_t default_depth_texture;

static WGPURenderPassDescriptor depth_pre_pass_descriptor = {0};
static WGPURenderPassDepthStencilAttachmentDescriptor dppd_rp_ds_att_descriptor;

static WGPURenderPassColorAttachmentDescriptor dpd_rp_color_att_descriptors[2]
                                                                           [1];
static WGPURenderPassDepthStencilAttachmentDescriptor
  dpd_rp_ds_att_descriptors[2];
static WGPURenderPassDescriptor draw_pass_descriptors[2] = {0};

static WGPURenderPassColorAttachmentDescriptor tqd_rp_color_att_descriptors[2]
                                                                           [1];
static WGPURenderPassDescriptor texture_quad_pass_descriptors[2] = {0};

static WGPUBindGroupLayout depth_texture_bind_group_layout;
static WGPUBindGroup depth_texture_bind_group;

static WGPUBuffer uniform_buffer;
static WGPUBuffer camera_matrix_buffer;
static WGPUBuffer camera_matrix_reversed_depth_buffer;

static uint32_t uniform_buffer_size = num_instances * matrix_stride;

static WGPUBindGroup uniform_bind_groups[2] = {0};

// Other variables
static const char* example_title = "Reversed Z";
static bool prepared             = false;

// https://github.com/toji/gl-matrix/commit/e906eb7bb02822a81b1d197c6b5b33563c0403c0
static float* perspective_zo(float (*out)[16], float fovy, float aspect,
                             float near, float* far)
{
  const float f = 1.0f / tan(fovy / 2.0f);
  (*out)[0]     = f / aspect;
  (*out)[1]     = 0.0f;
  (*out)[2]     = 0.0f;
  (*out)[3]     = 0.0f;
  (*out)[4]     = 0.0f;
  (*out)[5]     = f;
  (*out)[6]     = 0.0f;
  (*out)[7]     = 0.0f;
  (*out)[8]     = 0.0f;
  (*out)[9]     = 0.0f;
  (*out)[11]    = -1.0f;
  (*out)[12]    = 0.0f;
  (*out)[13]    = 0.0f;
  (*out)[15]    = 0.0f;
  if (far != NULL && *far != INFINITY) {
    const float nf = 1.0f / (near - *far);
    (*out)[10]     = *far * nf;
    (*out)[14]     = *far * near * nf;
  }
  else {
    (*out)[10] = -1.0f;
    (*out)[14] = -near;
  }
  return *out;
}

static void float_array_to_mat4(float (*float_array)[16], mat4* out)
{
  uint32_t i = 0;
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      (*out)[r][c] = (*float_array)[i++];
    }
  }
}

typedef enum render_mode_enum {
  RenderMode_Color              = 0,
  RenderMode_Precision_Error    = 1,
  RenderMode_Depth_Texture_Quad = 2,
} render_mode_enum;

static render_mode_enum current_render_mode = RenderMode_Color;

typedef enum depth_buffer_mode_enum {
  DepthBufferMode_Default  = 0,
  DepthBufferMode_Reversed = 1,
} depth_buffer_mode_enum;

static const depth_buffer_mode_enum depth_buffer_modes[2] = {
  DepthBufferMode_Default,  // Default
  DepthBufferMode_Reversed, // Reversed
};

static const WGPUCompareFunction depth_compare_funcs[2] = {
  WGPUCompareFunction_Less,    // Default
  WGPUCompareFunction_Greater, // Reversed
};

static const float depth_load_values[2] = {
  1.0f, // Default
  0.0f, // Reversed
};

static void prepare_vertex_buffer(wgpu_context_t* wgpu_context)
{
  static const float geometry_vertex_array[(4 + 4) * 6 * 2] = {
    // float4 position, float4 color
    -1 - o, -1, d,  1, 1, 0, 0, 1, //
    1 - o,  -1, d,  1, 1, 0, 0, 1, //
    -1 - o, 1,  d,  1, 1, 0, 0, 1, //
    1 - o,  -1, d,  1, 1, 0, 0, 1, //
    1 - o,  1,  d,  1, 1, 0, 0, 1, //
    -1 - o, 1,  d,  1, 1, 0, 0, 1, //

    -1 + o, -1, -d, 1, 0, 1, 0, 1, //
    1 + o,  -1, -d, 1, 0, 1, 0, 1, //
    -1 + o, 1,  -d, 1, 0, 1, 0, 1, //
    1 + o,  -1, -d, 1, 0, 1, 0, 1, //
    1 + o,  1,  -d, 1, 0, 1, 0, 1, //
    -1 + o, 1,  -d, 1, 0, 1, 0, 1, //
  };
  vertices.count              = (uint32_t)ARRAY_SIZE(geometry_vertex_array);
  uint32_t vertex_buffer_size = vertices.count * sizeof(float);

  // Create vertex buffer
  vertices.buffer
    = wgpu_create_buffer_from_data(wgpu_context, geometry_vertex_array,
                                   vertex_buffer_size, WGPUBufferUsage_Vertex);
}

// depthPrePass is used to render scene to the depth texture
// this is not needed if you just want to use reversed z to render a scene
static void prepare_depth_pre_pass_render_pipeline(wgpu_context_t* wgpu_context)
{
  // Rasterization state
  WGPURasterizationStateDescriptor rasterization_state
    = wgpu_create_rasterization_state_descriptor(
      &(create_rasterization_state_desc_t){
        .front_face = WGPUFrontFace_CCW,
        .cull_mode  = WGPUCullMode_Back,
      });

  // Depth and stencil state containing depth and stencil compare and test
  // operations
  WGPUDepthStencilStateDescriptor depth_stencil_state_desc
    = wgpu_create_depth_stencil_state_descriptor(
      &(create_depth_stencil_state_desc_t){
        .format              = depth_buffer_format,
        .depth_write_enabled = true,
      });
  depth_stencil_state_desc.depthCompare = WGPUCompareFunction_Less;

  // Vertex input binding (=> Input assembly) description
  WGPU_VERTSTATE(
    depth_pre_pass, geometry_vertex_size,
    /* Attribute descriptions */
    // Attribute location 0: Position
    WGPU_VERTATTR_DESC(0, WGPUVertexFormat_Float32x4, geometry_position_offset))

  // Shaders
  // Vertex shader
  wgpu_shader_t vert_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Vertex shader SPIR-V
                    .file = "shaders/reversed_z/depth_pre_pass.vert.spv",
                  });
  // Fragment shader
  wgpu_shader_t frag_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Fragment shader SPIR-V
                    .file = "shaders/reversed_z/depth_pre_pass.frag.spv",
                  });

  // depthPrePass is used to render scene to the depth texture
  // this is not needed if you just want to use reversed z to render a scene
  WGPURenderPipelineDescriptor depth_pre_pass_render_pipeline_descriptor_base
    = (WGPURenderPipelineDescriptor){
      // Vertex shader
      .vertexStage = vert_shader.programmable_stage_descriptor,
      // Fragment shader
      .fragmentStage = &frag_shader.programmable_stage_descriptor,
      // Rasterization state
      .rasterizationState     = &rasterization_state,
      .primitiveTopology      = WGPUPrimitiveTopology_TriangleList,
      .depthStencilState      = &depth_stencil_state_desc,
      .vertexState            = &vert_state_depth_pre_pass,
      .sampleCount            = 1,
      .sampleMask             = 0xFFFFFFFF,
      .alphaToCoverageEnabled = false,
    };

  // we need the depthCompare to fit the depth buffer mode we are using.
  // this is the same for other passes
  /* Default */
  depth_stencil_state_desc.depthCompare
    = depth_compare_funcs[(uint32_t)DepthBufferMode_Default];
  depth_pre_pass_pipelines[(uint32_t)DepthBufferMode_Default]
    = wgpuDeviceCreateRenderPipeline(
      wgpu_context->device, &depth_pre_pass_render_pipeline_descriptor_base);
  /* Reversed */
  depth_stencil_state_desc.depthCompare
    = depth_compare_funcs[(uint32_t)DepthBufferMode_Reversed];
  depth_pre_pass_pipelines[(uint32_t)DepthBufferMode_Reversed]
    = wgpuDeviceCreateRenderPipeline(
      wgpu_context->device, &depth_pre_pass_render_pipeline_descriptor_base);

  // Shader modules are no longer needed once the graphics pipeline has been
  // created
  wgpu_shader_release(&frag_shader);
  wgpu_shader_release(&vert_shader);
}

// precisionPass is to draw precision error as color of depth value stored in
// depth buffer compared to that directly calcualated in the shader
static void prepare_precision_pass_render_pipeline(wgpu_context_t* wgpu_context)
{
  // Rasterization state
  WGPURasterizationStateDescriptor rasterization_state
    = wgpu_create_rasterization_state_descriptor(
      &(create_rasterization_state_desc_t){
        .front_face = WGPUFrontFace_CCW,
        .cull_mode  = WGPUCullMode_Back,
      });

  // Color blend state
  WGPUColorStateDescriptor color_state_desc
    = wgpu_create_color_state_descriptor(&(create_color_state_desc_t){
      .format       = wgpu_context->swap_chain.format,
      .enable_blend = true,
    });

  // Depth and stencil state containing depth and stencil compare and test
  // operations
  WGPUDepthStencilStateDescriptor depth_stencil_state_desc
    = wgpu_create_depth_stencil_state_descriptor(
      &(create_depth_stencil_state_desc_t){
        .format              = depth_buffer_format,
        .depth_write_enabled = true,
      });
  depth_stencil_state_desc.depthCompare = WGPUCompareFunction_Less;

  // Vertex input binding (=> Input assembly) description
  WGPU_VERTSTATE(
    precision_error_pass, geometry_vertex_size,
    /* Attribute descriptions */
    // Attribute location 0: Position
    WGPU_VERTATTR_DESC(0, WGPUVertexFormat_Float32x4, geometry_position_offset))

  // Shaders
  // Vertex shader
  wgpu_shader_t vert_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Vertex shader SPIR-V
                    .file = "shaders/reversed_z/precision_error_pass.vert.spv",
                  });
  // Fragment shader
  wgpu_shader_t frag_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Fragment shader SPIR-V
                    .file = "shaders/reversed_z/precision_error_pass.frag.spv",
                  });

  // precisionPass is to draw precision error as color of depth value stored in
  // depth buffer compared to that directly calcualated in the shader
  WGPURenderPipelineDescriptor precision_pass_render_pipeline_descriptor_base
    = (WGPURenderPipelineDescriptor){
      // Vertex shader
      .vertexStage = vert_shader.programmable_stage_descriptor,
      // Fragment shader
      .fragmentStage = &frag_shader.programmable_stage_descriptor,
      // Rasterization state
      .rasterizationState     = &rasterization_state,
      .primitiveTopology      = WGPUPrimitiveTopology_TriangleList,
      .colorStateCount        = 1,
      .colorStates            = &color_state_desc,
      .depthStencilState      = &depth_stencil_state_desc,
      .vertexState            = &vert_state_precision_error_pass,
      .sampleCount            = 1,
      .sampleMask             = 0xFFFFFFFF,
      .alphaToCoverageEnabled = false,
    };

  /* Default */
  depth_stencil_state_desc.depthCompare
    = depth_compare_funcs[(uint32_t)DepthBufferMode_Default];
  precision_pass_pipelines[(uint32_t)DepthBufferMode_Default]
    = wgpuDeviceCreateRenderPipeline(
      wgpu_context->device, &precision_pass_render_pipeline_descriptor_base);
  /* Reversed */
  depth_stencil_state_desc.depthCompare
    = depth_compare_funcs[(uint32_t)DepthBufferMode_Reversed];
  precision_pass_pipelines[(uint32_t)DepthBufferMode_Reversed]
    = wgpuDeviceCreateRenderPipeline(
      wgpu_context->device, &precision_pass_render_pipeline_descriptor_base);

  // Shader modules are no longer needed once the graphics pipeline has been
  // created
  wgpu_shader_release(&frag_shader);
  wgpu_shader_release(&vert_shader);
}

// colorPass is the regular render pass to render the scene
static void prepare_color_pass_render_pipeline(wgpu_context_t* wgpu_context)
{
  // Rasterization state
  WGPURasterizationStateDescriptor rasterization_state
    = wgpu_create_rasterization_state_descriptor(
      &(create_rasterization_state_desc_t){
        .front_face = WGPUFrontFace_CCW,
        .cull_mode  = WGPUCullMode_Back,
      });

  // Color blend state
  WGPUColorStateDescriptor color_state_desc
    = wgpu_create_color_state_descriptor(&(create_color_state_desc_t){
      .format       = wgpu_context->swap_chain.format,
      .enable_blend = true,
    });

  // Depth and stencil state containing depth and stencil compare and test
  // operations
  WGPUDepthStencilStateDescriptor depth_stencil_state_desc
    = wgpu_create_depth_stencil_state_descriptor(
      &(create_depth_stencil_state_desc_t){
        .format              = depth_buffer_format,
        .depth_write_enabled = true,
      });
  depth_stencil_state_desc.depthCompare = WGPUCompareFunction_Less;

  // Vertex input binding (=> Input assembly) description
  WGPU_VERTSTATE(
    color_pass, geometry_vertex_size,
    /* Attribute descriptions */
    // Attribute location 0: Position
    WGPU_VERTATTR_DESC(0, WGPUVertexFormat_Float32x4, geometry_position_offset),
    // Attribute location 1: Color
    WGPU_VERTATTR_DESC(1, WGPUVertexFormat_Float32x4, geometry_color_offset))

  // Shaders
  // Vertex shader
  wgpu_shader_t vert_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Vertex shader SPIR-V
                    .file = "shaders/reversed_z/color_pass.vert.spv",
                  });
  // Fragment shader
  wgpu_shader_t frag_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Fragment shader SPIR-V
                    .file = "shaders/reversed_z/color_pass.frag.spv",
                  });

  // colorPass is the regular render pass to render the scene
  WGPURenderPipelineDescriptor color_passRender_pipeline_descriptor_base
    = (WGPURenderPipelineDescriptor){
      // Vertex shader
      .vertexStage = vert_shader.programmable_stage_descriptor,
      // Fragment shader
      .fragmentStage = &frag_shader.programmable_stage_descriptor,
      // Rasterization state
      .rasterizationState     = &rasterization_state,
      .primitiveTopology      = WGPUPrimitiveTopology_TriangleList,
      .colorStateCount        = 1,
      .colorStates            = &color_state_desc,
      .depthStencilState      = &depth_stencil_state_desc,
      .vertexState            = &vert_state_color_pass,
      .sampleCount            = 1,
      .sampleMask             = 0xFFFFFFFF,
      .alphaToCoverageEnabled = false,
    };

  /* Default */
  depth_stencil_state_desc.depthCompare
    = depth_compare_funcs[(uint32_t)DepthBufferMode_Default];
  color_pass_pipelines[(uint32_t)DepthBufferMode_Default]
    = wgpuDeviceCreateRenderPipeline(
      wgpu_context->device, &color_passRender_pipeline_descriptor_base);
  /* Reversed */
  depth_stencil_state_desc.depthCompare
    = depth_compare_funcs[(uint32_t)DepthBufferMode_Reversed];
  color_pass_pipelines[(uint32_t)DepthBufferMode_Reversed]
    = wgpuDeviceCreateRenderPipeline(
      wgpu_context->device, &color_passRender_pipeline_descriptor_base);

  // Shader modules are no longer needed once the graphics pipeline has been
  // created
  wgpu_shader_release(&frag_shader);
  wgpu_shader_release(&vert_shader);
}

// textureQuadPass is draw a full screen quad of depth texture
// to see the difference of depth value using reversed z compared to default
// depth buffer usage 0.0 will be the furthest and 1.0 will be the closest
static void
prepare_texture_quad_pass_render_pipeline(wgpu_context_t* wgpu_context)
{
  // Rasterization state
  WGPURasterizationStateDescriptor rasterization_state
    = wgpu_create_rasterization_state_descriptor(
      &(create_rasterization_state_desc_t){
        .front_face = WGPUFrontFace_CCW,
        .cull_mode  = WGPUCullMode_Back,
      });

  // Color blend state
  WGPUColorStateDescriptor color_state_desc
    = wgpu_create_color_state_descriptor(&(create_color_state_desc_t){
      .format       = wgpu_context->swap_chain.format,
      .enable_blend = true,
    });

  // Shaders
  // Vertex shader
  wgpu_shader_t vert_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Vertex shader SPIR-V
                    .file = "shaders/reversed_z/texture_quad.vert.spv",
                  });
  // Fragment shader
  wgpu_shader_t frag_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Fragment shader SPIR-V
                    .file = "shaders/reversed_z/texture_quad.frag.spv",
                  });

  // textureQuadPass is draw a full screen quad of depth texture
  // to see the difference of depth value using reversed z compared to default
  // depth buffer usage 0.0 will be the furthest and 1.0 will be the closest
  texture_quad_pass_pipeline = wgpuDeviceCreateRenderPipeline(
    wgpu_context->device,
    &(WGPURenderPipelineDescriptor){
      // Vertex shader
      .vertexStage = vert_shader.programmable_stage_descriptor,
      // Fragment shader
      .fragmentStage = &frag_shader.programmable_stage_descriptor,
      // Rasterization state
      .rasterizationState     = &rasterization_state,
      .primitiveTopology      = WGPUPrimitiveTopology_TriangleList,
      .colorStateCount        = 1,
      .colorStates            = &color_state_desc,
      .sampleCount            = 1,
      .sampleMask             = 0xFFFFFFFF,
      .alphaToCoverageEnabled = false,
    });

  // Shader modules are no longer needed once the graphics pipeline has been
  // created
  wgpu_shader_release(&frag_shader);
  wgpu_shader_release(&vert_shader);
}

static void prepare_depth_textures(wgpu_context_t* wgpu_context)
{
  // Create the depth texture.
  {
    WGPUTextureDescriptor texture_desc = {
      .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_Sampled,
      .dimension     = WGPUTextureDimension_2D,
      .format        = depth_buffer_format,
      .mipLevelCount = 1,
      .sampleCount   = 1,
      .size          = (WGPUExtent3D)  {
        .width               = wgpu_context->surface.width,
        .height              = wgpu_context->surface.height,
        .depth               = 1,
        .depthOrArrayLayers  = 1,
      },
    };
    depth_texture.texture
      = wgpuDeviceCreateTexture(wgpu_context->device, &texture_desc);

    // Create the texture view
    WGPUTextureViewDescriptor texture_view_dec = {
      .dimension       = WGPUTextureViewDimension_2D,
      .format          = depth_buffer_format,
      .baseMipLevel    = 0,
      .mipLevelCount   = 1,
      .baseArrayLayer  = 0,
      .arrayLayerCount = 1,
    };
    depth_texture.view
      = wgpuTextureCreateView(depth_texture.texture, &texture_view_dec);

    // Create the sampler
    WGPUSamplerDescriptor sampler_desc = {
      .addressModeU  = WGPUAddressMode_ClampToEdge,
      .addressModeV  = WGPUAddressMode_ClampToEdge,
      .addressModeW  = WGPUAddressMode_ClampToEdge,
      .minFilter     = WGPUFilterMode_Linear,
      .magFilter     = WGPUFilterMode_Linear,
      .mipmapFilter  = WGPUFilterMode_Nearest,
      .lodMinClamp   = 0.0f,
      .lodMaxClamp   = 1.0f,
      .maxAnisotropy = 1,
    };
    depth_texture.sampler
      = wgpuDeviceCreateSampler(wgpu_context->device, &sampler_desc);
  }

  // Create the default depth texture.
  {
    WGPUTextureDescriptor texture_desc = {
      .usage         = WGPUTextureUsage_RenderAttachment,
      .dimension     = WGPUTextureDimension_2D,
      .format        = depth_buffer_format,
      .mipLevelCount = 1,
      .sampleCount   = 1,
      .size          = (WGPUExtent3D)  {
        .width               = wgpu_context->surface.width,
        .height              = wgpu_context->surface.height,
        .depth               = 1,
        .depthOrArrayLayers  = 1,
      },
    };
    default_depth_texture.texture
      = wgpuDeviceCreateTexture(wgpu_context->device, &texture_desc);

    // Create the texture view
    WGPUTextureViewDescriptor texture_view_dec = {
      .dimension       = WGPUTextureViewDimension_2D,
      .format          = depth_buffer_format,
      .baseMipLevel    = 0,
      .mipLevelCount   = 1,
      .baseArrayLayer  = 0,
      .arrayLayerCount = 1,
    };
    default_depth_texture.view
      = wgpuTextureCreateView(default_depth_texture.texture, &texture_view_dec);

    // Create the sampler
    WGPUSamplerDescriptor sampler_desc = {
      .addressModeU  = WGPUAddressMode_ClampToEdge,
      .addressModeV  = WGPUAddressMode_ClampToEdge,
      .addressModeW  = WGPUAddressMode_ClampToEdge,
      .minFilter     = WGPUFilterMode_Linear,
      .magFilter     = WGPUFilterMode_Linear,
      .mipmapFilter  = WGPUFilterMode_Nearest,
      .lodMinClamp   = 0.0f,
      .lodMaxClamp   = 1.0f,
      .maxAnisotropy = 1,
    };
    default_depth_texture.sampler
      = wgpuDeviceCreateSampler(wgpu_context->device, &sampler_desc);
  }
}

static void prepare_depth_pre_pass_descriptor()
{
  dppd_rp_ds_att_descriptor = (WGPURenderPassDepthStencilAttachmentDescriptor){
    .view           = depth_texture.view,
    .depthLoadOp    = WGPULoadOp_Clear,
    .depthStoreOp   = WGPUStoreOp_Store,
    .clearDepth     = 1.0f,
    .stencilLoadOp  = WGPULoadOp_Clear,
    .stencilStoreOp = WGPUStoreOp_Store,
    .clearStencil   = 0,
  };

  depth_pre_pass_descriptor = (WGPURenderPassDescriptor){
    .colorAttachmentCount   = 0,
    .colorAttachments       = NULL,
    .depthStencilAttachment = &dppd_rp_ds_att_descriptor,
  };
}

// drawPassDescriptor and drawPassLoadDescriptor are used for drawing
// the scene twice using different depth buffer mode on splitted viewport
// of the same canvas
// see the difference of the loadValue of the colorAttachments
static void prepare_draw_pass_descriptors()
{
  // drawPassDescriptor
  {
    // Color attachment
    dpd_rp_color_att_descriptors[0][0] = (WGPURenderPassColorAttachmentDescriptor) {
      .view       = NULL, // attachment is acquired and set in render loop.
      .attachment = NULL,
      .loadOp     = WGPULoadOp_Clear,
      .storeOp    = WGPUStoreOp_Store,
      .clearColor = (WGPUColor) {
        .r = 0.0f,
        .g = 0.0f,
        .b = 0.5f,
        .a = 1.0f,
      },
    };

    dpd_rp_ds_att_descriptors[0]
      = (WGPURenderPassDepthStencilAttachmentDescriptor){
        .view           = default_depth_texture.view,
        .depthLoadOp    = WGPULoadOp_Clear,
        .depthStoreOp   = WGPUStoreOp_Store,
        .clearDepth     = 1.0f,
        .stencilLoadOp  = WGPULoadOp_Clear,
        .stencilStoreOp = WGPUStoreOp_Store,
        .clearStencil   = 0,
      };

    draw_pass_descriptors[0] = (WGPURenderPassDescriptor){
      .colorAttachmentCount   = 1,
      .colorAttachments       = dpd_rp_color_att_descriptors[0],
      .depthStencilAttachment = &dpd_rp_ds_att_descriptors[0],
    };
  }

  // drawPassLoadDescriptor
  {
    dpd_rp_color_att_descriptors[1][0]
      = (WGPURenderPassColorAttachmentDescriptor){
        .view   = NULL, // attachment is acquired and set in render loop.
        .loadOp = WGPULoadOp_Load,
      };

    dpd_rp_ds_att_descriptors[1]
      = (WGPURenderPassDepthStencilAttachmentDescriptor){
        .view           = default_depth_texture.view,
        .depthLoadOp    = WGPULoadOp_Load,
        .depthStoreOp   = WGPUStoreOp_Store,
        .clearDepth     = 1.0f,
        .stencilLoadOp  = WGPULoadOp_Clear,
        .stencilStoreOp = WGPUStoreOp_Store,
        .clearStencil   = 0,
      };

    draw_pass_descriptors[1] = (WGPURenderPassDescriptor){
      .colorAttachmentCount   = 1,
      .colorAttachments       = dpd_rp_color_att_descriptors[1],
      .depthStencilAttachment = &dpd_rp_ds_att_descriptors[1],
    };
  }
}

static void prepare_texture_quad_pass_descriptors()
{
  // textureQuadPassDescriptor
  {
    tqd_rp_color_att_descriptors[0][0]
     = (WGPURenderPassColorAttachmentDescriptor) {
      .view       = NULL, // attachment is acquired and set in render loop.
      .attachment = NULL,
      .loadOp     = WGPULoadOp_Clear,
      .storeOp    = WGPUStoreOp_Store,
      .clearColor = (WGPUColor) {
        .r = 0.0f,
        .g = 0.0f,
        .b = 0.5f,
        .a = 1.0f,
      },
    };

    texture_quad_pass_descriptors[0] = (WGPURenderPassDescriptor){
      .colorAttachmentCount = 1,
      .colorAttachments     = tqd_rp_color_att_descriptors[0],
    };
  }

  // textureQuadPassLoadDescriptor
  {
    tqd_rp_color_att_descriptors[1][0]
      = (WGPURenderPassColorAttachmentDescriptor){
        .view   = NULL, // attachment is acquired and set in render loop.
        .loadOp = WGPULoadOp_Load,
      };

    texture_quad_pass_descriptors[1] = (WGPURenderPassDescriptor){
      .colorAttachmentCount = 1,
      .colorAttachments     = tqd_rp_color_att_descriptors[1],
    };
  }
}

static void
prepare_depth_texture_bind_group_layout(wgpu_context_t* wgpu_context)
{
  WGPUBindGroupLayoutEntry bgl_entries[2] = {
    [0] = (WGPUBindGroupLayoutEntry) {
      // Texture view
      .binding = 0,
      .visibility = WGPUShaderStage_Fragment,
      .texture = (WGPUTextureBindingLayout) {
        .sampleType = WGPUTextureSampleType_Float,
        .viewDimension = WGPUTextureViewDimension_2D,
        .multisampled = false,
      },
      .storageTexture = {0},
    },
    [1] = (WGPUBindGroupLayoutEntry) {
      // Sampler
      .binding = 1,
      .visibility = WGPUShaderStage_Fragment,
      .sampler = (WGPUSamplerBindingLayout){
        .type=WGPUSamplerBindingType_Filtering,
      },
      .texture = {0},
    }
  };
  WGPUBindGroupLayoutDescriptor bgl_desc = {
    .entryCount = (uint32_t)ARRAY_SIZE(bgl_entries),
    .entries    = bgl_entries,
  };
  depth_texture_bind_group_layout
    = wgpuDeviceCreateBindGroupLayout(wgpu_context->device, &bgl_desc);
  ASSERT(depth_texture_bind_group_layout != NULL)
}

static void prepare_depth_texture_bind_group(wgpu_context_t* wgpu_context)
{
  WGPUBindGroupEntry bg_entries[2] = {
    [0] = (WGPUBindGroupEntry) {
      .binding = 0,
      .textureView = depth_texture.view,
    },
    [1] = (WGPUBindGroupEntry) {
      .binding = 1,
      .sampler = depth_texture.sampler,
    }
  };
  WGPUBindGroupDescriptor bg_desc = {
    .layout     = depth_texture_bind_group_layout,
    .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
    .entries    = bg_entries,
  };
  depth_texture_bind_group
    = wgpuDeviceCreateBindGroup(wgpu_context->device, &bg_desc);
  ASSERT(depth_texture_bind_group != NULL)
}

static void prepare_uniform_buffers(wgpu_context_t* wgpu_context)
{
  uniform_buffer = wgpuDeviceCreateBuffer(
    wgpu_context->device,
    &(WGPUBufferDescriptor){
      .size  = uniform_buffer_size,
      .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
    });

  camera_matrix_buffer = wgpuDeviceCreateBuffer(
    wgpu_context->device,
    &(WGPUBufferDescriptor){
      .size  = sizeof(mat4), // 4x4 matrix
      .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
    });

  camera_matrix_reversed_depth_buffer = wgpuDeviceCreateBuffer(
    wgpu_context->device,
    &(WGPUBufferDescriptor){
      .size  = sizeof(mat4), // 4x4 matrix
      .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
    });
}

static void setup_uniform_bind_groups(wgpu_context_t* wgpu_context)
{
  // 1st uniform bind group
  {
    const uint32_t mode = (uint32_t)DepthBufferMode_Default;
    WGPUBindGroupEntry bg_entries[2] = {
      [0] = (WGPUBindGroupEntry) {
        .binding = 0,
        .buffer = uniform_buffer,
        .size  = uniform_buffer_size,
      },
      [1] = (WGPUBindGroupEntry) {
        .binding = 1,
        .buffer = camera_matrix_buffer,
        .size  = sizeof(mat4), // 4x4 matrix
      }
    };
    WGPURenderPipeline pipeline = depth_pre_pass_pipelines[mode];
    uniform_bind_groups[0]      = wgpuDeviceCreateBindGroup(
      wgpu_context->device,
      &(WGPUBindGroupDescriptor){
        .layout     = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0),
        .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
        .entries    = bg_entries,
      });
  }

  // 2nd uniform bind group
  {
    const uint32_t mode = (uint32_t)DepthBufferMode_Reversed;
    WGPUBindGroupEntry bg_entries[2] = {
      [0] = (WGPUBindGroupEntry) {
        .binding = 0,
        .buffer = uniform_buffer,
        .size  = uniform_buffer_size,
      },
      [1] = (WGPUBindGroupEntry) {
        .binding = 1,
        .buffer = camera_matrix_reversed_depth_buffer,
        .size  = sizeof(mat4), // 4x4 matrix
      }
    };
    WGPURenderPipeline pipeline = depth_pre_pass_pipelines[mode];
    uniform_bind_groups[mode]   = wgpuDeviceCreateBindGroup(
      wgpu_context->device,
      &(WGPUBindGroupDescriptor){
        .layout     = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0),
        .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
        .entries    = bg_entries,
      });
  }
}

static void init_uniform_buffers(wgpu_context_t* wgpu_context)
{
  uint32_t m = 0;
  for (uint32_t x = 0; x < x_count; ++x) {
    for (uint32_t y = 0; y < y_count; ++y) {
      const float z = -800.0f * m;
      const float s = 1.0f + 50.0f * m;

      glm_mat4_identity(model_matrices[m]);

      glm_translate(model_matrices[m], //
                    (vec3){
                      x - x_count / 2.0f + 0.5f,                       // x
                      (4.0f - 0.2f * z) * (y - y_count / 2.0f + 1.0f), // y
                      z,                                               // z
                    });
      glm_scale(model_matrices[m], (vec3){s, s, s});

      ++m;
    }
  }

  mat4 view_matrix = GLM_MAT4_IDENTITY_INIT;
  glm_translate(view_matrix, (vec3){0.0f, 0.0f, -12.0f});

  const float aspect
    = ((float)wgpu_context->surface.width / (float)wgpu_context->surface.height)
      * 0.5f;
  float projection_matrix_as_array[16] = {
    1.0f, 0.0f, 0.0f, 0.0f, //
    0.0f, 1.0f, 0.0f, 0.0f, //
    0.0f, 0.0f, 1.0f, 0.0f, //
    0.0f, 0.0f, 0.0f, 1.0f, //
  };
  float far = INFINITY;
  perspective_zo(&projection_matrix_as_array, (2.0f * PI) / 5.0f, aspect, 5.0f,
                 &far);
  mat4 projection_matrix = GLM_MAT4_IDENTITY_INIT;
  float_array_to_mat4(&projection_matrix_as_array, &projection_matrix);

  mat4 view_projection_matrix = GLM_MAT4_IDENTITY_INIT;
  glm_mat4_mul(projection_matrix, view_matrix, view_projection_matrix);
  mat4 reversed_range_view_projection_matrix = GLM_MAT4_IDENTITY_INIT;
  // to use 1/z we just multiple depthRangeRemapMatrix to our default camera
  // view projection matrix
  glm_mat4_mul(depth_range_remap_matrix, view_projection_matrix,
               reversed_range_view_projection_matrix);

  wgpu_queue_write_buffer(wgpu_context, camera_matrix_buffer, 0,
                          view_projection_matrix, sizeof(mat4));
  wgpu_queue_write_buffer(wgpu_context, camera_matrix_reversed_depth_buffer, 0,
                          reversed_range_view_projection_matrix, sizeof(mat4));
}

static void update_transformation_matrix(wgpu_example_context_t* context)
{
  const float now = context->frame.timestamp_millis / 1000.0f;

  const float sin_now = sin(now);
  const float cos_now = cos(now);

  for (uint32_t i = 0, m = 0; i < num_instances; ++i, m += matrix_float_count) {
    glm_mat4_copy(model_matrices[i], tmp_mat4);
    glm_rotate(tmp_mat4, (PI / 180) * 30.0f, (vec3){sin_now, cos_now, 0.0f});
    memcpy(&mvp_matrices_data[m], tmp_mat4, sizeof(mat4));
  }
}

static void update_uniform_buffers(wgpu_example_context_t* context)
{
  update_transformation_matrix(context);

  wgpu_queue_write_buffer(context->wgpu_context, uniform_buffer, 0,
                          &mvp_matrices_data, sizeof(mvp_matrices_data));
}

static int example_initialize(wgpu_example_context_t* context)
{
  UNUSED_VAR(depth_buffer_modes);

  if (context) {
    prepare_vertex_buffer(context->wgpu_context);
    prepare_depth_pre_pass_render_pipeline(context->wgpu_context);
    prepare_precision_pass_render_pipeline(context->wgpu_context);
    prepare_color_pass_render_pipeline(context->wgpu_context);
    prepare_texture_quad_pass_render_pipeline(context->wgpu_context);
    prepare_depth_textures(context->wgpu_context);
    prepare_depth_pre_pass_descriptor();
    prepare_draw_pass_descriptors();
    prepare_texture_quad_pass_descriptors();
    prepare_depth_texture_bind_group_layout(context->wgpu_context);
    prepare_depth_texture_bind_group(context->wgpu_context);
    prepare_uniform_buffers(context->wgpu_context);
    setup_uniform_bind_groups(context->wgpu_context);
    init_uniform_buffers(context->wgpu_context);
    prepared = true;
    return 0;
  }

  return 1;
}

static void example_on_update_ui_overlay(wgpu_example_context_t* context)
{
  if (imgui_overlay_header("Settings")) {
    imgui_overlay_checkBox(context->imgui_overlay, "Paused", &context->paused);
    static const char* mode[3] = {"color", "precision-error", "depth-texture"};
    int32_t item_index         = (int32_t)current_render_mode;
    if (imgui_overlay_combo_box(context->imgui_overlay, "Mode", &item_index,
                                mode, 3)) {
      current_render_mode = (render_mode_enum)item_index;
    }
  }
}

static WGPUCommandBuffer build_command_buffer(wgpu_context_t* wgpu_context)
{
  const WGPUTextureView attachment = wgpu_context->swap_chain.frame_buffer;

  // Create command encoder
  wgpu_context->cmd_enc
    = wgpuDeviceCreateCommandEncoder(wgpu_context->device, NULL);

  if (current_render_mode == RenderMode_Color) {
    for (uint32_t m = 0; m < (uint32_t)ARRAY_SIZE(depth_buffer_modes); ++m) {
      dpd_rp_color_att_descriptors[m][0].view  = attachment;
      dpd_rp_ds_att_descriptors[m].depthLoadOp = depth_load_values[m];
      WGPURenderPassEncoder color_pass = wgpuCommandEncoderBeginRenderPass(
        wgpu_context->cmd_enc, &draw_pass_descriptors[m]);
      wgpuRenderPassEncoderSetPipeline(color_pass, color_pass_pipelines[m]);
      wgpuRenderPassEncoderSetBindGroup(color_pass, 0, uniform_bind_groups[m],
                                        0, 0);
      wgpuRenderPassEncoderSetVertexBuffer(color_pass, 0, vertices.buffer, 0,
                                           0);
      wgpuRenderPassEncoderSetViewport(color_pass, viewport_width * m, 0.0f,
                                       viewport_width, default_canvas_height,
                                       0.0f, 1.0f);
      wgpuRenderPassEncoderDraw(color_pass, geometry_draw_count, num_instances,
                                0, 0);
      wgpuRenderPassEncoderEndPass(color_pass);
      WGPU_RELEASE_RESOURCE(RenderPassEncoder, color_pass)
    }
  }
  else if (current_render_mode == RenderMode_Precision_Error) {
    for (uint32_t m = 0; m < (uint32_t)ARRAY_SIZE(depth_buffer_modes); ++m) {
      // depthPrePass
      {
        dppd_rp_ds_att_descriptor.depthLoadOp = depth_load_values[m];
        WGPURenderPassEncoder depth_pre_pass
          = wgpuCommandEncoderBeginRenderPass(wgpu_context->cmd_enc,
                                              &depth_pre_pass_descriptor);
        wgpuRenderPassEncoderSetPipeline(depth_pre_pass,
                                         depth_pre_pass_pipelines[m]);
        wgpuRenderPassEncoderSetBindGroup(depth_pre_pass, 0,
                                          uniform_bind_groups[m], 0, 0);
        wgpuRenderPassEncoderSetVertexBuffer(depth_pre_pass, 0, vertices.buffer,
                                             0, 0);
        wgpuRenderPassEncoderSetViewport(depth_pre_pass, viewport_width * m,
                                         0.0f, viewport_width,
                                         default_canvas_height, 0.0f, 1.0f);
        wgpuRenderPassEncoderDraw(depth_pre_pass, geometry_draw_count,
                                  num_instances, 0, 0);
        wgpuRenderPassEncoderEndPass(depth_pre_pass);
        WGPU_RELEASE_RESOURCE(RenderPassEncoder, depth_pre_pass)
      }
      // precisionErrorPass
      {
        dpd_rp_color_att_descriptors[m][0].view  = attachment;
        dpd_rp_ds_att_descriptors[m].depthLoadOp = depth_load_values[m];
        WGPURenderPassEncoder precision_error_pass
          = wgpuCommandEncoderBeginRenderPass(wgpu_context->cmd_enc,
                                              &draw_pass_descriptors[m]);
        wgpuRenderPassEncoderSetPipeline(precision_error_pass,
                                         precision_pass_pipelines[m]);
        wgpuRenderPassEncoderSetBindGroup(precision_error_pass, 0,
                                          uniform_bind_groups[m], 0, 0);
        wgpuRenderPassEncoderSetBindGroup(precision_error_pass, 1,
                                          depth_texture_bind_group, 0, 0);
        wgpuRenderPassEncoderSetVertexBuffer(precision_error_pass, 0,
                                             vertices.buffer, 0, 0);
        wgpuRenderPassEncoderSetViewport(
          precision_error_pass, viewport_width * m, 0.0f, viewport_width,
          default_canvas_height, 0.0f, 1.0f);
        wgpuRenderPassEncoderDraw(precision_error_pass, geometry_draw_count,
                                  num_instances, 0, 0);
        wgpuRenderPassEncoderEndPass(precision_error_pass);
        WGPU_RELEASE_RESOURCE(RenderPassEncoder, precision_error_pass)
      }
    }
  }
  else {
    // depth texture quad
    for (uint32_t m = 0; m < (uint32_t)ARRAY_SIZE(depth_buffer_modes); ++m) {
      // depthPrePass
      {
        dppd_rp_ds_att_descriptor.depthLoadOp = depth_load_values[m];
        WGPURenderPassEncoder depth_pre_pass
          = wgpuCommandEncoderBeginRenderPass(wgpu_context->cmd_enc,
                                              &depth_pre_pass_descriptor);
        wgpuRenderPassEncoderSetPipeline(depth_pre_pass,
                                         depth_pre_pass_pipelines[m]);
        wgpuRenderPassEncoderSetBindGroup(depth_pre_pass, 0,
                                          uniform_bind_groups[m], 0, 0);
        wgpuRenderPassEncoderSetVertexBuffer(depth_pre_pass, 0, vertices.buffer,
                                             0, 0);
        wgpuRenderPassEncoderSetViewport(depth_pre_pass, viewport_width * m,
                                         0.0f, viewport_width,
                                         default_canvas_height, 0.0f, 1.0f);
        wgpuRenderPassEncoderDraw(depth_pre_pass, geometry_draw_count,
                                  num_instances, 0, 0);
        wgpuRenderPassEncoderEndPass(depth_pre_pass);
        WGPU_RELEASE_RESOURCE(RenderPassEncoder, depth_pre_pass)
      }
      // depthTextureQuadPass
      {
        tqd_rp_color_att_descriptors[m][0].view = attachment;
        WGPURenderPassEncoder depth_texture_quad_pass
          = wgpuCommandEncoderBeginRenderPass(
            wgpu_context->cmd_enc, &texture_quad_pass_descriptors[m]);
        wgpuRenderPassEncoderSetPipeline(depth_texture_quad_pass,
                                         texture_quad_pass_pipeline);
        wgpuRenderPassEncoderSetBindGroup(depth_texture_quad_pass, 0,
                                          depth_texture_bind_group, 0, 0);
        wgpuRenderPassEncoderSetViewport(
          depth_texture_quad_pass, viewport_width * m, 0.0f, viewport_width,
          default_canvas_height, 0.0f, 1.0f);
        wgpuRenderPassEncoderDraw(depth_texture_quad_pass, 6, 1, 0, 0);
        wgpuRenderPassEncoderEndPass(depth_texture_quad_pass);
        WGPU_RELEASE_RESOURCE(RenderPassEncoder, depth_texture_quad_pass)
      }
    }
  }

  // Draw ui overlay
  draw_ui(wgpu_context->context, example_on_update_ui_overlay);

  // Get command buffer
  WGPUCommandBuffer command_buffer
    = wgpu_get_command_buffer(wgpu_context->cmd_enc);
  ASSERT(command_buffer != NULL)
  WGPU_RELEASE_RESOURCE(CommandEncoder, wgpu_context->cmd_enc)

  return command_buffer;
}

static int example_draw(wgpu_example_context_t* context)
{
  // Prepare frame
  prepare_frame(context);

  // Command buffer to be submitted to the queue
  wgpu_context_t* wgpu_context                   = context->wgpu_context;
  wgpu_context->submit_info.command_buffer_count = 1;
  wgpu_context->submit_info.command_buffers[0]
    = build_command_buffer(context->wgpu_context);

  // Submit to queue
  submit_command_buffers(context);

  // Submit frame
  submit_frame(context);

  return 0;
}

static int example_render(wgpu_example_context_t* context)
{
  if (!prepared) {
    return 1;
  }
  const int draw_result = example_draw(context);
  if (!context->paused) {
    update_uniform_buffers(context);
  }
  return draw_result;
}

static void example_destroy(wgpu_example_context_t* context)
{
  UNUSED_VAR(context);

  WGPU_RELEASE_RESOURCE(Buffer, uniform_buffer)
  WGPU_RELEASE_RESOURCE(Buffer, camera_matrix_buffer)
  WGPU_RELEASE_RESOURCE(Buffer, camera_matrix_reversed_depth_buffer)

  WGPU_RELEASE_RESOURCE(BindGroupLayout, depth_texture_bind_group_layout)
  WGPU_RELEASE_RESOURCE(BindGroup, depth_texture_bind_group)
  for (uint32_t i = 0; i < 2; ++i) {
    WGPU_RELEASE_RESOURCE(BindGroup, uniform_bind_groups[i])
  }

  wgpu_destroy_texture(&depth_texture);
  wgpu_destroy_texture(&default_depth_texture);

  for (uint32_t i = 0; i < 2; ++i) {
    WGPU_RELEASE_RESOURCE(RenderPipeline, depth_pre_pass_pipelines[i])
    WGPU_RELEASE_RESOURCE(RenderPipeline, precision_pass_pipelines[i])
    WGPU_RELEASE_RESOURCE(RenderPipeline, color_pass_pipelines[i])
  }
  WGPU_RELEASE_RESOURCE(RenderPipeline, texture_quad_pass_pipeline)
}

void example_reversed_z(int argc, char* argv[])
{
  // clang-format off
  example_run(argc, argv, &(refexport_t){
    .example_settings = (wgpu_example_settings_t){
     .title  = example_title,
     .overlay = true,
    },
    .example_window_config = (window_config_t){
      .width = default_canvas_width,
      .height = default_canvas_height,
    },
    .example_initialize_func      = &example_initialize,
    .example_render_func          = &example_render,
    .example_destroy_func         = &example_destroy
  });
  // clang-format on
}
