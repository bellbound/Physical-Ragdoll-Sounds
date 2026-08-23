"""What each take's actor was actually wearing, slot by slot."""
import rlib
for t in rlib.takes():
    print(f"== {t.stem}  sex={t.sex} weight={t.meta.get('actor.weight_slider')} "
          f"scale={t.meta.get('actor.scale')} race={t.meta.get('actor.race','')} "
          f"interior={t.meta.get('environment.interior')} cell={t.meta.get('environment.cell','')}")
    for s in t.slots:
        print(f"   slot {s.get('slot'):>2} {s.get('site','?'):<10} {s.get('armour_type','-'):<9} "
              f"rating={s.get('armour_rating','-'):>6} w={s.get('weight','-'):>6}  "
              f"{s.get('name','')!r:<28} {','.join(s.get('keywords',[]))[:70]}")
