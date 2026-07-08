cmake_minimum_required(VERSION 3.18)
set(MOD_TARGET SMT5VAP)
project(${MOD_TARGET})

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
)
target_link_libraries(${MOD_TARGET} PUBLIC UE4SS)
target_link_libraries(${MOD_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/External/APCpp/lib/APCpp.lib
)