#include "GameFeed.h"

#include "rds/ConfigManager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "HiggsLink.h"
#include "VanillaGate.h"

namespace rds::game {
namespace {

/// Cost filter only, well under the engine's tunable `ingest.minImpactSpeed`:
/// anything dropped here can never be tuned back.
constexpr float kMinimumImpactSpeed = 5.0f;

/// ~0.5s. Throttles ragdoll rebuilds without going deaf during a rebuild storm
/// (six in three seconds on a disturbed standing actor).
constexpr int kRebuildTicks = 16;

/// ~3s of animated-and-untouched before we let go. A tumble can report animated
/// for a tick mid-fall; re-attaching costs the world write lock.
constexpr int kIdleTicksBeforeUntrack = 90;

std::chrono::steady_clock::time_point g_epoch = std::chrono::steady_clock::now();

using Vec = std::array<float, 3>;

[[nodiscard]] Vec LoadVector(const RE::hkVector4& v) {
    alignas(16) float raw[4]{};
    _mm_store_ps(raw, v.quad);
    return {raw[0], raw[1], raw[2]};
}

void StoreVector(Vec3& out, const RE::hkVector4& v, float scale) {
    const Vec raw = LoadVector(v);
    out = {raw[0] * scale, raw[1] * scale, raw[2] * scale};
}

[[nodiscard]] float Dot(const Vec& a, const Vec& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] Vec Cross(const Vec& a, const Vec& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

[[nodiscard]] float Norm(const Vec& a) { return std::sqrt(Dot(a, a)); }

/// Surface speed at `point`, not centre-of-mass speed: `v + w x (p - com)`. A
/// limb pivoting about its shoulder has a still centre and a fast hand, which is
/// what separates a scrape from a thud. Havok units; the caller scales.
[[nodiscard]] Vec PointVelocity(const RE::hkpRigidBody& body, const Vec& point) {
    const auto linear = LoadVector(body.motion.linearVelocity);
    const auto angular = LoadVector(body.motion.angularVelocity);
    // centerOfMass1 is the current end of the swept transform, not the start.
    const auto centre = LoadVector(body.motion.motionState.sweptTransform.centerOfMass1);
    const Vec arm{point[0] - centre[0], point[1] - centre[1], point[2] - centre[2]};
    const auto spin = Cross(angular, arm);
    return {linear[0] + spin[0], linear[1] + spin[1], linear[2] + spin[2]};
}

/// `kGetUp` is separate from `kAnimated` because the get-up blend carries the
/// largest and least meaningful closing speeds in the dataset: it is simulation
/// unwinding into animation, not a fall.
[[nodiscard]] ActorPhase PhaseOf(bool ragdolled, RE::KNOCK_STATE_ENUM knock) {
    if (ragdolled) {
        return ActorPhase::kRagdoll;
    }
    if (knock == RE::KNOCK_STATE_ENUM::kGetUp) {
        return ActorPhase::kGetUp;
    }
    return ActorPhase::kAnimated;
}

/// Does this actor take vanilla's body impacts away from it this tick?
/// Rule: wherever we answer for the sound. `kRagdoll` and `kAnimated` (in
/// animated mode) because we play those collisions; `kGetUp` because the blend
/// back to animation drives contacts no fall produced, which vanilla renders as
/// a burst of body impacts. See VanillaGate.h.
[[nodiscard]] bool ClaimsVanillaImpacts(ActorPhase phase, bool animatedMode) {
    switch (phase) {
        case ActorPhase::kRagdoll:
        case ActorPhase::kGetUp:
            return true;
        case ActorPhase::kAnimated:
            return animatedMode;
        default:
            return false;
    }
}

/// Which armour slot covers a body site. Timbre only, never physics (07 s11).
/// Skyrim's body slot covers torso and legs together, so thighs fall back to it.
[[nodiscard]] RE::BGSBipedObjectForm::BipedObjectSlot SlotForSite(LimbSite site) {
    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
    switch (site) {
        case LimbSite::kHead:
        case LimbSite::kNeck:
            return Slot::kHead;
        case LimbSite::kHand:
            return Slot::kHands;
        case LimbSite::kForearm:
            return Slot::kForearms;
        case LimbSite::kFoot:
            return Slot::kFeet;
        case LimbSite::kCalf:
            return Slot::kCalves;
        case LimbSite::kTorso:
        case LimbSite::kUpperArm:
        case LimbSite::kThigh:
        case LimbSite::kUnknown:
        default:
            return Slot::kBody;
    }
}

/// What is on a site, resolved through the slot that covers it.
///
/// Empty slots fall back to the body piece (a cuirass with no gauntlets still
/// means the forearm is not bare); `Armor:bInheritFromBody` turns that off for
/// the case it gets wrong, heavy boots on an otherwise naked body.
///
/// The nameless-and-weightless test decides whether `armor_bare` ever fires on a
/// modded body: TNG's skin is a real TESObjectARMO on five slots, so without it
/// a stripped subject reads as clothed. Mirrors `CoverageFrom` in the recording
/// loader, which the live path used to disagree with.
[[nodiscard]] Coverage CoverageForSite(RE::Actor& actor, LimbSite site,
                                       const ArmorConfig& armor) {
    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
    const Slot slot = SlotForSite(site);

    auto* worn = actor.GetWornArmor(slot);
    if (worn == nullptr && slot != Slot::kBody && armor.inheritFromBody) {
        worn = actor.GetWornArmor(Slot::kBody);
    }
    if (worn == nullptr) {
        return Coverage::kBare;
    }
    if (worn->IsHeavyArmor()) {
        return Coverage::kHeavy;
    }
    if (worn->IsLightArmor()) {
        return Coverage::kLight;
    }
    // Neither heavy nor light, so it is clothing - unless it is nameless and
    // weightless, which is what a body-replacer's skin looks like from here.
    if (armor.bareIsNaked) {
        const char* name = worn->GetName();
        const bool nameless = name == nullptr || *name == '\0';
        if (nameless && worn->weight <= 0.0f) {
            return Coverage::kBare;
        }
    }
    return Coverage::kCloth;
}

void SetText(char (&out)[24], std::string_view text) {
    const auto count = std::min(text.size(), sizeof(out) - 1);
    std::memcpy(out, text.data(), count);
    out[count] = '\0';
}

/// A state row. Through the ring rather than straight to the drain so it keeps
/// its place in time against the contacts either side: an out-of-order
/// `ragdoll_start` puts the phase machine a tick behind the fall.
void PushState(ContactRing& ring, ActorId actor, std::string_view state) {
    FeedEvent event{};
    event.timeMs = NowMs();
    event.actorId = actor;
    event.kind = EventKind::kState;
    SetText(event.text, state);
    ring.Push(event);
}

/// One `kLimbSample` per ragdoll limb: the only measurement of where the body
/// actually is. Everything else is a collision, and collisions are dense exactly
/// when a fall is busy - so air time inferred from the gaps between them peaks
/// at `ragdoll_start` and vanishes during the landing it exists to find.
///
/// Game thread only, from PublishTick, which is why the pose is published from
/// the tick rather than gathered in the listener.
///
/// The text is left empty on purpose: the recorder stamps its state-change
/// samples with that state, and `rds::pose::IsTickSample` tells the two apart on
/// exactly this field.
void PushLimbSamples(ContactRing& ring, const RagdollView& ragdoll, ActorId actor,
                     ActorPhase phase, bool inCombat) {
    const float scale = RE::bhkWorld::GetWorldScaleInverse();
    const TimeMs now = NowMs();
    for (std::size_t index = 0; index < ragdoll.limbs.size(); ++index) {
        auto* body = ragdoll.limbs[index].body.get();
        if (body == nullptr) {
            continue;
        }
        FeedEvent event{};
        event.timeMs = now;
        event.kind = EventKind::kLimbSample;
        event.actorId = actor;
        event.limbIndex = static_cast<std::uint16_t>(index);
        event.phase = phase;
        event.inCombat = inCombat;
        event.mass = body->motion.GetMass();
        event.limbRadius = body->motion.motionState.objectRadius * scale;
        event.bodySpeed = body->motion.linearVelocity.Length3() * scale;
        event.angularSpeed = body->motion.angularVelocity.Length3();
        StoreVector(event.velocity, body->motion.linearVelocity, scale);
        StoreVector(event.position, body->motion.motionState.transform.translation, scale);
        ring.Push(event);
    }
}

}  // namespace

TimeMs NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now() - g_epoch).count();
}

