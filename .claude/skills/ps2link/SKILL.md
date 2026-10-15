---
name: ps2link
description: Run, reset and debug the Halo PS2 build on the real PS2 over ps2link (ps2client, host:, crash.txt), and edit the SD2PSX/FreeMcBoot card that boots it. Use when the user wants to test on hardware, reset the console, read a hardware crash, or change what the PS2 boots.
---

# Running the PS2 build on hardware with ps2link

Everything below was learned on the project's own console (October 2026). Say
in every report that a result is hardware or PCSX2: PCSX2 forgives unaligned
accesses and null-page reads that the EE faults on.

## The setup

- The PS2 boots from an SD2PSX card (mounted on the Mac as `/Volumes/PS2`)
  with FreeMcBoot in `MemoryCards/PS2/BOOT/BootCard-1.mcd`. PS2Link is at
  `mc?:/PS2LINK/PS2LINK.ELF` on that virtual card, with `IPCONFIG.DAT`
  (PS2 `192.168.0.10`, mask `255.255.255.0`, gateway `192.168.0.1`).
- `LK_Auto_E1` in the card's `SYS-CONF/FREEMCB.CNF` is
  `mc?:/PS2LINK/PS2LINK.ELF`, so PS2Link starts at power-on and after a
  reset. `LK_Auto_E2` and `E3` are `OSDSYS` (the fallback). Hold **Cross** at the
  logo for the FMCB menu (its item 11 is also PS2Link).
- `ps2client` is `~/ps2dev/bin/ps2client`. Export
  `PS2DEV=$HOME/ps2dev PS2SDK=$HOME/ps2dev/ps2sdk` and put
  `$HOME/ps2dev/ee/bin:$HOME/ps2dev/bin` on `PATH` first.

## Is the console reachable? Check before anything else

```bash
ping -c 3 -t 5 192.168.0.10; arp -a | grep "192.168.0.10)"
```

- Replies and a real MAC (`0:4:1f:9e:ef:f2`): the IOP's network is up.
- `(incomplete)` in `arp`: the PS2 is not on the network, whatever its
  screen says. **The IP printed on the PS2Link screen is only the config
  file read, not proof of a link.** It happened once, with the right IP on
  screen and PS2Link started by hand, and cleared after the user retried the
  console (asked to power-cycle it and check the cable; which of those fixed it
  was not recorded). Do that before touching the card or ps2link: the card
  edit was ruled out, since every other file on the card was byte-identical.
- A ping reply says the IOP answers. It does not say PS2Link on the EE is
  ready to take a command.

## Run a build

1. Build (`ninja ps2`), then put `halo.elf` in a folder that is also the
   game's `host:` root: `config.toml`, `maps/ui.map` (converted with
   `tools/ps2/convert_map.py`), and the saves under `z/` go there. A worktree
   has no `assets/`; use `/Users/alex/Developer/OpenCE/assets/maps/`.
2. **Send a stripped copy:** `mips64r5900el-ps2-elf-strip -o halo_s.elf build/ps2/halo.elf`
   (3.4 MB instead of 26 MB; the same addresses). The unstripped ELF
   loaded once and then stalled in the next runs; the stripped one has loaded
   every time since. Keep the unstripped ELF for `addr2line`.
   In that folder: `ps2client -h 192.168.0.10 execee host:halo_s.elf`.
3. **Leave `ps2client` running.** It serves `host:` for the whole run, so a
   short `timeout` on it cuts the game's file access. When wrapping it with
   `timeout`, use 200 s or more and kill only that process (`pkill -f "ps2client -h"`
   if no other session uses one).
4. Run it in the background and write the output to a file. Loading the 26 MB
   ELF over UDP takes up to about 45 s; the log shows dots while it loads.
   Foreground `sleep` is blocked here: wait with `ping -c N -i 1 127.0.0.1`.
5. Only one `ps2client` at a time. A second one, or a rerun while the last load
   is stalled, leaves PS2Link in a state where `execee` prints nothing.

