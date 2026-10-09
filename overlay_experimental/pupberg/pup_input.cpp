#ifdef EMU_OVERLAY

#include "pupberg/pup_input.h"
#include "InGameOverlay/ImGui/imgui.h"

#include <chrono>
#include <vector>
#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#endif

namespace pupberg {

struct KeyMap
{
    uint32_t native; // X11 keysym or Windows VK
    ImGuiKey key;
    char lower;
    char upper;
};

#if defined(_WIN32)
#define PUP_K(x11, vk) vk
#else
#define PUP_K(x11, vk) x11
#endif

static const std::vector<KeyMap>& key_map()
{
    static std::vector<KeyMap> keys = [] {
        std::vector<KeyMap> k = {
            { PUP_K(XK_BackSpace, VK_BACK), ImGuiKey_Backspace, 0, 0 },
            { PUP_K(XK_Return, VK_RETURN), ImGuiKey_Enter, 0, 0 },
            { PUP_K(XK_Escape, VK_ESCAPE), ImGuiKey_Escape, 0, 0 },
            { PUP_K(XK_Delete, VK_DELETE), ImGuiKey_Delete, 0, 0 },
            { PUP_K(XK_Left, VK_LEFT), ImGuiKey_LeftArrow, 0, 0 },
            { PUP_K(XK_Right, VK_RIGHT), ImGuiKey_RightArrow, 0, 0 },
            { PUP_K(XK_Up, VK_UP), ImGuiKey_UpArrow, 0, 0 },
            { PUP_K(XK_Down, VK_DOWN), ImGuiKey_DownArrow, 0, 0 },
            { PUP_K(XK_Home, VK_HOME), ImGuiKey_Home, 0, 0 },
            { PUP_K(XK_End, VK_END), ImGuiKey_End, 0, 0 },
            { PUP_K(XK_Page_Up, VK_PRIOR), ImGuiKey_PageUp, 0, 0 },
            { PUP_K(XK_Page_Down, VK_NEXT), ImGuiKey_PageDown, 0, 0 },
            { PUP_K(XK_space, VK_SPACE), ImGuiKey_Space, ' ', ' ' },
            { PUP_K(XK_period, VK_OEM_PERIOD), ImGuiKey_Period, '.', '>' },
            { PUP_K(XK_minus, VK_OEM_MINUS), ImGuiKey_Minus, '-', '_' },
            { PUP_K(XK_Control_L, VK_LCONTROL), ImGuiMod_Ctrl, 0, 0 },
            { PUP_K(XK_Shift_L, VK_LSHIFT), ImGuiMod_Shift, 0, 0 },
        };
        for (int i = 0; i < 26; ++i) {
            k.push_back({ PUP_K((uint32_t)(XK_a + i), (uint32_t)('A' + i)), (ImGuiKey)(ImGuiKey_A + i), (char)('a' + i), (char)('A' + i) });
        }
        for (int i = 0; i < 10; ++i) {
            static const char shifted[] = ")!@#$%^&*(";
            k.push_back({ PUP_K((uint32_t)(XK_0 + i), (uint32_t)('0' + i)), (ImGuiKey)(ImGuiKey_0 + i), (char)('0' + i), shifted[i] });
        }
        return k;
    }();
    return keys;
}

static uint32_t toggle_key_native(InGameOverlay::ToggleKey k)
{
    using TK = InGameOverlay::ToggleKey;
    switch (k) {
    case TK::SHIFT: return PUP_K(XK_Shift_L, VK_SHIFT);
    case TK::CTRL:  return PUP_K(XK_Control_L, VK_CONTROL);
    case TK::ALT:   return PUP_K(XK_Alt_L, VK_MENU);
    case TK::TAB:   return PUP_K(XK_Tab, VK_TAB);
    default: break;
    }
    int f = (int)k - (int)TK::F1;
    if (f >= 0 && f < 12) return PUP_K((uint32_t)(XK_F1 + f), (uint32_t)(VK_F1 + f));
    return 0;
}

// ---------------------------------------------------------------------------
#if defined(_WIN32)

struct InputFallback::Impl
{
    std::vector<bool> prev_keys{};

