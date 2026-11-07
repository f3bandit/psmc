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
#include "alerts.hpp"
#include "card.hpp"
#include "net.hpp"

#include "fonts/inter_regular.h"
#include "fonts/inter_semibold.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

#ifdef __ANDROID__
// Android: the system file picker and "save to" dialog live in MCPSActivity (Java); picked files
// arrive here as paths in the app's own folder, on the UI thread, and are taken up by the loop.
#include <jni.h>
#include <mutex>
static std::mutex g_android_mu;
static std::vector<std::string> g_android_files, g_android_notes;

extern "C" JNIEXPORT void JNICALL Java_com_f3bandit_mcps_MCPSActivity_nativeFilePicked(JNIEnv* env, jclass, jstring path) {
  const char* c = env->GetStringUTFChars(path, nullptr);
  std::lock_guard<std::mutex> lk(g_android_mu);
  g_android_files.push_back(c);
  env->ReleaseStringUTFChars(path, c);
}
extern "C" JNIEXPORT void JNICALL Java_com_f3bandit_mcps_MCPSActivity_nativeNote(JNIEnv* env, jclass, jstring text) {
  const char* c = env->GetStringUTFChars(text, nullptr);
  std::lock_guard<std::mutex> lk(g_android_mu);
  g_android_notes.push_back(c);
  env->ReleaseStringUTFChars(text, c);
}

