# Making the slide sound real — a proposal

*Plain language. No code knowledge assumed. Nothing here is built yet.*

---

## 1. Where this sits

The mod replaces Skyrim's own sound for a body that has gone limp — knocked
down, thrown, killed, dragged. Vanilla plays one dirt-ish thud on every single
collision and a falling body makes thirty to sixty of them, so the vanilla
result is a machine-gun rattle. We silence vanilla's version entirely and build
our own mix from scratch.

The mod's whole job is **choosing what not to play**. Out of those thirty to
sixty collisions it picks four to six moments that a listener would actually
notice, builds each one out of several layered sounds stacked a few
milliseconds apart, and leaves real silence in between.

That covers the *bangs*. But a fall has one stretch that is not a bang at all:
the part where the body is skidding across the floor. That is continuous, not a
moment, so it needs its own machinery — and that machinery is the **slide**.

Two pieces make it up:

- **The slide detector.** Watches the collisions. When contacts stop looking
  like hits and start looking like sideways rubbing, and that keeps up for long
  enough or covers enough ground, it declares the body to be sliding. It also
  decides when the slide is over — either the body left the ground, or the
  rubbing stopped.
- **The scrape loop.** The sound of it. One long grinding file played on repeat
  for exactly as long as the detector says the slide lasts, getting louder and
  higher-pitched the faster the body is moving.

Everything else in the mod — the impacts, the bone crunches, the head thuds,
the cloth rustle underneath — runs alongside it and is not affected by any of
this.

---

## 2. What is wrong with it today

Your two complaints are both real and I found the cause of each. There are four
problems in total; the two extra ones are the reason it reads as "someone
turned a noise on."

### Problem 1 — it fires on almost nothing, and then plays at full body volume

The detector asks: *has any single body part been rubbing sideways at a
reasonable speed for a moment or two?* **Any single part.** One foot dragging
qualifies exactly as much as a whole torso on the floor.

Then, having decided a slide is happening, the loudness is set by **how fast
the body's centre is travelling — and nothing else at all.** There is no
measurement anywhere in the mod of *how much of the body is actually touching
the ground*.

So a corpse being dragged by one ankle, with the rest of it up in the air,
produces the same grind as the same corpse lying flat on its back doing the
same speed. That is the mismatch you are hearing. It is not a tuning problem —
the quantity that would fix it does not currently exist.

### Problem 2 — the sound always comes from the middle of the NPC

Your ear is right, and it is not subtle: **every scrape in the game plays from
the NPC's root bone**, which is roughly the pelvis.

What makes this frustrating is that the engine half of the mod does the right
thing. When it starts a scrape it carefully attaches a note saying *"this came
from the left foot."* The game half then throws that note away and pins the
sound to the body centre regardless. There is even a comment explaining why —
the worry was that a scrape hopping between limbs frame by frame would smear
around instead of tracking — but the cure was applied to every scrape rather
than the ones that needed it.

This also affects **you**. The mod is explicitly designed to attach the
player's own ragdoll sounds to the player's bones, because at arm's length
collapsing everything to one point sounds like the audio is inside your head.
Loops were left out of that. So your own scrape is currently inside your head.

### Problem 3 — every surface sounds identical

Impacts get a surface skin: stone sounds hard and short, wood sounds hollow,
soft ground sounds dull. The scrape loop has **no surface variation at all** —
one file, used for flagstone, floorboards, dirt, snow and ice alike.

### Problem 4 — the file itself is a rumble with no grit in it

This is written down in the mod's own to-do list as a known failure. When the
reference recordings were measured, a real body sliding on stone turned out to
have **sixty-five little grit peaks per second** riding on top of the rumble.
That irregularity is what makes it sound like a body and not like a noise
generator. Our file has the rumble and none of the grit.

There is a matching gap: a slot called `scrape_grain` — occasional one-off
sounds for the moments where a limb *catches* on something — was specified,
then dropped before anything was built. Those catches are the other half of why
a real slide sounds alive.

### And one extra thing I found that you did not ask about

The scrape system **claims** every rubbing contact, meaning it takes ownership
and stops any other part of the mod from making a sound out of it. That is
correct while a slide is running — you don't want a thud on every frame of a
skid.

