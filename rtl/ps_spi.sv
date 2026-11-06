// PocketStation device - memory card port (COM block)
// Copyright (c) 2026 f3bandit
// Based on ps_spi.sv from MiSTer-devel/PocketStation_MiSTer, Copyright (c) 2026
// Jamie Blanks. The register map and interface are his; the upstream module is
// an undocked stub, and this version adds the docked byte link.
// SPDX-License-Identifier: GPL-2.0-or-later
//
//   0x0C000000  COM_MODE    bit 0 drive DAT, bit 1 pull ACK low (pulse on set)
//   0x0C000004  COM_STAT1   bit 0 byte ready, bit 1 select released
//   0x0C000008  COM_DATA    read: byte from the console, write: next reply byte
//   0x0C000010  COM_CTRL1   bit 1 link enabled
//   0x0C000014  COM_STAT2   bit 0 byte ready
//   0x0C000018  COM_CTRL2   bit 0 clears ready, bit 1 also restarts the bit count
//
// Behaviour taken from the retail kernel (J110):
//  - The FIQ (INT_SPI) fires when the console pulls select low. The kernel then
//    polls STAT2 bit 0 for the first byte and walks away unless it is 81h.
//  - Per byte the kernel writes CTRL2=1, DATA=reply, MODE=1, MODE=3 and waits
//    for STAT2 bit 0 (or STAT1 bit 1 / the docking line). The reply goes out on
//    the console's next eight clocks; the MODE 1->3 step acknowledges the byte
//    just received.
//  - The sector-data loop (write) reads DATA, acks and polls STAT1 bit 0 with
//    no CTRL2 write, so the ack also clears ready.
//  - A command ends with the kernel waiting for STAT1 bit 1, select released.
//  - ps_intc samples sources on CPU clock enables, so the FIQ request is held
//    as a level while select is low.
// MODE bit 1 is therefore taken as a trigger: setting it produces one ACK pulse
// of ACK_US. The width is the one guessed number here; the Pico capture of a
// real unit is what settles it.