static void AndroidCallStatic(const char* method, const char* sig, const std::string& a = "", const std::string& b = "") {
  JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if (!env || !activity) return;
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = env->GetStaticMethodID(cls, method, sig);
  if (mid) {
    if (a.empty() && b.empty()) env->CallStaticVoidMethod(cls, mid);
    else {
      jstring ja = env->NewStringUTF(a.c_str()), jb = env->NewStringUTF(b.c_str());
      env->CallStaticVoidMethod(cls, mid, ja, jb);
      env->DeleteLocalRef(ja);
      env->DeleteLocalRef(jb);
    }
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}

static float AndroidFloat(const char* method, float fallback) {
  JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if (!env || !activity) return fallback;
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = env->GetStaticMethodID(cls, method, "()F");
  float v = mid ? env->CallStaticFloatMethod(cls, mid) : fallback;
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return v > 0 ? v : fallback;
}
#endif

namespace fs = std::filesystem;
using ps::u8;

static const char* APP_NAME = "MC/PS MemCard Pro";
static const char* APP_VERSION = "0.1.13";
#ifdef __ANDROID__
#define ON_THIS_DEVICE "on this phone"
#define ON_THIS_DEVICE_CAPS "ON THIS PHONE"
#else
#define ON_THIS_DEVICE "on this PC"
#define ON_THIS_DEVICE_CAPS "ON THIS PC"
#endif

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
  unsigned serial = 0;           // this unit's serial number, also kept in the saved state
  bool unlock = true;            // move apps locked to another unit over to this one
  bool mute = false;
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
      else if (k == "unlock") unlock = atoi(v.c_str()) != 0;
      else if (k == "mute") mute = atoi(v.c_str()) != 0;
      else if (k == "serial") serial = (unsigned)strtoul(v.c_str(), nullptr, 16);
    }
    fclose(f);
  }
  void Save() const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "bios=%s\ncard=%s\nport=%d\n", bios.c_str(), card.c_str(), port);
    if (serial) fprintf(f, "serial=%08X\n", serial);
    fprintf(f, "unlock=%d\n", unlock ? 1 : 0);
    fprintf(f, "mute=%d\n", mute ? 1 : 0);
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
#elif defined(__ANDROID__)
  (void)filter_desc; (void)patterns;
  AndroidCallStatic("pickFile", "()V");   // the file arrives later, through Open()
  return "";
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

  // Writes `data` where the person chooses: a save dialog on Windows, Android's "save to" picker
  // (from a copy in the app's cache) on a phone.
  void Export(const char* desc, const char* pattern, const char* ext, const std::string& name, const std::vector<u8>& data) {
#ifdef __ANDROID__
    (void)desc; (void)pattern; (void)ext;
    std::string tmp = dir + "export.tmp";
    FILE* fo = fopen(tmp.c_str(), "wb");
    bool ok = fo && fwrite(data.data(), 1, data.size(), fo) == data.size();
    if (fo) fclose(fo);
    if (!ok) { Note("Couldn't prepare " + name); return; }
    AndroidCallStatic("exportFile", "(Ljava/lang/String;Ljava/lang/String;)V", tmp, name);
#else
    std::string f = SaveFileDialog(desc, pattern, ext, name);
    if (f.empty()) return;
    FILE* fo = fopen(f.c_str(), "wb");
    bool ok = fo && fwrite(data.data(), 1, data.size(), fo) == data.size();
    if (fo) fclose(fo);
    Note(ok ? "Saved " + f : "Couldn't write " + f);
#endif
  }

  std::string CardPath() const { return cfg.card.empty() ? dir + "PocketStation.mcd" : cfg.card; }

  bool UseBios(const std::string& path) {
    auto rom = ReadFile(path);
    if (rom.size() != 16384) { Note("That isn't a PocketStation BIOS: it should be exactly 16 KB."); return false; }
    if (have_bios) SaveState();   // keep the unit as it was under the BIOS being replaced
    m.LoadBios(rom);
    bios_rom = rom;
    have_bios = true;
    cfg.bios = path;
    cfg.Save();
    PowerOn();
    return true;
  }

  // A real unit has a serial number from the factory. A new virtual one gets its own, kept in
  // the saved state from then on.
  u32 Serial() const { return (u32)m.sig.serial_hi << 16 | m.sig.serial_lo; }
  void GiveSerial() {
    u32 v = Serial();
    if (v == 0xFFFFFFFFu || v == 0) v = cfg.serial;   // the state was lost: the ini remembers it
    if (v == 0xFFFFFFFFu || v == 0) {
      // A new unit. Mixed from the clock and this process, not std::random_device, which some
      // MinGW runtimes don't back with real entropy.
      uint64_t x = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
      x ^= (uint64_t)time(nullptr) << 32 ^ (uint64_t)(uintptr_t)this;
      do {   // splitmix64
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        v = (u32)(z ^ (z >> 31));
      } while (v == 0 || v == 0xFFFFFFFFu);
    }
    m.sig.serial_lo = v & 0xFFFF;
    m.sig.serial_hi = v >> 16;
    if (cfg.serial != v) { cfg.serial = v; cfg.Save(); }
  }
  void Bind() {
    if (cfg.unlock && card::BindApps(cardimg, Serial())) { card::Save(CardPath(), cardimg); m.LoadCard(cardimg); watcher.Reset(); }
  }

  void PowerOn() {
    m.LoadCard(cardimg); watcher.Reset();
    home.clear();
    LoadHome();
    if (RestoreState()) { GiveSerial(); Bind(); return; }
    GiveSerial();
    Bind();
    m.LoadCard(cardimg); watcher.Reset();
    m.Reset();
    clock_sync_at = now + 1.0;   // once the kernel has finished starting up
  }

  // --- the unit's battery: kept in PocketStation.state beside the exe ------------------------
  // A real PocketStation keeps its RAM, its clock and whatever it was doing while it sits in a
  // drawer. The app does the same: the whole unit is saved on exit (and every 30 s, in case
  // Windows doesn't let it finish) and picked up again at start, with the clock moved on by the
  // time the app was closed. The card isn't in it; it stays in its own file.
  std::vector<u8> bios_rom;
  double state_save_at = -1;
  std::string StatePath() const { return dir + "PocketStation.state"; }
  static u32 Fnv(const std::vector<u8>& v) { u32 h = 2166136261u; for (u8 b : v) h = (h ^ b) * 16777619u; return h; }

  static long long DaysFromCivil(int y, int m, int d) {   // Howard Hinnant's algorithm
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
  }
  static void CivilFromDays(long long z, int& y, int& m, int& d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400) + (m <= 2);
  }
  static int FromBcd(u32 v) { return (int)((v >> 4 & 15) * 10 + (v & 15)); }

  // Moves the unit's clock on by `secs`, as the crystal would have while the app was closed.
  void AdvanceClock(long long secs) {
    if (secs <= 0) return;
    u32 c = m.rtc.clock, k = m.rtc.calendar;
    int ss = FromBcd(c), mi = FromBcd(c >> 8), hh = FromBcd(c >> 16), dow = (int)(c >> 24 & 7);
    int yy = FromBcd(k >> 16), mo = FromBcd(k >> 8), dd = FromBcd(k);
    if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59 || ss > 59) return;
    long long days = DaysFromCivil(2000 + yy, mo, dd);
    long long t = days * 86400 + hh * 3600 + mi * 60 + ss + secs;
    long long nd = t / 86400, rem = t % 86400;
    int y, mm, d;
    CivilFromDays(nd, y, mm, d);
    if (dow >= 1 && dow <= 7) dow = (int)((dow - 1 + (nd - days)) % 7) + 1;
    m.SetClock((u32)dow << 24 | Bcd((int)(rem / 3600)) << 16 | Bcd((int)(rem / 60 % 60)) << 8 | Bcd((int)(rem % 60)),
               (u32)Bcd(y % 100) << 16 | (u32)Bcd(mm) << 8 | Bcd(d));
  }

  void SaveState() {
    if (!have_bios) return;
    std::vector<u8> out = {'M', 'C', 'P', 'S', 'B', 'A', 'T', '1'};
    u32 h = Fnv(bios_rom);
    long long wall = (long long)time(nullptr);
    out.insert(out.end(), (u8*)&h, (u8*)&h + 4);
    out.insert(out.end(), (u8*)&wall, (u8*)&wall + 8);
    auto st = m.SaveState();
    out.insert(out.end(), st.begin(), st.end());
    std::string p = StatePath(), tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp.c_str()); return; }
    remove(p.c_str());
    rename(tmp.c_str(), p.c_str());
    SaveHome();
  }

  // --- leaving a game ---------------------------------------------------------------------
  // Most PocketStation games have no way back to the clock but the Reset button, which also
  // loses the clock setting. So whenever the unit sits in the kernel (the clock, browsing the
  // card) the app keeps a snapshot of it, and "Exit game" goes back to that snapshot with the
  // card as it is now and the clock moved on: the game simply stops, the way closing it would.
  // A card must never change under a running game (the game keeps state in RAM and writes it
  // over whatever is now on the card), so dropping a file in a game exits it first.
  float in_game = 0;                 // 1 while a game owns the unit
  double speaker_until = 0;          // the speaker indicator stays lit until then
  bool background = false;           // put away, still running (phones)
  SDL_Window* window = nullptr;
  alerts::Watcher watcher;           // games calling: see alerts.hpp
  double kernel_since = -1;
  std::vector<u8> home;              // the unit at the clock
  std::vector<u8> candidate;         // the next one, until a second later proves it
  long long candidate_wall = 0;
  long long home_wall = 0;
  std::string HomePath() const { return dir + "PocketStation.home"; }
  bool InGame() const { return in_game > 0.5f; }
  void ToggleMute() {
    cfg.mute = !cfg.mute;
    cfg.Save();
    if (audio && cfg.mute) SDL_ClearQueuedAudio(audio);
    Note(cfg.mute ? "Sound muted" : "Sound on");
  }

  bool InGameNow() { in_game = KernelScreen() ? 0.0f : 1.0f; return in_game > 0.5f; }

  // The J110 kernel's own screens (greeting, clock, browsing the card) keep their state at the
  // bottom of RAM; a game owns that RAM and the kernel clears it when it starts one. Where the CPU
  // happens to be says little: MGS sits in the kernel's idle loop most of the time.
  bool KernelScreen() const { return m.sram[0x230] == 0x01 && m.sram[0x240] == 0x01 && m.sram[0x236] != 0x00; }

  void TrackHome() {
    bool app = !KernelScreen();
    in_game = app ? 1.0f : 0.0f;
    // A snapshot only becomes the one to go back to once the unit has stayed on the kernel's
    // screens for another second after it, so it can't be the instant a game was launched.
    if (app || docked || m.buttons) { kernel_since = -1; candidate.clear(); return; }
    if (kernel_since < 0) kernel_since = now;
    if (now - kernel_since >= 1.0) {
      if (!candidate.empty()) { home = candidate; home_wall = candidate_wall; }
      candidate = m.SaveState();
      candidate_wall = (long long)time(nullptr);
      kernel_since = now;
    }
  }

  void SaveHome() {
    if (home.empty() || !have_bios) return;
    std::vector<u8> out = {'M', 'C', 'P', 'S', 'H', 'O', 'M', '2'};
    u32 h = Fnv(bios_rom);
    out.insert(out.end(), (u8*)&h, (u8*)&h + 4);
    out.insert(out.end(), (u8*)&home_wall, (u8*)&home_wall + 8);
    out.insert(out.end(), home.begin(), home.end());
    FILE* f = fopen(HomePath().c_str(), "wb");
    if (!f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
  }

  void LoadHome() {
    auto d = ReadFile(HomePath());
    if (d.size() < 20 || memcmp(d.data(), "MCPSHOM2", 8) != 0) return;
    u32 h;
    memcpy(&h, &d[8], 4);
    if (h != Fnv(bios_rom)) return;
    memcpy(&home_wall, &d[12], 8);
    home.assign(d.begin() + 20, d.end());
  }

  // No snapshot (a game was already running when this version first started): restart the
  // unit and do what a person would, quickly: past the greeting, the clock from the PC (5Ch),
  // then hold Up into setting mode and confirm each field so the kernel counts it as set.
  void ColdReturn() {
    m.LoadCard(cardimg); watcher.Reset();
    m.Reset();
    auto press = [&](u8 mask, double hold) { m.SetButtons(mask); m.RunFor(hold); m.SetButtons(0); m.RunFor(0.5); };
    m.RunFor(1.0);
    SyncClock();
    m.RunFor(0.5);
    press(1 << ps::BTN_ENTER, 0.15);
    m.RunFor(0.5);
    press(1 << ps::BTN_UP, 2.0);
    for (int i = 0; i < 6; i++) press(1 << ps::BTN_ENTER, 0.12);
    m.RunFor(1.0);
    SyncClock();
    in_game = 0;
    home = m.SaveState();
    home_wall = (long long)time(nullptr);
  }

  bool ExitGame() {
    if (home.empty()) { ColdReturn(); return true; }
    u32 serial = Serial();
    if (!m.LoadState(home)) return false;
    m.sig.serial_lo = serial & 0xFFFF;   // the unit is the same one
    m.sig.serial_hi = serial >> 16;
    m.LoadCard(cardimg); watcher.Reset();
    AdvanceClock((long long)time(nullptr) - home_wall);
    home_wall = (long long)time(nullptr);
    home = m.SaveState();
    in_game = 0;
    Rescan();
    return true;
  }

  bool RestoreState() {
    auto d = ReadFile(StatePath());
    if (d.size() < 20 || memcmp(d.data(), "MCPSBAT1", 8) != 0) return false;
    u32 h; long long wall;
    memcpy(&h, &d[8], 4);
    memcpy(&wall, &d[12], 8);
    if (h != Fnv(bios_rom)) return false;   // saved with another BIOS: start fresh
    if (!m.LoadState(std::vector<u8>(d.begin() + 20, d.end()))) return false;
    m.LoadCard(cardimg); watcher.Reset();
    AdvanceClock((long long)time(nullptr) - wall);
    Rescan();   // the card file may have changed while the app was closed
    return true;
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

  // After the card changes from this side (a file dropped, a card loaded), the unit has to be
  // told, as a console does: the J110 kernel lists the games on the card only at power-on and
  // after it has been docked. So it gets a brief dock with a harmless ID command (53h).
  void Rescan() {
    if (!have_bios) return;
    bool was = docked;
    m.SetDocked(true);
    m.RunFor(0.2);
    m.CardCommand({0x81, 0x53, 0, 0, 0, 0, 0, 0, 0, 0});
    m.RunFor(0.3);
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
    if (have_bios && InGameNow()) {
      if (!ExitGame()) {
        Note("A game is running and there's no saved clock to go back to: press Reset (you'll set the clock again), then drop the file.");
        return;
      }
    }
    if (size == card::SIZE || size == card::SIZE + 3904) {
      // A whole card: it becomes the card in the PocketStation, the old one kept as .bak.
      std::vector<u8> img;
      std::string err = card::Load(path, img);
      if (!err.empty()) { Note(err); return; }
      card::Save(CardPath() + ".bak", cardimg);
      cardimg = img;
      if (cfg.unlock) card::BindApps(cardimg, Serial());
      card::Save(CardPath(), cardimg);
      m.LoadCard(cardimg); watcher.Reset();
      Rescan();
      Note("Card loaded from " + fs::path(path).filename().string() + " (the previous card is in " + fs::path(CardPath()).filename().string() + ".bak)");
      return;
    }
    // A single save: onto the card.
    std::vector<u8> img = cardimg;
    std::string err = card::Load(path, img);
    if (!err.empty()) { Note(fs::path(path).filename().string() + ": " + err); return; }
    cardimg = img;
    if (cfg.unlock) card::BindApps(cardimg, Serial());
    card::Save(CardPath(), cardimg);
    m.LoadCard(cardimg); watcher.Reset();
    Rescan();
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

  // A game is calling: in the app, and as a notification when the app isn't in front.
  void Deliver(const alerts::Alert& a) {
    Note(a.title + ": " + a.text);
#ifdef __ANDROID__
    if (background) AndroidCallStatic("postAlert", "(Ljava/lang/String;Ljava/lang/String;)V", a.title, a.text);
#else
    if (window && !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED);
#endif
  }

  void Step(double dt, const u8* keys, double cap = 0.1) {
    now += dt;
    if (!have_bios) return;
    u8 held = Buttons(keys);
    m.SetButtons(held);
    if (docked && now >= undock_at) { docked = false; m.SetDocked(false); }
    if (clock_sync_at >= 0 && now >= clock_sync_at) { clock_sync_at = -1; SyncClock(); }
    m.RunFor(std::min(dt, cap));
    TrackHome();
    if (state_save_at < 0) state_save_at = now + 30;
    if (now >= state_save_at) { SaveState(); state_save_at = now + 30; }
    if (m.CardDirty()) { cardimg = m.Card(); card_save_at = now + 1.0; }
    if (card_save_at >= 0 && now >= card_save_at) { card::Save(CardPath(), cardimg); card_save_at = -1; }
    server.Poll();
    bool sound = false;
    {
      // The speaker indicator follows the sound itself: lit while samples moved recently.
      const auto& pending = m.audio;
      for (size_t i = 1; i < pending.size(); i++)
        if (pending[i] != pending[i - 1]) { speaker_until = now + 0.25; sound = true; break; }
    }
    {
      std::vector<alerts::Alert> calls;
      watcher.Poll(m, now, sound, held != 0, calls);
      for (auto& a : calls) Deliver(a);
    }
    if (audio && !cfg.mute) {
      auto a = m.TakeAudio();
      if (SDL_GetQueuedAudioSize(audio) < 32768 * 2 / 6) {
        std::vector<int16_t> out(a.size());
        for (size_t i = 0; i < a.size(); i++) out[i] = (int16_t)std::clamp(a[i] * 48, -32767, 32767);
        if (!out.empty()) SDL_QueueAudio(audio, out.data(), (Uint32)(out.size() * 2));
      }
    } else {
      m.TakeAudio();   // no sound device: don't let samples pile up
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
    bool tall = Portrait();
    dl->AddText(font_bold, 18, ImVec2(p.x + (tall ? 20 : 28), p.y + 21), Col(ACCENT), tall ? "POCKETSTATION" : "P O C K E T S T A T I O N");
    const char* tabs[2] = {"Play", "Cards"};
    for (int i = 0; i < 2; i++) {
      bool on = page == i;
      ImGui::SetCursorPos(ImVec2(tall ? w - 20 - (2 - i) * 80 + 8 : 280.0f + i * 80, 14));
      ImGui::PushStyleColor(ImGuiCol_Button, on ? ColV(ACCENT_BG) : ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_Border, on ? ColV(ACCENT) : ColV(LINE));
      ImGui::PushStyleColor(ImGuiCol_Text, on ? ColV(TEXT) : ColV(TEXT3));
      if (ImGui::Button(tabs[i], ImVec2(72, 34))) page = i;
      ImGui::PopStyleColor(3);
    }
    // Network chip on the right (on a phone held upright it's on the Cards page instead).
    if (tall) { dl->AddLine(ImVec2(p.x, p.y + 61), ImVec2(p.x + w, p.y + 61), Col(LINE)); return; }
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

  // A phone held upright: the window is taller than wide.
  bool Portrait() const { ImVec2 d = ImGui::GetIO().DisplaySize; return d.y > d.x; }

  // Touch scrolling for a child window: a drag that didn't start on a widget scrolls it.
  void DragScroll() {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseDragging(0) && !ImGui::IsAnyItemActive())
      ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
  }

  void PlayPage() {
    float H = ImGui::GetWindowHeight() - 62, W = ImGui::GetWindowWidth();
    if (Portrait()) {
      float uh = std::min(H * 0.74f, (W - 20) / 536.0f * 712 + 70);
      ImGui::SetCursorPos(ImVec2(0, 62));
      ImGui::BeginChild("unit", ImVec2(W, uh));
      UnitPanel();
      ImGui::EndChild();
      ImGui::SetCursorPos(ImVec2(0, 62 + uh));
      ImGui::BeginChild("games", ImVec2(W, H - uh), ImGuiChildFlags_None);
      ImGui::SetCursorPos(ImVec2(20, 8));
      GameList(W - 40);
      DragScroll();
      ImGui::EndChild();
      return;
    }
    // Left: the mini-games on the card.
    ImGui::SetCursorPos(ImVec2(0, 62));
    ImGui::BeginChild("games", ImVec2(300, H), ImGuiChildFlags_None);
    ImGui::SetCursorPos(ImVec2(20, 20));
    GameList(260);
    ImGui::EndChild();
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + 300, wp.y + 62), ImVec2(wp.x + 300, wp.y + 62 + H), Col(LINE));
    ImGui::SetCursorPos(ImVec2(301, 62));
    ImGui::BeginChild("unit", ImVec2(W - 301, H));
    UnitPanel();
    ImGui::EndChild();
  }

  void GameList(float lw) {
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
      if (ListCard(alerts::DisplayName(e).c_str(), sub, e.product.c_str(), selected == e.first_block, lw)) selected = e.first_block;
      ImGui::PopID();
    }
    ListCard("Clock", "Built in", nullptr, false, lw);
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT4));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + lw);
#ifdef __ANDROID__
    ImGui::TextUnformatted(games ? "On the clock, Left / Right shows the games on the card; Fire starts the one shown. Exit game goes back to the clock."
                                 : "No mini-games on this card yet. Add them from the Cards page.");
