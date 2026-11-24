// oranges' setup: the program a player starts (oranges on Linux, oranges.exe on Windows, at the top of a release). It
// builds their own port from their own copy of the game, without a terminal: choose the game, the folder, the
// options, Build, Play (TODO.md task 2).
//
// It runs the pipeline as a child process, the release's own Python on `python -m oranges make --progress json`
// (oranges/make.py has the events), and shows its stages, progress and log; closing the child's stdin cancels it.
// `python -m oranges check` says whether a package is the supported build. Nothing here reads the game itself.
//
// Where things are: the release's folder holds this program, python/ and src/ (oranges' tree: oranges/, host/,
// third_party/); in a checkout (a developer's build, python -m oranges.gui) the tree is the checkout and the Python its
// .venv. ORANGES_PYTHON names another Python. Its own settings are setup.ini in the user folder (as the game's saves:
// ~/.local/share/oranges, %APPDATA%\oranges), the game's options (Console mode) go to oranges_options.ini, the file the
// game's F1 menu writes (host/hos_options.c, linked in).
//
//   oranges [--nup FILE] [--dir FOLDER] [--page game|folder|options|build|play] [--build]
//           [--shot FILE.bmp] [--shot-after SECONDS] [--quit-after-shot] [--quit-after-build]
//
// (--shot and the --quit options are for tests: the window's picture as a BMP.)
// Drawn with Dear ImGui (third_party/imgui: its SDL3 and SDL_Renderer backends) in the colours of the game's F1 menu
// (host/vk/vk_menu.cpp, Longshot's Modern dark).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

extern "C" {
#include "hos_options.h"
void hos_log(const char *fmt, ...) {   // (hos_options.c's log)
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}
}

namespace fs = std::filesystem;
namespace {

// ---- small things ----

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string gb(double bytes) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f GB", bytes / 1e9);
    return b;
}

std::string duration(double s) {
    char b[32];
    if (s < 60) std::snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 3600) std::snprintf(b, sizeof b, "%d min %02d s", (int)s / 60, (int)s % 60);
    else std::snprintf(b, sizeof b, "%d h %02d min", (int)s / 3600, (int)s / 60 % 60);
    return b;
}

double now_s() { return SDL_GetTicksNS() / 1e9; }

// the user folder (the game's: hos_main.c data_dir)
fs::path user_dir() {
    if (const char* own = std::getenv("ORANGES_USER"); own && *own) return from_u8(own);
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    return from_u8(appdata && *appdata ? appdata : ".") / "oranges";
#else
    const char *xdg = std::getenv("XDG_DATA_HOME"), *home = std::getenv("HOME");
    if (xdg && xdg[0] == '/') return fs::path(xdg) / "oranges";
    return fs::path(home && *home ? home : ".") / ".local" / "share" / "oranges";
#endif
}

fs::path home_dir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return from_u8(h && *h ? h : ".");
}

uint64_t free_space(fs::path p) {
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec)) {
        if (p == p.parent_path()) break;
        p = p.parent_path();
    }
    const auto s = fs::space(p, ec);
    return ec ? 0 : s.available;
}

// ---- JSON, just what make's events need: objects of strings, numbers, booleans and arrays ----

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
    const Json& operator[](const char* k) const {
        static const Json none;
        auto it = obj.find(k);
        return it == obj.end() ? none : it->second;
    }
    std::string s() const { return kind == Str ? str : ""; }
    double n() const { return kind == Num ? num : 0; }
};

struct JsonParser {
    const char* p;
    const char* end;
    bool ok = true;
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool eat(char c) {
        ws();
        if (p < end && *p == c) {
            p++;
            return true;
        }
        return false;
    }
    static void put_utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) o += (char)(0xC0 | c >> 6), o += (char)(0x80 | (c & 63));
        else if (c < 0x10000) o += (char)(0xE0 | c >> 12), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
        else o += (char)(0xF0 | c >> 18), o += (char)(0x80 | (c >> 12 & 63)), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
    }
    std::string string() {
        std::string o;
        p++;   // (the opening quote)
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                switch (*p) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    if (end - p < 5) return ok = false, o;
                    uint32_t c = (uint32_t)std::strtoul(std::string(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (c >= 0xD800 && c < 0xDC00 && end - p >= 7 && p[1] == '\\' && p[2] == 'u') {
                        const uint32_t lo = (uint32_t)std::strtoul(std::string(p + 3, 4).c_str(), nullptr, 16);
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                    put_utf8(o, c);
                    break;
                }
                default: o += *p;
                }
                p++;
            } else {
                o += *p++;
            }
        }
        if (p >= end) ok = false;
        p++;
        return o;
    }
    Json value() {
        Json v;
        ws();
        if (p >= end) return ok = false, v;
        if (*p == '{') {
            v.kind = Json::Obj;
            p++;
            if (eat('}')) return v;
            do {
                ws();
                if (p >= end || *p != '"') return ok = false, v;
                std::string k = string();
                if (!eat(':')) return ok = false, v;
                v.obj[k] = value();
            } while (ok && eat(','));
            if (!eat('}')) ok = false;
        } else if (*p == '[') {
            v.kind = Json::Arr;
            p++;
            if (eat(']')) return v;
            do v.arr.push_back(value());
            while (ok && eat(','));
            if (!eat(']')) ok = false;
        } else if (*p == '"') {
            v.kind = Json::Str;
            v.str = string();
        } else if (!std::strncmp(p, "true", 4) || !std::strncmp(p, "false", 5)) {
            v.kind = Json::Bool;
            v.num = *p == 't';
            p += *p == 't' ? 4 : 5;
        } else if (!std::strncmp(p, "null", 4)) {
            p += 4;
        } else {
            char* e;
            v.kind = Json::Num;
            v.num = std::strtod(std::string(p, std::min<size_t>(end - p, 40)).c_str(), &e);
            const size_t n = e - std::string(p, std::min<size_t>(end - p, 40)).c_str();
            if (!n) return ok = false, v;
            p += n;
        }
        return v;
    }
};

