#include "rendering/enhanced_lighting_emulation.hpp"

#include "rendering/stock_light_math.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace wowee {
namespace rendering {
namespace enhanced_lighting_emulation {

namespace fl = enhanced_lighting_table;

namespace {

constexpr int kDay = stock_light::kHalfMinutesPerDay;
constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};
// Orange against blue and green against magenta, each of no luminance.
constexpr float kWarm[3] = {1.0f, -(0.2126f - 0.0722f) / 0.7152f, -1.0f};
constexpr float kGreen[3] = {-1.0f, (0.2126f + 0.0722f) / 0.7152f, -1.0f};

enum Group { kSun, kAmbient, kSky, kFogColor, kSprite, kClouds, kNone };
// The colour channels re-graded (LightIntBand order) and the grade each takes.
constexpr Group kChannelGroup[fl::kColorChannels] = {
    kSun, kAmbient, kSky, kSky, kSky, kSky, kSky, kFogColor,
    kNone, kSprite, kClouds, kClouds, kNone, kNone, kNone, kNone, kNone, kNone};

bool readGrade(const nlohmann::json& j, Grade& g) {
    if (!j.is_array() || j.size() != 4) return false;
    for (const auto& v : j) {
        if (!v.is_number()) return false;
    }
    g = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
    return true;
}

bool readGrades(const nlohmann::json& j, size_t n, std::vector<Grade>& out) {
    if (!j.is_array() || j.size() != n) return false;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        if (!readGrade(j[i], out[i])) return false;
    }
    return true;
}

int bandTime(float hours) {
    int t = static_cast<int>(std::nearbyint(hours * 120.0f)) % kDay;
    return t < 0 ? t + kDay : t;
}

/// How much of each band applies at half-minute t: linear between the band
/// times, round midnight.
std::vector<float> bandWeights(const std::vector<int>& bands, int t) {
    const size_t n = bands.size();
    std::vector<float> w(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const int a = bands[i], b = bands[(i + 1) % n];
        int span = ((b - a) % kDay + kDay) % kDay;
        if (span == 0) span = kDay;
        const int d = ((t - a) % kDay + kDay) % kDay;
        if (d < span) {
            const float f = static_cast<float>(d) / static_cast<float>(span);
            w[i] += 1.0f - f;
            w[(i + 1) % n] += f;
            return w;
        }
    }
    w[0] = 1.0f;
    return w;
}

Grade mix(const std::vector<float>& w, const std::vector<ClassBand>& bands, Grade ClassBand::*which) {
    Grade g{0.0f, 0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < w.size(); ++i) {
        const Grade& b = bands[i].*which;
        g.level += w[i] * b.level;
        g.saturation += w[i] * b.saturation;
        g.warmth += w[i] * b.warmth;
        g.tint += w[i] * b.tint;
    }
    return g;
}

Grade mix(const std::vector<float>& w, const std::vector<Grade>& bands) {
    Grade g{0.0f, 0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < w.size(); ++i) {
        g.level += w[i] * bands[i].level;
        g.saturation += w[i] * bands[i].saturation;
        g.warmth += w[i] * bands[i].warmth;
        g.tint += w[i] * bands[i].tint;
    }
    return g;
}

uint32_t pack(const std::array<float, 3>& rgb) {
    uint32_t out = 0;
    for (int i = 0; i < 3; ++i) {
        const float v = std::nearbyint(rgb[i]);
        const uint32_t b = static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f));
        out |= b << (16 - 8 * i);
    }
    return out;
}

std::array<float, 3> unpack(uint32_t c) {
    return {static_cast<float>((c >> 16) & 0xFFu), static_cast<float>((c >> 8) & 0xFFu),
            static_cast<float>(c & 0xFFu)};
}

