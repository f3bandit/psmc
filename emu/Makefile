# PocketStation emulator (GPL-3.0-or-later): ARM7TDMI from NanoBoyAdvance, hardware from
# Jamie Blanks' MiSTer core. See ../docs/emulator-spec.md.
CXX ?= g++
CXXFLAGS ?= -O2
FLAGS := -std=c++17 -I. $(CXXFLAGS)
SRCS := pocketstation.cpp arm/tablegen/tablegen.cc ps_emu_main.cpp

ps_emu: $(SRCS) pocketstation.hpp arm/arm7tdmi.hh bus/bus.hh
	$(CXX) $(FLAGS) $(SRCS) -o $@

ps_emu.exe: $(SRCS) pocketstation.hpp arm/arm7tdmi.hh bus/bus.hh
	x86_64-w64-mingw32-g++ $(FLAGS) -static $(SRCS) -o $@

clean:
	rm -f ps_emu ps_emu.exe
