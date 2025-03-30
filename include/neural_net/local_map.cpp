#include "local_map.h"
#include "kaolin_wisp_cpp/spc_ops/spc_ops.h"
#include "llog/llog.h"
#include "neural_net/encodings/encodings.h"
#include "utils/ply_utils/ply_utils_torch.h"

#include "params/params.h"
#include "utils/coordinates.h"
#include "utils/utils.h"
#include <kaolin/csrc/render/spc/raytrace.h>

#include "mesher/cumcubes/include/cumcubes.hpp"
using namespace std;

LocalMap::LocalMap(const torch::Tensor &_pos_W_M, float _x_min, float _x_max,
                   float _y_min, float _y_max, float _z_min, float _z_max)
    : SubMap(_pos_W_M, _x_min, _x_max, _y_min, _y_max, _z_min, _z_max) {
  p_encoding_map_ = std::shared_ptr<EncodingMap>(new_map(pos_W_M_));

  p_mesher_ = std::make_shared<mesher::Mesher>();

  if (k_dir_embedding_degree > 0 && k_dir_embedding_degree <= 4) {
    p_dir_encoder_tcnn_ = std::make_shared<SHEncoding>(k_dir_embedding_degree);
    dir_mask = torch::zeros({1, (long)p_dir_encoder_tcnn_->get_out_dim()},
                            torch::kFloat32)
                   .to(k_device);
  }
  int k_strc_dim = 2;

  int encode_feat_dim = p_encoding_map_->p_encoder_tcnn_->get_out_dim();
  int color_encode_feat_dim =
      p_encoding_map_->p_color_encoder_tcnn_->get_out_dim();

  nlohmann::json network_config = {
      {"otype", "FullyFusedMLP"},           {"activation", "ReLU"},
      {"output_activation", "None"},        {"n_neurons", k_hidden_dim},
      {"n_hidden_layers", k_geo_num_layer},
  };

  p_decoder_tcnn_ = std::make_shared<TCNNNetwork>(encode_feat_dim, k_strc_dim,
                                                  network_config, "decoder");

  p_decoder_tcnn_->params_ = register_parameter(p_decoder_tcnn_->name_,
                                                p_decoder_tcnn_->params_, true);

  nlohmann::json rgb_network_config = {
      {"otype", "FullyFusedMLP"},
      {"activation", "ReLU"},
      {"output_activation", "Sigmoid"},
      {"n_neurons", k_color_hidden_dim},
      {"n_hidden_layers", k_color_num_layer},
  };

  if (p_dir_encoder_tcnn_) {
    p_color_decoder_tcnn_ = std::make_shared<TCNNNetwork>(
        color_encode_feat_dim + p_dir_encoder_tcnn_->get_out_dim(), 3,
        rgb_network_config, "color_decoder");
  } else {
    p_color_decoder_tcnn_ = std::make_shared<TCNNNetwork>(
        color_encode_feat_dim, 3, rgb_network_config, "color_decoder");
  }
  p_color_decoder_tcnn_->params_ = register_parameter(
      p_color_decoder_tcnn_->name_, p_color_decoder_tcnn_->params_, true);
}

EncodingMap *LocalMap::new_map(const torch::Tensor &_pos_W_M) {
  auto p_submap = new EncodingMap(_pos_W_M, "local_map", k_x_min, k_x_max,
                                  k_y_min, k_y_max, k_z_min, k_z_max);

  p_submap->p_encoder_tcnn_->params_ =
      register_parameter(p_submap->p_encoder_tcnn_->name_,
                         p_submap->p_encoder_tcnn_->params_, true);
  p_submap->p_color_encoder_tcnn_->params_ =
      register_parameter(p_submap->p_color_encoder_tcnn_->name_,
                         p_submap->p_color_encoder_tcnn_->params_, true);

  return p_submap;
}

torch::Tensor LocalMap::get_feat(const torch::Tensor &xyz,
                                 const int &encoding_type,
                                 const bool &normalized) {
  auto xyz_feat = p_encoding_map_->encoding(xyz, encoding_type, normalized);
  return xyz_feat;
}

std::vector<torch::Tensor> LocalMap::get_sdf(const torch::Tensor &xyz,
                                             const bool &normalized) {
  static auto p_t_get_feat = llog::CreateTimer("   get_feat");
  p_t_get_feat->tic();
  torch::Tensor xyz_feat = get_feat(xyz, 0, normalized);
  p_t_get_feat->toc_sum();

  static auto p_t_forward = llog::CreateTimer("   forward");
  p_t_forward->tic();

  torch::Tensor xyz_attr = p_decoder_tcnn_->forward(xyz_feat);

  p_t_forward->toc_sum();
  std::vector<torch::Tensor> split_results;
  split_results = torch::split(xyz_attr, {1, 1}, -1); // sdf, isigma

  static auto softplus =
      torch::nn::Softplus(torch::nn::SoftplusOptions().beta(100));
  return {split_results[0], (1 + softplus(split_results[1])) * k_bce_isigma};
}

