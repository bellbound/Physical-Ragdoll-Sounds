#include "GameFeed.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace rds::game {
namespace {

/// Contacts under this never reach the ring at all.
///
/// Deliberately well under the engine's own `ingest.minImpactSpeed` of 20: this
/// is a cost filter, not a design one. The design's floor belongs in the config
/// where it can be tuned, and anything filtered here can never be tuned back.
constexpr float kMinimumImpactSpeed = 5.0f;

/// How many ticks to wait before rebuilding a ragdoll whose bodies have left the
/// world. About half a second.
///
/// Throttled, but not hard: a rebuild storm - and `ragdoll_rebuilt` fires six
/// times in three seconds on a disturbed standing actor - must not leave the mod
/// deaf (07 section 7).
constexpr int kRebuildTicks = 16;

/// How many ticks an actor may sit animated and untouched before we stop
/// listening to it. Roughly three seconds, so a knockdown that briefly reports
/// animated mid-tumble does not lose its listeners and have to re-attach.
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

/// How fast the material of `body` is moving at the world point `point`.
///
/// `linearVelocity` is the velocity of the centre of mass, which is not the
/// velocity of the surface: a limb pivoting about its shoulder has a still centre
/// and a fast hand. The rigid-body identity `v + w x (p - com)` is the whole
/// difference, and it is what makes a scrape distinguishable from a thud.
///
/// Havok units throughout; the caller scales.
[[nodiscard]] Vec PointVelocity(const RE::hkpRigidBody& body, const Vec& point) {
    const auto linear = LoadVector(body.motion.linearVelocity);
    const auto angular = LoadVector(body.motion.angularVelocity);
    // centerOfMass1 is the current end of the swept transform - where the body is
    // now, rather than where it was at the start of the step.
    const auto centre = LoadVector(body.motion.motionState.sweptTransform.centerOfMass1);
    const Vec arm{point[0] - centre[0], point[1] - centre[1], point[2] - centre[2]};
    const auto spin = Cross(angular, arm);
    return {linear[0] + spin[0], linear[1] + spin[1], linear[2] + spin[2]};
}

/// The three-phase model the recorder used, and the reason `kGetUp` is named
/// rather than folded into `kAnimated`: `ragdoll_end` and `knock_get_up` land on
/// the same instant, and the frames after them carry the largest and least
/// meaningful closing speeds in the whole dataset - a blend from simulation back
/// to animation, not a fall.
[[nodiscard]] ActorPhase PhaseOf(bool ragdolled, RE::KNOCK_STATE_ENUM knock) {
    if (ragdolled) {
        return ActorPhase::kRagdoll;
    }
    if (knock == RE::KNOCK_STATE_ENUM::kGetUp) {
        return ActorPhase::kGetUp;
    }
    return ActorPhase::kAnimated;
}

/// Which armour slot covers a body site.
///
/// An axis of timbre only, never of physics (07 section 11) - armour changes what
/// a limb sounds like hitting stone, not how hard it hits. Vanilla agrees: its
/// footstep sets have armour-weight variants and its impact sets do not use
/// armour as a physics term anywhere.
///
/// Skyrim's body slot covers the torso and the legs together, which is why thighs
/// and calves fall back to it rather than having anything of their own.
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
/// A site with nothing in its own slot falls back to the body piece rather than
/// reporting bare: a cuirass with no separate gauntlets still means the forearm
/// is not bare skin, and reading it as bare would pick the wrong timbre for the
/// most common case in the game.
[[nodiscard]] Coverage CoverageForSite(RE::Actor& actor, LimbSite site) {
    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
    const Slot slot = SlotForSite(site);

    auto* worn = actor.GetWornArmor(slot);
    if (worn == nullptr && slot != Slot::kBody) {
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
    return Coverage::kCloth;
}

void SetText(char (&out)[24], std::string_view text) {
    const auto count = std::min(text.size(), sizeof(out) - 1);
    std::memcpy(out, text.data(), count);
    out[count] = '\0';
}

/// A state row, pushed through the same ring as the contacts.
///
/// It goes through the ring rather than straight into the drain so it keeps its
/// place in time against the contacts around it. That matters at both ends of a
/// knockdown: `ragdoll_start` arriving after the first contact would put the
/// phase machine a tick behind the fall, and `ragdoll_end` arriving early would
/// close the event while its settle was still playing.
void PushState(ContactRing& ring, ActorId actor, std::string_view state) {
    FeedEvent event{};
    event.timeMs = NowMs();
    event.actorId = actor;
    event.kind = EventKind::kState;
    SetText(event.text, state);
    ring.Push(event);
}

/// One `kLimbSample` per ragdoll limb: where it is and how fast it is going.
///
/// This is the only measurement of where the body actually *is*. Everything
/// else the engine gets is a collision, and collisions are dense exactly when a
/// fall is busy and absent exactly when it is not - so air time inferred from
/// the gaps between them is maximal at `ragdoll_start`, when the actor was
/// standing on the floor a frame earlier, and near zero during the landing that
/// air time exists to find.
///
/// Game thread only, from PublishTick. It reads the same fields the contact
/// callback reads, but none of the callback's restrictions apply here - which is
/// why the pose is published from the tick rather than gathered in the listener.
///
/// The text is deliberately left empty. QuickModMenuNG's recorder emits a
/// limb_sample per limb on a *state change* and stamps it with that state; those
/// two snapshots are a launch pose, not a signal, and `rds::pose::IsTickSample`
/// tells the two apart on exactly this field.
void PushLimbSamples(ContactRing& ring, const RagdollView& ragdoll, ActorId actor,
                     ActorPhase phase) {
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
/// Holds a reference to its body, so the body cannot be freed while a listener is
/// still registered on it.
class LimbListener final : public RE::hkpContactListener {
public:
    /// `pub` outlives every listener it owns - Tracked is heap-allocated and
    /// never moved - so the raw pointer is safe. It carries the two things only
    /// the game thread can work out: whether the actor is ragdolling, and what
    /// the land under them is made of.
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
    ///
    /// `contactPointCallbackDelay` is how many steps Havok waits between contact
    /// point callbacks for an entity; Skyrim leaves it high because nothing in the
    /// base game wants them. The previous value is kept so the body is handed back
    /// exactly as it was found.
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

        // THE GATE. One relaxed load, and the only reason the mod does not play
        // impacts while NPCs walk around: the ragdoll bodies are there and
        // colliding the whole time an actor is animated.
        const auto phase = m_pub != nullptr
                               ? static_cast<ActorPhase>(m_pub->phase.load(std::memory_order_relaxed))
                               : ActorPhase::kUnknown;
        if (phase != ActorPhase::kRagdoll) {
            return;
        }

        // Havok's sign convention: negative separating velocity is closing. A
        // positive one is two bodies coming apart, which is not an impact.
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
    /// not about the body this listener belongs to, which should not happen but
    /// costs one compare to be sure of.
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
        // The body's own bounding radius: the only size the callback can reach
        // without walking a shape hierarchy, and enough to tell a hand from a
        // torso when the bone name is not one we recognise.
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

    /// Split the relative motion at the contact into "into the surface" and
    /// "along it".
    ///
    /// Both bodies contribute: a limb landing on a moving limb slides by the
    /// difference, not by its own speed. `normalSpeed` is our own arithmetic for
    /// the quantity Havok reports as `separatingVelocity`, written out beside it
    /// so the two can be compared - a disagreement is a solver blow-up detected
    /// on the arithmetic rather than on a threshold (07 section 2).
    ///
    /// The relative velocity is ALWAYS body[0] minus body[1], never "us minus
    /// them". The contact normal belongs to the pair and its direction is fixed by
    /// that ordering, not by which of the two listeners Havok happened to call -
    /// so subtracting in listener order flips the sign on exactly those contacts
    /// where our limb was body[1]. That was 23.3 % of the recorder's first run.
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

    /// The surface material, and where it came from.
    ///
    /// The contact's own shape answers for everything built out of geometry.
    /// Terrain does not - `hkpBvTreeShape` returns nothing for any shape key - so
    /// the land record sampled under the actor by the tick stands in, and the
    /// event says which of the two it was rather than letting a sampled answer
    /// pass for a measured one (07 section 8).
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
        // The shape key picks the leaf out of a compound shape, which is what a
        // terrain or mesh collider is - without it every rock in a mesh reads as
        // the same material.
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

void GameFeed::SetBodySampleEveryNTicks(std::int32_t ticks) {
    m_bodySampleEveryNTicks = std::max(0, ticks);
}

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
        // The world went away under us. Still detach: removing a listener only
        // touches the entity's own array, and we hold a reference to every body.
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
    for (const RagdollBody& body : tracked.ragdoll.limbs) {
        LimbInfo limb;
        limb.boneName = body.name;
        // Resolved from the NAME, never from the index: 18 bodies in a fixed
        // order is the vanilla humanoid skeleton and nothing else.
        limb.site = SiteFromBoneName(body.name);
        limb.chain = ChainFromBoneName(body.name);
        limb.coverage = CoverageForSite(actor, limb.site);
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
    // Knocked covers the lead-in states as well as the ragdoll itself, which is
    // what gets the listeners on in time: attaching only once IsInRagdollState is
    // true would miss a knockdown's first contacts.
    auto* state = actor.AsActorState();
    const auto knock = state != nullptr ? state->GetKnockState() : RE::KNOCK_STATE_ENUM::kNormal;
    return actor.IsInRagdollState() || knock != RE::KNOCK_STATE_ENUM::kNormal;
}

void GameFeed::RefreshListener() {
    // The camera, not the body. The recorder deliberately used the body's facing
    // to keep head tracking out of its rows; a live mod wants the ears.
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
    // Non-zero or the engine treats the listener as absent and pins every actor
    // to the Full tier forever.
    m_listener.timeMs = NowMs();
}

void GameFeed::PublishTick(float frameTimeSec) {
    m_frameTimeSec = frameTimeSec;
    ++m_tick;
    RefreshListener();

    std::lock_guard lock{m_mutex};

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

        for (std::size_t i = 0; i < tracked->ragdoll.limbs.size(); ++i) {
            tracked->listeners.push_back(std::make_unique<LimbListener>(
                tracked->ragdoll.limbs[i].body, &m_ring, &tracked->pub, id,
                static_cast<std::uint16_t>(i)));
        }
        Tracked& ref = *tracked;
        m_actors.emplace(id, std::move(tracked));
        Attach(ref, id);
        spdlog::info("feed: tracking {:08X} {} ({} limbs)", id,
                     ref.pub.profile.name.empty() ? "?" : ref.pub.profile.name,
                     ref.pub.profile.limbs.size());
    };

    consider(RE::PlayerCharacter::GetSingleton());
    if (auto* processLists = RE::ProcessLists::GetSingleton()) {
        for (auto& handle : processLists->highActorHandles) {
            if (auto actor = handle.get()) {
                consider(actor.get());
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

        const float distSq = distanceSqTo(actor->GetPosition());
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

        // Phase first: the callback reads this and everything else in the tick is
        // downstream of it being right.
        tracked.pub.phase.store(static_cast<std::uint8_t>(phase), std::memory_order_relaxed);

        // The engine opens a knockdown on ragdoll_start and closes it on
        // ragdoll_end or knock_get_up. Without these it would acquire the actor on
        // the first contact and never let go: the crash state would linger, the
        // phase machine would never reach Rest, and the one summary line per
        // knockdown would never be written.
        if (phase != tracked.lastPhase) {
            if (phase == ActorPhase::kRagdoll) {
                PushState(m_ring, it->first, "ragdoll_start");
            } else if (tracked.lastPhase == ActorPhase::kRagdoll) {
                PushState(m_ring, it->first,
                          phase == ActorPhase::kGetUp ? "knock_get_up" : "ragdoll_end");
            }
            tracked.lastPhase = phase;
        }

        // The pose, on the same terms and for the same reason: game-thread only,
        // and the engine cannot measure a fall without it. Skipped while the
        // actor is on its feet, so a village of walking NPCs costs nothing.
        if (m_bodySampleEveryNTicks > 0 && phase != ActorPhase::kAnimated &&
            m_tick % static_cast<std::uint64_t>(m_bodySampleEveryNTicks) == 0) {
            PushLimbSamples(m_ring, tracked.ragdoll, it->first, phase);
        }

        // Game-thread only, which is the whole reason it is published rather than
        // read in the callback.
        if (auto* tes = RE::TES::GetSingleton()) {
            const auto material = tes->GetLandMaterialType(actor->GetPosition());
            tracked.pub.material.store(static_cast<std::uint32_t>(material),
                                       std::memory_order_relaxed);
        }

        // The ragdoll is rebuilt on cell change, on 3D reload, and repeatedly on a
        // disturbed standing actor. Re-capture rather than going deaf.
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
        // Not immediately: a tumble can report animated for a tick in the middle,
        // and re-attaching costs the world write lock.
        if (phase == ActorPhase::kAnimated) {
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
}

bool GameFeed::Drain(TimeMs untilMs, std::vector<FeedEvent>& out) {
    (void)untilMs;  // a live feed has nothing later than now

    m_scratch.clear();
    m_ring.Drain(m_scratch);
    if (m_scratch.empty()) {
        return true;
    }

    // Self-collision identity is resolved here rather than in the callback: it
    // means walking the limb table, and the callback may not.
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

    // The ring is already in publication order, which for a single solver step is
    // time order. Sorted anyway, because several worker threads publish into it
    // and their interleaving is not ordered by timestamp.
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
    // The ragdoll body's own name is the bone it was authored on, so it is also
    // the name of the node in the actor's 3D.
    return root->GetObjectByName(RE::BSFixedString(limbs[limbIndex].boneName.c_str()));
}

void GameFeed::Clear() {
    std::lock_guard lock{m_mutex};
    for (auto& [id, tracked] : m_actors) {
        Detach(*tracked);
    }
    m_actors.clear();
    // Safe now: every producer has been detached above.
    m_ring.Reset();
    m_scratch.clear();
}

}  // namespace rds::game
