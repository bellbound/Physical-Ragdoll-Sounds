
A body falling over produces something like thirty to sixty collisions in a couple of seconds. If you played a sound for each one you'd get a machine gun. Real games play about four to six sounds for that fall. So the job is mostly throwing things away — about nine out of ten, even of the ones that seem loud enough to keep.

The rule for what's decided where: physics decides when a sound happens, how loud, where, and how long. Design decides how many, which one, and what it sounds like. That split is because you can check the first group with your eyes — a hard landing must be loud, a scrape must last as long as the scrape — and get those wrong and it reads as broken instantly. Nobody can check the second group; nobody knows what a ragdoll head on floorboards should sound like.

The phases. A fall is tracked through seven stages, and each one gets a different budget:

- Launch — being knocked off balance
- Airborne — off the ground. Almost nothing plays; maybe a low rising whoosh
- Primary impact — the landing. This gets nearly the whole budget
- Tumble — rolling and flailing after
- Slide — sustained scraping along a surface
- Settle — coming to rest. Deliberately near-silent, because the last twenty collisions of any fall are just limbs flopping
- Rest — done. One quiet closing sound, because falls that just stop feel unfinished

How a single impact is built. This is the part that makes it sound like weight instead of a click. An impact isn't one sound — it's four, arriving at different times:

- at 0 ms, a bright thin tick — the contact itself, and the quietest part
- ~8 ms later, the surface — wood knock, stone, or something dull
- ~20 ms later, the low thud of flesh and mass
- ~65 ms later, a deep boom that slides downward in pitch, from about 150 Hz to 30 Hz — and it's the loudest part of all

So the sound starts bright and quiet and finishes dark, loud and short. That late boom isn't physical — nothing about a body landing delays its bass by 65 ms — it's a deliberate trick, and it's the single biggest reason a hit feels heavy. Measured it in all four Skate 3 clips and it's the thing the original design had missed entirely.

Loudness isn't tiers. A light tap is nearly all tick and no boom; a heavy fall is nearly all boom with the tick riding on top. Same handful of files, smooth continuum, no seams to hear.

How the throwing-away works. Four rules, in order: no two sounds closer than 46 ms (below that, ears stop hearing them as separate — they just turn to mud); several hits on one arm inside a moment collapse into one arm sound; anything more than 12 dB under what's currently ringing is dropped entirely rather than played quietly; and what survives is grouped into short bursts of three to five, with real silence — a third of a second or more — between bursts. A three-second tumble should be four bursts of sound and a lot of nothing.

Underneath all of it runs a quiet continuous bed of cloth rustle, about thirty decibels down, which papers over the joins.
