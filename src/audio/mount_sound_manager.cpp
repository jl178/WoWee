#include "audio/mount_sound_manager.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <random>

namespace wowee {
namespace audio {

namespace {
// Every sound here is the character's own, or its mount's - one unit to the
// stock client: in the head at 0.65 of its level with the listener at the
// character, priority 110 (SoundOptions::ownCharacter).
SoundOptions ownSound() {
    SoundOptions o;
    o.ownCharacter = true;
    return o;
}
}  // namespace

MountSoundManager::MountSoundManager() {
}

MountSoundManager::~MountSoundManager() {
    shutdown();
}

bool MountSoundManager::initialize(pipeline::AssetManager* assets) {
    assetManager_ = assets;
    if (!assetManager_) {
        LOG_WARNING("Mount sound manager: no asset manager");
        return false;
    }

    loadMountSounds();

    int totalSamples = wingFlapSounds_.size() + wingIdleSounds_.size();
    for (const auto& [family, sounds] : familySounds_) {
        totalSamples += sounds.move.size() + sounds.jump.size() +
                        sounds.land.size() + sounds.idle.size();
    }
    LOG_INFO("Mount sound manager initialized (", totalSamples, " clips, ",
             familySounds_.size(), " mount families)");
    return true;
}

void MountSoundManager::shutdown() {
    stopAllMountSounds();
    mounted_ = false;
    wingFlapSounds_.clear();
    wingIdleSounds_.clear();
    familySounds_.clear();
    assetManager_ = nullptr;
}

static void loadSoundList(pipeline::AssetManager* assets,
                          const std::vector<std::string>& paths,
                          std::vector<MountSample>& out) {
    for (const auto& path : paths) {
        if (!assets->fileExists(path)) continue;
        auto data = assets->readFile(path);
        if (data.empty()) continue;
        MountSample sample;
        sample.path = path;
        sample.data = std::move(data);
        out.push_back(std::move(sample));
    }
}

void MountSoundManager::loadMountSounds() {
    if (!assetManager_) return;

    // Flying mount wing flaps (movement)
    std::vector<std::string> wingFlapPaths = {
        "Sound\\Creature\\Dragons\\HugeWingFlap1.wav",
        "Sound\\Creature\\Dragons\\HugeWingFlap2.wav",
        "Sound\\Creature\\Dragons\\HugeWingFlap3.wav",
        "Sound\\Creature\\DragonWhelp\\mDragonWhelpWingFlapA.wav",
        "Sound\\Creature\\DragonWhelp\\mDragonWhelpWingFlapB.wav",
    };
    for (const auto& path : wingFlapPaths) {
        MountSample sample;
        if (loadSound(path, sample)) {
            wingFlapSounds_.push_back(std::move(sample));
        }
    }

    // Flying mount idle/hovering
    std::vector<std::string> wingIdlePaths = {
        "Sound\\Creature\\DragonHawk\\DragonHawkPreAggro.wav",
        "Sound\\Creature\\DragonHawk\\DragonHawkAggro.wav",
        "Sound\\Creature\\Dragons\\DragonPreAggro.wav",
    };
    for (const auto& path : wingIdlePaths) {
        MountSample sample;
        if (loadSound(path, sample)) {
            wingIdleSounds_.push_back(std::move(sample));
        }
    }

    // --- Per-family ground mount sounds ---

    // Horse
    {
        auto& s = familySounds_[MountFamily::HORSE];
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Horse\\mHorseAggroA.wav",
        }, s.move);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Horse\\mHorseAttackA.wav",
            "Sound\\Creature\\Horse\\mHorseAttackB.wav",
            "Sound\\Creature\\Horse\\mHorseAttackC.wav",
        }, s.jump);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Horse\\mHorseWoundA.wav",
            "Sound\\Creature\\Horse\\mHorseWoundB.wav",
        }, s.land);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Horse\\mHorseStand3A.wav",
        }, s.idle);
    }

    // Wolf
    {
        auto& s = familySounds_[MountFamily::WOLF];
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Wolf\\mWolfAggroA.wav",
            "Sound\\Creature\\Wolf\\mWolfAggroB.wav",
            "Sound\\Creature\\Wolf\\mWolfAggroC.wav",
        }, s.move);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Wolf\\mWolfAttackA.wav",
            "Sound\\Creature\\Wolf\\mWolfAttackB.wav",
            "Sound\\Creature\\Wolf\\mWolfAttackC.wav",
        }, s.jump);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Wolf\\mWolfWoundA.wav",
            "Sound\\Creature\\Wolf\\mWolfWoundB.wav",
            "Sound\\Creature\\Wolf\\mWolfWoundC.wav",
        }, s.land);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Wolf\\mWolfFidget2a.wav",
            "Sound\\Creature\\Wolf\\mWolfFidget2b.wav",
            "Sound\\Creature\\Wolf\\mWolfFidget2c.wav",
        }, s.idle);
    }

    // Ram
    {
        auto& s = familySounds_[MountFamily::RAM];
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Ram\\RamAggro.wav",
            "Sound\\Creature\\Ram\\RamPreAggro.wav",
        }, s.move);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Ram\\RamAttackA.wav",
            "Sound\\Creature\\Ram\\RamAttackB.wav",
            "Sound\\Creature\\Ram\\RamAttackC.wav",
        }, s.jump);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Ram\\RamWoundA.wav",
            "Sound\\Creature\\Ram\\RamWoundB.wav",
            "Sound\\Creature\\Ram\\RamWoundC.wav",
        }, s.land);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Ram\\RamPreAggro.wav",
        }, s.idle);
    }

    // Tiger (also saber cats / nightsaber)
    {
        auto& s = familySounds_[MountFamily::TIGER];
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Tiger\\mTigerAggroA.wav",
        }, s.move);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Tiger\\mTigerAttackA.wav",
            "Sound\\Creature\\Tiger\\mTigerAttackB.wav",
            "Sound\\Creature\\Tiger\\mTigerAttackC.wav",
            "Sound\\Creature\\Tiger\\mTigerAttackD.wav",
        }, s.jump);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Tiger\\mTigerWoundA.wav",
            "Sound\\Creature\\Tiger\\mTigerWoundB.wav",
            "Sound\\Creature\\Tiger\\mTigerWoundC.wav",
        }, s.land);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Tiger\\mTigerStand2A.wav",
        }, s.idle);
    }

    // Raptor
    {
        auto& s = familySounds_[MountFamily::RAPTOR];
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Raptor\\mRaptorAggroA.wav",
        }, s.move);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Raptor\\mRaptorAttackA.wav",
            "Sound\\Creature\\Raptor\\mRaptorAttackB.wav",
            "Sound\\Creature\\Raptor\\mRaptorAttackC.wav",
        }, s.jump);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Raptor\\mRaptorWoundA.wav",
            "Sound\\Creature\\Raptor\\mRaptorWoundB.wav",
            "Sound\\Creature\\Raptor\\mRaptorWoundC.wav",
        }, s.land);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Raptor\\mRaptorAggroA.wav",
        }, s.idle);
    }

    // Kodo
    {
        auto& s = familySounds_[MountFamily::KODO];
        loadSoundList(assetManager_, {
            "Sound\\Creature\\KodoBeast\\KodoBeastAggroA.wav",
        }, s.move);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\KodoBeast\\KodoBeastAttackA.wav",
            "Sound\\Creature\\KodoBeast\\KodoBeastAttackB.wav",
            "Sound\\Creature\\KodoBeast\\KodoBeastAttackC.wav",
        }, s.jump);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\KodoBeast\\KodoBeastWoundA.wav",
            "Sound\\Creature\\KodoBeast\\KodoBeastWoundB.wav",
            "Sound\\Creature\\KodoBeast\\KodoBeastWoundC.wav",
        }, s.land);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\KodoBeast\\KodoBeastStand02A.wav",
        }, s.idle);
    }

    // Mechanostrider
    {
        auto& s = familySounds_[MountFamily::MECHANOSTRIDER];
        loadSoundList(assetManager_, {
            "Sound\\Creature\\MechaStrider\\MechaStriderAggro.wav",
            "Sound\\Creature\\MechaStrider\\MechaStriderPreAggro.wav",
        }, s.move);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\MechaStrider\\MechaStriderAttackA.wav",
            "Sound\\Creature\\MechaStrider\\MechaStriderAttackB.wav",
            "Sound\\Creature\\MechaStrider\\MechaStriderAttackC.wav",
        }, s.jump);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\MechaStrider\\MechaStriderAttackA.wav",
        }, s.land);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\MechaStrider\\MechaStriderPreAggro.wav",
        }, s.idle);
    }

    // Tallstrider (plainstrider mounts)
    {
        auto& s = familySounds_[MountFamily::TALLSTRIDER];
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Tallstrider\\TallstriderAggro.wav",
            "Sound\\Creature\\Tallstrider\\TallstriderPreAggro.wav",
        }, s.move);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Tallstrider\\TallstriderAttackA.wav",
            "Sound\\Creature\\Tallstrider\\TallstriderAttackB.wav",
            "Sound\\Creature\\Tallstrider\\TallstriderAttackC.wav",
        }, s.jump);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Tallstrider\\TallstriderWoundA.wav",
            "Sound\\Creature\\Tallstrider\\TallstriderWoundB.wav",
            "Sound\\Creature\\Tallstrider\\TallstriderWoundC.wav",
        }, s.land);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\Tallstrider\\TallstriderPreAggro.wav",
        }, s.idle);
    }

    // Undead horse (skeletal warhorse)
    {
        auto& s = familySounds_[MountFamily::UNDEAD_HORSE];
        loadSoundList(assetManager_, {
            "Sound\\Creature\\HorseUndead\\HorseUndeadAggro.wav",
        }, s.move);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\HorseUndead\\HorseUndeadAttackA.wav",
            "Sound\\Creature\\HorseUndead\\HorseUndeadAttackB.wav",
            "Sound\\Creature\\HorseUndead\\HorseUndeadAttackC.wav",
        }, s.jump);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\HorseUndead\\HorseUndeadWoundA.wav",
            "Sound\\Creature\\HorseUndead\\HorseUndeadWoundB.wav",
            "Sound\\Creature\\HorseUndead\\HorseUndeadWoundC.wav",
        }, s.land);
        loadSoundList(assetManager_, {
            "Sound\\Creature\\HorseUndead\\HorseUndeadPreAggro.wav",
        }, s.idle);
    }

    // Log loaded families
    for (const auto& [family, sounds] : familySounds_) {
        int total = sounds.move.size() + sounds.jump.size() +
                    sounds.land.size() + sounds.idle.size();
        if (total > 0) {
            LOG_INFO("Mount family ", static_cast<int>(family), ": ",
                     sounds.move.size(), " move, ", sounds.jump.size(), " jump, ",
                     sounds.land.size(), " land, ", sounds.idle.size(), " idle");
        }
    }
}

