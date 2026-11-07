cmake_minimum_required(VERSION 3.18)
project(MCPS LANGUAGES C CXX)

# SDL2, built from source as libSDL2.so (SDLActivity loads it by name).
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL2_SRC} SDL2 EXCLUDE_FROM_ALL)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(IMGUI ${ROOT}/app/third_party/imgui)

add_library(main SHARED
  ${ROOT}/app/main.cpp ${ROOT}/app/card.cpp ${ROOT}/app/net.cpp
  ${ROOT}/emu/pocketstation.cpp ${ROOT}/emu/arm/tablegen/tablegen.cc
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(main PRIVATE ${ROOT}/app ${ROOT}/emu ${IMGUI} ${IMGUI}/backends)
target_compile_options(main PRIVATE -O2 -fbracket-depth=8192)
target_link_libraries(main PRIVATE SDL2 SDL2main android log)
