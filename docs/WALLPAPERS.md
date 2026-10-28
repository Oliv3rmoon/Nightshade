# Wallpapers

An image of your own behind the launcher's screens. **Settings > Appearance > Wallpaper**
lists the images in the wallpapers folders. The choice is kept in the settings file as
`"launcher": { "wallpaper": "beach.png" }`; with no wallpaper (or one this build cannot
read) the theme's own art shows, as before.

## The folders

The images are read from two folders on the console:

```
<game files folder>/wallpapers          # beside keys/, firmware/ and roms/
/data/prosperoeden/wallpapers
```

- **From a PC, without FTP.** The game files folder is the one chosen in **Settings > Game
  files**, which holds `keys`, `firmware` and `roms` (the `assets` folder you keep when you
  replace a copy of the app on a USB drive). Make a `wallpapers` folder in it on your PC, put
  the images in it, and copy the folder to the drive as you copy the rest. This folder is read
  first: an image replaced there is the one shown.
- **Over FTP.** `/data/prosperoeden/wallpapers` works as before. The app creates it when it
  starts; put images in it over FTP while the app is running.

With the game files folder left at `/data/prosperoeden` (the default) both are the same
folder. An image whose name is in both folders is listed once and read from the game files
folder. The settings keep only the name, so a wallpaper chosen before keeps working. When the
drive that holds the game files folder is not plugged in, its images are not listed, and a
wallpaper chosen from it shows the data folder's image of the same name, or the theme's art. A
new game files folder applies the next time the app opens, as everywhere else.

The dialog shows the folders under its list. Leave and reopen **Settings > Appearance >
Wallpaper** to see new images: the list is read each time the dialog opens. An image already
in use is loaded again when the settings are saved, so a replaced file takes effect without
restarting.

## Formats

PNG and JPEG are what a photo or a screenshot usually is; BMP, GIF (its first picture), TGA
and BC7 DDS are read too. An image over 8192 pixels a side is halved until it fits, and the
whole picture is resized to the screen: an image of another shape keeps its own proportions,
with the theme showing behind what it leaves over, instead of being stretched or cut.

## What shows where

The Home screen and the Library draw one background image, in this order:

1. a game's own cover, where the theme shows one (Home and the selected game in the Library);
2. the wallpaper, when one is chosen;
3. the theme's own artwork.

Panels and cards sample the blurred copy of whichever of them is in use.
