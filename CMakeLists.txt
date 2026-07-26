cmake_minimum_required(VERSION 3.20)
project(rp2350_hid_bridge_sdk LANGUAGES CXX)

enable_testing()
find_package(Threads REQUIRED)

add_library(rp2350_hid_bridge SHARED
    src/c_api.cpp
)
target_compile_definitions(rp2350_hid_bridge PRIVATE RP2350_HID_BRIDGE_BUILD_DLL)
target_include_directories(rp2350_hid_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(rp2350_hid_bridge PUBLIC cxx_std_17)
target_link_libraries(rp2350_hid_bridge PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(rp2350_hid_bridge PRIVATE setupapi)
endif()

if(MSVC)
    target_compile_options(rp2350_hid_bridge PUBLIC /utf-8)
endif()

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE rp2350_hid_bridge)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_c_api
    tests/test_c_api.cpp
    src/c_api.cpp
)
target_include_directories(test_c_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(test_c_api PRIVATE cxx_std_17)
target_compile_definitions(test_c_api PRIVATE
    RP2350_HID_BRIDGE_BUILD_DLL
    RP2350_HID_BRIDGE_TESTING
)
target_link_libraries(test_c_api PRIVATE Threads::Threads)
add_test(NAME c_api COMMAND test_c_api)

add_executable(basic_example examples/basic.cpp)
target_link_libraries(basic_example PRIVATE rp2350_hid_bridge)
add_test(NAME dll_smoke COMMAND basic_example --abi-smoke)

add_executable(script_example examples/script_demo.cpp)
target_link_libraries(script_example PRIVATE rp2350_hid_bridge)