The problem is it claims them **all the time, even when no slide is happening
at all.** During an ordinary tumble roughly half of all worthwhile contacts
count as "rubbing," so half of them get claimed, no slide opens because it was
just one glancing knock, and the scrape system then plays nothing.

**Half the contacts in an ordinary knockdown are being deleted from the mix and
replaced with nothing.** That is very likely a big part of "it doesn't match
what's happening in game" — not because the scrape is wrong, but because the
scrape is quietly eating the impacts around it.

---

## 3. What I propose

### 3a. Your two-slot idea — yes, with one change

You are right and it is the correct shape for this. Two scrape sounds:

- **Body scrape** — a heavy, full-weight grind. Falling at a run and skidding
  several metres. Being dragged flat on your back.
- **Limb scrape** — light, dry, much quieter, its own volume control. A foot
  dragging, a hand trailing, a forearm scraping along.

The one change I would make: **do not make the mod pick one or the other.**

If it picks, there is a boundary, and boundaries are what people hear. A body
that starts flat and rolls onto one shoulder would audibly snap from one file
to the other halfway through. The rest of the mod is built specifically to
avoid this — impacts have no loudness "tiers", they blend layers continuously
so there is no step to notice.

So: **run both loops at the same time and cross-fade between them** on a single
continuous measurement. One foot down → the body loop is at silence and only
the quiet limb scrape plays. Body flops flat → the body loop swells in, the
limb scrape recedes. Rolls onto its side mid-slide → they trade smoothly and
you hear the weight change rather than a switch.

Each still gets its own volume, its own speed response and its own on/off
switch, exactly as you asked.

### 3b. The measurement that makes it work: how much body is on the floor

This is the missing piece, and it is what actually fixes "too sensitive."

Every body part in the mod already has a realistic weight attached to it — a
proper anatomical table, not the wonky numbers Skyrim's own ragdoll uses. Out
of a whole body: a foot is about **1.5 %**. A whole torso is about **45 %**. A
head is about **7 %**.

So instead of asking *"is anything rubbing?"* the mod should add up the weight
of the parts that **are** rubbing, and express it as a fraction of the whole
body. Call it the **contact fraction**.

- One foot dragging → 1.5 %
- Both feet and both shins → about 13 %
- A body lying flat and skidding → 60 % and up

Then:

- The **limb scrape** is what plays at the bottom of that range.
- The **body scrape** stays silent below about 20 %, fades in, and is at full
  weight by about 55 %.
- The current "any single part will do" trigger goes away entirely.

This is measured, not guessed — it comes straight from which parts reported
contact, which the mod already tracks per part. It costs nothing to compute.

Two things I want to be careful about, and will hold myself to:

1. This measurement shapes **the loop only**. It must never be allowed to
   silence an impact. The mod has a firm rule that no inferred state gets to
   suppress a real collision, and that rule earned itself the hard way.
2. If a body's pose is unavailable for some reason, the fallback is the
   *quieter* answer — treat it as limb-only. Never guess loud.

### 3c. Where the sounds come from

Three fixes, and they are separable:

**The limb scrape follows the actual limb.** The note the engine already
attaches gets honoured instead of discarded. A dragging foot sounds like it is
at the foot.

**With stickiness, so it doesn't smear.** The original worry was legitimate.
The rule: the limb scrape holds onto whichever limb it started on and only
moves once a *different* limb has been the main one rubbing for about a fifth
of a second. It moves on a natural seam in the loop, so nothing clicks.

**The body scrape stops using the pelvis.** Even for a genuine full-body slide,
the pelvis is not where the sound is. It should hang on whichever part of the
torso is actually lowest — the chest when face-down, the back when face-up.
Still one single point, so no smearing, but the right point.

**Optional, and worth trying:** for a really long body slide, a second quieter
copy at the far end of the body — shoulder and shin, say. A person sliding five
metres on their back is not a point source, it's a two-metre-long noise, and
two points is the cheapest way to suggest that. Off by default, and
automatically skipped for distant NPCs since the mod already strips loops out
past about ten metres.

### 3d. The head

Agreed — the head uses the limb scrape. A skull dragging on stone is a small
contact patch, not a body weight.

One refinement rather than a whole extra sound: play it as the **same limb file
pitched down slightly with its own small volume offset.** The head-impact sound
in the mod earns its "that was a head" quality from a faint ring rather than
from being a different sound, and the same trick applies here for a fraction of
the asset cost. If it turns out not to be enough, a proper head variant is a
file drop, not a code change.