bool parse_json(const std::string& text, Json& out) {
    JsonParser jp{text.data(), text.data() + text.size()};
    out = jp.value();
    return jp.ok && out.kind == Json::Obj;
}

// ---- where the release's parts are ----

struct Paths {
    fs::path root;     // (the release's folder, or the checkout)
    fs::path src;      // (oranges' tree: oranges/, host/, third_party/)
    fs::path python;
    std::string version = "dev";
    bool release = false;
} paths;

bool find_paths() {
    const char* base = SDL_GetBasePath();
    fs::path d = from_u8(base ? base : ".");
    for (int up = 0; up < 5 && !d.empty(); up++, d = d.parent_path()) {
        std::error_code ec;
        if (fs::exists(d / "src" / "oranges" / "__init__.py", ec)) {
            paths.root = d, paths.src = d / "src", paths.release = true;
            break;
        }
        if (fs::exists(d / "oranges" / "__init__.py", ec)) {
            paths.root = d, paths.src = d;
            break;
        }
        if (d == d.parent_path()) break;
    }
    if (paths.src.empty()) return false;
    if (const char* py = std::getenv("ORANGES_PYTHON"); py && *py) {
        paths.python = from_u8(py);
    } else {
#ifdef _WIN32
        // (pythonw: no console window opens for it; its stdout is our pipe)
        paths.python = paths.release ? paths.root / "python" / "pythonw.exe" : paths.src / ".venv" / "Scripts" / "pythonw.exe";
#else
        paths.python = paths.release ? paths.root / "python" / "bin" / "python3" : paths.src / ".venv" / "bin" / "python";
#endif
    }
    std::ifstream v(paths.src / "VERSION");
    if (v && std::getline(v, paths.version) && paths.version.empty()) paths.version = "dev";
    return true;
}

// ---- the setup's own settings (setup.ini in the user folder) ----

struct Settings {
    std::string nup, dir;
    int jobs = 0, handheld = 0;
    bool clean = false;
} settings;

fs::path settings_path() { return user_dir() / "setup.ini"; }

void load_settings() {
    std::ifstream f(settings_path());
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "nup") settings.nup = v;
        else if (k == "dir") settings.dir = v;
        else if (k == "jobs") settings.jobs = std::atoi(v.c_str());
        else if (k == "clean") settings.clean = v == "1";
    }
}

void save_settings() {
    std::error_code ec;
    fs::create_directories(user_dir(), ec);
    const fs::path tmp = settings_path().string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "nup=" << settings.nup << "\ndir=" << settings.dir << "\njobs=" << settings.jobs
          << "\nclean=" << (settings.clean ? 1 : 0) << "\n";
    }
    fs::rename(tmp, settings_path(), ec);
}

// ---- a child process: its output as lines, read on a thread ----

struct Child {
    SDL_Process* proc = nullptr;
    std::thread reader;
    std::mutex m;
    std::deque<std::string> lines;
    std::atomic<bool> finished{false};
    int exit_code = -1;
    bool stdin_closed = false;
    double cancel_at = 0;

    bool start(const std::vector<std::string>& args, bool keep_stdin) {
        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                              keep_stdin ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        SDL_Environment* env = SDL_CreateEnvironment(true);
        SDL_SetEnvironmentVariable(env, "PYTHONUNBUFFERED", "1", true);
        SDL_SetEnvironmentVariable(env, "PYTHONUTF8", "1", true);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
        proc = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        SDL_DestroyEnvironment(env);
        if (!proc) return false;
        stdin_closed = !keep_stdin;
        reader = std::thread([this] {
            SDL_IOStream* out = SDL_GetProcessOutput(proc);
            std::string pending;
            char buf[8192];
            for (;;) {
                const size_t n = out ? SDL_ReadIO(out, buf, sizeof buf) : 0;
                if (n) {
                    pending.append(buf, n);
                    size_t nl;
                    while ((nl = pending.find('\n')) != std::string::npos) {
                        std::string line = pending.substr(0, nl);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        pending.erase(0, nl + 1);
                        std::lock_guard<std::mutex> g(m);
                        lines.push_back(std::move(line));
                    }
                    continue;
                }
                if (out && SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) {
                    SDL_Delay(20);
                    continue;
                }
                break;
            }
            if (!pending.empty()) {
                std::lock_guard<std::mutex> g(m);
                lines.push_back(pending);
            }
            int code = -1;
            SDL_WaitProcess(proc, true, &code);
            exit_code = code;
            finished = true;
        });
        return true;
    }
    bool running() const { return proc && !finished; }
    std::vector<std::string> take() {
        std::lock_guard<std::mutex> g(m);
        std::vector<std::string> out(lines.begin(), lines.end());
        lines.clear();
        return out;
    }
    void cancel() {   // (make's way: stdin ends; killed if it hasn't stopped 20 s later)
        if (!proc || finished) return;
        if (!stdin_closed) {
            SDL_CloseIO(SDL_GetProcessInput(proc));
            stdin_closed = true;
            cancel_at = now_s();
        }
    }
    void poll() {
        if (cancel_at && !finished && now_s() - cancel_at > 20) {
            SDL_KillProcess(proc, true);
            cancel_at = 0;
        }
    }
    void reset() {
        if (proc && !finished) {   // (make's own cancel first: it kills its compiles; killed only if it hangs)
            cancel();
            for (int i = 0; i < 300 && !finished; i++) SDL_Delay(50);
            if (!finished) SDL_KillProcess(proc, true);
        }
        if (reader.joinable()) reader.join();
        if (proc) SDL_DestroyProcess(proc);
        proc = nullptr;
        finished = false;
        exit_code = -1;
        cancel_at = 0;
        lines.clear();
    }
    ~Child() { reset(); }
};

