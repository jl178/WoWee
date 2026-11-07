# The interface's screen space on wide windows

How the 3.3.5a (build 12340) client sizes and places its glue and in-game
interface on a window wider than 16:9 (2560x1080 and the other 21:9 and 32:9
shapes), and how its interface text is rasterised. Read from `Wow.exe` 12340
with Ghidra (headless, the project in the session scratchpad `ui-re/`) and
from the 3.3.5a GlueXML/FrameXML. Each rule names the function, data or file
it came from.

The pure rules are `game::glue::glueParentRect` (`include/game/glue_rules.hpp`,
`tests/test_glue_rules.cpp`) and `WidgetTree::setHudAspect`
(`include/ui/widget_tree.hpp`, `tests/test_widget_tree.cpp` "[hud]").

## 1. The aspect the engine uses

| What | Where | Rule |
|---|---|---|
| Aspect source | 0x50f7c0 | `widescreen` CVar on and `gxResolution` set: `sscanf("%d%c%d")` of `gxResolution`, w / h. Otherwise 4 / 3. No clamp. |
| `widescreen` | 0x76a630 | registered "Allow widescreen support", default `"1"` (0x9e1464). |
| Mode list | 0x54f050 | with `widescreen` on, every mode wider than 640x480 with aspect >= 1.248 (0xa0c8c8) is offered; 21:9 and 32:9 modes included. |
| SetAspect | 0x47bf90 | stores a (0xac0cb0), `0.75 a` (0xac0cbc), DDC height `1 / sqrt(a^2 + 1)` (0xac0cb8) and width `a / sqrt(a^2 + 1)` (0xac0cb4). See world-ui.md section 1 for the DDC units. |
| Glue | CGlueMgr init 0x4da5f0 | `SetAspect(0x50f7c0())`, and the same aspect kept at 0xb6af84. |
| In game | 0x5126e0 | `SetAspect(0x50f7c0())`, kept at 0xbd07f8. |

So the engine itself never boxes anything: both the glue and the game
interface are laid out in a space 768 units tall and `768 a` units wide.

## 2. GetScreenWidth / GetScreenHeight

| Function | Glue table (0xac3e80) | Game table (0xac84f0) |
|---|---|---|
| GetScreenWidth | 0x4dcc20: DDC width (0x47c070) to units through 0x47bfe0 / 0x47c050 | 0x51aef0: the same, divided by the UIParent scale (`[0xbd0778] + 0x7c`, 1.0 with no UIParent) |
| GetScreenHeight | 0x4dcc70 | 0x51af50 |

With the scale 1 these are `768 a` and 768: the whole window, in glue and in
game alike.

## 3. Glue: GlueParent pillarboxes above 16:9

`GlueParent_OnLoad` (GlueXML `GlueParent.lua`, runs once at load):

```lua
local width = GetScreenWidth();
local height = GetScreenHeight();
if ( width / height > 16 / 9) then
    local maxWidth = height * 16 / 9;
    local barWidth = ( width - maxWidth ) / 2;
    self:ClearAllPoints();
    self:SetPoint("TOPLEFT", barWidth, 0);
    self:SetPoint("BOTTOMRIGHT", -barWidth, 0);
end
```

Every glue frame is a child of GlueParent - the login, realm list,
character select and create screens, their dialogs and the three ModelFFX
scenes - so the whole glue is a centred 16:9 area at the window's full
height; nothing is drawn outside it, which is the clear colour, black. At
2560x1080 the bars are 320 pixels each and GlueParent is 1920 wide. At 16:9
and narrower nothing happens and GlueParent fills the window (it is
`setAllPoints`). Because the ModelFFX frames are GlueParent-sized, their
projection (0x6bfe00, `vertical = diagonal fov / sqrt(1 + a^2)` with the
frame's own aspect) never sees anything wider than 16:9.

The loading screen is not GlueXML: it fits its own art (glue-screens.md
section 6, 0x40a270), and the pillarbox there is the art's 4:3 or 1.6.

## 4. In game: UIParent fills the window

FrameXML has no counterpart of GlueParent_OnLoad. `UIParent` is
`setAllPoints` with no parent - the whole window - and the default frames
anchor to its edges and centre: the action bars and the micro menu to its
BOTTOM, the player and target frames to its TOPLEFT, the minimap cluster to
its TOPRIGHT, the chat frame to its BOTTOMLEFT, the bags, the right action
bars and the default tooltip to its BOTTOMRIGHT, the UI panels to its
TOPLEFT at their `UIPanelWindows` offsets. On 21:9 the bars stay in the
middle and the corners go out to the window's corners: the stock stretched
layout. The only aspect-dependent FrameXML code is cosmetic:

- `CinematicFrame_OnLoad`: above 4:3 the cinematic bars are
  `(height - min(width / 2, height)) / 2` tall - none at 2:1 and wider.
- `WorldMapFrame_OnLoad`: the old BlackoutWorld sizing is commented out.
- `UIParent.lua` 2914 (`GetScreenWidth() / screenWidth`), the container
  frames' column wrap (`containerframe.lua` 549) and the tooltip and
  dropdown edge clamps measure against `GetScreenWidth()`, the whole window.

## 5. Interface text rasterisation

From `stock_font_metrics.hpp` (Wow.exe's own rules, documented there): a
font string's height is an em size; its pixel size is
`round(height x screenH x effective scale / 768)`; glyphs are rasterised by
FreeType without hinting, one sample per pixel, at `min(px, 32)`, and above
32 pixels the 32 pixel layout and bitmaps are scaled up by `px / 32`. So
stock text is drawn at its real pixel size up to 32 pixels (including at a
UI scale below 1), and stretched above.

## 6. Wowee, against the above

| Item | Before | Now |
|---|---|---|
| Glue above 16:9 | every glue screen, and its 3D scene, laid out across the whole window | GlueParent: `glue::GlueParentScope` (`glue_ui.hpp`) gives the glue screens GlueParent's size and pointer, moves what they draw into the centred 16:9 rect, clips to it and fills the bars black; the scenes are built at GlueParent's size, so their FBO and projection are 16:9 |
| Glue at 16:9 and narrower | the whole window | unchanged |
| In game | UIParent the whole window, frames to its edges | unchanged (stock); the ultrawide HUD is an enhancement row, N15 |
| `GetScreenWidth` | `768 a / scale` | unchanged |
| Text up to 32 px | real pixel size, stock pen | unchanged; Wowee bakes with ImGui's 2x horizontal oversampling (stock is one sample per pixel), a softer bitmap - left as the off path, see the gaps |
| Text above 32 px | 32 px bake stretched | unchanged; the sharp interface row (N14) bakes at the real size |

### Gaps

- **Text bitmaps.** Stock's FreeType bitmaps are one sample per pixel; the
  off path keeps Wowee's existing ImGui bake (two horizontal samples,
  filtered, plus `RasterizerMultiply` 1.35 for weight). The N14 row's bake
  (one sample, whole-pixel origins) is the closer of the two to stock's
  bitmaps; making it the off path is a decision for Jered, since it changes
  what "off" draws.
- **UI texture mips.** Not traced: whether CSimpleTexture's images carry the
  BLP's mips and sample them. The off path keeps one level, bilinear.
- **Engine-drawn world text** (nameplates, bubbles, world text) is sized in
  DDC (world-ui.md section 1) and is not moved by the ultrawide HUD.