void ResetClock() { g_epoch = std::chrono::steady_clock::now(); }

// -- the contact listener ----------------------------------------------------

/// One per ragdoll limb, so the callback knows which limb it is without a lookup.
/// Holds a reference to its body so it cannot be freed while still registered.
class LimbListener final : public RE::hkpContactListener {
public:
    /// `pub` outlives every listener it owns (Tracked is heap-allocated and never
    /// moved). It carries the two things only the game thread can work out:
    /// whether the actor is ragdolling, and what the land under them is.
    LimbListener(RE::hkRefPtr<RE::hkpRigidBody> body, ContactRing* ring,
                 const ActorPublication* pub, ActorId actorId, std::uint16_t limbIndex)
        : m_body(std::move(body)),
          m_ring(ring),
          m_pub(pub),
          m_actorId(actorId),
          m_limbIndex(limbIndex) {}

    ~LimbListener() override { Detach(); }

    LimbListener(const LimbListener&) = delete;
    LimbListener& operator=(const LimbListener&) = delete;

    /// Registers on the body and asks Havok for every step's contact points.
    /// `contactPointCallbackDelay` is the step gap between callbacks; Skyrim
    /// leaves it high. Saved so the body is handed back as it was found.
    void Attach() {
        if (m_attached || !m_body) {
            return;
        }
        m_savedDelay = m_body->contactPointCallbackDelay;
        m_body->contactPointCallbackDelay = 0;
        m_body->AddContactListener(this);
        m_attached = true;
    }