std::vector<std::string> python_args(std::initializer_list<std::string> rest) {
    std::vector<std::string> a = {u8(paths.python), "-m", "oranges"};
    a.insert(a.end(), rest);
    return a;
}

// ---- the game: .nup files found in the usual folders, and the check of the one chosen ----

struct Found {
    std::string path;
    uint64_t size;
};

struct Scan {
    std::thread t;
    std::mutex m;
    std::vector<Found> found;
    std::atomic<bool> done{false};
    void start() {
        t = std::thread([this] {
            const fs::path h = home_dir();
            std::vector<fs::path> roots = {h / "Downloads", h / "Desktop", h / "Documents", h / "Games", h};
#ifndef _WIN32
            if (const char* xdg = std::getenv("XDG_DOWNLOAD_DIR")) roots.insert(roots.begin(), xdg);
#endif
            std::vector<std::string> seen;
            for (const auto& r : roots) {
                std::error_code ec;
                const int max_depth = r == h ? 0 : 3;
                fs::recursive_directory_iterator it(r, fs::directory_options::skip_permission_denied, ec), e;
                for (; !ec && it != e; it.increment(ec)) {
                    const auto& p = it->path();
                    const std::string name = u8(p.filename());
                    if (it->is_directory(ec)) {
                        if ((!name.empty() && name[0] == '.') || it.depth() >= max_depth || name == "node_modules" ||
                            name == "AppData")
                            it.disable_recursion_pending();
                        continue;
                    }
                    if (p.extension() != ".nup" && p.extension() != ".NUP") continue;
                    const std::string s = u8(fs::weakly_canonical(p, ec));
                    if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
                    seen.push_back(s);
                    std::lock_guard<std::mutex> g(m);
                    found.push_back({s, (uint64_t)it->file_size(ec)});
                }
            }
            done = true;
        });
    }
    ~Scan() {
        if (t.joinable()) t.join();
    }
} scan;

struct Check {
    Child child;
    std::string nup, text;
    enum { None, Running, Good, Bad, Failed } state = None;
    void start(const std::string& path) {
        child.reset();
        nup = path, text.clear();
        state = child.start(python_args({"check", path}), false) ? Running : Failed;
        if (state == Failed) text = std::string("The pipeline could not be started: ") + SDL_GetError();
    }
    void poll() {
        if (state != Running) return;
        for (auto& l : child.take()) text += (text.empty() ? "" : "\n") + l;
        if (child.finished) {
            for (auto& l : child.take()) text += (text.empty() ? "" : "\n") + l;
            state = child.exit_code == 0 ? Good : child.exit_code == 1 ? Bad : Failed;
        }
    }
} check;

// ---- the build: make's events ----

struct Stage {
    std::string id, label;
    enum { Todo, Running, Done, Failed } state = Todo;
    double done = 0, total = 0, started = 0, first_progress_t = 0, first_progress = 0;
    std::string item;
    int count = 0, of = 0;
};

struct Build {
    Child child;
    std::vector<Stage> stages;
    std::deque<std::string> log;
    std::string error_kind, error_message, log_path, exe;
    double started = 0, finished_at = 0;
    bool running = false, succeeded = false, cancelled = false, scroll = true;

    Stage* stage(const std::string& id) {
        for (auto& s : stages)
            if (s.id == id) return &s;
        return nullptr;
    }
    void add_log(const std::string& l) {
        log.push_back(l);
        while (log.size() > 4000) log.pop_front();
    }
    void start() {
        child.reset();
        stages.clear(), log.clear();
        error_kind.clear(), error_message.clear(), exe.clear();
        succeeded = cancelled = false;
        log_path = u8(from_u8(settings.dir) / "make.log");
        std::vector<std::string> a = python_args({"make", settings.nup, "--dir", settings.dir, "--progress", "json"});
        if (settings.jobs > 0) a.insert(a.end(), {"--jobs", std::to_string(settings.jobs)});
        if (settings.clean) a.push_back("--clean");
        started = now_s(), finished_at = 0;
        running = child.start(a, true);
        if (!running) {
            error_kind = "internal";
            error_message = std::string("The pipeline could not be started (") + u8(paths.python) + "): " + SDL_GetError();
            finished_at = now_s();
        }
    }
    void event(const Json& e) {
        const std::string ev = e["event"].s();
        if (ev == "stages") {
            stages.clear();
            for (const auto& s : e["stages"].arr) stages.push_back({s["id"].s(), s["label"].s()});
        } else if (ev == "stage") {
            for (auto& s : stages)
                if (s.state == Stage::Running) s.state = Stage::Done;
            if (Stage* s = stage(e["id"].s())) s->state = Stage::Running, s->started = now_s();
        } else if (ev == "progress") {
            if (Stage* s = stage(e["id"].s())) {
                s->done = e["done"].n(), s->total = e["total"].n();
                if (e["item"].kind == Json::Str && !e["item"].s().empty()) s->item = e["item"].s();
                if (e["of"].kind == Json::Num) s->count = (int)e["count"].n(), s->of = (int)e["of"].n();
                if (!s->first_progress_t && s->done > 0 && s->done < s->total)
                    s->first_progress_t = now_s(), s->first_progress = s->done;
            }
        } else if (ev == "log") {
            add_log(e["text"].s());
        } else if (ev == "done") {
            exe = e["exe"].s();
            succeeded = true;
            for (auto& s : stages) s.state = Stage::Done;
        } else if (ev == "error") {
            error_kind = e["kind"].s(), error_message = e["message"].s();
            if (!e["log"].s().empty()) log_path = e["log"].s();
            for (auto& s : stages)
                if (s.state == Stage::Running) s.state = Stage::Failed;
        }
    }
    void poll() {
        child.poll();
        if (!running) return;
        for (auto& l : child.take()) {
            Json e;
            if (!l.empty() && l[0] == '{' && parse_json(l, e)) event(e);
            else add_log(l);   // (not an event: Python's own output, a traceback)
        }
        if (child.finished) {
            for (auto& l : child.take()) add_log(l);
            running = false;
            finished_at = now_s();
            if (!succeeded && error_kind.empty()) {
                error_kind = cancelled ? "cancelled" : "internal";
                error_message = cancelled ? "The build was cancelled."
                                          : "The pipeline stopped unexpectedly (exit " + std::to_string(child.exit_code) + ").";
            }
        }
    }
    // the whole build's share done: the stages weighted by their usual time
    float overall() const {
        static const std::map<std::string, float> weight = {{"check", 1}, {"extract", 10}, {"lift", 15}, {"compile", 72}, {"link", 2}};
        float sum = 0, got = 0;
        for (const auto& s : stages) {
            auto it = weight.find(s.id);
            const float w = it == weight.end() ? 1 : it->second;
            sum += w;
            if (s.state == Stage::Done) got += w;
            else if (s.state == Stage::Running && s.total > 0) got += w * (float)(s.done / s.total);
        }
        return sum ? got / sum : 0;
    }
} build;