#else
    ImGui::TextUnformatted(games ? "On the clock, Left / Right shows the games on the card; Fire starts the one shown. Exit game (F3, or Start on a controller) goes back to the clock."
                                 : "No mini-games on this card yet. Drop a .mcs, .psv or .gme on this window, or load them from the Cards page.");
#endif
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
  }

  // The screen: 32x32, a gap between pixels like the real glass. Without a BIOS, where to get one.
  void Lcd(ImDrawList* dl, ImVec2 at, float iw, float s) {
    float x0 = at.x, y = at.y;
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
        if (!f.empty()) Open(f);
      }
      ImGui::PopStyleColor(2);
    }
  }

  // D-pad on the left and Fire on the right of a row `iw` wide, clickable and touchable.
  void Controls(ImDrawList* dl, ImVec2 at, float iw, float s, float dp, float fr) {
    float x0 = at.x, y = at.y;
    mouse_buttons = 0;
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
    ImVec2 fc(x0 + iw - 24 * s - fr, dc.y);
    ImGui::SetCursorScreenPos(ImVec2(fc.x - fr, fc.y - fr));
    ImGui::InvisibleButton("fire", ImVec2(fr * 2, fr * 2));
    if (ImGui::IsItemActive()) mouse_buttons |= 1 << ps::BTN_ENTER;
    bool fire = held & (1 << ps::BTN_ENTER);
    dl->AddCircleFilled(fc, fr, Col(fire ? 0xd99236 : ACCENT));
    ImVec2 ft = font_bold->CalcTextSizeA(16 * s, FLT_MAX, 0, "Fire");
    dl->AddText(font_bold, 16 * s, ImVec2(fc.x - ft.x / 2, fc.y - ft.y / 2), Col(BG), "Fire");
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
    const char* ir = m.IrSending() ? "IR: sending" : m.IrListening() ? "IR: receiving" : "IR: off";
    dl->AddText(ImGui::GetFont(), 13 * s, ImVec2(x0 + 70 * s, y), (m.IrSending() || m.IrListening()) ? Col(ACCENT) : Col(TEXT3), ir);
    // The speaker doubles as the mute switch (also M on the keyboard).
    bool spk = now < speaker_until;
    const char* spk_text = cfg.mute ? (spk ? "Muted (sound)" : "Muted") : (spk ? "Speaker on" : "Speaker off");
    ImGui::SetCursorScreenPos(ImVec2(x0 + 146 * s, y - 4 * s));
    if (ImGui::InvisibleButton("speaker", ImVec2(96 * s, 22 * s))) ToggleMute();
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip(cfg.mute ? "Click to unmute (M)" : "Click to mute (M)");
    dl->AddText(ImGui::GetFont(), 13 * s, ImVec2(x0 + 150 * s, y), cfg.mute ? Col(ACCENT) : hov ? Col(TEXT) : Col(TEXT3), spk_text);
    if (docked) dl->AddText(ImGui::GetFont(), 13 * s, ImVec2(x0 + 250 * s, y), Col(ACCENT), "Docked");
    ImGui::SetCursorScreenPos(ImVec2(x0 + iw - 70 * s, y - 5 * s));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    if (ImGui::Button("Reset", ImVec2(70 * s, 26 * s)) && have_bios) { m.LoadCard(cardimg); watcher.Reset(); m.Reset(); }
    if (InGame()) {
      ImGui::SetCursorScreenPos(ImVec2(x0 + iw - 170 * s, y - 5 * s));
      if (ImGui::Button("Exit game", ImVec2(96 * s, 26 * s)) && !ExitGame())
        Note("No saved clock to go back to yet: press Reset to leave the game.");
    }
    ImGui::PopStyleColor(2);
    y += 34 * s;

    Lcd(dl, ImVec2(x0, y), iw, s);
    y += iw + 30 * s;

    Controls(dl, ImVec2(x0, y), iw, s, 120 * s, 42 * s);

    // Key hints under the unit.
#ifdef __ANDROID__
    std::string hint = pad ? std::string("Controller: ") + SDL_GameControllerName(pad) : "";
#else
    std::string hint = "Arrows: d-pad      Z / Enter: Fire      ";
    hint += pad ? std::string("Controller: ") + SDL_GameControllerName(pad) : "Controller: plug one in";
#endif
    ImVec2 hs = ImGui::CalcTextSize(hint.c_str());
    dl->AddText(ImVec2(c.x - hs.x / 2, bmax.y + 14), Col(TEXT3), hint.c_str());
  }

  // ------------------------------------------------------------------ phones and tablets --
  // Laid out in dp as SS1 Tool for Android is: a 56 dp app bar, the page, and a 72 dp bottom
  // navigation on phones; at 600 dp wide and up (tablets, phones turned sideways) an 88 dp
  // navigation rail on the left instead. The status and navigation bars stay (styles.xml).
  static constexpr float APPBAR = 56, BOTTOMNAV = 72, RAIL = 88;
  enum { PAGE_PLAY = 0, PAGE_CARDS = 1, PAGE_GAMES = 2, PAGE_LINK = 3 };

  bool Wide() const { return ImGui::GetIO().DisplaySize.x >= 600; }

  // Nav icons, drawn in a 24-unit box like the mockup's SVGs.
  void NavIcon(ImDrawList* dl, int pg, ImVec2 o, float sz, ImU32 c) {
    float k = sz / 24, w = 1.8f * k;
    auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
    switch (pg) {
      case PAGE_PLAY:   // the unit: body, screen, d-pad
        dl->AddRect(P(6, 2.5f), P(18, 21.5f), c, 3 * k, 0, w);
        dl->AddRect(P(8.5f, 5), P(15.5f, 11), c, 1 * k, 0, w);
        dl->AddCircle(P(14.5f, 16.5f), 1.6f * k, c, 0, w);
        dl->AddLine(P(8.5f, 16.5f), P(11.5f, 16.5f), c, w);
        dl->AddLine(P(10, 15), P(10, 18), c, w);
        break;
      case PAGE_GAMES:  // a list
        dl->AddRect(P(3.5f, 4), P(20.5f, 9), c, 1.5f * k, 0, w);
        dl->AddRect(P(3.5f, 11), P(20.5f, 16), c, 1.5f * k, 0, w);
        dl->AddLine(P(3.5f, 19.5f), P(14.5f, 19.5f), c, w);
        break;
      case PAGE_CARDS: {  // a card with its label
        ImVec2 pts[5] = {P(6, 3), P(15, 3), P(19, 7), P(19, 21), P(6, 21)};
        dl->AddPolyline(pts, 5, c, ImDrawFlags_Closed, w);
        dl->AddLine(P(9, 14), P(16, 14), c, w);
        dl->AddLine(P(9, 17.5f), P(16, 17.5f), c, w);
        break;
      }
      case PAGE_LINK: {  // Bluetooth
        ImVec2 pts[6] = {P(7, 7), P(17, 17), P(12, 21), P(12, 3), P(17, 7), P(7, 17)};
        dl->AddPolyline(pts, 6, c, 0, w);
        break;
      }
    }
  }

  void NavButton(int pg, const char* label, ImVec2 a, ImVec2 b) {
    auto* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(pg);
    if (ImGui::InvisibleButton("nav", ImVec2(b.x - a.x, b.y - a.y))) page = pg;
    ImGui::PopID();
    bool on = page == pg;
    float cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
    ImVec2 pa(cx - 28, cy - 22), pb(cx + 28, cy + 8);   // the 56x30 pill around the icon
    if (on) dl->AddRectFilled(pa, pb, Col(0x3a2a12), 15);
    NavIcon(dl, pg, ImVec2(cx - 11, cy - 18), 22, on ? Col(ACCENT) : Col(TEXT3));
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(12, FLT_MAX, 0, label);
    dl->AddText(on ? font_bold : ImGui::GetFont(), 12, ImVec2(cx - ts.x / 2, cy + 12), on ? Col(TEXT) : Col(TEXT3), label);
  }

  void MobileFrame() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float W = vp->Size.x, H = vp->Size.y;
    bool wide = Wide();
    if (page > PAGE_LINK) page = PAGE_PLAY;

    // App bar: the name, how the network card stands, mute.
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + APPBAR), Col(PANEL));
    dl->AddLine(ImVec2(o.x, o.y + APPBAR), ImVec2(o.x + W, o.y + APPBAR), Col(0x000000, 100));
    dl->AddText(font_bold, 17, ImVec2(o.x + 16, o.y + 9), Col(ACCENT), "POCKETSTATION");
    std::string chip;
    unsigned dot;
    if (!server.Listening()) { chip = "Network card off"; dot = TEXT4; }
    else if (docked) { chip = "Docked · " + server.Peer(); dot = ACCENT; }
    else if (server.Connected()) { chip = "SS1 connected · " + server.Peer(); dot = GOOD; }
    else { chip = "Network card · port " + std::to_string(server.port); dot = TEXT4; }
    dl->AddCircleFilled(ImVec2(o.x + 19, o.y + 38), 3.5f, Col(dot));
    dl->AddText(ImGui::GetFont(), 12, ImVec2(o.x + 28, o.y + 31), Col(TEXT3), chip.c_str());
    {
      // Mute: a speaker, struck through when muted.
      ImVec2 c(o.x + W - 32, o.y + APPBAR / 2);
      ImGui::SetCursorScreenPos(ImVec2(c.x - 24, c.y - 24));
      if (ImGui::InvisibleButton("mute", ImVec2(48, 48))) ToggleMute();
      ImU32 col = cfg.mute ? Col(ACCENT) : (now < speaker_until ? Col(TEXT) : Col(TEXT3));
      ImVec2 sp[6] = {ImVec2(c.x - 9, c.y - 3), ImVec2(c.x - 5, c.y - 3), ImVec2(c.x, c.y - 8), ImVec2(c.x, c.y + 8), ImVec2(c.x - 5, c.y + 3), ImVec2(c.x - 9, c.y + 3)};
      dl->AddPolyline(sp, 6, col, ImDrawFlags_Closed, 1.8f);
      if (cfg.mute) {
        dl->AddLine(ImVec2(c.x + 4, c.y - 4), ImVec2(c.x + 10, c.y + 4), col, 1.8f);
        dl->AddLine(ImVec2(c.x + 10, c.y - 4), ImVec2(c.x + 4, c.y + 4), col, 1.8f);
      } else {
        dl->PathArcTo(c, 6, -0.9f, 0.9f, 8);
        dl->PathStroke(col, 0, 1.8f);
        dl->PathArcTo(c, 10, -0.9f, 0.9f, 10);
        dl->PathStroke(col, 0, 1.8f);
      }
    }

    // Navigation: bottom bar on phones, rail on wide screens.
    struct Tab { int pg; const char* label; } tabs[4] = {{PAGE_PLAY, "Play"}, {PAGE_GAMES, "Games"}, {PAGE_CARDS, "Cards"}, {PAGE_LINK, "Link"}};
    ImVec2 ca, cb;   // the page's area
    if (wide) {
      dl->AddRectFilled(ImVec2(o.x, o.y + APPBAR), ImVec2(o.x + RAIL, o.y + H), Col(PANEL));
      dl->AddLine(ImVec2(o.x + RAIL, o.y + APPBAR), ImVec2(o.x + RAIL, o.y + H), Col(0x000000, 100));
      for (int i = 0; i < 4; i++) {
        float y = o.y + APPBAR + 12 + i * 72;
        NavButton(tabs[i].pg, tabs[i].label, ImVec2(o.x, y), ImVec2(o.x + RAIL, y + 64));
      }
      ca = ImVec2(RAIL, APPBAR);
      cb = ImVec2(W, H);
    } else {
      dl->AddRectFilled(ImVec2(o.x, o.y + H - BOTTOMNAV), ImVec2(o.x + W, o.y + H), Col(PANEL));
      dl->AddLine(ImVec2(o.x, o.y + H - BOTTOMNAV), ImVec2(o.x + W, o.y + H - BOTTOMNAV), Col(0x000000, 100));
      float tw = (W - 8) / 4;
      for (int i = 0; i < 4; i++)
        NavButton(tabs[i].pg, tabs[i].label, ImVec2(o.x + 4 + i * tw, o.y + H - BOTTOMNAV), ImVec2(o.x + 4 + (i + 1) * tw, o.y + H));
      ca = ImVec2(0, APPBAR);
      cb = ImVec2(W, H - BOTTOMNAV);
    }

    ImGui::SetCursorPos(ImVec2(ca.x, ca.y + 1));
    ImVec2 cs(cb.x - ca.x, cb.y - ca.y - 1);
    ImGui::BeginChild("page", cs, ImGuiChildFlags_None, page == PAGE_PLAY ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
    float pad = wide ? 24 : 12, cw = std::min(cs.x - 2 * pad, 960.0f), cx = (cs.x - cw) / 2;
    switch (page) {
      case PAGE_PLAY: MobilePlay(cs, wide); break;
      case PAGE_GAMES:
        ImGui::SetCursorPos(ImVec2(cx, 12));
        GameList(cw);
        ImGui::Dummy(ImVec2(0, 24));
        DragScroll();
        break;
      case PAGE_CARDS:
        ImGui::SetCursorPos(ImVec2(cx, 12));
        CardMain(cw, wide ? 5 : 3, true);
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::SetCursorPosX(cx);
        CardSide(cw);
        ImGui::Dummy(ImVec2(0, 24));
        DragScroll();
        break;
      case PAGE_LINK:
        ImGui::SetCursorPos(ImVec2(cx, 12));
        MobileLink(cw, chip);
        DragScroll();
        break;
    }
    ImGui::EndChild();

    if (now < note_until && !note.empty()) {
      // As SS1 Tool's toast: above the bottom bar, the width of the screen less 12 dp a side.
      auto* fg = ImGui::GetForegroundDrawList();
      float l = o.x + (wide ? RAIL + 16 : 12), r = wide ? std::min(o.x + W - 16, l + 560) : o.x + W - 12;
      float bottom = o.y + H - (wide ? 16 : BOTTOMNAV + 12);
      ImGui::PushTextWrapPos(0);
      ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(14, FLT_MAX, r - l - 32, note.c_str());
      ImGui::PopTextWrapPos();
      ImVec2 a(l, bottom - ts.y - 24);
      fg->AddRectFilled(a, ImVec2(r, bottom), Col(CARD), 8);
      fg->AddRect(a, ImVec2(r, bottom), Col(ACCENT), 8);
      fg->AddText(ImGui::GetFont(), 14, ImVec2(a.x + 16, a.y + 12), Col(TEXT), note.c_str(), nullptr, r - l - 32);
    }
    ImGui::End();
  }

  // Play: no device body, just its status, the screen and the controls, as large as fit.
  void MobilePlay(ImVec2 cs, bool wide) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    const float pad = 16, status = 40;
    float lcd, x0, y0;
    if (!wide) {
      // Upright: status, screen, controls, centred down the page.
      float ctl = 170;
      lcd = std::max(120.0f, std::min(cs.x - 2 * pad, cs.y - status - ctl - 2 * pad));
      float block = status + lcd + ctl;
      x0 = o.x + (cs.x - lcd) / 2;
      y0 = o.y + std::max(pad, (cs.y - block) / 2);
      MobileStatus(dl, ImVec2(x0, y0), lcd);
      Lcd(dl, ImVec2(x0, y0 + status), lcd, lcd / 440);
      Controls(dl, ImVec2(x0 - 8, y0 + status + lcd + 24), lcd + 16, 1, 150, 46);
    } else {
      // Sideways or a tablet: the screen on the left, the controls beside it.
      lcd = std::max(120.0f, std::min(cs.y - status - 2 * pad, cs.x * 0.6f));
      float right = cs.x - lcd - 3 * pad;
      float total = lcd + pad + std::min(right, 380.0f);
      x0 = o.x + std::max(pad, (cs.x - total) / 2);
      y0 = o.y + std::max(pad, (cs.y - status - lcd) / 2);
      MobileStatus(dl, ImVec2(x0, y0), lcd);
      Lcd(dl, ImVec2(x0, y0 + status), lcd, lcd / 440);
      float rw = std::min(right, 380.0f);
      float dp = std::min(150.0f, rw * 0.45f);
      Controls(dl, ImVec2(x0 + lcd + pad, y0 + status + lcd / 2 - dp / 2 - 24 + 24), rw, 1, dp, std::min(46.0f, rw * 0.13f));
    }
  }

  // LED, IR, docked; Exit game and Reset on the right.
  void MobileStatus(ImDrawList* dl, ImVec2 a, float w) {
    float y = a.y + 8;
    bool led = m.LedOn();
    dl->AddCircleFilled(ImVec2(a.x + 6, y + 8), 5, led ? Col(LED_ON) : Col(0x3a2224));
    dl->AddText(ImGui::GetFont(), 13, ImVec2(a.x + 16, y + 1), Col(TEXT3), "LED");
    const char* ir = m.IrSending() ? "IR sending" : m.IrListening() ? "IR receiving" : "IR off";
    dl->AddText(ImGui::GetFont(), 13, ImVec2(a.x + 56, y + 1), (m.IrSending() || m.IrListening()) ? Col(ACCENT) : Col(TEXT3), ir);
    if (docked) dl->AddText(ImGui::GetFont(), 13, ImVec2(a.x + 146, y + 1), Col(ACCENT), "Docked");
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 4));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    float bx = a.x + w - 64;
    ImGui::SetCursorScreenPos(ImVec2(bx, a.y + 2));
    if (ImGui::Button("Reset", ImVec2(64, 30)) && have_bios) { m.LoadCard(cardimg); watcher.Reset(); m.Reset(); }
    if (InGame()) {
      ImGui::SetCursorScreenPos(ImVec2(bx - 98, a.y + 2));
      if (ImGui::Button("Exit game", ImVec2(90, 30)) && !ExitGame())
        Note("No saved clock to go back to yet: press Reset to leave the game.");
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
  }

  // Link: the network card, and two units talking by infrared.
  void MobileLink(float w, const std::string& chip) {
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextUnformatted("NETWORK CARD");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    ListCard(chip.c_str(), "A console running Phosphor (SS1) uses this phone as its memory card.",
             ("Port " + std::to_string(server.port) + " · the unit docks while the console talks to it").c_str(), server.Connected(), w);
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextUnformatted("INFRARED");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    const char* ir = m.IrSending() ? "Sending" : m.IrListening() ? "Listening" : "Off";
    ListCard((std::string("Infrared: ") + ir).c_str(), "Games that swap data between two units (MGS IDs and others).",
             "Linking two phones over Bluetooth is coming in a later version.", false, w);
    ImGui::EndGroup();
  }

  void CardsPage() {
    float H = ImGui::GetWindowHeight() - 62, W = ImGui::GetWindowWidth();
    if (Portrait()) {
      ImGui::SetCursorPos(ImVec2(0, 62));
      ImGui::BeginChild("cards", ImVec2(W, H));
      ImGui::SetCursorPos(ImVec2(20, 16));
      CardMain(W - 40, 3, true);
      ImGui::Dummy(ImVec2(0, 12));
      ImGui::SetCursorPosX(20);
      CardSide(W - 40);
      ImGui::Dummy(ImVec2(0, 30));
      DragScroll();
      ImGui::EndChild();
      return;
    }
    ImGui::SetCursorPos(ImVec2(0, 62));
    ImGui::BeginChild("cardside", ImVec2(320, H));
    ImGui::SetCursorPos(ImVec2(20, 20));
    CardSide(280);
    ImGui::EndChild();
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + 320, wp.y + 62), ImVec2(wp.x + 320, wp.y + 62 + H), Col(LINE));
    ImGui::SetCursorPos(ImVec2(321, 62));
    ImGui::BeginChild("cardmain", ImVec2(W - 321, H));
    ImGui::SetCursorPos(ImVec2(32, 24));
    CardMain(ImGui::GetContentRegionAvail().x - 32, 5, false);
    ImGui::EndChild();
  }

  void CardSide(float sw) {
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextUnformatted(ON_THIS_DEVICE_CAPS);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    char sub[96];
    snprintf(sub, sizeof sub, "%d of 15 blocks used", 15 - card::FreeBlocks(cardimg));
    ListCard(fs::path(CardPath()).filename().string().c_str(), sub, nullptr, true, sw);
    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Load a card...", ImVec2(sw, 34))) {
      std::string f = OpenFileDialog("Memory cards and saves", "*.mcd;*.mcr;*.mc;*.srm;*.gme;*.mcs;*.psv;*.psx;*.ps1");
      if (!f.empty()) Open(f);
    }
    if (ImGui::Button("Add a save or mini-game...", ImVec2(sw, 34))) {
      std::string f = OpenFileDialog("Single saves", "*.mcs;*.psv;*.psx;*.ps1");
      if (!f.empty()) Open(f);
    }
    if (ImGui::Button("Save the card as...", ImVec2(sw, 34)))
      Export("Memory card (raw)", "*.mcd", "mcd", "PocketStation.mcd", cardimg);
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextUnformatted("NETWORK CARD");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    std::string ns = server.Listening() ? (server.Connected() ? "Connected: " + server.Peer() : "Waiting on port " + std::to_string(server.port)) : "Off";
    snprintf(sub, sizeof sub, "%d commands answered", server.commands);
    ListCard(ns.c_str(), sub, nullptr, false, sw);
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextUnformatted("BIOS");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    ListCard(have_bios ? fs::path(cfg.bios).filename().string().c_str() : "None yet", have_bios ? "Your own dump" : "Drop a 16 KB BIOS file", nullptr, false, sw);
    if (ImGui::Button("Choose BIOS...", ImVec2(sw, 34))) {
      std::string f = OpenFileDialog("PocketStation BIOS (16 KB)", "*.bin;*.rom");
      if (!f.empty()) Open(f);
    }
    ImGui::EndGroup();
  }

  // The card: name, blocks (`cols` to a row), the selected file. `tall` stacks the header.
  void CardMain(float gw, int cols, bool tall) {
    ImGui::BeginGroup();
    ImGui::PushFont(font_big);
    if (tall) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + gw);   // phones: wrap, never off the edge
    ImGui::TextWrapped("%s", fs::path(CardPath()).filename().string().c_str());
    ImGui::PopFont();
    if (!tall) ImGui::SameLine(0, 14);
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT3));
    ImGui::TextWrapped(ON_THIS_DEVICE " · %d of 15 blocks used · unit serial %08X", 15 - card::FreeBlocks(cardimg), Serial());
    ImGui::PopStyleColor();
    if (tall) ImGui::PopTextWrapPos();
    // Copy protection: some apps (MGS Integral's) only run on the PocketStation the game
    // installed them on. With this on, such an app is moved over to this unit when it arrives.
    if (!tall) ImGui::SameLine(0, 18);
    if (ImGui::Checkbox("Unlock apps from other PocketStations", &cfg.unlock)) {
      cfg.Save();
      if (cfg.unlock) Bind();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Some games lock their PocketStation app to one unit's serial number;\n"
                        "a copy from another unit marks itself a copy and wipes its data.\n"
                        "On: such apps are moved to this unit when added. Off: the real check.");
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
    float cw = (gw - 10 * (cols - 1)) / cols;
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
        dl->AddText(font_bold, 14, ImVec2(p.x + 12, p.y + 12), Col(TEXT), alerts::DisplayName(e).c_str());
        char s2[64];
        snprintf(s2, sizeof s2, "%s · block %d of %d", e.pocketstation ? "PocketStation app" : "save", b - e.first_block + 1, e.blocks);
        dl->AddText(ImGui::GetFont(), 12, ImVec2(p.x + 12, p.y + 34), Col(TEXT3), s2);
        dl->AddText(ImGui::GetFont(), 12, ImVec2(p.x + 12, p.y + 52), Col(TEXT4), e.product.c_str());
        dl->PopClipRect();
      }
      if (b % cols != 0 && b != 15) ImGui::SameLine(0, 10);
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
      ImGui::TextUnformatted(alerts::DisplayName(*se).c_str());
      ImGui::PopFont();
      ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT2));
      ImGui::Text("%s · %s · %d block%s", se->product.c_str(), se->pocketstation ? (se->mcx1 ? "PocketStation app, resumable" : "PocketStation app") : "save",
                  se->blocks, se->blocks == 1 ? "" : "s");
      ImGui::PopStyleColor();
      ImGui::Dummy(ImVec2(0, 6));
      ImGui::PushStyleColor(ImGuiCol_Button, ColV(LINE));
      if (ImGui::Button("Save as .mcs...", ImVec2(150, 34)))
        Export("Single save", "*.mcs", "mcs", se->product + ".mcs", card::ExportSave(cardimg, se->first_block));
      ImGui::PopStyleColor();
      ImGui::EndGroup();
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 130));
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushStyleColor(ImGuiCol_Text, ColV(TEXT4));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + gw);
    ImGui::TextUnformatted("The card saves itself as you play. Loading another card keeps the old one as .bak.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
  }

  void Frame() {
#if defined(__ANDROID__) || defined(MCPS_PHONE_LAYOUT)
    MobileFrame();
    return;
#endif
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
  std::vector<std::string> drops;
  bool keep_state = false;
  struct ScriptPress { int frame, mask, frames; };
  std::vector<ScriptPress> presses;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--screenshot" && i + 1 < argc) shot = argv[++i];
    else if (a == "--frames" && i + 1 < argc) shot_frames = atoi(argv[++i]);
    else if (a == "--page" && i + 1 < argc) shot_page = atoi(argv[++i]);
    else if (a == "--bios" && i + 1 < argc) bios_arg = argv[++i];
    else if (a == "--card" && i + 1 < argc) card_arg = argv[++i];
    else if (a == "--keep-state") keep_state = true;
    else if (a == "--drop" && i + 1 < argc) drops.push_back(argv[++i]);
    else if (a == "--press" && i + 1 < argc) {   // FRAME:MASK:FRAMES, for headless checks
      int f = 0, mk = 0, n = 0;
      if (sscanf(argv[++i], "%d:%d:%d", &f, &mk, &n) == 3) presses.push_back({f, mk, n});
    }
  }

