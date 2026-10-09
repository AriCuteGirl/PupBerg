#ifndef __INCLUDED_PUP_INPUT_H__
#define __INCLUDED_PUP_INPUT_H__

#ifdef EMU_OVERLAY

#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

#include "InGameOverlay/RendererHook.h"

namespace pupberg {

// Some games read their input in a way the InGameOverlay hooks never see
// (ex: SDL3 / sdl2-compat on Linux), so the toggle combo and the overlay input never arrive.
// This polls the keyboard/mouse state directly as a fallback:
//  - a worker thread watches the toggle combo and calls `on_combo` on its rising edge
//  - feed_imgui() pushes mouse + basic keys into ImGui while the overlay is shown
class InputFallback
{
public:
    // home_key: the Home key alone also toggles the overlay (when not typing in a text field)
    InputFallback(std::vector<InGameOverlay::ToggleKey> combo, bool home_key, std::function<void()> on_combo);
    ~InputFallback();

    // call from the render thread, inside the overlay frame
    void feed_imgui();
    // set from the render thread, the Home key is ignored while a text field has focus
    void set_typing(bool typing) { this->typing = typing; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
    std::vector<InGameOverlay::ToggleKey> combo{};
    bool home_key = false;
    std::atomic<bool> typing = false;
    std::function<void()> on_combo{};
    std::atomic<bool> stop = false;
    std::thread worker{};

    void worker_proc();
};

}

#endif // EMU_OVERLAY

#endif // __INCLUDED_PUP_INPUT_H__