bool MountSoundManager::loadSound(const std::string& path, MountSample& sample) {
    if (!assetManager_ || !assetManager_->fileExists(path)) {
        return false;
    }

    auto data = assetManager_->readFile(path);
    if (data.empty()) {
        return false;
    }

    sample.path = path;
    sample.data = std::move(data);
    return true;
}

const MountSoundManager::FamilySounds& MountSoundManager::getCurrentFamilySounds() const {
    static const FamilySounds empty;

    auto it = familySounds_.find(currentMountFamily_);
    if (it != familySounds_.end()) {
        return it->second;
    }

    return empty;
}

void MountSoundManager::update(float deltaTime) {
    if (!mounted_) {
        soundLoopTimer_ = 0.0f;
        return;
    }

    soundLoopTimer_ += deltaTime;
    updateMountSounds();
}

void MountSoundManager::setMoving(bool moving) {
    if (moving_ != moving) {
        moving_ = moving;
        if (mounted_) {
            updateMountSounds();
        }
    }
}

void MountSoundManager::setFlying(bool flying) {
    if (flying_ != flying) {
        flying_ = flying;
        if (mounted_) {
            updateMountSounds();
        }
    }
}

void MountSoundManager::setGrounded(bool grounded) {
    setFlying(!grounded);
}

