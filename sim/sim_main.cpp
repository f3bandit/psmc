// PocketStation device - Verilator harness
// Copyright (c) 2026 f3bandit
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Usage: ps_sim [options]
//   --bios FILE         boot ROM (default: the core's open enhanced BIOS)
//   --card FILE         128 KiB card image loaded into flash (default: freshly formatted)
//   --seconds S         run time (default 3)
//   --snap S            LCD snapshot interval (default 0.5)
//   --out DIR           output directory (default out)
//   --press BTN@T[+D]   enter|right|left|down|up at T seconds for D seconds (default 0.15)
//   --dock T            apply console power (com_vcc) at T seconds
//   --card-test T       at T seconds (docks first if needed) run the console-side test:
//                       ID (53h), read frames 0/1, write frame 63, read it back,
//                       and compare against flash through the save port
//   --dump-card FILE    write the flash contents to FILE at the end
//   --cmd-gap MS        idle time after each command (default 1 ms)
//   --probe T           at T seconds run the PocketStation probes tools/ps_probe.py runs
//   --serve PORT        run as a virtual PocketStation on TCP PORT (docked; see serve_line)
//
// Bus timing mirrors ps_snac_link.sv / ps_snac_card.sv from the PocketStation core
// (Jamie Blanks, GPL-2.0-or-later): 250 kHz, LSB first, clock idles high, ack
// ignored for 3 us, 100 us ack ceiling (4 ms around a read's address and through
// writes), 65 us gaps through the header and 20 us through data, 500 us
// attention hold after a write.

#include "Vps_sim_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cerrno>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef PS_CLK_SYS_HZ
#define PS_CLK_SYS_HZ 31981568
#endif
static const double CLK_HZ = PS_CLK_SYS_HZ;
static const int US = (PS_CLK_SYS_HZ + 999999) / 1000000;   // clk_sys cycles per microsecond, rounded up as the core does
static const int HALF_BIT = PS_CLK_SYS_HZ / 500000;          // half of a 250 kHz bit

struct Press { int bit; double t, dur; };

static Vps_sim_top* top;
static uint64_t cyc = 0, start = 0;
static std::vector<Press> presses;
static std::string out = "out";
static double snap = 0.5, next_snap = 0.5;
static int snap_no = 0;
static int led = -1, fsel = -1, stby = -1, spk = -1;
static double cmd_gap_ms = 1.0;
static int dirty_pulses = 0;         // ps_flash pulses card_dirty once per committed frame      // console-side gap between commands

static inline double now() { return (cyc - start) / CLK_HZ; }

static inline void tick() {
    top->clk_sys = 0; top->eval();
    top->clk_sys = 1; top->eval();
    cyc++;
}

static void read_panel(uint8_t px[32][32]) {
    uint16_t a0 = top->panel_addr;
    for (int a = 0; a < 1024; a++) {
        top->panel_addr = a;
        tick();
        px[a >> 5][a & 31] = top->panel_level;
    }
    top->panel_addr = a0;
}

static void write_pgm(const std::string& path, uint8_t px[32][32], int scale) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    int w = 32 * scale;
    fprintf(f, "P5\n%d %d\n255\n", w, w);
    for (int y = 0; y < w; y++)
        for (int x = 0; x < w; x++) fputc(255 - px[y / scale][x / scale] * 17, f);
    fclose(f);
}

static void ascii(uint8_t px[32][32]) {
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) putchar(px[y][x] > 7 ? '#' : (px[y][x] ? '+' : '.'));
        putchar('\n');
    }
}

// One clk_sys cycle plus everything that watches the unit.
static void step() {
    if (start && (cyc & 63) == 0) {
        double t = now();
        uint8_t b = 0;
        for (auto& p : presses) if (t >= p.t && t < p.t + p.dur) b |= 1 << p.bit;
        top->buttons = b;
    }
    tick();
    dirty_pulses += top->card_dirty;
    if (!start || (cyc & 63)) return;
    if (top->led_on != led || top->freq_sel != fsel || top->standby != stby || top->speaker_active != spk) {
        led = top->led_on; fsel = top->freq_sel; stby = top->standby; spk = top->speaker_active;
        printf("t=%8.4fs led=%d freq_sel=%d standby=%d speaker=%d\n", now(), led, fsel, stby, spk);
    }
    if (now() >= next_snap) {
        uint8_t px[32][32];
        read_panel(px);
        char name[64];
        snprintf(name, sizeof name, "/lcd_%03d_%05.2fs.pgm", snap_no++, now());
        write_pgm(out + name, px, 5);
        next_snap += snap;
    }
}

