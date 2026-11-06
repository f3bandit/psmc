// PocketStation device - Verilator simulation top
// Copyright (c) 2026 f3bandit
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Wraps the PocketStation core by Jamie Blanks (MiSTer-devel/PocketStation_MiSTer,
// GPL-2.0-or-later) the same way the MiSTer top does: ps_clocks drives the
// clock enables, pocketstation_board is the unit. Everything MiSTer-specific
// (HPS, video scaler, savestates, SDRAM) is left out. The memory card port is
// brought out so the C++ card-bus harness can drive it.

module ps_sim_top (
	input  wire        clk_sys,
	input  wire        reset,

	input  wire  [4:0] buttons,

	input  wire  [9:0] panel_addr,
	output wire  [3:0] panel_level,

	output wire        led_on,
	output wire  [3:0] freq_sel,
	output wire        standby,
	output wire  [9:0] dac_value,
	output wire        speaker_active,

	output wire        com_dat,
	output wire        com_dat_oe,
	input  wire        com_cmd,
	input  wire        com_clk,
	input  wire        com_sel_n,
	output wire        com_ack,
	output wire        com_ack_oe,
	input  wire        com_vcc,

	input  wire        load_en,
	input  wire        load_boot,
	input  wire [16:0] load_addr,
	input  wire [15:0] load_data,
	input  wire        load_we,

	input  wire        save_en,
	input  wire [16:1] save_addr,
	output wire [15:0] save_rdata,
	output wire        card_dirty
);

	wire ce_sys, ce_xtal, clock_locked;

`ifndef PS_CLK_SYS_HZ
`define PS_CLK_SYS_HZ 31981568
`endif

	ps_clocks #(.CLK_SYS_HZ(64'd`PS_CLK_SYS_HZ)) clocks (
		.clk_sys  (clk_sys),
		.reset    (reset),
		.freq_sel (freq_sel),
		.standby  (standby),
		.park_req (1'b0),
		.parked   (),
		.ce_sys   (ce_sys),
		.ce_xtal  (ce_xtal),
		.locked   (clock_locked),
		.ss_load  (1'b0),
		.ss_din   (64'd0),
		.ss_dout  ()
	);

	pocketstation_board board (
		.clk_sys        (clk_sys),
		.reset          (reset),
		.ce_sys         (ce_sys),
		.ce_xtal        (ce_xtal),
		.freq_sel       (freq_sel),
		.standby        (standby),
		.pll_locked     (clock_locked),
		.buttons        (buttons),
		.panel_addr     (panel_addr),
		.panel_level    (panel_level),
		.lcd_contrast   (),
		.dac_value      (dac_value),
		.dac_enable     (),
		.speaker_active (speaker_active),
		.led_on         (led_on),
		.ir_txd         (),
		.ir_tx_oe       (),
		.ir_low_output  (),
		.ir_rxd         (1'b1),
		.com_dat        (com_dat),
		.com_dat_oe     (com_dat_oe),
		.com_cmd        (com_cmd),
		.com_clk        (com_clk),
		.com_sel_n      (com_sel_n),
		.com_ack        (com_ack),
		.com_ack_oe     (com_ack_oe),
		.com_vcc        (com_vcc),
		.battery_low    (1'b0),
		.rtc_seed_valid    (1'b0),
		.rtc_seed_apply    (1'b0),
		.rtc_seed_clock    (32'd0),
		.rtc_seed_calendar (32'd0),
		.load_en        (load_en),
		.load_boot      (load_boot),
		.load_addr      (load_addr),
		.load_data      (load_data),
		.load_we        (load_we),
		.save_en        (save_en),
		.save_addr      (save_addr),
		.save_we        (1'b0),
		.save_data      (16'd0),
		.save_rdata     (save_rdata),
		.card_dirty     (card_dirty),
		.ss_addr        (10'd0),
		.ss_din         (64'd0),
		.ss_wren        (1'b0),
		.ss_dout        (),
		.ss_mem_type    (3'd0),
		.ss_mem_addr    (25'd0),
		.ss_mem_rd      (1'b0),
		.ss_mem_wr      (1'b0),
		.ss_mem_wdata   (8'd0),
		.ss_mem_rdata   (),
		.ss_pause_req   (1'b0),
		.ss_pause_ready (),
		.ss_scan        (1'b0),
		.ss_restore     (1'b0),
		.ss_scan_done   ()
	);

endmodule
