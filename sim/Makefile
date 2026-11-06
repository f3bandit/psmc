# PocketStation device - Verilator build
# Core: MiSTer-devel/PocketStation_MiSTer by Jamie Blanks (GPL-2.0-or-later)
CORE ?= ../../PocketStation_MiSTer
R := $(CORE)/rtl

SRCS := $(R)/ps_pkg.sv $(R)/arm7tdmi/arm7tdmi_pkg.sv $(R)/arm7tdmi/arm7tdmi_core.sv \
        $(filter-out $(R)/asic/ps_spi.sv,$(wildcard $(R)/asic/*.sv)) ../rtl/ps_spi.sv $(R)/mem/cache_ram.v \
        $(R)/board/ps_lcd_panel.sv $(R)/board/pocketstation_board.sv \
        $(R)/ps_clocks.sv ps_sim_top.sv

VFLAGS := --cc --exe --build -j 0 -O3 --x-assign fast --x-initial fast \
          -Wno-fatal -Wno-lint -Wno-style --top-module ps_sim_top \
          -CFLAGS "-O2 -march=native"

ps_sim: $(SRCS) sim_main.cpp
	verilator $(VFLAGS) $(SRCS) sim_main.cpp -o ps_sim
	cp obj_dir/ps_sim .

clean:
	rm -rf obj_dir ps_sim out

# Same build with COM register tracing ([com] lines on stdout)
ps_sim_trace: $(SRCS) sim_main.cpp
	verilator $(VFLAGS) -DPS_TRACE -Mdir obj_trace $(SRCS) sim_main.cpp -o ps_sim_trace
	cp obj_trace/ps_sim_trace .

# 8 MHz system clock (244 x the crystal): the CPU's top rate, 4x fewer cycles to simulate.
# For the PC app; the MiSTer build keeps 32 MHz.
ps_sim_8m: $(SRCS) sim_main.cpp
	rm -rf obj_8m
	verilator $(VFLAGS) -DPS_CLK_SYS_HZ=7995392 -CFLAGS -DPS_CLK_SYS_HZ=7995392 -Mdir obj_8m $(SRCS) sim_main.cpp -o ps_sim_8m
	cp obj_8m/ps_sim_8m .

# Windows .exe of the 8 MHz build (cross-compiled with MinGW-w64, static, no DLLs needed)
ps_sim_8m.exe: $(SRCS) sim_main.cpp
	rm -rf obj_win
	verilator --cc --exe -O3 --x-assign fast --x-initial fast -Wno-fatal -Wno-lint -Wno-style \
	  --top-module ps_sim_top -DPS_CLK_SYS_HZ=7995392 -Mdir obj_win $(SRCS) sim_main.cpp -o ps_sim_8m.exe
	$(MAKE) -C obj_win -f Vps_sim_top.mk -j 4 CXX=x86_64-w64-mingw32-g++ LINK=x86_64-w64-mingw32-g++ \
	  OPT_FAST="-O3" OPT_SLOW="-O2" OPT_GLOBAL="-O2" \
	  USER_CPPFLAGS="-DPS_CLK_SYS_HZ=7995392" \
	  LDFLAGS="-static" LIBS="-lws2_32" ps_sim_8m.exe
	cp obj_win/ps_sim_8m.exe .
