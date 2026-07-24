cmake_minimum_required(VERSION 3.20)
project(rp2350_hid_bridge_sdk LANGUAGES CXX)

enable_testing()
find_package(Threads REQUIRED)

add_library(rp2350_hid_bridge INTERFACE)
target_include_directories(rp2350_hid_bridge INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(rp2350_hid_bridge INTERFACE cxx_std_17)
target_link_libraries(rp2350_hid_bridge INTERFACE Threads::Threads)

if(MSVC)
    target_compile_options(rp2350_hid_bridge INTERFACE /utf-8)
endif()

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE rp2350_hid_bridge)
add_test(NAME protocol COMMAND test_protocol)

add_executable(basic_example examples/basic.cpp)
target_link_libraries(basic_example PRIVATE rp2350_hid_bridge)

add_executable(script_example examples/script_demo.cpp)
target_link_libraries(script_example PRIVATE rp2350_hid_bridge)
