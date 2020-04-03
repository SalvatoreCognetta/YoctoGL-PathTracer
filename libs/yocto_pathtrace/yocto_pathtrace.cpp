//
// Implementation for Yocto/RayTrace.
//

//
// LICENSE:
//
// Copyright (c) 2016 -- 2020 Fabio Pellacini
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include "yocto_pathtrace.h"

#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
using namespace std::string_literals;

// -----------------------------------------------------------------------------
// MATH FUNCTIONS
// -----------------------------------------------------------------------------
namespace yocto::pathtrace {

// import math symbols for use
using math::abs;
using math::acos;
using math::atan2;
using math::clamp;
using math::cos;
using math::exp;
using math::flt_max;
using math::fmod;
using math::fresnel_conductor;
using math::fresnel_dielectric;
using math::identity3x3f;
using math::invalidb3f;
using math::log;
using math::make_rng;
using math::max;
using math::min;
using math::pif;
using math::pow;
using math::sample_discrete_cdf;
using math::sample_discrete_cdf_pdf;
using math::sample_uniform;
using math::sample_uniform_pdf;
using math::sin;
using math::sqrt;
using math::zero2f;
using math::zero2i;
using math::zero3f;
using math::zero3i;
using math::zero4f;
using math::zero4i;

}  // namespace yocto::pathtrace