    bool focused()
    {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }
    bool down(uint32_t vk) { return (GetAsyncKeyState((int)vk) & 0x8000) != 0; }
    bool combo_down(const std::vector<uint32_t> &keys)
    {
        if (keys.empty() || !focused()) return false;
        for (auto k : keys) if (!down(k)) return false;
        return true;
    }
    bool mouse(float &x, float &y, bool &l, bool &r, bool &m)
    {
        if (!focused()) return false;
        POINT p{};
        if (!GetCursorPos(&p)) return false;
        ScreenToClient(GetForegroundWindow(), &p);
        x = (float)p.x; y = (float)p.y;
        l = down(VK_LBUTTON); r = down(VK_RBUTTON); m = down(VK_MBUTTON);
        return true;
    }
    bool key_down(uint32_t native) { return focused() && down(native); }
    bool any_shift() { return down(VK_SHIFT); }
};

// ---------------------------------------------------------------------------
#else

struct InputFallback::Impl
{
    void *lib = nullptr;
    Display *combo_dpy = nullptr; // used by the worker thread only
    Display *input_dpy = nullptr; // used by the render thread only
    char input_keys[32]{};
    std::vector<bool> prev_keys{};

    decltype(&XOpenDisplay) pXOpenDisplay{};
    decltype(&XCloseDisplay) pXCloseDisplay{};
    decltype(&XQueryKeymap) pXQueryKeymap{};
    decltype(&XKeysymToKeycode) pXKeysymToKeycode{};
    decltype(&XQueryPointer) pXQueryPointer{};
    decltype(&XGetInputFocus) pXGetInputFocus{};
    decltype(&XTranslateCoordinates) pXTranslateCoordinates{};
    decltype(&XInternAtom) pXInternAtom{};
    decltype(&XGetWindowProperty) pXGetWindowProperty{};
    decltype(&XQueryTree) pXQueryTree{};
    decltype(&XFree) pXFree{};

    Impl()
    {
        lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return;
        bool ok = true;
        auto load = [&](auto &fn, const char *name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name)); ok &= fn != nullptr; };
        load(pXOpenDisplay, "XOpenDisplay");
        load(pXCloseDisplay, "XCloseDisplay");
        load(pXQueryKeymap, "XQueryKeymap");
        load(pXKeysymToKeycode, "XKeysymToKeycode");
        load(pXQueryPointer, "XQueryPointer");
        load(pXGetInputFocus, "XGetInputFocus");
        load(pXTranslateCoordinates, "XTranslateCoordinates");
        load(pXInternAtom, "XInternAtom");
        load(pXGetWindowProperty, "XGetWindowProperty");
        load(pXQueryTree, "XQueryTree");
        load(pXFree, "XFree");
        if (!ok) { dlclose(lib); lib = nullptr; return; }
        combo_dpy = pXOpenDisplay(nullptr);
        input_dpy = pXOpenDisplay(nullptr);
    }

    ~Impl()
    {
        if (combo_dpy) pXCloseDisplay(combo_dpy);
        if (input_dpy) pXCloseDisplay(input_dpy);
        if (lib) dlclose(lib);
    }

    // the focused X window (or one of its parents) belongs to this process
    Window focused_window(Display *d)
    {
        Window w = 0;
        int revert = 0;
        pXGetInputFocus(d, &w, &revert);
        if (w == None || w == PointerRoot) return 0;
        Atom pid_atom = pXInternAtom(d, "_NET_WM_PID", True);
        if (pid_atom == None) return w; // can't check, assume it's the game

        Window cur = w;
        for (int depth = 0; depth < 6 && cur; ++depth) {
            Atom type; int format; unsigned long count, after; unsigned char *data = nullptr;
            if (pXGetWindowProperty(d, cur, pid_atom, 0, 1, False, XA_CARDINAL, &type, &format, &count, &after, &data) == Success && data) {
                bool mine = count == 1 && (pid_t)(*(unsigned long *)data) == getpid();
                pXFree(data);
                return mine ? w : 0;
            }
            Window root = 0, parent = 0, *children = nullptr;
            unsigned int nchildren = 0;
            if (!pXQueryTree(d, cur, &root, &parent, &children, &nchildren)) break;
            if (children) pXFree(children);
            if (parent == root) break;
            cur = parent;
        }
        return 0;
    }

    static bool keymap_down(Display *d, decltype(&XKeysymToKeycode) k2c, const char keys[32], uint32_t keysym)
    {
        int code = k2c(d, keysym);
        return code > 0 && (keys[code / 8] & (1 << (code % 8)));
    }

    bool combo_down(const std::vector<uint32_t> &keys)
    {
        if (!combo_dpy || keys.empty()) return false;
        char km[32]{};
        pXQueryKeymap(combo_dpy, km);
        for (auto k : keys) if (!keymap_down(combo_dpy, pXKeysymToKeycode, km, k)) return false;
        return focused_window(combo_dpy) != 0;
    }

    bool mouse(float &x, float &y, bool &l, bool &r, bool &m)
    {
        if (!input_dpy) return false;
        Window w = focused_window(input_dpy);
        if (!w) return false;
        pXQueryKeymap(input_dpy, input_keys);
        Window root, child;
        int rx, ry, wx, wy;
        unsigned int mask = 0;
        if (!pXQueryPointer(input_dpy, w, &root, &child, &rx, &ry, &wx, &wy, &mask)) return false;
        x = (float)wx; y = (float)wy;
        l = mask & Button1Mask; m = mask & Button2Mask; r = mask & Button3Mask;
        return true;
    }

    // valid after mouse() refreshed input_keys this frame
    bool key_down(uint32_t native) { return input_dpy && keymap_down(input_dpy, pXKeysymToKeycode, input_keys, native); }
    bool any_shift() { return key_down(XK_Shift_L) || key_down(XK_Shift_R); }
};

