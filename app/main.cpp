// MC/PS MemCard Pro: the PocketStation app for Windows.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// Plays PocketStation mini-games on the PC and serves the virtual card on the network.
// The PocketStation is the emulator in ../emu (hardware after Jamie Blanks' MiSTer core, ARM7TDMI
// from NanoBoyAdvance). Window, sound and controllers: SDL2 (zlib). UI: Dear ImGui (MIT).
// Font: Inter (SIL OFL 1.1). The BIOS is the user's own; none ships with the app.

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "../emu/pocketstation.hpp"
#include "card.hpp"
#include "net.hpp"

#include "fonts/inter_regular.h"
#include "fonts/inter_semibold.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

namespace fs = std::filesystem;
using ps::u8;

static const char* APP_NAME = "MC/PS MemCard Pro";
static const char* APP_VERSION = "0.1.0";

// Colours from the app mockup.
static ImU32 Col(unsigned rgb, int a = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }
static ImVec4 ColV(unsigned rgb) { return ImGui::ColorConvertU32ToFloat4(Col(rgb)); }
enum : unsigned {
  BG = 0x0e0e10, PANEL = 0x17171b, CARD = 0x1d1d22, LINE = 0x2a2a31, TEXT = 0xece8e1,
  TEXT2 = 0xc9c5be, TEXT3 = 0xa9a59f, TEXT4 = 0x8d8a85, ACCENT = 0xffb347, ACCENT_BG = 0x2a2219,
  GOOD = 0x5cf08a, LED_ON = 0xe5484d, LCD_BG = 0xc9cdb8, LCD_PX = 0x22241c,
};

// ---------------------------------------------------------------------------------- files --

static std::vector<u8> ReadFile(const std::string& path) {
  std::vector<u8> v;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return v;
  u8 buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
  fclose(f);
  return v;
}

struct Settings {
  std::string bios, card;
  int port = 7550;
  std::string path;

  void Load(const std::string& p) {
    path = p;
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
      std::string s = line;
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      auto eq = s.find('=');
      if (eq == std::string::npos) continue;
      std::string k = s.substr(0, eq), v = s.substr(eq + 1);
      if (k == "bios") bios = v;
      else if (k == "card") card = v;
      else if (k == "port") port = atoi(v.c_str());
    }
    fclose(f);
  }
  void Save() const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "bios=%s\ncard=%s\nport=%d\n", bios.c_str(), card.c_str(), port);
    fclose(f);
  }
};

static std::string OpenFileDialog(const char* filter_desc, const char* patterns) {
#ifdef _WIN32
  wchar_t file[1024] = L"";
  wchar_t filter[512];
  // "desc\0patterns\0All files\0*.*\0\0"
  std::string f = std::string(filter_desc) + '\0' + patterns + '\0' + "All files" + '\0' + "*.*" + '\0' + '\0';
  MultiByteToWideChar(CP_UTF8, 0, f.data(), (int)f.size(), filter, 512);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = file;
  ofn.nMaxFile = 1024;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return "";
  char out[4096];
  WideCharToMultiByte(CP_UTF8, 0, file, -1, out, sizeof out, nullptr, nullptr);
  return out;
#else
  (void)filter_desc; (void)patterns;
  return "";   // Linux builds are for testing: drop files on the window instead
#endif
}

static std::string SaveFileDialog(const char* filter_desc, const char* patterns, const char* def_ext, const std::string& name) {
#ifdef _WIN32
  wchar_t file[1024] = L"";
  MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, file, 1024);
  wchar_t filter[512], ext[16];
  std::string f = std::string(filter_desc) + '\0' + patterns + '\0' + '\0';
  MultiByteToWideChar(CP_UTF8, 0, f.data(), (int)f.size(), filter, 512);
  MultiByteToWideChar(CP_UTF8, 0, def_ext, -1, ext, 16);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = file;
  ofn.nMaxFile = 1024;
  ofn.lpstrDefExt = ext;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&ofn)) return "";
  char out[4096];
  WideCharToMultiByte(CP_UTF8, 0, file, -1, out, sizeof out, nullptr, nullptr);
  return out;
#else
  (void)filter_desc; (void)patterns; (void)def_ext; (void)name;
  return "";
#endif
}

// --------------------------------------------------------------------------------- the app --

struct App {
  ps::PocketStation m;
  Settings cfg;
  std::string dir;               // where the exe lives; settings and the default card go here
  bool have_bios = false;
  std::vector<u8> cardimg;
  double card_save_at = -1;
  net::Server server;
  bool docked = false;
  double now = 0, undock_at = -1;
  int page = 0;                  // 0 Play, 1 Cards
  int selected = -1;             // first block of the selected file
  std::string note;
  double note_until = 0;
  u8 mouse_buttons = 0;
  SDL_GameController* pad = nullptr;
  SDL_AudioDeviceID audio = 0;
  ImFont* font_bold = nullptr;
  ImFont* font_big = nullptr;

