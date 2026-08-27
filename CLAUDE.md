# Physical Ragdoll Sounds - working rules

## Never kill the testbench. Wait for it.

`RagdollSoundsTestbench.exe` is the tuning app and it is usually open while you
work, because that is the whole point of it: the config panel edits the same
`AlgorithmConfig` you are changing in code, and a session in it is a tuning pass
that is not written down anywhere else. Closing it throws that away.

Windows locks a running exe, so a link into `testbench\build\testbench\` fails
while it is open. **That is a reason to wait, not a reason to close it.**

    # check, do not kill
    Get-Process RagdollSoundsTestbench -ErrorAction SilentlyContinue

When the exe is locked:

1. Compile anyway - objects are not locked, only the exe is. `ninja
   core/rds-verify.exe` builds the whole engine and links a *different*
   binary, so it verifies the change without touching the locked one.
2. Set a **Monitor** on the process and let the link and the deploy fire when it
   exits. Do not poll in a sleep loop and do not ask the user to close it.
3. Report what is still pending, so a run that ends before the app closes does
   not look finished when it is not.

## Changing the config: patch the running testbench

**When the user asks for a tuning change - louder, softer, fewer, later, "the
slides start too eagerly" - do it through `tools/tune.py`, not by editing an
ini.** The testbench is open (see the rule above), it is the thing making the
sound he is judging, and it holds a session that is not written down anywhere
else. A hand-edited ini is a file that session will never read.

    python tools/tune.py status
    python tools/tune.py set Slide:fSlideMinDurationMs=120 -m "slides started too eagerly"

That patches the config the focused side is playing **in the running program**,
saves the result as a **new** file in `testbench\configs\`, and selects it in
the picker. No restart, no reload; the next play is the new sound, and a
connected game gets it on the same frame. Nothing is overwritten - the file it
was patched from is still there - and the edit is on the app's undo stack, so
**Ctrl+Z in the testbench takes back anything this does**.

The name is the next number in the family it came from (`config_24_08_7` ->
`config_24_08_8`) and the picker is newest first, so what the user is being
asked to listen to is at the top of the list. Say the name in your reply: it is
how he finds it and how he goes back to it.

| what | how |
|---|---|
| is it up, what is it playing | `python tools/tune.py status` |
| the configs in the picker | `python tools/tune.py list` |
| what a parameter is set to now | `python tools/tune.py get slide` |
| change one or several | `python tools/tune.py set A:x=1 B:y=2 -m "why"` |
| against another config | `python tools/tune.py set ... --from config_24_08_3` |
| an audition, no file | `python tools/tune.py set ... --no-save` |
| put an existing config back on | `python tools/tune.py load config_24_08_7` |

Notes that save time:

- The section may be dropped when the key is unique: `fSlideMinDurationMs=120`
  works. A near-miss name comes back with the candidates.
- A patch is all or nothing. One bad key and nothing moves, so a failed command
  has left the session exactly as it was.
- A value outside the schema's range is clamped and the reply says so - read it,
  because a clamp is a change that did not happen.
- `-m` is not decoration: it goes into the new ini's header comment and into the
  log, and it is the only thing that says what a config found tomorrow was for.
- `--no-save` for a sweep of a dozen values, then one `set` for the one that
  won. A folder of twelve near-identical configs is a folder nobody reads.

It talks to a loopback socket on the devbench port plus one (27861), described
in `testbench\src\Control.h`. "no testbench on 127.0.0.1:27861" means the app
is not running - **ask him to start it**, and do not go and edit the ini
instead.

## Always deploy what changed

A change that only exists in the repo is a change the game has not got. When
work is done and the locks are gone, deploy the halves that actually changed:

| what changed | how it gets deployed |
|---|---|
| `plugin\`, `core\` | `pwsh -File ..\..\build-skse-mods.ps1 -Mod Physical-Ragoll-Sounds` |
| `assets\sfx\` (the pack or the library) | `pwsh -File deploy-pack.ps1` |
| the tuning - a `tune.py set` you want kept | `python tools\deploy-config.py` |
| `testbench\` only | nothing to deploy - relink and it is done |

NO need to ask the user for permission to deploy the mod

`core\` is in the first row on purpose: it is a static lib linked into the game
DLL, so an engine change is a DLL change even when no file under `plugin\` was
touched.

The tuning row is the one that is easy to forget, because nothing looks broken
without it: `tune.py` patches the *running testbench*, and a connected game
picks that up over the devbench socket on the same frame - so the tuning is
audible in game long before any file holds it. Launch without the testbench and
the game reads `RagdollSounds_Algorithm.ini`, which is whatever was last
written there. Run `deploy-config.py` at the end of a session that is worth
keeping. It splits the config into the two inis the plugin actually reads (the
per-class `[Surface.<name>]` blocks live in a separate file, and
`MigrateSurfaces` will not rescue them from the wrong one), writes the release
overlay and the repo's pristine set as well, and clears the MO2 `overwrite\`
pair that would otherwise shadow all of it. It does not touch
`RagdollSounds.ini` - that one is diagnostics, and the shipping copy has
`bEnableDevbench = 0` on purpose.

Build scripts need **pwsh 7**, not `powershell.exe` - the Windows PowerShell 5
here has no `Get-FileHash` and the build dies mid-script while still reporting
success.

## Building

- The game DLL builds **only** through `build-skse-mods.ps1`. A bare
  `cmake --preset` on `plugin\` dies inside vcpkg before it reaches our code.
- The engine and the testbench build through ninja in
  `testbench\build\testbench\`, and MSVC is **not** on PATH - call
  `vcvars64.bat` first or every compile fails at `cl` not being found.
- `ninja core/rds-verify.exe` is the target to verify an engine change with.
  Never trust `RagdollSoundsTestbench-bench.exe`; it is a stale copy.

## rds-verify

    core\rds-verify.exe Research\NewRecordings assets\sfx

`Research\NewRecordings` is the recording set - `takes\` is the *sound bank*
work area and holds no recordings, so pointing the verifier at it prints "no
recordings" and passes everything that is left.

Useful flags: `--only <take>` for one recording, `--set Section:Key=value` for
any parameter in the schema (this is the cheapest A/B there is - no rebuild),
`--debug` for the firehose. Note `-v` is **not** the debug flag; it is silently
eaten as the bank directory.

**Pin the seed before comparing two configs.** `Slots:iRngSeed` defaults to 0,
which seeds from the clock, so two runs of the same config are two different
dice rolls. `--set Slots:iRngSeed=12345` and the totals are repeatable to the
check.

They are repeatable now. Until 2026-08-24 the totals wandered by one or two
between identical runs on a pinned seed as well, and that was the `determinism`
check itself: it compared cues with `memcmp`, and `Cue` has three bytes of
padding after `collapsed` that the copy into the collector's vector does not
carry across - so it was reading the allocator's leftovers and failing two takes
out of nine at random on runs whose cue lists were identical. It compares field
by field now (`SameCue` in `Offline.cpp`), so a determinism failure means
something.
