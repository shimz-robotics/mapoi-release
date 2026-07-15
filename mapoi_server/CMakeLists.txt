cmake_minimum_required(VERSION 3.5)
project(mapoi_server)

find_package(ament_cmake_auto REQUIRED)
ament_auto_find_build_dependencies()

# yaml-cpp: ament_auto does not reliably propagate the yaml-cpp link library
# on Jazzy (yaml-cpp 0.8 exports yaml-cpp::yaml-cpp, Humble's 0.7 uses yaml-cpp).
find_package(yaml-cpp REQUIRED)
if(TARGET yaml-cpp::yaml-cpp)
  set(_mapoi_yaml_cpp_target yaml-cpp::yaml-cpp)
else()
  set(_mapoi_yaml_cpp_target yaml-cpp)
endif()

ament_auto_add_executable(mapoi_server
  src/mapoi_server.cpp
  src/initial_pose_resolver.cpp
  src/map_state_store.cpp
)
target_link_libraries(mapoi_server ${_mapoi_yaml_cpp_target})

ament_auto_add_executable(mapoi_nav2_bridge
  src/mapoi_nav2_bridge.cpp
  src/mapoi_nav2_bridge_route.cpp
  src/mapoi_nav2_bridge_goal.cpp
  src/mapoi_nav2_bridge_map_switch.cpp
  src/mapoi_nav2_bridge_backend_status.cpp
)

ament_auto_add_executable(mapoi_amcl_localization_bridge
  src/mapoi_amcl_localization_bridge.cpp
)

ament_auto_add_executable(mapoi_rviz2_publisher
  src/mapoi_rviz2_publisher.cpp
)
target_link_libraries(mapoi_rviz2_publisher ${_mapoi_yaml_cpp_target})

# mapoi_gazebo_bridge: Gazebo Classic (gazebo_msgs) 用、Humble で build される。
# Jazzy では gazebo_msgs が存在しないため find_package QUIET で skip。
find_package(gazebo_msgs QUIET)
if(gazebo_msgs_FOUND)
  ament_auto_add_executable(mapoi_gazebo_bridge
    src/mapoi_gazebo_bridge.cpp
    src/initial_pose_resolver.cpp
  )
  target_link_libraries(mapoi_gazebo_bridge ${_mapoi_yaml_cpp_target})
  ament_target_dependencies(mapoi_gazebo_bridge gazebo_msgs)
  message(STATUS "Building mapoi_gazebo_bridge (Gazebo Classic)")
else()
  message(STATUS "Skipping mapoi_gazebo_bridge (gazebo_msgs not found)")
endif()

# mapoi_gz_bridge: gz-sim (ros_gz_interfaces) 用、Jazzy 以降で build される。
# Humble の image にも ros_gz_interfaces が含まれている場合があるが、
# Humble の rclcpp は create_client(name, rclcpp::QoS, group) シグネチャを持たないため、
# find_package が成功しても build は skip する必要がある。
# package.xml の condition と揃え、ROS_DISTRO != humble のときのみ build する。
find_package(ros_gz_interfaces QUIET)
if(ros_gz_interfaces_FOUND AND NOT "$ENV{ROS_DISTRO}" STREQUAL "humble")
  ament_auto_add_executable(mapoi_gz_bridge
    src/mapoi_gz_bridge.cpp
    src/initial_pose_resolver.cpp
  )
  target_link_libraries(mapoi_gz_bridge ${_mapoi_yaml_cpp_target})
  ament_target_dependencies(mapoi_gz_bridge ros_gz_interfaces)
  message(STATUS "Building mapoi_gz_bridge (gz-sim, ROS_DISTRO=$ENV{ROS_DISTRO})")
else()
  message(STATUS "Skipping mapoi_gz_bridge "
    "(ros_gz_interfaces_FOUND=${ros_gz_interfaces_FOUND}, ROS_DISTRO=$ENV{ROS_DISTRO})")
endif()


