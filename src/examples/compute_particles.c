#include "example_base.h"
#include "examples.h"

#include <string.h>

#include "../webgpu/imgui_overlay.h"

/* -------------------------------------------------------------------------- *
 * WebGPU Example - Compute Shader Particle System
 *
 * Attraction based 2D GPU particle system using compute shaders. Particle data
 * is stored in a shader storage buffer.
 *
 * Ref:
 * https://github.com/SaschaWillems/Vulkan/tree/master/examples/computeparticles
 * -------------------------------------------------------------------------- */

#define PARTICLE_COUNT 256 * 1024

static float timer           = 0.0f;
static float animStart       = 20.0f;
static bool attach_to_cursor = false;

static struct {
  texture_t particle;
  texture_t gradient;
} textures;

// Resources for the graphics part of the example
static struct {
  WGPUBindGroupLayout bind_group_layout; // Particle system rendering shader
                                         // binding layout
  WGPUBindGroup bind_group; // Particle system rendering shader bindings
  WGPUPipelineLayout pipeline_layout; // Layout of the graphics pipeline
  WGPURenderPipeline pipeline;        // Particle rendering pipeline
} graphics;

// Resources for the compute part of the example
static struct {
  WGPUBuffer storage_buffer; // (Shader) storage buffer object containing the
                             // particles
  WGPUBuffer uniform_buffer; // Uniform buffer object containing particle
                             // system parameters
  WGPUBindGroupLayout bind_group_layout; // Compute shader binding layout
  WGPUBindGroup bind_group;              // Compute shader bindings
  WGPUPipelineLayout pipeline_layout;    // Layout of the compute pipeline
  WGPUComputePipeline pipeline; // Compute pipeline for updating particle
                                // positions
  struct compute_ubo_t {        // Compute shader uniform block object
    float delta_t;              // Frame delta time
    float dest_x;               // x position of the attractor
    float dest_y;               // y position of the attractor
    int32_t particle_count;
  } ubo;
} compute;

// Render pass descriptor for frame buffer writes
static WGPURenderPassColorAttachmentDescriptor rp_color_att_descriptors[1];
static WGPURenderPassDescriptor render_pass_desc;

// SSBO particle declaration
typedef struct particle_t {
  vec2 pos;          // Particle position
  vec2 vel;          // Particle velocity
  vec4 gradient_pos; // Texture coordinates for the gradient ramp map
} particle_t;

// Other variables
static const char* example_title = "Compute Shader Particle System";
static bool prepared             = false;

static float rand_float_min_max(float min, float max)
{
  /* [min, max] */
  return ((max - min) * ((float)rand() / (float)RAND_MAX)) + min;
}

static void load_assets(wgpu_context_t* wgpu_context)
{
  textures.particle = wgpu_texture_load_from_ktx_file(
    wgpu_context, "textures/particle01_rgba.ktx");
  textures.gradient = wgpu_texture_load_from_ktx_file(
    wgpu_context, "textures/particle_gradient_rgba.ktx");
}

// Setup and fill the compute shader storage buffers containing the particles
static void prepare_storage_buffers(wgpu_context_t* wgpu_context)
{
  // Initial particle positions
  static particle_t particle_buffer[PARTICLE_COUNT] = {0};
  for (uint32_t i = 0; i < (uint32_t)PARTICLE_COUNT; ++i) {
    particle_buffer[i] = (particle_t){
      .pos = {rand_float_min_max(-1.0f, 1.0f), rand_float_min_max(-1.0f, 1.0f)},
      .vel = GLM_VEC2_ZERO_INIT,
      .gradient_pos = GLM_VEC4_ZERO_INIT,
    };
    particle_buffer[i].gradient_pos[0] = particle_buffer[i].pos[0] / 2.0f;
  }

  uint64_t storage_buffer_size = PARTICLE_COUNT * sizeof(particle_t);

  // Staging
  // SSBO won't be changed on the host after upload
  compute.storage_buffer = wgpu_create_buffer_from_data(
    wgpu_context, &particle_buffer, storage_buffer_size,
    WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage);
}