// ---- the installed port ----

struct Installed {
    bool ok = false;
    std::string exe, oranges, built, nup;
    bool cleaned = false;
    void load() {
        ok = false;
        if (settings.dir.empty()) return;
        std::ifstream f(from_u8(settings.dir) / "oranges.json", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        Json j;
        std::error_code ec;
        if (!f || !parse_json(ss.str(), j) || !fs::exists(from_u8(j["exe"].s()), ec)) return;
        exe = j["exe"].s(), oranges = j["oranges"].s(), built = j["built"].s(), nup = j["nup"].s();
        if (built.size() >= 16) built = built.substr(0, 10) + " " + built.substr(11, 5) + " UTC";   // (ISO 8601, UTC)
        cleaned = j["cleaned"].num != 0;
        ok = true;
    }
} installed;

// ---- the look ----

constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
const ImVec4 kInk = ImColor(226, 230, 238), kSoft = ImColor(190, 197, 210), kDim = ImColor(140, 148, 164),
             kAccent = ImColor(138, 174, 255), kGood = ImColor(120, 210, 140), kBad = ImColor(250, 120, 110),
             kWarn = ImColor(240, 200, 110);
ImFont* body = nullptr;
ImFont* bold = nullptr;
float ui_scale = 1;

ImFont* load_font(const char* const* list, int n, float px) {
    for (int i = 0; i < n; i++) {
        std::error_code ec;
        if (fs::exists(list[i], ec))
            if (ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(list[i], px)) return f;
    }
    return nullptr;
}

void set_look(float scale) {
    ui_scale = scale;
    static const char* const regular[] = {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "C:/Windows/Fonts/segoeui.ttf"};
    static const char* const strong[] = {"/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "C:/Windows/Fonts/segoeuib.ttf"};
    ImGuiIO& io = ImGui::GetIO();
    body = load_font(regular, (int)SDL_arraysize(regular), 17 * scale);
    if (!body) {
        ImFontConfig c;
        c.SizePixels = 15 * scale;
        body = io.Fonts->AddFontDefault(&c);
    }
    bold = load_font(strong, (int)SDL_arraysize(strong), 17 * scale);
    if (!bold) bold = body;
    io.FontDefault = body;
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0, st.FrameRounding = 5, st.GrabRounding = 5, st.ChildRounding = 6, st.PopupRounding = 6;
    st.FramePadding = ImVec2(10, 6), st.ItemSpacing = ImVec2(10, 8), st.WindowPadding = ImVec2(18, 16);
    st.FontSizeBase = 17;
    st.ScaleAllSizes(scale);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImColor(17, 19, 25);
    c[ImGuiCol_ChildBg] = ImColor(21, 23, 30);
    c[ImGuiCol_PopupBg] = ImColor(21, 23, 30);
    c[ImGuiCol_Border] = ImColor(255, 255, 255, 30);
    c[ImGuiCol_Text] = kInk;
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_FrameBg] = ImColor(29, 32, 41);
    c[ImGuiCol_FrameBgHovered] = ImColor(38, 44, 60);
    c[ImGuiCol_FrameBgActive] = ImColor(40, 46, 64);
    c[ImGuiCol_Button] = ImColor(40, 46, 64);
    c[ImGuiCol_ButtonHovered] = ImColor(70, 104, 200);
    c[ImGuiCol_ButtonActive] = ImColor(110, 148, 250);
    c[ImGuiCol_Header] = ImColor(38, 44, 60);
    c[ImGuiCol_HeaderHovered] = ImColor(50, 60, 86);
    c[ImGuiCol_HeaderActive] = ImColor(70, 104, 200);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImColor(110, 148, 250);
    c[ImGuiCol_PlotHistogram] = ImColor(110, 148, 250);
    c[ImGuiCol_Separator] = ImColor(255, 255, 255, 24);
}

bool primary_button(const char* label, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor(110, 148, 250));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor(138, 174, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor(90, 128, 230));
    ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(8, 14, 32));
    ImGui::PushFont(bold, 0);
    const bool r = ImGui::Button(label, ImVec2(0, 0));
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    return r;
}

