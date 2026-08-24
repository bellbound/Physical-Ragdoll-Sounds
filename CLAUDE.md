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

## Always deploy what changed

A change that only exists in the repo is a change the game has not got. When
work is done and the locks are gone, deploy the halves that actually changed:

| what changed | how it gets deployed |
|---|---|
| `plugin\`, `core\` | `pwsh -File ..\..\build-skse-mods.ps1 -Mod Physical-Ragoll-Sounds` |
| `assets\sfx\` (the pack or the library) | `pwsh -File deploy-pack.ps1` |
| `testbench\` only | nothing to deploy - relink and it is done |

`core\` is in the first row on purpose: it is a static lib linked into the game
DLL, so an engine change is a DLL change even when no file under `plugin\` was
touched.

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

**The failure total moves by one or two between identical runs.** The
per-take `determinism` check is byte-exact within a process, but the totals
across processes are not: `Slots:iRngSeed` defaults to 0, which seeds from the
clock. Pin it with `--set Slots:iRngSeed=12345` before comparing two configs, or
you will read dice as a regression.