/// The set's key times: 3.3.5's keys on the channels re-graded and the two
/// fog floats, and the band times; at most kMaxKeys, the key nearest the one
/// before it dropped first (never a band time).
std::vector<int> keyTimes(const std::vector<int>& bands, const StockSet& set) {
    std::set<int> ts(bands.begin(), bands.end());
    const std::set<int> anchors(bands.begin(), bands.end());
    for (uint32_t c = 0; c < fl::kColorChannels; ++c) {
        if (kChannelGroup[c] == kNone) continue;
        for (int k = 0; k < set.colors[c].count; ++k) ts.insert(set.colors[c].times[k] % kDay);
    }
    for (const FloatBand& f : set.floats) {
        for (int k = 0; k < f.count; ++k) ts.insert(f.times[k] % kDay);
    }
    std::vector<int> out(ts.begin(), ts.end());
    while (out.size() > fl::kMaxKeys) {
        int bestGap = kDay + 1, bestTime = -1;
        for (size_t i = 0; i < out.size(); ++i) {
            if (anchors.count(out[i])) continue;
            const int prev = out[(i + out.size() - 1) % out.size()];
            const int gap = ((out[i] - prev) % kDay + kDay) % kDay;
            if (gap < bestGap || (gap == bestGap && out[i] < bestTime)) {
                bestGap = gap;
                bestTime = out[i];
            }
        }
        if (bestTime < 0) break;
        out.erase(std::find(out.begin(), out.end(), bestTime));
    }
    return out;
}

}  // namespace

bool parseParams(const std::string& text, Params& out, std::string& error) {
    out = Params{};
    const auto j = nlohmann::json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        error = "not JSON";
        return false;
    }
    Params p;
    const auto& hours = j.value("bandHours", nlohmann::json());
    if (!hours.is_array() || hours.empty()) {
        error = "no bandHours";
        return false;
    }
    for (const auto& h : hours) {
        if (!h.is_number()) {
            error = "bandHours: not a number";
            return false;
        }
        p.bandHours.push_back(h.get<float>());
    }
    const size_t n = p.bandHours.size();
    const auto& classes = j.value("classes", nlohmann::json());
    if (!classes.is_object() || classes.empty()) {
        error = "no classes";
        return false;
    }
    // In file order; nlohmann's object is sorted, so the default class is
    // found by name and put first.
    for (const auto& [name, c] : classes.items()) {
        ZoneClass zc;
        zc.name = name;
        for (const auto& z : c.value("zones", nlohmann::json::array())) {
            if (z.is_number_unsigned()) zc.zones.push_back(z.get<uint32_t>());
        }
        std::vector<Grade> sun, ambient, sky, fogColor;
        if (!readGrades(c.value("sun", nlohmann::json()), n, sun) ||
            !readGrades(c.value("ambient", nlohmann::json()), n, ambient) ||
            !readGrades(c.value("sky", nlohmann::json()), n, sky) ||
            !readGrades(c.value("fogColor", nlohmann::json()), n, fogColor)) {
            error = "class " + name + ": a grade list does not match bandHours";
            return false;
        }
        const auto& fog = c.value("fogRange", nlohmann::json());
        if (!fog.is_array() || fog.size() != n) {
            error = "class " + name + ": fogRange does not match bandHours";
            return false;
        }
        zc.bands.resize(n);
        for (size_t i = 0; i < n; ++i) {
            if (!fog[i].is_array() || fog[i].size() != 2 || !fog[i][0].is_number() || !fog[i][1].is_number()) {
                error = "class " + name + ": fogRange entries are [scale, offset]";
                return false;
            }
            zc.bands[i] = {sun[i], ambient[i], sky[i], fogColor[i], fog[i][0].get<float>(),
                           fog[i][1].get<float>()};
        }
        const auto& air = c.value("airDensity", nlohmann::json(0.0));
        zc.airDensity = air.is_number() ? air.get<float>() : 0.0f;
        if (!(zc.airDensity >= 0.0f && zc.airDensity < 1.0f)) {
            error = "class " + name + ": airDensity out of range";
            return false;
        }
        if (name == "default") {
            p.classes.insert(p.classes.begin(), std::move(zc));
        } else {
            p.classes.push_back(std::move(zc));
        }
    }
    if (p.classes.front().name != "default") {
        error = "no \"default\" class";
        return false;
    }
    const auto& shared = j.value("shared", nlohmann::json());
    const auto& sprite = shared.is_object() ? shared.value("sprite", nlohmann::json()) : nlohmann::json();
    if (!sprite.is_array() || sprite.size() != n) {
        error = "shared.sprite does not match bandHours";
        return false;
    }
    for (const auto& s : sprite) {
        if (!s.is_array() || s.size() != 1 || !s[0].is_number()) {
            error = "shared.sprite entries are [toward white]";
            return false;
        }
        p.spriteWhite.push_back(s[0].get<float>());
    }
    if (!readGrades(shared.is_object() ? shared.value("clouds", nlohmann::json()) : nlohmann::json(), n,
                    p.clouds)) {
        error = "shared.clouds does not match bandHours";
        return false;
    }
    out = std::move(p);
    return true;
}

