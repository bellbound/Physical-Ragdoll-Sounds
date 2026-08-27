#!/usr/bin/env python
r"""Put the tuning the testbench is playing into the game.

    python tools/deploy-config.py                 # whatever side A holds right now
    python tools/deploy-config.py config_22_08_99 # a named one
    python tools/deploy-config.py --dry-run

`deploy-pack.ps1` deploys the *sounds* and the assignments. This deploys the
*numbers*, which had no path at all: a tuning session ended with the testbench
playing `config_22_08_125` and the game - launched on its own, with no devbench
socket attached - still reading whatever `RagdollSounds_Algorithm.ini` was last
written, which on this machine was five days and eight configs behind.

Three things make it more than a copy.

**The split.** A testbench config is one file holding both halves; the plugin
reads two. `[Surfaces]` is a real algorithm section and stays; the per-class
`[Surface.<name>]` blocks are what `RagdollSounds_Algorithm_Surfaces.ini` is
for, and `ConfigManager::MigrateSurfaces` will *not* rescue them - it only
migrates six legacy keys, so a config copied whole loses every per-surface trim
and offset it carried.

**The shadow.** The plugin round-trips its inis at startup and a write through
MO2's VFS lands in `overwrite\`, which then shadows the mod's own copy for every
later launch. Deploying into the mod folder alone changes nothing. The stale
pair is removed here, exactly as `deploy-pack.ps1` does for the sfx ini.

**The overlay.** `deployment_files\main\` is what a downloader gets, so it is
written too - otherwise the tuning reaches this machine and nobody else.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tune  # noqa: E402  - same directory, and the socket lives there

REPO = Path(__file__).resolve().parent.parent
CONFIGS = REPO / "testbench" / "configs"

# The deployed mod, through the junction-free real path.
MOD = REPO.parent.parent / "papyrus" / "mods" / "Physical Ragdoll Sounds"
GAME = MOD / "SKSE" / "Plugins" / "RagdollSounds"
OVERLAY = MOD / "deployment_files" / "main" / "SKSE" / "Plugins" / "RagdollSounds"
# The repo's own pristine set. Flat, and every Sfx line in it is blank.
PRISTINE = REPO / "deployment_files" / "main"
MO2_OVERWRITE = Path(r"C:\games\skyrim\MGON\overwrite\SKSE\Plugins\RagdollSounds")

ALGORITHM = "RagdollSounds_Algorithm.ini"
SURFACES = "RagdollSounds_Algorithm_Surfaces.ini"
SFX = "RagdollSounds_SFX.ini"

SURFACES_HEADER = """; RagdollSounds_Algorithm_Surfaces.ini - one block per floor you have opened
;
; Split out of a testbench config by tools/deploy-config.py. A surface with no
; block inherits from its parent - metal, glass and ice from stone; dirt, gravel,
; snow, water and body from soft; a puddle from water and bone from body - and a
; root with no block takes the [Surfaces] section of RagdollSounds_Algorithm.ini.
;
; Delete a block to go back to inheriting.
"""


def selected_config() -> str:
    """What the focused side is playing, straight from the running app."""
    port = tune.control_port()
    try:
        reply = tune.talk("op=status\n", port)
    except OSError as exc:
        # The usual case is "the app is shut". Naming a config works without it,
        # which is the whole reason this is not a hard requirement.
        raise SystemExit(
            f"no testbench on 127.0.0.1:{port} ({exc}).\n"
            "Start it, or name the config to deploy: "
            "python tools/deploy-config.py config_22_08_125"
        ) from exc
    name = tune.value_of(reply, "config")
    if not name:
        raise SystemExit("testbench answered but named no config")
    if tune.value_of(reply, "unsaved") == "1":
        raise SystemExit(
            f"{name} has unsaved edits. `tune.py set` saves as a new config, so this means the "
            "app has changes no file holds - save them there first, or name a config explicitly."
        )
    return name


def split(text: str) -> tuple[str, str, int]:
    """-> (algorithm half, surfaces half, block count).

    Counted here rather than by searching the output, because every per-class
    key carries a comment naming its parent - "Inherited from [Surface.stone]
    until..." - so counting `[Surface.` in the finished text reports the
    comments as well and comes out nearly three times too high.
    """
    algorithm: list[str] = []
    surfaces: list[str] = []
    target = algorithm
    blocks = 0
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            # `[Surfaces]` is the root section and belongs to the algorithm;
            # `[Surface.dirt]` is a per-class block and does not.
            if stripped[1:-1].startswith("Surface."):
                target = surfaces
                blocks += 1
            else:
                target = algorithm
        target.append(line)
    return "".join(algorithm), "".join(surfaces), blocks


def write(path: Path, text: str, dry: bool, changed: list[str]) -> None:
    old = path.read_text(encoding="utf-8", errors="replace") if path.exists() else None
    if old == text:
        print(f"  = {path}")
        return
    changed.append(str(path))
    print(f"  {'~' if dry else '+'} {path}")
    if dry:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    if old is not None:
        shutil.copy2(path, path.with_suffix(path.suffix + ".bak"))
    path.write_text(text, encoding="utf-8", newline="")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", nargs="?", help="config name; default is what the testbench holds")
    ap.add_argument("--dry-run", action="store_true", help="say what would change, touch nothing")
    ap.add_argument("--keep-shadow", action="store_true",
                    help="leave MO2 overwrite alone (it will keep shadowing what this writes)")
    args = ap.parse_args()

    name = args.config or selected_config()
    source = CONFIGS / f"{name}.ini"
    if not source.exists():
        raise SystemExit(f"no such config: {source}")

    algorithm, surfaces, blocks = split(source.read_text(encoding="utf-8", errors="replace"))
    if not surfaces.strip():
        print(f"note: {name} has no per-surface blocks, so the surfaces ini is written empty")
    surfaces = SURFACES_HEADER + "\n" + surfaces if surfaces.strip() else SURFACES_HEADER

    # The assignments are already correct in the overlay - deploy-pack.ps1 keeps
    # them there. The pristine set is the one that never got them.
    sfx_source = OVERLAY / SFX
    sfx_text = sfx_source.read_text(encoding="utf-8", errors="replace") if sfx_source.exists() else None

    print(f"deploying {name} ({source.stat().st_size} bytes, "
          f"{blocks} surface block(s))")
    changed: list[str] = []

    print("the game reads these:")
    write(GAME / ALGORITHM, algorithm, args.dry_run, changed)
    write(GAME / SURFACES, surfaces, args.dry_run, changed)

    print("the release overlay:")
    write(OVERLAY / ALGORITHM, algorithm, args.dry_run, changed)
    write(OVERLAY / SURFACES, surfaces, args.dry_run, changed)

    print("the repo's pristine set:")
    write(PRISTINE / ALGORITHM, algorithm, args.dry_run, changed)
    write(PRISTINE / SURFACES, surfaces, args.dry_run, changed)
    if sfx_text is not None:
        write(PRISTINE / SFX, sfx_text, args.dry_run, changed)
    else:
        print(f"  ! no {sfx_source} to take assignments from")

    if args.keep_shadow:
        print("overwrite: left alone (--keep-shadow); it shadows everything above")
    else:
        print("overwrite: removing the shadowing pair so the next launch reads what we wrote")
        for f in (ALGORITHM, SURFACES):
            p = MO2_OVERWRITE / f
            if not p.exists():
                print(f"  = {p} (not there)")
                continue
            print(f"  {'~' if args.dry_run else '-'} {p}")
            if not args.dry_run:
                shutil.move(str(p), str(p) + ".bak")

    print()
    if args.dry_run:
        print(f"dry run - {len(changed)} file(s) would change")
    else:
        print(f"{len(changed)} file(s) written. The game now plays {name} on its own, "
              "with no testbench attached.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
