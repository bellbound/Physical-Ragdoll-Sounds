> **Historical.** This is the prompt and the capture notes for the *first* dataset — the
> fourteen hand-driven leash-yank takes on Nazeem and Sybille Stentor. Those recordings have
> been deleted and replaced by the scripted run in `NewRecordings/`. Nothing below describes
> the data that is actually here; read [01-Dataset-Map.md](01-Dataset-Map.md) for that. Kept
> because it is where the questions the numbered documents answer came from.

2_ -> Pushed over - faceplant from standing
3_ -> Pushed over - faceplant from standing, then knocked over twice while getting uo
4_ -> Falling down stairs
5_ -> Heavy Impact against immoveable object, then tumble a bit 
6_ (170 ish impacts) -> Pushed over - faceplant from standing (like 2_) but indoors (higher fps)
7_ ->  Pushed over - faceplant from standing (female, no armor or cloth
8_ -> Heavy Imoulse, faceplant & slide over ground then heavy impact against immoveable object (female, no armor or cloth
9_ -> Tried to to same as 7_ (Pushed over - faceplant from standing (female, no armor or cloth)) -> Pushed over - faceplant from standing (female, no armor or cloth) (so we can see variance between similar falls)
10_ nazeem (248 impacs) -> Pushed over - faceplant from standing, but landed on back (actor is naked)
11_ nazeem in heavy armor -> Pushed over - faceplant from standing
12_ nazeem in heavy armor -> Heavy Imoulse, faceplant & slide over ground then medium heavy impact against immoveable object
13_ nazeem in heavy armor -> Absolute chaos (these are pretty dissimilar from each other, i just went haywire to produce a chaotic sample for each npc)
14_ female naked -> absolute chaos (these are pretty dissimilar from each other, i just went haywire to produce a chaotic sample for each npc)

Notes on the data:
- 2- 7 was a male in light armor (the indices should match there) - after that i just increased the count, but it should map to stentor after eg _8 -> = _1 stentor. at _10 -13 i switched to capturing nazeem again, 14 stentor - Try to 
- Some captures I made a mistake I discarded and did not include in the above list, but their recordings are still in recordings, please try to match the recordings to my discriptions and discard the rest
- 10 same npc but naked
- The npcs were ragdolled by me pulling a leash thats around their neck in VR (My vr controllers speed / my yank motion gave them their speed, so the initial impules are very variable)
- Whenever npcs fall down, they try to get up after ~1s or so when theyre no longer violently ragdolling - Not sure wether the get up animation produces impact data, but this animation would be very similar across all recordings, though some of the recordings might end mid animation
- Indoor and outdoor recordings are at different fps (outdoor ~30fps avg, indoor 60 fps average, needs to also work at anywhere between 24 and 144fps )
- Bones* contain the bones of the actors if needed, Bittercup is the player, no recordings of them. The other two are the male and female
- Data is huge, be careful  what you load into your context. Use scripts to preprocess

Start by taking apart the data:
- What can we infer / derive from the data? What is contained? What is well usable and what is too unreliable?
- Make a list of things we need to find out about the data
- What might need cleaning / verifying?
- What imprecisions / outside variables we currently have no data on do we know exist?
- What differences are there between the similar runs, males and females, does armor have an influnce on the physics?
- Does the data match up with the descriptions i made?
- Do we have enough here to make a semi physical impact sound system? 
- Any issues in our data capture pipeline in QuickModMenuNG that might lead us to wrong conclusions? (that was used to get this data) 
- When creating the sound system, what do need to keep in mind to make sure it always works reliably? 
- Create documents in Research so we can reference them later
- Anything else youd need me to record to answer unclear questions? 

All the knowledge we pull from the data will influence this system that i will build after: Physics based ragdoll sounds. But your task is only to analyse the data, keeping in mind what its for, but not yet designing the system.

Prompt for later:
```
Im trying to create a physical ragdoll sound mod for skyrim: Sounds based on actual bone impacts - with Different sounds based on what limb, what intensity, what armor is worn, and the ground - with the intensity also dependant on force

Create a scheme / plan for how this will work in general:
    - What different modes / sounds exist? 
        - Eg. a strong hand impact will cause us to ignore elbow impacts and play a single 'arm flop impact sound', while equal arm elbow and hand impacts will play sounds for both
        - How do we determine which sounds to play and which to ignore
            - Propose at least 2 distinct schemes here
    - How do other modern games handle this? Inspire from this
    - What unique / different sound effects do we want and how long should they be? 
        - Keep them to the minimum amount for the highest felt impact


Design the system such that we can easily tune it using a configuration:
    - Eg clear scalars to play with
    - subsystem boundries that we can toggle
    - If multiple approaches: clear distincations so we can choose what to apply it
    - This is so I can in a later step create a testbench app that allows me to finetune the sounds based on impact data. 
    - How much inspiration should we take from how sounds / ragdoll sounds would get physically created in the real world and how much game-logic-that-sounds-good should we do? What would work better on our data?
    ```