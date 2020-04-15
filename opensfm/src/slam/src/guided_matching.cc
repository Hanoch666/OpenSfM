#include <slam/guided_matching.h>
#include <slam/third_party/orb_extractor/util/angle_checker.h>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/core/eigen.hpp>
#include <unordered_set>
#include <map/camera.h>
namespace slam
{

GridParameters::GridParameters(unsigned int grid_cols, unsigned int grid_rows,
                               float img_min_width, float img_min_height,
                               float img_max_width, float img_max_height,
                               float inv_cell_width, float inv_cell_height) : 
                               grid_cols_(grid_cols), grid_rows_(grid_rows),
                               img_min_width_(img_min_width), img_min_height_(img_min_height),
                               img_max_width_(img_max_width), img_max_height_(img_max_height),
                               inv_cell_width_(inv_cell_width), inv_cell_height_(inv_cell_height)
{
}

size_t
GuidedMatcher::ComputeMedianDescriptorIdx(const std::vector<cv::Mat> &descriptors)
{
  // First, calculate the distance between features in all combinations
  const auto num_descs = descriptors.size();
  // std::cout << "Computing: " << num_descs << std::endl;
  std::vector<std::vector<unsigned int>> hamm_dists(num_descs, std::vector<unsigned int>(num_descs));
  for (unsigned int i = 0; i < num_descs; ++i)
  {
    hamm_dists.at(i).at(i) = 0;
    for (unsigned int j = i + 1; j < num_descs; ++j)
    {
      const auto dist = compute_descriptor_distance_32(descriptors.at(i), descriptors.at(j));
      hamm_dists.at(i).at(j) = dist;
      hamm_dists.at(j).at(i) = dist;
    }
  }

  // 中央値に最も近いものを求める
  // Find the closest to the median
  unsigned int best_median_dist = MAX_HAMMING_DIST;
  unsigned int best_idx = 0;
  for (unsigned idx = 0; idx < num_descs; ++idx)
  {
    std::vector<unsigned int> partial_hamm_dists(hamm_dists.at(idx).begin(), hamm_dists.at(idx).begin() + num_descs);
    std::sort(partial_hamm_dists.begin(), partial_hamm_dists.end());
    const auto median_dist = partial_hamm_dists.at(static_cast<unsigned int>(0.5 * (num_descs - 1)));
    if (median_dist < best_median_dist)
    {
      best_median_dist = median_dist;
      best_idx = idx;
    }
  }
  return best_idx;
}

size_t 
GuidedMatcher::FindBestMatchForLandmark(const map::Landmark *const lm, map::Shot& curr_shot,
                                        const float reproj_x, const float reproj_y,
                                        const int last_scale_level, const float scaled_margin) const
{

  // std::cout << "idx_last: " << idx_last << "lm id: " << lm->lm_id_ << "in pt2D: " << curr_frm.im_name << std::fixed << pt2D << ", " << last_scale_level << " rot_cw: " << rot_cw << " t_cw: " << trans_cw << std::endl;
  const auto indices = GetKeypointsInCell(curr_shot.slam_data_.undist_keypts_,
                                          curr_shot.slam_data_.keypt_indices_in_cells_, reproj_x, reproj_y,
                                          scaled_margin ,
                                          last_scale_level - 1, last_scale_level + 1);
                                        
  if (indices.empty())
  {
    return NO_MATCH;
  }

  const auto lm_desc = lm->slam_data_.descriptor_;
  unsigned int best_hamm_dist = MAX_HAMMING_DIST;
  int best_idx = -1;

  for (const auto curr_idx : indices)
  {
    const auto* curr_lm = curr_shot.GetLandmark(curr_idx);
    //prevent adding to already set landmarks
    // if (!( curr_lm != nullptr && curr_lm->HasObservations()))
    if (curr_lm == nullptr || (curr_lm != nullptr && !curr_lm->HasObservations()))
    {
      const auto& desc = curr_shot.GetDescriptor(curr_idx);
      const auto hamm_dist = compute_descriptor_distance_32(lm_desc, desc);
      if (hamm_dist < best_hamm_dist)
      {
        best_hamm_dist = hamm_dist;
        best_idx = curr_idx;
      }
    }
  }
  return HAMMING_DIST_THR_HIGH < best_hamm_dist ? NO_MATCH : best_idx;
}

MatchIndices
GuidedMatcher::MatchKptsToKpts(const std::vector<cv::KeyPoint> &undist_keypts_1, const cv::Mat &descriptors_1,
                               const std::vector<cv::KeyPoint> &undist_keypts_2, const cv::Mat &descriptors_2,
                               const CellIndices &keypts_indices_in_cells_2,
                               const Eigen::MatrixX2f &prevMatched, const size_t margin) const
{
  MatchIndices match_indices; // Index in 1, Index in 2
  if (undist_keypts_1.empty() || undist_keypts_2.empty() || keypts_indices_in_cells_2.empty())
  {
    std::cout << "Return empty!" << std::endl;
    return match_indices;
  }
  constexpr auto check_orientation_{true};
  constexpr float lowe_ratio_{0.9};
  const size_t num_pts_1 = undist_keypts_1.size();
  const size_t num_pts_2 = undist_keypts_2.size();

  std::vector<unsigned int> matched_dists_in_frm_2(num_pts_2, MAX_HAMMING_DIST);
  std::vector<int> matched_indices_1_in_frm_2(num_pts_2, -1);
  std::vector<int> matched_indices_2_in_frm_1 = std::vector<int>(num_pts_1, -1);
  size_t num_matches = 0; // Todo: should be the same as matches.size()
  openvslam::match::angle_checker<int> angle_checker;
  for (size_t idx_1 = 0; idx_1 < num_pts_1; ++idx_1)
  {
    // f1 = x, y, size, angle, octave
    const auto &u_kpt_1 = undist_keypts_1.at(idx_1);
    const Eigen::Vector2f pt2D = prevMatched.row(idx_1);
    const float scale_1 = u_kpt_1.octave;
    if (scale_1 < 0)
      continue;
    // const auto indices = get_keypoints_in_cell(frame2.undist_keypts_, frame2.keypts_indices_in_cells_,
    //                                            u_kpt_1.pt.x, u_kpt_1.pt.y, margin, scale_1, scale_1);
    const auto indices = GetKeypointsInCell(undist_keypts_2, keypts_indices_in_cells_2,
                                            pt2D[0], pt2D[1], margin, scale_1, scale_1);
    if (indices.empty())
      continue; // No valid match

    // Read the descriptor
    const auto &d1 = descriptors_1.row(idx_1);
    auto best_hamm_dist = MAX_HAMMING_DIST;
    auto second_best_hamm_dist = MAX_HAMMING_DIST;
    int best_idx_2 = -1;
    for (const auto idx_2 : indices)
    {
      const auto &d2 = descriptors_2.row(idx_2);
      const auto hamm_dist = compute_descriptor_distance_32(d1, d2);
      // through if the point already matched is closer
      if (matched_dists_in_frm_2.at(idx_2) <= hamm_dist)
      {
        continue;
      }
      if (hamm_dist < best_hamm_dist)
      {
        second_best_hamm_dist = best_hamm_dist;
        best_hamm_dist = hamm_dist;
        best_idx_2 = idx_2;
      }
      else if (hamm_dist < second_best_hamm_dist)
      {
        second_best_hamm_dist = hamm_dist;
      }
    }

    if (HAMMING_DIST_THR_LOW < best_hamm_dist)
    {
      continue;
    }

    // ratio test
    if (second_best_hamm_dist * lowe_ratio_ < static_cast<float>(best_hamm_dist))
    {
      continue;
    }

    const auto prev_idx_1 = matched_indices_1_in_frm_2.at(best_idx_2);
    if (0 <= prev_idx_1)
    {
      matched_indices_2_in_frm_1.at(prev_idx_1) = -1;
      --num_matches;
    }

    // 互いの対応情報を記録する
    matched_indices_2_in_frm_1.at(idx_1) = best_idx_2;
    matched_indices_1_in_frm_2.at(best_idx_2) = idx_1;
    matched_dists_in_frm_2.at(best_idx_2) = best_hamm_dist;
    ++num_matches;

    if (check_orientation_)
    {
      const auto delta_angle = undist_keypts_1.at(idx_1).angle - undist_keypts_2.at(best_idx_2).angle;
      angle_checker.append_delta_angle(delta_angle, idx_1);
    }
  }

  if (check_orientation_)
  {
    const auto invalid_matches = angle_checker.get_invalid_matches();
    for (const auto invalid_idx_1 : invalid_matches)
    {
      if (0 <= matched_indices_2_in_frm_1.at(invalid_idx_1))
      {
        matched_indices_2_in_frm_1.at(invalid_idx_1) = -1;
        --num_matches;
      }
    }
  }

  match_indices.reserve(num_pts_1);
  for (unsigned int idx_1 = 0; idx_1 < matched_indices_2_in_frm_1.size(); ++idx_1)
  {
    const auto idx_2 = matched_indices_2_in_frm_1.at(idx_1);
    if (idx_2 >= 0)
    {
      match_indices.emplace_back(std::make_pair(idx_1, idx_2));
    }
  }
  return match_indices;
}

void GuidedMatcher::DistributeUndistKeyptsToGrid(const std::vector<cv::KeyPoint> &undist_keypts, CellIndices &keypt_indices_in_cells) const
{
  const size_t num_pts = undist_keypts.size();
  const size_t num_to_reserve = 0.5 * num_pts / (grid_params_.grid_cols_ * grid_params_.grid_rows_);
  keypt_indices_in_cells.resize(grid_params_.grid_cols_);
  for (auto &keypt_indices_in_row : keypt_indices_in_cells)
  {
    keypt_indices_in_row.resize(grid_params_.grid_rows_);
    for (auto &keypt_indices_in_cell : keypt_indices_in_row)
    {
      keypt_indices_in_cell.reserve(num_to_reserve);
    }
  }
  for (size_t idx = 0; idx < num_pts; ++idx)
  {
    const auto &keypt = undist_keypts.at(idx);
    const int cell_idx_x = std::round((keypt.pt.x - grid_params_.img_min_width_) * grid_params_.inv_cell_width_);
    const int cell_idx_y = std::round((keypt.pt.y - grid_params_.img_min_height_) * grid_params_.inv_cell_height_);
    if ((0 <= cell_idx_x && cell_idx_x < static_cast<int>(grid_params_.grid_cols_) && 0 <= cell_idx_y && cell_idx_y < static_cast<int>(grid_params_.grid_rows_)))
    {
      keypt_indices_in_cells.at(cell_idx_x).at(cell_idx_y).push_back(idx);
    }
  }
}

std::vector<size_t>
GuidedMatcher::GetKeypointsInCell(const std::vector<cv::KeyPoint> &undist_keypts,
                                  const CellIndices &keypt_indices_in_cells,
                                  const float ref_x, const float ref_y, const float margin,
                                  const int min_level, const int max_level) const
{
  std::vector<size_t> indices;
  indices.reserve(undist_keypts.size());
  const int min_cell_idx_x = std::max(0, cvFloor((ref_x - grid_params_.img_min_width_ - margin) * grid_params_.inv_cell_width_));

  if (static_cast<int>(grid_params_.grid_cols_) <= min_cell_idx_x)
  {
    return indices;
  }

  const int max_cell_idx_x = std::min(static_cast<int>(grid_params_.grid_cols_ - 1), cvCeil((ref_x - grid_params_.img_min_width_ + margin) * grid_params_.inv_cell_width_));
  if (max_cell_idx_x < 0)
  {
    return indices;
  }

  const int min_cell_idx_y = std::max(0, cvFloor((ref_y - grid_params_.img_min_height_ - margin) * grid_params_.inv_cell_height_));
  if (static_cast<int>(grid_params_.grid_rows_) <= min_cell_idx_y)
  {
    return indices;
  }

  const int max_cell_idx_y = std::min(static_cast<int>(grid_params_.grid_rows_ - 1), cvCeil((ref_y - grid_params_.img_min_height_ + margin) * grid_params_.inv_cell_height_));
  if (max_cell_idx_y < 0)
  {
    return indices;
  }

  const bool check_level = (0 < min_level) || (0 <= max_level);
  for (int cell_idx_x = min_cell_idx_x; cell_idx_x <= max_cell_idx_x; ++cell_idx_x)
  {
    for (int cell_idx_y = min_cell_idx_y; cell_idx_y <= max_cell_idx_y; ++cell_idx_y)
    {
      const auto &keypt_indices_in_cell = keypt_indices_in_cells.at(cell_idx_x).at(cell_idx_y);

      if (keypt_indices_in_cell.empty())
      {
        continue;
      }

      for (unsigned int idx : keypt_indices_in_cell)
      {
        const auto &keypt = undist_keypts[idx];
        if (check_level)
        {
          if (keypt.octave < min_level || (0 <= max_level && max_level < keypt.octave))
          {
            continue;
          }
        }
        const float dist_x = keypt.pt.x - ref_x;
        const float dist_y = keypt.pt.y - ref_y;
        if (std::abs(dist_x) < margin && std::abs(dist_y) < margin)
        {
          indices.push_back(idx);
          if (idx >= undist_keypts.size())
          {
            std::cout << "keypts idx error!" << idx << std::endl;
            // exit(0);
          }
        }
      }
    }
  }
  return indices;
}

size_t
GuidedMatcher::AssignShot1LandmarksToShot2Kpts(const map::Shot &last_shot, map::Shot &curr_shot, const float margin) const
{
  size_t num_matches = 0;
  constexpr auto check_orientation_{true};
  constexpr float lowe_ratio_{0.9};
  openvslam::match::angle_checker<int> angle_checker;

  const auto &cam_pose = curr_shot.GetPose();
  const Eigen::Matrix3f rot_cw = cam_pose.RotationWorldToCamera().cast<float>(); //  cam_pose_cw.block<3, 3>(0, 0);
  const Eigen::Vector3f trans_cw = cam_pose.TranslationWorldToCamera().cast<float>();

  std::cout << "last_frm: " << last_shot.name_ << " nK: " << last_shot.NumberOfKeyPoints() << std::endl;
  MatchIndices matches;
  auto &landmarks = last_shot.GetLandmarks();
  const auto num_keypts = landmarks.size();
  const auto& cam = last_shot.shot_camera_.camera_model_;
  std::cout << "N valid: " << last_shot.ComputeNumValidLandmarks(1) << " for " << last_shot.name_ << std::endl;
  for (unsigned int idx_last = 0; idx_last < num_keypts; ++idx_last)
  {
    auto lm = landmarks.at(idx_last);
    std::cout << "lm: " << lm << "idx_last: " << idx_last <<","<< landmarks.size() << std::endl;

    if (lm != nullptr)
    {
      // Global standard 3D point coordinates
      const Eigen::Vector3f pos_w = lm->GetGlobalPos().cast<float>();
      // Reproject to find visibility
      Eigen::Vector2f pt2D;
      
      if (cam.ReprojectToImage(rot_cw, trans_cw, pos_w, pt2D))
      {
        //check if it is within our grid
        if (grid_params_.in_grid(pt2D))
        {
          std::cout << "Getting KP!" << std::endl;
          const auto last_scale_level = last_shot.GetKeyPoint(idx_last).octave;
          std::cout << "Got KP!" << std::endl;
          const auto scaled_margin = margin * scale_factors_.at(last_scale_level);
          std::cout << "scale_factors_:" << scale_factors_.size() << std::endl;
          const auto best_idx = FindBestMatchForLandmark(lm, curr_shot, pt2D[0], pt2D[1], last_scale_level, scaled_margin);
          if (best_idx != NO_MATCH)
          {
            std::cout << "Trigger: " << best_idx << "/" << curr_shot.NumberOfKeyPoints() << std::endl;
            // Valid matching
            curr_shot.AddLandmarkObservation(lm, best_idx);
            std::cout << "Triggered: " << best_idx << std::endl;
            ++num_matches;
            // std::cout << "num_matches: " << num_matches << std::endl;
            if (check_orientation_)
            {
              const auto delta_angle = last_shot.slam_data_.undist_keypts_.at(idx_last).angle -
                                       curr_shot.slam_data_.undist_keypts_.at(best_idx).angle;
              angle_checker.append_delta_angle(delta_angle, best_idx);
            }
          }
        }
        else
        {
          std::cout << "Not in grid" << pt2D.transpose() << std::endl;
        }
      }
      else
      {
        std::cout << "Reprojection failed: " << pt2D.transpose() << std::endl;
      }
    }
  }

  // Clean-up step

  if (check_orientation_)
  {
    const auto invalid_matches = angle_checker.get_invalid_matches();
    for (const auto invalid_idx : invalid_matches)
    {
      // curr_frm.landmarks_.at(invalid_idx) = nullptr;
      curr_shot.RemoveLandmarkObservation(invalid_idx);
      --num_matches;
    }
  }
  return num_matches;
}

bool 
GuidedMatcher::IsObservable(map::Landmark* lm, const map::Shot& shot, const float ray_cos_thr,
                            Eigen::Vector2d& reproj, size_t& pred_scale_level) const 
{
  const Eigen::Vector3d pos_w = lm->GetGlobalPos();
  const auto& pose = shot.GetPose();
  const Eigen::Matrix3d rot_cw = pose.RotationWorldToCamera();
  const Eigen::Vector3d trans_cw = pose.TranslationWorldToCamera();
  const auto& cam = shot.shot_camera_.camera_model_;

  const bool in_image = cam.ReprojectToImage(rot_cw, trans_cw, pos_w, reproj);
  const auto& lm_data = lm->slam_data_;
  if (in_image)
  {
    if (grid_params_.in_grid(reproj.cast<float>()))
    {
      const Eigen::Vector3d cam_to_lm_vec = pos_w - pose.GetOrigin();
      const auto cam_to_lm_dist = cam_to_lm_vec.norm();
      // check if inside orb sale
      if (lm_data.GetMinValidDistance()  <= cam_to_lm_dist && 
          lm_data.GetMaxValidDistance() >= cam_to_lm_dist)
      {
        const Eigen::Vector3d obs_mean_normal = lm_data.mean_normal_;
        const auto ray_cos = cam_to_lm_vec.dot(obs_mean_normal) / cam_to_lm_dist;
        if (ray_cos > ray_cos_thr)
        {
          pred_scale_level = PredScaleLevel(lm_data.GetMaxValidDistance(), cam_to_lm_dist);
          return true;
        }
      }
    }
    

  }
  return false;
}

size_t 
GuidedMatcher::PredScaleLevel(const float max_valid_dist, const float cam_to_lm_dist) const
{
  const auto ratio = max_valid_dist / cam_to_lm_dist;
  const auto pred_scale_level = static_cast<int>(std::ceil(std::log(ratio) / log_scale_factor_));
  if (pred_scale_level < 0) return 0;
  if (num_scale_levels_ <= static_cast<unsigned int>(pred_scale_level)) return num_scale_levels_ - 1;
  return static_cast<unsigned int>(pred_scale_level);
}
}; // namespace slam