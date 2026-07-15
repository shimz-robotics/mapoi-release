cmake_minimum_required(VERSION 3.5)
project(mapoi_webui)

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_python REQUIRED)

ament_python_install_package(${PROJECT_NAME})

install(
  DIRECTORY
    web
    launch
  DESTINATION share/${PROJECT_NAME}
)

install(PROGRAMS
  mapoi_webui/mapoi_webui_node.py
  DESTINATION lib/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_cmake_pytest REQUIRED)
  # yaml_handler._round_poi_floats の桁数契約 pin (#242)。frontend (poi-filter.js
  # roundTo + POSE_*_DIGITS / TOLERANCE_*_DIGITS) と必ず一致させる。
  ament_add_pytest_test(test_yaml_handler_round
    test/test_yaml_handler_round.py
    TIMEOUT 10
  )
  # yaml_handler.compute_config_version の決定性 / 変化検出 pin (#241 楽観的競合検出)。
  ament_add_pytest_test(test_yaml_handler_version
    test/test_yaml_handler_version.py
    TIMEOUT 10
  )
  # Flask endpoint レベルでの `/api/pois` POST 楽観的競合検出契約 (#241 / #246)。
  # 純関数 test (test_yaml_handler_version) と分けて、409 + code + current_version 経路を pin。
  ament_add_pytest_test(test_api_save_pois_version_conflict
    test/test_api_save_pois_version_conflict.py
    TIMEOUT 10
  )
  # navigation API endpoint 契約 (#199): /api/nav/switch-map validation (400/404)、
  # /api/mode・/api/nav/status の payload shape を Flask test_client で固定。
  ament_add_pytest_test(test_api_nav_endpoints
    test/test_api_nav_endpoints.py
    TIMEOUT 10
  )
  # SSE / config publish 経路 (#135 (B), #173): config_path_callback の map 名抽出 +
  # broadcast 判定、_broadcast_sse_event の fanout / 形状、/api/events の header +
  # client 登録・配信・切断 discard lifecycle を pin (#193 残課題のカバレッジ穴埋め)。
  ament_add_pytest_test(test_api_sse_config_events
    test/test_api_sse_config_events.py
    TIMEOUT 15
  )
  # /api/maps/select の REST 契約 (#343): resolved_initial_poi_name key (srv rename の
  # REST 側 mirror) と 400/503 経路を Flask test_client で固定。
  ament_add_pytest_test(test_api_select_map
    test/test_api_select_map.py
    TIMEOUT 10
  )
endif()

ament_package()
