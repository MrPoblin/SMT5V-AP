cmake_minimum_required(VERSION 3.18)
set(MOD_TARGET SMT5VAP)
project(${MOD_TARGET})

# Build jsoncpp from source so its symbols are available to the mod.
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_CMAKE_PACKAGE OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_STRICT_ISO ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/External/APCpp/jsoncpp jsoncpp_build)
set_property(TARGET jsoncpp_static PROPERTY POSITION_INDEPENDENT_CODE ON)

# Auto-discover root/src files (relative paths)
file(GLOB ROOT_SRC CONFIGURE_DEPENDS "*.cpp")
file(GLOB_RECURSE SRC_SRC CONFIGURE_DEPENDS "src/*.cpp")

file(GLOB ROOT_HDR CONFIGURE_DEPENDS "*.hpp" "*.h")
file(GLOB_RECURSE SRC_HDR CONFIGURE_DEPENDS "src/*.hpp" "src/*.h")

set(SOURCES
    ${ROOT_SRC} ${SRC_SRC}
    ${ROOT_HDR} ${SRC_HDR}
)

add_library(${MOD_TARGET} SHARED ${SOURCES})
target_include_directories(${MOD_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/External/APCpp
    ${CMAKE_CURRENT_SOURCE_DIR}/External/APCpp/jsoncpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../Output/_deps/polyhook2-src
)
target_link_libraries(${MOD_TARGET} PUBLIC UE4SS)
# PolyHook2 + its backend libs are prebuilt per-config under Output/<CONFIG>/lib.
# This is a multi-config (VS) generator, so the active config is only known at
# build time -> use the $<CONFIG> generator expression so the linked libs always
# match the --config passed to `cmake --build` (Dev / Debug / Shipping / Test).
set(SMT5V_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Output")
target_link_libraries(${MOD_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/External/APCpp/build/$<IF:$<CONFIG:Game__Debug__Win64>,Debug,Release>/APCpp.lib
    jsoncpp_static
    ${SMT5V_OUTPUT_DIR}/$<CONFIG>/lib/PolyHook_2.lib
    ${SMT5V_OUTPUT_DIR}/$<CONFIG>/lib/asmjit.lib
    ${SMT5V_OUTPUT_DIR}/$<CONFIG>/lib/Zydis.lib
    ${SMT5V_OUTPUT_DIR}/$<CONFIG>/lib/Zycore.lib
)