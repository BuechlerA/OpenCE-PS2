# PlayStation 2 port: the boot and runtime layer ("the kernel")

Status: planning. The milestones below are not started; "Where it stands"
is what Phase 1 did before them.

This document plans the layer that boots the game on a PS2 and runs it:
everything between power-on and `game_initialize`, and what the game's Xbox
kernel calls run on. It is the part of [ps2_port_plan.md](ps2_port_plan.md)
that Phase 1 started and Phase 4 finishes ("Phase 1: the simulation on
hardware", "Phase 4: sound, input, saves and streaming").

It is not a new operating system. The port plan commits to ps2dev's GCC and
ps2sdk on the EE's own kernel, and a kernel of our own would cost months and
give the game nothing it needs. "The kernel" here is:

- the Xbox kernel emulation (`port/linux/src/xbox_kernel.c`, built as it is on
  ps2sdk's pthreads) and the PS2 files that support it (`port/ps2/src`);
- the program's start-up: the IOP, its modules, the data root, the memory map;
- the services a player needs that the platform layer still answers with
  nothing: the gamepad, saves, disc reads.

## Where it stands

Done in Phase 1 (measured in PCSX2 only, unless it says hardware):

- `ninja ps2` links `build/ps2/halo.elf`. The game boots to the main menu,
  loads a map and plays: all 13 multiplayer levels and the campaign level
  `c20` (300 ticks).
- The kernel shim runs unchanged on ps2sdk. The EE's kernel does not share
  time between threads of one priority, so `ps2_vblank.c` gives every game
  thread priority 92 and rotates that queue at each vertical blank.
- The memory map is fixed: the heap ends at `HALO_PORT_TAG_CACHE_ADDRESS`
  (0xEB0000), and `ps2_memory.c` stands in for `XPhysicalAlloc` from the heap.
- Files go through newlib and ps2sdk's devices. The data root is the ELF's
  folder, which in practice is `host:`.
- The services the PS2 does not have yet are answered with nothing
  (`ps2_null_platform.c`: sound, network; `ps2_input.c`: no gamepad but the
  benchmark's).
- On hardware (a PS2 running ps2link), the game reaches its main menu. Its
  second run died in ps2sdk's `_request_end` (a null packet address, probably
  two threads in the file RPC). A fault handler that writes `host:crash.txt`
  was written for it and is not in this tree yet.

So "boots and runs" has two gaps:

1. It is proven only under PCSX2 and ps2link, not standalone on a console.
2. The services a player needs (pad, saves, disc reads) are missing.

## The milestones

Each milestone has a test that decides whether it is done. Say in each commit
whether it was checked in PCSX2 only or on hardware.

The target is the full single-player campaign on a retail PS2, fat or slim
(see "Decisions"). That means no HDD (nothing here may use `hdd0:` or the
expansion bay's modules), one player (no multitap), and a campaign level, not
a multiplayer one, as the test level from K5 on.

A retail PS2 boots no program of ours by itself: it refuses a burned DVD as
a game disc and runs nothing from a memory card or USB. The way in is
FreeMcBoot on the memory card (the development console has it, on its SD2PSX
card), which launches the ELF from the card or from `mass:`, and reads the
maps from a DVD-R made with ESR (the disc is laid out as a DVD-Video, which
the drive accepts, and the ESR launcher or our own loader reads the data
from it) or from `mass:`. The alternative is a modchip. The late slims
(SCPH-9000x with BIOS 2.30) do not run FreeMcBoot from a card, so "any
model" is not promised: the plan says which models K2 was checked on.

| # | Milestone | Work | Done when |
| --- | --- | --- | --- |
| K0 | Hardware parity | Take every ps2sdk file RPC under one lock, in a PS2 file shim around `xbox_files.c` and `posix_files.c`. Bring in the fault handler. Run the PCSX2 benchmark on the console. | Five runs in a row of menu, level and quit on hardware, with no fault. |
| K1 | Fault visibility | An EE exception handler keeps the registers, `EPC` and the game's stack walk (`stack_walk_windows.c`) in a buffer reserved at boot, and a thread writes them to `host:` or `mc0:` once the handler returns: the file RPC goes over the SIF and cannot run with interrupts off, and it is the RPC K0 is putting under a lock. (`ps2_crash.c`, in the kernel-boot tree, writes `host:crash.txt` and is the start.) A memory ledger at boot prints every block taken, what is left of the 32 MB and, once K2 loads the modules, what is left of the IOP's 2 MB. | A deliberate unaligned access and a null read each leave a readable dump. |
| K2 | Standalone boot | Reset the IOP at start-up and load its modules. The pad, the memory card and the disc need none of ours: `sio2man`, `padman`, `mcman`, `mcserv`, `cdvdman` and `cdvdfsv` are in the console's ROM (`rom0:`), and ps2sdk's libpad, libmc and libcdvd are written for them (some libpad calls are not in `rom0:padman`; K4 says which it needs). The USB drive needs `usbd`, `bdm`, `bdmfs_fatfs` and `usbmass_bd`, which ps2sdk builds from its own source (`$PS2DEV/ps2sdk/iop/irx`): the build embeds them in the ELF from there. Under ps2link the reset is skipped, since it takes `host:` with it; that is the development mode. One data-root abstraction (`host:`, `cdrom0:`, `mass:`) replaces "the ELF's folder". A `SYSTEM.CNF` and a disc layout that ESR accepts. | The ELF, launched by FreeMcBoot, runs from a USB drive (`mass:`) with no network, then with the maps on an ESR DVD-R, on a slim and on a fat console; the commit names the models. |
| K3 | Alignment and FPU audit | Run the Linux build with `-fsanitize=alignment` through the menus and all ten campaign levels, scripts included. That covers the code's own accesses; the tag data's layout it covers only where that run reads it, and PCSX2 never faults on it. So the converter also checks every map: the tag schema (`tag_schema.json`) knows each field's type, and an 8-byte field (`double`, `long long`) at an offset that is not a multiple of 8 is a fault on the EE. Run the flush-to-zero, denormals-are-zero and trap audit (both are Phase 0 items of the port plan). | No reports from either, or each one fixed with a `port:` comment. |
| K4 | Gamepad | libpad and padman for one DualShock 2, mapped to `XINPUT_GAMEPAD` in `ps2_input.c` (the README's button table: white is L1, black is R1), with the vibration motors. The multitap is out of scope while split screen is. | Menus and `c20` play with a DualShock 2. |
| K5 | Saves | The game's save calls (the campaign's checkpoints and its persistent state) go to the memory card. A checkpoint today writes the whole game state (3.4 MB, `GAME_STATE_SIZE`) on the main thread with the game's time stopped (`game_state_save`, `game_state_write_to_file` in `game_state_xbox.c`) to a file kept open, and a revert (every death) reads it back (`game_state_read_from_file`, into place on the PS2). A card holds 8 MB and writes slowly, so the PS2's version compresses the state with the game's own zlib, keeps the compressed image in memory for the reverts, and writes it to the card from a thread for the next boot. Before the design: measure the compressed size on the Linux build (the plan's "a few hundred KB" is a guess), and budget zlib's working memory (about 256 KB at the default settings, less with a smaller window) and the image out of the 1 MB `c20` leaves. | Play `c20` to a checkpoint, die and revert, switch the console off, load it. |
| K6 | Disc reads | `cdrom0:` replaces `host:` for the maps: with no HDD, the converted map is read in place from the DVD, as `ps2_cached_map_file_open` already does. An asynchronous read thread, with its own semaphore and not a game thread's. A prefetch hook for `predicted_resources.c`. A plan for the structure BSP switches inside a campaign level (below). PCSX2 boots an ISO and emulates the drive's speed and seeks, so the layout and the switch stall are measured there first, and on the console on the ESR disc (K2) after. | `c20`, then every converted campaign level, loads from a DVD, in PCSX2 and on the console, and crosses its BSP switches with the stall decided below. |
| K7 | Scheduler and Gate 1 | Measure the vertical-blank rotation on hardware. Check what ps2sdk's `sched_yield` does (the kernel shim's `SwitchToThread` and `Sleep(0)`): if it does not rotate the EE's ready queue, a game thread that spins waiting for another keeps the EE until the next vertical blank, and the shim should rotate the queue itself (`RotateThreadReadyQueue`). Decide whether the condition variables should become raw EE semaphores. Run the benchmark on a campaign level. | Gate 1 of the port plan: a typical campaign tick of 15 ms or less on a real PS2. |

Sound (the SPU2 module) and the renderer belong to Phases 3 and 4. They need
K2's module loader first and nothing else from this plan.

### What the campaign changes

- **The test level.** Today only `c20` loads (17.2 MB of tags and BSP, 1 MB of
  the 32 MB never taken). The other nine campaign levels need the map
  converter's reductions (Phase 2 of the port plan: 13.7 to 14.4 MB of tag
  data). K6 and K7 on those levels wait for it; K0 to K5 do not.