static void update_uniform_buffers(wgpu_example_context_t* context)
{
  wgpu_context_t* wgpu_context = context->wgpu_context;

  // Update uniform buffer data
  compute.ubo.delta_t = context->frame_timer * 2.5f;
  if (!attach_to_cursor) {
    compute.ubo.dest_x = sin(glm_rad(timer * 360.0f)) * 0.75f;
    compute.ubo.dest_y = 0.0f;
  }
  else {
    float width  = (float)wgpu_context->surface.width;
    float height = (float)wgpu_context->surface.height;

    float normalized_mx
      = (context->mouse_position[0] - (width / 2.0f)) / (width / 2.0f);
    float normalized_my
      = ((height / 2.0f) - context->mouse_position[1]) / (height / 2.0f);
    compute.ubo.dest_x = normalized_mx;
    compute.ubo.dest_y = normalized_my;
  }

  // Map uniform buffer and update it
  wgpu_queue_write_buffer(wgpu_context, compute.uniform_buffer, 0, &compute.ubo,
                          sizeof(compute.ubo));
}

// Prepare and initialize uniform buffer containing shader uniforms
static void prepare_uniform_buffers(wgpu_example_context_t* context)
{
  // Initialize the uniform buffer block
  compute.ubo.particle_count = PARTICLE_COUNT;

  // Compute shader uniform buffer block
  compute.uniform_buffer = wgpuDeviceCreateBuffer(
    context->wgpu_context->device,
    &(WGPUBufferDescriptor){
      .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
      .size  = sizeof(compute.ubo),
    });

  update_uniform_buffers(context);
}

static void setup_render_pass(wgpu_context_t* wgpu_context)
{
  // Color attachment
  rp_color_att_descriptors[0] = (WGPURenderPassColorAttachmentDescriptor) {
      .view = NULL,
      .attachment = NULL,
      .loadOp     = WGPULoadOp_Clear,
      .storeOp    = WGPUStoreOp_Store,
      .clearColor = (WGPUColor) {
        .r = 0.025f,
        .g = 0.025f,
        .b = 0.025f,
        .a = 1.0f,
      },
  };

  // Depth attachment
  wgpu_setup_deph_stencil(wgpu_context);

  // Render pass descriptor
  render_pass_desc = (WGPURenderPassDescriptor){
    .colorAttachmentCount   = 1,
    .colorAttachments       = rp_color_att_descriptors,
    .depthStencilAttachment = &wgpu_context->depth_stencil.att_desc,
  };
}