module ps_spi #(
	parameter longint unsigned CLK_SYS_HZ = 64'd31_981_568,
	parameter int ACK_US = 3
) (
	input  wire        clk_sys,
	input  wire        reset,

	input  wire        reg_sel,
	input  wire        reg_write,
	input  wire  [4:2] reg_addr,
	input  wire [31:0] reg_wdata,
	output reg  [31:0] reg_rdata,

	input  wire        dat_i,
	output wire        dat_o,
	output wire        dat_oe,
	input  wire        cmd_i,
	input  wire        clk_i,
	input  wire        sel_n_i,
	output wire        ack_o,
	output wire        ack_oe,

	output wire        irq,

	input  wire        ss_load,
	input  wire [63:0] ss_din,
	output wire [63:0] ss_dout
);

	localparam int US_CYCLES  = int'((CLK_SYS_HZ + 64'd999_999) / 64'd1_000_000);
	localparam int ACK_CYCLES = US_CYCLES * ACK_US;

	reg  [2:0] mode = 3'd0;
	reg  [1:0] ctrl1 = 2'd0;
	reg  [1:0] ctrl2 = 2'd0;
	reg  [7:0] tx = 8'hFF;       // reply byte, written by the kernel
	reg  [7:0] rx = 8'd0;        // last complete byte from the console
	reg  [7:0] shift = 8'd0;
	reg  [2:0] bitn = 3'd0;
	reg        ready = 1'b0;
	reg        released = 1'b1;
	reg        out_bit = 1'b1;
	reg [15:0] ack_cnt = 16'd0;

	// Two-flop synchronisers; the connector is asynchronous to clk_sys.
	reg [2:0] sel_s = 3'b111, clk_s = 3'b111, cmd_s = 3'b111;
	always @(posedge clk_sys) begin
		sel_s <= {sel_s[1:0], sel_n_i};
		clk_s <= {clk_s[1:0], clk_i};
		cmd_s <= {cmd_s[1:0], cmd_i};
	end
	wire sel_low  = ~sel_s[1];
	wire sel_fall = sel_s[2] & ~sel_s[1];
	wire sel_rise = ~sel_s[2] & sel_s[1];
	wire clk_fall = clk_s[2] & ~clk_s[1];
	wire clk_rise = ~clk_s[2] & clk_s[1];

	wire link_on = ctrl1[1];

	assign dat_o  = out_bit;
	assign dat_oe = mode[0] && sel_low && link_on;
	assign ack_o  = 1'b0;
	assign ack_oe = (ack_cnt != 16'd0);

	// Held for as long as select is low. ps_intc latches on the rising edge but
	// samples its sources only on CPU clock enables, so a one-clock pulse would
	// be missed at every rate below the full system clock.
	assign irq = sel_low && link_on;

	assign ss_dout = {28'd0, released, ready, bitn, shift, rx, tx, ctrl2, ctrl1, mode};

	wire mode_write = reg_sel && reg_write && (reg_addr == 3'h0);

	always @(posedge clk_sys) begin
		if (reset) begin
			mode <= 3'd0; ctrl1 <= 2'd0; ctrl2 <= 2'd0;
			tx <= 8'hFF; rx <= 8'd0; shift <= 8'd0; bitn <= 3'd0;
			ready <= 1'b0; released <= 1'b1; out_bit <= 1'b1; ack_cnt <= 16'd0;
		end
		else if (ss_load) begin
			mode  <= ss_din[2:0];
			ctrl1 <= ss_din[4:3];
			ctrl2 <= ss_din[6:5];
			tx    <= ss_din[14:7];
			rx    <= ss_din[22:15];
			shift <= ss_din[30:23];
			bitn  <= ss_din[33:31];
			ready <= ss_din[34];
			released <= ss_din[35];
			ack_cnt <= 16'd0;
		end
		else begin
			// ACK: one pulse each time MODE bit 1 goes from clear to set.
			// The acknowledge also consumes the byte: the kernel's sector-data loop
			// polls STAT1 straight after it without touching CTRL2.
			if (mode_write && reg_wdata[1] && !mode[1]) begin
				ack_cnt <= 16'(ACK_CYCLES);
				ready   <= 1'b0;
			end
			else if (ack_cnt != 16'd0)
				ack_cnt <= ack_cnt - 16'd1;

			if (reg_sel && reg_write) begin
				case (reg_addr)
					3'h0: mode  <= reg_wdata[2:0];
					3'h2: tx    <= reg_wdata[7:0];
					3'h4: ctrl1 <= reg_wdata[1:0];
					3'h6: begin
						ctrl2 <= reg_wdata[1:0];
						if (reg_wdata[0]) ready <= 1'b0;
						if (reg_wdata[1]) bitn  <= 3'd0;
					end
					default: ;
				endcase
			end

			if (sel_fall) begin
				bitn     <= 3'd0;
				released <= 1'b0;
				ready    <= 1'b0;
				out_bit  <= 1'b1;
			end
			else if (sel_rise) begin
				released <= 1'b1;
				bitn     <= 3'd0;
				out_bit  <= 1'b1;
				ack_cnt  <= 16'd0;
			end
			else if (sel_low) begin
				// Console presents a bit on the falling edge, samples on the rising.
				if (clk_fall)
					out_bit <= tx[bitn];
				if (clk_rise) begin
					shift <= {cmd_s[1], shift[7:1]};
					bitn  <= bitn + 3'd1;
					if (bitn == 3'd7) begin
						rx      <= {cmd_s[1], shift[7:1]};
						ready   <= 1'b1;
						out_bit <= 1'b1;
					end
				end
			end
		end
	end

	always @(*) begin
		case (reg_addr)
			3'h0:    reg_rdata = {29'd0, mode};
			3'h1:    reg_rdata = {30'd0, released, ready};
			3'h2:    reg_rdata = {24'd0, rx};
			3'h4:    reg_rdata = {30'd0, ctrl1};
			3'h5:    reg_rdata = {31'd0, ready};
			3'h6:    reg_rdata = {30'd0, ctrl2};
			default: reg_rdata = 32'd0;
		endcase
	end

`ifdef PS_TRACE
	always @(posedge clk_sys) begin
		if (reg_sel && reg_write) $display("[com] W %02x <= %02x", {reg_addr,2'b00}, reg_wdata[7:0]);
		if (sel_fall) $display("[com] SEL low  link_on=%0d", link_on);
		if (sel_rise) $display("[com] SEL high");
		if (clk_rise && sel_low && bitn==3'd7) $display("[com] RX %02x  (tx %02x)", {cmd_s[1], shift[7:1]}, tx);
	end
`endif

`ifndef SYNTHESIS
	wire _unused = &{1'b0, dat_i, 1'b0};
`endif

endmodule