  void Note(const std::string& s) { note = s; note_until = now + 5; }

  std::string CardPath() const { return cfg.card.empty() ? dir + "PocketStation.mcd" : cfg.card; }

  bool UseBios(const std::string& path) {
    auto rom = ReadFile(path);
    if (rom.size() != 16384) { Note("That isn't a PocketStation BIOS: it should be exactly 16 KB."); return false; }
    m.LoadBios(rom);
    have_bios = true;
    cfg.bios = path;
    cfg.Save();
    PowerOn();
    return true;
  }

  void PowerOn() {
    m.LoadCard(cardimg);
    m.Reset();
    clock_sync_at = now + 1.0;   // once the kernel has finished starting up
  }

  // Sets the PocketStation's clock from the PC, the way a PS1 game does: docked, BU command 5Ch
  // function 00h. The kernel's start-up overwrites the RTC, so this runs a second after power-on.
  double clock_sync_at = -1;
  static u8 Bcd(int v) { return (u8)((v / 10) << 4 | (v % 10)); }
  void SyncClock() {
    time_t tt = time(nullptr);
    tm lt = *localtime(&tt);
    bool was = docked;
    m.SetDocked(true);
    m.RunFor(0.2);
    std::vector<u8> tx = {0x81, 0x5C, 0x00, 0x00, 0x00,
                          Bcd(lt.tm_mday), Bcd(lt.tm_mon + 1), Bcd(lt.tm_year % 100), Bcd(19 + lt.tm_year / 100),
                          Bcd(lt.tm_sec), Bcd(lt.tm_min), Bcd(lt.tm_hour), Bcd(lt.tm_wday + 1), 0x00};
    for (int i = 0; i < 10; i++) {
      auto r = m.CardCommand(tx);
      m.RunFor(0.02);
      if (r.complete || r.rx.size() > 1) break;
      m.RunFor(0.0167);
    }
    if (!was) m.SetDocked(false);
  }

  void LoadCardFile() {
    cardimg.clear();
    std::string p = CardPath();
    if (fs::exists(p)) {
      std::string err = card::Load(p, cardimg);
      if (!err.empty()) { Note(p + ": " + err); cardimg = card::Formatted(); }
    } else {
      cardimg = card::Formatted();
      card::Save(p, cardimg);
    }
  }

  // Anything dropped on the window or picked in a dialog.
  void Open(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    auto size = fs::exists(path) ? fs::file_size(path) : 0;
    if (size == 16384 && (ext == ".bin" || ext == ".rom" || ext == "")) { if (UseBios(path)) Note("BIOS loaded: " + fs::path(path).filename().string()); return; }
    if (size == card::SIZE || size == card::SIZE + 3904) {
      // A whole card: it becomes the card in the PocketStation, the old one kept as .bak.
      std::vector<u8> img;
      std::string err = card::Load(path, img);
      if (!err.empty()) { Note(err); return; }
      card::Save(CardPath() + ".bak", cardimg);
      cardimg = img;
      card::Save(CardPath(), cardimg);
      m.LoadCard(cardimg);
      Note("Card loaded from " + fs::path(path).filename().string() + " (the previous card is in " + fs::path(CardPath()).filename().string() + ".bak)");
      return;
    }
    // A single save: onto the card.
    std::vector<u8> img = cardimg;
    std::string err = card::Load(path, img);
    if (!err.empty()) { Note(fs::path(path).filename().string() + ": " + err); return; }
    cardimg = img;
    card::Save(CardPath(), cardimg);
    m.LoadCard(cardimg);
    Note("Added " + fs::path(path).filename().string() + " to the card");
  }

