"""Loader for the ragdoll impact takes.

The CSVs are the whole point and they are big, so nothing here holds more than
one take at a time unless a caller asks for it. The YAML sidecar is read with a
hand-rolled parser rather than PyYAML: it is machine-written by a known writer,
so the shapes it can take are few, and this keeps the scripts dependency-free
beyond pandas.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field

import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
# The scripted-run dataset. `Recordings/` held the 14 hand-driven takes this
# research started from; they were superseded and deleted, and nothing here
# reads them any more.
REC = os.path.normpath(os.path.join(HERE, "..", "NewRecordings"))

# The current header. `slide_speed` is listed only so a take from before the
# 2026-08-22 recorder fixes still parses if one turns up - it measures nothing
# and nothing in these scripts reads it. See Research/05-Capture-Pipeline-Issues.md.
NUMERIC = [
    "t_ms", "game_hour", "limb_index", "impact_speed", "body_speed",
    "angular_speed", "mass", "pos_x", "pos_y", "pos_z", "nrm_x", "nrm_y", "nrm_z",
    "vel_x", "vel_y", "vel_z", "manifold_first",
    "normal_speed", "tangent_speed", "limb_radius", "other_limb", "manifold_last",
    "dropped",
    "slide_speed",   # retired, never read
]

# 1 game unit is about 1.428 cm; Havok's own scale is the reciprocal.
UNITS_PER_METRE = 69.99


@dataclass
class Take:
    stem: str
    actor: str
    index: int
    meta: dict
    limbs: list = field(default_factory=list)   # dicts: limb_index, name, mass, body
    slots: list = field(default_factory=list)   # dicts: slot, site, name, armour_type, keywords
    coverage: dict = field(default_factory=dict)  # site -> {type, name, weight}
    csv_path: str = ""

    @property
    def bodies(self) -> dict:
        """Havok body pointer -> limb name, for this actor's own ragdoll."""
        return {l["body"]: l["name"] for l in self.limbs}

    @property
    def sex(self) -> str:
        return self.meta.get("actor.sex", "?")

    @property
    def note(self) -> str:
        """Which of the scripted run's takes this is, as the recorder wrote it."""
        return self.meta.get("recording.note", "")

    @property
    def interior(self) -> bool:
        return self.meta.get("environment.interior") == "true"

    COVERING_SITES = ("head", "torso", "hands", "forearms", "feet", "calves")

    @staticmethod
    def _is_skin(entry: dict) -> bool:
        """Is this `coverage` entry actually bare skin?

        The recorder's coverage map reports the heaviest ARMO covering a site,
        and TNG's skin is an ARMO: nameless, weightless, armour_type clothing.
        So a stripped actor reads as `clothing` on every site and `bare` never
        appears. Nameless and weightless is the tell - a real garment has at
        least one of the two. See Research/05-Capture-Pipeline-Issues.md.
        """
        try:
            weight = float(entry.get("weight", 0) or 0)
        except ValueError:
            weight = 0.0
        return not entry.get("name") and weight <= 0.0

    def covering(self, site: str) -> str:
        """bare / clothing / light / heavy for one body site."""
        entry = self.coverage.get(site)
        if not entry:
            return "bare"
        if self._is_skin(entry):
            return "bare"
        return entry.get("type", "clothing")

    def armour_kind(self) -> str:
        """One word for what the body was wearing: heavy, light, clothing, naked."""
        kinds = {self.covering(s) for s in self.COVERING_SITES}
        for k in ("heavy", "light", "clothing"):
            if k in kinds:
                return k
        return "naked"

    def load(self, kinds=None) -> pd.DataFrame:
        df = pd.read_csv(self.csv_path, low_memory=False)
        if kinds is not None:
            df = df[df["event"].isin(kinds)]
        for c in NUMERIC:
            if c in df.columns:
                df[c] = pd.to_numeric(df[c], errors="coerce")
        df["state"] = df["state"].fillna("")
        # A take from before the recorder learned about phases has no phase
        # column. Filling it with "unknown" rather than the ragdoll/animated
        # split reconstructed offline keeps "the recorder said so" and "we
        # worked it out from the state rows" from looking like the same claim.
        if "phase" not in df.columns:
            df["phase"] = "unknown"
        return df

    @property
    def has_phase(self) -> bool:
        """Did the recorder stamp the phase, or must it be reconstructed?"""
        return "phase" in pd.read_csv(self.csv_path, nrows=0).columns

    @property
    def dropped(self):
        """Events the ring threw away, or None for a take that never said.

        Anything but 0 means the take is incomplete and its counts must not be
        compared against a clean one.
        """
        value = self.meta.get("session.dropped")
        return int(value) if value not in (None, "") else None


_KV = re.compile(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*):\s*(.*)$")


def _scalar(text: str):
    text = text.split("   #")[0].strip()
    if text.startswith('"') and text.endswith('"') and len(text) >= 2:
        return text[1:-1]
    return text


def _parse_yaml(path: str) -> Take:
    meta, limbs, slots, coverage = {}, [], [], {}
    section, item, subsection = None, None, None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            stripped = line.lstrip()
            indent = len(line) - len(stripped)

            if indent == 0 and stripped.endswith(":"):
                section = stripped[:-1]
                item = subsection = None
                continue

            # `coverage:` is a map of site -> inline dict, not a list, so it
            # needs its own arm; without it the sites land in meta as raw text.
            if section == "armour" and indent == 2 and stripped == "coverage:":
                subsection = "coverage"
                item = None
                continue
            if subsection == "coverage":
                if indent >= 4 and ":" in stripped:
                    key, _, value = stripped.partition(":")
                    coverage[key.strip()] = {
                        k: v for k, v in re.findall(r'(\w+):\s*"?([^",}]*)"?', value)
                    }
                    continue
                subsection = None

            if stripped.startswith("- "):
                item = {}
                (limbs if section == "ragdoll" else slots).append(item)
                stripped = stripped[2:]
                indent += 2

            m = _KV.match(" " * indent + stripped)
            if not m:
                continue
            key, value = m.group(2), m.group(3)

            if item is not None and indent >= 6:
                if key == "keywords":
                    item["keywords"] = re.findall(r'"([^"]*)"', value)
                elif key == "form":
                    ids = re.findall(r'id: "([^"]*)"', value)
                    item["form_id"] = ids[0] if ids else ""
                else:
                    item[key] = _scalar(value)
                continue

            if value and not value.endswith(":"):
                meta[f"{section}.{key}"] = _scalar(value)

    stem = os.path.basename(path)[:-5]
    actor, _, idx = stem.partition("_impacts_log_")
    take = Take(stem=stem, actor=actor, index=int(idx), meta=meta, limbs=limbs, slots=slots,
                coverage=coverage,
                csv_path=os.path.join(os.path.dirname(path), stem + ".csv"))
    for l in take.limbs:
        l["limb_index"] = int(l.get("limb_index", -1))
        l["mass"] = float(l.get("mass", 0.0))
    return take


def takes() -> list[Take]:
    out = []
    for name in sorted(os.listdir(REC)):
        if name.endswith(".yaml"):
            out.append(_parse_yaml(os.path.join(REC, name)))
    out.sort(key=lambda t: (t.actor, t.index))
    return out


def by_stem() -> dict:
    return {t.stem: t for t in takes()}
