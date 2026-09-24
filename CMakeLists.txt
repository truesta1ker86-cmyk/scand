cmake_minimum_required(VERSION 3.16)
project(scand LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ==============================================================================
# Зависимости через FetchContent (скачиваются автоматически)
# ==============================================================================

# 1. CPR (C++ Requests)
FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 99f140d222152fa7332b0ae433ad899086ece3fd
)
# Выключаем libpsl, чтобы обойти баг с meson на воркерах и Linux
set(CMAKE_USE_LIBPSL OFF CACHE INTERNAL "")
set(CPR_BUILD_TESTS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(cpr)

# 2. inih (INI Not Invented Here)
set(INIH_BUILD_TESTS OFF CACHE INTERNAL "")
FetchContent_Declare(
    inih
    GIT_REPOSITORY https://github.com/benhoyt/inih.git
    GIT_TAG r53
)
FetchContent_MakeAvailable(inih)

# 3. libpqxx (Официальный драйвер PostgreSQL для C++)
# Версия 7.9.2 содержит исправление бага совместимости со свежим CMake!
set(SKIP_BUILD_TESTS ON CACHE INTERNAL "")
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

# ==============================================================================
# Конфигурация таргета вашего проекта
# ==============================================================================

file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(scand ${ALL_SOURCES})

target_include_directories(scand PRIVATE include)

target_link_libraries(scand PRIVATE
    inih                # или INIReader (в зависимости от вашего C/C++ API)
    Boost::system
    Boost::json
    Threads::Threads
    cpr::cpr
    pqxx_static         # Нативный статический таргет libpqxx
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