void heading(const char* text) {
    ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Spacing();
}

// ---- the pages ----

enum Page { kGame, kFolder, kOptions, kBuild, kPlay, kPages };
const char* const page_names[kPages] = {"Game", "Folder", "Options", "Build", "Play"};
Page page = kGame;
SDL_Window* window = nullptr;
std::string notice;   // (a line under the page: a shortcut made, a folder opened)

// file dialogs answer on another thread: their answer is picked up by the next frame
std::mutex dialog_m;
std::string dialog_nup, dialog_dir;

void SDLCALL on_nup(void*, const char* const* files, int) {
    if (files && files[0]) {
        std::lock_guard<std::mutex> g(dialog_m);
        dialog_nup = files[0];
    }
}

void SDLCALL on_dir(void*, const char* const* files, int) {
    if (files && files[0]) {
        std::lock_guard<std::mutex> g(dialog_m);
        dialog_dir = files[0];
    }
}

void choose_nup(const std::string& path) {
    if (settings.nup == path && check.state != Check::None) return;
    settings.nup = path;
    save_settings();
    check.start(path);
}

bool game_ok() { return check.state == Check::Good && check.nup == settings.nup; }

constexpr double kNeeded = 12.5e9;   // (RomFS 10 GB, the C 1.4 GB, objects 0.8 GB, the image)

bool folder_ok() {
    if (settings.dir.empty()) return false;
    const fs::path d = from_u8(settings.dir);
    return d.is_absolute();
}