static void wait_cycles(uint64_t n) { while (n--) step(); }

// ---------------------------------------------------------------- card bus --
// Open drain: a pin reads low only while the unit drives it low.
static inline int dat_level() { return (top->com_dat_oe && !top->com_dat) ? 0 : 1; }
static inline int ack_level() { return (top->com_ack_oe && !top->com_ack) ? 0 : 1; }

struct Xfer { uint8_t rx; bool acked; };

static Xfer xfer_byte(uint8_t tx, bool want_ack, bool long_ack, bool long_gap) {
    uint8_t rx = 0;
    for (int i = 0; i < 8; i++) {
        top->com_clk = 0;
        top->com_cmd = (tx >> i) & 1;
        wait_cycles(HALF_BIT);
        top->com_clk = 1;
        step();
        rx |= dat_level() << i;
        wait_cycles(HALF_BIT - 1);
    }
    top->com_cmd = 1;
    bool acked = false;
    if (want_ack) {
        uint64_t limit = (uint64_t)(long_ack ? 4000 : 100) * US;
        for (uint64_t c = 0; c < limit; c++) {
            step();
            if (c >= 3u * US && !ack_level()) { acked = true; break; }
        }
        uint64_t held = 0;                           // let the pulse finish
        while (acked && !ack_level() && held < 1000u * US) { step(); held++; }
        if (held >= 1000u * US) printf("  [bus] ack stuck low >1 ms after byte %02X\n", tx);
    }
    wait_cycles((long_gap ? 65 : 20) * US);
    return {rx, acked};
}

// Runs one command frame. Stops early when the card stops acknowledging.
static std::vector<uint8_t> frame_once(const std::vector<uint8_t>& tx,
                                       int long_ack_from, int long_ack_to,
                                       int header_len, bool writing, bool* complete);

// A console that gets no ack on the first byte tries again next frame; the
// kernel answers nothing while it is still committing a written frame.
static std::vector<uint8_t> frame_cmd(const std::vector<uint8_t>& tx,
                                      int long_ack_from, int long_ack_to,
                                      int header_len, bool writing, bool* complete) {
    std::vector<uint8_t> rx;
    for (int tries = 0; tries < 60; tries++) {
        double t0 = now();
        rx = frame_once(tx, long_ack_from, long_ack_to, header_len, writing, complete);
        if (*complete || rx.size() > 1) {
            if (tries) printf("  [bus] answered after %d retries (%.1f ms)\n", tries, (now() - t0) * 1000.0 + tries * 16.7);
            return rx;
        }
        wait_cycles((uint64_t)(16.7 * 1000.0) * US);
    }
    return rx;
}

static std::vector<uint8_t> frame_once(const std::vector<uint8_t>& tx,
                                       int long_ack_from, int long_ack_to,
                                       int header_len, bool writing, bool* complete) {
    std::vector<uint8_t> rx;
    top->com_sel_n = 0;
    wait_cycles(10 * US);
    *complete = true;
    for (size_t i = 0; i < tx.size(); i++) {
        bool last = (i + 1 == tx.size());
        bool la = writing || ((int)i >= long_ack_from && (int)i <= long_ack_to);
        Xfer x = xfer_byte(tx[i], !last, la, (int)i < header_len);
        rx.push_back(x.rx);
        if (!last && !x.acked) { *complete = false; break; }
    }
    wait_cycles((writing ? 500 : 10) * US);
    top->com_sel_n = 1;
    wait_cycles((uint64_t)(cmd_gap_ms * 1000.0) * US);
    return rx;
}

static void hexline(const char* tag, const std::vector<uint8_t>& v, size_t from, size_t n) {
    printf("  %s", tag);
    for (size_t i = from; i < from + n && i < v.size(); i++) printf(" %02X", v[i]);
    printf("\n");
}

static uint8_t xor_sum(const uint8_t* p, int n) { uint8_t c = 0; while (n--) c ^= *p++; return c; }

