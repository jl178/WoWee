#pragma once

/**
 * m2_asset_loader.hpp - reading an M2 and its .skin out of the asset tree.
 *
 * Separate from m2_loader.hpp on purpose: that file parses bytes and knows
 * nothing about where they came from, which is what lets it be tested on its own
 * without an asset tree to point at.
 */

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline {

class AssetManager;
struct M2Model;

/**
 * Read an M2 and the .skin beside it.
 *
 * Models from version 264 keep their submeshes in that separate file; earlier
 * ones carry them inside the M2 and it is not asked for. Returns false when the
 * M2 cannot be read or does not parse into a usable model.
 *
 * Four copies of this existed, one per thing that loads a weapon, a helm or a
 * portrait, and they disagreed about whether to name the model after its path -
 * which is what everything downstream identifies it by.
 */
bool loadM2WithSkin(AssetManager& assets, const std::string& m2Path, M2Model& outModel);

/**
 * The parsed models loadM2WithSkin keeps.
 *
 * Every weapon, helm and shoulder piece went through loadM2WithSkin each time
 * it was attached - every spawn of a guard wearing it, every time a player in
 * view drew or sheathed a sword - and each time the file was read, inflated
 * and parsed again on the main thread. The parse is now kept, by path, and a
 * later call copies it out. Thread-safe: a worker can warm it
 * (prefetchM2WithSkin) for the main thread to find.
 *
 * Bounded (least recently used goes first); a model that did not load is not
 * kept, so a missing file is asked about again as before.
 */
std::shared_ptr<const M2Model> loadM2WithSkinShared(AssetManager& assets, const std::string& m2Path);
/** Parse into the cache without copying out - for worker threads. */
void prefetchM2WithSkin(AssetManager& assets, const std::string& m2Path);
/** Drop every kept model (the asset tree changed). */
void clearM2ModelCache();
/** Models kept, and the cap. */
size_t m2ModelCacheSize();

/**
 * The external .anim file for one sequence: the model path without its
 * extension, then the animation id and the variation, zero-padded -
 * Character\Human\Male\HumanMale0097-00.anim.
 */
std::string animPathForM2(const std::string& m2Path, uint32_t animId, uint32_t variationIndex);

/**
 * Load the keyframes that live outside the M2.
 *
 * A sequence with flag 0x20 carries its data inside the model; the rest have it
 * in a file beside it, and a sequence whose file is missing simply does not
 * animate. `wantedAnimIds` limits the work to a few animations - loading every
 * external sequence of a character model stalls the frame, which is why the
 * paths that run during play name the three or five they actually need. An
 * empty list loads them all.
 *
 * `m2Data` is the original file bytes: the track headers the .anim data slots
 * into are still read from there.
 *
 * Six copies of this loop existed, three of which derived the base path by
 * chopping three characters off the model path.
 */
void loadExternalAnimations(AssetManager& assets, const std::string& m2Path,
                            const std::vector<uint8_t>& m2Data, M2Model& model,
                            std::initializer_list<uint32_t> wantedAnimIds = {});

}  // namespace pipeline
}  // namespace wowee
