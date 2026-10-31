# game_totk: the run-time side of the TotK patches (data page writer, frame step, output size, aspect).
# The code patches themselves are applied to the module image before recompilation (python -m games.totk.patches).
add_library(game_totk STATIC runtime/totk_patches.c)
target_include_directories(game_totk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(nxrecomp::game_totk ALIAS game_totk)

if(BUILD_TESTING)
  add_executable(test_totk_patches tests/test_totk_patches.c)
  target_link_libraries(test_totk_patches PRIVATE game_totk)
  add_test(NAME totk_patches COMMAND test_totk_patches)
endif()
