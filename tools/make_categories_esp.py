"""Author RagdollSounds.esp: the two sound categories the mod's voices play on.

Why a plugin at all, when nothing else about this mod needs one: a volume slider
in Skyrim's Audio settings *is* a sound category record, and a category record
can only come from a plugin. The engine builds that page by walking the load
order's SNCT forms and showing every one with the ShouldAppearOnMenu flag, so
shipping two records is the whole mechanism. Nothing at runtime writes a
category volume - see the note on GetCategoryVolume in SkyrimNet's
docs/Engine Audio RE Notes/RE-FINDINGS.md section 4b, which establishes that the
getter does not read back what the setter wrote and that driving a category from
code is therefore not viable.

The two records and why they nest:

    Ragdoll Sounds  (0x800)  parent $Master        - every voice the mod opens
    Ragdoll Gore    (0x801)  parent Ragdoll Sounds - the crunch and gore layers

A category's level multiplies its parent's, so the gore slider trims gore
*within* whatever the ragdoll slider has already set, and pulling either to zero
silences what it governs. That is the arrangement the sliders were asked for and
it falls out of the parent link rather than out of any code.

VNAM (the record's own static multiplier) and UNAM (where the slider sits before
the player has touched it) multiply, and both are uint16 fixed point over 65535.

**The ragdoll slider ships at 0.6 and nothing in the mix compensates for it.**
That is deliberate, and the arithmetic is worth writing down because the obvious
alternative - ship at 0.6 and put 4.4 dB back into `fMasterGainDb` so the default
still lands at the tuned level - does not work and cannot be made to work.

Every multiplier in the engine's category chain caps at unity: VNAM, UNAM and the
player's slider are all 0-1, and the parent chain only attenuates. So the buffer
we hand the engine is the loudest this mod can ever be, and every slider position
is a cut from it. Room to turn it *up* can only be bought with default level.
There is no other currency.

Buying it in the mix instead was measured and rejected. `rds-verify --headroom`
mixes every composite in the corpus twice, once as it ships and once with a
make-up gain added to every cue - which is exactly what raising `fMasterGainDb`
does, master being a term in `cue.gainDb` that nothing upstream reads. Over 1820
composites at the shipping defaults the post-clip peak runs median 0.012, p90
0.161, p99 0.809, max 0.901, so the top percent is already hard against
`MixParams::clipCeiling` - about 3.8 dB inside the soft clip before anything is
added. Add 6 dB and the quiet 90% take it cleanly (p90 loses 0.23 dB) while the
loudest composites deliver **0.7 dB of it**, the tanh eating the other 5.3. Even
+2 dB reaches them by only 0.4 dB. Make-up gain lands on the quiet contacts and
not on the hero hits, so what it actually does is flatten the dynamic contrast
the mod is built around.

So the ragdoll slider takes the honest version: UNAM 0.6, the mod ships 4.4 dB
under its tuned calibration, the mix is untouched, every internal relationship
survives intact, and a player who wants the old level or louder moves one slider.

The gore slider stays at 1.0. Its job is to take gore *out*, and a default below
unity would ship the crunch and gore layers under the balance they were tuned at
- not a level change anybody asked for, just the mix being wrong out of the box.
It inherits the ragdoll slider's upward room through the parent link anyway.

VNAM is 1.0 on both, for the reason UNAM is not: the mod's levels are tuned in its
own ini, and a static multiplier is a second trim in a file nobody would think to
look in. The default belongs in the part the player can see and move.

One further level change is unavoidable and is $Master's own VNAM of 0.90, which
our voices did not pay while they had no category - about 0.9 dB, and the price of
being on the same bus structure as every other sound in the game.

Light-flagged (ESL), so it costs no plugin slot. Skyrim VR is SSE 1.4.15 and has
no light plugin support of its own, but every load order this ships into has it -
the one it was built against runs 1971 light plugins against 223 full ones, so
the support is not in question and a full slot would be one of the 31 it has
left. The record ids stay inside the 0x000-0xFFF an ESL may use.

The cost of the flag: an ESL's runtime FormID depends on its position among the
other ESLs, and the slider position the game saves to `[AudioMenu]` in
SkyrimPrefs.ini is keyed by that FormID. Reorder the light plugins and the
sliders go back to their UNAM defaults, which is the top of the range - so the
failure is "the player's trim is forgotten", never "the mod went silent".

Run from anywhere; writes beside the mod folder unless --out says otherwise.
"""

