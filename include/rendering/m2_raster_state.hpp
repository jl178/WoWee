// m2_raster_state.hpp - an M2 batch's cull and depth state from its render
// flags, as the stock client sets them (CM2Model state setup 0x81FE90;
// docs/re/m2-runtime.md #24).
#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {

struct M2RasterState {
    bool cullBack = false;   ///< back faces culled
    bool depthTest = true;
    bool depthWrite = true;
    bool operator==(const M2RasterState&) const = default;
};

/// The stock rule: render flag 0x04 two-sided, 0x08 no depth test, 0x10 no
/// depth write (GxRs states 0x11, 0x0D, 0x0F set from `~flags >> n & 1`). The
/// depth write follows the flag alone, whatever the blend: a blended layer
/// without 0x10 writes depth in stock. The sky model tests and never writes.
///
/// `legacy` is Wowee's old state (WOWEE_M2_LEGACY_RENDER_FLAGS): no culling,
/// depth always tested, written as the pass's pipeline says.
inline M2RasterState m2RasterStateForFlags(uint16_t materialFlags, bool skyMode,
                                           bool pipelineWritesDepth, bool legacy) {
    M2RasterState st;
    if (legacy) {
        st.cullBack = false;
        st.depthTest = true;
        st.depthWrite = pipelineWritesDepth && !skyMode;
        return st;
    }
    st.cullBack = (materialFlags & 0x04) == 0;
    st.depthTest = (materialFlags & 0x08) == 0;
    st.depthWrite = (materialFlags & 0x10) == 0 && !skyMode;
    return st;
}

}  // namespace rendering
}  // namespace wowee
