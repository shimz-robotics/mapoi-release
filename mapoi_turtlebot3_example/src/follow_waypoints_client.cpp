#include "mapoi_turtlebot3_example/follow_waypoints_client.hpp"
#include <chrono>
#include <functional>

using namespace std::chrono_literals;

// コンストラクタの実装
FollowWaypointsClient::FollowWaypointsClient(const rclcpp::NodeOptions & options)
: Node("follow_waypoints_client_node", options)
{
  this->get_logger().set_level(rclcpp::Logger::Level::Info);

  // アクションクライアントの作成
  this->client_ptr_ = rclcpp_action::create_client<FollowWaypoints>(
    this,
    "follow_waypoints"
  );
  
  RCLCPP_INFO(this->get_logger(), "アクションクライアントノードを起動しました。");
}

// ウェイポイント送信を開始するパブリック関数
void FollowWaypointsClient::send_waypoints(const std::vector<geometry_msgs::msg::PoseStamped>& waypoints)
{
  if (!waypoints.empty()) {
    this->waypoints_to_send_ = waypoints;

    // サーバーチェックと送信を試みるタイマーを起動
    this->timer_ = this->create_wall_timer(
      500ms,
      std::bind(&FollowWaypointsClient::check_and_send_goal, this));
  } else {
    RCLCPP_WARN(this->get_logger(), "送信すべきウェイポイントがリストにありません。");
  }
}

// サーバーの可用性をチェックし、ゴールを送信するプライベート関数
void FollowWaypointsClient::check_and_send_goal()
{
  if (!this->client_ptr_->wait_for_action_server(1s)) {
    RCLCPP_WARN(this->get_logger(), "アクションサーバーが見つかりません。再試行します...");
    return;
  }
  
  RCLCPP_INFO(this->get_logger(), "アクションサーバーが利用可能です。ゴールを送信します。");
  this->timer_->cancel(); // サーバーが見つかったらタイマーを停止

  auto goal_msg = FollowWaypoints::Goal();
  goal_msg.poses = this->waypoints_to_send_;
  
  // コールバックの設定
  auto send_goal_options = rclcpp_action::Client<FollowWaypoints>::SendGoalOptions();
  send_goal_options.goal_response_callback =
    std::bind(&FollowWaypointsClient::goal_response_callback, this, std::placeholders::_1);
  send_goal_options.feedback_callback =
    std::bind(&FollowWaypointsClient::feedback_callback, this, std::placeholders::_1, std::placeholders::_2);
  send_goal_options.result_callback =
    std::bind(&FollowWaypointsClient::result_callback, this, std::placeholders::_1);

  // ゴールの非同期送信
  this->client_ptr_->async_send_goal(goal_msg, send_goal_options);
}

// ゴール要求に対する応答コールバック
void FollowWaypointsClient::goal_response_callback(const GoalHandleFollowWaypoints::SharedPtr & goal_handle)
{
  if (!goal_handle) {
    RCLCPP_ERROR(this->get_logger(), "ゴールは拒否されました。");
  } else {
    RCLCPP_INFO(this->get_logger(), "ゴールは受け付けられました。ウェイポイント追従を開始します。");
  }
}

// フィードバックコールバック
void FollowWaypointsClient::feedback_callback(
  GoalHandleFollowWaypoints::SharedPtr,
  const std::shared_ptr<const FollowWaypoints::Feedback> feedback)
{
  RCLCPP_INFO(this->get_logger(), "現在のウェイポイント・インデックス: %u", 
    feedback->current_waypoint);
}

// 結果コールバック
void FollowWaypointsClient::result_callback(const GoalHandleFollowWaypoints::WrappedResult & result)
{
  switch (result.code) {
    case rclcpp_action::ResultCode::SUCCEEDED:
      RCLCPP_INFO(this->get_logger(), "✅ すべてのウェイポイント追従が成功しました！");
      break;
    case rclcpp_action::ResultCode::ABORTED:
      RCLCPP_ERROR(this->get_logger(), "❌ ウェイポイント追従が中断されました。");
      // 中断されたウェイポイントのインデックスは result.result->missed_waypoints で確認できます
      break;
    case rclcpp_action::ResultCode::CANCELED:
      RCLCPP_WARN(this->get_logger(), "⚠️ ウェイポイント追従がキャンセルされました。");
      break;
    default:
      RCLCPP_ERROR(this->get_logger(), "❓ 不明な結果コードです。");
      break;
  }
  // ナビゲーション完了後、ノードをシャットダウン
  rclcpp::shutdown(); 
}

// メイン関数
int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  // 1. ウェイポイントリストの作成（今回は固定値）
  std::vector<geometry_msgs::msg::PoseStamped> initial_waypoints;

  // ウェイポイント #1: x=-0.5, y=-0.5, yaw=0.0 (w=1.0)
  // geometry_msgs::msg::PoseStamped wp1;
  // wp1.header.frame_id = "map";
  // wp1.pose.position.x = -0.5;
  // wp1.pose.position.y = -0.5;
  // wp1.pose.orientation.w = 1.0; 
  // initial_waypoints.push_back(wp1);

  // ウェイポイント #2: x=0.5, y=-0.5, yaw=1.57rad (90度)
  geometry_msgs::msg::PoseStamped wp2;
  wp2.header.frame_id = "map";
  wp2.pose.position.x = 0.5;
  wp2.pose.position.y = -0.5;
  wp2.pose.orientation.z = 0.707;
  wp2.pose.orientation.w = 0.707;
  initial_waypoints.push_back(wp2);

  // 2. クライアントノードを起動し、ウェイポイントを送信
  auto client_node = std::make_shared<FollowWaypointsClient>();

  // ウェイポイントリストに最新の時刻ヘッダーを付与
  for (auto& wp : initial_waypoints) {
      wp.header.stamp = client_node->now();
  }

  client_node->send_waypoints(initial_waypoints);
  
  rclcpp::spin(client_node);
  rclcpp::shutdown();
  return 0;
}
