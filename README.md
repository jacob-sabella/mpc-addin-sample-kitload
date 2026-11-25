# mpc-addin-sample-kitload

An addin for the MPC Sample that makes Load Kit work bank by bank. Stock Load Kit replaces the whole drum program: every
bank comes from the kit, and the sample pool is cleared. With this addin, B3 Load Kit first shows the KITLOAD page, where
you put the kit's banks onto the banks you choose. Only those banks change; every other pad keeps its sample and
settings.

![The KITLOAD page after B3 Load Kit: the kit's A bank goes onto banks B, C and D](docs/screenshots/kitload-prompt.png)

- **The page** (needs mpc-addin-sample-draw-handler 1.1.1 or later): B3 Load Kit opens it with the kit's name and the
  banks it fills (`KIT BANKS A16 B16`). One row per bank of your project: the dial picks a row, a dial press (or K1)
  steps the kit bank that goes there, `-` keeps the bank as it is. B1-B3 along the top of the screen:
  - **ALL** loads the whole kit, exactly as MPC does without the addin.
  - **LOAD** changes only the mapped banks. The sample pool isn't cleared, so your other pads keep their samples.
  - **CANCEL** loads nothing (its undo does nothing either).
  The map you load with is where the next Load Kit starts. Holding PAD FX while the page is up also cancels.
- **Undo and redo** are MPC's own: undo puts the whole drum program back as it was before the load; redo does the same
  mapped load again.
- **Settings view:** opened from RACK's page list (mpc-addin-sample-rack) when no kit is waiting. ASK ON/OFF (off: Load
  Kit as MPC does it, no page), and SET/CLEAR for the map the next Load Kit starts with.

![The KITLOAD settings view](docs/screenshots/kitload-settings.png)

**Limits.** MPC Sample firmware 1.4.0 only: every address the addin patches is checked first, and on any other
firmware it stays off and Load Kit is unchanged. Program-wide settings (the drum program's own values, not the pads')
still come from the kit, as with stock Load Kit. After a mapped load, samples no pad uses any more stay in the pool
until you purge it. A kit is what MPC's Load Kit loads: an MPC Sample project (`.xpj`).

## How

The addin is preloaded into MPC (`LD_PRELOAD`) and hooks Load Kit's own commands (`src/hooks.c`):
`LoadProjectAsKitCommand::execute` waits on MPC's main thread while the page asks (the buttons still reach the page
through the draw handler). For LOAD, the sample-pool clear is skipped, and the drum import gives each mapped pad the kit
pad from its kit bank. Every other pad is copied back from the program the load replaced, which MPC keeps for its undo,
through MPC's own pad export and import.

## Settings

`mpc_sample_kitload.conf`, next to the library (the page writes it): `ask` (1: B3 stops at the page, 0: Load Kit as
MPC does it) and `map` (`KITBANK:PROJECTBANK,...`, e.g. `A:C,B:D`).

## Install

With the shared addin installer from mpc-vst-plugins (`tools/release/addin`): `tools/release-aarch64.sh <version>`
builds the zip; on the device, `sh install.sh` in the unpacked folder. Install mpc-addin-sample-draw-handler first.

## Develop

`build-aarch64.sh` builds `build/aarch64/mpc_sample_kitload.so` with the aarch64 cross compiler, or in an arm64
container. `src/sample_draw.h` is the draw handler's client header (as in mpc-addin-sample-rack).

## License

MIT.