uint32_t zoneOf(const Light& light, const std::vector<ZoneBox>& boxes) {
    uint32_t best = 0;
    float bestDist = 0.0f, bestArea = 0.0f;
    bool found = false;
    for (const ZoneBox& b : boxes) {
        if (b.mapId != light.mapId) continue;
        const float dx = std::max({b.bottom - light.worldX, 0.0f, light.worldX - b.top});
        const float dy = std::max({b.right - light.worldY, 0.0f, light.worldY - b.left});
        const float dist = std::hypot(dx, dy);
        const float area = (b.left - b.right) * (b.top - b.bottom);
        if (!found || dist < bestDist || (dist == bestDist && area < bestArea)) {
            best = b.areaId;
            bestDist = dist;
            bestArea = area;
            found = true;
        }
    }
    return best;
}

size_t classOf(const Light& light, const std::vector<ZoneBox>& boxes, const Params& params) {
    if (light.isMapDefault) return 0;
    const uint32_t zone = zoneOf(light, boxes);
    for (size_t i = 0; i < params.classes.size(); ++i) {
        const auto& z = params.classes[i].zones;
        if (std::find(z.begin(), z.end(), zone) != z.end()) return i;
    }
    return 0;
}

std::array<float, 3> applyGrade(const std::array<float, 3>& rgb, const Grade& g) {
    const float y = kLuma[0] * rgb[0] + kLuma[1] * rgb[1] + kLuma[2] * rgb[2];
    std::array<float, 3> out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = g.level * y + g.saturation * (rgb[i] - y) + y * (g.warmth * kWarm[i] + g.tint * kGreen[i]);
    }
    return out;
}

