# MC/PS MemCard Pro (GPL-3.0-or-later).
# Linux build (for testing):   make
# Windows build:               make win SDL2=/path/to/SDL2-2.30.9/x86_64-w64-mingw32
#   (SDL2 MinGW devel package: https://github.com/libsdl-org/SDL/releases)
IMGUI := third_party/imgui
SRCS := main.cpp card.cpp net.cpp alerts.cpp link.cpp monitor.cpp ../emu/pocketstation.cpp ../emu/arm/tablegen/tablegen.cc \
        $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp $(IMGUI)/imgui_tables.cpp $(IMGUI)/imgui_widgets.cpp \
        $(IMGUI)/backends/imgui_impl_sdl2.cpp $(IMGUI)/backends/imgui_impl_sdlrenderer2.cpp
FLAGS := -std=c++17 -O2 -I. -I../emu -I$(IMGUI) -I$(IMGUI)/backends

# Our own kernel (../kernel), built in so no BIOS is needed.
builtin_kernel.inc: ../kernel/mcps-kernel.bin
	python3 -c "import sys; d=open(sys.argv[1],'rb').read(); print('// Generated from kernel/mcps-kernel.bin (make): MC/PS kernel, GPL-3.0-or-later.'); print('static const unsigned char kBuiltinKernel[%d] = {' % len(d)); [print(' ' + ','.join('0x%02x' % b for b in d[i:i+24]) + ',') for i in range(0, len(d), 24)]; print('};')" $< > $@

../kernel/mcps-kernel.bin: ../kernel/*.c ../kernel/*.h ../kernel/*.S ../kernel/link.ld
	$(MAKE) -C ../kernel

mcps: $(SRCS) *.hpp ../emu/*.hpp builtin_kernel.inc
	$(CXX) $(FLAGS) $(shell sdl2-config --cflags) $(SRCS) $(shell sdl2-config --libs) -o $@

SDL2 ?= ../../deps/SDL2-2.30.9/x86_64-w64-mingw32
win: MCPS.exe
MCPS.exe: $(SRCS) *.hpp ../emu/*.hpp builtin_kernel.inc
	x86_64-w64-mingw32-g++ $(FLAGS) -I$(SDL2)/include/SDL2 -Dmain=SDL_main $(SRCS) -o $@ \
	  -static -mwindows -L$(SDL2)/lib -lmingw32 -lSDL2main -lSDL2 \
	  -lm -ldinput8 -ldxguid -ldxerr8 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 \
	  -lshell32 -lversion -luuid -lsetupapi -lws2_32 -lcomdlg32

clean:
	rm -f mcps MCPS.exe