    void Detach() {
        if (m_attached && m_body) {
            m_body->RemoveContactListener(this);
            m_body->contactPointCallbackDelay = m_savedDelay;
        }
        m_attached = false;
    }

    void ContactPointCallback(const RE::hkpContactPointEvent& event) override {
        if (event.separatingVelocity == nullptr || event.contactPoint == nullptr ||
            m_ring == nullptr) {
            return;
        }

        // THE GATE. The only reason the mod stays quiet while NPCs walk around:
        // the ragdoll bodies are there and colliding the whole time.
        // `bAnimatedMode` opens it, read as a second relaxed load off the same
        // publication rather than from the config - this is a Havok worker thread
        // inside the solver and may not take a lock. No publication, no gate.
        const auto phase = m_pub != nullptr
                               ? static_cast<ActorPhase>(m_pub->phase.load(std::memory_order_relaxed))
                               : ActorPhase::kUnknown;
        const bool inCombat =
            m_pub != nullptr && m_pub->inCombat.load(std::memory_order_relaxed);
        if (phase != ActorPhase::kRagdoll &&
            !(m_pub != nullptr && m_pub->hearAnimated.load(std::memory_order_relaxed))) {
            return;
        }

        // Havok's sign convention: negative separating velocity is closing.
        const float scale = RE::bhkWorld::GetWorldScaleInverse();
        const float closing = -*event.separatingVelocity * scale;
        if (!(closing >= kMinimumImpactSpeed)) {
            return;
        }

        FeedEvent record{};
        std::uint32_t otherIndex = 0;
        if (!Fill(record, event.bodies, otherIndex)) {
            return;
        }
        record.phase = phase;
        record.inCombat = inCombat;
        record.manifoldFirst = event.firstCallbackForFullManifold;
        record.manifoldLast = event.lastCallbackForFullManifold;
        record.impactSpeed = closing;
        StoreVector(record.position, event.contactPoint->position, scale);
        StoreVector(record.normal, event.contactPoint->separatingNormal, 1.0f);

        Decompose(record, event, scale);
        Materialise(record, event, otherIndex);

        m_ring->Push(record);
    }

private:
    /// Everything the callback needs off our own body. False when the event is
    /// not about this listener's body - shouldn't happen, costs one compare.
    bool Fill(FeedEvent& record, RE::hkpRigidBody* const (&bodies)[2],
              std::uint32_t& otherIndex) const {
        auto* self = m_body.get();
        if (self == nullptr) {
            return false;
        }
        const bool selfIsA = bodies[0] == self;
        if (!selfIsA && bodies[1] != self) {
            return false;
        }
        otherIndex = selfIsA ? 1u : 0u;
        const auto* other = bodies[otherIndex];

        const float scale = RE::bhkWorld::GetWorldScaleInverse();
        record.timeMs = NowMs();
        record.kind = EventKind::kImpact;
        record.actorId = m_actorId;
        record.limbIndex = m_limbIndex;
        record.mass = self->motion.GetMass();
        record.bodySpeed = self->motion.linearVelocity.Length3() * scale;
        record.angularSpeed = self->motion.angularVelocity.Length3();
        // The only size reachable without walking a shape hierarchy, and enough
        // to tell a hand from a torso when the bone name is unrecognised.
        record.limbRadius = self->motion.motionState.objectRadius * scale;
        StoreVector(record.velocity, self->motion.linearVelocity, scale);
        record.otherBody = reinterpret_cast<std::uint64_t>(other);
        record.otherLimb = -1;
        if (other != nullptr) {
            if (const auto* collidable = other->GetCollidable()) {
                record.otherLayer = static_cast<ColLayer>(collidable->GetCollisionLayer());
            }
        }
        return true;
    }

