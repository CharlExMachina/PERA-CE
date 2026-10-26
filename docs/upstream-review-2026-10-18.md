# Selective upstream review: testing merge on main

Reviewed October 18, 2026 from this fork's `main` at `5bd4cebe2`, through
upstream `c20866353` (merge of pull request #406, testing into main). The
previous reviewed-through revision was `23b542601f2ca505c7a0143703e92fbda6075e18`.

The comparison is the tree diff from fork `main` to upstream `main`, not a
wholesale merge. Histories diverged (201 fork-only commits, 190 upstream-only).
Many upstream commits already exist here under other hashes: text chat and
streamer mode, map sharing and its rate, mods, the resampler, first-person
legs, shield color and glow, screen fog, Custom Edition map names, titles
kept under menus, the postgame host screen returning to the main menu, and
the larger transparent-geometry limit.

## Adopted

| Upstream commit | Actual change | Decision |
| --- | --- | --- |
| [`1b61b529294dee3bd3c90a816781400d24dcc709`](https://github.com/cybersecurity/halo-ce-universal/commit/1b61b529294dee3bd3c90a816781400d24dcc709) | A WebRTC SCTP channel reports every gap and gives up a gap that has sat for a second, so one loss does not stall the channel. The web test change was not taken; this fork has no web tree. | **Adopt.** Local commit on `main`. Mac internet play uses this file for a browser peer. The UDP path is unchanged. |

## Excluded

Upstream's halo-1 reconcile, merged in `0110c1600`, replaces fork fixes with
the original bytes. That includes clearing 64 bytes in the single-pixel S3TC
decoder, asserting a block coordinate of 4, drawing scope ticks from the unit
aim instead of the view, halting on a meter alpha outside 0 to 1, and dropping
the first-person plasma charge. Those stay as this fork has them.

The campaign pause pitch stays at the fork's 33. Upstream's 30 is the PC
pause menu. HUD button styles (Classic, Gems, menu gems) are replacement
artwork and stay out. Upstream's `main.c` names a Mac guest Android and adds
the browser main loop; the Mac label stays.

## Deferred

The web build, site, and `HALO_WEB` branches in shared files. This fork has
no `port/web` tree. Taking those branches as they stand drops Mac-only cases
in the guest window size, the config path, and the platform label. A later
web port can add them beside the Mac cases.
