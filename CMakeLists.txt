cmake_minimum_required(VERSION 3.16)
project(scand LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git
                         GIT_TAG 99f140d222152fa7332b0ae433ad899086ece3fd)
FetchContent_MakeAvailable(cpr)

find_package(Boost REQUIRED COMPONENTS system json)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)

# Библиотека inih (C-файл)
add_library(inih STATIC third_party/inih/ini.c)
target_include_directories(inih PUBLIC third_party/inih)


file(GLOB_RECURSE ALL_SOURCES "src/*.cpp" "include/*.hpp")

# Создание исполняемого файла (выполняется ОДИН раз)
add_executable(scand ${ALL_SOURCES})

target_include_directories(scand PRIVATE include)

target_link_libraries(scand PRIVATE
    inih
    Boost::system
    Boost::json
    Threads::Threads
    cpr::cpr
    PkgConfig::PQXX
)

target_compile_options(scand PRIVATE -Wall -Wextra -O0 -g)
target_link_options(scand PRIVATE -rdynamic)

configure_file(
    ${CMAKE_SOURCE_DIR}/config.ini
    ${CMAKE_BINARY_DIR}/config.ini
    COPYONLY
)
