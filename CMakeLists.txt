cmake_minimum_required(VERSION 3.1)
if(POLICY CMP0025)
# MUST call before project() and enable_language
  cmake_policy(SET CMP0025 NEW) # since 3.0, prefer AppleClang instead of Clang.
endif()

project(mdk-r3d VERSION 0.19.0 DESCRIPTION "mdk r3d plugin")
set(PROJECT_VERSION_TWEAK 0)
if(CMAKE_PROJECT_NAME STREQUAL mdk) # build in source tree
    include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR})
endif()

set(R3DSDK "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE STRING "R3D SDK dir")

find_package(Threads)
link_libraries(Threads::Threads)

#include(cmake/FindMDK.cmake) # TODO: shared

if(APPLE)
  set(CMAKE_SHARED_MODULE_SUFFIX ".dylib") # default is so
endif()

add_library(${PROJECT_NAME} MODULE
    R3DReader.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    #VERSION ${PROJECT_VERSION} # -current_version can not be applied for MODULE
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME ${PROJECT_NAME}
    CLEAN_DIRECT_OUTPUT 1
  )

target_include_directories(${PROJECT_NAME} PRIVATE ${R3DSDK}/Include)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
math(EXPR BITS "8 * ${CMAKE_SIZEOF_VOID_P}")
if(APPLE)
  target_link_directories(${PROJECT_NAME} PRIVATE ${R3DSDK}/Lib/mac64)
  target_link_libraries(${PROJECT_NAME} PRIVATE R3DSDK-libcpp)
  # set rpath
elseif(WIN32)
  target_link_directories(${PROJECT_NAME} PRIVATE ${R3DSDK}/Lib/win${BITS})
  target_link_libraries(${PROJECT_NAME} PRIVATE R3DSDK-2017MD$<$<CONFIG:DEBUG>:d>)
else()
  target_link_directories(${PROJECT_NAME} PRIVATE sdk/Lib/linux${BITS})
  target_link_libraries(${PROJECT_NAME} PRIVATE R3DSDK-cpp11 dl)
endif()

setup_mdk_plugin(${PROJECT_NAME})
