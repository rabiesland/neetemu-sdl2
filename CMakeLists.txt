cmake_minimum_required(VERSION 3.16)
project(neetemu VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(NEETEMU_VERSION="${PROJECT_VERSION}")

if(NOT YSLUA_DIR)
  if(DEFINED ENV{YSLUA_DIR})
    set(YSLUA_DIR "$ENV{YSLUA_DIR}")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Neet-YSLua/lua/lua.h")
    set(YSLUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Neet-YSLua")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Neet-YSLua/lua/lua.h")
    set(YSLUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Neet-YSLua")
  else()
    set(YSLUA_DIR "/home/devuan/Projects/neetcomputers/Neet-YSLua"
        CACHE PATH "Neet-YSLua checkout")
  endif()
endif()
if(NOT EXISTS "${YSLUA_DIR}/lua/lua.h")
  message(FATAL_ERROR
    "YSLua not found at YSLUA_DIR=${YSLUA_DIR}.\n"
    "Provide it with one of:\n"
    "  cmake -DYSLUA_DIR=/path/to/Neet-YSLua ...\n"
    "  YSLUA_DIR=/path/to/Neet-YSLua cmake ...\n"
    "  <repo>/../Neet-YSLua  (side-by-side checkout)\n"
    "  <repo>/third_party/Neet-YSLua")
endif()
message(STATUS "Using YSLua at ${YSLUA_DIR}")

set(YSLUA_BUILD_RUNTIME_SHARED ON)
add_subdirectory(${YSLUA_DIR}/lua yslua_lua)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(neetemu
  compat.c main.c sched.c screen.c event.c files.c chip.c io.c overlay.c build.c)

target_include_directories(neetemu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${YSLUA_DIR}/lua)

target_compile_options(neetemu PRIVATE ${SDL2_CFLAGS})
target_link_directories(neetemu PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(neetemu PRIVATE yslua ${SDL2_LIBRARIES} m ${CMAKE_DL_LIBS})

add_custom_command(TARGET neetemu POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:yslua>" "$<TARGET_FILE_DIR:neetemu>")
if(UNIX AND NOT APPLE)
  set_target_properties(neetemu PROPERTIES
    INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib"
    BUILD_WITH_INSTALL_RPATH TRUE)
elseif(APPLE)
  set_target_properties(neetemu PROPERTIES
    INSTALL_RPATH "@loader_path:@loader_path/../lib"
    BUILD_WITH_INSTALL_RPATH TRUE)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neetemu PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

include(GNUInstallDirs)

install(TARGETS neetemu
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "$<TARGET_FILE:yslua>"
  DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/disk/"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/neetemu/disk)

if(UNIX AND NOT APPLE)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/neetemu.desktop"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

set(CPACK_PACKAGE_NAME "neetemu")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Host-faithful emulator for NeetComputers Lua machines")
set(CPACK_PACKAGE_CONTACT "NeetComputers project")
set(CPACK_GENERATOR "TGZ")
include(CPack)
