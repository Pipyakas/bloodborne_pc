// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the first-launch screen. When no game folder is known, run_windows.py starts
// `bb-probe --first-run <result file>` before preparing the game: a centered ImGui window (SDL
// renderer) asks for the game folder, or for the game's .pkg files and where to install them
// (next to the executable by default, or another folder). Packages are installed by
// bbport-pkg.exe (tools/setup/PkgInstall.cs) next to the executable, whose progress is shown
// here. The chosen folder is written to the result file; the exit code is 0 then, 1 when the
// user quits.
//
// BB_FIRST_RUN_SCRIPT (tests and agents): `;`-separated steps run in order without showing the
// window: click:<button>, folder:<path> / pkgs:<path>|<path> / dest:<path> (the next folder,
// package or destination dialog returns them), wait:<page> (choose, folder, packages,
// installing, done, failed), shot:<png path>, sleep:<frames>.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <miniz.h>
#include "../bbgpu.h"
#include "bbport_overlay.h"
#include "bbport_imgui.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

namespace {

enum class Page { Choose, Folder, Packages, Installing, Done, Failed };
const char* const PageNames[] = {"choose", "folder", "packages", "installing", "done", "failed"};

#ifdef _WIN32
constexpr char Sep = '\\';
#else
constexpr char Sep = '/';
#endif

std::string Join(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir.back() == '\\' || dir.back() == '/') return dir + name;
    return dir + Sep + name;
}

bool IsFile(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

/// TITLE, TITLE_ID, APP_VER and CATEGORY from a param.sfo (see tools/setup/PkgInstall.cs).
struct Sfo {
    std::string title, id, version;
};

bool ReadSfo(const std::string& path, Sfo& out) {
    size_t size = 0;
    auto* d = static_cast<unsigned char*>(SDL_LoadFile(path.c_str(), &size));
    if (!d) return false;
    const auto u16 = [&](size_t o) { return size_t(d[o] | d[o + 1] << 8); };
    const auto u32 = [&](size_t o) { return size_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | size_t(d[o + 3]) << 24); };
    bool ok = size >= 20 && d[0] == 0 && d[1] == 'P' && d[2] == 'S' && d[3] == 'F';
    if (ok) {
        const size_t keys = u32(8), data = u32(12), count = u32(16);
        for (size_t i = 0; i < count && 20 + i * 16 + 16 <= size; ++i) {
            const size_t e = 20 + i * 16, k = keys + u16(e), format = u16(e + 2);
            const size_t length = u32(e + 4), at = data + u32(e + 12);
            if (k >= size || at + length > size || format != 0x0204) continue;
            const std::string key(reinterpret_cast<char*>(d + k), strnlen(reinterpret_cast<char*>(d + k), size - k));
            std::string value(reinterpret_cast<char*>(d + at), length);
            value.resize(strnlen(value.c_str(), value.size()));
            if (key == "TITLE") out.title = value;
            if (key == "TITLE_ID") out.id = value;
            if (key == "APP_VER") out.version = value;
        }
    }
    SDL_free(d);
    return ok;
}

class FirstRun {
public:
    explicit FirstRun(const char* result) : result_path(result) {}
    int Run();

private:
    // ---- state ----
    Page page = Page::Choose;
    std::string result_path, game_dir, message;
    bool message_error = false;
    std::vector<std::string> packages, package_info;
    std::string title_id;
    std::string exe_dir;
    std::string dest_other;
    bool dest_exe = true;
    // Install progress (the reader thread writes these).
    std::mutex mutex;
    std::vector<std::string> log;
    float fraction = 0.0f;
    std::string progress_text, installed;
    SDL_Process* process = nullptr;
    std::thread reader;
    std::atomic<bool> reading{false};
    // Answers of the native dialogs (their callbacks may come from another thread).
    std::mutex dialog_mutex;
    int dialog_kind = 0; // 1 folder, 2 packages, 3 destination
    std::vector<std::string> dialog_result;
    bool dialog_done = false, dialog_open = false;
    // ---- script ----
    std::vector<std::string> script;
    size_t step = 0;
    int sleep_frames = 0;
    std::string click, canned_folder, canned_dest;
    std::vector<std::string> canned_pkgs;
    std::string shot;
    bool quit = false, finished = false;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    float scale = 1.0f;
    int focused_page = -1;
    bool page_focus = true;