#endif

// ---------------------------------------------------------------------------

InputFallback::InputFallback(std::vector<InGameOverlay::ToggleKey> combo, bool home_key, std::function<void()> on_combo) :
    impl(std::make_unique<Impl>()),
    combo(std::move(combo)),
    home_key(home_key),
    on_combo(std::move(on_combo))
{
    worker = std::thread(&InputFallback::worker_proc, this);
}

InputFallback::~InputFallback()
{
    stop = true;
    if (worker.joinable()) worker.join();
}

void InputFallback::worker_proc()
{
    std::vector<uint32_t> native{};
    for (auto k : combo) {
        uint32_t n = toggle_key_native(k);
        if (n) native.push_back(n);
    }

    const std::vector<uint32_t> home{ PUP_K(XK_Home, VK_HOME) };

    // start as pressed so nothing fires if the keys are held while the game starts
    bool prev = true, prev_home = true;
    while (!stop) {
        bool now = impl->combo_down(native);
        if (now && !prev && on_combo) on_combo();
        prev = now;

        if (home_key) {
            bool now_home = impl->combo_down(home);
            if (now_home && !prev_home && !typing && on_combo) on_combo();
            prev_home = now_home;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void InputFallback::feed_imgui()
{
    ImGuiIO &io = ImGui::GetIO();
    float x, y;
    bool l, r, m;
    if (!impl->mouse(x, y, l, r, m)) return;

    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, l);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, r);
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, m);

    const auto &keys = key_map();
    if (impl->prev_keys.size() != keys.size()) impl->prev_keys.assign(keys.size(), false);
    bool shift = impl->any_shift();
    for (size_t i = 0; i < keys.size(); ++i) {
        bool down = impl->key_down(keys[i].native);
        if (down != impl->prev_keys[i]) {
            io.AddKeyEvent(keys[i].key, down);
            // no text while ctrl is held, so ctrl+v pastes instead of typing a 'v'
            if (down && keys[i].lower && !io.KeyCtrl) io.AddInputCharacter((unsigned)(shift ? keys[i].upper : keys[i].lower));
            impl->prev_keys[i] = down;
        }
    }
}

}

#endif // EMU_OVERLAY