    /// Split the relative motion at the contact into normal and tangential.
    ///
    /// Both bodies contribute: a limb landing on a moving limb slides by the
    /// difference. `normalSpeed` duplicates Havok's `separatingVelocity` by our
    /// own arithmetic so the two can be compared - a disagreement is a solver
    /// blow-up caught without a threshold (07 s2).
    ///
    /// ALWAYS body[0] minus body[1], never "us minus them": the normal's
    /// direction is fixed by that ordering, not by which listener Havok called,
    /// so subtracting in listener order flips the sign on 23.3% of contacts.
    static void Decompose(FeedEvent& record, const RE::hkpContactPointEvent& event, float scale) {
        const auto* first = event.bodies[0];
        const auto* second = event.bodies[1];
        if (first == nullptr || second == nullptr || event.contactPoint == nullptr) {
            return;
        }
        const auto point = LoadVector(event.contactPoint->position);
        const auto theirs = PointVelocity(*second, point);
        const auto ours = PointVelocity(*first, point);
        const Vec relative{ours[0] - theirs[0], ours[1] - theirs[1], ours[2] - theirs[2]};

        const auto normal = LoadVector(event.contactPoint->separatingNormal);
        const float along = Dot(relative, normal);
        const Vec tangent{relative[0] - normal[0] * along, relative[1] - normal[1] * along,
                          relative[2] - normal[2] * along};
        // Same sign convention as impactSpeed: closing is positive.
        record.normalSpeed = -along * scale;
        // A length, so it never depended on the ordering.
        record.tangentSpeed = Norm(tangent) * scale;
    }

    /// The surface material, and where it came from. The contact's own shape
    /// answers for geometry; terrain does not (`hkpBvTreeShape` returns nothing
    /// for any shape key), so the land record sampled by the tick stands in and
    /// the event records which of the two it was (07 s8).
    void Materialise(FeedEvent& record, const RE::hkpContactPointEvent& event,
                     std::uint32_t otherIndex) const {
        record.otherMaterial = MaterialOf(event, otherIndex);
        if (record.otherMaterial != 0) {
            record.materialSource = MaterialSource::kShape;
            return;
        }
        if (record.otherLayer == ColLayer::kGround && m_pub != nullptr) {
            if (const auto land = m_pub->material.load(std::memory_order_relaxed); land != 0) {
                record.otherMaterial = land;
                record.materialSource = MaterialSource::kTerrain;
            }
        }
    }

    /// The Skyrim surface material of whatever was hit, or 0. Every step is
    /// null-checked: this runs inside the solver.
    [[nodiscard]] static std::uint32_t MaterialOf(const RE::hkpContactPointEvent& event,
                                                  std::uint32_t otherIndex) {
        const auto* other = event.bodies[otherIndex];
        if (other == nullptr) {
            return 0;
        }
        const auto* collidable = other->GetCollidable();
        const auto* shape = collidable != nullptr ? collidable->shape : nullptr;
        auto* bhk = shape != nullptr ? shape->userData : nullptr;
        if (bhk == nullptr) {
            return 0;
        }
        // The shape key picks the leaf out of a compound shape; without it every
        // rock in a mesh reads as the same material.
        auto key = RE::HK_INVALID_SHAPE_KEY;
        if (const auto* keys = event.GetShapeKeys(otherIndex)) {
            key = keys[0];
        }
        return static_cast<std::uint32_t>(bhk->GetMaterialID(key));
    }

    RE::hkRefPtr<RE::hkpRigidBody> m_body;
    ContactRing* m_ring{};
    const ActorPublication* m_pub{};
    ActorId m_actorId{};
    std::uint16_t m_limbIndex{};
    std::uint16_t m_savedDelay{};
    bool m_attached{false};
};

// -- GameFeed ----------------------------------------------------------------

GameFeed::GameFeed() { m_scratch.reserve(256); }

GameFeed::~GameFeed() { Clear(); }

void GameFeed::Install() {
    if (m_installed) {
        return;
    }
    m_installed = true;
    ResetClock();
    spdlog::info("feed: armed; listeners attach per actor as they are knocked down");
}

void GameFeed::SetCullRadius(float units) { m_cullRadius = std::max(1.0f, units); }

void GameFeed::SetGameIntegration(const GameIntegrationConfig& game) {
    m_animatedMode = game.animatedMode;
    // Rustle and air time are both built off the pose, so an animated actor is
    // only worth sampling when one of them is on. The slide is not in the list:
    // it opens off graze contacts and falls back cleanly without a pose.
    m_animatedPose = game.animatedMode && (game.animatedRustle || game.animatedAirTime);
}