    std::string Tool() const { return Join(exe_dir, "bbport-pkg.exe"); }
    bool Button(const char* label, float width = 0.0f);
    void OpenDialog(int kind);
    void CheckDialog();
    void ChooseFolder(const std::string& dir);
    void ChoosePackages(const std::vector<std::string>& files);
    std::string Destination() const { return dest_exe ? exe_dir : dest_other; }
    void StartInstall();
    void PollInstall();
    void Finish(const std::string& dir);
    void RunScript();
    void Draw();
    void SaveShot(const std::string& path);
};

bool FirstRun::Button(const char* label, float width) {
    if (page_focus) {
        ImGui::SetKeyboardFocusHere();
        page_focus = false;
    }
    const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
    if (!click.empty() && click == label) {
        click.clear();
        return true;
    }
    return pressed;
}

void FirstRun::OpenDialog(int kind) {
    // Scripted answers stand in for the native dialogs.
    if (kind == 1 && !canned_folder.empty()) { ChooseFolder(canned_folder); canned_folder.clear(); return; }
    if (kind == 2 && !canned_pkgs.empty()) { ChoosePackages(canned_pkgs); canned_pkgs.clear(); return; }
    if (kind == 3 && !canned_dest.empty()) { dest_other = canned_dest; dest_exe = false; canned_dest.clear(); return; }
    {
        std::scoped_lock lock{dialog_mutex};
        if (dialog_open) return;
        dialog_open = true;
        dialog_kind = kind;
        dialog_done = false;
        dialog_result.clear();
    }
    const auto done = [](void* user, const char* const* files, int) {
        auto* self = static_cast<FirstRun*>(user);
        std::scoped_lock lock{self->dialog_mutex};
        for (; files && *files; ++files) self->dialog_result.push_back(*files);
        self->dialog_done = true;
    };
    if (kind == 2) {
        static const SDL_DialogFileFilter filters[] = {{"PS4 packages", "pkg"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(done, this, window, filters, 2, nullptr, true);
    } else {
        SDL_ShowOpenFolderDialog(done, this, window, nullptr, false);
    }
}

void FirstRun::CheckDialog() {
    std::vector<std::string> files;
    int kind;
    {
        std::scoped_lock lock{dialog_mutex};
        if (!dialog_open || !dialog_done) return;
        dialog_open = false;
        files.swap(dialog_result);
        kind = dialog_kind;
    }
    if (files.empty()) return; // cancelled
    if (kind == 1) ChooseFolder(files[0]);
    if (kind == 2) ChoosePackages(files);
    if (kind == 3) { dest_other = files[0]; dest_exe = false; }
}

void FirstRun::ChooseFolder(const std::string& dir) {
    page = Page::Folder;
    game_dir = dir;
    message_error = true;
    if (!IsFile(Join(dir, "eboot.bin"))) {
        message = "No eboot.bin in this folder: choose the folder of the game's data (it holds eboot.bin, "
                  "sce_sys and dvdroot_ps4).";
        return;
    }
    Sfo sfo;
    if (!ReadSfo(Join(Join(dir, "sce_sys"), "param.sfo"), sfo)) {
        message = "eboot.bin found, but sce_sys/param.sfo is missing, so the version is unknown (bbport needs v01.09).";
        return;
    }
    message = (sfo.title.empty() ? std::string("?") : sfo.title) + "  " + sfo.id + "  v" + sfo.version;
    message_error = sfo.version.rfind("01.09", 0) != 0;
    if (message_error) message += ": bbport needs v01.09 (install the 1.09 update into this folder).";
}

void FirstRun::ChoosePackages(const std::vector<std::string>& files) {
    page = Page::Packages;
    packages = files;
    package_info.clear();
    title_id.clear();
    message.clear();
    message_error = false;
    if (!IsFile(Tool())) {
        message = "bbport-pkg.exe is missing next to the game executable (build.sh builds it on Windows).";
        message_error = true;
        return;
    }
    // `bbport-pkg info` describes each package: "<path>: <title> <TITLE_ID> v<version> (<kind>, <size>)".
    const std::string tool = Tool();
    std::vector<const char*> args{tool.c_str(), "info"};
    for (const std::string& f : files) args.push_back(f.c_str());
    args.push_back(nullptr);
    SDL_Process* p = SDL_CreateProcess(args.data(), true);
    if (!p) {
        message = std::string("Cannot start bbport-pkg.exe: ") + SDL_GetError();
        message_error = true;
        return;
    }
    size_t size = 0;
    int code = -1;
    char* out = static_cast<char*>(SDL_ReadProcess(p, &size, &code));
    SDL_DestroyProcess(p);
    std::string text(out ? out : "", size);
    SDL_free(out);
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ' ') continue;
        // Drop the path: the file name is enough.
        const size_t colon = line.find(": ", 2);
        std::string shown = line;
        if (colon != std::string::npos) {
            std::string path = line.substr(0, colon);
            const size_t slash = path.find_last_of("\\/");
            shown = (slash == std::string::npos ? path : path.substr(slash + 1)) + "\n    " + line.substr(colon + 2);
            const size_t cusa = line.find("CUSA", colon);
            if (cusa != std::string::npos && title_id.empty()) title_id = line.substr(cusa, 9);
        }
        package_info.push_back(shown);
    }
    if (code != 0) {
        message = package_info.empty() ? "These files are not PS4 packages." : package_info.back();
        message_error = true;
    }
}

