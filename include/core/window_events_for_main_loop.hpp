#pragma once
// window_events_for_main_loop.hpp - window events a loading screen's event
// pump must hand back to the main loop.

#include <SDL3/SDL.h>

#include <vector>

namespace wowee::core {

// Window events a loading screen's own event pump takes off the queue but
// must not swallow. The pumps in world_loader.cpp run instead of the main loop for as long
// as a map loads, and they only noted a resize's size: the swapchain was never
// marked for rebuilding and the interface was never told. On Wayland nothing
// else rebuilds it - presenting to a resized surface is not an error there -
// so a window the desktop resized during a load (another window tiling in
// beside it) kept drawing its old-sized image into the new size: the interface
// laid out for the new width and drawn over the old one, its right half off
// the image and the minimap stretched across it, until the window happened to
// return to the old size. These go back on the queue once the pump has
// drained it, so the main loop handles them as it handles any other.
struct WindowEventsForMainLoop {
    std::vector<SDL_Event> kept;
    WindowEventsForMainLoop() = default;
    WindowEventsForMainLoop(const WindowEventsForMainLoop&) = delete;
    WindowEventsForMainLoop& operator=(const WindowEventsForMainLoop&) = delete;
    void note(const SDL_Event& e) {
        if (e.type == SDL_EVENT_WINDOW_RESIZED ||
            e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
            e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
            e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            kept.push_back(e);
        }
    }
    ~WindowEventsForMainLoop() {
        for (SDL_Event& e : kept) SDL_PushEvent(&e);
    }
};

}  // namespace wowee::core