std::vector<torch::Tensor> LocalMap::get_gradient(const torch::Tensor &_xyz,
                                                  const float &delta,
                                                  torch::Tensor _sdf,
                                                  bool _heissian) {
  // [6,1,3]
  auto offsets = torch::tensor({{{delta, 0.0f, 0.0f}},
                                {{-delta, 0.0f, 0.0f}},
                                {{0.0f, delta, 0.0f}},
                                {{0.0f, -delta, 0.0f}},
                                {{0.0f, 0.0f, delta}},
                                {{0.0f, 0.0f, -delta}}},
                               _xyz.options().requires_grad(false));

  // [6,n,3]
  torch::Tensor points = _xyz.unsqueeze(0) + offsets;
  // [6, n, 1]
  auto points_sdf = get_sdf(points.view({-1, 3}))[0].view({6, _xyz.size(0), 1});
  // [n,3]
  auto inv_delta = 1.0 / delta;
  auto gradient = 0.5 * inv_delta *
                  torch::cat({(points_sdf[0] - points_sdf[1]),
                              (points_sdf[2] - points_sdf[3]),
                              (points_sdf[4] - points_sdf[5])},
                             1);

  if (_heissian) {
    if (!_sdf.defined()) {
      _sdf = get_sdf(_xyz)[0];
    }
    // [n,3]
    auto hessian_coef = inv_delta * inv_delta;
    auto hessian = hessian_coef * (torch::cat({(points_sdf[0] + points_sdf[1]),
                                               (points_sdf[2] + points_sdf[3]),
                                               (points_sdf[4] + points_sdf[5])},
                                              1) -
                                   2 * _sdf);
    return {gradient, hessian};
  } else {
    return {gradient};
  }
}

std::vector<torch::Tensor> LocalMap::get_color(const torch::Tensor &xyz,
                                               const torch::Tensor &dir) {
  static auto p_t_get_feat = llog::CreateTimer("   get_color");
  p_t_get_feat->tic();
  torch::Tensor encoding = get_feat(xyz, 1);
  if (p_dir_encoder_tcnn_) {
    // SH encoding must be in the range [0, 1]
    auto normalized_dir = 0.5f * (dir + 1.0f);
    auto dir_encoding = p_dir_encoder_tcnn_->forward(normalized_dir);

    dir_encoding = dir_encoding * dir_mask;
    encoding = torch::cat({encoding, dir_encoding}, 1);
  }
  p_t_get_feat->toc_sum();

  static auto p_t_forward = llog::CreateTimer("   forward");
  p_t_forward->tic();

  auto rgb = p_color_decoder_tcnn_->forward(encoding);

  p_t_forward->toc_sum();
  return {rgb};
}

#ifdef ENABLE_ROS
void LocalMap::pub_mesh(const ros::Publisher &_mesh_pub,
                        const ros::Publisher &_mesh_color_pub,
                        const torch::Tensor &vertice, const torch::Tensor &face,
                        const torch::Tensor &color,
                        const std_msgs::Header &_header,
                        const std::string &_uuid) {
  if (_mesh_pub.getNumSubscribers() > 0) {
    mesh_msgs::MeshGeometry mesh;
    mesh_msgs::MeshVertexColors mesh_color;
    mesher::tensor_to_mesh(mesh, mesh_color, vertice.cpu(), face.cpu(),
                           color.cpu());
    mesh_msgs::MeshGeometryStamped mesh_stamped;
    mesh_stamped.header = _header;
    mesh_stamped.uuid = _uuid;
    mesh_stamped.mesh_geometry = mesh;
    _mesh_pub.publish(mesh_stamped);

    mesh_msgs::MeshVertexColorsStamped mesh_color_stamped;
    mesh_color_stamped.header = _header;
    mesh_color_stamped.uuid = _uuid;
    mesh_color_stamped.mesh_vertex_colors = mesh_color;
    _mesh_color_pub.publish(mesh_color_stamped);
  }
}