// -----------------------------------------------------------------------------
// IMPLEMENTATION FOR SCENE EVALUATION
// -----------------------------------------------------------------------------
namespace yocto::pathtrace {

// Check texture size
static vec2i texture_size(const ptr::texture* texture) {
  if (!texture->colorf.empty()) {
    return texture->colorf.size();
  } else if (!texture->colorb.empty()) {
    return texture->colorb.size();
  } else if (!texture->scalarf.empty()) {
    return texture->scalarf.size();
  } else if (!texture->scalarb.empty()) {
    return texture->scalarb.size();
  } else {
    return zero2i;
  }
}

// Evaluate a texture
static vec3f lookup_texture(
    const ptr::texture* texture, const vec2i& ij, bool ldr_as_linear = false) {
  if (!texture->colorf.empty()) {
    return texture->colorf[ij];
  } else if (!texture->colorb.empty()) {
    return ldr_as_linear ? byte_to_float(texture->colorb[ij])
                         : srgb_to_rgb(byte_to_float(texture->colorb[ij]));
  } else if (!texture->scalarf.empty()) {
    return vec3f{texture->scalarf[ij]};
  } else if (!texture->scalarb.empty()) {
    return ldr_as_linear
               ? byte_to_float(vec3b{texture->scalarb[ij]})
               : srgb_to_rgb(byte_to_float(vec3b{texture->scalarb[ij]}));
  } else {
    return {1, 1, 1};
  }
}

// Evaluate a texture
static vec3f eval_texture(const ptr::texture* texture, const vec2f& uv,
    bool ldr_as_linear = false, bool no_interpolation = false,
    bool clamp_to_edge = false) {
  // get texture
  if (!texture) return {1, 1, 1};

  // get yimg::image width/height
  auto size = texture_size(texture);

  // get coordinates normalized for tiling
  auto s = 0.0f, t = 0.0f;
  if (clamp_to_edge) {
    s = clamp(uv.x, 0.0f, 1.0f) * size.x;
    t = clamp(uv.y, 0.0f, 1.0f) * size.y;
  } else {
    s = fmod(uv.x, 1.0f) * size.x;
    if (s < 0) s += size.x;
    t = fmod(uv.y, 1.0f) * size.y;
    if (t < 0) t += size.y;
  }

  // get yimg::image coordinates and residuals
  auto i = clamp((int)s, 0, size.x - 1), j = clamp((int)t, 0, size.y - 1);
  auto ii = (i + 1) % size.x, jj = (j + 1) % size.y;
  auto u = s - i, v = t - j;

  if (no_interpolation) return lookup_texture(texture, {i, j}, ldr_as_linear);

  // handle interpolation
  return lookup_texture(texture, {i, j}, ldr_as_linear) * (1 - u) * (1 - v) +
         lookup_texture(texture, {i, jj}, ldr_as_linear) * (1 - u) * v +
         lookup_texture(texture, {ii, j}, ldr_as_linear) * u * (1 - v) +
         lookup_texture(texture, {ii, jj}, ldr_as_linear) * u * v;
}
static float eval_texturef(const ptr::texture* texture, const vec2f& uv,
    bool ldr_as_linear = false, bool no_interpolation = false,
    bool clamp_to_edge = false) {
  return eval_texture(
      texture, uv, ldr_as_linear, no_interpolation, clamp_to_edge)
      .x;
}

// Generates a ray from a camera for yimg::image plane coordinate uv and
// the lens coordinates luv.
static ray3f eval_camera(
    const ptr::camera* camera, const vec2f& image_uv, const vec2f& lens_uv) {
  // YOUR CODE GOES HERE ------------------------------------------------------
  // point on the image plane
  auto q = vec3f{camera->film.x * (0.5f - image_uv.x),
            camera->film.y * (image_uv.y - 0.5f), camera->lens};
  // ray direction through the lens center
  auto dc = -normalize(q);
  // point on the lens
  auto e = vec3f{lens_uv.x * camera->aperture / 2,
            lens_uv.y * camera->aperture / 2, 0};
  // point on the focus plane
  auto p = dc * camera->focus / abs(dc.z);
  // correct ray direction to account for camera focusing
  auto d = normalize(p - e);
  // done
  return ray3f{transform_point(camera->frame, e),
          transform_direction(camera->frame, d)};
}

// Samples a camera ray at pixel ij of an image of size size with puv and luv
// as random numbers for pixel and lens respectively
static ray3f sample_camera(const ptr::camera* camera, const vec2i& ij,
    const vec2i& size, const vec2f& puv, const vec2f& luv) {
  // YOUR CODE GOES HERE ------------------------------------------------------
  auto uv = vec2f{(ij.x + puv.x) / size.x,
              (ij.y + puv.y) / size.y};
  return eval_camera(camera, uv, sample_disk(luv));
}

// Eval position
static vec3f eval_position(
    const ptr::object* object, int element, const vec2f& uv) {
  auto shape = object->shape;
  if (!shape->triangles.empty()) {
    auto t = shape->triangles[element];
    return transform_point(
        object->frame, interpolate_triangle(shape->positions[t.x],
                           shape->positions[t.y], shape->positions[t.z], uv));
  } else if (!shape->lines.empty()) {
    auto l = shape->lines[element];
    return transform_point(object->frame,
        interpolate_line(shape->positions[l.x], shape->positions[l.y], uv.x));
  } else if (!shape->points.empty()) {
    return transform_point(
        object->frame, shape->positions[shape->points[element]]);
  } else {
    return zero3f;
  }
}

// Shape element normal.
static vec3f eval_element_normal(const ptr::object* object, int element) {
  auto shape = object->shape;
  if (!shape->triangles.empty()) {
    auto t = shape->triangles[element];
    return transform_normal(
        object->frame, triangle_normal(shape->positions[t.x],
                           shape->positions[t.y], shape->positions[t.z]));
  } else if (!shape->lines.empty()) {
    auto l = shape->lines[element];
    return transform_normal(object->frame,
        line_tangent(shape->positions[l.x], shape->positions[l.y]));
  } else if (!shape->points.empty()) {
    return {0, 0, 1};
  } else {
    return {0, 0, 0};
  }
}

// Eval normal
static vec3f eval_normal(
    const ptr::object* object, int element, const vec2f& uv) {
  auto shape = object->shape;
  if (shape->normals.empty()) return eval_element_normal(object, element);
  if (!shape->triangles.empty()) {
    auto t = shape->triangles[element];
    return transform_normal(
        object->frame, normalize(interpolate_triangle(shape->normals[t.x],
                           shape->normals[t.y], shape->normals[t.z], uv)));
  } else if (!shape->lines.empty()) {
    auto l = shape->lines[element];
    return transform_normal(object->frame,
        normalize(
            interpolate_line(shape->normals[l.x], shape->normals[l.y], uv.x)));
  } else if (!shape->points.empty()) {
    return transform_normal(
        object->frame, normalize(shape->normals[shape->points[element]]));
  } else {
    return zero3f;
  }
}

// Eval texcoord
static vec2f eval_texcoord(
    const ptr::object* object, int element, const vec2f& uv) {
  auto shape = object->shape;
  if (shape->texcoords.empty()) return uv;
  if (!shape->triangles.empty()) {
    auto t = shape->triangles[element];
    return interpolate_triangle(shape->texcoords[t.x], shape->texcoords[t.y],
        shape->texcoords[t.z], uv);
  } else if (!shape->lines.empty()) {
    auto l = shape->lines[element];
    return interpolate_line(shape->texcoords[l.x], shape->texcoords[l.y], uv.x);
  } else if (!shape->points.empty()) {
    return shape->texcoords[shape->points[element]];
  } else {
    return zero2f;
  }
}

// Eval shading normal
static vec3f eval_shading_normal(const ptr::object* object, int element,
    const vec2f& uv, const vec3f& outgoing) {
  auto shape  = object->shape;
  auto normal = eval_normal(object, element, uv);
  if (!shape->triangles.empty()) {
    if (!object->material->thin) return normal;
    return dot(normal, outgoing) >= 0 ? normal : -normal;
  } else if (!shape->lines.empty()) {
    return orthonormalize(outgoing, normal);
  } else if (!shape->points.empty()) {
    return -outgoing;
  } else {
    return zero3f;
  }
}

// Brdf
struct brdf {
  // brdf lobes
  vec3f diffuse      = {0, 0, 0};
  vec3f specular     = {0, 0, 0};
  vec3f metal        = {0, 0, 0};
  vec3f transmission = {0, 0, 0};
  vec3f refraction   = {0, 0, 0};
  float roughness    = 0;
  float opacity      = 1;
  float ior          = 1;
  vec3f meta         = {0, 0, 0};
  vec3f metak        = {0, 0, 0};
  // weights
  float diffuse_pdf      = 0;
  float specular_pdf     = 0;
  float metal_pdf        = 0;
  float transmission_pdf = 0;
  float refraction_pdf   = 0;
};

// Eval material to obatain emission, brdf and opacity.
static vec3f eval_emission(const ptr::object* object, int element,
    const vec2f& uv, const vec3f& normal, const vec3f& outgoing) {
  auto material = object->material;
  auto texcoord = eval_texcoord(object, element, uv);
  return material->emission * eval_texture(material->emission_tex, texcoord);
}

// Eval material to obatain emission, brdf and opacity.
static brdf eval_brdf(const ptr::object* object, int element, const vec2f& uv,
    const vec3f& normal, const vec3f& outgoing) {
  auto material = object->material;
  auto texcoord = eval_texcoord(object, element, uv);
  auto brdf     = ptr::brdf{};
  auto color    = material->color *
               eval_texture(material->color_tex, texcoord, false);
  auto specular = material->specular *
                  eval_texturef(material->specular_tex, texcoord, true);
  auto metallic = material->metallic *
                  eval_texturef(material->metallic_tex, texcoord, true);
  auto roughness = material->roughness *
                   eval_texturef(material->roughness_tex, texcoord, true);
  auto ior          = material->ior;
  auto transmission = material->transmission *
                      eval_texturef(material->emission_tex, texcoord, true);
  auto opacity = material->opacity *
                 eval_texturef(material->opacity_tex, texcoord, true);
  auto thin = material->thin || !material->transmission;

  // factors
  auto weight = vec3f{1, 1, 1};
  brdf.metal  = weight * metallic;
  weight *= 1 - metallic;
  brdf.refraction = thin ? zero3f : weight * transmission;
  weight *= 1 - (thin ? 0 : transmission);
  brdf.specular = weight * specular;
  weight *= 1 - specular * fresnel_dielectric(ior, outgoing, normal);
  brdf.transmission = weight * transmission * color;
  weight *= 1 - transmission;
  brdf.diffuse   = weight * color;
  brdf.meta      = reflectivity_to_eta(color);
  brdf.metak     = zero3f;
  brdf.roughness = roughness * roughness;
  brdf.ior       = ior;
  brdf.opacity   = opacity;

  // textures
  if (brdf.diffuse != zero3f || brdf.roughness) {
    brdf.roughness = clamp(brdf.roughness, 0.03f * 0.03f, 1.0f);
  }
  if (brdf.specular == zero3f && brdf.metal == zero3f &&
      brdf.transmission == zero3f && brdf.refraction == zero3f) {
    brdf.roughness = 1;
  }
  if (brdf.opacity > 0.999f) brdf.opacity = 1;

  // weights
  brdf.diffuse_pdf  = max(brdf.diffuse);
  brdf.specular_pdf = max(
      brdf.specular * fresnel_dielectric(brdf.ior, normal, outgoing));
  brdf.metal_pdf = max(
      brdf.metal * fresnel_conductor(brdf.meta, brdf.metak, normal, outgoing));
  brdf.transmission_pdf = max(brdf.transmission);
  brdf.refraction_pdf   = max(brdf.refraction);
  auto pdf_sum = brdf.diffuse_pdf + brdf.specular_pdf + brdf.metal_pdf +
                 brdf.transmission_pdf + brdf.refraction_pdf;
  if (pdf_sum) {
    brdf.diffuse_pdf /= pdf_sum;
    brdf.specular_pdf /= pdf_sum;
    brdf.metal_pdf /= pdf_sum;
    brdf.transmission_pdf /= pdf_sum;
    brdf.refraction_pdf /= pdf_sum;
  }
  return brdf;
}

// check if a brdf is a delta
static bool is_delta(const ptr::brdf& brdf) { return !brdf.roughness; }

// Evaluate all environment color.
static vec3f eval_environment(const ptr::scene* scene, const ray3f& ray) {
  auto emission = zero3f;
  for (auto environment : scene->environments) {
    auto wl       = transform_direction(inverse(environment->frame), ray.d);
    auto texcoord = vec2f{
        atan2(wl.z, wl.x) / (2 * pif), acos(clamp(wl.y, -1.0f, 1.0f)) / pif};
    if (texcoord.x < 0) texcoord.x += 1;
    emission += environment->emission *
                eval_texture(environment->emission_tex, texcoord);
  }
  return emission;
}

}  // namespace yocto::pathtrace