void FirstRun::StartInstall() {
    std::string tool = Tool(), dest = Destination();
    std::vector<const char*> args{tool.c_str(), "install", dest.c_str()};
    for (const std::string& f : packages) args.push_back(f.c_str());
    args.push_back(nullptr);
    {
        std::scoped_lock lock{mutex};
        log.clear();
        fraction = 0.0f;
        progress_text = "Starting...";
        installed.clear();
    }
    SDL_CreateDirectory(dest.c_str());
    process = SDL_CreateProcess(args.data(), true);
    if (!process) {
        page = Page::Failed;
        message = std::string("Cannot start bbport-pkg.exe: ") + SDL_GetError();
        return;
    }
    page = Page::Installing;
    reading = true;
    reader = std::thread([this] {
        SDL_IOStream* out = SDL_GetProcessOutput(process);
        std::string line;
        char buffer[4096];
        for (;;) {
            const size_t n = SDL_ReadIO(out, buffer, sizeof(buffer));
            if (n == 0) {
                // The pipe is non-blocking: no data yet is not the end of the output.
                if (SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) { SDL_Delay(20); continue; }
                break;
            }
            for (size_t i = 0; i < n; ++i) {
                if (buffer[i] != '\n') { if (buffer[i] != '\r') line += buffer[i]; continue; }
                std::scoped_lock lock{mutex};
                float percent;
                if (std::sscanf(line.c_str(), " %f%%", &percent) == 1 && line.find("GB") != std::string::npos) {
                    fraction = percent / 100.0f;
                    progress_text = line.substr(line.find('%') + 1);
                } else if (!line.empty()) {
                    if (line.rfind("Installed: ", 0) == 0) installed = line.substr(11);
                    log.push_back(line);
                }
                line.clear();
            }
        }
        reading = false;
    });
}