### 3e. Making it stop sounding like a noise generator

Four things, in the order I would do them:

1. **Build the grain layer.** Sparse individual scrape-grains fired on the
   moments a limb actually catches, on top of the loop. This is the single
   biggest one. A slide's character is its irregularity, and right now the mod
   has none.

2. **Let the level move.** At the moment loudness follows the body's centre of
   mass, which is smooth by nature, and the mod additionally throws away any
   volume change smaller than about three-quarters of a decibel. The result is
   a level so steady it reads as a constant. Blending in a little of the
   *contact* speed — which is genuinely spiky, because limbs load and unload as
   they tumble — makes the grind breathe. This has to be a wobble *around* the
   body speed and not a replacement for it: it used to be driven purely by
   contact speed, that was wrong, and I do not want to walk back into it.

3. **Give the loop surface variants** — stone, wood, soft — exactly like the
   impacts already have. The mod's file system already handles falling back to
   whatever exists, so we can ship one and add the others later without
   touching anything.

4. **Fix how it ends.** Right now every slide fades out over a fixed 140
   milliseconds. A body grinding to a halt on gravel does not fade — it slows
   down. Since the volume already tracks speed, mostly this means shortening
   the fade and letting the deceleration do the work, plus a final grit or two
   as it stops. To be explicit: that is a tail on a loop that was already
   playing, not the mod inventing a collision that never happened.

### 3f. The claim bug

The scrape system should only take ownership of a rubbing contact **while a
slide is actually running.** Otherwise the contact falls through to the normal
impact path, which already knows how to make a glancing blow quiet.

One-line change in effect, and I would expect it to be immediately audible on
any ordinary knockdown — sounds that are currently silent will come back.

Related and worth measuring at the same time: the threshold that decides
"rubbing versus hit" is currently set so that **about half** of all worthwhile
contacts count as rubbing. The mod's own notes flag this as untested and
suspicious. It can be swept without rebuilding anything, so it is close to a
free experiment.

---

## 4. Is this a rewrite?

**No, and I would argue against making it one.** The architecture is sound —
"physics decides when, how loud, where and how long; design decides what it
sounds like" is exactly the right split, and this proposal does not disturb it.
The failure is not structural; it is that one number the design needs — how
much body is on the floor — was never measured, and the level had to stand on
speed alone in its absence.

In shape, the work is:

- one new measurement, sitting next to the existing slide bookkeeping
- one component that now produces two loops instead of one
- two new sound slots plus surface variants of both
- one fix in the game-side audio code so loops can hang on a limb
- one ownership fix

The only thing I would call a genuine refactor is the scrape component itself,
which currently assumes it will ever have exactly one loop running and needs to
handle two. That is small and self-contained.

---

## 5. What I would do in what order

| # | Change | Effort | What you'd hear |
|---|---|---|---|
| 1 | The claim fix | tiny | Impacts come back during tumbles. Immediate |
| 2 | Contact fraction + the two loops | medium | Feet and hands stop sounding like body slides. **This is your main complaint** |
| 3 | Limb scrape follows the limb; head included | small | The sound is where the limb is, including for you |
| 4 | Body scrape hangs on the lowest torso part | small | Full slides stop being a point at the pelvis |
| 5 | Build `scrape_limb` and `scrape_grain` | asset work | It stops sounding like a noise file |
| 6 | Surface variants of both loops | asset work | Stone and rugs stop being the same thing |
| 7 | Level wobble, and the deceleration ending | small | Grinds breathe and stop properly |

1 through 4 are all engine work and all testable in the tuning app against the
existing recordings before anything reaches the game. 5 and 6 are sound design
and can happen in parallel.

---

## 6. Two things I would like your call on

**How quiet should a limb scrape be?** My instinct is that a single dragging
foot should be *only just* audible at conversational distance — closer to the
cloth rustle than to the body grind. Easy to overshoot in the tuning app where
you're listening for it deliberately. Tell me which way you'd rather I err and
I'll set the default there.

**The double-point body slide** (§3c, the optional one). It is the one idea
here I am least sure of — it might read as beautifully long, or it might read
as two scrapes. Worth building behind a switch and judging by ear, or not worth
the complexity? Happy either way.