// -----------------------------------------------------------------------------
// IMPLEMENTATION FOR SHAPE/SCENE BVH
// -----------------------------------------------------------------------------
namespace yocto::pathtrace {

// primitive used to sort bvh entries
struct bvh_primitive {
  bbox3f bbox      = invalidb3f;
  vec3f  center    = zero3f;
  int    primitive = 0;
};

// Splits a BVH node. Returns split position and axis.
static std::pair<int, int> split_middle(
    std::vector<bvh_primitive>& primitives, int start, int end) {
  // initialize split axis and position
  auto axis = 0;
  auto mid  = (start + end) / 2;

  // compute primintive bounds and size
  auto cbbox = invalidb3f;
  for (auto i = start; i < end; i++) cbbox = merge(cbbox, primitives[i].center);
  auto csize = cbbox.max - cbbox.min;
  if (csize == zero3f) return {mid, axis};

  // split along largest
  if (csize.x >= csize.y && csize.x >= csize.z) axis = 0;
  if (csize.y >= csize.x && csize.y >= csize.z) axis = 1;
  if (csize.z >= csize.x && csize.z >= csize.y) axis = 2;

  // split the space in the middle along the largest axis
  mid = (int)(std::partition(primitives.data() + start, primitives.data() + end,
                  [axis, middle = center(cbbox)[axis]](auto& primitive) {
                    return primitive.center[axis] < middle;
                  }) -
              primitives.data());

  // if we were not able to split, just break the primitives in half
  if (mid == start || mid == end) {
    // throw std::runtime_error("bad bvh split");
    mid = (start + end) / 2;
  }

  return {mid, axis};
}

// Maximum number of primitives per BVH node.
const int bvh_max_prims = 4;

// Build BVH nodes
static void build_bvh(
    std::vector<bvh_node>& nodes, std::vector<bvh_primitive>& primitives) {
  // prepare to build nodes
  nodes.clear();
  nodes.reserve(primitives.size() * 2);

  // queue up first node
  auto queue = std::deque<vec3i>{{0, 0, (int)primitives.size()}};
  nodes.emplace_back();

  // create nodes until the queue is empty
  while (!queue.empty()) {
    // grab node to work on
    auto next = queue.front();
    queue.pop_front();
    auto nodeid = next.x, start = next.y, end = next.z;

    // grab node
    auto& node = nodes[nodeid];

    // compute bounds
    node.bbox = invalidb3f;
    for (auto i = start; i < end; i++)
      node.bbox = merge(node.bbox, primitives[i].bbox);

    // split into two children
    if (end - start > bvh_max_prims) {
      // get split
      auto [mid, axis] = split_middle(primitives, start, end);

      // make an internal node
      node.internal = true;
      node.axis     = axis;
      node.num      = 2;
      node.start    = (int)nodes.size();
      nodes.emplace_back();
      nodes.emplace_back();
      queue.push_back({node.start + 0, start, mid});
      queue.push_back({node.start + 1, mid, end});
    } else {
      // Make a leaf node
      node.internal = false;
      node.num      = end - start;
      node.start    = start;
    }
  }

  // cleanup
  nodes.shrink_to_fit();
}

static void init_bvh(ptr::shape* shape, const trace_params& params) {
  // build primitives
  auto primitives = std::vector<bvh_primitive>{};
  if (!shape->points.empty()) {
    for (auto idx = 0; idx < shape->points.size(); idx++) {
      auto& p             = shape->points[idx];
      auto& primitive     = primitives.emplace_back();
      primitive.bbox      = point_bounds(shape->positions[p], shape->radius[p]);
      primitive.center    = center(primitive.bbox);
      primitive.primitive = idx;
    }
  } else if (!shape->lines.empty()) {
    for (auto idx = 0; idx < shape->lines.size(); idx++) {
      auto& l         = shape->lines[idx];
      auto& primitive = primitives.emplace_back();
      primitive.bbox = line_bounds(shape->positions[l.x], shape->positions[l.y],
          shape->radius[l.x], shape->radius[l.y]);
      primitive.center    = center(primitive.bbox);
      primitive.primitive = idx;
    }
  } else if (!shape->triangles.empty()) {
    for (auto idx = 0; idx < shape->triangles.size(); idx++) {
      auto& primitive = primitives.emplace_back();
      auto& t         = shape->triangles[idx];
      primitive.bbox  = triangle_bounds(
          shape->positions[t.x], shape->positions[t.y], shape->positions[t.z]);
      primitive.center    = center(primitive.bbox);
      primitive.primitive = idx;
    }
  }

  // build nodes
  if (shape->bvh) delete shape->bvh;
  shape->bvh = new bvh_tree{};
  build_bvh(shape->bvh->nodes, primitives);

  // set bvh primitives
  shape->bvh->primitives.reserve(primitives.size());
  for (auto& primitive : primitives) {
    shape->bvh->primitives.push_back(primitive.primitive);
  }
}

void init_bvh(ptr::scene* scene, const trace_params& params,
    progress_callback progress_cb) {
  // handle progress
  auto progress = vec2i{0, 1 + (int)scene->shapes.size()};

  // shapes
  for (auto idx = 0; idx < scene->shapes.size(); idx++) {
    if (progress_cb) progress_cb("build shape bvh", progress.x++, progress.y);
    init_bvh(scene->shapes[idx], params);
  }

  // handle progress
  if (progress_cb) progress_cb("build scene bvh", progress.x++, progress.y);

  // instance bboxes
  auto primitives = std::vector<bvh_primitive>{};
  auto object_id  = 0;
  for (auto object : scene->objects) {
    auto& primitive = primitives.emplace_back();
    primitive.bbox =
        object->shape->bvh->nodes.empty()
            ? invalidb3f
            : transform_bbox(object->frame, object->shape->bvh->nodes[0].bbox);
    primitive.center    = center(primitive.bbox);
    primitive.primitive = object_id++;
  }

  // build nodes
  if (scene->bvh) delete scene->bvh;
  scene->bvh = new bvh_tree{};
  build_bvh(scene->bvh->nodes, primitives);

  // set bvh primitives
  scene->bvh->primitives.reserve(primitives.size());
  for (auto& primitive : primitives) {
    scene->bvh->primitives.push_back(primitive.primitive);
  }

  // handle progress
  if (progress_cb) progress_cb("build bvh", progress.x++, progress.y);
}

// Intersect ray with a bvh->
static bool intersect_shape_bvh(ptr::shape* shape, const ray3f& ray_,
    int& element, vec2f& uv, float& distance, bool find_any) {
  // get bvh and shape pointers for fast access
  auto bvh = shape->bvh;

  // check empty
  if (bvh->nodes.empty()) return false;

  // node stack
  int  node_stack[128];
  auto node_cur          = 0;
  node_stack[node_cur++] = 0;

  // shared variables
  auto hit = false;

  // copy ray to modify it
  auto ray = ray_;

  // prepare ray for fast queries
  auto ray_dinv  = vec3f{1 / ray.d.x, 1 / ray.d.y, 1 / ray.d.z};
  auto ray_dsign = vec3i{(ray_dinv.x < 0) ? 1 : 0, (ray_dinv.y < 0) ? 1 : 0,
      (ray_dinv.z < 0) ? 1 : 0};

  // walking stack
  while (node_cur) {
    // grab node
    auto& node = bvh->nodes[node_stack[--node_cur]];

    // intersect bbox
    // if (!intersect_bbox(ray, ray_dinv, ray_dsign, node.bbox)) continue;
    if (!intersect_bbox(ray, ray_dinv, node.bbox)) continue;

    // intersect node, switching based on node type
    // for each type, iterate over the the primitive list
    if (node.internal) {
      // for internal nodes, attempts to proceed along the
      // split axis from smallest to largest nodes
      if (ray_dsign[node.axis]) {
        node_stack[node_cur++] = node.start + 0;
        node_stack[node_cur++] = node.start + 1;
      } else {
        node_stack[node_cur++] = node.start + 1;
        node_stack[node_cur++] = node.start + 0;
      }
    } else if (!shape->points.empty()) {
      for (auto idx = node.start; idx < node.start + node.num; idx++) {
        auto& p = shape->points[shape->bvh->primitives[idx]];
        if (intersect_point(
                ray, shape->positions[p], shape->radius[p], uv, distance)) {
          hit      = true;
          element  = shape->bvh->primitives[idx];
          ray.tmax = distance;
        }
      }
    } else if (!shape->lines.empty()) {
      for (auto idx = node.start; idx < node.start + node.num; idx++) {
        auto& l = shape->lines[shape->bvh->primitives[idx]];
        if (intersect_line(ray, shape->positions[l.x], shape->positions[l.y],
                shape->radius[l.x], shape->radius[l.y], uv, distance)) {
          hit      = true;
          element  = shape->bvh->primitives[idx];
          ray.tmax = distance;
        }
      }
    } else if (!shape->triangles.empty()) {
      for (auto idx = node.start; idx < node.start + node.num; idx++) {
        auto& t = shape->triangles[shape->bvh->primitives[idx]];
        if (intersect_triangle(ray, shape->positions[t.x],
                shape->positions[t.y], shape->positions[t.z], uv, distance)) {
          hit      = true;
          element  = shape->bvh->primitives[idx];
          ray.tmax = distance;
        }
      }
    }

    // check for early exit
    if (find_any && hit) return hit;
  }

  return hit;
}

// Intersect ray with a bvh->
static bool intersect_scene_bvh(const ptr::scene* scene, const ray3f& ray_,
    int& object, int& element, vec2f& uv, float& distance, bool find_any,
    bool non_rigid_frames) {
  // get bvh and scene pointers for fast access
  auto bvh = scene->bvh;

  // check empty
  if (bvh->nodes.empty()) return false;

  // node stack
  int  node_stack[128];
  auto node_cur          = 0;
  node_stack[node_cur++] = 0;

  // shared variables
  auto hit = false;

  // copy ray to modify it
  auto ray = ray_;

  // prepare ray for fast queries
  auto ray_dinv  = vec3f{1 / ray.d.x, 1 / ray.d.y, 1 / ray.d.z};
  auto ray_dsign = vec3i{(ray_dinv.x < 0) ? 1 : 0, (ray_dinv.y < 0) ? 1 : 0,
      (ray_dinv.z < 0) ? 1 : 0};

  // walking stack
  while (node_cur) {
    // grab node
    auto& node = bvh->nodes[node_stack[--node_cur]];

    // intersect bbox
    // if (!intersect_bbox(ray, ray_dinv, ray_dsign, node.bbox)) continue;
    if (!intersect_bbox(ray, ray_dinv, node.bbox)) continue;

    // intersect node, switching based on node type
    // for each type, iterate over the the primitive list
    if (node.internal) {
      // for internal nodes, attempts to proceed along the
      // split axis from smallest to largest nodes
      if (ray_dsign[node.axis]) {
        node_stack[node_cur++] = node.start + 0;
        node_stack[node_cur++] = node.start + 1;
      } else {
        node_stack[node_cur++] = node.start + 1;
        node_stack[node_cur++] = node.start + 0;
      }
    } else {
      for (auto idx = node.start; idx < node.start + node.num; idx++) {
        auto object_ = scene->objects[scene->bvh->primitives[idx]];
        auto inv_ray = transform_ray(
            inverse(object_->frame, non_rigid_frames), ray);
        if (intersect_shape_bvh(
                object_->shape, inv_ray, element, uv, distance, find_any)) {
          hit      = true;
          object   = scene->bvh->primitives[idx];
          ray.tmax = distance;
        }
      }
    }

    // check for early exit
    if (find_any && hit) return hit;
  }

  return hit;
}

// Intersect ray with a bvh->
static bool intersect_instance_bvh(const ptr::object* object, const ray3f& ray,
    int& element, vec2f& uv, float& distance, bool find_any,
    bool non_rigid_frames) {
  auto inv_ray = transform_ray(inverse(object->frame, non_rigid_frames), ray);
  return intersect_shape_bvh(
      object->shape, inv_ray, element, uv, distance, find_any);
}

intersection3f intersect_scene_bvh(const ptr::scene* scene, const ray3f& ray,
    bool find_any, bool non_rigid_frames) {
  auto intersection = intersection3f{};
  intersection.hit  = intersect_scene_bvh(scene, ray, intersection.object,
      intersection.element, intersection.uv, intersection.distance, find_any,
      non_rigid_frames);
  return intersection;
}
intersection3f intersect_instance_bvh(const ptr::object* object,
    const ray3f& ray, bool find_any, bool non_rigid_frames) {
  auto intersection = intersection3f{};
  intersection.hit  = intersect_instance_bvh(object, ray, intersection.element,
      intersection.uv, intersection.distance, find_any, non_rigid_frames);
  return intersection;
}

}  // namespace yocto::pathtrace