void FirstRun::PollInstall() {
    if (page != Page::Installing || reading) return;
    int code = -1;
    if (!SDL_WaitProcess(process, false, &code)) return;
    reader.join();
    SDL_DestroyProcess(process);
    process = nullptr;
    std::scoped_lock lock{mutex};
    if (code == 0 && !installed.empty()) {
        page = Page::Done;
        ChooseFolder(installed);
        page = Page::Done;
    } else {
        page = Page::Failed;
        message = "Installing failed";
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->rfind("error: ", 0) == 0) { message = it->substr(7); break; }
        }
    }
}

void FirstRun::Finish(const std::string& dir) {
    if (FILE* f = std::fopen(result_path.c_str(), "wb")) {
        std::fputs(dir.c_str(), f);
        std::fclose(f);
        finished = true;
    } else {
        message = "Cannot write " + result_path;
        message_error = true;
    }
}

void FirstRun::RunScript() {
    if (sleep_frames > 0) { --sleep_frames; return; }
    if (step >= script.size() || !click.empty()) return;
    const std::string& s = script[step];
    const size_t colon = s.find(':');
    const std::string verb = s.substr(0, colon), arg = colon == std::string::npos ? "" : s.substr(colon + 1);
    if (verb == "wait") {
        if (arg != PageNames[int(page)]) return;
    } else if (verb == "click") {
        click = arg;
    } else if (verb == "folder") {
        canned_folder = arg;
    } else if (verb == "dest") {
        canned_dest = arg;
    } else if (verb == "pkgs") {
        canned_pkgs.clear();
        for (size_t at = 0; at <= arg.size();) {
            size_t end = arg.find('|', at);
            if (end == std::string::npos) end = arg.size();
            canned_pkgs.push_back(arg.substr(at, end - at));
            at = end + 1;
        }
    } else if (verb == "shot") {
        shot = arg;
    } else if (verb == "sleep") {
        sleep_frames = std::atoi(arg.c_str());
    }
    std::printf("First run: script %s (page %s)\n", s.c_str(), PageNames[int(page)]);
    ++step;
    sleep_frames = std::max(sleep_frames, 3); // let the UI settle between steps
}