The game's `platform_log` lines reach the console with their text but without
the formatted values (`memory: ` with nothing after it); read
`debug.txt` in the host folder for the full log.

## Reset

```bash
timeout 20 ps2client -h 192.168.0.10 reset
```

- The console drops off the network within about 5 s and is back, with PS2Link
  started by the autoboot, in under 15 s. No menu input is needed.
- **A ping straight after `reset` can still succeed** (the old PS2Link has not
  gone yet). Wait for the pings to fail and then to succeed again before
  `execee`, or the run attaches to a dying session.
- If the console does not come back: look at the TV. A stalled ELF load
  or a half-reset leaves it answering pings with no PS2Link on the EE; reset
  again, and if that fails power-cycle it.

## Read a hardware crash

`port/ps2/src/ps2_crash.c` replaces ps2link's own exception screen (which parks
the EE and makes `dumpmem` and `scrdump` return nothing). On a fault it prints
the exception, the registers and a stack scan to the `ps2client` output and
writes `crash.txt` in the host folder.

- Name an address with the ELF that was running:
  `mips64r5900el-ps2-elf-addr2line -f -i -e halo.elf 0xEPC 0xRA ...`. Use
  the copy you ran; a rebuild moves the code.
- `Cause` bit 31 set means the fault is in a branch delay slot: the fault is
  at EPC+4.
- Address error, store/load (codes 4 and 5) with an odd or non-multiple-of-4
  `BadVAddr` is an unaligned access. The first one found this way was
  `debug_malloc`'s trailing signature (`source/cseries/debug_memory.c`),
  fixed with memcpy helpers marked `port:`. The second was the script
  runtime's stack (`hs_stack_frame` slots started 2 bytes past a multiple
  of 4); expect more of the same kind, one per run.
- `HALO_PS2_CRASH_TEST=trap|null|unaligned python configure.py` builds a copy
  that faults at start-up to test the report. PCSX2 handles the null store and
  ignores the unaligned one; only the console tests those two.

## Edit the boot card

Only with the user's explicit go-ahead: it is the console's boot path, and
the permission check refuses it without a rule from the user.

1. Mount the card, and back up first: copy `BootCard-1.mcd` and the
   current `FREEMCB.CNF` to a new `/Volumes/PS2/Backup-before-<what>-<date>/`
   with a README that says how to restore (copy the image back with the PS2 off).
2. Work on a scratch copy of the image. mymc++ is in
   `~/PS2-RiptOPL-migration-20260928/tools/mymcplusplus-master`; run it with
   `PYTHONPATH` set to that folder and `python3 -m mymcplusplus IMAGE ...`:
   `extract -o OUT -d SYS-CONF FREEMCB.CNF`, `remove SYS-CONF/FREEMCB.CNF`,
   `add -d SYS-CONF FREEMCB.CNF`, `check`.
3. Edit the config **as bytes**. It has CRLF line endings; Python's text mode
   rewrites every line.
4. Check: `check` reports no errors, the file reads back identical, and a
   SHA-1 of every file in both images differs only in `FREEMCB.CNF`
   (write the comparison so it can fail: an empty listing proves nothing).
5. Only then copy the image onto the card, `sync`, and compare it again.
6. Tell the user to put the card back, and test on the console.

To undo: copy the backup image over `MemoryCards/PS2/BOOT/BootCard-1.mcd`.

## Not done, not verified

- `dumpmem` wrote no file ("Unknown device 'mc'", fd -19), and `dumpreg`
  printed nothing. Do not rely on either until checked.
- Why the unstripped 26 MB ELF stalled while loading is not known; the
  stripped copy avoids it (checked in two loads after a power cycle).
- After the console's EE hangs (a stalled load, or a crash that parked it), the
  IOP can keep answering pings while `ps2client reset` does nothing. A power
  cycle fixed that once. A reset after a *handled* crash (`crash.txt`) works.
- ps2link itself is unmodified (the stock `RenameMe` build, 0269a955). Keeping
  the EE serving RPCs after a fault, or a reset that returns to ps2link without
  the BIOS, would need a build from `github.com/ps2dev/ps2link`.
