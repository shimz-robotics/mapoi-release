// Copyright (c) 2019 Intel Corporation
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

#pragma once

#include <QObject>
#include <memory>
#include <rviz_default_plugins/tools/pose/pose_tool.hpp>
#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <tf2/LinearMath/Quaternion.h>


// namespace rviz_common
// {

// class DisplayContext;

// namespace properties
// {
// class StringProperty;
// }  // namespace properties
// }  // namespace rviz_common

namespace mapoi_rviz_plugins
{

class RVIZ_DEFAULT_PLUGINS_PUBLIC MapoiPoseTool : public rviz_default_plugins::tools::PoseTool
{
  Q_OBJECT

public:
  MapoiPoseTool();
  ~MapoiPoseTool() override;

  void onInitialize() override;

protected:
  void onPoseSet(double x, double y, double theta) override;

private:
  rclcpp::Node::SharedPtr node_;
  rclcpp::Publisher<geometry_msgs::msg::PoseStamped>::SharedPtr poi_pose_pub_;
};

}  // namespace mapoi_rviz_plugins