static bool card_id() {
    bool ok;
    auto rx = frame_cmd({0x81, 0x53, 0, 0, 0, 0, 0, 0, 0, 0}, -1, -1, 6, false, &ok);
    printf("[card] 53h ID: %s, %zu bytes\n", ok ? "complete" : "NO ACK", rx.size());
    hexline("rx:", rx, 0, rx.size());
    return ok && rx.size() == 10 && rx[2] == 0x5A && rx[3] == 0x5D && rx[4] == 0x5C && rx[5] == 0x5D;
}

static bool card_read(int frame, uint8_t data[128], uint8_t* flag) {
    std::vector<uint8_t> tx(140, 0);
    tx[0] = 0x81; tx[1] = 0x52; tx[4] = frame >> 8; tx[5] = frame & 0xFF;
    bool ok;
    auto rx = frame_cmd(tx, 5, 6, 6, false, &ok);
    printf("[card] 52h read frame %d: %s, %zu bytes\n", frame, ok ? "complete" : "NO ACK", rx.size());
    hexline("hdr:", rx, 0, 10);
    if (rx.size() > 1 && flag) *flag = rx[1];
    if (!ok || rx.size() != 140) return false;
    memcpy(data, &rx[10], 128);
    uint8_t chk = rx[138], want = (frame >> 8) ^ (frame & 0xFF) ^ xor_sum(data, 128);
    bool good = rx[2] == 0x5A && rx[3] == 0x5D && rx[6] == 0x5C && rx[7] == 0x5D &&
                rx[8] == (frame >> 8) && rx[9] == (frame & 0xFF) && chk == want && rx[139] == 0x47;
    printf("  checksum %02X (want %02X), end %02X -> %s\n", chk, want, rx[139], good ? "GOOD" : "BAD");
    return good;
}

static bool card_write(int frame, const uint8_t data[128]) {
    std::vector<uint8_t> tx(138, 0);
    tx[0] = 0x81; tx[1] = 0x57; tx[4] = frame >> 8; tx[5] = frame & 0xFF;
    memcpy(&tx[6], data, 128);
    tx[134] = (frame >> 8) ^ (frame & 0xFF) ^ xor_sum(data, 128);
    bool ok;
    auto rx = frame_cmd(tx, -1, -1, 6, true, &ok);
    printf("[card] 57h write frame %d: %s, %zu bytes\n", frame, ok ? "complete" : "NO ACK", rx.size());
    hexline("hdr:", rx, 0, 6);
    if (rx.size() >= 3) hexline("end:", rx, rx.size() - 3, 3);
    return ok && rx.size() == 138 && rx[135] == 0x5C && rx[136] == 0x5D && rx[137] == 0x47;
}

static void flash_read(uint8_t* buf, int bytes) {
    top->save_en = 1;
    for (int a = 0; a < bytes; a += 2) {
        top->save_addr = a >> 1;
        tick();
        tick();
        buf[a] = top->save_rdata & 0xFF;
        buf[a + 1] = top->save_rdata >> 8;
    }
    top->save_en = 0;
}

static void format_card(std::vector<uint8_t>& c) {
    std::fill(c.begin(), c.end(), 0);
    c[0] = 'M'; c[1] = 'C'; c[127] = 'M' ^ 'C';
    for (int f = 1; f < 16; f++) {
        uint8_t* p = &c[f * 128];
        p[0] = 0xA0; p[8] = 0xFF; p[9] = 0xFF;
        p[127] = xor_sum(p, 127);
    }
    for (int f = 16; f < 36; f++) {
        uint8_t* p = &c[f * 128];
        p[0] = p[1] = p[2] = p[3] = 0xFF; p[8] = 0xFF; p[9] = 0xFF;
        p[127] = xor_sum(p, 127);
    }
    c[63 * 128] = 'M'; c[63 * 128 + 1] = 'C'; c[63 * 128 + 127] = 'M' ^ 'C';
}

// ------------------------------------------------------------------ serve --
// The virtual PocketStation on the network. A console can't wait on a network round trip for each
// byte (it gives a card about 100 us to ack), so whole commands come in and go through the card
// bus here at console timing. Line protocol, same replies as Phosphor's API:
//   card_raw HEX      {"ok":true,"count":N,"stop":"none"|"no card"|"no ack","data":"<hex>"}
//   press BTN MS      enter|right|left|down|up held for MS milliseconds
//   lcd               {"ok":true,"w":32,"h":32,"levels":"<1024 hex digits, 0 light .. f dark>"}
//   status            {"ok":true,"t":seconds,"led":0|1,"freq":N,"standby":0|1,"docked":0|1}
//   dock 0|1          console power on the connector
static int srv_fd = -1, cli_fd = -1;
static std::string cli_in;

