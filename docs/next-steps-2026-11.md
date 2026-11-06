# Next steps after the Nov 2-6 week (written 2026-11-06)

Branch `audio-data-driven`. Sources: `week-plan-2026-11.md` (final summary
and the Nov 6 decisions), `enhancements-plan.md`, every `docs/re/*.md`,
`regression-tour-2026-11-04.md`, `performance.md` and
`~/workplace/wowpack/docs/status-2026-11.md`. Items marked "(code)" were
checked against the source tree on Nov 6, not only against a spec.

Sizes: **S** under a day, **M** one to three days, **L** more.
Value is how much the item changes the feel of play: **H** a player notices
it in the first hour, **M** a player notices it in some places, **L** an
edge case or something only an addon or a log would see.

Rules that still hold: no PRs outside jl178; builds `nice -n 19 ionice -c3`,
-j4, one at a time; never touch Jered's client HOME
(`~/workplace/wowee-jtrain-home`) or his desktop windows; stage commits by
file list; each commit gets build, ctest, sweep_guard and a smoke login.

## 0. Decisions now in force (Nov 6)

- Normal maps ("Surface bumps") and parallax: off in the recommended set
  and every preset. Still rows on the Enhancements page.
- Torch and lamp light (`locallights`): stays on in the recommended set.
  Goldshire's forest view stays over the 3 ms budget by choice.
- Wowee soundtrack: off by default.
- Action bars cast on key-down (`ActionButtonUseKeyDown` defaults to 1).
  `/console ActionButtonUseKeyDown 0` gives the stock key-up.
- Disenchant asks nothing, as stock.
- View distance default 1900, Classic included; slider 400-2400.
- Water sky reflection: an option, off by default.
- Valgarde child display 19092: stays stock (drawn white).
- **Jered's feedback:** the shadow and occlusion enhancements (sun shadows,
  shadow edges, contact shadows) looked worse and dropped many frames on
  his machine. That rework is in progress in another session. Until it
  lands, no other work touches `shadow_filter.glsli`, `shadow_cache.hpp`,
  `contact_shadows.*`, `Renderer::renderShadowPass` or the shadow rows.
  Every cost figure in `enhancements-plan.md` is from the Iris Xe test box;
  his machine behaves differently, so new rows need a measurement there
  (through a build he runs, not his HOME).

Still open for Jered: upstream PRs or delete `wowpack-sync`; the stock
`Wow.exe` test of `patch-Z.MPQ`; deploying the IP and City Siege packs;
memory budgets (smaller caches, or empty the world caches at logout).

## 1. Stock fidelity backlog

Grouped by area, deduplicated across specs. Each line: item, size, value,
source.

### 1.1 Stability and release (do first)

| Item | Size | Value | Source |
|---|---|---|---|
| ASan soak (stopped by the usage limit Nov 4): long tour under ASan, then the same under `WOWEE_VULKAN_GPU_VALIDATION=1` with a loop of logins | M | H | week plan, regression tour |
| Windows package of HEAD (last one is 38b3106, Nov 3); clang check stays green | S | H | week plan |
| Logout memory: caches sized from free RAM never empty; RSS climbs to 9.5-9.9 GB. Needs Jered's choice (smaller budgets or empty world caches at logout) | S-M | M | regression tour |
| Retired stock option pages (Camera, Combat, Names, Combat Text, Sound) still listed by name, so `InterfaceOptionsFrame_OpenToCategory("Combat")` shows nothing | S | L | regression tour |
| Instance joins "General - The Deadmines"; stock joins no zone channel in an instance. Verify after the chat second pass, fix if still there | S | M | regression tour |

### 1.2 Game objects (largest visible gap)

| Item | Size | Value | Source |
|---|---|---|---|
| GO sounds: keep `$GO0-5`/`$GC0-3` in `isM2SoundEventTag`, fire them from `M2Renderer` for GO instances, play GameObjectDisplayInfo.Sound[n]. Doors, chests, mailboxes are silent today | M | H | sound-engine 11.4 |
| GO animations: door open/close, chest open, boat and elevator states from GO state and anim flags (not specced yet; RE the GO state to sequence rule first) | M | H | week plan "not done" |
| GO loot: drop `autoStoreGameObjectLoot` and the delayed `CMSG_LOOT` probes; stock sends USE and waits | S | M | interactions §10 |
| GO quest giver status not tracked in the quest status table (cursor and marker) | S | M | interactions §10 |

### 1.3 Characters and units

