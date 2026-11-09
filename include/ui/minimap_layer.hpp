#pragma once

// Where the minimap lands among the interface's frames.
//
// In the client the minimap is a frame like any other: its map and its blips
// are drawn at the Minimap frame's strata and level, over its parent's art and
// under whatever sits above it. Here the map image and the blips are drawn by
// the client (the widget renderer and GameScreen's marker pass), and the
// interface's frames by the widget renderer - all into ImGui's background
// draw list, the blips long before the frames.
//
// So the background list is split into channels for the frame and merged in
// this order:
//
//   0  the world's own overlays (nameplates, names, combat text, bubbles)
//   1  interface frames that sort before the Minimap frame
//   2  the map image
//   3  the minimap blips
//   4  interface frames that sort after it
//
// The widget renderer, which knows the draw order before it draws, puts each
// frame in 1 or 4 and the map in 2; the marker pass writes into 3. Without a
// minimap frame on screen every interface frame goes to 4, which is where all
// of them were before: over the blips.
//
// A frame that never begins (the login screen) leaves every call a no-op.

#include <imgui.h>

namespace wowee::ui::minimap_layer {

enum Channel : int { kWorld = 0, kBeforeMinimap = 1, kMap = 2, kBlips = 3, kAfterMinimap = 4, kCount = 5 };

struct State {
    ImDrawListSplitter splitter;
    ImDrawList* list = nullptr;
};

inline State& state() {
    static State s;
    return s;
}

/// Split the background list for this frame. Call once ImGui's frame has begun.
inline void beginFrame() {
    State& s = state();
    if (s.list) return;
    s.list = ImGui::GetBackgroundDrawList();
    s.splitter.Split(s.list, kCount);
    s.splitter.SetCurrentChannel(s.list, kWorld);
}

inline bool active() { return state().list != nullptr; }

inline void use(Channel c) {
    State& s = state();
    if (s.list) s.splitter.SetCurrentChannel(s.list, c);
}

/// Merge the channels back, in order. Call before ImGui's frame is closed.
inline void endFrame() {
    State& s = state();
    if (!s.list) return;
    s.splitter.Merge(s.list);
    s.list = nullptr;
}

}  // namespace wowee::ui::minimap_layer