static void setup_pipeline_layout(wgpu_context_t* wgpu_context)
{
  WGPUBindGroupLayoutEntry bgl_entries[4] = {
    [0] = (WGPUBindGroupLayoutEntry) {
      // Binding 0 : Particle color map texture
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
      // Binding 1 : Particle color map sampler
      .binding = 1,
      .visibility = WGPUShaderStage_Fragment,
      .sampler = (WGPUSamplerBindingLayout){
        .type=WGPUSamplerBindingType_Filtering,
      },
      .texture = {0},
    },
    [2] = (WGPUBindGroupLayoutEntry) {
      // Binding 2 : Particle gradient ramp texture
      .binding = 2,
      .visibility = WGPUShaderStage_Fragment,
      .texture = (WGPUTextureBindingLayout) {
        .sampleType = WGPUTextureSampleType_Float,
        .viewDimension = WGPUTextureViewDimension_2D,
        .multisampled = false,
      },
      .storageTexture = {0},
    },
    [3] = (WGPUBindGroupLayoutEntry) {
      // Binding 3 : Particle gradient ramp sampler
      .binding = 3,
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
  graphics.bind_group_layout
    = wgpuDeviceCreateBindGroupLayout(wgpu_context->device, &bgl_desc);
  ASSERT(graphics.bind_group_layout != NULL)

  // Create the pipeline layout
  WGPUPipelineLayoutDescriptor pipeline_layout_desc = {
    .bindGroupLayoutCount = 1,
    .bindGroupLayouts     = &graphics.bind_group_layout,
  };
  graphics.pipeline_layout = wgpuDeviceCreatePipelineLayout(
    wgpu_context->device, &pipeline_layout_desc);
  ASSERT(graphics.pipeline_layout != NULL);
}

static void prepare_pipelines(wgpu_context_t* wgpu_context)
{
  // Rasterization state
  WGPURasterizationStateDescriptor rasterization_state_desc
    = wgpu_create_rasterization_state_descriptor(
      &(create_rasterization_state_desc_t){
        .front_face = WGPUFrontFace_CCW,
        .cull_mode  = WGPUCullMode_None,
      });

  // Color blend state: additive blending
  WGPUColorStateDescriptor color_state_desc
    = wgpu_create_color_state_descriptor(&(create_color_state_desc_t){
      .format       = wgpu_context->swap_chain.format,
      .enable_blend = true,
    });
  color_state_desc.colorBlend.srcFactor = WGPUBlendFactor_One;
  color_state_desc.colorBlend.dstFactor = WGPUBlendFactor_One;
  color_state_desc.alphaBlend.srcFactor = WGPUBlendFactor_SrcAlpha;
  color_state_desc.alphaBlend.dstFactor = WGPUBlendFactor_DstAlpha;

  // Depth and stencil state containing depth and stencil compare and test
  // operations
  WGPUDepthStencilStateDescriptor depth_stencil_state_desc
    = wgpu_create_depth_stencil_state_descriptor(
      &(create_depth_stencil_state_desc_t){
        .format              = WGPUTextureFormat_Depth24PlusStencil8,
        .depth_write_enabled = false,
      });

  // Vertex input binding (=> Input assembly)
  WGPU_VERTSTATE(particle, sizeof(particle_t),
                 // Attribute descriptions
                 // Describes memory layout and shader positions
                 // Attribute location 0: Position
                 WGPU_VERTATTR_DESC(0, WGPUVertexFormat_Float32x2,
                                    offsetof(particle_t, pos)),
                 // Attribute location 1: Gradient position
                 WGPU_VERTATTR_DESC(1, WGPUVertexFormat_Float32x4,
                                    offsetof(particle_t, gradient_pos)))

  // Shaders
  // Vertex shader
  wgpu_shader_t vert_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Vertex shader SPIR-V
                    .file = "shaders/compute_particles/particle.vert.spv",
                  });
  // Fragment shader
  wgpu_shader_t frag_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Fragment shader SPIR-V
                    .file = "shaders/compute_particles/particle.frag.spv",
                  });

  // Create rendering pipeline using the specified states
  graphics.pipeline = wgpuDeviceCreateRenderPipeline(
    wgpu_context->device,
    &(WGPURenderPipelineDescriptor){
      .layout = graphics.pipeline_layout,
      // Vertex shader
      .vertexStage = vert_shader.programmable_stage_descriptor,
      // Fragment shader
      .fragmentStage = &frag_shader.programmable_stage_descriptor,
      // Rasterization state
      .rasterizationState     = &rasterization_state_desc,
      .primitiveTopology      = WGPUPrimitiveTopology_PointList,
      .colorStateCount        = 1,
      .colorStates            = &color_state_desc,
      .depthStencilState      = &depth_stencil_state_desc,
      .vertexState            = &vert_state_particle,
      .sampleCount            = 1,
      .sampleMask             = 0xFFFFFFFF,
      .alphaToCoverageEnabled = false,
    });

  // Shader modules are no longer needed once the graphics pipeline has been
  // created
  wgpu_shader_release(&frag_shader);
  wgpu_shader_release(&vert_shader);
}

static void setup_bind_groups(wgpu_context_t* wgpu_context)
{
  WGPUBindGroupEntry bg_entries[4] = {
    [0] = (WGPUBindGroupEntry) {
      // Binding 0 : Particle color map texture
      .binding = 0,
      .textureView = textures.particle.view,
    },
    [1] = (WGPUBindGroupEntry) {
       // Binding 1 : Particle color map sampler
      .binding = 1,
      .sampler = textures.particle.sampler,
    },
    [2] = (WGPUBindGroupEntry) {
       // Binding 2 : Particle gradient ramp texture
      .binding = 2,
      .textureView = textures.gradient.view,
    },
    [3] = (WGPUBindGroupEntry) {
      // Binding 3 : Particle gradient ramp sampler
      .binding = 3,
      .sampler = textures.gradient.sampler,
    }
  };

  WGPUBindGroupDescriptor bg_desc = {
    .layout     = graphics.bind_group_layout,
    .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
    .entries    = bg_entries,
  };

  graphics.bind_group
    = wgpuDeviceCreateBindGroup(wgpu_context->device, &bg_desc);
  ASSERT(graphics.bind_group != NULL)
}

