#ifndef MAPOI_TURTLEBOT3_EXAMPLE__CAMERA_NODE_HPP_
#define MAPOI_TURTLEBOT3_EXAMPLE__CAMERA_NODE_HPP_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <mapoi_interfaces/msg/poi_event.hpp>

#include "mapoi_turtlebot3_example/poi_event_utils.hpp"

namespace mapoi_turtlebot3_example
{

class CameraNode : public rclcpp::Node
{
public:
  // capture_duration_sec の安全範囲。
  //   - 下限 1ms (0.001s): 0 以下は "撮影しないで即 resume" になるため当然弾くが、
  //     極小正値 (0.0001s 等) も static_cast<int64_t>(v * 1000.0) = 0ms タイマー化
  //     して create_wall_timer(0ms) 相当の即時発火になり、撮影 mock として意味を
  //     成さない。`μs と s の単位ミス` typo も同じく弾く。
  //   - 上限 600s: demo / 試験で想定されるカメラ露光の最大値。これを超える値は
  //     大抵 typo (秒/ミリ秒の単位ミス) なので WARN して default に落とす。
  static constexpr double kCaptureDurationDefaultSec = 1.5;
  static constexpr double kCaptureDurationMinSec = 0.001;
  static constexpr double kCaptureDurationMaxSec = 600.0;

  explicit CameraNode(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());

  // shutdown 時に pending な resume_timer_ を明示 cancel する。
  // 通常 destructor で shared_ptr<Timer> が破棄されれば良いが、callback が
  // 別 thread で発火する寸前のタイミングで node 内部 state が壊れた状態を
  // 触らせない安全側の挙動 (#248 項目 7)。
  ~CameraNode() override;

  // capture_duration_sec の入力値を安全な範囲に正規化する純関数。
  //   - NaN / inf / kCaptureDurationMinSec 未満 / kCaptureDurationMaxSec 超
  //     → kCaptureDurationDefaultSec
  //   - それ以外はそのまま返す (min / max は inclusive)
  // 純関数として切り出しているのは unit test で境界条件を pin するため。
  static double sanitize_capture_duration_sec(double v);

private:
  void on_poi_event(const mapoi_interfaces::msg::PoiEvent::SharedPtr msg);
  void do_capture(const std::string & poi_name, const std::string & description);
  void publish_resume(const std::string & poi_name);

  rclcpp::Subscription<mapoi_interfaces::msg::PoiEvent>::SharedPtr poi_event_sub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr resume_pub_;
  rclcpp::TimerBase::SharedPtr resume_timer_;
  // MultiThreadedExecutor / Reentrant callback_group での同時 2 発火に耐える
  // ように atomic + compare_exchange で「false → true」遷移を取れたスレッド
  // だけが capture を実行する。SingleThreadedExecutor 前提でも race を作らない
  // 防御 (#248 項目 3)。
  std::atomic<bool> capture_in_flight_{false};
};

}  // namespace mapoi_turtlebot3_example

#endif  // MAPOI_TURTLEBOT3_EXAMPLE__CAMERA_NODE_HPP_