fl::Table build(const Params& params, const std::vector<Light>& lights, const std::vector<ZoneBox>& boxes,
                const std::function<bool(uint32_t, StockSet&)>& stockSet) {
    fl::Table table;
    if (params.classes.empty() || params.bandHours.empty()) return table;
    std::vector<int> bands;
    for (float h : params.bandHours) bands.push_back(bandTime(h));
    std::map<std::pair<uint32_t, size_t>, uint32_t> made;  // (set, class) -> profile

    std::vector<Light> sorted = lights;
    std::sort(sorted.begin(), sorted.end(), [](const Light& a, const Light& b) { return a.id < b.id; });
    for (const Light& light : sorted) {
        if (light.mapId != 0 && light.mapId != 1) continue;
        const size_t cls = classOf(light, boxes, params);
        const ZoneClass& zc = params.classes[cls];
        const std::pair<uint32_t, uint32_t> slots[2] = {{0u, light.clearParams}, {2u, light.stormParams}};
        for (const auto& [slot, paramsId] : slots) {
            if (paramsId == 0) continue;
            auto it = made.find({paramsId, cls});
            if (it == made.end()) {
                StockSet set;
                if (!stockSet(paramsId, set)) continue;
                uint32_t colorMask = 0, floatMask = 0;
                for (uint32_t c = 0; c < fl::kColorChannels; ++c) {
                    if (kChannelGroup[c] != kNone && set.colors[c].count > 0) colorMask |= 1u << c;
                }
                for (uint32_t f = 0; f < 2; ++f) {
                    if (set.floats[f].count > 0) floatMask |= 1u << f;
                }
                if (colorMask == 0) continue;
                fl::Profile prof;
                prof.referenceParamsId = paramsId;
                prof.firstKey = static_cast<uint32_t>(table.keys.size());
                prof.colorMask = colorMask;
                prof.floatMask = floatMask;
                prof.airDensity = zc.airDensity;
                for (int t : keyTimes(bands, set)) {
                    const std::vector<float> w = bandWeights(bands, t);
                    fl::Key key;
                    key.time = static_cast<uint32_t>(t);
                    for (uint32_t c = 0; c < fl::kColorChannels; ++c) {
                        if (!(colorMask & (1u << c))) continue;
                        const ColorBand& b = set.colors[c];
                        std::array<float, 3> rgb =
                            unpack(stock_light::sampleColorBand(b.times, b.packed, b.count, t));
                        switch (kChannelGroup[c]) {
                            case kSun: rgb = applyGrade(rgb, mix(w, zc.bands, &ClassBand::sun)); break;
                            case kAmbient: rgb = applyGrade(rgb, mix(w, zc.bands, &ClassBand::ambient)); break;
                            case kSky: rgb = applyGrade(rgb, mix(w, zc.bands, &ClassBand::sky)); break;
                            case kFogColor: rgb = applyGrade(rgb, mix(w, zc.bands, &ClassBand::fogColor)); break;
                            case kClouds: rgb = applyGrade(rgb, mix(w, params.clouds)); break;
                            case kSprite: {
                                float k = 0.0f;
                                for (size_t i = 0; i < w.size(); ++i) k += w[i] * params.spriteWhite[i];
                                for (float& v : rgb) v += k * (255.0f - v);
                                break;
                            }
                            case kNone: break;
                        }
                        if (fl::isSkyChannel(c)) {
                            for (float& v : rgb) v *= fl::kSkyExposure;
                        }
                        key.color[c] = pack(rgb);
                    }
                    float endScale = 0.0f, startOffset = 0.0f;
                    for (size_t i = 0; i < w.size(); ++i) {
                        endScale += w[i] * zc.bands[i].fogEndScale;
                        startOffset += w[i] * zc.bands[i].fogStartOffset;
                    }
                    if (set.floats[0].count > 0) {
                        const FloatBand& f = set.floats[0];
                        // Never nearer than ten yards (36ths of a yard).
                        key.value[0] = std::max(360.0f, stock_light::sampleFloatBand(f.times, f.values, f.count, t) *
                                                            endScale);
                    }
                    if (set.floats[1].count > 0) {
                        const FloatBand& f = set.floats[1];
                        key.value[1] = std::clamp(
                            stock_light::sampleFloatBand(f.times, f.values, f.count, t) + startOffset, -1.0f, 0.99f);
                    }
                    table.keys.push_back(key);
                }
                prof.keyCount = static_cast<uint32_t>(table.keys.size()) - prof.firstKey;
                it = made.emplace(std::make_pair(paramsId, cls), static_cast<uint32_t>(table.profiles.size())).first;
                table.profiles.push_back(prof);
            }
            table.links.push_back({light.id, slot, it->second});
        }
    }
    return table;
}

std::vector<uint8_t> serialize(const fl::Table& table) {
    std::vector<uint8_t> out;
    auto word = [&](uint32_t w) {
        const size_t at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, &w, 4);
    };
    auto real = [&](float f) {
        uint32_t w = 0;
        std::memcpy(&w, &f, 4);
        word(w);
    };
    out.insert(out.end(), {'W', 'F', 'L', '2'});
    word(static_cast<uint32_t>(table.keys.size()));
    word(static_cast<uint32_t>(table.profiles.size()));
    word(static_cast<uint32_t>(table.links.size()));
    for (const fl::Key& k : table.keys) {
        word(k.time);
        for (uint32_t c : k.color) word(c);
        for (float v : k.value) real(v);
    }
    for (const fl::Profile& p : table.profiles) {
        word(p.referenceParamsId);
        word(p.firstKey);
        word(p.keyCount);
        word(p.colorMask);
        word(p.floatMask);
        real(p.airDensity);
    }
    for (const fl::Link& l : table.links) {
        word(l.lightId);
        word(l.slot);
        word(l.profile);
    }
    return out;
}

}  // namespace enhanced_lighting_emulation
}  // namespace rendering
}  // namespace wowee