| Item | Size | Value | Source |
|---|---|---|---|
| Ghost tint fades in over 1000 ms; on a mount stock puts tint and alpha on the mount | S | M | characters T3, death-ghost-loot T1 |
| Interior unit light: per-unit MOCV ray under the feet (needs a WMO query) | M | H | characters L4 |
| Terrain-shadow darkening of units and the weather factor ×(1-0.35w) | S | M | characters L11 |
| Composite: underwear conditions, dest alpha 255, guild emblem on tabards | M | M | characters C6 |
| Hand grip attachment and quivers | S | M | characters A7 |
| Point lights: chosen per object not per pixel, animated intensity, lights carried by CharacterRenderer models (an NPC's torch) | M | M | characters L3 |
| Unlit M2Color RGB on characters | S | L | characters L6 |
| Blob: drape on the world's triangles, liquids as receivers, CreatureModelData GeoBox, HAS_NO_SHADOW_BLOB, Submerged; the shadow pass still runs with shadows off | M | M | characters L10 (wait for the shadow rework) |
| Name text depth: WMO collision query only; terrain and M2 do not hide names | S | M | world-ui §8 |
| Status prefixes, summon owner titles on names | S | L | world-ui §8 |
| Pet vs guardian by 0x71b600; owner by CHARMEDBY then CREATEDBY | S | L | world-ui §8 |
| Nameplate stacking walk 0x615e10; plate cast bar hold-and-fade | S | M | world-ui §8 |
| World text types 4, 5, 8/9, 10 (threat); the top-of-view clamp | S | L | world-ui §8 |
| Bubble colours from the player's ChatTypeInfo recolours | S | L | world-ui §8 |
| Red name after damage only for damage this client logs | S | L | world-ui §8 |

### 1.4 Animation and unit motion

| Item | Size | Value | Source |
|---|---|---|---|
| Emote gating by EmoteFlags (0x4F5410); emote sheath kept local (no CMSG_SETSHEATHED) | S | M | animation §6 |
| Facing a channel object while channelling; emote-state 0x2000 exception | S | L | animation §6 |
| Swing timer measured on variation 0 cuts a longer rolled variation | S | M | animation §6 |
| Stun from the aura's state kit, not `setStunned` | S | L | animation §6 |
| Stand states 9 (Submerge/Emerge) and 10 (Birth) | S | L | animation §6 |
| Flying spline bank and spine/head twist for remote flyers | S | M | unit-motion §7 |
| Tier transition anims ToFly 458, ToGround 460, FlyToGround 463 | S | M | unit-motion §7 |
| Remote dead reckoning through world collision (walls stop a runner) | M | M | unit-motion §7 |
| Phase carry for remote units; flags2 0x400/0x800/0x1000 acted on | S | L | unit-motion §7 |
| Upper-body swing half-speed rule on track 0; Auto Shot turn-in-place shuffle | S | L | combat-targeting §5 |
| Blow with no events still plays its reaction (kept on purpose); 4x-reach self-stop vs Wowee's 40 yd leash | S | L | combat-targeting §5 |
| Loot hold: loop vs hold of a finished one-shot | S | L | interactions §10 |

### 1.5 M2 runtime and particles

| Item | Size | Value | Source |
|---|---|---|---|
| Animated colour RGB per instance (needs a per-instance tint) | M | M | m2-runtime #18 |
| UV rotation and scale (full texture matrix; SSBO has a vec2 only) | M | M | m2-runtime #19, #30 |
| Flag 0x01 sequences on doodads: play N laps then hold | S | M | m2-runtime #20 |
| Variation re-roll per lap on loops (CGUnit callback) | S | L | m2-runtime #21 |
| Render flags 0x04/0x08/0x10: cull, depth test, depth write per batch | M | M | m2-runtime #24 |
| Transparent order: distance, then priorityPlane, then layer; particles sorted with the model's transparent batches | M | H | m2-runtime #25, particles #43 |
| Blend of non-bone tracks during a cross-fade | S | L | m2-runtime #12 |
| Weapon attachment FX batches (blend ≥ 3) on attached weapons | S | M | m2-runtime #31 |
| Baked-path skip threshold 0.01 vs stock 1e-4; opaque return above 0.99999 | S | L | m2-runtime #16, #17 |
| Global sequence clock as a float loses ms after ~4.6 h | S | L | m2-runtime #4 |
| Particle ground snap 0x2000, model particles, child emitters | M | M | particles #45 |
| Spline emitters (approximated) and Blizzard RNG (statistical only) | M | L | particles #46, #47 |
| Ribbon blends 0, 1, 5, 6 (only alpha and additive pipelines) | S | M | world-lighting §9 |
| M2 Mod/Mod2x batches fog to white / 0x80 (check after #22 fix) | S | L | world-lighting §9 |
| Doodad distance fade per size class (tables 0xADF38C..0xADF3DC) and the 10 yd / flag 0x400 rule. **(code)** Wowee uses its own `m2InstanceMaxDistSq` with a 0.55 or `fadeStartFraction` start | M | H | m2-runtime §4 note, new |

### 1.6 World lighting, sky, weather, screen effects

| Item | Size | Value | Source |
|---|---|---|---|
| Mist sheets (SnowMist01 past weather level 0.5) | S | M | world-lighting §8 |
| Weather.dbc colour and storm scale | S | M | world-lighting §8 |
| Lightning dome flash tint (stock) before any bolt enhancement | S | M | world-lighting §9, U17 |
| Timed screen tint `0xd38b50` | S | L | world-lighting §9 |
| ScreenEffect types 2/3 (nether/special colour wash) | S | M | world-lighting, death-ghost G2 |
| Underwater glow wave distortion | S | M | world-lighting §8 |
| Drunk: FOV widening, bubbles (slurred chat done) | S | L | world-lighting §8 |
| Sky models drawn after sun, moons and clouds | S | M | world-lighting §9 |
| Cloud noise from the client's random table; churn speed | S | L | world-lighting §9 |
| Blue Child (needs a server day count or a stock rule) | S | L | world-lighting §9 |
| Light pick: hard-coded light rectangles 0x77eed0; SMSG_OVERRIDE_LIGHT list; drunk and other ScreenEffect light slots | M | M | world-lighting §9 |
| Fog constant mapping (c12/c30/c4) traced, not inferred | S | L | world-lighting §9 |
| WMO MFOG and the second fog set (flag 0x8000) | M | M | wmo-rendering §9 |
| WMO env term under the shadow; ConstantAlpha blend 11; hard-coded Stormwind stair water mask | S | L | wmo-rendering §9 |

### 1.7 Terrain and water

| Item | Size | Value | Source |
|---|---|---|---|
| Crystalsong cube-map layers (MCLY 0x400 + MTXF bit 0) | M | M | terrain §11 |
| Horizon occluder test 0x7cce00; WDL WMO doodads | S | L | terrain §11 |
| Alpha resolution option 32x32 for the lower presets | S | L | terrain §11 |
| Streaming shape: stock chunk square vs Wowee tile circle | M | L | terrain §11 |
| Terrain liquid camera test: ground under the eye, WMO Flags 0x4 allowance | S | M | water §8 |
| Sun/moon fade over the first 10 yd under water | S | L | water §8 |
| Green Lava for MCLQ magma on map 530 | S | L | water §8 |
| Ramp 8.8 fixed point; mote scale on liquid change; motes drawn with the world | S | L | water §8 |
| Swim strokes and water-entry splash: stock trigger not found; hand-made sounds remain | M | M | sound-engine 11.4 |

### 1.8 Movement and camera

| Item | Size | Value | Source |
|---|---|---|---|
| Mouse-look scale: trace the event delta units (0x6020B0, 0x47C020) and map the sensitivity slider to `cameraYawMoveSpeed`/`cameraPitchMoveSpeed` | S | H | movement-camera §11 |
| Step mechanics: step only when a wall blocks; `kMaxStepUp` 0.6 tolerance | M | M | movement-camera §11 |
| Box vs cylinder collider; doodad test fixed 2 yd height | M | M | movement-camera §11 |
| Swim ledge exits: swimmer leaves water only by the hop | M | M | movement-camera §11 |
| Steep ground: FALLING and the box slide, not Wowee's terrain slide | M | M | movement-camera §11 |
| Air keys after walking off an edge (HEARTBEAT with FALLING) | S | L | movement-camera §11 |
| No FALL_LAND for a landing within 1/36 yd of the apex | S | L | movement-camera §11 |
| cameraDive and the surface/submerge pitches; first-person model fade | S | M | movement-camera §10 |
| Click-to-move (`autoInteract`) | M | L | movement-camera, interactions |
| Vehicle FIXED_POSITION facing limits (turrets) | M | M | mounts-taxi §6 |
| Mouselook drives vehicle aim | S | M | mounts-taxi §6 |
| Vehicle camera entering/exiting blend, CameraFacingChaseRate; verify the Skytalon full fade at default zoom | S | M | mounts-taxi §6 |
| Trajectory launch from the missile attachment, jitter, doodad hits | S | L | mounts-taxi §6 |
| Vehicle UI sounds (EnterUISoundID/ExitUISoundID) played | S | L | mounts-taxi §6 |
| Taxi: bank roll sign, camera height from Fly bounds, faction gate on nodes | S | L | mounts-taxi §6 |
| Mount wing glide `$WGG` (glide column in CreatureSounds) | S | L | mounts-taxi §6 |
| Bindings that call undefined functions: `ToggleAutoRun`, `NextView`, `PrevView`, `SetView`, `SaveView`, `ResetView`, `FlipCameraYaw`, pitch keys (45 names, most debug) | S | M | regression tour |

### 1.9 Spells, input, combat

| Item | Size | Value | Source |
|---|---|---|---|
| Ground target under a ship (transport guid in the point) | S | L | spell-cast §14 |
| SPELLMOD_RADIUS and range mods on the circle; circle during a cast | S | M | spell-cast §14 |
| Summon preview model 0xD3F570 | S | L | spell-cast §14 |
| Unit-flag pending spells (heal with no target, self-cast off); `SpellTargetUnit` | S | M | spell-cast §14 |
| CharProc 6/15/16/17; item missile ItemVisual; DO_NOT_SHEATH flag 0x100; queued kits 0x1; MissileFollowGround units; missile offsets | M | L | spell-cast §14, spell-audit |
| Lit lightning bolt emissive | S | L | spell-audit |
| Pad buttons still cast from GameScreen's poll (move to the binding path) | S | M | input-timing §7 |
| SPELLMOD_GLOBAL_COOLDOWN on the predicted GCD; category cooldowns in the client check | S | M | input-timing §7 |
| Client refusals fire `UNIT_SPELLCAST_FAILED`; fizzle for other units | S | M | input-timing §7 |
| Event tokens vehicle, partypet, raid, arena, boss (also `unitTokensForGuid`) | S | M | input-timing §7, names §8 |
| CanAttack/CanAssist: vehicle as reference, commentator rule | S | L | input-timing §7 |
| `SpellStopCasting` first branch; SitStandOrDescendStart stand-up | S | L | input-timing §7 |
| Melee 8 yd pre-check and Charge checks are Wowee's own | S | L | input-timing §7 |
| Shapeshift: `autoUnshift` on NPC talk or object use | S | M | interactions §10 |
| NPC emote state stops while talking (0x7171C0) | S | L | interactions §10 |
| Friendly units with no NPC flags: no attack fallback, "clicked" sound | S | M | interactions §10 |
| Tutorial and 0x73AD70 emote on NPC interact | S | L | interactions §10 |
| Vehicle boarding and `CMSG_SPELLCLICK` tested on AzerothCore | S | M | interactions §10 |

### 1.10 UI, chat, items, flows

| Item | Size | Value | Source |
|---|---|---|---|
| WoWCombatLog.txt (`LoggingCombat`) | S | L | chat C.8 |
| Combat log: building damage, UNIT_DESTROYED by totem slot, missile GO miss timing, CAST_FAILED 0x81 text, realm suffix, wait for names instead of "Unknown" after 3 s | M | L | chat C.8 |
| Talk animation on own model and mounted units | S | L | chat §7 |
| Joined-channel mask per character | S | M | chat §7 |
| Text emote sound index; dance studio path | S | L | chat §7 |
| Idle sit, AFK, 30-minute logout (0x52AF40) | S | M | chat §7 |
| Follow faction test via FactionTemplate | S | L | chat §7 |
| Right-click equip lifts the item while AUTOEQUIP_BIND is up | S | L | items gaps |
| Prospect/mill client preconditions; END_REFUND for sockets; TRADE_REPLACE_ENCHANT | S | L | items gaps |
| IsUsableItem noMana; GetSpellCooldown waiting on its event | S | M | items gaps |
| Ammo dropped over the world goes back | S | L | items gaps |
| Mail letter reading checked in game | S | L | items gaps |
| Pet spell targeting cursor (0x805D70); `Sound_EnablePetSounds` | S | M | pets §11 |
| Pet runs not done: DK ghoul, spirit wolves, first-tame rename, Feed Pet | S | L | pets §11 |
| Professions: stat lines in search; rebuild count; CloseTradeSkill event; linked-name timing | S | L | professions gaps |
| Skinning exercised in game | S | M | professions gaps |
| Minimap: indoor group by the portal walk, not height; building rule with root flag 0x400; vehicle icons; POI slot 0 and guide arrow; tracking order; off-map party check | M | M | maps §2 |
| World map: gossip POI polling; .zmp edge | S | L | maps §3.5 |
| Name cache flight limit 128; status 2/3; declined and realm names; `UnitName` of objects, items, corpses; totem slot refresh | S | L | names §8 |
| PvP: arena opponent stats block, arena roster sort, faction byte watch, dynamic-difficulty lines, instance reset state (`CanShowResetInstances`), lock counts, scoreboard tie order, flag carrier order | M | L | pvp §7 |
| Explore-objective completion text; LFG requeue on `UPDATE_STATUS` | S | L | gameplay-flows §19 |
| Glue: pillarboxing above 16:9 (needs a glue frame rect through every glue layout and hit test) | M | M | glue-screens |
| Glue: preview glow pass; ghost extras in the preview; loading screen taxi variant and A/B progress split | S | L | glue-screens |
| Stand sound (40%, 10 s spacing); AI reaction 0 Alert | S | L | sound-engine §8 |

### 1.11 New RE areas, not yet specced

Each was checked against the code on Nov 6 where noted. Order is by value.

| Area | What to find | Size | Value | State in code |
|---|---|---|---|---|
| Doodad and unit draw distance | Size-class fade tables, the unit visibility fade on create and on leaving range, `farclip`-driven cull of small doodads, LOD switch distances for M2 skins | M | H | **(code)** Wowee's own max distance and fade fraction |
| Game object state to animation | GO state (`GO_STATE_*`), anim progress, door/chest/elevator/boat sequences, transport GO animation clocks | M | H | **(code)** not done (week plan) |
| Camera collision details | What collides (terrain, WMO, M2 collision, water), the ray set, near-plane radius, `cameraWaterCollision`, pull-in on doodads, the first-person fade | M | H | partly in movement-camera §10; first-person fade and dive not traced |
| Unit selection and highlight | Mouseover highlight strength and timing, selection circle for corpses, GOs and players, the circle draw order with water | S | M | target circle stock; mouseover rule only partly specced |
| Cursor and tooltip timing | Tooltip show delay, fade-out after leaving a unit (stock fades), `TOOLTIP_UPDATE_TIME`, cursor change latency over moving units | S | M | **(code)** one reference to TOOLTIP_UPDATE_TIME; no fade spec |
| Key repeat | Which bindings repeat while held (camera zoom, turn), edit-box repeat rate, repeat while the game is unfocused | S | M | **(code)** repeat events are filtered in a few places; no stock rule |
| Macro conditionals | Full stock list and order: `button`, `actionbar`/`bar`, `bonusbar`, `equipped`/`worn`, `flyable`, `modifier:` alias, `stance:n`/`form:n` edge cases, `pet:name`, `unithasvehicleui`, `cursor`, `channeling:spell`, all unit tokens in `@`/`target=` (party1, targettarget, arena1) | M | H | **(code)** `macro_evaluator.cpp` covers about 45 words; there is also a second Lua `SecureCmdOptionParse` in `lua_engine.cpp` - one of the two should go |
| Aura display and cancel rules | Buff and debuff order, the 40/16 caps, which auras are hidden, `CancelUnitBuff` limits (in combat, debuffs, shapeshift via buff), timer text format | S | M | **(code)** API exists; rules not specced |
| Totems | Totem bar slots, destroy by right-click, the timer, the totem name events, PLAYER_TOTEM_UPDATE on name arrival | S | M | **(code)** GetTotemInfo exists; refresh gap in names §8 |
| Talent and glyph UI edge cases | Preview talents, respec dual spec, glyph socket unlock levels, glyph replace prompt, talent point events | S | M | **(code)** glyph API exists; prerequisites fixed in pets work |
| Stock CVar list | Every 3.3.5a CVar, default, range and effect; which ones Wowee honours, ignores or answers wrongly | M | H | **(code)** no single CVar table; defaults spread over the schema and GetCVarDefault |
| Video settings mapping | Stock Video panel controls to Wowee settings (farclip, `groundEffectDensity`, `groundEffectDist`, `shadowLevel`, `extShadowQuality`, `environmentDetail`, `weatherDensity`, `textureFilteringMode`, `characterAmbient`, `M2Faster`, `projectedTextures`) | M | M | partly; plan 2.1 #17 notes clutter 100% does not map to stock 16 |
| Gamma and brightness | Stock gamma ramp (hardware ramp vs shader), `gamma` CVar range, what Brightness does to UI vs world | S | M | **(code)** Get/SetGamma exist |
| Screenshot behaviour | Stock writes `Screenshots/WoWScrnShot_MMDDYY_HHMMSS.jpg` (`screenshotFormat`, `screenshotQuality`), fires SCREENSHOT_SUCCEEDED / FAILED and shows "Screen captured" | S | L | **(code)** Wowee writes PNG and a chat line "Screenshot saved: ..."; no event |
| Sound driver options | `Sound_OutputDriverName`, `Sound_OutputQuality`, `Sound_DSPBufferSize`, `Sound_EnableHardware`, `Sound_EnableErrorSpeech`, and the Sound panel's device list | S | L | **(code)** almost none |
| Loading pipeline order | What stock loads before the loading screen ends (terrain radius, WMOs, M2s near the player, units), when it fires PLAYER_ENTERING_WORLD vs the screen fade | M | M | Wowee's own order; progress split is a glue gap |
| Network latency handling | `GetNetStats` (stock bandwidth/latency sampling), heartbeat-based latency, how stock hides latency for casts (it does not queue: `SpellQueueWindow` is post-3.3.5) | S | M | **(code)** GetNetStats exists |
| Warden | Verify the emulator against what AzerothCore checks with Warden on; confirm no false kicks on long sessions | M | L | **(code)** implemented (`warden_*.cpp`) |
| GlueXML login UI | Running the stock GlueXML (AccountLogin, CharacterSelect, CharacterCreate) instead of ImGui glue screens | L | M | judged a large separate project in glue-screens |
| Mouse-look scale | (see 1.8) | S | H | |
| Footprints and other "believed stock" effects | One check each against Wow.exe: footprints (FootprintTextures.dbc), M2 distance dither, pressed-object highlight, quest markers, loot sparkles, level-up, underwater motes | S | M | plan 2.2 note |
| Minimap indoor portal walk | Group choice by the portal walk (1.3 of maps.md) | S | M | maps §2 |
| Character texture compositing detail | Remaining C6 rules, plus the bake size per quality, mip generation of the composite, and when stock re-composites (equip, barber) | S | M | characters §8 |

## 2. Enhancement backlog

Every row keeps the framework rules: one toggle, off is the stock picture
with no pass and no allocation, Classic is `Enhancements{}`, a pixel test at
0 differing pixels with the clock frozen. "Cost" is effort, then a GPU guess
on the Iris Xe at 1251x986. "Taste risk" is the chance it makes the game
read as a newer one.

### 2.1 Shadow and occlusion rework (in progress, another session)

Jered saw the shadow rows look worse and drop frames. Whatever the rework
lands, the backlog for it is:

- Measure on Jered's machine class, not only the Iris Xe.
- Sun shadows, shadow edges and contact shadows: either they look better
  than the stock blob plus MCSH in his views, or they come out of the
  recommended set. The stock blob is a fine default.
- E3 leftovers: contact-hardening (blocker search), a near cascade for
  static shadows, swaying trees cast still shadows, units past the 64
  nearest get no map shadow.
- E13 ambient occlusion stays unbuilt until the rework is judged; it was
  rated high taste risk before this feedback.

### 2.2 Remaining plan items

| Id | Item | Look | Off = stock | Cost | Taste risk |
|---|---|---|---|---|---|
| E6 | Snow cover while it snows | Tops of ground, rocks, roofs whiten over minutes in snowfall, fade after; never indoors | factor 0, block skipped | S, <0.1 ms | low; check Winterspring and Storm Peaks unchanged |
| E21b | Snow puffs and a landing puff | Same footfall system, white puffs on SoundID 4 ground; a small puff on landing a jump | no sprites, no pipeline | S, ~0 | low |
| E20b | Collision ease | Camera eases in at walls instead of snapping | stock snap and 2 s ease out | S, 0 | medium: seeing through walls; was left stock by choice |
| E4b | Soft glow cards and ribbons | Lamp halos and trails stop cutting into walls | hard edges | S, <0.2 ms | low |
| E13 | Ambient occlusion, ambient term only | Faint darkening in corners, under doodads | no pass | M, 0.8-1.2 ms | high (see 2.1) |
| E17 | Upscaled texture pack | Sharper art, same painting | override dir not searched | M, memory 4x | high: most upscales repaint |
| E22 | Ambient life | Fireflies by water at night, moths at lamps, from stock models | nothing spawned | M, ~0 | high: adds content |
| U6 | Glow sprites vs stock glow cards | Compare and decide | stock cards | S | medium |
| U7 | Extra smoke | Check for double smoke against stock emitters; remove if double | off | S | medium |
| U17 | Lightning bolts | Only after the stock dome flash (1.6) | forced off | S | medium |

### 2.3 New ideas

| # | Idea | Look | Off = stock | Cost | Taste risk |
|---|---|---|---|---|---|
| N1 | Texture sharpness: LOD bias and mip filtering | Ground and walls stay crisp at a glancing angle; anisotropic filtering already exists (16x), this adds a small negative mip bias and trilinear on all samplers | stock sampler, bias 0 | S, ~0.1 ms | low; shimmer if overdone, cap the bias |
| N2 | Temporal AA at native resolution | Stable edges on fences, leaves and wires without MSAA cost; reuse the FSR2/3 path at render scale 1.0 | MSAA/FXAA as chosen | M, 0.5-1 ms | medium: ghosting on fast units and particles; needs reactive masks |
| N3 | Ground clutter density and fade | Clutter reaches further and thins smoothly instead of popping at the clutter edge; map "100%" to stock `groundEffectDensity` first (fidelity) | stock density and distance | M, 0.3-1 ms | low if density stays stock near the player |
| N4 | Glow quality | Same ffxGlow look, computed at a higher resolution with a wider tap so bright edges do not shimmer when the camera moves | stock quarter-size glow | S, 0.2 ms | low; must not raise the glow amount |
| N5 | Cloud shadows | Large soft shadows drift over the ground from the sky's own cloud layer and its wind; off in storms already dark | none | M, 0.2-0.4 ms | medium: stock skies never shade the ground |
| N6 | Wind ripples on water | Gusts darken and roughen patches of the flat water sheet, from the same wind that moves foliage | flat sheet, stock normal | S, 0.1-0.2 ms | low-medium: keep amplitude small |
| N7 | Deeper footprints in snow and sand | Stock footprints last longer and sink slightly in snow and sand tilesets | stock FootprintTextures behaviour | S, ~0 | low; check footprints are stock first (1.11) |
| N8 | Breath in cold zones | A faint puff of breath at the mouth every few seconds in Northrend, Dun Morogh, Winterspring, when outdoors and the light is cold | none | S, ~0 | medium: needs a zone rule from data (AreaTable/Light), not a hand list |
| N9 | Torch flicker shadows | The nearest one or two flickering torches cast short moving shadows from units | none | L, 1-2 ms | medium; costly; wait for 2.1 |
| N10 | Puddles after rain | Wet ground keeps a few low patches glossy for a while after rain stops, with a sky-only reflection | wet ground stock (off) | M, 0.2-0.5 ms | medium: reflections are not in 3.3.5 |
| N11 | Sky: moon phases and finer stars | Moon texture follows a phase cycle; star field with less aliasing at high resolution (sharp stars exists, this is anti-aliased stars) | stock moon and stars.mdl | S, ~0 | medium: stock moon has no phases |
| N12 | Dithered doodad fade | Doodads fade in and out at range with a screen-door dither instead of blended alpha pop; also fixes sort artefacts on fading trees | stock alpha fade (after 1.5 doodad fade work) | S, ~0 | low |
| N13 | Rim light in dark places | A faint edge light on units in caves and at night so silhouettes read; from the scene's own ambient colour | none | S, <0.1 ms | medium: can look modern; cap strength |
| N14 | High-DPI UI and fonts | FrameXML fonts rasterised at the real pixel size, textures sampled with proper mips, so text is crisp at 1440p/4K and UI scale below 1 | stock font sizes at 768-high layout | M, ~0 | low |
| N15 | Ultrawide HUD | On 21:9, keep the action bars and unit frames inside a centred 16:9 area while the world fills the screen (glue pillarboxing is stock work, 1.10) | stock stretched layout | M, 0 | low |
| N16 | Smoother camera follow | Optional damping on follow turns and on terrain height changes (stairs, slopes), derived from the stock smooth CVars | stock `cameraSmoothStyle` | S, 0 | low-medium: some players want the stock snap |
| N17 | Gamepad polish | Gamepad support exists; add a cursor mode for loot and vendors, a radial for bars 2-4, and stock-timed key-down on pad casts (1.9) | gamepad off | M, 0 | low |
| N18 | Spell queue window | Cast pressed in the last N ms of a cast or GCD is held and sent when it can go, as later clients do; `SpellQueueWindow` already answers 0 | 0 ms (stock) | S, 0 | low for feel, but it changes play timing; off by default |
| N19 | HRTF audio | Binaural rendering of 3D sounds for headphones | stock pan | S, ~0 CPU | low |
| N20 | Heat haze | Faint shimmer over lava and in Tanaris and Silithus at noon | none | S, 0.1-0.2 ms | high: easy to overdo |
| N21 | Landing and splash polish | Water entry splash and swim strokes from the stock models once the trigger is found (1.7), with a softer ripple ring | stock (once found) | S, ~0 | low |

Recommended first: N1, N4, N12, N14 (low risk, near zero cost). Then E6,
E21b, N3, N6. Keep N5, N8, N10, N11, N13 off by default until Jered sees
shots. N9 and N20 last.

## 3. wowpack backlog

From `~/workplace/wowpack/docs/status-2026-11.md`.

| Item | Size | Owner |
|---|---|---|
| Stock-client test: `patch-Z.MPQ` in a clean 3.3.5a `Data/` on Windows, `.tele tolbarad`; check terrain, textures, water, buildings, loading screen, area names, map window, minimap; also whether an addon inside an MPQ is listed (gap 7) | S | Jered |
| Windows Wowee over the LAN with the pack URL (plain HTTP on RFC1918, Windows paths) | S | after the Windows package (1.1) |
| Live delta sync: change one mask, rebuild, confirm only changed ADTs download (nginx log) | S | agent |
| Re-run e2e suites (`test/e2e`, AC fork `e2e/smoke`); they create test characters, needs Jered's OK | S | agent, after OK |
| Tag `v0.1.0` after the four above; first run of the release job | S | agent |
| `wowpack-sync` branch: delete, or keep as the base for upstream PRs | S | Jered |
| Pack transfer size: gzip in `wowpack serve` or nginx `gzip_types` (DBCs ~4:1; 49 MB spell.dbc) | S | agent |
| Compressed alpha maps (2048 bytes, no big-alpha): halves ADT size | M | agent |
| Map images with doodads, buildings, labels; a way to supply hand-drawn world map art | M | agent |
| Tol Barad content: reserved creature ids unused, more quests for 50-55, flight path or boat instead of the gossip teleport | M | agent, Jered for scope |
| IP and City Siege packs: stock client and Wowee in-game checks; deploy only with Jered's go-ahead (changes live spell data, needs the SQL) | M | Jered decides |
| A real two-module merge of one table (none of the current modules share one) | S | agent |
| Locales: document that a pack is built from one locale | S | agent |
| Addon folder case: pack paths are lowercase (SavedVariables move to `citysiege.lua`); document or keep case | S | agent |
| `cmd/wowpack` tests if its flags grow | S | agent |
| Wowee side: the cross-map `.tele` stale position seen once more (fixed Nov 2 per the week plan; recheck on map 901); WDL only visible on maps larger than the far clip | S | agent |

## 4. Suggested order for the next sessions

Lanes are chosen so two agents do not edit the same files. The shadow
rework (2.1) owns its files until it lands.

**Session 1**
- Lane A (core, tooling): ASan soak, then the validation login loop; fix
  what they find. Then the Windows package of HEAD. Files: whatever the soak
  finds; packaging scripts.
- Lane B (M2 + audio): GO sounds, then GO state animations (1.2). Files:
  `m2_renderer*`, `npc_interaction_callback_handler.cpp`, `audio/*`,
  GO handling in `game/`. Do not run a particle or doodad-fade agent beside
  it.
- Lane C (Lua/UI only): undefined binding functions (ToggleAutoRun, views),
  duplicate option names, macro conditionals (merge the two parsers),
  extra unit tokens, totem refresh. Files: `src/addons/*`,
  `src/ui/chat/macro_evaluator.cpp`.

**Session 2**
- Lane A (movement/camera): mouse-look scale, cameraDive and first-person
  fade, collision ease decision, view bindings' camera side. Files:
  `camera_controller.*`, `movement_*`. Coordinate with Lane C on the view
  functions (C adds the Lua names, A the camera calls).
- Lane B (M2 transparency): transparent order (distance, priorityPlane,
  layer) with particles in the same sort, render flags 0x04/0x08/0x10,
  per-instance colour RGB and UV matrix. Files: `m2_renderer*`, `m2*.frag`,
  `character_renderer.cpp`.
- Lane C (sky and weather): mist sheets, Weather.dbc, dome flash, sky model
  order, ScreenEffect 2/3, underwater glow wave. Files: `lighting_manager*`,
  `skybox*`, `weather*`, `fxaa.frag`.

**Session 3**
- Lane A (RE): doodad and unit draw distance and fade (1.11), then the
  dithered fade (N12) on top. Files: `m2_renderer_render.cpp`,
  `m2_renderer_instance.cpp` - not beside Session 2 Lane B.
- Lane B (characters): MOCV interior light per unit, L11, C6, hand grip and
  quivers, ghost fade. Files: `character_renderer*`, `character.frag`,
  composite rules. Not beside the shadow rework if it touches
  `character.frag`.
- Lane C (UI enhancements): N14 high-DPI fonts, N15 ultrawide HUD, glue
  pillarboxing. Files: UI renderer, `glue_*`, FrameXML layout.

**Session 4 and on**
- Low-risk enhancements N1, N4, E6, E21b, N3, N6 (rendering, after the
  shadow rework lands; each is its own row and shader block).
- Stock CVar table and video settings mapping (1.11): one table like the
  enhancement rows. Files: `settings_schema.cpp`, `lua_system_api.cpp`.
  Run alone; it touches many settings paths.
- wowpack lane (separate repo, always parallel-safe): gzip serving,
  delta-sync check, compressed alpha, e2e and the tag once Jered has done
  the stock-client test.
- Remaining small fidelity items from 1.3-1.10 in batches by area.

Parallel-safe at any time: the wowpack lane; RE-only spec writing (Ghidra,
no source edits); doc work.

## Log
