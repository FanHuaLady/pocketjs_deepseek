cmake_minimum_required(VERSION 3.16)

project(pocketjs_gif_rv1106
    VERSION 0.1.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

set(QUICKJS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs"
    CACHE PATH "QuickJS source directory"
)
set(POCKETJS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pocketjs"
    CACHE PATH "PocketJS source directory"
)
set(POCKETJS_CORE_LIB
    "${POCKETJS_DIR}/engine/ui-cabi/target/armv7-unknown-linux-uclibceabihf/release/libpocketjs_symbian_core.a"
    CACHE FILEPATH "PocketJS Rust UI core static library"
)

set(POCKETJS_RUNTIME_C
    "${POCKETJS_DIR}/engine/quickjs-c/pocket_runtime.c"
)

add_executable(pocket_host
    src/main.c
    src/device/framebuffer.c
    src/device/orientation.c
    src/device/touchscreen.c
    src/runtime/guest_file.c
    src/runtime/host_log.c
    src/runtime/pet_config.c
    src/runtime/pet_host.c
    "${POCKETJS_RUNTIME_C}"
)

target_include_directories(pocket_host
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${QUICKJS_DIR}"
        "${POCKETJS_DIR}/engine/quickjs-c"
        "${POCKETJS_DIR}/engine/ui-cabi/include"
        "${POCKETJS_DIR}/contracts/generated"
)

target_compile_definitions(pocket_host
    PRIVATE
        POCKETJS_TARGET_ID="rv1106"
        POCKETJS_HOST_ABI=1
        POCKET_RASTER_DENSITY=1
        POCKET_RUNTIME_HARNESS
)

target_compile_options(pocket_host
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_link_libraries(pocket_host
    PRIVATE
        "${QUICKJS_DIR}/libquickjs.a"
        "${POCKETJS_CORE_LIB}"
        m
        pthread
        dl
)

add_custom_target(pocketjs-core
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_core_rv1106.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building PocketJS Rust UI core"
)
add_dependencies(pocket_host pocketjs-core)

add_custom_target(pocketjs-guest
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_guest_rv1106.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building PocketJS Solid guest bundle"
)

message(STATUS "Target system: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Target processor: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "PocketJS core: ${POCKETJS_CORE_LIB}")
