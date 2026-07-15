// Sample subscriber: PoiEvent 駆動の音声ガイド再生 mock node (#88)。
//
// `mapoi/events` を subscribe し、`audio_info` custom tag が付与された POI で
// `EVENT_ENTER` を受信したらガイド音声を再生する想定の sample。
//
// 本 sample は **mock** 実装で、espeak / festival 等は呼ばず、ログに
// `[AUDIO_GUIDE] play: <poi_name>` を出力するだけ。実機運用では `play_*`
// 関数を音声合成 / 再生 system call に差し替える。
//
// 仕様 (#220 / #88):
//   - `EVENT_ENTER` は route 走行中 + route 登録 POI のみ発火 (v0.5.0+ の PoiEvent 仕様)。
//   - `audio_info` tag を持つ POI で EVENT_ENTER 受信 → ガイド再生 trigger。
//   - 同 POI で EXIT までは再 trigger しない (lifecycle 上 1 visit = 1 ENTER で十分)。
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>
#include <mapoi_interfaces/msg/poi_event.hpp>

#include "mapoi_turtlebot3_example/poi_event_utils.hpp"

namespace mapoi_turtlebot3_example
{

class AudioGuideNode : public rclcpp::Node
{
public:
  AudioGuideNode()
  : Node("audio_guide_node")
  {
    poi_event_sub_ = this->create_subscription<mapoi_interfaces::msg::PoiEvent>(
      "mapoi/events", 10,
      std::bind(&AudioGuideNode::on_poi_event, this, std::placeholders::_1));
    RCLCPP_INFO(this->get_logger(),
      "audio_guide_node started; waiting for EVENT_ENTER on POIs tagged 'audio_info'.");
  }

private:
  void on_poi_event(const mapoi_interfaces::msg::PoiEvent::SharedPtr msg)
  {
    if (msg->event_type != mapoi_interfaces::msg::PoiEvent::EVENT_ENTER) {
      return;
    }
    if (!has_tag(msg->poi.tags, "audio_info")) {
      return;
    }
    play_guide(msg->poi.name, msg->poi.description);
  }

  void play_guide(const std::string & poi_name, const std::string & description)
  {
    // mock 再生: 実機では espeak / festival / 音声ファイル再生に差し替える。
    RCLCPP_INFO(this->get_logger(),
      "[AUDIO_GUIDE] play: poi='%s' desc='%s'",
      poi_name.c_str(), description.c_str());
  }

  rclcpp::Subscription<mapoi_interfaces::msg::PoiEvent>::SharedPtr poi_event_sub_;
};

}  // namespace mapoi_turtlebot3_example

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<mapoi_turtlebot3_example::AudioGuideNode>());
  rclcpp::shutdown();
  return 0;
}
