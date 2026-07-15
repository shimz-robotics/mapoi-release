#include <memory>
#include <math.h>

#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>

class PrintInitialPose : public rclcpp::Node
{
  public:
    PrintInitialPose()
    : Node("print_initialpose")
    {
      initialpose_sub_ = this->create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(
      "/initialpose", 1, std::bind(&PrintInitialPose::initialpose_cb, this, std::placeholders::_1));
       RCLCPP_INFO(this->get_logger(), "Draw 2D Pose Estimate on Rviz");
    }

  private:
    void initialpose_cb(const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg) const
    {
      auto x = msg->pose.pose.orientation.x;
      auto y = msg->pose.pose.orientation.y;
      auto z = msg->pose.pose.orientation.z;
      auto w = msg->pose.pose.orientation.w;

      // auto t0 = +2.0 * (w * x + y * z);
      // auto t1 = +1.0 - 2.0 * (x * x + y * y);
      // auto roll_x = atan2(t0, t1);

      // auto t2 = +2.0 * (w * y - z * x);
      // auto t2 = +1.0 if t2 > +1.0 else t2;
      // auto t2 = -1.0 if t2 < -1.0 else t2;
      // auto pitch_y = asin(t2)

      auto t3 = +2.0 * (w * z + x * y);
      auto t4 = +1.0 - 2.0 * (y * y + z * z);
      auto yaw_z = atan2(t3, t4);

      RCLCPP_INFO(this->get_logger(), "pose: {x: %f, y: %f, yaw: %f}",
        msg->pose.pose.position.x, msg->pose.pose.position.y, yaw_z);
    }
    rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr initialpose_sub_;
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<PrintInitialPose>());
  rclcpp::shutdown();
  return 0;
}