  u8 Buttons(const u8* keys) {
    u8 b = mouse_buttons;
    if (!ImGui::GetIO().WantTextInput) {
      if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_KP_ENTER]) b |= 1 << ps::BTN_ENTER;
      if (keys[SDL_SCANCODE_RIGHT]) b |= 1 << ps::BTN_RIGHT;
      if (keys[SDL_SCANCODE_LEFT]) b |= 1 << ps::BTN_LEFT;
      if (keys[SDL_SCANCODE_DOWN]) b |= 1 << ps::BTN_DOWN;
      if (keys[SDL_SCANCODE_UP]) b |= 1 << ps::BTN_UP;
    }
    if (pad) {
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) b |= 1 << ps::BTN_ENTER;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= 1 << ps::BTN_RIGHT;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= 1 << ps::BTN_LEFT;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= 1 << ps::BTN_DOWN;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= 1 << ps::BTN_UP;
      int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
      if (x > 16000) b |= 1 << ps::BTN_RIGHT;
      if (x < -16000) b |= 1 << ps::BTN_LEFT;
      if (y > 16000) b |= 1 << ps::BTN_DOWN;
      if (y < -16000) b |= 1 << ps::BTN_UP;
    }
    return b;
  }

  void Step(double dt, const u8* keys) {
    now += dt;
    if (!have_bios) return;
    m.SetButtons(Buttons(keys));
    if (docked && now >= undock_at) { docked = false; m.SetDocked(false); }
    if (clock_sync_at >= 0 && now >= clock_sync_at) { clock_sync_at = -1; SyncClock(); }
    m.RunFor(std::min(dt, 0.1));
    if (m.CardDirty()) { cardimg = m.Card(); card_save_at = now + 1.0; }
    if (card_save_at >= 0 && now >= card_save_at) { card::Save(CardPath(), cardimg); card_save_at = -1; }
    server.Poll();
    if (audio) {
      auto a = m.TakeAudio();
      if (SDL_GetQueuedAudioSize(audio) < 32768 * 2 / 6) {
        std::vector<int16_t> out(a.size());
        for (size_t i = 0; i < a.size(); i++) out[i] = (int16_t)std::clamp(a[i] * 48, -32767, 32767);
        if (!out.empty()) SDL_QueueAudio(audio, out.data(), (Uint32)(out.size() * 2));
      }
    }
  }

  void StartServer() {
    server.card_raw = [this](const std::vector<unsigned char>& tx, bool& complete) {
      if (!docked) { docked = true; m.SetDocked(true); }
      undock_at = now + 3.0;   // back to games 3 s after the console stops talking
      auto r = m.CardCommand(tx);
      complete = r.complete;
      if (m.CardDirty()) { cardimg = m.Card(); card_save_at = now + 1.0; }
      return r.rx;
    };
    server.status = [this]() {
      return std::string("{\"ok\":true,\"docked\":") + (docked ? "true" : "false") + ",\"led\":" + (m.LedOn() ? "1" : "0") + "}";
    };
    if (!server.Open(cfg.port)) Note("Couldn't open port " + std::to_string(cfg.port) + " for the network card (in use?)");
  }

  // ------------------------------------------------------------------------------ drawing --

  void TopBar() {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    dl->AddText(font_bold, 18, ImVec2(p.x + 28, p.y + 21), Col(ACCENT), "P O C K E T S T A T I O N");
    const char* tabs[2] = {"Play", "Cards"};
    for (int i = 0; i < 2; i++) {
      bool on = page == i;
      ImGui::SetCursorPos(ImVec2(280.0f + i * 80, 14));
      ImGui::PushStyleColor(ImGuiCol_Button, on ? ColV(ACCENT_BG) : ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_Border, on ? ColV(ACCENT) : ColV(LINE));
      ImGui::PushStyleColor(ImGuiCol_Text, on ? ColV(TEXT) : ColV(TEXT3));
      if (ImGui::Button(tabs[i], ImVec2(72, 34))) page = i;
      ImGui::PopStyleColor(3);
    }
    // Network chip on the right.
    std::string chip;
    unsigned dot;
    if (!server.Listening()) { chip = "Network card off"; dot = TEXT4; }
    else if (docked) { chip = "Docked · " + server.Peer(); dot = ACCENT; }
    else if (server.Connected()) { chip = "SS1 · " + server.Peer(); dot = GOOD; }
    else { chip = "Network card · port " + std::to_string(server.port); dot = TEXT4; }
    ImVec2 ts = ImGui::CalcTextSize(chip.c_str());
    ImVec2 a(p.x + w - 28 - ts.x - 36, p.y + 14), b(p.x + w - 28, p.y + 14 + 32);
    dl->AddRectFilled(a, b, Col(PANEL), 16);
    dl->AddRect(a, b, Col(LINE), 16);
    dl->AddCircleFilled(ImVec2(a.x + 16, a.y + 16), 4, Col(dot));
    dl->AddText(ImVec2(a.x + 28, a.y + 16 - ts.y / 2), Col(TEXT2), chip.c_str());
    dl->AddLine(ImVec2(p.x, p.y + 61), ImVec2(p.x + w, p.y + 61), Col(LINE));
  }

  bool ListCard(const char* title, const char* sub, const char* sub2, bool sel, float w) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = sub2 ? 70 : 52;
    ImGui::InvisibleButton(title, ImVec2(w, h));
    bool clicked = ImGui::IsItemClicked();
    bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(sel ? ACCENT_BG : (hov ? CARD : PANEL)), 10);
    if (sel) dl->AddRect(p, ImVec2(p.x + w, p.y + h), Col(ACCENT), 10);
    dl->AddText(font_bold, 15, ImVec2(p.x + 14, p.y + 10), Col(TEXT), title);
    dl->AddText(ImGui::GetFont(), 13, ImVec2(p.x + 14, p.y + 30), Col(sel ? TEXT2 : TEXT3), sub);
    if (sub2) dl->AddText(ImGui::GetFont(), 12, ImVec2(p.x + 14, p.y + 48), Col(TEXT4), sub2);
    ImGui::Dummy(ImVec2(0, 2));
    return clicked;
  }

  void PlayPage() {
    float H = ImGui::GetWindowHeight() - 62, W = ImGui::GetWindowWidth();
    // Left: the mini-games on the card.
    ImGui::SetCursorPos(ImVec2(0, 62));
    ImGui::BeginChild("games", ImVec2(300, H), ImGuiChildFlags_None);
    ImGui::SetCursorPos(ImVec2(20, 20));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextUnformatted("MINI-GAMES ON THE CARD");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    auto entries = card::List(cardimg);
    int games = 0;
    for (auto& e : entries) {
      if (!e.pocketstation) continue;
      games++;
      char sub[96];
      snprintf(sub, sizeof sub, "block %d%s · %d block%s%s", e.first_block, e.blocks > 1 ? ("-" + std::to_string(e.first_block + e.blocks - 1)).c_str() : "",
               e.blocks, e.blocks == 1 ? "" : "s", e.mcx1 ? " · resumable" : "");
      ImGui::PushID(e.first_block);
      if (ListCard(e.title.c_str(), sub, e.product.c_str(), selected == e.first_block, 260)) selected = e.first_block;
      ImGui::PopID();
    }
    ListCard("Clock", "Built in", nullptr, false, 260);
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT4));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260);
    ImGui::TextUnformatted(games ? "On the clock, Left / Right shows the games on the card; Fire starts the one shown. Hold Fire in a game for its menu."
                                 : "No mini-games on this card yet. Drop a .mcs, .psv or .gme on this window, or load them from the Cards page.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::EndChild();
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + 300, wp.y + 62), ImVec2(wp.x + 300, wp.y + 62 + H), Col(LINE));
    ImGui::SetCursorPos(ImVec2(301, 62));
    ImGui::BeginChild("unit", ImVec2(W - 301, H));
    UnitPanel();
    ImGui::EndChild();
  }

  void UnitPanel() {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 o = ImGui::GetCursorScreenPos();
    const float body_w = 496, body_h = 712;
    float s = std::min(1.0f, std::min(avail.x / (body_w + 40), (avail.y - 40) / body_h));
    ImVec2 c(o.x + avail.x / 2, o.y + (avail.y - 34) / 2);
    ImVec2 bmin(c.x - body_w * s / 2, c.y - body_h * s / 2), bmax(c.x + body_w * s / 2, c.y + body_h * s / 2);
    dl->AddRectFilled(bmin, bmax, Col(CARD), 28 * s);
    dl->AddRect(bmin, bmax, Col(LINE), 28 * s);
    float x0 = bmin.x + 28 * s, y = bmin.y + 26 * s, iw = 440 * s;

    // Status row: LED, IR, speaker, reset.
    bool led = m.LedOn();
    dl->AddCircleFilled(ImVec2(x0 + 5 * s, y + 8 * s), 5 * s, led ? Col(LED_ON) : Col(0x3a2224));
    dl->AddText(ImGui::GetFont(), 13 * s, ImVec2(x0 + 16 * s, y), Col(TEXT3), "LED");
    dl->AddText(ImGui::GetFont(), 13 * s, ImVec2(x0 + 70 * s, y), Col(TEXT3), "IR: off");
    bool spk = m.dac.active && !(m.pio.out & 0x20);
    dl->AddText(ImGui::GetFont(), 13 * s, ImVec2(x0 + 150 * s, y), Col(TEXT3), spk ? "Speaker on" : "Speaker off");
    if (docked) dl->AddText(ImGui::GetFont(), 13 * s, ImVec2(x0 + 250 * s, y), Col(ACCENT), "Docked");
    ImGui::SetCursorScreenPos(ImVec2(x0 + iw - 70 * s, y - 5 * s));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    if (ImGui::Button("Reset", ImVec2(70 * s, 26 * s)) && have_bios) { m.LoadCard(cardimg); m.Reset(); }
    ImGui::PopStyleColor(2);
    y += 34 * s;

    // The screen: 32x32, a gap between pixels like the real glass.
    ImVec2 smin(x0, y), smax(x0 + iw, y + iw);
    dl->AddRectFilled(smin, smax, Col(LCD_BG), 6 * s);
    if (have_bios) {
      u8 px[32][32];
      m.Frame(px);
      float cell = (iw - 24 * s) / 32, gap = std::max(1.0f, cell * 0.08f);
      for (int py = 0; py < 32; py++)
        for (int pxx = 0; pxx < 32; pxx++)
          if (px[py][pxx]) {
            ImVec2 a(smin.x + 12 * s + pxx * cell, smin.y + 12 * s + py * cell);
            dl->AddRectFilled(a, ImVec2(a.x + cell - gap, a.y + cell - gap), Col(LCD_PX));
          }
    } else {
      const char* t1 = "Drop your PocketStation BIOS here";
      const char* t2 = "16 KB, dumped from your own unit";
      ImVec2 s1 = font_bold->CalcTextSizeA(18 * s, FLT_MAX, 0, t1), s2 = ImGui::GetFont()->CalcTextSizeA(14 * s, FLT_MAX, 0, t2);
      dl->AddText(font_bold, 18 * s, ImVec2(smin.x + (iw - s1.x) / 2, smin.y + iw / 2 - 40 * s), Col(LCD_PX), t1);
      dl->AddText(ImGui::GetFont(), 14 * s, ImVec2(smin.x + (iw - s2.x) / 2, smin.y + iw / 2 - 12 * s), Col(0x4a4c3e), t2);
      ImGui::SetCursorScreenPos(ImVec2(smin.x + iw / 2 - 70 * s, smin.y + iw / 2 + 20 * s));
      ImGui::PushStyleColor(ImGuiCol_Button, ColV(LCD_PX));
      ImGui::PushStyleColor(ImGuiCol_Text, ColV(LCD_BG));
      if (ImGui::Button("Choose file...", ImVec2(140 * s, 34 * s))) {
        std::string f = OpenFileDialog("PocketStation BIOS (16 KB)", "*.bin;*.rom");
        if (!f.empty()) UseBios(f);
      }
      ImGui::PopStyleColor(2);
    }
    y += iw + 30 * s;

    // D-pad and Fire, clickable.
    mouse_buttons = 0;
    float dp = 120 * s;
    ImVec2 dc(x0 + 24 * s + dp / 2, y + dp / 2);
    float arm = dp / 2, half = dp * 0.17f;
    unsigned pad_col = 0x2a2a31;
    dl->AddRectFilled(ImVec2(dc.x - half, dc.y - arm), ImVec2(dc.x + half, dc.y + arm), Col(pad_col), 4 * s);
    dl->AddRectFilled(ImVec2(dc.x - arm, dc.y - half), ImVec2(dc.x + arm, dc.y + half), Col(pad_col), 4 * s);
    struct Dir { int btn; ImVec2 a, b; } dirs[4] = {
      {ps::BTN_UP, ImVec2(dc.x - half, dc.y - arm), ImVec2(dc.x + half, dc.y - half)},
      {ps::BTN_DOWN, ImVec2(dc.x - half, dc.y + half), ImVec2(dc.x + half, dc.y + arm)},
      {ps::BTN_LEFT, ImVec2(dc.x - arm, dc.y - half), ImVec2(dc.x - half, dc.y + half)},
      {ps::BTN_RIGHT, ImVec2(dc.x + half, dc.y - half), ImVec2(dc.x + arm, dc.y + half)},
    };
    u8 held = have_bios ? m.buttons : 0;
    for (auto& d : dirs) {
      ImGui::SetCursorScreenPos(d.a);
      ImGui::PushID(d.btn);
      ImGui::InvisibleButton("dir", ImVec2(d.b.x - d.a.x, d.b.y - d.a.y));
      if (ImGui::IsItemActive()) mouse_buttons |= 1 << d.btn;
      ImGui::PopID();
      if (held & (1 << d.btn)) dl->AddRectFilled(d.a, d.b, Col(0x45454f), 4 * s);
      ImVec2 m2((d.a.x + d.b.x) / 2, (d.a.y + d.b.y) / 2);
      float t = 7 * s;
      switch (d.btn) {
        case ps::BTN_UP: dl->AddTriangleFilled(ImVec2(m2.x, m2.y - t), ImVec2(m2.x - t, m2.y + t * 0.6f), ImVec2(m2.x + t, m2.y + t * 0.6f), Col(0x5c5a57)); break;
        case ps::BTN_DOWN: dl->AddTriangleFilled(ImVec2(m2.x, m2.y + t), ImVec2(m2.x + t, m2.y - t * 0.6f), ImVec2(m2.x - t, m2.y - t * 0.6f), Col(0x5c5a57)); break;
        case ps::BTN_LEFT: dl->AddTriangleFilled(ImVec2(m2.x - t, m2.y), ImVec2(m2.x + t * 0.6f, m2.y + t), ImVec2(m2.x + t * 0.6f, m2.y - t), Col(0x5c5a57)); break;
        case ps::BTN_RIGHT: dl->AddTriangleFilled(ImVec2(m2.x + t, m2.y), ImVec2(m2.x - t * 0.6f, m2.y - t), ImVec2(m2.x - t * 0.6f, m2.y + t), Col(0x5c5a57)); break;
      }
    }
    dl->AddCircleFilled(dc, 9 * s, Col(0x222227));
    // Fire, centred on the d-pad's row.
    float fr = 42 * s;
    ImVec2 fc(x0 + iw - 24 * s - fr, dc.y);
    ImGui::SetCursorScreenPos(ImVec2(fc.x - fr, fc.y - fr));
    ImGui::InvisibleButton("fire", ImVec2(fr * 2, fr * 2));
    if (ImGui::IsItemActive()) mouse_buttons |= 1 << ps::BTN_ENTER;
    bool fire = held & (1 << ps::BTN_ENTER);
    dl->AddCircleFilled(fc, fr, Col(fire ? 0xd99236 : ACCENT));
    ImVec2 ft = font_bold->CalcTextSizeA(16 * s, FLT_MAX, 0, "Fire");
    dl->AddText(font_bold, 16 * s, ImVec2(fc.x - ft.x / 2, fc.y - ft.y / 2), Col(BG), "Fire");

    // Key hints under the unit.
    std::string hint = "Arrows: d-pad      Z / Enter: Fire      ";
    hint += pad ? std::string("Controller: ") + SDL_GameControllerName(pad) : "Controller: plug one in";
    ImVec2 hs = ImGui::CalcTextSize(hint.c_str());
    dl->AddText(ImVec2(c.x - hs.x / 2, bmax.y + 14), Col(TEXT3), hint.c_str());
  }

  void CardsPage() {
    float H = ImGui::GetWindowHeight() - 62, W = ImGui::GetWindowWidth();
    ImGui::SetCursorPos(ImVec2(0, 62));
    ImGui::BeginChild("cardside", ImVec2(320, H));
    ImGui::SetCursorPos(ImVec2(20, 20));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextUnformatted("ON THIS PC");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    char sub[96];
    snprintf(sub, sizeof sub, "%d of 15 blocks used", 15 - card::FreeBlocks(cardimg));
    ListCard(fs::path(CardPath()).filename().string().c_str(), sub, nullptr, true, 280);
    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Load a card...", ImVec2(280, 34))) {
      std::string f = OpenFileDialog("Memory cards and saves", "*.mcd;*.mcr;*.mc;*.srm;*.gme;*.mcs;*.psv;*.psx;*.ps1");
      if (!f.empty()) Open(f);
    }
    if (ImGui::Button("Add a save or mini-game...", ImVec2(280, 34))) {
      std::string f = OpenFileDialog("Single saves", "*.mcs;*.psv;*.psx;*.ps1");
      if (!f.empty()) Open(f);
    }
    if (ImGui::Button("Save the card as...", ImVec2(280, 34))) {
      std::string f = SaveFileDialog("Memory card (raw)", "*.mcd", "mcd", "PocketStation.mcd");
      if (!f.empty()) Note(card::Save(f, cardimg) ? "Card saved to " + f : "Couldn't write " + f);
    }
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextUnformatted("NETWORK CARD");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    std::string ns = server.Listening() ? (server.Connected() ? "Connected: " + server.Peer() : "Waiting on port " + std::to_string(server.port)) : "Off";
    snprintf(sub, sizeof sub, "%d commands answered", server.commands);
    ListCard(ns.c_str(), sub, nullptr, false, 280);
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextUnformatted("BIOS");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    ListCard(have_bios ? fs::path(cfg.bios).filename().string().c_str() : "None yet", have_bios ? "Your own dump" : "Drop a 16 KB BIOS file", nullptr, false, 280);
    if (ImGui::Button("Choose BIOS...", ImVec2(280, 34))) {
      std::string f = OpenFileDialog("PocketStation BIOS (16 KB)", "*.bin;*.rom");
      if (!f.empty()) UseBios(f);
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + 320, wp.y + 62), ImVec2(wp.x + 320, wp.y + 62 + H), Col(LINE));
    ImGui::SetCursorPos(ImVec2(321, 62));
    ImGui::BeginChild("cardmain", ImVec2(W - 321, H));
    ImGui::SetCursorPos(ImVec2(32, 24));
    ImGui::BeginGroup();
    ImGui::PushFont(font_big);
    ImGui::TextUnformatted(fs::path(CardPath()).filename().string().c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, 14);
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::Text("on this PC · %d of 15 blocks used", 15 - card::FreeBlocks(cardimg));
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 10));

    auto entries = card::List(cardimg);
    std::map<int, const card::Entry*> owner;
    for (auto& e : entries) {
      int cur = e.first_block;
      for (int i = 0; i < e.blocks && cur >= 1 && cur < 16; i++) {
        owner[cur] = &e;
        int next = cardimg[cur * 128 + 8] | cardimg[cur * 128 + 9] << 8;
        if (next == 0xFFFF) break;
        cur = next + 1;
      }
    }
    float gw = ImGui::GetContentRegionAvail().x - 32, cw = (gw - 40) / 5;
    auto* dl = ImGui::GetWindowDrawList();
    for (int b = 1; b < 16; b++) {
      ImVec2 p = ImGui::GetCursorScreenPos();
      auto it = owner.find(b);
      ImGui::PushID(b);
      ImGui::InvisibleButton("blk", ImVec2(cw, 92));
      if (ImGui::IsItemClicked() && it != owner.end()) selected = it->second->first_block;
      ImGui::PopID();
      ImVec2 q(p.x + cw, p.y + 92);
      if (it == owner.end()) {
        dl->AddRect(p, q, Col(LINE), 10);
        ImVec2 ts = ImGui::CalcTextSize("Free");
        dl->AddText(ImVec2(p.x + (cw - ts.x) / 2, p.y + 46 - ts.y / 2), Col(TEXT4), "Free");
      } else {
        const card::Entry& e = *it->second;
        bool sel = selected == e.first_block;
        dl->AddRectFilled(p, q, Col(sel ? ACCENT_BG : PANEL), 10);
        if (sel) dl->AddRect(p, q, Col(ACCENT), 10);
        dl->PushClipRect(p, q, true);
        dl->AddText(font_bold, 14, ImVec2(p.x + 12, p.y + 12), Col(TEXT), e.title.c_str());
        char s2[64];
        snprintf(s2, sizeof s2, "%s · block %d of %d", e.pocketstation ? "PocketStation app" : "save", b - e.first_block + 1, e.blocks);
        dl->AddText(ImGui::GetFont(), 12, ImVec2(p.x + 12, p.y + 34), Col(TEXT3), s2);
        dl->AddText(ImGui::GetFont(), 12, ImVec2(p.x + 12, p.y + 52), Col(TEXT4), e.product.c_str());
        dl->PopClipRect();
      }
      if (b % 5 != 0) ImGui::SameLine(0, 10);
      else ImGui::Dummy(ImVec2(0, 2));
    }

    const card::Entry* se = nullptr;
    for (auto& e : entries) if (e.first_block == selected) se = &e;
    if (se) {
      ImGui::Dummy(ImVec2(0, 10));
      ImVec2 p = ImGui::GetCursorScreenPos();
      dl->AddRectFilled(p, ImVec2(p.x + gw, p.y + 120), Col(PANEL), 12);
      dl->AddRect(p, ImVec2(p.x + gw, p.y + 120), Col(LINE), 12);
      ImGui::SetCursorScreenPos(ImVec2(p.x + 20, p.y + 16));
      ImGui::BeginGroup();
      ImGui::PushFont(font_bold);
      ImGui::TextUnformatted(se->title.c_str());
      ImGui::PopFont();
      ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT2));
      ImGui::Text("%s · %s · %d block%s", se->product.c_str(), se->pocketstation ? (se->mcx1 ? "PocketStation app, resumable" : "PocketStation app") : "save",
                  se->blocks, se->blocks == 1 ? "" : "s");
      ImGui::PopStyleColor();
      ImGui::Dummy(ImVec2(0, 6));
      ImGui::PushStyleColor(ImGuiCol_Button, ColV(LINE));
      if (ImGui::Button("Save as .mcs...", ImVec2(150, 34))) {
        std::string f = SaveFileDialog("Single save", "*.mcs", "mcs", se->product + ".mcs");
        if (!f.empty()) {
          auto data = card::ExportSave(cardimg, se->first_block);
          FILE* fo = fopen(f.c_str(), "wb");
          bool ok = fo && fwrite(data.data(), 1, data.size(), fo) == data.size();
          if (fo) fclose(fo);
          Note(ok ? "Saved " + f : "Couldn't write " + f);
        }
      }
      ImGui::PopStyleColor();
      ImGui::EndGroup();
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 130));
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT4));
    ImGui::TextUnformatted("The card saves itself as you play. Loading another card keeps the old one as .bak.");
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::EndChild();
  }

  void Frame() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    TopBar();
    if (page == 0) PlayPage();
    else CardsPage();
    if (now < note_until && !note.empty()) {
      auto* dl = ImGui::GetForegroundDrawList();
      ImVec2 ts = ImGui::CalcTextSize(note.c_str());
      ImVec2 a(vp->Pos.x + (vp->Size.x - ts.x) / 2 - 16, vp->Pos.y + vp->Size.y - 56);
      dl->AddRectFilled(a, ImVec2(a.x + ts.x + 32, a.y + 36), Col(PANEL), 10);
      dl->AddRect(a, ImVec2(a.x + ts.x + 32, a.y + 36), Col(ACCENT), 10);
      dl->AddText(ImVec2(a.x + 16, a.y + 18 - ts.y / 2), Col(TEXT), note.c_str());
    }
    ImGui::End();
  }
};

