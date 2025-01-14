#include "examples.h"

#include <stdlib.h>
#include <string.h>

#include "../core/log.h"
#include "../core/macro.h"

void example_animometer(int argc, char* argv[]);
// void example_aquarium(int argc, char* argv[]);
void example_basisu(int argc, char* argv[]);
void example_bind_groups(int argc, char* argv[]);
void example_bloom(int argc, char* argv[]);
void example_clear_screen(int argc, char* argv[]);
void example_compute_boids(int argc, char* argv[]);
void example_compute_n_body(int argc, char* argv[]);
void example_compute_particles(int argc, char* argv[]);
void example_compute_particles_easing(int argc, char* argv[]);
void example_compute_particles_webgpu_logo(int argc, char* argv[]);
void example_compute_ray_tracing(int argc, char* argv[]);
void example_compute_shader(int argc, char* argv[]);
void example_conservative_raster(int argc, char* argv[]);
void example_conway(int argc, char* argv[]);
void example_conway_paletted_blurring(int argc, char* argv[]);
void example_coordinate_system(int argc, char* argv[]);
void example_cube_reflection(int argc, char* argv[]);
void example_cubemap(int argc, char* argv[]);
void example_deferred_rendering(int argc, char* argv[]);
void example_dynamic_uniform_buffer(int argc, char* argv[]);
void example_equirectangular_image(int argc, char* argv[]);
void example_gears(int argc, char* argv[]);
void example_gerstner_waves(int argc, char* argv[]);
void example_gltf_loading(int argc, char* argv[]);
void example_gltf_scene_rendering(int argc, char* argv[]);
void example_hdr(int argc, char* argv[]);
void example_image_blur(int argc, char* argv[]);
void example_imgui_overlay(int argc, char* argv[]);
void example_immersive_video(int argc, char* argv[]);
void example_instanced_cube(int argc, char* argv[]);
void example_minimal(int argc, char* argv[]);
void example_msaa_line(int argc, char* argv[]);
void example_multi_sampling(int argc, char* argv[]);
void example_n_body_simulation(int argc, char* argv[]);
void example_occlusion_query(int argc, char* argv[]);
void example_offscreen_rendering(int argc, char* argv[]);
void example_parallax_mapping(int argc, char* argv[]);
void example_pbr_basic(int argc, char* argv[]);
void example_pbr_ibl(int argc, char* argv[]);
void example_pbr_texture(int argc, char* argv[]);
void example_post_processing(int argc, char* argv[]);
void example_prng(int argc, char* argv[]);
void example_procedural_mesh(int argc, char* argv[]);
void example_radial_blur(int argc, char* argv[]);
void example_reversed_z(int argc, char* argv[]);
void example_screenshot(int argc, char* argv[]);
void example_shadertoy(int argc, char* argv[]);
void example_shadow_mapping(int argc, char* argv[]);
void example_skybox(int argc, char* argv[]);
void example_square(int argc, char* argv[]);
void example_stencil_buffer(int argc, char* argv[]);
void example_terrain_mesh(int argc, char* argv[]);
void example_text_overlay(int argc, char* argv[]);
void example_texture_3d(int argc, char* argv[]);
void example_texture_mipmap_gen(int argc, char* argv[]);
void example_textured_cube(int argc, char* argv[]);
void example_textured_quad(int argc, char* argv[]);
void example_triangle(int argc, char* argv[]);
void example_two_cubes(int argc, char* argv[]);
void example_video_uploading(int argc, char* argv[]);
void example_wireframe_vertex_pulling(int argc, char* argv[]);

static examplecase_t g_example_cases[] = {
  {"animometer", example_animometer},
  // {"aquarium", example_aquarium},
  {"basisu", example_basisu},
  {"bind_groups", example_bind_groups},
  {"bloom", example_bloom},
  {"clear_screen", example_clear_screen},
  {"compute_boids", example_compute_boids},
  {"compute_particles", example_compute_particles},
  {"compute_particles_easing", example_compute_particles_easing},
  {"compute_particles_webgpu_logo", example_compute_particles_webgpu_logo},
  {"compute_ray_tracing", example_compute_ray_tracing},
  {"compute_shader", example_compute_shader},
  {"conservative_raster", example_conservative_raster},
  {"conway", example_conway},
  {"conway_paletted_blurring", example_conway_paletted_blurring},
  {"coordinate_system", example_coordinate_system},
  {"cube_reflection", example_cube_reflection},
  {"cubemap", example_cubemap},
  {"deferred_rendering", example_deferred_rendering},
  {"dynamic_uniform_buffer", example_dynamic_uniform_buffer},
  {"equirectangular_image", example_equirectangular_image},
  {"gears", example_gears},
  {"gerstner_waves", example_gerstner_waves},
  {"gltf_loading", example_gltf_loading},
  {"gltf_scene_rendering", example_gltf_scene_rendering},
  {"hdr", example_hdr},
  {"image_blur", example_image_blur},
  {"imgui_overlay", example_imgui_overlay},
  {"immersive_video", example_immersive_video},
  {"instanced_cube", example_instanced_cube},
  {"minimal", example_minimal},
  {"msaa_line", example_msaa_line},
  {"multi_sampling", example_multi_sampling},
  {"n_body_simulation", example_n_body_simulation},
  {"occlusion_query", example_occlusion_query},
  {"offscreen_rendering", example_offscreen_rendering},
  {"parallax_mapping", example_parallax_mapping},
  {"pbr_basic", example_pbr_basic},
  {"pbr_ibl", example_pbr_ibl},
  {"pbr_texture", example_pbr_texture},
  {"post_processing", example_post_processing},
  {"prng", example_prng},
  {"procedural_mesh", example_procedural_mesh},
  {"radial_blur", example_radial_blur},
  {"reversed_z", example_reversed_z},
  {"screenshot", example_screenshot},
  {"shadertoy", example_shadertoy},
  {"shadow_mapping", example_shadow_mapping},
  {"skybox", example_skybox},
  {"square", example_square},
  {"stencil_buffer", example_stencil_buffer},
  {"terrain_mesh", example_terrain_mesh},
  {"text_overlay", example_text_overlay},
  {"texture_3d", example_texture_3d},
  {"texture_mipmap_gen", example_texture_mipmap_gen},
  {"textured_cube", example_textured_cube},
  {"textured_quad", example_textured_quad},
  {"triangle", example_triangle},
  {"two_cubes", example_two_cubes},
  {"video_uploading", example_video_uploading},
  {"wireframe_vertex_pulling", example_wireframe_vertex_pulling},
};

int get_number_of_examples()
{
  return ARRAY_SIZE(g_example_cases);
}

examplecase_t* get_examples()
{
  return g_example_cases;
}

examplecase_t* get_example_by_name(const char* example_name)
{
  examplecase_t* example     = NULL;
  const int num_examplecases = get_number_of_examples();
  for (int i = 0; i < num_examplecases; i++) {
    if (strcmp(g_example_cases[i].example_name, example_name) == 0) {
      example = &g_example_cases[i];
      break;
    }
  }
  return example;
}

examplecase_t* get_random_example()
{
  const int i = rand() % get_number_of_examples();
  return &g_example_cases[i];
}

void log_example_names()
{
  const int num_examplecases = get_number_of_examples();
  printf("Available examples (%d):\n", num_examplecases);
  for (int i = 0; i < num_examplecases; i++) {
    printf("  |- %s\n", g_example_cases[i].example_name);
  }
}