void page_game() {
    heading("Your copy of the game");
    ImGui::TextWrapped("oranges builds a PC port from your own copy of the game, "
                       "title ID 01006F8002326000, version 3.0.3 "
                       "(the base game with that update applied), as one .nup file. Nothing of the game comes with "
                       "oranges, and nothing is downloaded.");
    ImGui::Spacing();
    ImGui::TextDisabled(scan.done ? "Found in your folders:" : "Looking in your folders...");
    std::vector<Found> found;
    {
        std::lock_guard<std::mutex> g(scan.m);
        found = scan.found;
    }
    ImGui::BeginChild("found", ImVec2(0, 190 * ui_scale), ImGuiChildFlags_Borders);
    for (const auto& f : found) {
        char label[1200];
        std::snprintf(label, sizeof label, "%s##%s", u8(from_u8(f.path).filename()).c_str(), f.path.c_str());
        if (ImGui::Selectable(label, settings.nup == f.path)) choose_nup(f.path);
        ImGui::SameLine(std::max(ImGui::GetContentRegionAvail().x * 0.45f, 200 * ui_scale));
        ImGui::TextDisabled("%s", gb((double)f.size).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", u8(from_u8(f.path).parent_path()).c_str());
    }
    if (scan.done && found.empty()) ImGui::TextDisabled("No .nup files in Downloads, Desktop, Documents or Games.");
    ImGui::EndChild();
    if (ImGui::Button("Browse...")) {
        static const SDL_DialogFileFilter filters[] = {{"Game package (.nup)", "nup"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(on_nup, nullptr, window, filters, 2, u8(home_dir()).c_str(), false);
    }
    ImGui::Spacing();
    if (!settings.nup.empty()) {
        ImGui::TextUnformatted(settings.nup.c_str());
        switch (check.state) {
        case Check::Running: ImGui::TextColored(kDim, "Checking it..."); break;
        case Check::Good: ImGui::TextColored(kGood, "This is the supported version (%s).", check.text.c_str()); break;
        case Check::Bad:
            ImGui::PushStyleColor(ImGuiCol_Text, kBad);
            ImGui::TextWrapped("oranges can't build this copy: %s. It needs version 3.0.3 of the game.", check.text.c_str());
            ImGui::PopStyleColor();
            break;
        case Check::Failed:
            ImGui::PushStyleColor(ImGuiCol_Text, kBad);
            ImGui::TextWrapped("This file could not be read as a game package:\n%s", check.text.c_str());
            ImGui::PopStyleColor();
            break;
        default: break;
        }
    }
}

void page_folder() {
    heading("Where to build it");
    ImGui::TextWrapped("The port is built and kept in this folder: the game's files (about 10 GB), the program, and "
                       "what the build makes on the way (about 2.5 GB more while building). Your saves and settings "
                       "are kept in your user folder (%s), not here.", u8(user_dir()).c_str());
    ImGui::Spacing();
    static char buf[1024];
    static std::string shown;
    if (shown != settings.dir) std::snprintf(buf, sizeof buf, "%s", settings.dir.c_str()), shown = settings.dir;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120 * ui_scale);
    if (ImGui::InputText("##dir", buf, sizeof buf)) settings.dir = shown = buf;
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings(), installed.load();
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) SDL_ShowOpenFolderDialog(on_dir, nullptr, window, settings.dir.c_str(), false);
    ImGui::Spacing();
    if (!folder_ok()) {
        ImGui::TextColored(kWarn, "Choose a folder (a full path).");
        return;
    }
    const double free = (double)free_space(from_u8(settings.dir));
    if (free >= kNeeded)
        ImGui::TextColored(kGood, "%s free there: enough (about %s needed while building).", gb(free).c_str(), gb(kNeeded).c_str());
    else
        ImGui::TextColored(kWarn, "%s free there. The build needs about %s (less if part of it is already there).",
                           gb(free).c_str(), gb(kNeeded).c_str());
    std::error_code ec;
    if (fs::exists(from_u8(settings.dir) / "oranges.json", ec))
        ImGui::TextColored(kSoft, "A port was built here before: building again keeps what is still up to date.");
}

int default_jobs() {
    const int cores = SDL_GetNumLogicalCPUCores();
    const double gbytes = SDL_GetSystemRAM() / 1024.0;
    return std::max(1, std::min(cores, (int)(gbytes * 0.8 / 2.5)));
}

void page_options() {
    heading("Options");
    const int cores = SDL_GetNumLogicalCPUCores();
    ImGui::TextWrapped("Build jobs: how many parts of the program are compiled at once. Each takes up to 2.5 GB of "
                       "memory; this computer has %.0f GB and %d threads.", SDL_GetSystemRAM() / 1024.0, cores);
    int jobs = settings.jobs > 0 ? settings.jobs : default_jobs();
    ImGui::SetNextItemWidth(300 * ui_scale);
    if (ImGui::SliderInt("##jobs", &jobs, 1, std::max(cores, 1))) settings.jobs = jobs == default_jobs() ? 0 : jobs;
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
    ImGui::SameLine();
    ImGui::TextDisabled(settings.jobs ? "(chosen)" : "(what fits in memory)");
    ImGui::Spacing();
    if (ImGui::Checkbox("Delete the build's own files when it is done (2.2 GB)", &settings.clean)) save_settings();
    ImGui::TextDisabled("Keeping them makes a rebuild for a new oranges version faster.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("The game's own settings (F1 in the game changes them later):");
    int mode = hos_option_int("console_mode", 0);
    const char* modes[] = {"Docked (1920x1080)", "Handheld (1280x720, lighter on the GPU)"};
    ImGui::SetNextItemWidth(360 * ui_scale);
    if (ImGui::Combo("Console mode", &mode, modes, 2)) {
        if (mode) hos_option_set("console_mode", "1");
        else hos_option_unset("console_mode");
    }
    ImGui::TextDisabled("Saved in %s", hos_options_path());
}

void stage_dot(const Stage& s) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 6 * ui_scale, h = ImGui::GetTextLineHeight();
    const ImVec2 c(p.x + r + 2, p.y + h / 2);
    switch (s.state) {
    case Stage::Done: d->AddCircleFilled(c, r, ImColor(kGood)); break;
    case Stage::Running: d->AddCircle(c, r, ImColor(kAccent), 0, 2 * ui_scale); d->AddCircleFilled(c, r * 0.45f, ImColor(kAccent)); break;
    case Stage::Failed: d->AddCircleFilled(c, r, ImColor(kBad)); break;
    default: d->AddCircle(c, r, ImColor(kDim), 0, 1.5f * ui_scale);
    }
    ImGui::Dummy(ImVec2(2 * r + 8, h));
    ImGui::SameLine();
}

std::string eta(const Stage& s) {
    if (s.total <= 0 || s.done <= 0 || !s.first_progress_t) return "";
    const double t = now_s() - s.first_progress_t, d = s.done - s.first_progress;
    if (t < 3 || d <= 0) return "";
    return "about " + duration((s.total - s.done) * t / d) + " left";
}

void open_path(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    SDL_OpenURL(("file://" + u8(p)).c_str());
#endif
}

void page_build() {
    heading("Build");
    if (build.stages.empty() && !build.running && build.error_kind.empty()) {
        ImGui::TextWrapped("Everything is set: %s into %s, %d build jobs.", u8(from_u8(settings.nup).filename()).c_str(),
                           settings.dir.c_str(), settings.jobs ? settings.jobs : default_jobs());
        ImGui::TextWrapped("The build takes from a few minutes to an hour, depending on the computer. It can be "
                           "cancelled and started again: it picks up where it stopped.");
        ImGui::Spacing();
        if (primary_button("Build", game_ok() && folder_ok())) build.start();
        if (!game_ok()) ImGui::TextColored(kWarn, "Choose a supported copy of the game first (Game).");
        else if (!folder_ok()) ImGui::TextColored(kWarn, "Choose a folder first (Folder).");
        return;
    }
    for (const auto& s : build.stages) {
        stage_dot(s);
        ImGui::PushFont(s.state == Stage::Running ? bold : body, 0);
        ImGui::TextColored(s.state == Stage::Todo ? kDim : kInk, "%s", s.label.c_str());
        ImGui::PopFont();
        if (s.state == Stage::Running && s.total > 0) {
            ImGui::SameLine(260 * ui_scale);
            char over[160];
            if (s.of) std::snprintf(over, sizeof over, "%d of %d", s.count, s.of);
            else std::snprintf(over, sizeof over, "%.0f%%", 100 * s.done / s.total);
            ImGui::ProgressBar((float)(s.done / s.total), ImVec2(260 * ui_scale, 0), over);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", eta(s).c_str());
        }
    }
    ImGui::Spacing();
    char over[64];
    const double elapsed = (build.finished_at ? build.finished_at : now_s()) - build.started;
    std::snprintf(over, sizeof over, "%.0f%%", 100 * build.overall());
    ImGui::ProgressBar(build.overall(), ImVec2(-1, 0), over);
    ImGui::TextDisabled("%s %s", build.running ? "Running for" : "Took", duration(elapsed).c_str());
    if (build.running) {
        ImGui::SameLine();
        ImGui::BeginDisabled(build.cancelled);
        if (ImGui::Button(build.cancelled ? "Cancelling..." : "Cancel")) build.cancelled = true, build.child.cancel();
        ImGui::EndDisabled();
    }
    if (!build.running && !build.error_kind.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, build.error_kind == "cancelled" ? kWarn : kBad);
        ImGui::TextWrapped("%s", build.error_message.c_str());
        ImGui::PopStyleColor();
        if (build.error_kind == "memory")
            ImGui::TextWrapped("Lower the build jobs in Options, then build again.");
        else if (build.error_kind == "space")
            ImGui::TextWrapped("Free some space or choose another folder, then build again.");
        else if (build.error_kind != "cancelled" && build.error_kind != "game")
            ImGui::TextWrapped("The log has the details, for a report: %s", build.log_path.c_str());
        if (primary_button(build.error_kind == "cancelled" ? "Continue the build" : "Build again")) build.start();
        ImGui::SameLine();
        if (ImGui::Button("Open the build folder")) open_path(from_u8(settings.dir));
    }
    if (build.succeeded && !build.running) {
        ImGui::TextColored(kGood, "Built: %s", build.exe.c_str());
        if (primary_button("Continue")) installed.load(), page = kPlay;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Log");
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, kSoft);
    ImGuiListClipper clip;
    clip.Begin((int)build.log.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(build.log[i].c_str());
    ImGui::PopStyleColor();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void launch(const std::string& exe) {
    const char* argv[] = {exe.c_str(), nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)argv);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process* p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (p) {
        notice = "Started. (The game's log is in " + u8(user_dir()) + ".)";
        SDL_DestroyProcess(p);
    } else {
        notice = std::string("It could not be started: ") + SDL_GetError();
    }
}

std::string make_shortcut(const std::string& exe) {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (!appdata) return "No Start menu folder (APPDATA is not set).";
    const fs::path lnk = from_u8(appdata) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "oranges.lnk";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    std::string result = "The shortcut could not be made.";
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link))) {
        link->SetPath(from_u8(exe).wstring().c_str());
        link->SetWorkingDirectory(from_u8(exe).parent_path().wstring().c_str());
        link->SetDescription(L"Built by oranges");
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file))) {
            if (SUCCEEDED(file->Save(lnk.wstring().c_str(), TRUE))) result = "Added to the Start menu: " + u8(lnk);
            file->Release();
        }
        link->Release();
    }
    CoUninitialize();
    return result;
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const fs::path apps = (xdg && xdg[0] == '/' ? fs::path(xdg) : home_dir() / ".local" / "share") / "applications";
    std::error_code ec;
    fs::create_directories(apps, ec);
    const fs::path f = apps / "oranges.desktop";
    std::ofstream o(f, std::ios::binary);
    std::string quoted = exe;
    for (size_t i = 0; (i = quoted.find_first_of("\"`$\\", i)) != std::string::npos; i += 2) quoted.insert(i, "\\");
    o << "[Desktop Entry]\nType=Application\nName=oranges\nComment=Built by "
         "oranges\nExec=\"" << quoted << "\"\nPath=" << u8(from_u8(exe).parent_path())
      << "\nTerminal=false\nCategories=Game;\n";
    o.close();
    return o ? "Added to the applications menu: " + u8(f) : "The shortcut could not be written: " + u8(f);