void LocalMap::meshing_(ros::Publisher &mesh_pub,
                        ros::Publisher &mesh_color_pub,
                        std_msgs::Header &header, float _res, bool _save,
                        const std::string &uuid) {
  c10::cuda::CUDACachingAllocator::emptyCache();
  torch::NoGradGuard no_grad;
  if (_save) {
    p_mesher_->vec_face_attr_.clear();
    p_mesher_->vec_face_.clear();
    p_mesher_->vec_vertice_.clear();
  }
  float x_center = pos_W_M_[0][0].item<float>();
  float y_center = pos_W_M_[0][1].item<float>();
  float z_center = pos_W_M_[0][2].item<float>();

  float x_min = xyz_min_M_margin_[0][0].item<float>();
  float x_max = xyz_max_M_margin_[0][0].item<float>();
  float x_scale = x_max - x_min;
  float x_res = x_scale / _res;

  float y_min = xyz_min_M_margin_[0][1].item<float>();
  float y_max = xyz_max_M_margin_[0][1].item<float>();
  float y_scale = y_max - y_min;
  float y_res = y_scale / _res;

  float z_min = xyz_min_M_margin_[0][2].item<float>();
  float z_max = xyz_max_M_margin_[0][2].item<float>();
  float z_scale = z_max - z_min;
  float z_res = z_scale / _res;
  float yz_res = y_res * z_res;

  int x_step = k_vis_batch_pt_num / yz_res + 1;
  int steps = x_res / x_step + 1;
  float step_size = x_step * _res;

  int count = 0;
  for (int i = 0; i < steps; ++i) {
    std::cout << "\rstep: " << i << " / " << steps;
    float start = i * step_size + x_min;
    float end = start + step_size;
    if (i == steps - 1) {
      end = end > x_max ? x_max : end;
    }
    if (end == start)
      break;

    std::vector<float> lower = {start + x_center, y_min + y_center,
                                z_min + z_center};
    torch::Tensor mesh_xyz = utils::meshgrid_3d(
        lower[0], end + x_center + _res, lower[1], y_max + y_center + _res,
        lower[2], z_max + z_center + _res, _res, k_device);
    long x_num = mesh_xyz.size(0);
    long y_num = mesh_xyz.size(1);
    long z_num = mesh_xyz.size(2);
    mesh_xyz = mesh_xyz.view({-1, 3});

    torch::Tensor mesh_mask = get_valid_mask(mesh_xyz);
    if (mesh_mask.sum().item<int>() == 0) {
      continue;
    }
    auto mask_xyz_sdf_results = get_sdf(mesh_xyz.index({mesh_mask}));
    torch::Tensor mask_xyz_sdf = mask_xyz_sdf_results[0];
    // make a extremely large value to represent the outside of the mesh
    // to push outward invisible floaters to boundary and filter them
    torch::Tensor mesh_sdf =
        torch::full({mesh_xyz.size(0), 1}, 1e-6f).to(k_device);
    mesh_sdf.index_put_({mesh_mask}, mask_xyz_sdf);
    mesh_sdf = mesh_sdf.view({x_num, y_num, z_num});

    std::vector<float> upper;
    upper.push_back(lower[0] + x_num * _res);
    upper.push_back(lower[1] + y_num * _res);
    upper.push_back(lower[2] + z_num * _res);
    auto mc_results = mc::marching_cubes(mesh_sdf, 0.0f, lower, upper);
    auto vertices_cu = mc_results[0];
    auto faces_cu = mc_results[1];

    // if (faces_cu.numel() == 0)
    //   continue;

    // // filter boundary artifacts
    // auto qpts = (vertices_cu / _res).floor().to(torch::kInt16);
    // auto qpts_neighbor = spc_ops::points_to_neighbors(qpts).view({-1, 3});
    // auto vertices_neighbor = qpts_neighbor.to(torch::kFloat32) * _res;
    // auto valid_mask = get_valid_mask(vertices_neighbor).view({-1,
    // 27}).all(1);

    // valid_mask = valid_mask.index({faces_cu.view(-1)}).view({-1, 3}).all(-1);
    // auto valid_idx = torch::nonzero(valid_mask).view({-1});
    // faces_cu = faces_cu.index_select(0, valid_idx);
    if (faces_cu.numel() == 0 || vertices_cu.numel() == 0)
      continue;

    auto normal_cu = get_gradient(vertices_cu, _res, {}, false)[0];

    normal_cu = torch::nn::functional::normalize(
        normal_cu, torch::nn::functional::NormalizeFuncOptions().dim(-1));
    torch::Tensor color_cu;
    switch (k_vis_attribute) {
    case 1:
      color_cu = ply_utils::cal_face_normal_color(normal_cu);
      break;
    case 2:
      color_cu = get_color(vertices_cu, -normal_cu)[0];
      break;
    }

    pub_mesh(mesh_pub, mesh_color_pub, vertices_cu, faces_cu, color_cu, header,
             uuid);

    if (_save) {
      faces_cu = faces_cu + count;
      count += vertices_cu.size(0);
      vertices_cu = coords::reset_world_system(vertices_cu, k_dataset_type);
      p_mesher_->vec_vertice_.emplace_back(vertices_cu.cpu());
      p_mesher_->vec_face_.emplace_back(faces_cu.cpu());
      color_cu = (color_cu * 255).to(torch::kUInt8);
      p_mesher_->vec_face_attr_.emplace_back(color_cu.cpu());
    }
  }
}
#endif