static void cli_send(const std::string& line) {
    if (cli_fd < 0) return;
    std::string l = line + "\n";
    if (send(cli_fd, l.data(), l.size(), MSG_NOSIGNAL) != (ssize_t)l.size()) { close(cli_fd); cli_fd = -1; }
}

static std::string hex(const std::vector<uint8_t>& v) {
    static const char* h = "0123456789abcdef";
    std::string o;
    for (uint8_t b : v) { o += h[b >> 4]; o += h[b & 15]; }
    return o;
}

static void serve_line(const std::string& line) {
    char out[256];
    if (line.rfind("card_raw ", 0) == 0) {
        std::string hx = line.substr(9);
        while (!hx.empty() && (hx.back() == '\r' || hx.back() == ' ')) hx.pop_back();
        if (hx.empty() || hx.size() % 2 || hx.size() > 512) { cli_send("{\"ok\":false,\"error\":\"data is 1..256 bytes of hex\"}"); return; }
        std::vector<uint8_t> tx;
        for (size_t i = 0; i < hx.size(); i += 2) tx.push_back((uint8_t)strtoul(hx.substr(i, 2).c_str(), nullptr, 16));
        bool ok;
        auto rx = frame_once(tx, 0, (int)tx.size(), 6, false, &ok);
        const char* stop = ok ? "none" : (rx.size() <= 1 ? "no card" : "no ack");
        cli_send("{\"ok\":true,\"count\":" + std::to_string(rx.size()) + ",\"stop\":\"" + stop + "\",\"data\":\"" + hex(rx) + "\"}");
    } else if (line.rfind("press ", 0) == 0) {
        const char* names[5] = {"enter", "right", "left", "down", "up"};
        char nm[16] = ""; int ms = 150;
        sscanf(line.c_str() + 6, "%15s %d", nm, &ms);
        int bit = -1;
        for (int b = 0; b < 5; b++) if (!strcmp(nm, names[b])) bit = b;
        if (bit < 0) { cli_send("{\"ok\":false,\"error\":\"button is enter|right|left|down|up\"}"); return; }
        presses.push_back({bit, now(), ms / 1000.0});
        cli_send("{\"ok\":true}");
    } else if (line == "lcd") {
        uint8_t px[32][32];
        read_panel(px);
        std::string o = "{\"ok\":true,\"w\":32,\"h\":32,\"levels\":\"";
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) o += "0123456789abcdef"[px[y][x] & 15];
        cli_send(o + "\"}");
    } else if (line == "status") {
        snprintf(out, sizeof out, "{\"ok\":true,\"t\":%.3f,\"led\":%d,\"freq\":%d,\"standby\":%d,\"docked\":%d}",
                 now(), top->led_on, top->freq_sel, top->standby, top->com_vcc);
        cli_send(out);
    } else if (line == "dock 0" || line == "dock 1") {
        top->com_vcc = line[5] == '1';
        cli_send("{\"ok\":true}");
    } else {
        cli_send("{\"ok\":false,\"error\":\"unknown command\"}");
    }
}

static void serve_poll() {
    if (srv_fd < 0) return;
    if (cli_fd < 0) {
        int c = accept(srv_fd, nullptr, nullptr);
        if (c >= 0) { fcntl(c, F_SETFL, O_NONBLOCK); cli_fd = c; cli_in.clear(); printf("[serve] client connected\n"); }
        return;
    }
    char buf[2048];
    ssize_t n = recv(cli_fd, buf, sizeof buf, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close(cli_fd); cli_fd = -1; printf("[serve] client gone\n"); return; }
    if (n > 0) cli_in.append(buf, n);
    size_t nl;
    while ((nl = cli_in.find('\n')) != std::string::npos) {
        std::string line = cli_in.substr(0, nl);
        cli_in.erase(0, nl + 1);
        serve_line(line);
    }
}

static bool serve_open(int port) {
    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv_fd, (sockaddr*)&a, sizeof a) || listen(srv_fd, 1)) { perror("serve"); return false; }
    fcntl(srv_fd, F_SETFL, O_NONBLOCK);
    printf("[serve] virtual PocketStation on port %d\n", port);
    return true;
}

