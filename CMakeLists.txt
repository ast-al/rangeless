cmake_minimum_required(VERSION 3.1)
project(fn)


set(fn_VERSION_MAJOR 1)
set(fn_VERSION_MINOR 0)

add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(
    ${PROJECT_NAME}
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
              $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS} -O0 -ggdb3 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -O2")


set(fn_INCLUDES ${PROJECT_SOURCE_DIR}/include/fn.hpp)


set(CppCheck_REPORT ${PROJECT_BINARY_DIR}/cppcheck.report)
add_custom_command(
    OUTPUT  ${CppCheck_REPORT}
    COMMAND cppcheck ${fn_INCLUDES} --enable=all --force --inconclusive &>cppcheck.report
)
add_custom_target(cppcheck DEPENDS ${CppCheck_REPORT})
set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES ${CppCheck_REPORT})

add_subdirectory(test)