void LocalMap::meshing_(float _res, bool _save) {
  torch::NoGradGuard no_grad;
  if (_save) {
    p_mesher_->vec_face_attr_.clear();
    p_mesher_->vec_face_.clear();
    p_mesher_->vec_vertice_.clear();
  }
  float x_center = pos_W_M_[0][0].item<float>();
  float y_center = pos_W_M_[0][1].item<float>();
  float z_center = pos_W_M_[0][2].item<float>();

  float x_min = xyz_min_M_margin_[0][0].item<float>();
  float x_max = xyz_max_M_margin_[0][0].item<float>();
  float x_scale = x_max - x_min;
  float x_res = x_scale / _res;

  float y_min = xyz_min_M_margin_[0][1].item<float>();
  float y_max = xyz_max_M_margin_[0][1].item<float>();
  float y_scale = y_max - y_min;
  float y_res = y_scale / _res;

  float z_min = xyz_min_M_margin_[0][2].item<float>();
  float z_max = xyz_max_M_margin_[0][2].item<float>();
  float z_scale = z_max - z_min;
  float z_res = z_scale / _res;
  float yz_res = y_res * z_res;

  int x_step = k_vis_batch_pt_num / yz_res + 1;
  int steps = x_res / x_step + 1;
  float step_size = x_step * _res;

  int count = 0;
  for (int i = 0; i < steps; ++i) {
    float start = i * step_size + x_min;
    float end = start + step_size;
    if (i == steps - 1) {
      end = end > x_max ? x_max : end;
    }
    if (end == start)
      break;

    std::vector<float> lower = {start + x_center, y_min + y_center,
                                z_min + z_center};
    torch::Tensor mesh_xyz = utils::meshgrid_3d(
        lower[0], end + x_center + _res, lower[1], y_max + y_center + _res,
        lower[2], z_max + z_center + _res, _res, k_device);
    long x_num = mesh_xyz.size(0);
    long y_num = mesh_xyz.size(1);
    long z_num = mesh_xyz.size(2);
    mesh_xyz = mesh_xyz.view({-1, 3});

    torch::Tensor mesh_mask = get_valid_mask(mesh_xyz);
    if (mesh_mask.sum().item<int>() == 0) {
      continue;
    }
    auto mask_xyz_sdf_results = get_sdf(mesh_xyz.index({mesh_mask}));
    torch::Tensor mask_xyz_sdf = mask_xyz_sdf_results[0];
    // make a extremely large value to represent the outside of the mesh
    // to push outward invisible floaters to boundary and filter them
    torch::Tensor mesh_sdf =
        torch::full({mesh_xyz.size(0), 1}, 1e-6f).to(k_device);
    mesh_sdf.index_put_({mesh_mask}, mask_xyz_sdf);
    mesh_sdf = mesh_sdf.view({x_num, y_num, z_num});

    std::vector<float> upper;
    upper.push_back(lower[0] + x_num * _res);
    upper.push_back(lower[1] + y_num * _res);
    upper.push_back(lower[2] + z_num * _res);
    auto mc_results = mc::marching_cubes(mesh_sdf, 0.0f, lower, upper);
    auto vertices_cu = mc_results[0];
    auto faces_cu = mc_results[1];

    // if (faces_cu.numel() == 0)
    //   continue;

    // // filter boundary artifacts
    // auto qpts = (vertices_cu / _res).floor().to(torch::kInt16);
    // auto qpts_neighbor = spc_ops::points_to_neighbors(qpts).view({-1, 3});
    // auto vertices_neighbor = qpts_neighbor.to(torch::kFloat32) * _res;
    // auto valid_mask = get_valid_mask(vertices_neighbor).view({-1,
    // 27}).all(1);

    // valid_mask = valid_mask.index({faces_cu.view(-1)}).view({-1, 3}).all(-1);
    // auto valid_idx = torch::nonzero(valid_mask).view({-1});
    // faces_cu = faces_cu.index_select(0, valid_idx);
    if (faces_cu.numel() == 0 || vertices_cu.numel() == 0)
      continue;

    auto normal_cu = get_gradient(vertices_cu, _res, {}, false)[0];

    normal_cu = torch::nn::functional::normalize(
        normal_cu, torch::nn::functional::NormalizeFuncOptions().dim(-1));
    torch::Tensor color_cu;
    switch (k_vis_attribute) {
    case 1:
      color_cu = ply_utils::cal_face_normal_color(normal_cu);
      break;
    case 2:
      color_cu = get_color(vertices_cu, -normal_cu)[0];
      break;
    }

    if (_save) {
      faces_cu = faces_cu + count;
      count += vertices_cu.size(0);
      vertices_cu = coords::reset_world_system(vertices_cu, k_dataset_type);
      p_mesher_->vec_vertice_.emplace_back(vertices_cu.cpu());
      p_mesher_->vec_face_.emplace_back(faces_cu.cpu());
      color_cu = (color_cu * 255).to(torch::kUInt8);
      p_mesher_->vec_face_attr_.emplace_back(color_cu.cpu());
    }
  }
}