void GameFeed::SetBodySampleEveryNTicks(std::int32_t ticks) {
    m_bodySampleEveryNTicks = std::max(0, ticks);
}

void GameFeed::PushEvent(const FeedEvent& event) { m_ring.Push(event); }

std::uint64_t GameFeed::Dropped() const { return m_ring.Dropped(); }

void GameFeed::Attach(Tracked& tracked, ActorId actor) {
    if (tracked.attached || !tracked.ragdoll.Valid()) {
        return;
    }
    // Under the world write lock, once per ragdoll rather than once per limb.
    RE::BSWriteLockGuard worldLock{tracked.ragdoll.world->worldLock};
    for (auto& listener : tracked.listeners) {
        listener->Attach();
    }
    tracked.attached = true;
    spdlog::debug("feed: attached {} limb listener(s) to {:08X}", tracked.listeners.size(), actor);
}

void GameFeed::Detach(Tracked& tracked) {
    if (!tracked.attached) {
        tracked.listeners.clear();
        return;
    }
    if (tracked.ragdoll.world) {
        RE::BSWriteLockGuard worldLock{tracked.ragdoll.world->worldLock};
        for (auto& listener : tracked.listeners) {
            listener->Detach();
        }
    } else {
        // World gone. Still detach: that only touches the entity's own array,
        // and we hold a reference to every body.
        for (auto& listener : tracked.listeners) {
            listener->Detach();
        }
    }
    tracked.attached = false;
    tracked.listeners.clear();
}

void GameFeed::BuildProfile(Tracked& tracked, RE::Actor& actor) {
    ActorProfile& profile = tracked.pub.profile;
    profile.actorId = actor.GetFormID();
    profile.isPlayer = actor.IsPlayerRef();
    profile.scale = actor.GetScale();

    const char* name = actor.GetName();
    profile.name = name != nullptr ? name : "";

    profile.limbs.clear();
    profile.limbs.reserve(tracked.ragdoll.limbs.size());
    // Once per profile, not per limb: Algorithm() copies (so the caller cannot
    // read a config mid-reload), and eighteen copies per knockdown for two
    // booleans is a real cost.
    const ArmorConfig armor = ConfigManager::Get().Algorithm(ActorMode::kRagdoll).armor;
    for (const RagdollBody& body : tracked.ragdoll.limbs) {
        LimbInfo limb;
        limb.boneName = body.name;
        // From the NAME, never the index: 18 bodies in a fixed order is the
        // vanilla humanoid skeleton and nothing else.
        limb.site = SiteFromBoneName(body.name);
        limb.chain = ChainFromBoneName(body.name);
        limb.coverage = CoverageForSite(actor, limb.site, armor);
        limb.havokMass = body.mass;
        limb.radius = body.radius;
        limb.bodyId = reinterpret_cast<std::uint64_t>(body.body.get());
        profile.limbs.push_back(std::move(limb));
    }
}

bool GameFeed::ShouldTrack(RE::Actor& actor, float distanceSq) const {
    if (actor.IsDisabled() || actor.IsDeleted()) {
        return false;
    }
    if (distanceSq > m_cullRadius * m_cullRadius) {
        return false;
    }
    if (!actor.Is3DLoaded()) {
        return false;
    }
    // Animated mode wants everybody inside the radius, on their feet or not.
    if (m_animatedMode) {
        return true;
    }
    // Knocked covers the lead-in states, so the listeners are on in time -
    // waiting for IsInRagdollState would miss a knockdown's first contacts.
    // The dead come in by the first term: IsInRagdollState stays true for a
    // corpse, whose knock state has usually gone back to normal by then.
    auto* state = actor.AsActorState();
    const auto knock = state != nullptr ? state->GetKnockState() : RE::KNOCK_STATE_ENUM::kNormal;
    return actor.IsInRagdollState() || knock != RE::KNOCK_STATE_ENUM::kNormal;
}

void GameFeed::RefreshListener() {
    // The camera, not the body: the recorder kept head tracking out of its rows,
    // a live mod wants the ears.
    auto* camera = RE::PlayerCamera::GetSingleton();
    if (camera != nullptr && camera->cameraRoot) {
        const auto& world = camera->cameraRoot->world;
        m_listener.position = {world.translate.x, world.translate.y, world.translate.z};
        const auto forward = world.rotate.GetVectorY();
        m_listener.facing = {forward.x, forward.y, forward.z};
    } else if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        const auto p = player->GetPosition();
        m_listener.position = {p.x, p.y, p.z};
        const float angle = player->GetAngleZ();
        m_listener.facing = {std::sin(angle), std::cos(angle), 0.0f};
    }
    // Non-zero, or the engine reads the listener as absent and pins every actor
    // to the Full tier.
    m_listener.timeMs = NowMs();
}

