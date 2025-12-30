cmake_minimum_required(VERSION 3.8)
project(hector_testing_utils)

# Default to C++17
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(std_msgs REQUIRED)

add_library(hector_testing_utils INTERFACE)
target_include_directories(hector_testing_utils INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

ament_target_dependencies(hector_testing_utils INTERFACE
  rclcpp
  rclcpp_action
)

install(TARGETS hector_testing_utils
  EXPORT export_${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include
)

install(DIRECTORY config launch
  DESTINATION share/${PROJECT_NAME}
  OPTIONAL
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(example_interfaces REQUIRED)

  ament_add_gtest(test_hector_testing_utils test/test_hector_testing_utils.cpp)
  target_link_libraries(test_hector_testing_utils hector_testing_utils)
  ament_target_dependencies(test_hector_testing_utils
    rclcpp
    rclcpp_action
    example_interfaces
    std_msgs
  )
  target_compile_definitions(
    test_hector_testing_utils
    PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test")
endif()

ament_export_targets(export_${PROJECT_NAME} HAS_LIBRARY_TARGET)
ament_export_dependencies(rclcpp rclcpp_action)
ament_export_include_directories(include)

ament_package()