void MountSoundManager::playRearUpSound() {
    if (!mounted_) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActionSoundTime_).count();
    if (elapsed < 200) return;
    lastActionSoundTime_ = now;

    if (currentMountType_ == MountType::GROUND) {
        const auto& sounds = getCurrentFamilySounds();
        if (!sounds.move.empty()) {
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<size_t> dist(0, sounds.move.size() - 1);
            const auto& sample = sounds.move[dist(rng)];
            if (!sample.data.empty()) {
                AudioEngine::instance().playSound2D(sample.data, 0.7f * volumeScale_, 1.0f, SoundBus::World, ownSound());
            }
        }
    } else if (currentMountType_ == MountType::FLYING && !wingIdleSounds_.empty()) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, wingIdleSounds_.size() - 1);
        const auto& sample = wingIdleSounds_[dist(rng)];
        if (!sample.data.empty()) {
            AudioEngine::instance().playSound2D(sample.data, 0.6f * volumeScale_, 1.1f, SoundBus::World, ownSound());
        }
    }
}

void MountSoundManager::playJumpSound() {
    if (!mounted_) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActionSoundTime_).count();
    if (elapsed < 200) return;
    lastActionSoundTime_ = now;

    if (currentMountType_ == MountType::GROUND) {
        const auto& sounds = getCurrentFamilySounds();
        if (!sounds.jump.empty()) {
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<size_t> dist(0, sounds.jump.size() - 1);
            const auto& sample = sounds.jump[dist(rng)];
            if (!sample.data.empty()) {
                AudioEngine::instance().playSound2D(sample.data, 0.5f * volumeScale_, 1.1f, SoundBus::World, ownSound());
            }
        }
    } else if (currentMountType_ == MountType::FLYING && !wingFlapSounds_.empty()) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, wingFlapSounds_.size() - 1);
        const auto& sample = wingFlapSounds_[dist(rng)];
        if (!sample.data.empty()) {
            AudioEngine::instance().playSound2D(sample.data, 0.4f * volumeScale_, 1.0f, SoundBus::World, ownSound());
        }
    }
}