#endif
}

std::thread cleaner;
std::atomic<bool> cleaning{false};

void page_play() {
    heading("Play");
    if (!installed.ok) {
        ImGui::TextWrapped("No port has been built in %s yet.", settings.dir.empty() ? "the chosen folder" : settings.dir.c_str());
        if (primary_button("Go to Build")) page = kBuild;
        return;
    }
    ImGui::TextWrapped("Built %s with oranges %s.", installed.built.c_str(), installed.oranges.c_str());
    ImGui::TextDisabled("%s", installed.exe.c_str());
    if (paths.release && installed.oranges != paths.version) {
        ImGui::Spacing();
        ImGui::TextColored(kWarn, "This is oranges %s. Build again to update the port (saves and settings are kept).",
                           paths.version.c_str());
        if (ImGui::Button("Build again")) page = kBuild, build.start();
    }
    ImGui::Spacing();
    if (primary_button("Play")) launch(installed.exe);
    ImGui::Spacing();
    if (ImGui::Button("Add a shortcut")) notice = make_shortcut(installed.exe);
    ImGui::SameLine();
    if (ImGui::Button("Open the save folder")) open_path(user_dir() / "saves");
    ImGui::SameLine();
    if (ImGui::Button("Open the build folder")) open_path(from_u8(settings.dir));
    ImGui::Spacing();
    std::error_code ec;
    const fs::path d = from_u8(settings.dir);
    const bool leftovers = fs::exists(d / "gen", ec) || fs::exists(fs::path(installed.exe).parent_path() / "obj", ec);
    if (leftovers) {
        ImGui::BeginDisabled(cleaning);
        if (ImGui::Button(cleaning ? "Deleting..." : "Delete the build's own files (the generated C and objects, 2.2 GB)")) {
            cleaning = true;
            if (cleaner.joinable()) cleaner.join();
            cleaner = std::thread([d, obj = fs::path(installed.exe).parent_path() / "obj"] {
                std::error_code e;
                fs::remove_all(d / "gen", e);
                fs::remove_all(obj, e);
                cleaning = false;
            });
            notice = "Deleting them: a later build makes them again.";
        }
        ImGui::EndDisabled();
    }
    ImGui::TextDisabled("F1 in the game opens its settings (graphics, controls).");
}

bool can_enter(Page p) {
    if (build.running) return p == kBuild;
    switch (p) {
    case kFolder: return !settings.nup.empty();
    case kOptions: return game_ok() && folder_ok();
    case kBuild: return game_ok() && folder_ok();
    case kPlay: return installed.ok;
    default: return true;
    }
}

