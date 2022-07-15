cmake_minimum_required(VERSION 3.5)
project(rviz_plugin_zed_od)

# Default to C++14
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Qt5 boilerplate options from http://doc.qt.io/qt-5/cmake-manual.html
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

find_package(ament_cmake REQUIRED)
find_package(pluginlib REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Test Concurrent)
find_package(rclcpp REQUIRED)
find_package(rviz_common REQUIRED)
find_package(rviz_default_plugins REQUIRED)
find_package(rviz_ogre_vendor REQUIRED)
find_package(rviz_rendering REQUIRED)
find_package(zed_interfaces REQUIRED)


include_directories(include)

## This plugin includes Qt widgets, so we must include Qt.
## We'll use the version that rviz used so they are compatible.
# if(rviz_QT_VERSION VERSION_LESS "5")
#   message(STATUS "Using Qt4 based on the rviz_QT_VERSION: ${rviz_QT_VERSION}")
#   find_package(Qt4 ${rviz_QT_VERSION} EXACT REQUIRED QtCore QtGui)
#   ## pull in all required include dirs, define QT_LIBRARIES, etc.
#   include(${QT_USE_FILE})
# else()
#   message(STATUS "Using Qt5 based on the rviz_QT_VERSION: ${rviz_QT_VERSION}")
#   find_package(Qt5 ${rviz_QT_VERSION} EXACT REQUIRED Core Widgets)
#   ## make target_link_libraries(${QT_LIBRARIES}) pull in all required dependencies
#   set(QT_LIBRARIES Qt5::Widgets)
# endif()

include_directories(src/plugin/include)

## Here we specify the list of source files.
## The generated MOC files are included automatically as headers.
set(SRC_FILES
  src/plugin/src/zed_od_display.cpp
  src/plugin/src/zed_od_info.cpp
)

set(INC_FILES
  src/plugin/include/zed_od_display.hpp
  src/plugin/include/zed_od_info.hpp
)
set(dependencies
  pluginlib
  Qt5
  rclcpp
  rviz_common
  rviz_default_plugins
  rviz_ogre_vendor
  rviz_rendering
  zed_interfaces
)

## An rviz plugin is just a shared library, so here we declare the
## library to be called ``${PROJECT_NAME}`` (which is
## "rviz_plugin_zed_od", and specify the list of source files we collected above
## in ``${SRC_FILES}`` and ``${INC_FILES}``.
add_library(${PROJECT_NAME} SHARED
  ${SRC_FILES}
  ${INC_FILES}
)

target_include_directories(${PROJECT_NAME} PUBLIC
  ${zed_interfaces_INCLUDE_DIRS}
  ${OGRE_INCLUDE_DIRS}
  ${Qt5Widgets_INCLUDE_DIRS}
)


ament_target_dependencies(${PROJECT_NAME}
  ${dependencies}
)

## Link the executable with whatever Qt libraries have been defined by
## the ``find_package(Qt4 ...)`` line above, or by the
## ``set(QT_LIBRARIES Qt5::Widgets)``, and with whatever libraries
## catkin has included.
##
## Although this puts "rviz_plugin_zed_od" as the name of the library, cmake knows it is a
## library and names the actual file something like
## "librviz_plugin_zed_od.so", or whatever is appropriate for your
## particular OS.
target_link_libraries(${PROJECT_NAME} rviz_common::rviz_common)

# prevent pluginlib from using boost
target_compile_definitions(${PROJECT_NAME} PUBLIC "PLUGINLIB__DISABLE_BOOST_FUNCTIONS")

# Causes the visibility macros to use dllexport rather than dllimport,
# which is appropriate when building the dll but not consuming it.
# TODO: Make this specific to this project (not rviz default plugins)
target_compile_definitions(${PROJECT_NAME} PRIVATE "RVIZ_DEFAULT_PLUGINS_BUILDING_LIBRARY")


pluginlib_export_plugin_description_file(rviz_common plugin_description.xml)

## Install rules

install(
  TARGETS ${PROJECT_NAME}
  EXPORT ${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

install(
  DIRECTORY src/plugin/include/
  DESTINATION include/
)

ament_export_include_directories(src/plugin/include)
ament_export_targets(${PROJECT_NAME} HAS_LIBRARY_TARGET)
ament_export_dependencies(
  Qt5
  rviz_common
  rclcpp
  zed_interfaces
)

ament_package()
