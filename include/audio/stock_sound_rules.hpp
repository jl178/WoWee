#pragma once

// How the stock 3.3.5a client (build 12340) plays a sound, as opposed to
// which sound it plays. Every number here is read out of Wow.exe; the
// addresses are in docs/re/sound-engine.md. Pure: no miniaudio, no assets,
// so the rules can be tested on their own and every manager can share them.

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace wowee {
namespace audio {
namespace stock {

// ---------------------------------------------------------------------------
// Channels (SoundEngine.cpp init 0x87c710, CVars 0x4d1050)
// ---------------------------------------------------------------------------

/// Sound_NumChannels: the voices FMOD actually mixes. The client asks FMOD
/// for 512 virtual channels and lets this many be real.
constexpr int kDefaultNumChannels = 32;
/// Init clamps the CVar to this range ("Requires At Least 12").
constexpr int kMinNumChannels = 12;
constexpr int kMaxNumChannels = 128;
/// Every SoundEntries kit gets two FMOD sound groups with setMaxAudible(3)
/// (0x4cbdb0 -> 0x878ad0). A fourth copy of one kit steals the least
/// important of the three (FMOD_SOUNDGROUP_BEHAVIOR_STEALLOWEST).
constexpr int kMaxAudiblePerKit = 3;
/// Sound_VirtualPlayWindow (looked up at 0x87a656, never registered, so the
/// default): a one-shot still virtual - out of the real voices - this long
/// after it started is stopped instead of waiting for a voice.
constexpr uint32_t kVirtualPlayWindowMs = 400;

// FMOD channel priorities: 0 most important, 256 least.
constexpr int kPriorityDefault = 128;       ///< props default -1 -> FMOD's 128
constexpr int kPriorityHeard = 127;         ///< a one-shot that has played real (0x87a580)
constexpr int kPriorityOwnFootstep = 115;   ///< the player's own steps (0x4cf170)
constexpr int kPriorityOwnCharacter = 110;  ///< sounds of the player's own unit
constexpr int kPriorityZone = 100;          ///< zone music and ambience (0x4c9910)

/// Sound categories: the "type" in the play properties (props[0]). The
/// first seven are FMOD channel groups by name; 7..17 are SFX with a cap on
/// how many may play at once across the whole client (table 0x9f2268,
/// checked in PlaySoundKit 0x4c6a40 before anything is started).
enum class Category : uint8_t {
    Sfx = 0,
    Music = 1,
    Ambience = 2,
    Cinematic = 3,
    ScriptSound = 4,
    ScriptMusic = 5,
    RacialCinematic = 6,      ///< cap 1
    ErrorSpeech = 7,          ///< cap 1 - VocalUISounds lines (0x986d00)
    UnitLoop = 8,             ///< cap 2 - a unit's own looping kit (0x746720)
    CreatureExertion = 9,     ///< cap 1 - attack grunts (0x747310 kinds 0, 1)
    WeaponSwing = 10,         ///< cap 2 - WeaponSwingSounds2 (0x4d0720)
    CreatureInjury = 11,      ///< cap 2 - wound cries (kinds 2, 3, 9)
    PlayerInjury = 12,        ///< cap 1 - the active player's own cries (0x7631a0)
    Footstep = 13,            ///< cap 6 - other units' steps (0x4cf170)
    WeaponImpact = 14,        ///< cap 4 - WeaponImpactSounds, $AH0..$AH3 (0x4d0540, 0x756240)
    CreatureExertionAlt = 15, ///< cap 1 - exertion of models with flag 0x20
    CreatureInjuryAlt = 16,   ///< cap 2 - injury of models with flag 0x20
    OwnFootstep = 17,         ///< cap 4 - the player's own steps
    Count = 18
};

/// How many sounds of a category may play at once; 0 for no limit.
int categoryCap(Category c);
/// The channel group (and so the volume slider) a category mixes into.
/// 7..17 are all "SFX".
Category volumeGroup(Category c);

// ---------------------------------------------------------------------------
// Distance (rolloff callback 0x878320 -> 0x8782b0, set3DSettings 0x87c710)
// ---------------------------------------------------------------------------

/// System::set3DSettings(doppler 1, distance factor 1, rolloff 4) and the
/// rolloff callback use the same 4.
constexpr float kRolloffScale = 4.0f;
/// The callback fades to silence linearly over the last tenth of the cutoff.
constexpr float kCutoffFadeStart = 0.9f;
/// MinDistance and DistanceCutoff when a sound has no row to give them:
/// the values half of SoundEntries uses (8 and 45).
constexpr float kDefaultMinDistance = 8.0f;
constexpr float kDefaultCutoff = 45.0f;

/// The gain the stock rolloff callback returns for a sound at `distance`
/// yards with SoundEntries MinDistance `minDistance` and DistanceCutoff
/// `cutoff` (FMOD's min and max distance):
///   1                                        inside MinDistance
///   min / (min + 4 * (d - min))              beyond it, FMOD's inverse
///                                            rolloff at scale 4
///   ... * (1 - (d - 0.9c) / (c - 0.9c))      over the last tenth of the cutoff
///   0                                        at and past the cutoff
float rolloffGain(float distance, float minDistance, float cutoff);

// ---------------------------------------------------------------------------
// Listener (WorldFrame 0x4fa5f0)
// ---------------------------------------------------------------------------

/// Sound_ListenerBackDist and Sound_ListenerUpDist are read but never
/// registered, so their defaults stand.
constexpr float kListenerBackDist = 2.0f;
constexpr float kListenerUpDist = 4.0f;

/// With Sound_ListenerAtCharacter (on by default) the listener stands at the
/// character's position, 2 yd back along the camera's view direction and
/// 4 yd up, facing where the camera faces. Velocity is always zero: no
/// Doppler anywhere (set3DAttributes is given a null velocity).
glm::vec3 listenerPosition(const glm::vec3& characterPosition, const glm::vec3& cameraForward);

/// Sounds of the player's own unit (voice, steps, emotes, model and spell
/// sounds - not its weapon swings, 0x4d0720 places those at the unit like
/// anyone's) play in 2D at this multiple of their volume while the listener
/// is at the character (0x9f2818). See SoundOptions::ownCharacter.
constexpr float kOwnCharacterVolume = 0.65f;

/// CreatureSoundData kinds (0x7461e0): 0 exertion, 1 exertion critical,
/// 2 injury, 3 injury critical, 4 stun, 5 stand, 7 wing flap, 8 alert,
/// 9 injury crushing blow, 10 wing glide, 11 jump start, 12 jump end.
enum class UnitSoundKind : uint8_t {
    Exertion = 0, ExertionCritical = 1, Injury = 2, InjuryCritical = 3, Stun = 4, Stand = 5,
    WingFlap = 7, Alert = 8, InjuryCrushingBlow = 9, WingGlide = 10, JumpStart = 11, JumpEnd = 12,
};
/// The chance in 100 that an unforced unit sound plays: a roll against
/// 0xadb5d8 for creatures (0x747310) and 0xadba74 for players (0x7631a0).
/// Exertion 70 / 35, injury 60 / 30, stand 40 for both, the rest always.
int unitSoundChance(UnitSoundKind kind, bool player);
/// The category a unit sound plays in (0x746b30): exertion 9, injury 11,
/// and for the active player's own injuries 12 (0x7631a0).
Category unitSoundCategory(UnitSoundKind kind, bool ownPlayer);

// ---------------------------------------------------------------------------
// Volumes (CVars 0x4d1600; channel volume 0x878fd0)
// ---------------------------------------------------------------------------

constexpr float kDefaultMasterVolume = 1.0f;
constexpr float kDefaultSfxVolume = 1.0f;
constexpr float kDefaultMusicVolume = 0.4f;
constexpr float kDefaultAmbienceVolume = 0.6f;

// ---------------------------------------------------------------------------
// SoundEntries
// ---------------------------------------------------------------------------

/// SoundEntries.Flags bits the engine reads (0x4c6a40). The 3.3.5 table
/// sets no others.
enum : uint32_t {
    kFlagNoDuplicate = 0x20,    ///< refused while the same kit is playing
    kFlagLoop = 0x200,          ///< loops unless the caller says otherwise
    kFlagRandomPitch = 0x400,   ///< frequency x uniform [0.85, 1.15)
    kFlagRandomVolume = 0x800,  ///< volume + uniform [-0.15, 0.15), then clamped
};
constexpr float kRandomPitchMin = 0.85f;
constexpr float kRandomPitchMax = 1.15f;
constexpr float kRandomVolumeSpread = 0.15f;

/// The volume a kit plays at: VolumeFloat times the caller's multiplier,
/// plus the random spread when the row asks for it, clamped to [0, 1].
float kitVolume(float rowVolume, uint32_t flags, float multiplier, std::mt19937& rng);
/// The playback-rate multiplier: 1, or uniform [0.85, 1.15) for 0x400 rows.
float kitPitch(uint32_t flags, std::mt19937& rng);

/// A kit's file bag (kit +0x70..+0x94, last pick at +0x44).
struct KitBag {
    std::array<uint32_t, 10> remaining{};
    int last = -1;
};

/// The file a kit plays next, the way the default selection mode (2) of
/// 0x4cbac0 picks it: each file sits in a bag Freq times; a draw takes one
/// out, weighted by what is left, never the file played last; an empty bag
/// refills from Freq. With every Freq 1 - nearly the whole table - a kit
/// plays each of its files once, in random order, before any repeats, and
/// never the same file twice running. Returns -1 when no file can be drawn
/// (every weight 0), when the stock client plays nothing.
int pickKitFile(KitBag& bag, const std::vector<uint32_t>& freq, std::mt19937& rng);

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

/// What the voice limiter knows of a playing sound.
struct Voice {
    int priority = kPriorityDefault;  ///< lower is more important
    float audibility = 1.0f;          ///< volume x distance gain
    uint32_t kitId = 0;               ///< SoundEntries id; 0 for a bare file
    Category category = Category::Sfx;
};

/// What to do with a sound that wants to start.
struct VoiceDecision {
    bool admit = true;
    /// Indexes into the active list of the voices to stop for it.
    std::vector<size_t> evict;
    enum class Reason : uint8_t { None, CategoryFull, Duplicate, NoVoice } reason = Reason::None;
};

/// Decide whether `incoming` may start beside `active`, the way the stock
/// engine and FMOD between them do:
///  1. its category is at its cap (0x9f2268): refused;
///  2. its kit has the no-duplicate flag and is playing: refused;
///  3. its kit already has three voices: the least important of them goes
///     (sound group STEALLOWEST);
///  4. every real voice is taken: the least important voice of all goes if
///     the newcomer outranks it (lower priority number, or the same and
///     louder); otherwise the newcomer is the one left without a voice and
///     is dropped, as the 400 ms virtual window would drop it.
/// `realVoices` is the number of voices the mixer may use for these sounds.
VoiceDecision decideVoice(const std::vector<Voice>& active, const Voice& incoming, int realVoices,
                          bool noDuplicate = false);

/// True when `a` is the less important of the two: the higher priority
/// number, or the same and quieter. FMOD's virtual voice order.
bool lessImportant(const Voice& a, const Voice& b);

// ---------------------------------------------------------------------------
// Time of day (0x4c9850)
// ---------------------------------------------------------------------------

/// Day is 05:30 up to 21:00 of the game clock; the rest is night. Zone music
/// and ambience swap their day and night entries at those two times.
bool isDaytime(float hours);

// ---------------------------------------------------------------------------
// Zone music and ambience (SoundInterface2ZoneSounds.cpp, 0x4c9910)
// ---------------------------------------------------------------------------

/// Music crossfade: the incoming track fades in over 4 s while the outgoing
/// one fades out over its own 4 s. A track that starts in silence fades in
/// over 0.01 s - at once.
constexpr float kMusicFadeSeconds = 4.0f;
constexpr float kMusicFirstFadeInSeconds = 0.01f;
/// Ambience loops fade in and out over 5 s.
constexpr float kAmbienceFadeSeconds = 5.0f;

}  // namespace stock
}  // namespace audio
}  // namespace wowee