void MountSoundManager::playLandSound() {
    if (!mounted_) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActionSoundTime_).count();
    if (elapsed < 200) return;
    lastActionSoundTime_ = now;

    if (currentMountType_ == MountType::GROUND) {
        const auto& sounds = getCurrentFamilySounds();
        if (!sounds.land.empty()) {
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<size_t> dist(0, sounds.land.size() - 1);
            const auto& sample = sounds.land[dist(rng)];
            if (!sample.data.empty()) {
                AudioEngine::instance().playSound2D(sample.data, 0.6f * volumeScale_, 0.85f, SoundBus::World, ownSound());
            }
        }
    }
}

void MountSoundManager::playIdleSound() {
    if (!mounted_ || moving_) return;

    if (currentMountType_ == MountType::GROUND) {
        const auto& sounds = getCurrentFamilySounds();
        if (!sounds.idle.empty()) {
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<size_t> dist(0, sounds.idle.size() - 1);
            const auto& sample = sounds.idle[dist(rng)];
            if (!sample.data.empty()) {
                AudioEngine::instance().playSound2D(sample.data, 0.35f * volumeScale_, 0.95f, SoundBus::World, ownSound());
            }
        }
    }
}

void MountSoundManager::updateMountSounds() {
    if (!AudioEngine::instance().isInitialized() || !mounted_) {
        return;
    }

    static std::mt19937 rng(std::random_device{}());

    // Flying mounts
    if (currentMountType_ == MountType::FLYING && flying_) {
        if (moving_ && !wingFlapSounds_.empty()) {
            if (soundLoopTimer_ >= 1.2f) {
                std::uniform_int_distribution<size_t> dist(0, wingFlapSounds_.size() - 1);
                const auto& sample = wingFlapSounds_[dist(rng)];
                std::uniform_real_distribution<float> volumeDist(0.4f, 0.5f);
                std::uniform_real_distribution<float> pitchDist(0.95f, 1.05f);
                AudioEngine::instance().playSound2D(
                    sample.data, volumeDist(rng) * volumeScale_, pitchDist(rng), SoundBus::World, ownSound());
                soundLoopTimer_ = 0.0f;
            }
        } else if (!moving_ && !wingIdleSounds_.empty()) {
            if (soundLoopTimer_ >= 3.5f) {
                std::uniform_int_distribution<size_t> dist(0, wingIdleSounds_.size() - 1);
                const auto& sample = wingIdleSounds_[dist(rng)];
                std::uniform_real_distribution<float> volumeDist(0.3f, 0.4f);
                std::uniform_real_distribution<float> pitchDist(0.98f, 1.02f);
                AudioEngine::instance().playSound2D(
                    sample.data, volumeDist(rng) * volumeScale_, pitchDist(rng), SoundBus::World, ownSound());
                soundLoopTimer_ = 0.0f;
            }
        }
    }
    // Ground mounts - use per-family sounds
    else if (currentMountType_ == MountType::GROUND && !flying_) {
        // Horse vocals are already driven semantically by MountFSM: rear-up uses
        // the aggro/whinny clip and the sparse idle timer uses the stand/snort clip.
        // Do not also run the generic 4.5/8-second ambient loop, which made a horse
        // whinny repeatedly while merely standing or travelling.
        if (currentMountFamily_ == MountFamily::HORSE ||
            currentMountFamily_ == MountFamily::UNDEAD_HORSE) {
            return;
        }
        const auto& sounds = getCurrentFamilySounds();
        if (moving_ && !sounds.move.empty()) {
            if (soundLoopTimer_ >= 8.0f) {
                std::uniform_int_distribution<size_t> dist(0, sounds.move.size() - 1);
                const auto& sample = sounds.move[dist(rng)];
                std::uniform_real_distribution<float> volumeDist(0.35f, 0.45f);
                std::uniform_real_distribution<float> pitchDist(0.97f, 1.03f);
                AudioEngine::instance().playSound2D(
                    sample.data, volumeDist(rng) * volumeScale_, pitchDist(rng), SoundBus::World, ownSound());
                soundLoopTimer_ = 0.0f;
            }
        } else if (!moving_ && !sounds.idle.empty()) {
            if (soundLoopTimer_ >= 4.5f) {
                std::uniform_int_distribution<size_t> dist(0, sounds.idle.size() - 1);
                const auto& sample = sounds.idle[dist(rng)];
                std::uniform_real_distribution<float> volumeDist(0.25f, 0.35f);
                std::uniform_real_distribution<float> pitchDist(0.98f, 1.02f);
                AudioEngine::instance().playSound2D(
                    sample.data, volumeDist(rng) * volumeScale_, pitchDist(rng), SoundBus::World, ownSound());
                soundLoopTimer_ = 0.0f;
            }
        }
    }
}

void MountSoundManager::stopAllMountSounds() {
    soundLoopTimer_ = 0.0f;
}

} // namespace audio
} // namespace wowee
