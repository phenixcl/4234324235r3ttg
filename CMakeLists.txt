cmake_minimum_required(VERSION 3.20)
project(foo_yandex_music)

set(FB2K_SDK_DIR "C:/foobar2000-sdk") # поменяй на свой путь
add_subdirectory(${FB2K_SDK_DIR} fb2k_sdk)

find_package(nlohmann_json CONFIG REQUIRED)

add_library(foo_yandex_music SHARED main.cpp)
target_link_libraries(foo_yandex_music PRIVATE foobar2000_component_client foobar2000_sdk_helpers nlohmann_json::nlohmann_json winhttp)
set_target_properties(foo_yandex_music PROPERTIES SUFFIX ".dll")