void GameFeed::PublishTick(float frameTimeSec) {
    m_frameTimeSec = frameTimeSec;
    ++m_tick;
    RefreshListener();

    std::lock_guard lock{m_mutex};

    // Refilled from scratch each tick: a claim is a statement about this tick,
    // and an actor can leave the set by getting up, walking out or losing its 3D.
    BeginVanillaGate();

    // Both hands, once a tick rather than twice per tracked actor. 0 for an empty
    // hand, a grabbed crate, or any call outside VR.
    const std::uint32_t heldLeft = higgs::HeldActorId(true);
    const std::uint32_t heldRight = higgs::HeldActorId(false);

    const Vec3 ears = m_listener.position;
    const auto distanceSqTo = [&ears](const RE::NiPoint3& p) {
        const float dx = p.x - ears.x;
        const float dy = p.y - ears.y;
        const float dz = p.z - ears.z;
        return dx * dx + dy * dy + dz * dz;
    };

    // -- pick up anyone newly knocked down ----------------------------------
    const auto consider = [&](RE::Actor* actor) {
        if (actor == nullptr) {
            return;
        }
        const ActorId id = actor->GetFormID();
        const float distSq = distanceSqTo(actor->GetPosition());
        const bool wanted = ShouldTrack(*actor, distSq);
        const auto it = m_actors.find(id);
        if (it != m_actors.end() || !wanted) {
            return;
        }
        auto tracked = std::make_unique<Tracked>();
        tracked->ragdoll = CaptureRagdoll(*actor);
        if (!tracked->ragdoll.Valid()) {
            return;
        }
        tracked->ref = RE::NiPointer<RE::TESObjectREFR>{actor};
        BuildProfile(*tracked, *actor);
        tracked->pub.tracked.store(true, std::memory_order_relaxed);
        // Before the listeners go on, not on the refresh pass below: the gate has
        // to be right from the first contact this actor's new listeners see.
        tracked->pub.hearAnimated.store(m_animatedMode, std::memory_order_relaxed);

        for (std::size_t i = 0; i < tracked->ragdoll.limbs.size(); ++i) {
            tracked->listeners.push_back(std::make_unique<LimbListener>(
                tracked->ragdoll.limbs[i].body, &m_ring, &tracked->pub, id,
                static_cast<std::uint16_t>(i)));
        }
        Tracked& ref = *tracked;
        m_actors.emplace(id, std::move(tracked));
        Attach(ref, id);
        spdlog::info("feed: tracking {:08X} {} ({} limbs){}", id,
                     ref.pub.profile.name.empty() ? "?" : ref.pub.profile.name,
                     ref.pub.profile.limbs.size(), actor->IsDead() ? ", dead" : "");
    };

    consider(RE::PlayerCharacter::GetSingleton());
    if (auto* processLists = RE::ProcessLists::GetSingleton()) {
        // Every process list, not just the high one: the process manager demotes
        // an actor out of high once it has no AI left to run, which is exactly
        // what a settled corpse is. ShouldTrack discards whatever the other three
        // add that is out of earshot or has no 3D.
        for (auto* list : {&processLists->highActorHandles,
                           &processLists->middleHighActorHandles,
                           &processLists->middleLowActorHandles,
                           &processLists->lowActorHandles}) {
            for (auto& handle : *list) {
                if (auto actor = handle.get()) {
                    consider(actor.get());
                }
            }
        }
    }

    // -- refresh what is already tracked ------------------------------------
    for (auto it = m_actors.begin(); it != m_actors.end();) {
        Tracked& tracked = *it->second;
        auto* refr = tracked.ref.get();
        auto* actor = refr != nullptr ? refr->As<RE::Actor>() : nullptr;

        if (actor == nullptr || !actor->Is3DLoaded()) {
            PushState(m_ring, it->first, "actor_gone");
            Detach(tracked);
            it = m_actors.erase(it);
            continue;
        }

        const RE::NiPoint3 position = actor->GetPosition();
        const float distSq = distanceSqTo(position);
        if (distSq > m_cullRadius * m_cullRadius) {
            PushState(m_ring, it->first, "actor_gone");
            Detach(tracked);
            spdlog::debug("feed: untracking {:08X}, past the cull radius", it->first);
            it = m_actors.erase(it);
            continue;
        }

        auto* state = actor->AsActorState();
        const auto knock =
            state != nullptr ? state->GetKnockState() : RE::KNOCK_STATE_ENUM::kNormal;
        const bool ragdolled = actor->IsInRagdollState();
        const ActorPhase phase = PhaseOf(ragdolled, knock);

        // Asked of the actor rather than of the player: the axis is per body, and
        // a guard swinging a sword and the man he just knocked down are in one
        // fight and want opposite tuning. Only asked while they are upright -
        // `ModeFor` answers ragdoll whatever this says once they are down, and
        // `IsInCombat` walks the actor's combat groups, which is not free.
        const bool inCombat = phase == ActorPhase::kAnimated && actor->IsInCombat();

        // Phase first: the callback reads it, and the rest of the tick is
        // downstream of it being right.
        tracked.pub.phase.store(static_cast<std::uint8_t>(phase), std::memory_order_relaxed);
        tracked.pub.hearAnimated.store(m_animatedMode, std::memory_order_relaxed);
        tracked.pub.inCombat.store(inCombat, std::memory_order_relaxed);

        // The same answer to the impact manager, which cannot ask an actor
        // anything: vanilla's body impacts drop only where we answer for them.
        if (ClaimsVanillaImpacts(phase, m_animatedMode)) {
            AddVanillaGate(position.x, position.y, position.z);
        }

        // The engine opens a knockdown on ragdoll_start and closes it on
        // ragdoll_end or knock_get_up. Without these it acquires the actor on the
        // first contact and never lets go: the phase machine never reaches Rest.
        if (phase != tracked.lastPhase) {
            if (phase == ActorPhase::kRagdoll) {
                PushState(m_ring, it->first, "ragdoll_start");
                tracked.held = false;
            } else if (tracked.lastPhase == ActorPhase::kRagdoll) {
                PushState(m_ring, it->first,
                          phase == ActorPhase::kGetUp ? "knock_get_up" : "ragdoll_end");
            }
            tracked.lastPhase = phase;
        }

        // The mode edges, on the same terms as the phase edges above: published
        // when they move, so a recording carries the moment an actor entered a
        // fight and a replay puts them in the same column the game did. Without
        // these a capture of a brawl would replay as though nobody was fighting.
        if (inCombat != tracked.lastInCombat) {
            PushState(m_ring, it->first, inCombat ? "combat_start" : "combat_stop");
            tracked.lastInCombat = inCombat;
        }

        // Whether the player has this body in hand, on the same terms as the
        // phase: game thread, published on the edge, through the ring.
        // `AccumDamage:bRequireHeld` is the one rule that reads it. `ragdoll_start`
        // clears the flag because it hands the engine a fresh runtime, so a body
        // already held when it went limp must be reported held again.
        const bool held = (heldLeft != 0 && it->first == heldLeft) ||
                          (heldRight != 0 && it->first == heldRight);
        if (held != tracked.held) {
            PushState(m_ring, it->first, held ? "held_start" : "held_stop");
            tracked.held = held;
        }

        // The pose, same terms: game thread only, and the engine cannot measure a
        // fall without it. Skipped while the actor is on its feet, so a village of
        // walking NPCs costs nothing unless animated mode wants it.
        if (m_bodySampleEveryNTicks > 0 &&
            (phase != ActorPhase::kAnimated || m_animatedPose) &&
            m_tick % static_cast<std::uint64_t>(m_bodySampleEveryNTicks) == 0) {
            PushLimbSamples(m_ring, tracked.ragdoll, it->first, phase, inCombat);
        }

        // Game thread only, hence published rather than read in the callback.
        if (auto* tes = RE::TES::GetSingleton()) {
            const auto material = tes->GetLandMaterialType(actor->GetPosition());
            tracked.pub.material.store(static_cast<std::uint32_t>(material),
                                       std::memory_order_relaxed);
        }

        // Rebuilt on cell change, 3D reload, and repeatedly on a disturbed
        // standing actor. Re-capture rather than go deaf.
        if (!BodiesLive(tracked.ragdoll)) {
            if (--tracked.rebuildCountdown <= 0) {
                tracked.rebuildCountdown = kRebuildTicks;
                Detach(tracked);
                tracked.ragdoll = CaptureRagdoll(*actor);
                if (tracked.ragdoll.Valid()) {
                    BuildProfile(tracked, *actor);
                    for (std::size_t i = 0; i < tracked.ragdoll.limbs.size(); ++i) {
                        tracked.listeners.push_back(std::make_unique<LimbListener>(
                            tracked.ragdoll.limbs[i].body, &m_ring, &tracked.pub, it->first,
                            static_cast<std::uint16_t>(i)));
                    }
                    Attach(tracked, it->first);
                    spdlog::debug("feed: rebuilt the ragdoll for {:08X}", it->first);
                }
            }
        } else {
            tracked.rebuildCountdown = 0;
        }

        // Stop listening once they are back on their feet and have stayed there.
        // Animated mode never lets go here - on their feet is the case it exists
        // to hear - so it untracks only on cull radius or lost 3D, both above.
        if (phase == ActorPhase::kAnimated && !m_animatedMode) {
            if (++tracked.idleTicks > kIdleTicksBeforeUntrack) {
                Detach(tracked);
                spdlog::debug("feed: untracking {:08X}, back on their feet", it->first);
                it = m_actors.erase(it);
                continue;
            }
        } else {
            tracked.idleTicks = 0;
        }

        ++it;
    }

    CommitVanillaGate();
}

