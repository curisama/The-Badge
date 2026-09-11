# For checking — the device runs the apps itself and measures them. Normally commented out.
# add_compile_definitions(BADGE_APPBENCH=1)   # on, the device runs the apps itself and measures them
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-format)
project(badge_fw)

# 🚨 tinyusb looks for `tusb_config.h` **on its own include path** (tusb_option.h).
# But that component's CMakeLists knows nothing about our paths — just creating
# the file dies with "tusb_config.h: No such file". It is pushed in here.
# It has to be after project(), once the component library already exists.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main/usb")