#ifdef __ANDROID__
  // Keep running when put away: a PocketStation's games carry on in your pocket, and they call
  // you (alerts.hpp). MCPSActivity keeps a foreground service up meanwhile so Android lets us.
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
#endif
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __ANDROID__
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait PortraitUpsideDown LandscapeLeft LandscapeRight");
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
#endif
  std::string title = std::string(APP_NAME) + " " + APP_VERSION + " — PocketStation";
#ifdef __ANDROID__
  // Not fullscreen: the status and navigation bars stay, as in SS1 Tool.
  SDL_Window* win = SDL_CreateWindow(title.c_str(), 0, 0, 0, 0, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#elif defined(MCPS_PHONE_LAYOUT)   // desktop check of the phone layout: a 20:9 phone held upright
  int test_w = 1080, test_h = 2400;   // MCPS_WINDOW=WxH for other phones, or sideways
  if (getenv("MCPS_WINDOW")) sscanf(getenv("MCPS_WINDOW"), "%dx%d", &test_w, &test_h);
  SDL_Window* win = SDL_CreateWindow(title.c_str(), 0, 0, test_w, test_h, 0);
#else
  SDL_Window* win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 820,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#endif
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImFontConfig fc;
  fc.FontDataOwnedByAtlas = false;
  // Phones: the app is laid out in dp and the renderer scales it up to pixels, so the fonts are
  // drawn at the screen's density (sharp at any size) and scaled back to dp; ImGui's own text
  // follows Android's font size within 85-135%, as SS1 Tool's pages do.
  float density = 1, font_scale = 1;
#if defined(__ANDROID__)
  density = AndroidFloat("displayDensity", 2.0f);
  font_scale = std::clamp(AndroidFloat("fontScale", 1.0f), 0.85f, 1.35f);
#elif defined(MCPS_PHONE_LAYOUT)
  density = getenv("MCPS_DENSITY") ? (float)atof(getenv("MCPS_DENSITY")) : 2.625f;
  font_scale = getenv("MCPS_FONT_SCALE") ? std::clamp((float)atof(getenv("MCPS_FONT_SCALE")), 0.85f, 1.35f) : 1.0f;
#endif
  float fk = std::max(1.0f, density);
  io.FontGlobalScale = font_scale / fk;
  io.Fonts->AddFontFromMemoryCompressedTTF(inter_regular_compressed_data, inter_regular_compressed_size, 15 * fk, &fc);
  App app;
  app.window = win;
  app.font_bold = io.Fonts->AddFontFromMemoryCompressedTTF(inter_semibold_compressed_data, inter_semibold_compressed_size, 16 * fk, &fc);
  app.font_big = io.Fonts->AddFontFromMemoryCompressedTTF(inter_semibold_compressed_data, inter_semibold_compressed_size, 26 * fk, &fc);
  Style();
  ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
  ImGui_ImplSDLRenderer2_Init(ren);

#ifdef __ANDROID__
  // The app's own folder on shared storage (Android/data/com.f3bandit.mcps/files): settings, the
  // card and the saved unit live here, and a BIOS copied here by USB is found like beside the exe.
  const char* ext_dir = SDL_AndroidGetExternalStoragePath();
  app.dir = std::string(ext_dir ? ext_dir : SDL_AndroidGetInternalStoragePath()) + "/";
  fs::create_directories(app.dir);
#else
  char* base = SDL_GetBasePath();
  app.dir = base ? base : "./";
  SDL_free(base);
#endif
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
  for (auto& d : drops) app.Open(d);

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
      // Phones and tablets: the app can be put away or closed at any moment. Save the unit and
      // the card on the way out, and on return move the clock on by the time away (the emulation
      // doesn't run in the background, a real unit's clock does).
      if (e.type == SDL_APP_WILLENTERBACKGROUND || e.type == SDL_APP_TERMINATING) {
        if (app.card_save_at >= 0) { card::Save(app.CardPath(), app.cardimg); app.card_save_at = -1; }
        app.SaveState();
        app.background = true;
        if (app.audio) SDL_ClearQueuedAudio(app.audio);
      }
      if (e.type == SDL_APP_DIDENTERFOREGROUND) {
        app.background = false;
        last = std::chrono::steady_clock::now();
      }
      if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_AC_BACK) app.page = 0;
      if (e.type == SDL_CONTROLLERDEVICEADDED && !app.pad) app.pad = SDL_GameControllerOpen(e.cdevice.which);
      if (e.type == SDL_CONTROLLERDEVICEREMOVED && app.pad &&
          SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(app.pad)) == e.cdevice.which) {
        SDL_GameControllerClose(app.pad);
        app.pad = nullptr;
      }
      if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F2 && app.have_bios) { app.m.LoadCard(app.cardimg); app.m.Reset(); }
      if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_M && !ImGui::GetIO().WantTextInput) app.ToggleMute();
      if (((e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F3) ||
           (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_START)) && app.have_bios && app.InGame())
        if (!app.ExitGame()) app.Note("No saved clock to go back to yet: press Reset to leave the game.");
      if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK && app.have_bios) { app.m.LoadCard(app.cardimg); app.m.Reset(); }
      if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) app.page = 0;
      if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) app.page = 1;
    }
