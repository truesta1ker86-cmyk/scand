cmake_minimum_required(VERSION 3.16)
project(scand LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# CPR
FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 99f140d222152fa7332b0ae433ad899086ece3fd
)
FetchContent_MakeAvailable(cpr)

# inih — берём из benhoyt/inih, тег r53 (там есть ini.c)
FetchContent_Declare(
    inih
    GIT_REPOSITORY https://github.com/benhoyt/inih.git
    GIT_TAG r53
)
FetchContent_MakeAvailable(inih)

find_package(Boost REQUIRED COMPONENTS system json)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)

# Правильная библиотека inih: используем файлы из ${inih_SOURCE_DIR}
add_library(inih STATIC
    ${inih_SOURCE_DIR}/ini.c
)
target_include_directories(inih PUBLIC ${inih_SOURCE_DIR})

file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(scand ${ALL_SOURCES})

target_include_directories(scand PRIVATE include)

target_link_libraries(scand PRIVATE
    inih
    Boost::system
    Boost::json
    Threads::Threads
    cpr::cpr
    PkgConfig::PQXX
    OpenSSL::Crypto
)

target_compile_options(scand PRIVATE -Wall -Wextra -O2 -g)
target_link_options(scand PRIVATE -rdynamic)

configure_file(
    ${CMAKE_SOURCE_DIR}/config.ini
    ${CMAKE_BINARY_DIR}/config.ini
    COPYONLY
)
