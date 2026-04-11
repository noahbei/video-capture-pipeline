cmake_minimum_required(VERSION 3.20)
project(video-capture-pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Dependency discovery ---

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0>=1.20
    gstreamer-video-1.0>=1.20
    glib-2.0>=2.68
)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

# toml++: try CMake config first, fall back to manual header search
find_package(tomlplusplus QUIET)
if(NOT tomlplusplus_FOUND)
    find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp
        HINTS /usr/include /usr/local/include
    )
    if(NOT TOMLPLUSPLUS_INCLUDE_DIR)
        message(FATAL_ERROR
            "toml++ not found. Install libtomlplusplus-dev or place toml++/toml.hpp "
            "under a system include directory.")
    endif()
    add_library(tomlplusplus::tomlplusplus INTERFACE IMPORTED)
    set_target_properties(tomlplusplus::tomlplusplus PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${TOMLPLUSPLUS_INCLUDE_DIR}"
    )
    message(STATUS "Found toml++ headers at ${TOMLPLUSPLUS_INCLUDE_DIR}")
endif()

# --- Shared static library (makes unit testing individual modules easy) ---
add_library(vcpcore STATIC
    src/config.cpp
    src/pipeline.cpp
    src/encryptor.cpp
    src/disk_monitor.cpp
    src/health.cpp
)
target_include_directories(vcpcore PUBLIC include)
target_link_libraries(vcpcore PUBLIC
    PkgConfig::GST
    OpenSSL::Crypto
    tomlplusplus::tomlplusplus
)
target_compile_options(vcpcore PRIVATE -Wall -Wextra -Wpedantic)

# --- Main capture binary ---
add_executable(vcpcapture src/main.cpp)
target_link_libraries(vcpcapture PRIVATE vcpcore)
target_compile_options(vcpcapture PRIVATE -Wall -Wextra -Wpedantic)

# --- Decrypt tool ---
add_executable(vcpdecrypt tools/vcpdecrypt.cpp)
target_link_libraries(vcpdecrypt PRIVATE vcpcore)
target_compile_options(vcpdecrypt PRIVATE -Wall -Wextra -Wpedantic)

# --- Tests ---
enable_testing()
add_subdirectory(tests)

# --- Install ---
install(TARGETS vcpcapture vcpdecrypt RUNTIME DESTINATION bin)
install(FILES config.toml.example DESTINATION share/vcpcapture)