#ifdef __ANDROID__
    {
      // Wall time, counting time asleep: the emulation owes what the phone slept through.
      struct timespec ts;
      clock_gettime(CLOCK_BOOTTIME, &ts);
      double wall = ts.tv_sec + ts.tv_nsec * 1e-9;
      double mono = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
      static double last_wall = wall, last_mono = mono;
      static double owed = 0;
      double gap = wall - last_wall, awake = mono - last_mono;
      last_wall = wall;
      last_mono = mono;
      if (app.background) owed += gap;
      else owed += std::max(0.0, gap - awake - 0.05);   // asleep in front (screen off): the steady clock missed it
      if (owed > 12 * 3600.0) { app.AdvanceClock((long long)(owed - 1)); owed = 1; }   // too far to run through
      if (owed > 0) {
        // Run the owed time fast, a slice at a time, so the alerts it raises still come out.
        static const u8 no_keys[SDL_NUM_SCANCODES] = {};
        auto t0 = std::chrono::steady_clock::now();
        while (owed > 0 && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(app.background ? 40 : 8)) {
          double d = std::min(owed, 0.5);
          app.Step(d, no_keys, d);
          owed -= d;
        }
      }
      if (app.background) {
        SDL_Delay(owed > 0 ? 1 : 200);
        last = std::chrono::steady_clock::now();
        continue;   // nothing to draw
      }
      // In front, gaps above are owed time; the steady clock below covers the rest.
    }
