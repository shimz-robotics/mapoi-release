// Copyright (c) 2019 Intel Corporation
// Copyright (c) 2023 Shimuz Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mapoi_rviz_plugins/mapoi_pose_tool.hpp"

#include <memory>
#include <string>

#include <rviz_common/display_context.hpp>
#include <rviz_common/load_resource.hpp>

namespace mapoi_rviz_plugins
{

MapoiPoseTool::MapoiPoseTool()
: rviz_default_plugins::tools::PoseTool()
{
  shortcut_key_ = 'i';
}

MapoiPoseTool::~MapoiPoseTool()
{
}

void MapoiPoseTool::onInitialize()
{
  PoseTool::onInitialize();
  setName("Set Mapoi Pose");
  setIcon(rviz_common::loadPixmap("package://rviz_default_plugins/icons/classes/SetGoal.png"));
  node_ = context_->getRosNodeAbstraction().lock()->get_raw_node();
  poi_pose_pub_ = node_->create_publisher<geometry_msgs::msg::PoseStamped>("mapoi_rviz_pose", 1);
}

void
MapoiPoseTool::onPoseSet(double x, double y, double theta)
{
  auto msg = std::make_shared<geometry_msgs::msg::PoseStamped>();
  msg->header.frame_id = context_->getFrameManager()->getFixedFrame();
  msg->header.stamp = node_->now();
  msg->pose.position.x = x;
  msg->pose.position.y = y;
  tf2::Quaternion tf2_q;
  tf2_q.setRPY(0.0, 0.0, theta);
  tf2_q=tf2_q.normalize();
  msg->pose.orientation.x = tf2_q.x();
  msg->pose.orientation.y = tf2_q.y();
  msg->pose.orientation.z = tf2_q.z();
  msg->pose.orientation.w = tf2_q.w();
  poi_pose_pub_->publish(*msg);
}

}  // namespace mapoi_rviz_plugins

#include <pluginlib/class_list_macros.hpp>  // NOLINT
PLUGINLIB_EXPORT_CLASS(mapoi_rviz_plugins::MapoiPoseTool, rviz_common::Tool)