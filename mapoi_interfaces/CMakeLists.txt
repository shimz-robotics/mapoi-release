cmake_minimum_required(VERSION 3.8)
project(mapoi_interfaces)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(geometry_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Tolerance.msg"
  "msg/PointOfInterest.msg"
  "msg/PoiEvent.msg"
  "msg/TagDefinition.msg"
  "msg/InitialPoseRequest.msg"
  "msg/NavigationBackendStatus.msg"
  "msg/LocalizationBackendStatus.msg"
  "srv/SelectMap.srv"
  "srv/RequestInitialPose.srv"
  "srv/GetMapsInfo.srv"
  "srv/GetPoisInfo.srv"
  "srv/GetRoutePois.srv"
  "srv/GetRoutesInfo.srv"
  "srv/GetTagDefinitions.srv"
  DEPENDENCIES geometry_msgs builtin_interfaces
)

ament_package()