#endif
    auto t = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(t - last).count();
    last = t;
    if (!shot.empty()) {
      dt = 1.0 / 60;   // screenshots run on a fixed clock
      app.mouse_buttons = 0;
      for (auto& p : presses) if (frame >= p.frame && frame < p.frame + p.frames) app.mouse_buttons |= (u8)p.mask;
    }
    app.Step(dt, SDL_GetKeyboardState(nullptr));

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
#if defined(__ANDROID__) || defined(MCPS_PHONE_LAYOUT)
    {
      // In dp: the screen's pixels over its density, as Android lays apps out.
      int ww, wh;
      SDL_GetRendererOutputSize(ren, &ww, &wh);
      float lw = ww / density, lh = wh / density;
      SDL_RenderSetLogicalSize(ren, (int)lw, (int)lh);
      io.DisplaySize = ImVec2(lw, lh);
      io.DisplayFramebufferScale = ImVec2(1, 1);
    }
#endif
#ifdef __ANDROID__
    {
      std::vector<std::string> files, notes;
      {
        std::lock_guard<std::mutex> lk(g_android_mu);
        files.swap(g_android_files);
        notes.swap(g_android_notes);
      }
      for (auto& f : files) app.Open(f);
      for (auto& n : notes) app.Note(n);
    }
#endif
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
  if (shot.empty() || keep_state) app.SaveState();
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
