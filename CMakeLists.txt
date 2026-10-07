cmake_minimum_required(VERSION 3.10)
project(reactor_http_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
include(CTest)
option(ENABLE_SANITIZERS "启用 AddressSanitizer 与 UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined -no-pie")
endif()

add_library(network STATIC
    source/base/NetworkInit.cc
    source/net/Buffer.cc
    source/net/Socket.cc
    source/net/Channel.cc
    source/net/Poller.cc
    source/net/TimerWheel.cc
    source/net/EventLoop.cc
    source/net/LoopThreadPool.cc
    source/net/Connection.cc
    source/net/Acceptor.cc
    source/net/TcpServer.cc
)
target_include_directories(network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(network PUBLIC Threads::Threads)

add_library(http_protocol STATIC
    source/http/Util.cc
    source/http/HttpRequest.cc
    source/http/HttpResponse.cc
    source/http/HttpContext.cc
    source/http/HttpServer.cc
)
target_link_libraries(http_protocol PUBLIC network)

add_executable(echo_server source/echo/main.cc)
target_link_libraries(echo_server PRIVATE network)

add_executable(http_server source/http/main.cc)
target_link_libraries(http_server PRIVATE http_protocol)

if(BUILD_TESTING)
    find_package(PythonInterp 3 REQUIRED)
    add_executable(http_components_test
        tests/http_components_main.cc
        tests/http_components_second_tu.cc)
    target_link_libraries(http_components_test PRIVATE http_protocol)
    add_test(NAME http_components COMMAND http_components_test)
    add_executable(network_components_test tests/network_components.cc)
    target_link_libraries(network_components_test PRIVATE network)
    add_test(NAME network_components COMMAND network_components_test)
    add_executable(network_test_server tests/network_server.cc)
    target_link_libraries(network_test_server PRIVATE network)
    add_test(NAME network_integration COMMAND ${PYTHON_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network_integration.py $<TARGET_FILE:network_test_server>)
    add_executable(http_boundaries_test tests/http_boundaries.cc)
    target_link_libraries(http_boundaries_test PRIVATE http_protocol)
    add_test(NAME http_boundaries COMMAND http_boundaries_test)
    set_tests_properties(network_components network_integration PROPERTIES TIMEOUT 20)
    add_test(NAME echo_smoke
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.py
            echo $<TARGET_FILE:echo_server>)
    add_test(NAME http_smoke
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.py
            http $<TARGET_FILE:http_server>)
    set_tests_properties(echo_smoke http_smoke PROPERTIES TIMEOUT 15)
endif()
