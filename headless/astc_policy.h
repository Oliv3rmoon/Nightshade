// Eden PS5 port: where an ASTC texture that is kept as BC3 gets converted.
//
// The console's GPU has no ASTC, and above 1x such a texture is kept as BC3 (headless/main.cpp):
// decoded and compressed on the CPU. In place, inside the draw that first uses it, that held a
// frame of a large open-world game for 130 to 290 ms whenever new scenery came in. On another
// thread (Eden's asynchronous mode) no frame waits, but the texture is drawn before it is ready:
// in play that is a far texture a moment late, on a loading screen it is the picture itself, and
// wrong.
//
// So the frame decides. A frame with many draws is the game in play: its new textures are
// converted in the background. A frame with few is a loading screen, a menu or a title screen,
// where nothing moves that a wait would spoil: there the texture is converted in place and is
// right the first time it shows. The count is of the frame being made or the one before it,
// whichever is larger, so the first frames of play after a light screen are already "busy".
//
// But a light game is light in play too. Top Racer Collection (Oliv3rmoon/Nightshade#3, a retro
// racer) never has a busy frame: every ASTC texture it brought in during a race was converted in
// place, on every ImageTranscode thread at once, while the frame waited (no "Queuing async texture
// decode" in its log), and the race froze for 0.6 to 0.8 s at a time with its sound. So on a light
// frame only a texture about the size of a screen picture is converted in place (kInPlaceTexels,
// a 1080p picture and its mipmaps: tens of milliseconds); a larger one goes to the background as
// in play, and a light screen keeps moving while it is converted. Above kLightBackgroundTexels
// (a texture array of a large game's loading screen) it stays in place: the background copies
// the whole texture twice at once, where the conversion in place goes a layer at a time.
// The size is MapSizeBytes, which kept as BC3 is a byte per texel, so the work it takes.
#pragma once

namespace Eden::AstcPolicy {
// GPU thread only.
inline unsigned draws_now = 0, draws_before = 0;
// From this many draws a frame is the game in play (a large open-world game: 2,000 to 4,500
// in play, 300 to 750 on its loading screen).
inline constexpr unsigned kBusyFrame = 1000;
// On a light frame: converted in place up to this many texels, in the background above it, and in
// place again above kLightBackgroundTexels.
inline constexpr unsigned kInPlaceTexels = 3u << 20;
inline constexpr unsigned kLightBackgroundTexels = 32u << 20;

inline void Draw() noexcept { ++draws_now; }
inline void Frame() noexcept {
    draws_before = draws_now;
    draws_now = 0;
}
inline unsigned Draws() noexcept { return draws_now > draws_before ? draws_now : draws_before; }
inline bool Busy() noexcept { return Draws() >= kBusyFrame; }
// `texels`: MapSizeBytes of the texture to convert.
inline bool InBackground(unsigned texels) noexcept {
    return Busy() || (texels > kInPlaceTexels && texels <= kLightBackgroundTexels);
}

// For the log (tools/prepare-vulkan-port.py): conversions in place that held the frame for at least
// kSlowInPlaceNs, and textures a light frame sent to the background. The first ones of each are
// printed, then every 64th, each with the counts so far.
inline constexpr long long kSlowInPlaceNs = 16'000'000;
inline unsigned slow_in_place = 0, light_background = 0;
inline long long slow_in_place_ns = 0;
inline bool ReportSlowInPlace(long long ns) noexcept {
    if (ns < kSlowInPlaceNs) return false;
    slow_in_place_ns += ns;
    return ++slow_in_place <= 32 || slow_in_place % 64 == 0;
}
inline bool ReportLightBackground() noexcept {
    return ++light_background <= 16 || light_background % 64 == 0;
}
} // namespace Eden::AstcPolicy