import argparse
import struct
from pathlib import Path

# Record and group headers carry an "internal version" the game stamps; 44 is
# what Skyrim SE/VR writes and what SkyrimNet.esp - which is known to load in
# this exact game - carries. Copied rather than invented.
INTERNAL_VERSION = 44

MASTER = 'Skyrim.esm'

# _AudioCategoryMaster, in Skyrim.esm. Every vanilla sound's bus ultimately hangs
# off it, and unlike AudioCategoryPausedDuringMenu it does not stop or reap the
# sounds under it when a menu opens - which our voices, being externally fed
# buffers the engine must not reclaim underneath us, require.
VANILLA_MASTER_CATEGORY = 0x000EB803

# Our own records, as they are stored in the file: index 0x01 because there is
# exactly one master ahead of us. The game remaps the index at load.
OWN_INDEX = 0x01
LOCAL_RAGDOLL = 0x000800
LOCAL_GORE = 0x000801

FLAG_SHOULD_APPEAR_ON_MENU = 2

# TES4 record flag 0x200 is ESL. It is set on the file header only; the records
# inside are still written under the plugin's own master index and the game
# remaps them to 0xFE at load.
RECORD_FLAG_LIGHT = 0x200


def zstring(s: str) -> bytes:
    return s.encode('cp1252') + b'\x00'


def sub(tag: str, data: bytes) -> bytes:
    return tag.encode('ascii') + struct.pack('<H', len(data)) + data


def pct(value: float) -> bytes:
    """VNAM/UNAM are unsigned 16-bit fixed point over 65535."""
    return struct.pack('<H', max(0, min(65535, round(value * 65535))))


def record(tag: str, form_id: int, body: bytes, flags: int = 0) -> bytes:
    header = (tag.encode('ascii') + struct.pack('<III', len(body), flags, form_id) +
              struct.pack('<HHHH', 0, 0, INTERNAL_VERSION, 0))
    return header + body


def snct(form_id: int, editor_id: str, name: str, parent: int, vnam: float, unam: float) -> bytes:
    body = (sub('EDID', zstring(editor_id)) +
            sub('FULL', zstring(name)) +
            sub('FNAM', struct.pack('<I', FLAG_SHOULD_APPEAR_ON_MENU)) +
            sub('PNAM', struct.pack('<I', parent)) +
            sub('VNAM', pct(vnam)) +
            sub('UNAM', pct(unam)))
    return record('SNCT', form_id, body)


def group(label: str, records: bytes) -> bytes:
    header = (b'GRUP' + struct.pack('<I', len(records) + 24) + label.encode('ascii') +
              struct.pack('<I', 0) + struct.pack('<HHI', 0, 0, 0))
    return header + records


def tes4(num_records: int, next_object_id: int, author: str) -> bytes:
    body = (sub('HEDR', struct.pack('<fII', 1.71, num_records, next_object_id)) +
            sub('CNAM', zstring(author)) +
            sub('MAST', zstring(MASTER)) +
            sub('DATA', struct.pack('<Q', 0)) +
            sub('INTV', struct.pack('<I', 1)))
    return record('TES4', 0, body, RECORD_FLAG_LIGHT)


def build() -> bytes:
    ragdoll = OWN_INDEX << 24 | LOCAL_RAGDOLL
    gore = OWN_INDEX << 24 | LOCAL_GORE

    records = (
        # 0.6 so the slider has somewhere to go in both directions. See the
        # module docstring: it costs 4.4 dB of default level and that cost cannot
        # be bought back in the mix.
        snct(ragdoll, 'RDS_AudioCategoryRagdoll', 'Ragdoll Sounds',
             VANILLA_MASTER_CATEGORY, 1.0, 0.6) +
        # Under ours rather than under $Master, which is the entire difference
        # between "a second slider" and "a slider inside the first one". At unity,
        # because taking gore out is what this one is for.
        snct(gore, 'RDS_AudioCategoryGore', 'Ragdoll Gore', ragdoll, 1.0, 1.0)
    )
    return tes4(2, LOCAL_GORE + 1, 'Physical Ragdoll Sounds') + group('SNCT', records)


def main() -> None:
    default = (Path(__file__).resolve().parents[3] / 'papyrus' / 'mods' /
               'Physical Ragdoll Sounds' / 'RagdollSounds.esp')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=default)
    args = parser.parse_args()

    data = build()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    print(f'wrote {args.out} ({len(data)} bytes)')


if __name__ == '__main__':
    main()
