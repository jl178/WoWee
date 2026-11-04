// A loading screen's event pump hands window events back to the main loop.
//
// The pumps in world_loader.cpp drain SDL's queue while a map loads. They used
// to drop a resize after noting its size, so the swapchain was never rebuilt
// and the interface was laid out for one width and drawn into another (the
// regression tour of 2026-11-04). This drives the same helper over a real SDL
// event queue: no window is needed, only the events subsystem.
#include "core/window_events_for_main_loop.hpp"

#include <catch_amalgamated.hpp>

#include <SDL3/SDL.h>

#include <vector>

using wowee::core::WindowEventsForMainLoop;

namespace {

struct EventsSubsystem {
    bool ok = SDL_InitSubSystem(SDL_INIT_EVENTS);
    ~EventsSubsystem() {
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        SDL_QuitSubSystem(SDL_INIT_EVENTS);
    }
};

SDL_Event windowEvent(SDL_EventType type, int w = 0, int h = 0) {
    SDL_Event e{};
    e.type = type;
    e.window.windowID = 7;
    e.window.data1 = w;
    e.window.data2 = h;
    return e;
}

// One pass of a loading screen's pump, as world_loader.cpp writes it.
void pumpOnce() {
    SDL_Event event;
    WindowEventsForMainLoop keepForMainLoop;
    while (SDL_PollEvent(&event)) {
        keepForMainLoop.note(event);
    }
}

std::vector<SDL_Event> drain() {
    std::vector<SDL_Event> out;
    SDL_Event e;
    while (SDL_PollEvent(&e)) out.push_back(e);
    return out;
}

}  // namespace

TEST_CASE("a resize during a load reaches the main loop", "[loading_event_pump]") {
    EventsSubsystem sdl;
    REQUIRE(sdl.ok);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    SDL_Event resized = windowEvent(SDL_EVENT_WINDOW_RESIZED, 625, 986);
    SDL_Event pixels = windowEvent(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, 625, 986);
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    REQUIRE(SDL_PushEvent(&resized));
    REQUIRE(SDL_PushEvent(&key));
    REQUIRE(SDL_PushEvent(&pixels));

    // Several frames of loading screen, as a long load has.
    for (int frame = 0; frame < 3; ++frame) pumpOnce();

    const auto left = drain();
    REQUIRE(left.size() == 2);
    CHECK(left[0].type == SDL_EVENT_WINDOW_RESIZED);
    CHECK(left[0].window.data1 == 625);
    CHECK(left[0].window.data2 == 986);
    CHECK(left[1].type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
}

TEST_CASE("input during a load is still the pump's to drop", "[loading_event_pump]") {
    EventsSubsystem sdl;
    REQUIRE(sdl.ok);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    REQUIRE(SDL_PushEvent(&key));
    REQUIRE(SDL_PushEvent(&motion));
    pumpOnce();
    CHECK(drain().empty());
}
