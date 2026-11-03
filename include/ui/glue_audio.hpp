#pragma once

#include <string>
#include <string_view>

namespace wowee {
namespace ui {

/// The glue screens' sounds as GlueXML plays them: PlaySound for the
/// buttons, and PlayGlueAmbience / StopGlueAmbience for the loop under each
/// scene (Wow.exe 0x985fb0, 0x985f70). The glue music itself stays with
/// AuthScreen. Every call resolves its name through SoundEntries.dbc, read
/// here on first use since the glue screens come before the world loads it.
namespace glue_audio {

/// PlaySound(name).
void play(std::string_view soundName);

/// PlayGlueAmbience(name, fadeIn): loops the SoundEntries row, fading in.
/// The one already playing is kept when it is the same sound (the stock
/// call returns early for it), and otherwise stopped with a 3 s fade.
void playAmbience(std::string_view soundName, float fadeInSeconds);

/// The ambience for a glue scene ("UI_Orc"), GlueAmbienceTracks' row for
/// its key; nothing happens for a scene the table has no row for.
void playSceneAmbience(std::string_view sceneName);

/// StopGlueAmbience: fades the loop out over a second.
void stopAmbience(float fadeSeconds = 1.0f);

} // namespace glue_audio
} // namespace ui
} // namespace wowee