static void prepare_graphics(wgpu_example_context_t* context)
{
  wgpu_context_t* wgpu_context = context->wgpu_context;

  prepare_storage_buffers(wgpu_context);
  prepare_uniform_buffers(context);
  setup_pipeline_layout(wgpu_context);
  prepare_pipelines(wgpu_context);
  setup_bind_groups(wgpu_context);
  setup_render_pass(wgpu_context);
}

static void prepare_compute(wgpu_context_t* wgpu_context)
{
  /* Compute pipeline layout */
  WGPUBindGroupLayoutEntry bgl_entries[2] = {
    [0] = (WGPUBindGroupLayoutEntry) {
      // Binding 0 : Particle position storage buffer
      .binding = 0,
      .visibility = WGPUShaderStage_Compute,
      .buffer = (WGPUBufferBindingLayout) {
        .type = WGPUBufferBindingType_Storage,
        .minBindingSize = PARTICLE_COUNT * sizeof(particle_t),
      },
      .sampler = {0},
    },
    [1] = (WGPUBindGroupLayoutEntry) {
      // Binding 1 : Uniform buffer
      .binding = 1,
      .visibility = WGPUShaderStage_Compute,
      .buffer = (WGPUBufferBindingLayout) {
        .type = WGPUBufferBindingType_Uniform,
        .minBindingSize = sizeof(compute.ubo),
      },
      .sampler = {0},
    }
  };
  WGPUBindGroupLayoutDescriptor bgl_desc = {
    .entryCount = (uint32_t)ARRAY_SIZE(bgl_entries),
    .entries    = bgl_entries,
  };
  compute.bind_group_layout
    = wgpuDeviceCreateBindGroupLayout(wgpu_context->device, &bgl_desc);
  ASSERT(compute.bind_group_layout != NULL)

  WGPUPipelineLayoutDescriptor compute_pipeline_layout_desc = {
    .bindGroupLayoutCount = 1,
    .bindGroupLayouts     = &compute.bind_group_layout,
  };
  compute.pipeline_layout = wgpuDeviceCreatePipelineLayout(
    wgpu_context->device, &compute_pipeline_layout_desc);
  ASSERT(compute.pipeline_layout != NULL)

  /* Compute pipeline bind group */
  WGPUBindGroupEntry bg_entries[2] = {
    [0] = (WGPUBindGroupEntry) {
      // Binding 0 : Particle position storage buffer
      .binding = 0,
      .buffer = compute.storage_buffer,
      .size = PARTICLE_COUNT * sizeof(particle_t),
    },
    [1] = (WGPUBindGroupEntry) {
     // Binding 1 : Uniform buffer
      .binding = 1,
      .buffer = compute.uniform_buffer,
      .offset = 0,
      .size = sizeof(compute.ubo),
    },
  };
  WGPUBindGroupDescriptor bg_desc = {
    .layout     = compute.bind_group_layout,
    .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
    .entries    = bg_entries,
  };
  compute.bind_group
    = wgpuDeviceCreateBindGroup(wgpu_context->device, &bg_desc);

  /* Compute shader */
  wgpu_shader_t particle_comp_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Compute shader SPIR-V
                    .file = "shaders/compute_particles/particle.comp.spv",
                  });

  /* Create pipeline */
  compute.pipeline = wgpuDeviceCreateComputePipeline(
    wgpu_context->device,
    &(WGPUComputePipelineDescriptor){
      .layout       = compute.pipeline_layout,
      .computeStage = particle_comp_shader.programmable_stage_descriptor,
    });

  /* Partial clean-up */
  wgpu_shader_release(&particle_comp_shader);
}

static int example_initialize(wgpu_example_context_t* context)
{
  if (context) {
    load_assets(context->wgpu_context);
    prepare_graphics(context);
    prepare_compute(context->wgpu_context);
    prepared = true;
    return 0;
  }

  return 1;
}

static void example_on_update_ui_overlay(wgpu_example_context_t* context)
{
  if (imgui_overlay_header("Settings")) {
    imgui_overlay_checkBox(context->imgui_overlay, "Attach attractor to cursor",
                           &attach_to_cursor);
  }
}

