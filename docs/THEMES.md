# Themes

The launcher's look is a theme. This build has three: **Nightshade** (black and red, clean and
dark), **Canopy** (a tropical dusk, dark green glass and lime) and **Slate** (neutral glass, clean
and still). Nightshade is the default for a new install; a theme chosen before stays. **Settings >
Appearance > Theme** lists the themes a build has; the choice is kept in the settings file as
`"launcher": { "theme": "nightly" }`.

Nightshade has a layout of its own, `showcase`: a top bar with its flower
(`render-art.py nightshade-mark`, from `tools/launcher/art/nightshade-logo.svg`), the NIGHTSHADE
wordmark, the tabs Home, Library, Multiplayer and Settings, and the clock; the home screen over
the selected game's art, with the game's name, its facts as pills, a crimson Play and an
outlined Game settings, and a row of covers under them (the recent games first); a Library of six
covers to a row with a search (the system keyboard) and the filters All, Recently played and
Favorites; and Settings as a column of categories beside a glass panel of the focused one, where
its dialog opens. About is Settings' last row. The hints are at the bottom right. Its colours are
black `#0b0b0d` and crimson `#d4142f` with white on the accent, its type is larger than the other
themes' (it is read from a couch), and its warnings are amber, so they never read as the accent.
Its backdrop is a plate of crimson nightshade flowers and plum berries at the left and right
edges, a dark empty middle and a crimson haze rising from the bottom (`render-art.py
nightly-backdrop`, from `tools/launcher/art/nightshade-plate.jpg`). It is behind the Library,
Multiplayer, Settings and a game's settings, and behind the home screen when there is no game art;
a veil keeps the middle and the bands of the top bar and the hints dark, so the flowers frame the
screen without lying under words (every line of text keeps at least 4.5 to 1). The plate's alpha is
1 at the edges and in its light and 0 in its dark middle, so the same texture is the frame laid
over a game's art at the home screen's right edge: no second picture, no blur at run time. It is
2048 by 1152 like every backdrop (9 MiB of graphics memory, as the backdrop it replaced). A focused
card, panel or button has a crimson ring of 4 pixels and a strong soft light (`glow` 1.7), and
cards and panels cast soft shadows. Its accent presets are Red, Crimson, Rose, White, Violet and
Ice.

The game's art is its cover, made small and blurred once when the selection rests on a game
(`Textures::show_game_art`), never every frame; the art before it fades out (at once with Reduce
motion). Real blur behind panels is not something the renderer does: a panel shows a blurred copy
of the art it lies on, as every theme's glass does.

**Settings > Appearance > Launcher** selects a separately installed runtime launcher.
Runtime packs can replace layout, navigation, assets and sounds without rebuilding the
app; see [Runtime launcher packs](RUNTIME_LAUNCHERS.md). The built-in color themes
described here remain compiled into the app. Runtime packs are not loaded into this
color-theme table.

The two show the range: Canopy keeps the rail, the hero and the artwork-driven glass as the
launcher was first drawn. Slate turns on every layout variant — `list_settings`,
`minimal_home` and `top_nav` (the two-column dashboard), a flat near-opaque scrim
(`scrim_opacity` 0.92) over a backdrop with no motes or drift, and `game_page_offset` 60
under its taller header.

A theme is data. It says what the screens are painted with, and nothing about where things are.

## What a theme sets

Everything below is one structure, `pe::ui::Theme` in
[`headless/prosperoeden/pe/ui/theme.hpp`](../headless/prosperoeden/pe/ui/theme.hpp).