// The PocketStation probes tools/ps_probe.py runs on real hardware, for side-by-side comparison.
static void raw_show(const char* name, const std::vector<uint8_t>& tx) {
    bool ok;
    auto rx = frame_cmd(tx, 0, (int)tx.size(), 6, false, &ok);
    printf("[probe] %-26s %3zu/%-3zu bytes  %s\n", name, rx.size(), tx.size(), ok ? "complete" : "stopped");
    for (size_t i = 0; i < rx.size(); i += 16) {
        printf("    tx"); for (size_t k = i; k < i + 16 && k < tx.size(); k++) printf(" %02X", tx[k]); printf("\n");
        printf("    rx"); for (size_t k = i; k < i + 16 && k < rx.size(); k++) printf(" %02X", rx[k]); printf("\n");
    }
}

static void probe() {
    printf("[probe] at t=%.4fs\n", now());
    std::vector<uint8_t> id = {0x81, 0x53}; id.resize(10, 0);
    raw_show("53h ID", id);
    raw_show("58h version", {0x81, 0x58, 0, 0, 0});
    std::vector<uint8_t> st = {0x81, 0x5A}; st.resize(21, 0);
    raw_show("5Ah status", st);
    std::vector<uint8_t> dt = {0x81, 0x5B, 0x00, 0, 0}; dt.resize(14, 0);
    raw_show("5Bh FUNC 00h date/time", dt);
    std::vector<uint8_t> mr = {0x81, 0x5B, 0x01, 0, 0x00, 0x00, 0x00, 0x04, 0x80, 0}; mr.resize(139, 0);
    raw_show("5Bh FUNC 01h @04000000", mr);
}

