// MapoiPanel::NavStatusCallback の定義 (#397 step 3 で mapoi_panel.cpp から別 TU へ分離)。
// nav status 文字列のパースと NavStatusLabel への表示更新を担う。
#include "mapoi_rviz_plugins/mapoi_panel.hpp"
#include "ui_mapoi_panel.h"
#include <algorithm>

namespace mapoi_rviz_plugins
{

void MapoiPanel::NavStatusCallback(std_msgs::msg::String::SharedPtr msg)
{
  // payload は "status" または "status:target" 形式 (#104)。
  // : split で target を抽出し、有れば後起動 panel / 外部ノード発行 nav でも
  // target 復元できるようにする。
  const auto & raw = msg->data;
  const auto colon = raw.find(':');
  const std::string status = (colon == std::string::npos) ? raw : raw.substr(0, colon);
  const std::string target = (colon == std::string::npos) ? std::string{} : raw.substr(colon + 1);
  QMetaObject::invokeMethod(this, [this, status, target]() {
    // 走行終了系の分岐で共通: 進捗表示をクリアし、遅延到着した mapoi/events による
    // 表示復活を抑制する (#406。抑制解除は navigating 分岐)。
    auto clear_route_progress = [this]() {
      route_progress_suppressed_ = true;
      SetRouteProgressIdle();
    };
    if (status == "navigating") {
      // 新しい走行の開始。前回終了時の抑制を解除する。抑制中だった場合のみクリアする
      // (走行中の navigating 再受信で進行中の進捗表示を消さないため)。
      if (route_progress_suppressed_) {
        route_progress_suppressed_ = false;
        SetRouteProgressIdle();
      }
      if (!target.empty() && current_nav_mode_ == "idle") {
        // 後起動 panel / 外部ノード発行 nav: payload の target を復元して表示。
        current_nav_target_ = target;
        SetNavStatus(tr("Navigating: ") + QString::fromStdString(target));
      } else if (current_nav_mode_ == "route") {
        SetNavStatus(
            tr("Navigating route: ") + QString::fromStdString(current_nav_target_));
      } else if (current_nav_mode_ == "idle") {
        // target も無い (旧 publisher 等) → 汎用表示にフォールバック。
        SetNavStatus(tr("Navigating"));
      }
      // goal mode は RunGoalButton で即時表示済みのため何もしない
    } else if (status == "succeeded") {
      current_nav_mode_ = "idle";
      SetNavStatus(
          target.empty() ? tr("Arrived") : tr("Arrived: ") + QString::fromStdString(target));
      clear_route_progress();
    } else if (status == "aborted") {
      current_nav_mode_ = "idle";
      SetNavStatus(
          target.empty() ? tr("Aborted") : tr("Aborted: ") + QString::fromStdString(target));
      clear_route_progress();
    } else if (status == "canceled") {
      current_nav_mode_ = "idle";
      SetNavStatus(
          target.empty() ? tr("Canceled") : tr("Canceled: ") + QString::fromStdString(target));
      clear_route_progress();
    } else if (status == "paused") {
      SetNavStatus(
          target.empty() ? tr("Paused") : tr("Paused: ") + QString::fromStdString(target));
    } else if (status == "map_switching") {
      SetNavStatus(
          target.empty() ? tr("Switching map")
                         : tr("Switching map: ") + QString::fromStdString(target));
    } else if (status == "map_switch_succeeded") {
      current_nav_mode_ = "idle";
      SetNavStatus(
          target.empty() ? tr("Map switched")
                         : tr("Map switched: ") + QString::fromStdString(target));
      clear_route_progress();
    } else if (status == "map_switch_failed") {
      current_nav_mode_ = "idle";
      SetNavStatus(
          target.empty() ? tr("Map switch failed")
                         : tr("Map switch failed: ") + QString::fromStdString(target));
      clear_route_progress();
    } else if (status == "backend_unavailable") {
      current_nav_mode_ = "idle";
      SetNavStatus(
          target.empty() ? tr("Navigation backend unavailable")
                         : tr("Navigation backend unavailable: ") + QString::fromStdString(target));
      clear_route_progress();
    } else if (status == "rejected") {
      // #339: 受理前に拒否されたコマンド (存在しない POI 名、landmark POI を goal 指定、
      // 空 route 等)。直前の status が居座って誤操作に気づけない事態を防ぐ。
      current_nav_mode_ = "idle";
      SetNavStatus(
          target.empty() ? tr("Command rejected")
                         : tr("Command rejected: ") + QString::fromStdString(target));
      clear_route_progress();
    }
  }, Qt::QueuedConnection);
}

// #406: mapoi/events コールバック。ROUTE 走行中にのみ受信される (IDLE/GOAL では発火しない)。
// n/総数の総数は highlighted_route_poi_names_ (panel でルート選択済み時に確定)。
// 外部ノード起点の走行など panel が未選択の場合はリストが空になるため n/総数 を省略し
// POI 名のみ表示するフォールバックとする。未知の event_type は無視する。
void MapoiPanel::PoiEventCallback(mapoi_interfaces::msg::PoiEvent::SharedPtr msg)
{
  const uint8_t event_type = msg->event_type;
  // 未知の event_type は無視 (EVENT_ENTER/EVENT_PAUSED/EVENT_EXIT のみ処理)
  if (event_type != mapoi_interfaces::msg::PoiEvent::EVENT_ENTER &&
      event_type != mapoi_interfaces::msg::PoiEvent::EVENT_PAUSED &&
      event_type != mapoi_interfaces::msg::PoiEvent::EVENT_EXIT) {
    return;
  }
  const std::string poi_name = msg->poi.name;
  if (poi_name.empty()) {
    return;  // bridge 契約上は非空だが、空名は表示価値が無いため防御的に無視
  }
  QMetaObject::invokeMethod(this, [this, event_type, poi_name]() {
    if (route_progress_suppressed_) {
      return;  // 走行終了後に遅延到着した event。表示を復活させない
    }
    // n/総数: panel で選択したルートの POI リスト内での先頭一致 index + 1。
    // リストが空 (外部ノード起点) または見つからない場合は n/総数 を省略する。
    // 同名 POI がルートに複数回現れる場合は常に先頭の index を示す (単純表示の割り切り)。
    // highlighted_route_poi_names_ は UI スレッドが書き換えるメンバのため、参照も
    // queued lambda 内 (UI スレッド) で行う (NavStatusCallback の current_nav_mode_ と同じ規約)。
    const auto & names = highlighted_route_poi_names_;
    std::string progress_suffix;
    if (!names.empty()) {
      const auto it = std::find(names.begin(), names.end(), poi_name);
      if (it != names.end()) {
        const int n = static_cast<int>(std::distance(names.begin(), it)) + 1;
        const int total = static_cast<int>(names.size());
        progress_suffix = " (" + std::to_string(n) + "/" + std::to_string(total) + ")";
      }
    }
    QString prefix;
    if (event_type == mapoi_interfaces::msg::PoiEvent::EVENT_ENTER) {
      prefix = tr("Entered: ");
    } else if (event_type == mapoi_interfaces::msg::PoiEvent::EVENT_PAUSED) {
      prefix = tr("Paused at: ");
    } else {  // EVENT_EXIT
      prefix = tr("Passed: ");
    }
    // idle 表示 (#451) はグレーの styleSheet を持つため、実値の表示時は既定色に戻す。
    ui_->RouteProgressLabel->setStyleSheet(QString{});
    ui_->RouteProgressLabel->setText(
        prefix + QString::fromStdString(poi_name + progress_suffix));
  }, Qt::QueuedConnection);
}

// #451: idle 時のプレースホルダ表示。空文字だと状態エリアが説明のない空白行に
// 見えるため、「<caption>: —」をグレーで常設する。ラベル自体を hide しないのは
// 従来どおり (出現/消滅でレイアウトが上下しないため)。

// NavStatusLabel の唯一の書き込み経路。idle のグレーを実値表示で既定色へ戻すため、
// setText 直呼びせず必ずここを通す (nav_status callback / nav ボタン群の全 17 箇所)。
void MapoiPanel::SetNavStatus(const QString & text)
{
  ui_->NavStatusLabel->setStyleSheet(QString{});
  ui_->NavStatusLabel->setText(text);
}

// NavStatusLabel は latched (transient_local) な mapoi/nav/status の replay で埋まるため、
// idle 表示が出るのは server/bridge が一度も publish していない起動直後のみ。
void MapoiPanel::SetNavStatusIdle()
{
  ui_->NavStatusLabel->setStyleSheet("color: gray;");
  ui_->NavStatusLabel->setText(tr("Nav status: —"));
}

void MapoiPanel::SetRouteProgressIdle()
{
  ui_->RouteProgressLabel->setStyleSheet("color: gray;");
  ui_->RouteProgressLabel->setText(tr("Route progress: —"));
}

// CommandRejectedLabel は通知ごとに赤/緑を setStyleSheet する (ShowTransientNotice)
// ため、idle 復帰時に必ずグレーへ戻す (戻し忘れると次の idle が着色されたまま残る)。
void MapoiPanel::SetNoticeIdle()
{
  ui_->CommandRejectedLabel->setStyleSheet("color: gray;");
  ui_->CommandRejectedLabel->setText(tr("Notice: —"));
}

// #401: CommandRejectedLabel への一時通知の共用ヘルパー。#398 の CommandRejectedLabel と
// reject_clear_timer_ (singleShot 5 秒) をここ (両者の責務がある TU) で集約する。
// UI (Qt メイン) スレッドからのみ呼ぶ前提: CommandRejectedCallback は queued lambda 内、
// LocalizationButton / MapoiRouteComboBox は Qt スロット内 = いずれも UI スレッド。
// is_error で文字色を切り替える (エラー=赤 / 情報=緑)。成功通知まで赤にすると
// オペレータに誤警戒を与えるため、.ui の初期値に頼らず呼び出しごとに設定する。
void MapoiPanel::ShowTransientNotice(const QString & text, bool is_error)
{
  ui_->CommandRejectedLabel->setStyleSheet(is_error ? "color: red;" : "color: green;");
  ui_->CommandRejectedLabel->setText(text);
  reject_clear_timer_->start();  // 連続通知時は自動リスタートで 5 秒延長
}

// #398: mapoi/nav/command_rejected コールバック。payload は target 名のみの文字列
// (README §Publishers command_rejected 節参照)。NavStatusLabel (latched 状態スナップショット) には
// 一切触れず、CommandRejectedLabel に一時表示して 5 秒後にクリアする。
void MapoiPanel::CommandRejectedCallback(std_msgs::msg::String::SharedPtr msg)
{
  const std::string target = msg->data;
  QMetaObject::invokeMethod(this, [this, target]() {
    const QString text = target.empty()
      ? tr("Command rejected")
      : tr("Command rejected: ") + QString::fromStdString(target);
    ShowTransientNotice(text);  // #401: 一時通知機構を共用 (挙動不変)
  }, Qt::QueuedConnection);
}

}  // namespace mapoi_rviz_plugins