static WGPUCommandBuffer build_command_buffer(wgpu_context_t* wgpu_context)
{
  rp_color_att_descriptors[0].view = wgpu_context->swap_chain.frame_buffer;

  // Create command encoder
  wgpu_context->cmd_enc
    = wgpuDeviceCreateCommandEncoder(wgpu_context->device, NULL);

  // Compute pass: Compute particle movement
  {
    wgpu_context->cpass_enc
      = wgpuCommandEncoderBeginComputePass(wgpu_context->cmd_enc, NULL);
    // Dispatch the compute job
    wgpuComputePassEncoderSetPipeline(wgpu_context->cpass_enc,
                                      compute.pipeline);
    wgpuComputePassEncoderSetBindGroup(wgpu_context->cpass_enc, 0,
                                       compute.bind_group, 0, NULL);
    wgpuComputePassEncoderDispatch(wgpu_context->cpass_enc,
                                   PARTICLE_COUNT / 256, 1, 1);
    wgpuComputePassEncoderEndPass(wgpu_context->cpass_enc);
    WGPU_RELEASE_RESOURCE(ComputePassEncoder, wgpu_context->cpass_enc)
  }

  // Render pass: Draw the particle system using the update vertex buffer
  {
    wgpu_context->rpass_enc = wgpuCommandEncoderBeginRenderPass(
      wgpu_context->cmd_enc, &render_pass_desc);
    wgpuRenderPassEncoderSetPipeline(wgpu_context->rpass_enc,
                                     graphics.pipeline);
    wgpuRenderPassEncoderSetBindGroup(wgpu_context->rpass_enc, 0,
                                      graphics.bind_group, 0, 0);
    wgpuRenderPassEncoderSetVertexBuffer(wgpu_context->rpass_enc, 0,
                                         compute.storage_buffer, 0, 0);
    wgpuRenderPassEncoderDraw(wgpu_context->rpass_enc, PARTICLE_COUNT, 1, 0, 0);
    wgpuRenderPassEncoderEndPass(wgpu_context->rpass_enc);
    WGPU_RELEASE_RESOURCE(RenderPassEncoder, wgpu_context->rpass_enc)
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
  int result = example_draw(context);

  if (!attach_to_cursor) {
    if (animStart > 0.0f) {
      animStart -= context->frame_timer * 5.0f;
    }
    else if (animStart <= 0.0f) {
      timer += context->frame_timer * 0.04f;
      if (timer > 1.f) {
        timer = 0.f;
      }
    }
  }

  update_uniform_buffers(context);

  return result;
}

static void example_destroy(wgpu_example_context_t* context)
{
  UNUSED_VAR(context);

  // Textures
  wgpu_destroy_texture(&textures.particle);
  wgpu_destroy_texture(&textures.gradient);

  // Graphics pipeline
  WGPU_RELEASE_RESOURCE(BindGroupLayout, graphics.bind_group_layout)
  WGPU_RELEASE_RESOURCE(BindGroup, graphics.bind_group)
  WGPU_RELEASE_RESOURCE(PipelineLayout, graphics.pipeline_layout)
  WGPU_RELEASE_RESOURCE(RenderPipeline, graphics.pipeline)

  // Compute pipeline
  WGPU_RELEASE_RESOURCE(Buffer, compute.storage_buffer)
  WGPU_RELEASE_RESOURCE(Buffer, compute.uniform_buffer)
  WGPU_RELEASE_RESOURCE(BindGroupLayout, compute.bind_group_layout)
  WGPU_RELEASE_RESOURCE(BindGroup, compute.bind_group)
  WGPU_RELEASE_RESOURCE(PipelineLayout, compute.pipeline_layout)
  WGPU_RELEASE_RESOURCE(ComputePipeline, compute.pipeline)
}

void example_compute_particles(int argc, char* argv[])
{
  // clang-format off
  example_run(argc, argv, &(refexport_t){
    .example_settings = (wgpu_example_settings_t){
     .title  = example_title,
     .overlay = true,
    },
    .example_initialize_func      = &example_initialize,
    .example_render_func          = &example_render,
    .example_destroy_func         = &example_destroy,
  });
  // clang-format on
}
