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