| Part | What it holds |
| --- | --- |
| Identity | `id` (in the settings file), `name` (as Settings shows it), `summary` (one line about it), `kit` (the recipe it starts from), `contract` (the version of the structure it was made for) |
| Colour: surfaces | `base`, `scrim`, `glass`, `panel`, `panel_edge`, `row`, `row_edge`, `row_focus` |
| Colour: accent | `accent`, `accent_deep`, `accent_pale`, `on_accent` (text on an accent fill), `highlight` (light in the artwork). Each theme carries an array of accent presets (`AccentPreset`) the player picks from in **Settings > Colour**; the choice is saved as `"launcher": { "accent": "ice" }` |
| Colour: text | `text`, `title`, `value`, `body`, `copy`, `meta`, `label`, `muted`, `faint`, `warning`, `rule` |
| Type | the sizes of the hero title, a screen's title, rows, captions; how much heavier titles are drawn; the spacing of the small capital headings |
| Shape | corner radii for rows, cards and panels; whether chips are pills; hairline and focus ring widths; how far the focus ring's light reaches and how strong it is (`glow`); whether panels are glass |
| Motion | the springs of the focus, the highlight and scrolling; how long a screen change and a game's start take; the delay between the parts of a screen as it arrives; the focus glow's pulse |
| Artwork | the backdrop, its blurred copy (behind glass panels) and the app's mark, as files in `ui/art`; where the artwork's light is |
| Layout variants | `list_settings` (settings as a vertical list instead of tiles), `game_backdrop` (the selected game's cover art fills the background), `minimal_home` (clean home layout: a detail card on the left with the Library grid on the right, no floating cover or controllers, no recently-played shelf), `top_nav` (horizontal section tabs — Home, Multiplayer, Settings, About — at the top instead of the sidebar rail; the Library grid lives on the home screen, and the other sections draw as right-side panels), `scrim_opacity` (>0: a flat scrim at this alpha over the backdrop; 0: the directional gradient scrim), `game_page_offset` (pixels to push the game settings columns below their base position), `showcase` (Nightshade's own layout, described above: it takes the place of `top_nav`, `minimal_home` and `list_settings` on the screens it draws) |
| Ambient light | how many motes of light rise over the artwork, and how far the artwork drifts |
| Sound | a folder of `ui/sounds` with its own cues (none: the launcher's own) |

The screen shown while a game loads is drawn by the emulator, not by the launcher, and takes
four of these colours with it: `accent`, `accent_pale`, `accent_deep` and `base`
(`Eden::Loading::Palette` in `headless/hud.h`). Everything else on it is the game's own cover.

## What a theme does not set

- **Where things are.** A theme can choose between predefined layout variants
  (`list_settings`, `game_backdrop`, `minimal_home`, `top_nav`, `scrim_opacity`,
  `game_page_offset`, `showcase`) but not invent new ones. `top_nav` is the largest: a two-column
  dashboard where the detail card and Library grid share the home screen, and the other
  sections draw as right-side panels.
- **Text.** The words and their translations are the launcher's.
- **Accessibility.** Larger text, High contrast and Reduce motion lie over whatever theme is in
  use (`pe::ui::Look`). A theme cannot turn them off.

## Accent colour presets

Surfaces and text belong to the theme; the accent is the one colour set the player may swap.
Each theme carries its own table of presets (`AccentPreset` in `theme.hpp`): an `id` for the
settings file, a `name` as Settings shows it, and the five accent roles — `accent`,
`accent_deep`, `accent_pale`, `on_accent` and `highlight`. Nothing else changes.

The tables sit beside the themes in `theme.cpp` (`kCanopyAccents`, `kSlateAccents`), and
`presets_for` names the table of each theme. The first preset is the theme's own accent
colours — Lime for Canopy, Silver for Slate — so it matches what the theme already draws.
"Default" in Settings is the theme itself, with no preset.

**Settings > Colour** lists Default and the theme's presets; six of the nine rows show at a
time and the list scrolls. The dialog opens on the choice in use. Cross applies a preset at
once, over the running launcher, and saves it as `"launcher": { "accent": "ice" }`; an id
the theme does not have reads as Default. **Settings > Theme** clears the accent: a new
theme brings its own.

The preset tables are written with hex colours (`Color::rgb(0xa9db63)`). `theme.cpp` and
`theme_kit.cpp` are the only files where the build allows a colour as a number
(`tools/check-launcher-theme.py`); a screen that named one would fail the build.

## Three rules

1. **Roles, not hues.** A screen asks for `th().accent`, never for a green. A colour written as a
   number in a screen would stay that colour in every theme, so the build fails on one
   (`tools/check-launcher-theme.py`).
2. **Canopy is the fallback.** It is compiled in. A theme starts as a copy of a complete one, so a
   part it does not set has a value; a theme named in the settings file that the build does not
   have leaves the first theme in use.
3. **A warning is marked, not only coloured.** `warning` always comes with its mark (the "!" in a
   ring), so a theme whose warning colour is close to another colour still reads.

## Where a theme starts: a recipe of the UI kit

The launcher draws with its own engine (`headless/prosperoeden/pe`: it shapes Arabic and Thai,
reads the console's fonts for Japanese, Korean, Chinese and Greek, and fits translated text to its
place). Its themes start from the theme recipes of
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui), the UI kit: the kit's
theme table is fetched at a pinned commit when the launcher is built (`ui-kit` in
`tools/deps.json`) and compiled in.

`theme_from_kit("acrylic", &theme)` (`pe/ui/theme_kit.cpp`) fills a launcher theme from a kit
theme:

- every colour role from the kit's page, surface, text, primary, accent and outline colours;
- the corners from the kit's radii, at this launcher's larger sizes;
- the focus ring from the kit's focus width and shadow blur;
- glass panels when the kit theme's surfaces are glass;
- the springs from the kit's.

Canopy and Slate both start from the kit's **Acrylic** (frosted glass, soft shadows, a ring of
light as the focus) and then set their own colours, corners and artwork over it.

## Adding a theme

1. In `pe/ui/theme.cpp`, write a function like `canopy()`: start from a kit recipe with
   `theme_from_kit`, then set what the theme wants of its own. Give it an `id` of lower-case
   letters, digits and hyphens (the settings file takes nothing else).
2. Add it to the table in `Store` there (its size is the number of themes).
3. Give it a table of accent presets beside the others in `theme.cpp` and name it in
   `presets_for`; the first preset is the theme's own accent colours. A theme without a
   table offers only Default in Settings > Colour.
4. Put its artwork in `headless/prosperoeden/ui/art`: a backdrop of 2048 by 1152 (the screen plus
   room to drift), the same picture blurred at a quarter of that size, and a square mark. Name the
   files in the theme. (Slate's backdrop is drawn by `tools/launcher/render-art.py`, no photo.)
5. Look at it: `tools/launcher/preview.sh` draws every screen on a PC. Run it once more with
   `PE_LOOK=large,contrast,calm` (the accessibility settings) and in a language with long words
   (`PE_LANG=de-DE`).
6. Text on a surface needs a contrast of 4.5 to 1 (3 to 1 from 24 pixels up); check `text`,
   `copy` and `meta` on `panel` and `row`, and `on_accent` on `accent`.

Changing the theme in Settings shows in the next frame for colours, corners and motion; artwork
is read when the launcher opens.

## The contract's version

`contract` is 1. A change that adds a part with a default keeps it; a change that old themes
cannot follow raises it, and a theme made for a lower one is completed from Canopy.