// -----------------------------------------------------------------------------
// IMPLEMENTATION FOR PATH TRACING
// -----------------------------------------------------------------------------
namespace yocto::pathtrace {

// Evaluate emission
static vec3f eval_emission(
    const vec3f& emission, const vec3f& normal, const vec3f& outgoing) {
  return emission;
}

// Evaluates/sample the BRDF scaled by the cosine of the incoming direction.
static vec3f eval_brdfcos(const ptr::brdf& brdf, const vec3f& normal,
    const vec3f& outgoing, const vec3f& incoming) {
  if (!brdf.roughness) return zero3f;
  // YOUR CODE GOES HERE ------------------------------------------------------
  auto brdfcos = zero3f;
  if (brdf.diffuse)
    brdfcos += brdf.diffuse * eval_diffuse_reflection(normal, outgoing, incoming);
  if (brdf.specular)
    brdfcos += brdf.specular * eval_microfacet_reflection(brdf.ior,
                                brdf.roughness, normal, outgoing, incoming);
  if (brdf.metal)
    brdfcos += brdf.metal * eval_microfacet_reflection(brdf.meta, brdf.metak,
                                        brdf.roughness, normal, outgoing, incoming);
  if (brdf.transmission)
    brdfcos += brdf.transmission * eval_microfacet_transmission(brdf.ior,
                                    brdf.roughness, normal, outgoing, incoming);
  return brdfcos;
}

static vec3f eval_delta(const ptr::brdf& brdf, const vec3f& normal,
    const vec3f& outgoing, const vec3f& incoming) {
  if (brdf.roughness) return zero3f;
  // YOUR CODE GOES HERE ------------------------------------------------------
  return {};
}

// Picks a direction based on the BRDF
static vec3f sample_brdfcos(const ptr::brdf& brdf, const vec3f& normal,
    const vec3f& outgoing, float rnl, const vec2f& rn) {
  if (!brdf.roughness) return zero3f;
  // YOUR CODE GOES HERE ------------------------------------------------------
  auto cdf = 0.0f;
  if (brdf.diffuse_pdf) {
    cdf += brdf.diffuse_pdf;
    if (rnl < cdf) 
      return sample_diffuse_reflection(normal, outgoing, rn); 
  }
  if (brdf.specular_pdf) {
    cdf += brdf.specular_pdf;
    if (rnl < cdf) 
      return sample_microfacet_reflection(brdf.ior, brdf.roughness, normal, outgoing, rn); 
  }
  if (brdf.metal_pdf) {
    cdf += brdf.metal_pdf;
    if (rnl < cdf) 
      return sample_microfacet_reflection(brdf.ior, brdf.roughness, normal, outgoing, rn); 
  }
  if (brdf.transmission_pdf) {
    cdf += brdf.transmission_pdf;
    if (rnl < cdf)
      return sample_microfacet_transmission(brdf.ior, brdf.roughness, normal, outgoing, rn);
  }
  return zero3f;
}

static vec3f sample_delta(const ptr::brdf& brdf, const vec3f& normal,
    const vec3f& outgoing, float rnl) {
  if (brdf.roughness) return zero3f;
  // YOUR CODE GOES HERE ------------------------------------------------------
  return {};
}

// Compute the weight for sampling the BRDF
static float sample_brdfcos_pdf(const ptr::brdf& brdf, const vec3f& normal,
    const vec3f& outgoing, const vec3f& incoming) {
  if (!brdf.roughness) return 0;
  // YOUR CODE GOES HERE ------------------------------------------------------
  auto pdf = 0.0f;
  if (brdf.diffuse_pdf) {
    pdf += brdf.diffuse_pdf*sample_diffuse_reflection_pdf(normal,outgoing,incoming); 
  }
  if (brdf.specular_pdf) {
    pdf += brdf.specular_pdf*sample_microfacet_reflection_pdf(brdf.ior,brdf.roughness,normal,outgoing,incoming);
  }
  if (brdf.metal_pdf) {
    pdf += brdf.metal_pdf*sample_microfacet_reflection_pdf(brdf.ior,brdf.roughness,normal,outgoing,incoming);
  }
  if (brdf.transmission_pdf) {
    pdf += brdf.transmission_pdf*
            sample_microfacet_transmission_pdf(brdf.ior,brdf.roughness,normal,outgoing,incoming);
  }
  return pdf;
}

static float sample_delta_pdf(const ptr::brdf& brdf, const vec3f& normal,
    const vec3f& outgoing, const vec3f& incoming) {
  if (brdf.roughness) return 0;
  // YOUR CODE GOES HERE ------------------------------------------------------
  return {};
}

// Sample lights wrt solid angle
static vec3f sample_lights(const ptr::scene* scene, const vec3f& position,
    float rl, float rel, const vec2f& ruv) {
  // YOUR CODE GOES HERE ------------------------------------------------------
  return {};
}

// Sample lights pdf
static float sample_lights_pdf(
    const ptr::scene* scene, const vec3f& position, const vec3f& direction) {
  // YOUR CODE GOES HERE ------------------------------------------------------
  return {};
}

// Path tracing.
static vec4f trace_path(const ptr::scene* scene, const ray3f& ray_,
    rng_state& rng, const trace_params& params) {
  // YOUR CODE GOES HERE ------------------------------------------------------
  return {};
}

// Recursive path tracing.
static vec4f trace_naive(const ptr::scene* scene, const ray3f& ray_,
    rng_state& rng, const trace_params& params) {
  // YOUR CODE GOES HERE ------------------------------------------------------
  auto l = vec4f{0}, w = vec4f{1};
  ray3f ray = ray_;
  for(auto bounce : yocto::common::range(params.bounces)) {

    auto isec = intersect_scene_bvh(scene, ray);
    if(!isec.hit) {
      vec4f env = {eval_environment(scene, ray), 1};
      l += w * env; 
      break; 
    }
    auto object   = scene->objects[isec.object];
    auto element  = isec.element;
    auto uv       = isec.uv;
    // outgoing
    auto outgoing = -ray.d;

    // auto [p, n] = eval_point(scene, isec, ray); // eval pos, norm
    auto p        = eval_position(object, element, uv);
    auto normal   = eval_shading_normal(object, element, uv, outgoing);
    
    // auto [e, f] = eval_material(isec);   // eval emission, bsdf
    auto f       = eval_brdf(object, element, uv, normal, outgoing);
    vec4f emission = {eval_emission(object, element, uv, normal, outgoing),1};
    l += w * emission;

    // float i = 1;
    
    // emission
    auto i = vec3f{0};
    if(!is_delta(f)) { // sample smooth brdfs (fold cos into f)
      i = sample_brdfcos(f,normal,outgoing, rand1f(rng),rand2f(rng)); // incoming
      w *= {eval_brdfcos(f,normal,outgoing, i) / sample_brdfcos_pdf(f,normal,outgoing,i),1};
    } else {
      // sample sharp brdfs
      i = sample_delta(f,normal,outgoing,rand1f(rng));
      // incoming
      w *= {eval_delta(f,normal,outgoing,i) / sample_delta_pdf(f,normal,outgoing,i),1};
    }

    if(rand1f(rng) >= min(1.f,max(w))) break; // russian roulette

    w *= 1 / min(1.f,max(w));

    // update weight
    ray = {p, i};  // recurse
  }
  return l;
}

// Eyelight for quick previewing.
static vec4f trace_eyelight(const ptr::scene* scene, const ray3f& ray_,
    rng_state& rng, const trace_params& params) {
  // initialize
  auto radiance = zero3f;
  auto weight   = vec3f{1, 1, 1};
  auto ray      = ray_;
  auto hit      = false;

  // trace  path
  for (auto bounce = 0; bounce < max(params.bounces, 4); bounce++) {
    // intersect next point
    auto intersection = intersect_scene_bvh(scene, ray);
    if (!intersection.hit) {
      radiance += weight * eval_environment(scene, ray);
      break;
    }

    // prepare shading point
    auto outgoing = -ray.d;
    auto object   = scene->objects[intersection.object];
    auto element  = intersection.element;
    auto uv       = intersection.uv;
    auto position = eval_position(object, element, uv);
    auto normal   = eval_shading_normal(object, element, uv, outgoing);
    auto emission = eval_emission(object, element, uv, normal, outgoing);
    auto brdf     = eval_brdf(object, element, uv, normal, outgoing);

    // handle opacity
    if (brdf.opacity < 1 && rand1f(rng) >= brdf.opacity) {
      ray = {position + ray.d * 1e-2f, ray.d};
      bounce -= 1;
      continue;
    }
    hit = true;

    // accumulate emission
    radiance += weight * eval_emission(emission, normal, outgoing);

    // brdf * light
    auto incoming = outgoing;
    radiance += weight * pif * eval_brdfcos(brdf, normal, outgoing, incoming);

    // continue path
    if (!is_delta(brdf)) break;
    incoming = sample_delta(brdf, normal, outgoing, rand1f(rng));
    weight *= eval_delta(brdf, normal, outgoing, incoming) /
              sample_delta_pdf(brdf, normal, outgoing, incoming);
    if (weight == zero3f || !isfinite(weight)) break;

    // setup next iteration
    ray = {position, incoming};
  }

  return {radiance, hit ? 1.0f : 0.0f};
}

// Trace a single ray from the camera using the given algorithm.
using shader_func = vec4f (*)(const ptr::scene* scene, const ray3f& ray,
    rng_state& rng, const trace_params& params);
static shader_func get_trace_shader_func(const trace_params& params) {
  switch (params.shader) {
    case shader_type::naive: return trace_naive;
    case shader_type::path: return trace_path;
    case shader_type::eyelight: return trace_eyelight;
    default: {
      throw std::runtime_error("sampler unknown");
      return nullptr;
    }
  }
}

// Trace a block of samples
vec4f trace_sample(ptr::state* state, const ptr::scene* scene,
    const ptr::camera* camera, const vec2i& ij, const trace_params& params) {
  auto  shader = get_trace_shader_func(params);
  auto& pixel  = state->pixels[ij];
  auto  ray    = sample_camera(
      camera, ij, state->pixels.size(), rand2f(pixel.rng), rand2f(pixel.rng));
  auto shaded = shader(scene, ray, pixel.rng, params);
  if (!isfinite(xyz(shaded))) xyz(shaded) = zero3f;
  if (max(xyz(shaded)) > params.clamp)
    xyz(shaded) = xyz(shaded) * (params.clamp / max(xyz(shaded)));
  pixel.accumulated += shaded;
  pixel.samples += 1;
  return pixel.accumulated / pixel.samples;
}

// Forward declaration
ptr::light* add_light(ptr::scene* scene);

// Init trace lights
void init_lights(ptr::scene* scene, const trace_params& params,
    progress_callback progress_cb) {
  // handle progress
  auto progress = vec2i{0, 1};
  if (progress_cb) progress_cb("build light", progress.x++, progress.y);

  for (auto light : scene->lights) delete light;
  scene->lights.clear();

  for (auto object : scene->objects) {
    if (object->material->emission == zero3f) continue;
    auto shape = object->shape;
    if (shape->triangles.empty()) continue;
    if (progress_cb) progress_cb("build light", progress.x++, ++progress.y);
    auto light    = add_light(scene);
    light->object = object;
    light->cdf    = std::vector<float>(shape->triangles.size());
    for (auto idx = 0; idx < light->cdf.size(); idx++) {
      auto& t         = shape->triangles[idx];
      light->cdf[idx] = triangle_area(
          shape->positions[t.x], shape->positions[t.y], shape->positions[t.z]);
      if (idx) light->cdf[idx] += light->cdf[idx - 1];
    }
  }
  for (auto environment : scene->environments) {
    if (environment->emission == zero3f) continue;
    if (progress_cb) progress_cb("build light", progress.x++, ++progress.y);
    auto light         = add_light(scene);
    light->environment = environment;
    if (environment->emission_tex) {
      auto texture = environment->emission_tex;
      auto size    = texture_size(texture);
      light->cdf   = std::vector<float>(size.x * size.y);
      for (auto i = 0; i < light->cdf.size(); i++) {
        auto ij       = vec2i{i % size.x, i / size.x};
        auto th       = (ij.y + 0.5f) * pif / size.y;
        auto value    = lookup_texture(texture, ij);
        light->cdf[i] = max(value) * sin(th);
        if (i) light->cdf[i] += light->cdf[i - 1];
      }
    }
  }

  // handle progress
  if (progress_cb) progress_cb("build light", progress.x++, progress.y);
}

// Init a sequence of random number generators.
void init_state(ptr::state* state, const ptr::scene* scene,
    const ptr::camera* camera, const trace_params& params) {
  auto image_size =
      (camera->film.x > camera->film.y)
          ? vec2i{params.resolution,
                (int)round(params.resolution * camera->film.y / camera->film.x)}
          : vec2i{
                (int)round(params.resolution * camera->film.x / camera->film.y),
                params.resolution};
  state->pixels.assign(image_size, pixel{});
  state->render.assign(image_size, zero4f);
  auto rng = make_rng(1301081);
  for (auto& pixel : state->pixels) {
    pixel.rng = make_rng(params.seed, rand1i(rng, 1 << 31) / 2 + 1);
  }
}

using std::atomic;
using std::deque;
using std::future;

// Simple parallel for used since our target platforms do not yet support
// parallel algorithms. `Func` takes the integer index.
template <typename Func>
inline void parallel_for(const vec2i& size, Func&& func) {
  auto             futures  = std::vector<std::future<void>>{};
  auto             nthreads = std::thread::hardware_concurrency();
  std::atomic<int> next_idx(0);
  for (auto thread_id = 0; thread_id < nthreads; thread_id++) {
    futures.emplace_back(
        std::async(std::launch::async, [&func, &next_idx, size]() {
          while (true) {
            auto j = next_idx.fetch_add(1);
            if (j >= size.y) break;
            for (auto i = 0; i < size.x; i++) func({i, j});
          }
        }));
  }
  for (auto& f : futures) f.get();
}
template <typename Func>
inline void parallel_for(
    const vec2i& size, std::atomic<bool>* stop, Func&& func) {
  auto             futures  = std::vector<std::future<void>>{};
  auto             nthreads = std::thread::hardware_concurrency();
  std::atomic<int> next_idx(0);
  for (auto thread_id = 0; thread_id < nthreads; thread_id++) {
    futures.emplace_back(
        std::async(std::launch::async, [&func, &next_idx, size, stop]() {
          while (true) {
            if (stop && *stop) return;
            auto j = next_idx.fetch_add(1);
            if (j >= size.y) break;
            for (auto i = 0; i < size.x; i++) func({i, j});
          }
        }));
  }
  for (auto& f : futures) f.get();
}

// Progressively compute an image by calling trace_samples multiple times.
void trace_samples(ptr::state* state, const ptr::scene* scene,
    const ptr::camera* camera, const trace_params& params) {
  if (params.noparallel) {
    for (auto j = 0; j < state->render.size().y; j++) {
      for (auto i = 0; i < state->render.size().x; i++) {
        state->render[{i, j}] = trace_sample(
            state, scene, camera, {i, j}, params);
      }
    }
  } else {
    parallel_for(
        state->render.size(), [state, scene, camera, &params](const vec2i& ij) {
          state->render[ij] = trace_sample(state, scene, camera, ij, params);
        });
  }
}

void trace_samples(ptr::state* state, const ptr::scene* scene,
    const ptr::camera* camera, const trace_params& params,
    std::atomic<bool>* stop) {
  if (params.noparallel) {
    for (auto j = 0; j < state->render.size().y; j++) {
      for (auto i = 0; i < state->render.size().x; i++) {
        if (stop && stop) return;
        state->render[{i, j}] = trace_sample(
            state, scene, camera, {i, j}, params);
      }
    }
  } else {
    parallel_for(state->render.size(), stop,
        [state, scene, camera, &params](const vec2i& ij) {
          state->render[ij] = trace_sample(state, scene, camera, ij, params);
        });
  }
}

}  // namespace yocto::pathtrace

