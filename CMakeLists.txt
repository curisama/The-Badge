# 검증용 — 기기가 스스로 앱을 돌려보고 잰다. 평소엔 주석.
# add_compile_definitions(BADGE_APPBENCH=1)   # 켜면 기기가 스스로 앱을 돌려 잰다
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-format)
project(badge_fw)

# 🚨 tinyusb 는 `tusb_config.h` 를 **제 포함경로에서** 찾는다(tusb_option.h).
# 그런데 그 컴포넌트의 CMakeLists 는 우리 쪽 경로를 모른다 — 파일만 만들어
# 두면 "tusb_config.h: No such file" 로 죽는다. 여기서 끼워 넣는다.
# project() 뒤라야 컴포넌트 라이브러리가 이미 만들어져 있다.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main/usb")