static int card_test(const std::vector<uint8_t>& image) {
    int fails = 0;
    printf("[card] test at t=%.4fs\n", now());
    if (!card_id()) { printf("[card] ID: FAIL\n"); fails++; }

    uint8_t buf[128], flag = 0;
    for (int f = 0; f < 2; f++) {
        bool ok = card_read(f, buf, &flag);
        bool match = ok && !memcmp(buf, &image[f * 128], 128);
        printf("[card] read %d: %s (flag %02X)\n", f, match ? "PASS" : "FAIL", flag);
        fails += !match;
    }

    uint8_t pat[128];
    for (int i = 0; i < 128; i++) pat[i] = (uint8_t)(i * 37 + 11);
    bool w = card_write(63, pat);
    printf("[card] write 63: %s\n", w ? "PASS" : "FAIL");
    fails += !w;

    bool r = card_read(63, buf, &flag) && !memcmp(buf, pat, 128);
    printf("[card] read-back 63: %s (flag %02X)\n", r ? "PASS" : "FAIL", flag);
    fails += !r;

    uint8_t fl[128];
    top->save_en = 1;
    for (int a = 0; a < 128; a += 2) {
        top->save_addr = (63 * 128 + a) >> 1;
        tick(); tick();
        fl[a] = top->save_rdata & 0xFF; fl[a + 1] = top->save_rdata >> 8;
    }
    top->save_en = 0;
    bool f = !memcmp(fl, pat, 128);
    printf("[card] flash frame 63 via save port: %s, flash commits=%d\n", f ? "PASS" : "FAIL", dirty_pulses);
    fails += !f;

    printf("[card] %s (%d failed)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string bios = "../../PocketStation_MiSTer/BIOS/enhanced_bios.bin";
    std::string card_file, dump_file;
    double seconds = 3.0, dock_t = -1, test_t = -1, probe_t = -1;
    int serve_port = 0;
    const char* names[5] = {"enter", "right", "left", "down", "up"};

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); } return argv[++i]; };
        if (a == "--bios") bios = next();
        else if (a == "--card") card_file = next();
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--snap") snap = atof(next().c_str());
        else if (a == "--out") out = next();
        else if (a == "--dock") dock_t = atof(next().c_str());
        else if (a == "--card-test") test_t = atof(next().c_str());
        else if (a == "--dump-card") dump_file = next();
        else if (a == "--cmd-gap") cmd_gap_ms = atof(next().c_str());
        else if (a == "--probe") probe_t = atof(next().c_str());
        else if (a == "--serve") serve_port = atoi(next().c_str());
        else if (a == "--press") {
            std::string p = next();
            size_t at = p.find('@');
            Press pr{-1, 0, 0.15};
            for (int b = 0; b < 5; b++) if (p.substr(0, at) == names[b]) pr.bit = b;
            if (pr.bit < 0 || at == std::string::npos) { fprintf(stderr, "bad --press %s\n", p.c_str()); return 2; }
            std::string rest = p.substr(at + 1);
            size_t plus = rest.find('+');
            pr.t = atof(rest.substr(0, plus).c_str());
            if (plus != std::string::npos) pr.dur = atof(rest.substr(plus + 1).c_str());
            presses.push_back(pr);
        } else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (test_t >= 0 && (dock_t < 0 || dock_t > test_t)) dock_t = test_t > 0.2 ? test_t - 0.2 : 0;
    if (probe_t >= 0 && (dock_t < 0 || dock_t > probe_t)) dock_t = probe_t > 0.2 ? probe_t - 0.2 : 0;
    next_snap = snap;
    mkdir(out.c_str(), 0755);

    std::vector<uint8_t> rom(16384, 0), card(131072, 0);
    FILE* bf = fopen(bios.c_str(), "rb");
    if (!bf) { perror(bios.c_str()); return 1; }
    size_t n = fread(rom.data(), 1, rom.size(), bf);
    fclose(bf);
    printf("BIOS %s: %zu bytes\n", bios.c_str(), n);
    if (!card_file.empty()) {
        FILE* cf = fopen(card_file.c_str(), "rb");
        if (!cf) { perror(card_file.c_str()); return 1; }
        size_t cn = fread(card.data(), 1, card.size(), cf);
        fclose(cf);
        printf("card %s: %zu bytes\n", card_file.c_str(), cn);
    } else {
        format_card(card);
        printf("card: freshly formatted\n");
    }

    top = new Vps_sim_top;
    top->com_sel_n = 1; top->com_clk = 1; top->com_cmd = 1; top->com_vcc = 0;
    top->reset = 1;
    for (int i = 0; i < 16; i++) tick();

    // Boot ROM and flash load, held in reset like the MiSTer ioctl download.
    top->load_en = 1;
    top->load_boot = 1;
    for (size_t a = 0; a + 1 < n; a += 2) {
        top->load_addr = a; top->load_data = rom[a] | (rom[a + 1] << 8);
        top->load_we = 1; tick(); top->load_we = 0; tick();
    }
    top->load_boot = 0;
    for (size_t a = 0; a < card.size(); a += 2) {
        top->load_addr = a; top->load_data = card[a] | (card[a + 1] << 8);
        top->load_we = 1; tick(); top->load_we = 0; tick();
    }
    top->load_en = 0;
    for (int i = 0; i < 64; i++) tick();
    top->reset = 0;
    start = cyc;

    int rc = 0;
    if (serve_port) {
        if (!serve_open(serve_port)) return 1;
        top->com_vcc = 1;            // docked: the virtual unit sits in a console
        if (seconds == 3.0) seconds = 1e9;
    }
    bool docked = false, tested = false, probed = false;
    uint64_t end = start + (uint64_t)(seconds * CLK_HZ);
    while (cyc < end) {
        if (!docked && dock_t >= 0 && now() >= dock_t) {
            top->com_vcc = 1; docked = true;
            printf("t=%8.4fs docked (com_vcc=1)\n", now());
        }
        if (!tested && test_t >= 0 && now() >= test_t) {
            tested = true;
            rc = card_test(card) ? 1 : 0;
        }
        if (!probed && probe_t >= 0 && now() >= probe_t) { probed = true; probe(); }
        if (serve_port && (cyc & 0x3FFF) == 0) serve_poll();   // about every 0.5 ms of unit time
        step();
    }

    uint8_t px[32][32];
    read_panel(px);
    write_pgm(out + "/lcd_final.pgm", px, 5);
    printf("final LCD at %.2fs:\n", now());
    ascii(px);

    if (!dump_file.empty()) {
        flash_read(card.data(), card.size());
        FILE* df = fopen(dump_file.c_str(), "wb");
        if (df) { fwrite(card.data(), 1, card.size(), df); fclose(df); printf("flash dumped to %s\n", dump_file.c_str()); }
    }
    top->final();
    delete top;
    return rc;
}