// -----------------------------------------------------------------------------
// SCENE CREATION
// -----------------------------------------------------------------------------
namespace yocto::pathtrace {

// cleanup
shape::~shape() {
  if (bvh) delete bvh;
}

// cleanup
scene::~scene() {
  if (bvh) delete bvh;
  for (auto camera : cameras) delete camera;
  for (auto object : objects) delete object;
  for (auto shape : shapes) delete shape;
  for (auto material : materials) delete material;
  for (auto texture : textures) delete texture;
  for (auto environment : environments) delete environment;
  for (auto light : lights) delete light;
}

// Add element
ptr::camera* add_camera(ptr::scene* scene) {
  return scene->cameras.emplace_back(new camera{});
}
ptr::texture* add_texture(ptr::scene* scene) {
  return scene->textures.emplace_back(new texture{});
}
ptr::shape* add_shape(ptr::scene* scene) {
  return scene->shapes.emplace_back(new shape{});
}
ptr::material* add_material(ptr::scene* scene) {
  return scene->materials.emplace_back(new material{});
}
ptr::object* add_object(ptr::scene* scene) {
  return scene->objects.emplace_back(new object{});
}
ptr::environment* add_environment(ptr::scene* scene) {
  return scene->environments.emplace_back(new environment{});
}
ptr::light* add_light(ptr::scene* scene) {
  return scene->lights.emplace_back(new light{});
}

// Set cameras
void set_frame(ptr::camera* camera, const frame3f& frame) {
  camera->frame = frame;
}
void set_lens(ptr::camera* camera, float lens, float aspect, float film) {
  camera->lens = lens;
  camera->film = aspect >= 1 ? vec2f{film, film / aspect}
                             : vec2f{film * aspect, film};
}
void set_focus(ptr::camera* camera, float aperture, float focus) {
  camera->aperture = aperture;
  camera->focus    = focus;
}

// Add texture
void set_texture(ptr::texture* texture, const img::image<vec3b>& img) {
  texture->colorb  = img;
  texture->colorf  = {};
  texture->scalarb = {};
  texture->scalarf = {};
}
void set_texture(ptr::texture* texture, const img::image<vec3f>& img) {
  texture->colorb  = {};
  texture->colorf  = img;
  texture->scalarb = {};
  texture->scalarf = {};
}
void set_texture(ptr::texture* texture, const img::image<byte>& img) {
  texture->colorb  = {};
  texture->colorf  = {};
  texture->scalarb = img;
  texture->scalarf = {};
}
void set_texture(ptr::texture* texture, const img::image<float>& img) {
  texture->colorb  = {};
  texture->colorf  = {};
  texture->scalarb = {};
  texture->scalarf = img;
}

// Add shape
void set_points(ptr::shape* shape, const std::vector<int>& points) {
  shape->points = points;
}
void set_lines(ptr::shape* shape, const std::vector<vec2i>& lines) {
  shape->lines = lines;
}
void set_triangles(ptr::shape* shape, const std::vector<vec3i>& triangles) {
  shape->triangles = triangles;
}
void set_positions(ptr::shape* shape, const std::vector<vec3f>& positions) {
  shape->positions = positions;
}
void set_normals(ptr::shape* shape, const std::vector<vec3f>& normals) {
  shape->normals = normals;
}
void set_texcoords(ptr::shape* shape, const std::vector<vec2f>& texcoords) {
  shape->texcoords = texcoords;
}
void set_radius(ptr::shape* shape, const std::vector<float>& radius) {
  shape->radius = radius;
}

// Add object
void set_frame(ptr::object* object, const frame3f& frame) {
  object->frame = frame;
}
void set_shape(ptr::object* object, ptr::shape* shape) {
  object->shape = shape;
}
void set_material(ptr::object* object, ptr::material* material) {
  object->material = material;
}

// Add material
void set_emission(ptr::material* material, const vec3f& emission,
    ptr::texture* emission_tex) {
  material->emission     = emission;
  material->emission_tex = emission_tex;
}
void set_color(
    ptr::material* material, const vec3f& color, ptr::texture* color_tex) {
  material->color     = color;
  material->color_tex = color_tex;
}
void set_specular(
    ptr::material* material, float specular, ptr::texture* specular_tex) {
  material->specular     = specular;
  material->specular_tex = specular_tex;
}
void set_metallic(
    ptr::material* material, float metallic, ptr::texture* metallic_tex) {
  material->metallic     = metallic;
  material->metallic_tex = metallic_tex;
}
void set_ior(ptr::material* material, float ior) { material->ior = ior; }
void set_transmission(ptr::material* material, float transmission, bool thin,
    float trdepth, ptr::texture* transmission_tex) {
  material->transmission     = transmission;
  material->thin             = thin;
  material->trdepth          = trdepth;
  material->transmission_tex = transmission_tex;
}
void set_thin(ptr::material* material, bool thin) { material->thin = thin; }
void set_roughness(
    ptr::material* material, float roughness, ptr::texture* roughness_tex) {
  material->roughness     = roughness;
  material->roughness_tex = roughness_tex;
}
void set_opacity(
    ptr::material* material, float opacity, ptr::texture* opacity_tex) {
  material->opacity     = opacity;
  material->opacity_tex = opacity_tex;
}
void set_scattering(ptr::material* material, const vec3f& scattering,
    float scanisotropy, ptr::texture* scattering_tex) {
  material->scattering     = scattering;
  material->scanisotropy   = scanisotropy;
  material->scattering_tex = scattering_tex;
}

// Add environment
void set_frame(ptr::environment* environment, const frame3f& frame) {
  environment->frame = frame;
}
void set_emission(ptr::environment* environment, const vec3f& emission,
    ptr::texture* emission_tex) {
  environment->emission     = emission;
  environment->emission_tex = emission_tex;
}

}  // namespace yocto::pathtrace