void FirstRun::Draw() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    page_focus = focused_page != int(page);
    focused_page = int(page);
    const float width = std::min(960.0f * scale, viewport->WorkSize.x * 0.94f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(width, viewport->WorkSize.y * 0.94f));
    if (page_focus) ImGui::SetNextWindowFocus();
    constexpr ImGuiWindowFlags Flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::Begin("##bbport_first_run", nullptr, Flags);
    ImGui::PushFont(nullptr, 36.0f);
    ImGui::TextUnformatted("Bloodborne");
    ImGui::PopFont();
    ImGui::TextDisabled("bbport: first launch");
    ImGui::TextWrapped("D-pad / left stick: move    Cross / A: select    Circle / B: back");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(width - ImGui::GetStyle().WindowPadding.x);
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const ImVec4 red(0.95f, 0.45f, 0.40f, 1.0f), ok(0.75f, 0.85f, 0.65f, 1.0f);

    switch (page) {
    case Page::Choose:
        ImGui::TextUnformatted("bbport needs the game's data: Bloodborne v1.09 (for example CUSA03173 or CUSA00900).");
        ImGui::Spacing();
        ImGui::TextUnformatted("Where is it?");
        ImGui::Spacing();
        if (Button("I have the game folder...", -1.0f)) OpenDialog(1);
        ImGui::TextDisabled("The folder with eboot.bin, sce_sys and dvdroot_ps4 (the 1.09 update included).");
        ImGui::Spacing();
        if (Button("Install from .pkg files...", -1.0f)) OpenDialog(2);
        ImGui::TextDisabled("The game's .pkg and its 1.09 update .pkg: they are installed into one folder.");
        ImGui::Spacing();
        ImGui::Separator();
        if (Button("Quit")) quit = true;
        break;

    case Page::Folder:
        ImGui::TextUnformatted("Game folder:");
        ImGui::TextDisabled("%s", game_dir.c_str());
        ImGui::Spacing();
        ImGui::TextColored(message_error ? red : ok, "%s", message.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        if (Button("Back", half)) page = Page::Choose;
        ImGui::SameLine();
        ImGui::BeginDisabled(!IsFile(Join(game_dir, "eboot.bin")));
        if (Button(message_error ? "Use it anyway" : "Play", half)) Finish(game_dir);
        ImGui::EndDisabled();
        break;

    case Page::Packages: {
        ImGui::TextUnformatted("Packages:");
        for (const std::string& info : package_info) ImGui::TextDisabled("%s", info.c_str());
        if (package_info.empty()) for (const std::string& p : packages) ImGui::TextDisabled("%s", p.c_str());
        ImGui::Spacing();
        if (!message.empty()) ImGui::TextColored(red, "%s", message.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("Install the game data (about 32 GB) to:");
        if (ImGui::RadioButton("Next to the game executable (default)", dest_exe)) dest_exe = true;
        ImGui::Indent();
        ImGui::TextDisabled("%s", exe_dir.c_str());
        ImGui::Unindent();
        if (ImGui::RadioButton("Another folder", !dest_exe)) {
            if (dest_other.empty()) OpenDialog(3);
            else dest_exe = false;
        }
        ImGui::Indent();
        ImGui::TextDisabled("%s", dest_other.empty() ? "(not chosen)" : dest_other.c_str());
        ImGui::SameLine();
        if (Button("Choose...")) OpenDialog(3);
        ImGui::Unindent();
        ImGui::Spacing();
        const std::string target = Join(Destination(), title_id.empty() ? "CUSA....." : title_id);
        ImGui::TextUnformatted("The game folder will be:");
        ImGui::TextColored(ok, "%s", target.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        if (Button("Back", half)) page = Page::Choose;
        ImGui::SameLine();
        ImGui::BeginDisabled(message_error || Destination().empty() || packages.empty());
        if (Button("Install", half)) StartInstall();
        ImGui::EndDisabled();
        break;
    }

    case Page::Installing:
    case Page::Done:
    case Page::Failed: {
        std::scoped_lock lock{mutex};
        ImGui::TextUnformatted(page == Page::Installing ? "Installing the game data..."
                               : page == Page::Done     ? "The game data is installed."
                                                        : "Installing failed.");
        ImGui::Spacing();
        ImGui::ProgressBar(page == Page::Done ? 1.0f : fraction, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("%s", page == Page::Done ? installed.c_str() : progress_text.c_str());
        ImGui::Spacing();
        const size_t from = log.size() > 6 ? log.size() - 6 : 0;
        for (size_t i = from; i < log.size(); ++i) ImGui::TextDisabled("%s", log[i].c_str());
        ImGui::Spacing();
        if (page != Page::Installing && !message.empty()) ImGui::TextColored(message_error ? red : ok, "%s", message.c_str());
        ImGui::Separator();
        if (page == Page::Installing) {
            if (Button("Cancel")) SDL_KillProcess(process, true);
        } else if (page == Page::Failed) {
            if (Button("Back", half)) page = Page::Packages;
        } else {
            if (Button("Quit", half)) quit = true;
            ImGui::SameLine();
            if (Button("Play", half)) Finish(installed);
        }
        break;
    }
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void FirstRun::SaveShot(const std::string& path) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface) return;
    SDL_Surface* rgb = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGB24);
    SDL_DestroySurface(surface);
    if (!rgb) return;
    // Rows packed for the PNG writer.
    std::vector<unsigned char> pixels(size_t(rgb->w) * rgb->h * 3);
    for (int y = 0; y < rgb->h; ++y) {
        std::memcpy(&pixels[size_t(y) * rgb->w * 3], static_cast<unsigned char*>(rgb->pixels) + size_t(y) * rgb->pitch, size_t(rgb->w) * 3);
    }
    size_t length = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(pixels.data(), rgb->w, rgb->h, 3, &length, MZ_BEST_SPEED, MZ_FALSE);
    if (png) {
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(png, 1, length, f);
            std::fclose(f);
        }
        mz_free(png);
    }
    SDL_DestroySurface(rgb);
}

int FirstRun::Run() {
    const char* script_env = std::getenv("BB_FIRST_RUN_SCRIPT");
    const bool scripted = script_env && *script_env;
    for (size_t at = 0; scripted && at < std::strlen(script_env);) {
        const char* end = std::strchr(script_env + at, ';');
        const size_t n = end ? size_t(end - script_env) - at : std::strlen(script_env) - at;
        if (n) script.emplace_back(script_env + at, n);
        at += n + 1;
    }
    sleep_frames = 5; // auto-sized windows take a frame to get their size
    const char* base = SDL_GetBasePath();
    exe_dir = base ? base : "";
    while (exe_dir.size() > 3 && (exe_dir.back() == '\\' || exe_dir.back() == '/')) exe_dir.pop_back();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::printf("First run: SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }
    // Scripted runs draw into a hidden window's offscreen target: nothing shows on the desktop.
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (scripted) flags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("Bloodborne (bbport): first launch", 1280, 720, flags);
    renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::printf("First run: no window: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    SDL_SetRenderVSync(renderer, 1);
    scale = scripted ? 1.0f : std::max(1.0f, SDL_GetWindowDisplayScale(window));
    SDL_Texture* target = scripted ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 1280, 720) : nullptr;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    BbImGui::ControllerStyle();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 0.96f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.55f, 0.48f, 0.32f, 0.8f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.20f, 0.17f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.38f, 0.32f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.50f, 0.42f, 0.27f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.15f, 0.13f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.23f, 0.18f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.34f, 0.29f, 0.21f, 1.0f);
    style.Colors[ImGuiCol_NavCursor] = ImVec4(0.85f, 0.72f, 0.42f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.62f, 0.52f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.85f, 0.72f, 0.42f, 1.0f);
    style.WindowBorderSize = 1.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    const unsigned char* font = nullptr;
    int font_size = 0;
    BbOverlay::FontData(&font, &font_size);
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(font), font_size, BbImGui::FontSize, &font_config);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    while (!quit && !finished) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
        }
        if (scripted) {
            RunScript();
            if (step >= script.size() && sleep_frames == 0 && shot.empty()) quit = true; // script done
        }
        CheckDialog();
        PollInstall();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        BbImGui::PollDirections();
        const bool was_editing = ImGui::IsAnyItemActive() ||
            ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        ImGui::NewFrame();
        Draw();
        if (!dialog_open && !was_editing &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
             ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            if (page == Page::Choose || page == Page::Done) quit = true;
            else if (page == Page::Installing) SDL_KillProcess(process, true);
            else page = page == Page::Failed ? Page::Packages : Page::Choose;
        }
        ImGui::Render();
        SDL_SetRenderTarget(renderer, target);
        SDL_SetRenderDrawColor(renderer, 10, 10, 12, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!shot.empty()) {
            SaveShot(shot);
            std::printf("First run: screenshot %s\n", shot.c_str());
            shot.clear();
        }
        SDL_SetRenderTarget(renderer, nullptr);
        if (scripted) SDL_Delay(16);
        else SDL_RenderPresent(renderer);
    }
    if (process) {
        SDL_KillProcess(process, true);
        if (reader.joinable()) reader.join();
        SDL_DestroyProcess(process);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (target) SDL_DestroyTexture(target);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf("First run: %s\n", finished ? "game folder chosen" : "quit");
    return finished ? 0 : 1;
}

} // namespace

extern "C" int bbgpu_first_run(const char* result_path) {
    FirstRun first_run(result_path);
    return first_run.Run();
}