void frame() {
    {
        std::lock_guard<std::mutex> g(dialog_m);
        if (!dialog_nup.empty()) choose_nup(dialog_nup), dialog_nup.clear();
        if (!dialog_dir.empty()) settings.dir = dialog_dir, dialog_dir.clear(), save_settings(), installed.load();
    }
    check.poll();
    const bool was_running = build.running;
    build.poll();
    if (was_running && !build.running && build.succeeded) installed.load();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("oranges", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus);
    // the steps, on the left
    ImGui::BeginChild("steps", ImVec2(190 * ui_scale, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.5f);
    ImGui::TextColored(kAccent, "oranges");
    ImGui::PopFont();
    ImGui::TextDisabled("%s", paths.version.c_str());
    ImGui::Spacing();
    ImGui::Spacing();
    for (int i = 0; i < kPages; i++) {
        char label[64];
        std::snprintf(label, sizeof label, "%d   %s", i + 1, page_names[i]);
        ImGui::BeginDisabled(!can_enter((Page)i) && page != i);
        if (ImGui::Selectable(label, page == i, 0, ImVec2(0, 30 * ui_scale))) page = (Page)i;
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    const float bar = ImGui::GetFrameHeightWithSpacing() + 12 * ui_scale;
    ImGui::BeginChild("page", ImVec2(0, -bar), ImGuiChildFlags_None);
    switch (page) {
    case kGame: page_game(); break;
    case kFolder: page_folder(); break;
    case kOptions: page_options(); break;
    case kBuild: page_build(); break;
    case kPlay: page_play(); break;
    default: break;
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (!notice.empty()) {
        ImGui::TextColored(kSoft, "%s", notice.c_str());
        ImGui::SameLine();
    }
    if (page != kBuild && page != kPlay) {
        const Page next = (Page)(page + 1);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - 220 * ui_scale));
        ImGui::BeginDisabled(page == kGame || build.running);
        if (ImGui::Button("Back", ImVec2(100 * ui_scale, 0))) page = (Page)(page - 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_enter(next));
        if (ImGui::Button("Next", ImVec2(100 * ui_scale, 0))) page = next;
        ImGui::EndDisabled();
    }
    ImGui::EndGroup();
    ImGui::End();
}

}  // namespace

int main(int argc, char** argv) {
    std::string shot, start_page;
    double shot_after = 2;
    bool quit_after_shot = false, quit_after_build = false, start_build = false;
    std::string arg_nup, arg_dir;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--nup" && more) arg_nup = argv[++i];
        else if (a == "--dir" && more) arg_dir = argv[++i];
        else if (a == "--page" && more) start_page = argv[++i];
        else if (a == "--build") start_build = true;
        else if (a == "--shot" && more) shot = argv[++i];
        else if (a == "--shot-after" && more) shot_after = std::atof(argv[++i]);
        else if (a == "--quit-after-shot") quit_after_shot = true;
        else if (a == "--quit-after-build") quit_after_build = true;
        else {
            std::fprintf(stderr, "usage: %s [--nup FILE] [--dir FOLDER] [--page NAME] [--build] [--shot FILE.bmp] "
                                 "[--shot-after S] [--quit-after-shot] [--quit-after-build]\n", argv[0]);
            return 2;
        }
    }
    SDL_SetAppMetadata("oranges", "1", "dev.oranges.setup");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "oranges: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const bool found = find_paths();
    load_settings();
    if (!arg_nup.empty()) settings.nup = u8(fs::absolute(from_u8(arg_nup)));
    if (!arg_dir.empty()) settings.dir = u8(fs::absolute(from_u8(arg_dir)));
    if (settings.dir.empty()) settings.dir = u8(home_dir() / "Games" / "oranges");
    hos_options_load();

    const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
    window = SDL_CreateWindow("oranges setup", (int)(1000 * scale), (int)(680 * scale), SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "oranges: no window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    set_look(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    if (!found) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "oranges",
                                 "This oranges is incomplete: its src folder (oranges' sources) is missing. Download "
                                 "the release again.", window);
        return 1;
    }
    std::error_code ec;
    if (!fs::exists(paths.python, ec)) {
        const std::string m = "This oranges is incomplete: its Python is missing (" + u8(paths.python) + "). Download the release again.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "oranges", m.c_str(), window);
        return 1;
    }
    scan.start();
    installed.load();
    if (!settings.nup.empty()) check.start(settings.nup);
    if (installed.ok) page = kPlay;
    for (int i = 0; i < kPages; i++)
        if (!start_page.empty() && SDL_strcasecmp(start_page.c_str(), page_names[i]) == 0) page = (Page)i;
    bool build_requested = start_build;

    const double t0 = now_s();
    bool quit = false, shot_taken = false;
    while (!quit) {
        // (idle: a frame when something happens, or every 100 ms while a child runs; the build's own redraws)
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, build.running || check.state == Check::Running || !scan.done || cleaning ? 100 : 500)) {
            do {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
            } while (SDL_PollEvent(&e));
        }
        if (build_requested && check.state != Check::Running) {
            build_requested = false;
            if (game_ok() && folder_ok()) page = kBuild, build.start();
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 17, 19, 25, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!shot.empty() && !shot_taken && now_s() - t0 >= shot_after) {
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SaveBMP(s, shot.c_str());
                SDL_DestroySurface(s);
            }
            shot_taken = true;
            if (quit_after_shot) quit = true;
        }
        SDL_RenderPresent(renderer);
        if (quit_after_build && build.finished_at && !build.running) quit = true;
    }
    if (build.running) build.child.cancel();
    build.child.reset();
    check.child.reset();
    if (cleaner.joinable()) cleaner.join();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::_Exit(quit_after_build ? (build.succeeded ? 0 : 1) : 0);   // (the scan's thread may still be walking folders)
}
