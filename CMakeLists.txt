cmake_minimum_required(VERSION 3.16)
project(scand LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(CPR_SKIP_LIBPSL_BUILD TRUE CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")

FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 99f140d222152fa7332b0ae433ad899086ece3fd
)

set(CPR_BUILD_REQS_FROM_SOURCE OFF CACHE INTERNAL "")
set(CMAKE_USE_LIBPSL OFF CACHE INTERNAL "")
set(CURL_USE_LIBPSL OFF CACHE INTERNAL "")
set(CPR_BUILD_TESTS OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(cpr)

# 2. inih (INI Not Invented Here)
set(INIH_BUILD_TESTS OFF CACHE INTERNAL "")
FetchContent_Declare(
    inih_download
    GIT_REPOSITORY https://github.com/benhoyt/inih.git
    GIT_TAG r53
)
FetchContent_GetProperties(inih_download)
if(NOT inih_download_POPULATED)
    FetchContent_Populate(inih_download)
endif()


# 3. libpqxx (Официальный драйвер PostgreSQL для C++)
# Версия 7.9.2 содержит исправление бага совместимости со свежим CMake!
set(SKIP_BUILD_TESTS ON CACHE INTERNAL "")
set(CMAKE_DISABLE_FIND_PACKAGE_libpqxx TRUE)
FetchContent_Declare(
    libpqxx
    GIT_REPOSITORY https://github.com/jtv/libpqxx.git
    GIT_TAG 7.9.2        # <- ОБНОВИЛИ ТЕГ ДО 7.9.2 ДЛЯ ИСПРАВЛЕНИЯ ОШИБКИ CMAKE
)
FetchContent_MakeAvailable(libpqxx)

# ==============================================================================
# Поиск системных библиотек ОС
# ==============================================================================
find_package(Boost REQUIRED COMPONENTS system json)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)


add_library(my_inih STATIC
    ${inih_download_SOURCE_DIR}/ini.c
)
target_include_directories(my_inih PUBLIC ${inih_download_SOURCE_DIR})

# ==============================================================================
# Конфигурация таргета вашего проекта
# ==============================================================================

file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(scand ${ALL_SOURCES})

target_include_directories(scand BEFORE PRIVATE
    ${libpqxx_SOURCE_DIR}/include
    ${libpqxx_BINARY_DIR}/include
)

target_include_directories(scand PRIVATE
    include
)

target_link_libraries(scand PRIVATE
    my_inih                # или INIReader (в зависимости от вашего C/C++ API)
    Boost::system
    Boost::json
    Threads::Threads
    cpr::cpr
    pqxx         # Нативный статический таргет libpqxx
    OpenSSL::Crypto
)

# Флаги компиляции
target_compile_options(scand PRIVATE -Wall -Wextra -O2 -g -Wno-deprecated-declarations)
target_link_options(scand PRIVATE -rdynamic)

# Копирование конфига в папку сборки
configure_file(
    ${CMAKE_SOURCE_DIR}/config.ini
    ${CMAKE_BINARY_DIR}/config.ini
    COPYONLY
)