- **No HDD.** The Xbox copied each map to its hard disk and read it there.
  The PS2 reads the converted map from the DVD at 3 to 5 MB/s and seeks
  slowly. A campaign level's tags (about 14 MB) take 3 to 5 seconds to load at
  the start. Textures and sounds stream during play, so K6 is more than a
  file layer: it is where the disc layout (a BSP's resources placed together)
  and the prefetch list earn their keep.
- **BSP switches.** The tag cache holds one structure BSP at a time (the
  largest is about 8 MB), and the game reads the next one into the same place
  when a level moves into it. That is a 2 to 3 second DVD read in the middle
  of play, and there is no room for a second buffer. K6 measures it and
  chooses between reading it behind a short fade, as the transition already
  is in the game, and shrinking the BSP by converter changes. It is not
  decided before the measurement.
- **Saves.** A campaign save is larger and more important than a multiplayer
  one (it carries the scripts' state and the level's progress), and the
  campaign reverts to it at every death. K5 tests a checkpoint and a revert
  on a level that has scripts running, not the menu.
- **One player.** The multitap, four-player capacities and split screen are
  out of scope, which also removes the second and later players' share of the
  game state.

### Order

- K0 and K1 first. Nothing else can be checked on hardware while the
  second-run fault stands.
- K2 and K3 next, in parallel.
- K4, K5 and K6 after K2, in parallel, on `c20`.
- K7 last, because it measures the finished system. K6 and K7 repeat on the
  other campaign levels as the converter produces them.

## Risks

| Risk | Effect | Reduction |
| --- | --- | --- |
| The EE has about 1 MB of the 32 MB to spare with `c20`, and about 5 MB with the 9 MB multiplayer tag cache. Each IRX's EE-side buffers, the pad's and the card's come out of it. The IOP's own 2 MB holds the modules and, later, the sound streaming buffers. | A module that loads in PCSX2 and fails on the console | K1's ledger gives both figures before K2 adds modules. Load only the modules a retail disc needs. |
| PCSX2 ignores unaligned accesses and null-page TLB misses; the EE faults on both. | A bug that shows only on hardware | K3 on the Linux build; run every milestone on the console as well. |
| ps2sdk's file and IOP RPCs are not safe across threads. | The K0 fault again, in K4 to K6 | One lock for all RPCs of a kind, none from the game threads that cannot wait, and none from an exception handler (K1). |
| The campaign tick is 45 ms in PCSX2 (`c20`: biped objects 60%, machines 16%, AI 14%). | Gate 1 fails on kernel work alone | That cost is Phase 5's (VU0 maths, the scratchpad) and the converter's (fewer keyframes). K7 only measures it. |
| With no HDD, a BSP switch or a texture stream stalls the game for seconds. | A freeze in play on every campaign level | Disc layout by BSP, the prefetch list, and the K6 measurement before any design. |
| Slim consoles differ from the fat ones (no expansion bay, a different DVD drive, not every model has a network port, and the last ones run no FreeMcBoot from a card). | A boot that works on the development console and fails on others | Test K2 on a slim and a fat model; load nothing meant for the expansion bay; name the models checked. |
| The way in on a retail console is FreeMcBoot and an ESR disc (or a modchip), which a player has to set up. | The port needs a prepared console | State it in the README; it is the condition every PS2 homebrew game has. |
| The USB modules are IRX files in the build. | A repository question | They are ps2sdk's own, built from its source, so the build copies them from the toolchain (`$PS2DEV/ps2sdk/iop/irx`) as it links its libraries; nothing is committed. |

## Rules

- Change `source/` as little as possible, and mark each change `port:`. The
  new code goes in `port/ps2/src` and `port/ps2/irx`.
- The Xbox SDK's headers are never read or copied.
- The game's data stays out of the repository, including the converted maps
  and any disc image K2 makes.
- Nothing may need an HDD or the network adapter to run a retail disc. Both
  are for development (ps2link) only.

## Decisions

1. **HDD.** Not required. The slim models have no HDD bay, and slim
   compatibility is a requirement. Maps are read from the DVD (K6).
2. **First target.** The full single-player campaign. Multiplayer maps remain
   the quick test for K0 to K4, because they load in a few seconds, but the
   milestones that need a campaign level (K5 to K7) use `c20` now and the
   other levels as the converter makes them.
3. **Boot.** FreeMcBoot launches the ELF and the maps are on an ESR DVD-R
   (or `mass:`); a modchip is the alternative, not a requirement. No retail
   console runs the port without one of them (above, "The milestones").
