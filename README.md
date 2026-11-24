# DAO Quick Saves

A plugin for **Dragon Age: Origins** that keeps more than one quicksave, and saves automatically the first time you enter a zone.

- **Rotating quicksaves.** Every F5 is kept in a ring of saves (5 by default, up to 20). The oldest is replaced. F9 reloads the most recent one, as the game does for any save.
- **First-visit zone saves.** The first time a character enters a zone, the game is saved in a separate ring (100 by default, up to 100), named `Zone <n> <zone name>`. The game's own autosave and your quicksaves never overwrite them.
- **Settings in the game.** A "DAO Quick Saves" section in the options, with a "Reset DAO Quick Saves" button. It shows up in the "Plugin Settings" window if you use that mod, and otherwise in the Online tab of the Options screen.
- **Settings in a file.** `daoquicksave.ini`, next to the plugin's DLL.

## Requirements

- **DAOLib**, installed in the game's folder: <https://github.com/Whocares66-Dev/DAOLib/releases> (follow its install instructions). It works with version 1.05.13263.0 of the game from GOG, EA and Steam.
- Optional: a "Plugin Settings" window for DAOLib plugins (Nexus Mods, Dragon Age: Origins, mod 7114). This plugin declares its settings the standard way, so the window picks them up by itself.

## Installation

Extract the zip into the game's folder, so that the DLL ends up in:

```
bin_ship\daolib\plugins\daoquicksaves\daoquicksaves.dll
```

The plugin writes `daoquicksave.ini` beside it the first time the game starts.

To uninstall, delete the `daoquicksaves` folder. Your saves stay as they are: the ones this plugin made are ordinary manual saves, which you can delete from the Load screen.

## How it works

- **F5.** The plugin takes the game's quicksave request and makes one save in the ring instead, so there is one save and one message on screen. The game's own `QuickSave_1` slot is no longer updated while the option is on; it is again when you turn it off.
- **Which saves.** The saves are manual saves in slots reserved at the top of the list: 381 to 400 for quicksaves, 401 to 500 for zone saves. Changing a number in the settings never moves an existing save.
- **First visit.** The list of zones a character has seen is kept inside every save, so loading an older save puts the list back as it was then. Loading a save is not a first visit. A save is not made in combat or in a cutscene; the plugin tries again a moment later.
- **Names.** Saves are named `Quick <n>` and `Zone <n> <zone name>`, with a number that goes up, so the most recent one is easy to find. The zone name is the one the game gives, in the game's language.

## Settings

In the game, or in `daoquicksave.ini`:

| Section | Key | Default | Meaning |
| --- | --- | --- | --- |
| `[QuickSaves]` | `Enabled` | 1 | Keep a copy of every quicksave in the ring (0 turns it off and leaves F5 as in the vanilla game). |
| `[QuickSaves]` | `Count` | 5 | How many to keep, 1 to 20. |
| `[ZoneSaves]` | `Enabled` | 1 | Save the first time a zone is entered. |
| `[ZoneSaves]` | `Count` | 100 | How many to keep, 1 to 100. |
| `[Debug]` | `LogMessages` | 0 | Log the engine's save and load messages (for bug reports). |

The log is `Documents\BioWare\Dragon Age\daolib\daoquicksaves.log`.

## Known limits

- The labels in the settings are in English.
- The "Restore Defaults" button of the "Plugin Settings" window does not reach plugins' settings; use this plugin's own "Reset DAO Quick Saves" button.

## Building

Visual Studio 2022 with the C++ workload, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, CMake and git. Everything is 32-bit, as the game is.

```
git clone --recursive https://github.com/Wabach/DAOQuicksaves
cd DAOQuickSaves
cmake --preset x86-release
cmake --build --preset x86-release
```

DAOLib comes in as a git submodule (`extern/DAOLib`); see its [guide](https://whocares66-dev.github.io/DAOLib/guide.html).

## License

GPLv3 (see `LICENSE`), as DAOLib, which this plugin compiles in. The libraries it builds on are listed in `THIRD-PARTY.txt`.

## Credits

- [DAOLib](https://github.com/Whocares66-Dev/DAOLib) by Whocares66-Dev, the library that makes plugins for Dragon Age: Origins possible.
- The author of the "Plugin Settings" mod, for the settings window.
- BioWare, for the game.
- This plugin was written with the help of Claude, an AI assistant by Anthropic.