DepthSamples LocalMap::sample(const DepthSamples &_samples,
                              int voxel_sample_num) {
  DepthSamples samples;
  if (voxel_sample_num < 1) {
    samples.xyz = _samples.xyz;
    samples.ray_sdf = torch::zeros_like(_samples.depth);
    samples.direction = _samples.direction;
    samples.ridx = _samples.ridx;
    return samples;
  }

  bool aabb_sample = true;
  if (aabb_sample) {
    // Make sure the kaolin gets inputs between -1 and 1.
    auto normalized_origin = xyz_to_m1p1_pts(_samples.origin);

    static auto p_t_raymarch = llog::CreateTimer("    raymarch");
    p_t_raymarch->tic();
    auto raymarch_results = p_acc_strcut_->raymarch(
        normalized_origin.contiguous(), _samples.direction.contiguous(),
        "voxel", voxel_sample_num);
    p_t_raymarch->toc_sum();

    samples = _samples.index_select(0, raymarch_results.ridx);
    samples.ridx = raymarch_results.ridx;
    samples.xyz = m1p1_pts_to_xyz(raymarch_results.samples);
    auto depth_samples = scale_from_m1p1(raymarch_results.depth_samples);
    samples.ray_sdf = samples.depth - depth_samples;
    samples.depth = depth_samples;

    // Filter out the points that too behind surface.
    auto front_idx = (samples.ray_sdf > 0).squeeze().nonzero().squeeze();
    samples = samples.index_select(0, front_idx);

    /* // sample points between sensor and first voxel
    auto first_hit = kaolin::mark_pack_boundaries_cuda(raymarch_results.ridx);
    auto start_idxes =
        torch::nonzero(first_hit).view({-1}).to(torch::kInt).contiguous();
    auto start_ridx = raymarch_results.ridx.index_select(0, start_idxes);

    auto blind_depth = samples.depth.index_select(0, start_idxes);
    blind_depth = blind_depth * torch::rand_like(blind_depth);

    DepthSamples blind_samples = _samples.index_select(0, start_ridx);
    blind_samples.ridx = start_ridx;
    blind_samples.ray_sdf = blind_samples.depth - blind_depth;
    blind_samples.depth = blind_depth;
    blind_samples.xyz =
        blind_samples.origin + blind_samples.direction * blind_depth;
    samples = samples.cat(blind_samples); */

    auto free_samples = utils::sample_free_pts(_samples, k_free_sample_num);
    samples = samples.cat(free_samples);
  } else {
    samples = utils::sample_free_pts(_samples, k_free_sample_num);
  }

  return samples;
}

DepthSamples LocalMap::filter_sample(DepthSamples &_samples) {
  // Make sure the kaolin gets inputs between -1 and 1.
  auto normalized_xyz = xyz_to_m1p1_pts(_samples.xyz);
  auto query_results = p_acc_strcut_->query(normalized_xyz);
  return _samples.index({query_results.pidx > -1});
}

void LocalMap::freeze_decoder() {
  auto param_pairs = this->named_parameters();
  for (auto &param_pair : param_pairs) {
    // compare the first 7 char of key with "decoder"
    if (param_pair.key() == "decoder") {
      param_pair.value().set_requires_grad(false);
    } else if (param_pair.key() == "color_decoder") {
      param_pair.value().set_requires_grad(false);
    }
  }
}