install(
  DIRECTORY
    launch
  DESTINATION share/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_cmake_pytest REQUIRED)
  find_package(launch_testing_ament_cmake REQUIRED)

  # Level 1: gtest unit tests
  ament_auto_add_gtest(test_nav2_bridge_unit
    test/test_nav2_bridge_unit.cpp
    src/mapoi_nav2_bridge.cpp
    src/mapoi_nav2_bridge_route.cpp
    src/mapoi_nav2_bridge_goal.cpp
    src/mapoi_nav2_bridge_map_switch.cpp
    src/mapoi_nav2_bridge_backend_status.cpp
  )
  target_compile_definitions(test_nav2_bridge_unit PRIVATE UNIT_TEST)

  # initial_pose_resolver は純関数なので test には resolver の cpp だけを link する
  # (#150 で mapoi_server.cpp 全体 link から軽量化、Node 依存を test から排除)。
  ament_auto_add_gtest(test_initial_pose_resolver
    test/test_initial_pose_resolver.cpp
    src/initial_pose_resolver.cpp
  )
  target_compile_definitions(test_initial_pose_resolver PRIVATE UNIT_TEST)
  # initial_pose_resolver.cpp は yaml-cpp 依存。本体 executable と同じく test binary にも明示 link
  # (#149 で見落とし、別 PC clean build で undefined reference 発覚 → hotfix)。
  # NOTE: humble の `ament_auto_add_gtest` 内で plain signature の target_link_libraries
  # が使われているため、ここも plain (= PUBLIC/PRIVATE/INTERFACE 無し) で揃える必要がある。
  # PRIVATE 化は CMake が「all-keyword or all-plain」混在を error にするため不可。
  target_link_libraries(test_initial_pose_resolver ${_mapoi_yaml_cpp_target})

  # #297: last-selected map 永続化 store の unit test。initial_pose_resolver と同じく
  # Node 非依存の純関数群なので store の cpp だけを link する (yaml-cpp も不要)。
  ament_auto_add_gtest(test_map_state_store
    test/test_map_state_store.cpp
    src/map_state_store.cpp
  )

  # System tag 定義は mapoi_server/system_tags.hpp を source of truth とする。
  # WebUI 側にも同 header との contract test を置き、言語横断の tag drift を検出する (#193)。
  ament_auto_add_gtest(test_system_tags_contract
    test/test_system_tags_contract.cpp
  )

  # Level 2: launch_testing integration tests
  # ROUTE-mode 経路は Nav2 follow_waypoints mock が前提なので別ファイルに分離 (#226)。
  # mock の MultiThreadedExecutor 立ち上げ + 複数 test の cancel/cleanup 待ちで
  # default 60s を超えやすい。連続 pause / landmark コントラスト test 追加 (#236) で
  # 7 test に増えたため 120s から 180s へ延長。
  add_launch_test(
    test/test_poi_event_route_integration.py
    TIMEOUT 180
  )
  # mapoi 主導 waypoint 到達モード (#243) は NavigateToPose mock 前提で別ファイルに分離。
  add_launch_test(
    test/test_poi_event_route_mapoi_arrival.py
    TIMEOUT 120
  )
  # mapoi モードの単発 Go (GOAL) 到達判定 (#261) も NavigateToPose mock 前提で別ファイルに分離。
  add_launch_test(
    test/test_poi_event_goal_mapoi_arrival.py
    TIMEOUT 120
  )
  # #211: mapoi/initialpose_poi の単一 writer 不変条件 + 新 service の publish/clear 検証。
  # mapoi_server + mapoi_nav2_bridge を起動するだけ (Nav2 mock 不要) で軽量。
  add_launch_test(
    test/test_multiwriter_initialpose_no_stale.py
    TIMEOUT 120
  )
  # #299 テスト残課題: operator map switch の LoadMap 成功 → request_initial_pose E2E。
  # 「POI 要求は LoadMap 成功後のみ」(#149/#184) を FakeMapServer (load_map mock) で pin する。
  add_launch_test(
    test/test_switch_map_loadmap_initialpose.py
    TIMEOUT 120
  )
  # #297: mapoi_server 再起動 E2E。state_path あり = 修正挙動 (last-selected map 復元 +
  # clear のみ、非空 POI の active push なし)、state_path なし = opt-out の従来挙動、を
  # 両方 pin する (PR #326 の characterization から assert 反転済み)。
  add_launch_test(
    test/test_server_restart_initialpose_relatch.py
    TIMEOUT 120
  )
  # #328: select_map の map_name 入力検証 (path traversal reject)。mapoi_server 単体で軽量。
  add_launch_test(
    test/test_select_map_validation.py
    TIMEOUT 60
  )
  # #339: 「受理したが未実行」経路の mapoi/nav/status 未 publish bug 再現。Nav2 mock 不要で軽量。
  add_launch_test(
    test/test_nav_status_rejected_paths.py
    TIMEOUT 60
  )
  # #339: get_pois_info service 自体が unreachable (mapoi_server 不在) な経路は別 fixture で分離。
  add_launch_test(
    test/test_nav_status_service_unavailable.py
    TIMEOUT 60
  )
  # #339 (Cursor review PR #352 round 2 high): 走行中の reject が status を上書きしないこと。
  # NavigateToPose mock 前提で別ファイルに分離。
  add_launch_test(
    test/test_nav_status_rejected_during_navigation.py
    TIMEOUT 120
  )
  # #353: get_route_pois backend の異常応答 (success=false / success=true & 空 pois_list) の
  # reject 経路。#342 で response に success/error_message が追加され決定論的 mock が可能に
  # なった。FakeRoutePoisServer (nav2_mocks.py) 前提で既存ファイルと mock 構成が異なるため分離
  # (docs/testing-policy.md 3節)。mapoi_server 不在で軽量。
  add_launch_test(
    test/test_nav_status_route_backend_failure.py
    TIMEOUT 60
  )

  # Install test config
  install(
    FILES test/test_mapoi_config.yaml
    DESTINATION share/${PROJECT_NAME}/test
  )
  # switch_map E2E / #297 再現 test 用の 2-map fixture (test/maps/test_map_{a,b})
  install(
    DIRECTORY test/maps
    DESTINATION share/${PROJECT_NAME}/test
  )
endif()

ament_auto_package(USE_SCOPED_HEADER_INSTALL_DIR)
