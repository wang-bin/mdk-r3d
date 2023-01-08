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
if(NOT EXISTS ${R3DSDK}/Include/R3DSDK.h)
  return()
endif()
message("Build with R3DSDK: ${R3DSDK}")

find_package(Threads)
link_libraries(Threads::Threads)

include(cmake/FindMDK.cmake)

add_library(${PROJECT_NAME} MODULE
    R3DReader.cpp
    R3DCxxAbi.cpp
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
  target_link_directories(${PROJECT_NAME} PRIVATE ${R3DSDK}/Lib/linux${BITS})
  target_link_libraries(${PROJECT_NAME} PRIVATE R3DSDKPIC-cpp11 dl)
  if(CMAKE_CXX_COMPILER_ID MATCHES Clang)
    string(FIND "${CMAKE_CXX_FLAGS}" "-stdlib=libc++" HAS_LIBCXX)
    message("R3D HAS_LIBCXX: ${HAS_LIBCXX}. CMAKE_SYSROOT: ${CMAKE_SYSROOT}")
    if(HAS_LIBCXX GREATER_EQUAL 0)
      set_property(SOURCE R3DCxxAbi.cpp APPEND PROPERTY COMPILE_FLAGS "-stdlib=libstdc++")
      file(GLOB_RECURSE GNUSTL_SOS LIST_DIRECTORIES false "${CMAKE_SYSROOT}/usr/lib/gcc/x86_64-linux-gnu/*/libstdc++.so") # we only build x64
      if(GNUSTL_SOS)
        list(GET GNUSTL_SOS -1 GNUSTL_SO)
        message("R3D GNUSTL_SO: ${GNUSTL_SO}")
        #target_link_libraries(${PROJECT_NAME} PRIVATE ${GNUSTL_SO}) # will convert libstdc++ path to -lstdc++
        set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${GNUSTL_SO}")
      endif()
    endif()
  endif()
endif()

setup_mdk_plugin(${PROJECT_NAME})