bool GameFeed::Drain(TimeMs untilMs, std::vector<FeedEvent>& out) {
    (void)untilMs;  // a live feed has nothing later than now

    m_scratch.clear();
    m_ring.Drain(m_scratch);
    if (m_scratch.empty()) {
        return true;
    }

    // Resolved here rather than in the callback: it walks the limb table.
    {
        std::lock_guard lock{m_mutex};
        for (FeedEvent& event : m_scratch) {
            if (event.otherBody == 0) {
                continue;
            }
            const auto it = m_actors.find(event.actorId);
            if (it == m_actors.end()) {
                continue;
            }
            const auto& limbs = it->second->pub.profile.limbs;
            for (std::size_t i = 0; i < limbs.size(); ++i) {
                if (limbs[i].bodyId == event.otherBody) {
                    event.otherLimb = static_cast<std::int32_t>(i);
                    break;
                }
            }
        }
    }

    // Publication order is time order within one solver step, but several worker
    // threads publish into the ring and their interleaving is not.
    std::ranges::stable_sort(
        m_scratch, [](const FeedEvent& a, const FeedEvent& b) { return a.timeMs < b.timeMs; });

    out.insert(out.end(), m_scratch.begin(), m_scratch.end());
    return true;
}

const ActorProfile* GameFeed::Profile(ActorId actor) const {
    std::lock_guard lock{m_mutex};
    const auto it = m_actors.find(actor);
    return it == m_actors.end() ? nullptr : &it->second->pub.profile;
}

