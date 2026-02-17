cmake_minimum_required(VERSION 3.16)
project(jam-G8)
set(TARGET_NAME "jam")

find_package(Threads REQUIRED)

# file(GLOB_RECURSE SRCS jam_src/*.c)
file(GLOB_RECURSE SRCS
jam_src/builtins.c
jam_src/command.c
jam_src/compile.c
jam_src/expand.c
jam_src/execdmc.c
jam_src/fileunix.c
jam_src/glob.c
jam_src/hash.c
jam_src/headers.c
jam_src/jam.c
jam_src/jambase.c
jam_src/jamgram.c
jam_src/lists.c
jam_src/make.c
jam_src/make1.c
jam_src/newstr.c
jam_src/option.c
jam_src/parse.c
jam_src/pathunix.c
jam_src/regexp.c
jam_src/rules.c
jam_src/scan.c
jam_src/search.c
jam_src/variable.c
jam_src/timestamp.c
jam_src/outFilter.c
)

file(GLOB_RECURSE HEADERS src/*.h)

add_executable(${TARGET_NAME} ${SRCS})

target_link_libraries(${TARGET_NAME} PRIVATE Threads::Threads)

# install
install(TARGETS 
  ${TARGET_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)

# uninstall
if (NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)
  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()

