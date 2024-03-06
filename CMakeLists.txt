cmake_minimum_required(VERSION 3.15)

project(mdk-r3d VERSION 0.25.0 DESCRIPTION "mdk r3d plugin")
set(PROJECT_VERSION_TWEAK 0)

set(R3DSDK "${CMAKE_CURRENT_SOURCE_DIR}/r3dsdk" CACHE STRING "R3D SDK dir")
set(MDKSDK "${CMAKE_CURRENT_SOURCE_DIR}/mdk-sdk" CACHE STRING "libmdk SDK dir")

if(NOT CMAKE_PROJECT_NAME STREQUAL mdk) # not build in source tree
  list(APPEND CMAKE_MODULE_PATH ${MDKSDK}/lib/cmake)
endif()
find_package(MDK)

if(NOT EXISTS ${R3DSDK}/Include/R3DSDK.h)
  message("R3DSDK.h not found in ${R3DSDK}/Include")
  return()
endif()
message("Build with R3DSDK: ${R3DSDK}")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads)
link_libraries(Threads::Threads)

setup_mdk_plugin(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
    R3DReader.cpp
    R3DCxxAbi.cpp
    Debayer.cpp
)
if(APPLE)
  target_sources(${PROJECT_NAME} PRIVATE MetalDebayer.mm)
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES
    #VERSION ${PROJECT_VERSION} # -current_version can not be applied for MODULE
    OUTPUT_NAME ${PROJECT_NAME}
  )

target_include_directories(${PROJECT_NAME} PRIVATE ${R3DSDK}/Include)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
math(EXPR BITS "8 * ${CMAKE_SIZEOF_VOID_P}")
if(APPLE)
  target_link_directories(${PROJECT_NAME} PRIVATE ${R3DSDK}/Lib/mac64)
  target_link_libraries(${PROJECT_NAME} PRIVATE R3DSDK-libcpp "-framework Metal" "-framework CoreGraphics" "-framework CoreFoundation")
  target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/sdk/lib")
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE -weak-lREDDecoder -weak-lREDR3D -weak-lREDMetal -weak-lREDOpenCL)
elseif(WIN32)
  set(VCRT_TYPE MD)
  if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "MultiThreaded.*DLL")
    set(VCRT_TYPE MT)
  endif()
  target_link_directories(${PROJECT_NAME} PRIVATE ${R3DSDK}/Lib/win${BITS})
  target_link_libraries(${PROJECT_NAME} PRIVATE R3DSDK-2017${VCRT_TYPE}$<$<CONFIG:DEBUG>:d>)
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

if(TARGET cppcompat) # requires https://github.com/wang-bin/cppcompat
  target_link_libraries(${PROJECT_NAME} PRIVATE cppcompat)
endif()
