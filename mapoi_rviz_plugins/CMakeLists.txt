cmake_minimum_required(VERSION 3.5)
project(mapoi_rviz_plugins)

find_package(ament_cmake_auto REQUIRED)
ament_auto_find_build_dependencies()

# Qt settings
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

pluginlib_export_plugin_description_file(rviz_common plugin_description.xml)

set(HEADERS
  include/mapoi_rviz_plugins/mapoi_panel.hpp
  include/mapoi_rviz_plugins/poi_editor.hpp
  include/mapoi_rviz_plugins/mapoi_pose_tool.hpp
)
qt5_wrap_ui(UIC_FILES
  src/ui/mapoi_panel.ui
  src/ui/poi_editor.ui
)
set(SOURCE_FILES
  src/mapoi_panel.cpp
  src/mapoi_panel_config.cpp
  src/mapoi_panel_backend_status.cpp
  src/mapoi_panel_nav_control.cpp
  src/mapoi_panel_nav_status.cpp
  src/poi_editor.cpp
  src/poi_editor_display_settings.cpp
  src/poi_editor_save.cpp
  src/poi_editor_table.cpp
  src/poi_editor_tags.cpp
  src/poi_editor_validation.cpp
  src/mapoi_pose_tool.cpp
)

ament_auto_add_library(${PROJECT_NAME} SHARED ${SOURCE_FILES} ${HEADERS} ${UIC_FILES})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  # poi_editor_helpers.hpp の純関数の unit test。try_parse_finite_double / split_and_trim (#158)
  # に加え、未保存編集ガードの decide_config_reload_guard / should_confirm_overwrite (#399) を含む。
  # header は Qt/ROS 非依存 (stdlib + config_path_update_policy.hpp のみ)。
  ament_auto_add_gtest(test_poi_editor_helpers
    test/test_poi_editor_helpers.cpp
  )
  # config_path_update_policy.hpp の ConfigPathCallback 分岐判定 (Reinit/Refresh/Noop) の
  # unit test (#169)。Panel callback はこの判定を UI 更新へ写像するだけ。header は Qt/ROS 非依存。
  ament_auto_add_gtest(test_config_path_update_policy
    test/test_config_path_update_policy.cpp
  )
  # poi_editor_helpers.hpp に追加した ValidatePois の pose/tolerance セル判定・tag 排他判定の
  # unit test (#346)。ValidatePois() 本体 (poi_editor_validation.cpp) は QMessageBox 表示に
  # これらの判定結果を渡すだけなので Qt を起動せずに判定ロジックだけを安く gate できる。
  ament_auto_add_gtest(test_poi_editor_validation
    test/test_poi_editor_validation.cpp
  )
  # mapoi_panel_helpers.hpp の build_backend_badge_text (#400) の unit test。バッジ表示規則と
  # 「切断時に stale reason を出さない」契約を pin する。header は <string> のみ依存。
  ament_auto_add_gtest(test_mapoi_panel_helpers
    test/test_mapoi_panel_helpers.cpp
  )
endif()

# 将来のROS 2（Kilted Kaiju以降）と互換性を持たせるため
# ament_auto_package()
ament_auto_package(
  USE_SCOPED_HEADER_INSTALL_DIR
)
