set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} \
    -g -pthread \
    -ftemplate-backtrace-limit=1 -Wfatal-errors \
    -Wall -Wextra \
    -Wconversion -Wdeprecated-declarations -Wstrict-aliasing \
    -Wlogical-op -Wmissing-declarations -Wpedantic -Wshadow \
    -Wsuggest-attribute=format -Wswitch -Wpointer-arith \
    -Wmissing-include-dirs -Winvalid-pch -Wmissing-format-attribute \
    -Wfloat-equal -Wdouble-promotion -Winit-self -Weffc++ \
    -Woverloaded-virtual -Wsign-promo -Wvla -Wredundant-decls \
    -Wcast-align -Wcast-qual -Wold-style-cast \
    -fprofile-arcs -ftest-coverage \
    -fsanitize=address -fsanitize=undefined")
    
link_libraries("-static-libasan -fsanitize=address -fsanitize=undefined -fprofile-arcs -ftest-coverage")

include_directories(${PROJECT_SOURCE_DIR}/include/)

add_executable(fn_test    ${PROJECT_SOURCE_DIR}/test/test.cpp)
add_executable(calendar   ${PROJECT_SOURCE_DIR}/test/calendar.cpp)
add_executable(aln_filter ${PROJECT_SOURCE_DIR}/test/aln_filter.cpp)

enable_testing()
add_test(fn_test fn_test)