const ListenerState& GameFeed::Listener() const { return m_listener; }

float GameFeed::FrameTimeSec() const { return m_frameTimeSec; }

bool GameFeed::ActorPosition(ActorId actor, Vec3& out) const {
    std::lock_guard lock{m_mutex};
    const auto it = m_actors.find(actor);
    if (it == m_actors.end()) {
        return false;
    }
    auto* refr = it->second->ref.get();
    if (refr == nullptr) {
        return false;
    }
    const auto p = refr->GetPosition();
    out = {p.x, p.y, p.z};
    return true;
}

RE::NiAVObject* GameFeed::RootNode(ActorId actor) const {
    std::lock_guard lock{m_mutex};
    const auto it = m_actors.find(actor);
    if (it == m_actors.end()) {
        return nullptr;
    }
    auto* refr = it->second->ref.get();
    return refr != nullptr ? refr->Get3D() : nullptr;
}

RE::NiAVObject* GameFeed::BoneNode(ActorId actor, std::int32_t limbIndex) const {
    if (limbIndex < 0) {
        return nullptr;
    }
    std::lock_guard lock{m_mutex};
    const auto it = m_actors.find(actor);
    if (it == m_actors.end()) {
        return nullptr;
    }
    const auto& limbs = it->second->pub.profile.limbs;
    if (static_cast<std::size_t>(limbIndex) >= limbs.size()) {
        return nullptr;
    }
    auto* refr = it->second->ref.get();
    auto* root = refr != nullptr ? refr->Get3D() : nullptr;
    if (root == nullptr) {
        return nullptr;
    }
    // A ragdoll body's name is the bone it was authored on, so it names the node.
    return root->GetObjectByName(RE::BSFixedString(limbs[limbIndex].boneName.c_str()));
}

void GameFeed::Clear() {
    std::lock_guard lock{m_mutex};
    for (auto& [id, tracked] : m_actors) {
        Detach(*tracked);
    }
    m_actors.clear();
    // Nobody tracked, nobody claiming - a load screen is exactly where a stale
    // claim would sit over a spot the player later walks past.
    ClearVanillaGate();
    // Safe now: every producer was detached above.
    m_ring.Reset();
    m_scratch.clear();
}

}  // namespace rds::game