static void Style() {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0;
  s.ChildRounding = 0;
  s.FrameRounding = 8;
  s.FrameBorderSize = 1;
  s.FramePadding = ImVec2(14, 8);
  s.ItemSpacing = ImVec2(8, 8);
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ColV(BG);
  c[ImGuiCol_ChildBg] = ColV(BG);
  c[ImGuiCol_Text] = ColV(TEXT);
  c[ImGuiCol_Border] = ColV(LINE);
  c[ImGuiCol_Button] = ColV(LINE);
  c[ImGuiCol_ButtonHovered] = ColV(0x3a3a43);
  c[ImGuiCol_ButtonActive] = ColV(0x4a4a55);
  c[ImGuiCol_FrameBg] = ColV(PANEL);
  c[ImGuiCol_ScrollbarBg] = ColV(BG);
}

int main(int argc, char** argv) {
  std::string shot, bios_arg, card_arg;
  int shot_frames = 120, shot_page = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--screenshot" && i + 1 < argc) shot = argv[++i];
    else if (a == "--frames" && i + 1 < argc) shot_frames = atoi(argv[++i]);
    else if (a == "--page" && i + 1 < argc) shot_page = atoi(argv[++i]);
    else if (a == "--bios" && i + 1 < argc) bios_arg = argv[++i];
    else if (a == "--card" && i + 1 < argc) card_arg = argv[++i];
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  std::string title = std::string(APP_NAME) + " " + APP_VERSION + " — PocketStation";
  SDL_Window* win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 820,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImFontConfig fc;
  fc.FontDataOwnedByAtlas = false;
  io.Fonts->AddFontFromMemoryCompressedTTF(inter_regular_compressed_data, inter_regular_compressed_size, 15, &fc);
  App app;
  app.font_bold = io.Fonts->AddFontFromMemoryCompressedTTF(inter_semibold_compressed_data, inter_semibold_compressed_size, 16, &fc);
  app.font_big = io.Fonts->AddFontFromMemoryCompressedTTF(inter_semibold_compressed_data, inter_semibold_compressed_size, 26, &fc);
  Style();
  ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
  ImGui_ImplSDLRenderer2_Init(ren);

  char* base = SDL_GetBasePath();
  app.dir = base ? base : "./";
  SDL_free(base);
  app.cfg.Load(app.dir + "mcps.ini");
  if (!card_arg.empty()) app.cfg.card = card_arg;
  app.LoadCardFile();
  if (app.cfg.path.size() && !fs::exists(app.cfg.path)) app.cfg.Save();

  // The BIOS: the one in the settings, else a 16 KB .bin/.rom beside the exe.
  std::string bios = !bios_arg.empty() ? bios_arg : app.cfg.bios;
  if (bios.empty() || !fs::exists(bios)) {
    bios.clear();
    for (const char* n : {"boot.rom", "J110.bin", "kernel.bin", "POCKSTAT.ROM", "KROM"})
      if (fs::exists(app.dir + n) && fs::file_size(app.dir + n) == 16384) { bios = app.dir + n; break; }
    if (bios.empty())
      for (auto& e : fs::directory_iterator(app.dir)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (e.is_regular_file() && e.file_size() == 16384 && (ext == ".bin" || ext == ".rom")) { bios = e.path().string(); break; }
      }
  }
  if (!bios.empty()) app.UseBios(bios);
  app.page = shot_page;

  SDL_AudioSpec want{}, got{};
  want.freq = 32768;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 512;
  app.audio = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
  if (app.audio) SDL_PauseAudioDevice(app.audio, 0);
  app.StartServer();

  auto last = std::chrono::steady_clock::now();
  bool run = true;
  int frame = 0;
  while (run) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL2_ProcessEvent(&e);
      if (e.type == SDL_QUIT) run = false;
      if (e.type == SDL_DROPFILE) { app.Open(e.drop.file); SDL_free(e.drop.file); }
      if (e.type == SDL_CONTROLLERDEVICEADDED && !app.pad) app.pad = SDL_GameControllerOpen(e.cdevice.which);
      if (e.type == SDL_CONTROLLERDEVICEREMOVED && app.pad &&
          SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(app.pad)) == e.cdevice.which) {
        SDL_GameControllerClose(app.pad);
        app.pad = nullptr;
      }
      if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F2 && app.have_bios) { app.m.LoadCard(app.cardimg); app.m.Reset(); }
      if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK && app.have_bios) { app.m.LoadCard(app.cardimg); app.m.Reset(); }
      if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) app.page = 0;
      if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) app.page = 1;
    }
    auto t = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(t - last).count();
    last = t;
    if (!shot.empty()) dt = 1.0 / 60;   // screenshots run on a fixed clock
    app.Step(dt, SDL_GetKeyboardState(nullptr));

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    app.Frame();
    ImGui::Render();
    SDL_SetRenderDrawColor(ren, 14, 14, 16, 255);
    SDL_RenderClear(ren);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);

    if (!shot.empty() && ++frame >= shot_frames) {
      int w, h;
      SDL_GetRendererOutputSize(ren, &w, &h);
      SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
      SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
      SDL_SaveBMP(s, shot.c_str());
      SDL_FreeSurface(s);
      run = false;
    }
    SDL_RenderPresent(ren);
  }

  if (app.card_save_at >= 0) card::Save(app.CardPath(), app.cardimg);
  app.server.Close();
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  if (app.audio) SDL_CloseAudioDevice(app.audio);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
