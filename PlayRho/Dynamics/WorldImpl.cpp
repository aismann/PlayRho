/*
 * Original work Copyright (c) 2006-2011 Erin Catto http://www.box2d.org
 * Modified work Copyright (c) 2020 Louis Langholtz https://github.com/louis-langholtz/PlayRho
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <PlayRho/Dynamics/WorldImpl.hpp>

#include <PlayRho/Dynamics/Body.hpp>
#include <PlayRho/Dynamics/BodyConf.hpp>
#include <PlayRho/Dynamics/StepConf.hpp>
#include <PlayRho/Dynamics/Island.hpp>
#include <PlayRho/Dynamics/MovementConf.hpp>
#include <PlayRho/Dynamics/ContactImpulsesList.hpp>

#include <PlayRho/Dynamics/Joints/Joint.hpp>
#include <PlayRho/Dynamics/Joints/RevoluteJointConf.hpp>
#include <PlayRho/Dynamics/Joints/PrismaticJointConf.hpp>
#include <PlayRho/Dynamics/Joints/DistanceJointConf.hpp>
#include <PlayRho/Dynamics/Joints/PulleyJointConf.hpp>
#include <PlayRho/Dynamics/Joints/TargetJointConf.hpp>
#include <PlayRho/Dynamics/Joints/GearJointConf.hpp>
#include <PlayRho/Dynamics/Joints/WheelJointConf.hpp>
#include <PlayRho/Dynamics/Joints/WeldJointConf.hpp>
#include <PlayRho/Dynamics/Joints/FrictionJointConf.hpp>
#include <PlayRho/Dynamics/Joints/RopeJointConf.hpp>
#include <PlayRho/Dynamics/Joints/MotorJointConf.hpp>

#include <PlayRho/Dynamics/Contacts/ConstraintSolverConf.hpp>
#include <PlayRho/Dynamics/Contacts/Contact.hpp>
#include <PlayRho/Dynamics/Contacts/ContactSolver.hpp>
#include <PlayRho/Dynamics/Contacts/VelocityConstraint.hpp>
#include <PlayRho/Dynamics/Contacts/PositionConstraint.hpp>

#include <PlayRho/Collision/WorldManifold.hpp>
#include <PlayRho/Collision/TimeOfImpact.hpp>
#include <PlayRho/Collision/RayCastOutput.hpp>
#include <PlayRho/Collision/DistanceProxy.hpp>

#include <PlayRho/Common/LengthError.hpp>
#include <PlayRho/Common/DynamicMemory.hpp>
#include <PlayRho/Common/FlagGuard.hpp>
#include <PlayRho/Common/WrongState.hpp>

#include <algorithm>
#include <new>
#include <functional>
#include <type_traits>
#include <memory>
#include <set>
#include <vector>

#ifdef DO_PAR_UNSEQ
#include <atomic>
#endif

//#define DO_THREADED
#if defined(DO_THREADED)
#include <future>
#endif

using std::for_each;
using std::remove;
using std::sort;
using std::transform;
using std::unique;

namespace playrho {
namespace d2 {

static_assert(std::is_default_constructible<WorldImpl>::value,
              "WorldImpl must be default constructible!");
static_assert(std::is_copy_constructible<WorldImpl>::value,
              "WorldImpl must be copy constructible!");
static_assert(std::is_copy_assignable<WorldImpl>::value,
              "WorldImpl must be copy assignable!");
static_assert(std::is_nothrow_destructible<WorldImpl>::value,
              "WorldImpl must be nothrow destructible!");

using playrho::size;

/// @brief Collection of body constraints.
using BodyConstraints = std::vector<BodyConstraint>;

/// @brief Collection of position constraints.
using PositionConstraints = std::vector<PositionConstraint>;

/// @brief Collection of velocity constraints.
using VelocityConstraints = std::vector<VelocityConstraint>;

/// @brief Contact updating configuration.
struct WorldImpl::ContactUpdateConf
{
    DistanceConf distance; ///< Distance configuration data.
    Manifold::Conf manifold; ///< Manifold configuration data.
};

namespace {

inline void IntegratePositions(BodyConstraints& bodies, Time h)
{
    assert(IsValid(h));
    for_each(begin(bodies), end(bodies), [&](BodyConstraint& bc) {
        const auto velocity = bc.GetVelocity();
        const auto translation = h * velocity.linear;
        const auto rotation = h * velocity.angular;
        bc.SetPosition(bc.GetPosition() + Position{translation, rotation});
    });
}

/// Reports the given constraints to the listener.
/// @details
/// This calls the listener's PostSolve method for all size(contacts) elements of
/// the given array of constraints.
/// @param listener Listener to call.
/// @param constraints Array of m_contactCount contact velocity constraint elements.
inline void Report(const WorldImpl::ImpulsesContactListener& listener,
                   const std::vector<ContactID>& contacts,
                   const VelocityConstraints& constraints,
                   StepConf::iteration_type solved)
{
    const auto numContacts = size(contacts);
    for (auto i = decltype(numContacts){0}; i < numContacts; ++i)
    {
        listener(contacts[i], GetContactImpulses(constraints[i]), solved);
    }
}

inline void AssignImpulses(Manifold& var, const VelocityConstraint& vc)
{
    assert(var.GetPointCount() >= vc.GetPointCount());
    
    auto assignProc = [&](VelocityConstraint::size_type i) {
        const auto& point = vc.GetPointAt(i);
        var.SetPointImpulses(i, point.normalImpulse, point.tangentImpulse);
    };
#if 0
    // Branch free assignment causes problems in TilesComeToRest test.
    assignProc(1);
    assignProc(0);
#else
    const auto count = vc.GetPointCount();
    for (auto i = decltype(count){0}; i < count; ++i)
    {
        assignProc(i);
    }
#endif
}

/// @brief Calculates the "warm start" velocity deltas for the given velocity constraint.
VelocityPair CalcWarmStartVelocityDeltas(const VelocityConstraint& vc,
                                         const std::vector<BodyConstraint>& bodies)
{
    auto vp = VelocityPair{Velocity{LinearVelocity2{}, 0_rpm}, Velocity{LinearVelocity2{}, 0_rpm}};

    const auto normal = vc.GetNormal();
    const auto tangent = vc.GetTangent();
    const auto pointCount = vc.GetPointCount();
    const auto bodyA = &bodies[vc.GetBodyA().get()];
    const auto bodyB = &bodies[vc.GetBodyB().get()];

    const auto invMassA = bodyA->GetInvMass();
    const auto invRotInertiaA = bodyA->GetInvRotInertia();

    const auto invMassB = bodyB->GetInvMass();
    const auto invRotInertiaB = bodyB->GetInvRotInertia();

    for (auto j = decltype(pointCount){0}; j < pointCount; ++j) {
        // inverse moment of inertia : L^-2 M^-1 QP^2
        // P is M L T^-2
        // GetPointRelPosA() is Length2
        // Cross(Length2, P) is: M L^2 T^-2
        // L^-2 M^-1 QP^2 M L^2 T^-2 is: QP^2 T^-2
        const auto& vcp = vc.GetPointAt(j);
        const auto P = vcp.normalImpulse * normal + vcp.tangentImpulse * tangent;
        const auto LA = Cross(vcp.relA, P) / Radian;
        const auto LB = Cross(vcp.relB, P) / Radian;
        std::get<0>(vp) -= Velocity{invMassA * P, invRotInertiaA * LA};
        std::get<1>(vp) += Velocity{invMassB * P, invRotInertiaB * LB};
    }

    return vp;
}

void WarmStartVelocities(const VelocityConstraints& velConstraints,
                         std::vector<BodyConstraint>& bodies)
{
    for_each(cbegin(velConstraints), cend(velConstraints), [&](const VelocityConstraint& vc) {
        const auto vp = CalcWarmStartVelocityDeltas(vc, bodies);
        const auto bodyA = &bodies[vc.GetBodyA().get()];
        const auto bodyB = &bodies[vc.GetBodyB().get()];
        bodyA->SetVelocity(bodyA->GetVelocity() + std::get<0>(vp));
        bodyB->SetVelocity(bodyB->GetVelocity() + std::get<1>(vp));
    });
}

BodyConstraints GetBodyConstraints(const Island::Bodies& bodies,
                                   const ArrayAllocator<Body>& bodyBuffer,
                                   Time h, MovementConf conf)
{
    auto constraints = std::vector<BodyConstraint>{};
    constraints.resize(size(bodyBuffer));
    for (const auto& id : bodies)
    {
        constraints[UnderlyingValue(id)] =
            GetBodyConstraint(bodyBuffer[UnderlyingValue(id)], h, conf);
    }
    return constraints;
}

PositionConstraints GetPositionConstraints(const Island::Contacts& contacts,
                                           const ArrayAllocator<FixtureConf>& fixtureBuffer,
                                           const ArrayAllocator<Contact>& contactBuffer,
                                           const ArrayAllocator<Manifold>& manifoldBuffer)
{
    auto constraints = PositionConstraints{};
    constraints.reserve(size(contacts));
    transform(cbegin(contacts), cend(contacts), back_inserter(constraints),
              [&](const auto& contactID) {
        const auto& contact = contactBuffer[UnderlyingValue(contactID)];
        const auto fixtureA = GetFixtureA(contact);
        const auto fixtureB = GetFixtureB(contact);
        const auto indexA = GetChildIndexA(contact);
        const auto indexB = GetChildIndexB(contact);
        const auto bodyA = GetBodyA(contact);
        const auto bodyB = GetBodyB(contact);
        const auto radiusA = GetVertexRadius(GetShape(fixtureBuffer[fixtureA.get()]), indexA);
        const auto radiusB = GetVertexRadius(GetShape(fixtureBuffer[fixtureB.get()]), indexB);
        const auto& manifold = manifoldBuffer[UnderlyingValue(contactID)];
        return PositionConstraint{manifold, bodyA, bodyB, radiusA + radiusB};
    });
    return constraints;
}

/// @brief Gets the velocity constraints for the given inputs.
/// @details Inializes the velocity constraints with the position dependent portions of
///   the current position constraints.
/// @post Velocity constraints will have their "normal" field set to the world manifold
///   normal for them.
/// @post Velocity constraints will have their constraint points set.
/// @see SolveVelocityConstraints.
VelocityConstraints GetVelocityConstraints(const Island::Contacts& contacts,
                                           const ArrayAllocator<FixtureConf>& fixtureBuffer,
                                           const ArrayAllocator<Contact>& contactBuffer,
                                           const ArrayAllocator<Manifold>& manifoldBuffer,
                                           BodyConstraints& bodies,
                                           const VelocityConstraint::Conf conf)
{
    auto velConstraints = VelocityConstraints{};
    velConstraints.reserve(size(contacts));
    transform(cbegin(contacts), cend(contacts), back_inserter(velConstraints),
              [&](const auto& contactID) {
        const auto& contact = contactBuffer[UnderlyingValue(contactID)];
        const auto bodyA = GetBodyA(contact);
        const auto bodyB = GetBodyB(contact);
        const auto fixtureA = GetFixtureA(contact);
        const auto fixtureB = GetFixtureB(contact);
        const auto indexA = GetChildIndexA(contact);
        const auto indexB = GetChildIndexB(contact);
        const auto friction = GetFriction(contact);
        const auto restitution = GetRestitution(contact);
        const auto tangentSpeed = GetTangentSpeed(contact);
        const auto& shapeA = GetShape(fixtureBuffer[UnderlyingValue(fixtureA)]);
        const auto& shapeB = GetShape(fixtureBuffer[UnderlyingValue(fixtureB)]);
        const auto& bodyConstraintA = bodies[UnderlyingValue(bodyA)];
        const auto& bodyConstraintB = bodies[UnderlyingValue(bodyB)];
        const auto radiusA = GetVertexRadius(shapeA, indexA);
        const auto radiusB = GetVertexRadius(shapeB, indexB);
        const auto xfA = GetTransformation(bodyConstraintA.GetPosition(),
                                           bodyConstraintA.GetLocalCenter());
        const auto xfB = GetTransformation(bodyConstraintB.GetPosition(),
                                           bodyConstraintB.GetLocalCenter());
        const auto& manifold = manifoldBuffer[contactID.get()];
        const auto worldManifold = GetWorldManifold(manifold, xfA, radiusA, xfB, radiusB);
        return VelocityConstraint{friction, restitution, tangentSpeed, worldManifold,
            bodyA, bodyB, bodies, conf};
    });
    return velConstraints;
}

/// "Solves" the velocity constraints.
/// @details Updates the velocities and velocity constraint points' normal and tangent impulses.
/// @pre <code>UpdateVelocityConstraints</code> has been called on the velocity constraints.
/// @return Maximum momentum used for solving both the tangential and normal portions of
///   the velocity constraints.
Momentum SolveVelocityConstraintsViaGS(VelocityConstraints& velConstraints, BodyConstraints& bodies)
{
    auto maxIncImpulse = 0_Ns;
    for_each(begin(velConstraints), end(velConstraints), [&](VelocityConstraint& vc)
    {
        maxIncImpulse = std::max(maxIncImpulse, GaussSeidel::SolveVelocityConstraint(vc, bodies));
    });
    return maxIncImpulse;
}

/// Solves the given position constraints.
/// @details This updates positions (and nothing else) by calling the position constraint solving function.
/// @note Can't expect the returned minimum separation to be greater than or equal to
///  <code>-conf.linearSlop</code> because code won't push the separation above this
///   amount to begin with.
/// @return Minimum separation.
Length SolvePositionConstraintsViaGS(PositionConstraints& posConstraints,
                                     BodyConstraints& bodies,
                                     const ConstraintSolverConf& conf)
{
    auto minSeparation = std::numeric_limits<Length>::infinity();
    
    for_each(begin(posConstraints), end(posConstraints), [&](PositionConstraint &pc) {
        assert(pc.GetBodyA() != pc.GetBodyB()); // Confirms ContactManager::Add() did its job.
        const auto res = GaussSeidel::SolvePositionConstraint(pc, true, true, bodies, conf);
        bodies[pc.GetBodyA().get()].SetPosition(res.pos_a);
        bodies[pc.GetBodyB().get()].SetPosition(res.pos_b);
        minSeparation = std::min(minSeparation, res.min_separation);
    });
    
    return minSeparation;
}

inline Time GetUnderActiveTime(const Body& b, const StepConf& conf) noexcept
{
    const auto underactive = IsUnderActive(b.GetVelocity(), conf.linearSleepTolerance,
                                           conf.angularSleepTolerance);
    const auto sleepable = b.IsSleepingAllowed();
    return (sleepable && underactive)? b.GetUnderActiveTime() + conf.deltaTime: 0_s;
}

inline Time UpdateUnderActiveTimes(const Island::Bodies& bodies,
                                   ArrayAllocator<Body>& bodyBuffer,
                                   const StepConf& conf)
{
    auto minUnderActiveTime = std::numeric_limits<Time>::infinity();
    for_each(cbegin(bodies), cend(bodies), [&](const auto& bodyID)
    {
        auto& b = bodyBuffer[UnderlyingValue(bodyID)];
        if (b.IsSpeedable())
        {
            const auto underActiveTime = GetUnderActiveTime(b, conf);
            b.SetUnderActiveTime(underActiveTime);
            minUnderActiveTime = std::min(minUnderActiveTime, underActiveTime);
        }
    });
    return minUnderActiveTime;
}

inline BodyCounter Sleepem(const Island::Bodies& bodies,
                           ArrayAllocator<Body>& bodyBuffer)
{
    auto unawoken = BodyCounter{0};
    for_each(cbegin(bodies), cend(bodies), [&](const auto& bodyID) {
        if (Unawaken(bodyBuffer[UnderlyingValue(bodyID)])) {
            ++unawoken;
        }
    });
    return unawoken;
}

inline bool IsValidForTime(TOIOutput::State state) noexcept
{
    return state == TOIOutput::e_touching;
}

void FlagForFiltering(ArrayAllocator<Contact>& contactBuffer, BodyID bodyA,
                      const std::vector<KeyedContactPtr>& contactsBodyB,
                      BodyID bodyB) noexcept
{
    for (const auto& ci: contactsBodyB) {
        auto& contact = contactBuffer[UnderlyingValue(std::get<ContactID>(ci))];
        const auto bA = contact.GetBodyA();
        const auto bB = contact.GetBodyB();
        const auto other = (bA != bodyB)? bA: bB;
        if (other == bodyA) {
            // Flag the contact for filtering at the next time step (where either
            // body is awake).
            contact.FlagForFiltering();
        }
    }
}

void FlagForFiltering(ArrayAllocator<Contact>& contactBuffer,
                      const std::vector<KeyedContactPtr>& contacts,
                      FixtureID id) noexcept
{
    std::for_each(cbegin(contacts), cend(contacts), [&contactBuffer, id](const auto& ci) {
        auto& contact = contactBuffer[std::get<ContactID>(ci).get()];
        if ((contact.GetFixtureA() == id) || (contact.GetFixtureB() == id)) {
            contact.FlagForFiltering();
        }
    });
}

/// @brief Gets the update configuration from the given step configuration data.
WorldImpl::ContactUpdateConf GetUpdateConf(const StepConf& conf) noexcept
{
    return WorldImpl::ContactUpdateConf{GetDistanceConf(conf), GetManifoldConf(conf)};
}

[[maybe_unused]]
bool HasSensor(const ArrayAllocator<FixtureConf>& fixtures, const Contact& c)
{
    return IsSensor(fixtures[UnderlyingValue(c.GetFixtureA())])
        || IsSensor(fixtures[UnderlyingValue(c.GetFixtureB())]);
}

template <typename T>
void FlagForUpdating(ArrayAllocator<Contact>& contactsBuffer, const T& contacts) noexcept
{
    std::for_each(begin(contacts), end(contacts), [&](const auto& ci) {
        contactsBuffer[UnderlyingValue(std::get<ContactID>(ci))].FlagForUpdating();
    });
}

inline bool EitherIsAccelerable(const Body& lhs, const Body& rhs) noexcept
{
    return lhs.IsAccelerable() || rhs.IsAccelerable();
}

bool ShouldCollide(const ArrayAllocator<Joint>& jointBuffer,
                   const ArrayAllocator<WorldImpl::BodyJoints>& bodyJoints,
                   BodyID lhs, BodyID rhs)
{
    // Does a joint prevent collision?
    const auto& joints = bodyJoints[lhs.get()];
    const auto it = std::find_if(cbegin(joints), cend(joints), [&](const auto& ji) {
        return (std::get<BodyID>(ji) == rhs) &&
            !GetCollideConnected(jointBuffer[std::get<JointID>(ji).get()]);
    });
    return it == end(joints);
}

/// @brief Executes function for all the fixtures of the given body.
void ForallFixtures(const WorldImpl::Fixtures& fixtures, std::function<void(FixtureID)> callback)
{
    for (const auto& f: fixtures) {
        callback(f);
    }
}

void Unset(std::vector<bool>& islanded, const WorldImpl::Bodies& elements)
{
    for (const auto& element: elements) {
        islanded[UnderlyingValue(element)] = false;
    }
}

void Unset(std::vector<bool>& islanded, const WorldImpl::Contacts& elements)
{
    for (const auto& element: elements) {
        islanded[UnderlyingValue(std::get<ContactID>(element))] = false;
    }
}

/// @brief Reset bodies for solve TOI.
void ResetBodiesForSolveTOI(WorldImpl::Bodies& bodies, ArrayAllocator<Body>& buffer) noexcept
{
    for_each(begin(bodies), end(bodies), [&](const auto& body) {
        buffer[UnderlyingValue(body)].ResetAlpha0();
    });
}

/// @brief Reset contacts for solve TOI.
void ResetBodyContactsForSolveTOI(ArrayAllocator<Contact>& buffer,
                                  const std::vector<KeyedContactPtr>& contacts) noexcept
{
    // Invalidate all contact TOIs on this displaced body.
    for_each(cbegin(contacts), cend(contacts), [&buffer](const auto& ci) {
        auto& contact = buffer[UnderlyingValue(std::get<ContactID>(ci))];
        contact.UnsetToi();
    });
}

/// @brief Reset contacts for solve TOI.
void ResetContactsForSolveTOI(ArrayAllocator<Contact>& buffer,
                              const WorldImpl::Contacts& contacts) noexcept
{
    for_each(begin(contacts), end(contacts), [&buffer](const auto& c) {
        auto& contact = buffer[UnderlyingValue(std::get<ContactID>(c))];
        contact.UnsetToi();
        contact.SetToiCount(0);
    });
}

/// @brief Destroys all of the given fixture's proxies.
void DestroyProxies(DynamicTree& tree,
                    std::vector<DynamicTree::Size>& fixtureProxies,
                    std::vector<DynamicTree::Size>& proxies) noexcept
{
    const auto childCount = size(fixtureProxies);
    if (childCount > 0) {
        // Destroy proxies in reverse order from what they were created in.
        for (auto i = childCount - 1; i < childCount; --i) {
            const auto treeId = fixtureProxies[i];
            EraseFirst(proxies, treeId);
            tree.DestroyLeaf(treeId);
        }
    }
    fixtureProxies.clear();
}

void CreateProxies(DynamicTree& tree, WorldImpl::Proxies& fixtureProxies,
                   BodyID bodyID, FixtureID fixtureID, const Shape& shape,
                   const Transformation& xfm, Length aabbExtension)
{
    // Reserve proxy space and create proxies in the broad-phase.
    const auto childCount = GetChildCount(shape);
    fixtureProxies.reserve(size(fixtureProxies) + childCount);
    for (auto childIndex = decltype(childCount){0}; childIndex < childCount; ++childIndex)
    {
        const auto dp = GetChild(shape, childIndex);
        const auto aabb = playrho::d2::ComputeAABB(dp, xfm);

        // Note: treeId from CreateLeaf can be higher than the number of fixture proxies.
        const auto fattenedAABB = GetFattenedAABB(aabb, aabbExtension);
        const auto treeId = tree.CreateLeaf(fattenedAABB, DynamicTree::LeafData{
            bodyID, fixtureID, childIndex});
        fixtureProxies.push_back(treeId);
    }
}

template <typename Element, typename Value>
auto FindTypeValue(const std::vector<Element>& container, const Value& value)
{
    return std::find_if(begin(container), end(container), [value](const auto& elem) {
        return std::get<Value>(elem) == value;
    });
}

void Erase(std::vector<KeyedContactPtr>& contacts, const std::function<bool(ContactID)>& callback)
{
    auto last = end(contacts);
    auto iter = begin(contacts);
    auto index = std::vector<KeyedContactPtr>::difference_type(0);
    while (iter != last) {
        const auto contact = std::get<ContactID>(*iter);
        if (callback(contact)) {
            contacts.erase(iter);
            iter = begin(contacts) + index;
            last = end(contacts);
        }
        else {
            iter = std::next(iter);
            ++index;
        }
    }
}

} // anonymous namespace

WorldImpl::WorldImpl(const WorldConf& def):
    m_tree(def.initialTreeSize),
    m_minVertexRadius{def.minVertexRadius},
    m_maxVertexRadius{def.maxVertexRadius}
{
    if (def.minVertexRadius > def.maxVertexRadius)
    {
        throw InvalidArgument("max vertex radius must be >= min vertex radius");
    }
    m_proxyKeys.reserve(1024);
    m_proxies.reserve(1024);
}

WorldImpl::~WorldImpl() noexcept
{
    Clear();
}

void WorldImpl::Clear() noexcept
{
    if (m_jointDestructionListener) {
        for_each(cbegin(m_joints), cend(m_joints), [this](const auto& id) {
            m_jointDestructionListener(id);
        });
    }
    if (m_fixtureDestructionListener) {
        for_each(cbegin(m_bodies), cend(m_bodies), [this](const auto& id) {
            for (const auto& fixture: m_bodyFixtures[id.get()]) {
                m_fixtureDestructionListener(fixture);
            }
        });
    }
    m_contacts.clear();
    m_joints.clear();
    m_bodies.clear();
    m_bodiesForProxies.clear();
    m_fixturesForProxies.clear();
    m_proxies.clear();
    m_proxyKeys.clear();
    m_tree.Clear();
    m_manifoldBuffer.clear();
    m_contactBuffer.clear();
    m_jointBuffer.clear();
    m_fixtureBuffer.clear();
    m_fixtureProxies.clear();
    m_bodyBuffer.clear();
    m_bodyContacts.clear();
    m_bodyJoints.clear();
    m_bodyFixtures.clear();
}

BodyCounter WorldImpl::GetBodyRange() const noexcept
{
    return static_cast<BodyCounter>(m_bodyBuffer.size());
}

FixtureCounter WorldImpl::GetFixtureRange() const noexcept
{
    return static_cast<FixtureCounter>(m_fixtureBuffer.size());
}

JointCounter WorldImpl::GetJointRange() const noexcept
{
    return static_cast<JointCounter>(m_jointBuffer.size());
}

ContactCounter WorldImpl::GetContactRange() const noexcept
{
    return static_cast<ContactCounter>(m_contactBuffer.size());
}

BodyID WorldImpl::CreateBody(const BodyConf& def)
{
    if (IsLocked()) {
        throw WrongState("CreateBody: world is locked");
    }
    if (size(m_bodies) >= MaxBodies) {
        throw LengthError("CreateBody: operation would exceed MaxBodies");
    }
    const auto id = static_cast<BodyID>(
        static_cast<BodyID::underlying_type>(m_bodyBuffer.Allocate(def)));
    m_bodyContacts.Allocate();
    m_bodyJoints.Allocate();
    m_bodyFixtures.Allocate();
    m_bodies.push_back(id);
    return id;
}

void WorldImpl::Remove(BodyID id) noexcept
{
    m_bodiesForProxies.erase(remove(begin(m_bodiesForProxies), end(m_bodiesForProxies), id),
                             end(m_bodiesForProxies));
    const auto it = find(cbegin(m_bodies), cend(m_bodies), id);
    if (it != cend(m_bodies)) {
        m_bodies.erase(it);
        m_bodyBuffer.Free(id.get());
        m_bodyContacts.Free(id.get());
        m_bodyJoints.Free(id.get());
        m_bodyFixtures.Free(id.get());
    }
}

void WorldImpl::Destroy(BodyID id)
{
    if (IsLocked()) {
        throw WrongState("Destroy: world is locked");
    }

    const auto& body = GetBody(id);

    // Delete the attached joints.
    auto& joints = m_bodyJoints[id.get()];
    while (!joints.empty()) {
        const auto jointID = std::get<JointID>(*begin(joints));
        if (m_jointDestructionListener) {
            m_jointDestructionListener(jointID);
        }
        const auto endIter = cend(m_joints);
        const auto iter = find(cbegin(m_joints), endIter, jointID);
        if (iter != endIter) {
            Remove(jointID); // removes joint from body!
            m_joints.erase(iter);
            m_jointBuffer.Free(jointID.get());
        }
    }

    // Destroy the attached contacts.
    Erase(m_bodyContacts[id.get()], [this,&body](ContactID contactID) {
        Destroy(contactID, &body);
        return true;
    });

    // Delete the attached fixtures. This destroys broad-phase proxies.
    ForallFixtures(m_bodyFixtures[id.get()], [this](FixtureID fixtureID) {
        if (m_fixtureDestructionListener) {
            m_fixtureDestructionListener(fixtureID);
        }
        EraseAll(m_fixturesForProxies, fixtureID);
        DestroyProxies(m_tree, m_fixtureProxies[UnderlyingValue(fixtureID)], m_proxies);
        m_fixtureBuffer.Free(fixtureID.get());
        m_fixtureProxies.Free(fixtureID.get());
    });
    m_bodyFixtures[id.get()].clear();

    Remove(id);
}

bool WorldImpl::IsDestroyed(BodyID id) const noexcept
{
    return m_bodyBuffer.FindFree(id.get());
}

void WorldImpl::SetJoint(JointID id, const Joint& def)
{
    if (IsLocked()) {
        throw WrongState("SetJoint: world is locked");
    }
    const auto endIter = cend(m_joints);
    const auto iter = find(cbegin(m_joints), endIter, id);
    if (iter != endIter) {
        Remove(id);
        m_jointBuffer[UnderlyingValue(id)] = def;
        Add(id, !GetCollideConnected(def));
    }
}

JointID WorldImpl::CreateJoint(const Joint& def)
{
    if (IsLocked()) {
        throw WrongState("CreateJoint: world is locked");
    }
    
    if (size(m_joints) >= MaxJoints) {
        throw LengthError("CreateJoint: operation would exceed MaxJoints");
    }

    const auto id = static_cast<JointID>(
        static_cast<JointID::underlying_type>(m_jointBuffer.Allocate(def)));
    m_joints.push_back(id);
    // Note: creating a joint doesn't wake the bodies.
    Add(id, !GetCollideConnected(def));
    return id;
}

void WorldImpl::Add(JointID id, bool flagForFiltering)
{
    const auto& joint = m_jointBuffer[UnderlyingValue(id)];
    const auto bodyA = GetBodyA(joint);
    const auto bodyB = GetBodyB(joint);
    if (bodyA != InvalidBodyID) {
        m_bodyJoints[bodyA.get()].push_back(std::make_pair(bodyB, id));
    }
    if (bodyB != InvalidBodyID) {
        m_bodyJoints[bodyB.get()].push_back(std::make_pair(bodyA, id));
    }
    if (flagForFiltering && (bodyA != InvalidBodyID) && (bodyB != InvalidBodyID)) {
        FlagForFiltering(m_contactBuffer, bodyA, m_bodyContacts[bodyB.get()], bodyB);
    }
}

void WorldImpl::Remove(JointID id) noexcept
{
    // Disconnect from island graph.
    const auto& joint = m_jointBuffer[UnderlyingValue(id)];
    const auto bodyIdA = GetBodyA(joint);
    const auto bodyIdB = GetBodyB(joint);
    const auto collideConnected = GetCollideConnected(joint);

    // If the joint prevented collisions, then flag any contacts for filtering.
    if ((!collideConnected) && (bodyIdA != InvalidBodyID) && (bodyIdB != InvalidBodyID)) {
        FlagForFiltering(m_contactBuffer, bodyIdA, m_bodyContacts[bodyIdB.get()], bodyIdB);
    }

    // Wake up connected bodies.
    if (bodyIdA != InvalidBodyID) {
        auto& bodyA = m_bodyBuffer[UnderlyingValue(bodyIdA)];
        bodyA.SetAwake();
        auto& bodyJoints = m_bodyJoints[bodyIdA.get()];
        const auto it = FindTypeValue(bodyJoints, id);
        assert(it != end(bodyJoints));
        if (it != end(bodyJoints)) {
            bodyJoints.erase(it);
        }
    }
    if (bodyIdB != InvalidBodyID) {
        auto& bodyB = m_bodyBuffer[UnderlyingValue(bodyIdB)];
        bodyB.SetAwake();
        auto& bodyJoints = m_bodyJoints[bodyIdB.get()];
        const auto it = FindTypeValue(bodyJoints, id);
        assert(it != end(bodyJoints));
        if (it != end(bodyJoints)) {
            bodyJoints.erase(it);
        }
    }
}

void WorldImpl::Destroy(JointID id)
{
    if (IsLocked()) {
        throw WrongState("Destroy: world is locked");
    }
    const auto endIter = cend(m_joints);
    const auto iter = find(cbegin(m_joints), endIter, id);
    if (iter != endIter) {
        Remove(id);
        m_joints.erase(iter);
        m_jointBuffer.Free(UnderlyingValue(id));
    }
}

bool WorldImpl::IsDestroyed(JointID id) const noexcept
{
    return m_jointBuffer.FindFree(id.get());
}

void WorldImpl::AddToIsland(Island& island, BodyID seedID,
                            BodyCounter& remNumBodies,
                            ContactCounter& remNumContacts,
                            JointCounter& remNumJoints)
{
#ifndef NDEBUG
    assert(!m_islandedBodies[UnderlyingValue(seedID)]);
    auto& seed = m_bodyBuffer[UnderlyingValue(seedID)];
    assert(seed.IsSpeedable());
    assert(seed.IsAwake());
    assert(seed.IsEnabled());
    assert(remNumBodies != 0);
    assert(remNumBodies < MaxBodies);
#endif
    // Perform a depth first search (DFS) on the constraint graph.
    // Create a stack for bodies to be is-in-island that aren't already in the island.
    auto bodies = std::vector<BodyID>{};
    bodies.reserve(remNumBodies);
    bodies.push_back(seedID);
    auto stack = BodyStack{std::move(bodies)};
    m_islandedBodies[UnderlyingValue(seedID)] = true;
    AddToIsland(island, stack, remNumBodies, remNumContacts, remNumJoints);
}

void WorldImpl::AddToIsland(Island& island, BodyStack& stack,
                            BodyCounter& remNumBodies,
                            ContactCounter& remNumContacts,
                            JointCounter& remNumJoints)
{
    while (!empty(stack))
    {
        // Grab the next body off the stack and add it to the island.
        const auto bodyID = stack.top();
        stack.pop();

        auto& body = m_bodyBuffer[UnderlyingValue(bodyID)];

        assert(body.IsEnabled());
        island.bodies.push_back(bodyID);
        assert(remNumBodies > 0);
        --remNumBodies;

        // Don't propagate islands across bodies that can't have a velocity (static bodies).
        // This keeps islands smaller and helps with isolating separable collision clusters.
        if (!body.IsSpeedable())
        {
            continue;
        }

        // Make sure the body is awake (without resetting sleep timer).
        body.SetAwakeFlag();

        const auto oldNumContacts = size(island.contacts);
        // Adds appropriate contacts of current body and appropriate 'other' bodies of those contacts.
        AddContactsToIsland(island, stack, m_bodyContacts[bodyID.get()], bodyID);

        const auto newNumContacts = size(island.contacts);
        assert(newNumContacts >= oldNumContacts);
        const auto netNumContacts = newNumContacts - oldNumContacts;
        assert(remNumContacts >= netNumContacts);
        remNumContacts -= netNumContacts;
        
        const auto numJoints = size(island.joints);
        // Adds appropriate joints of current body and appropriate 'other' bodies of those joint.
        AddJointsToIsland(island, stack, m_bodyJoints[bodyID.get()]);

        remNumJoints -= size(island.joints) - numJoints;
    }
}

void WorldImpl::AddContactsToIsland(Island& island, BodyStack& stack,
                                    const Contacts& contacts, BodyID bodyID)
{
    for_each(cbegin(contacts), cend(contacts), [&](const KeyedContactPtr& ci) {
        const auto contactID = std::get<ContactID>(ci);
        if (!m_islandedContacts[UnderlyingValue(contactID)]) {
            auto& contact = m_contactBuffer[UnderlyingValue(contactID)];
            if (IsEnabled(contact) && IsTouching(contact) && !IsSensor(contact))
            {
                const auto bodyA = GetBodyA(contact);
                const auto bodyB = GetBodyB(contact);
                const auto other = (bodyID != bodyA)? bodyA: bodyB;
                island.contacts.push_back(contactID);
                m_islandedContacts[UnderlyingValue(contactID)] = true;
                if (!m_islandedBodies[UnderlyingValue(other)])
                {
                    m_islandedBodies[UnderlyingValue(other)] = true;
                    stack.push(other);
                }
            }
        }
    });
}

void WorldImpl::AddJointsToIsland(Island& island, BodyStack& stack, const BodyJoints& joints)
{
    for_each(cbegin(joints), cend(joints), [this,&island,&stack](const auto& ji) {
        const auto jointID = std::get<JointID>(ji);
        assert(jointID != InvalidJointID);
        if (!m_islandedJoints[UnderlyingValue(jointID)]) {
            const auto otherID = std::get<BodyID>(ji);
            const auto other = (otherID == InvalidBodyID)? static_cast<Body*>(nullptr): &m_bodyBuffer[UnderlyingValue(otherID)];
            assert(!other || other->IsEnabled() || !other->IsAwake());
            if (!other || other->IsEnabled())
            {
                m_islandedJoints[UnderlyingValue(jointID)] = true;
                island.joints.push_back(jointID);
                if ((otherID != InvalidBodyID) && !m_islandedBodies[UnderlyingValue(otherID)])
                {
                    m_islandedBodies[UnderlyingValue(otherID)] = true;
                    stack.push(otherID);
                }
            }
        }
    });
}

WorldImpl::Bodies::size_type
WorldImpl::RemoveUnspeedablesFromIslanded(const std::vector<BodyID>& bodies,
                                          const ArrayAllocator<Body>& buffer,
                                          std::vector<bool>& islanded)
{
    // Allow static bodies to participate in other islands.
    auto numRemoved = Bodies::size_type{0};
    for_each(begin(bodies), end(bodies), [&](BodyID id) {
        if (!buffer[UnderlyingValue(id)].IsSpeedable()) {
            islanded[UnderlyingValue(id)] = false;
            ++numRemoved;
        }
    });
    return numRemoved;
}

RegStepStats WorldImpl::SolveReg(const StepConf& conf)
{
    auto stats = RegStepStats{};
    auto remNumBodies = static_cast<BodyCounter>(size(m_bodies)); // Remaining # of bodies.
    auto remNumContacts = static_cast<ContactCounter>(size(m_contacts)); // Remaining # of contacts.
    auto remNumJoints = static_cast<JointCounter>(size(m_joints)); // Remaining # of joints.

    // Clear all the island flags.
    // This builds the logical set of bodies, contacts, and joints eligible for resolution.
    // As bodies, contacts, or joints get added to resolution islands, they're essentially
    // removed from this eligible set.
    m_islandedBodies.clear();
    m_islandedContacts.clear();
    m_islandedJoints.clear();
    m_islandedBodies.resize(size(m_bodyBuffer));
    m_islandedContacts.resize(size(m_contactBuffer));
    m_islandedJoints.resize(size(m_jointBuffer));

#if defined(DO_THREADED)
    std::vector<std::future<IslandStats>> futures;
    futures.reserve(remNumBodies);
#endif
    // Build and simulate all awake islands.
    for (const auto& b: m_bodies) {
        if (!m_islandedBodies[UnderlyingValue(b)]) {
            auto& body = m_bodyBuffer[UnderlyingValue(b)];
            assert(!body.IsAwake() || body.IsSpeedable());
            if (body.IsAwake() && body.IsEnabled()) {
                ++stats.islandsFound;
                ::playrho::d2::Clear(m_island);
                // Size the island for the remaining un-evaluated bodies, contacts, and joints.
                Reserve(m_island, remNumBodies, remNumContacts, remNumJoints);
                AddToIsland(m_island, b, remNumBodies, remNumContacts, remNumJoints);
                remNumBodies += RemoveUnspeedablesFromIslanded(m_island.bodies, m_bodyBuffer,
                                                               m_islandedBodies);
#if defined(DO_THREADED)
                // Updates bodies' sweep.pos0 to current sweep.pos1 and bodies' sweep.pos1 to new positions
                futures.push_back(std::async(std::launch::async, &WorldImpl::SolveRegIslandViaGS,
                                             this, conf, m_island));
#else
                const auto solverResults = SolveRegIslandViaGS(conf, m_island);
                ::playrho::Update(stats, solverResults);
#endif
            }
        }
    }

#if defined(DO_THREADED)
    for (auto&& future: futures) {
        const auto solverResults = future.get();
        ::playrho::Update(stats, solverResults);
    }
#endif

    for (const auto& b: m_bodies) {
        if (m_islandedBodies[UnderlyingValue(b)]) {
            // A non-static body that was in an island may have moved.
            const auto& body = m_bodyBuffer[UnderlyingValue(b)];
            if (body.IsSpeedable()) {
                // Update fixtures (for broad-phase).
                stats.proxiesMoved += Synchronize(m_bodyFixtures[b.get()],
                                                  GetTransform0(body.GetSweep()),
                                                  GetTransformation(body),
                                                  conf.displaceMultiplier, conf.aabbExtension);
            }
        }
    }

    // Look for new contacts.
    stats.contactsAdded = FindNewContacts();
    
    return stats;
}

IslandStats WorldImpl::SolveRegIslandViaGS(const StepConf& conf, const Island& island)
{
    assert(!empty(island.bodies) || !empty(island.contacts) || !empty(island.joints));
    
    auto results = IslandStats{};
    results.positionIterations = conf.regPositionIterations;
    const auto h = conf.deltaTime; ///< Time step.

    // Update bodies' pos0 values.
    for_each(cbegin(island.bodies), cend(island.bodies), [&](const auto& bodyID) {
        auto& body = m_bodyBuffer[UnderlyingValue(bodyID)];
        body.SetPosition0(GetPosition1(body)); // like Advance0(1) on the sweep.
    });

    // Copy bodies' pos1 and velocity data into local arrays.
    auto bodyConstraints = GetBodyConstraints(island.bodies, m_bodyBuffer, h, GetMovementConf(conf));
    auto posConstraints = GetPositionConstraints(island.contacts,
                                                 m_fixtureBuffer, m_contactBuffer,
                                                 m_manifoldBuffer);
    auto velConstraints = GetVelocityConstraints(island.contacts,
                                                 m_fixtureBuffer, m_contactBuffer, m_manifoldBuffer,
                                                 bodyConstraints,
                                                 GetRegVelocityConstraintConf(conf));
    if (conf.doWarmStart)
    {
        WarmStartVelocities(velConstraints, bodyConstraints);
    }

    const auto psConf = GetRegConstraintSolverConf(conf);

    for_each(cbegin(island.joints), cend(island.joints), [&](const auto& id) {
        auto& joint = m_jointBuffer[UnderlyingValue(id)];
        InitVelocity(joint, bodyConstraints, conf, psConf);
    });
    
    results.velocityIterations = conf.regVelocityIterations;
    for (auto i = decltype(conf.regVelocityIterations){0}; i < conf.regVelocityIterations; ++i)
    {
        auto jointsOkay = true;
        for_each(cbegin(island.joints), cend(island.joints), [&](const auto& id) {
            auto& joint = m_jointBuffer[UnderlyingValue(id)];
            jointsOkay &= SolveVelocity(joint, bodyConstraints, conf);
        });

        // Note that the new incremental impulse can potentially be orders of magnitude
        // greater than the last incremental impulse used in this loop.
        const auto newIncImpulse = SolveVelocityConstraintsViaGS(velConstraints, bodyConstraints);
        results.maxIncImpulse = std::max(results.maxIncImpulse, newIncImpulse);

        if (jointsOkay && (newIncImpulse <= conf.regMinMomentum))
        {
            // No joint related velocity constraints were out of tolerance.
            // No body related velocity constraints were out of tolerance.
            // There does not appear to be any benefit to doing more loops now.
            // XXX: Is it really safe to bail now? Not certain of that.
            // Bail now assuming that this is helpful to do...
            results.velocityIterations = i + 1;
            break;
        }
    }
    
    // updates array of tentative new body positions per the velocities as if there were no obstacles...
    IntegratePositions(bodyConstraints, h);
    
    // Solve position constraints
    for (auto i = decltype(conf.regPositionIterations){0}; i < conf.regPositionIterations; ++i)
    {
        const auto minSeparation = SolvePositionConstraintsViaGS(posConstraints, bodyConstraints,
                                                                 psConf);
        results.minSeparation = std::min(results.minSeparation, minSeparation);
        const auto contactsOkay = (minSeparation >= conf.regMinSeparation);

        auto jointsOkay = true;
        for_each(cbegin(island.joints), cend(island.joints), [&](const auto& id) {
            auto& joint = m_jointBuffer[UnderlyingValue(id)];
            jointsOkay &= SolvePosition(joint, bodyConstraints, psConf);
        });

        if (contactsOkay && jointsOkay)
        {
            // Reached tolerance, early out...
            results.positionIterations = i + 1;
            results.solved = true;
            break;
        }
    }
    
    // Update normal and tangent impulses of contacts' manifold points
    for_each(cbegin(velConstraints), cend(velConstraints), [&](const VelocityConstraint& vc) {
        const auto i = static_cast<VelocityConstraints::size_type>(&vc - data(velConstraints));
        AssignImpulses(m_manifoldBuffer[island.contacts[i].get()], vc);
    });

    for (const auto& id: island.bodies) {
        const auto i = UnderlyingValue(id);
        const auto& bc = bodyConstraints[i];
        auto& body = m_bodyBuffer[i];
        // Could normalize position here to avoid unbounded angles but angular
        // normalization isn't handled correctly by joints that constrain rotation.
        body.JustSetVelocity(bc.GetVelocity());
        if (UpdateBody(body, bc.GetPosition())) {
            FlagForUpdating(m_contactBuffer, m_bodyContacts[i]);
        }
    }

    // XXX: Should contacts needing updating be updated now??

    if (m_postSolveContactListener)
    {
        Report(m_postSolveContactListener, island.contacts, velConstraints,
               results.solved? results.positionIterations - 1: StepConf::InvalidIteration);
    }
    
    results.bodiesSlept = BodyCounter{0};
    const auto minUnderActiveTime = UpdateUnderActiveTimes(island.bodies, m_bodyBuffer, conf);
    if ((minUnderActiveTime >= conf.minStillTimeToSleep) && results.solved)
    {
        results.bodiesSlept = static_cast<decltype(results.bodiesSlept)>(Sleepem(island.bodies,
                                                                                 m_bodyBuffer));
    }

    return results;
}

WorldImpl::UpdateContactsData
WorldImpl::UpdateContactTOIs(ArrayAllocator<Contact>& contactBuffer,
                             ArrayAllocator<Body>& bodyBuffer,
                             const ArrayAllocator<FixtureConf>& fixtureBuffer,
                             const Contacts& contacts, const StepConf& conf)
{
    auto results = UpdateContactsData{};

    const auto toiConf = GetToiConf(conf);
    for (const auto& contact: contacts)
    {
        auto& c = contactBuffer[UnderlyingValue(std::get<ContactID>(contact))];
        if (c.HasValidToi())
        {
            ++results.numValidTOI;
            continue;
        }
        if (!IsEnabled(c) || IsSensor(c) || !IsActive(c) || !IsImpenetrable(c))
        {
            continue;
        }
        if (c.GetToiCount() >= conf.maxSubSteps)
        {
            // What are the pros/cons of this?
            // Larger m_maxSubSteps slows down the simulation.
            // m_maxSubSteps of 44 and higher seems to decrease the occurrance of tunneling
            // of multiple bullet body collisions with static objects.
            ++results.numAtMaxSubSteps;
            continue;
        }

        auto& bA = bodyBuffer[UnderlyingValue(c.GetBodyA())];
        auto& bB = bodyBuffer[UnderlyingValue(c.GetBodyB())];

        /*
         * Put the sweeps onto the same time interval.
         * Presumably no unresolved collisions happen before the maximum of the bodies'
         * alpha-0 times. So long as the least TOI of the contacts is always the first
         * collision that gets dealt with, this presumption is safe.
         */
        const auto alpha0 = std::max(bA.GetSweep().GetAlpha0(), bB.GetSweep().GetAlpha0());
        assert(alpha0 >= 0 && alpha0 < 1);
        bA.Advance0(alpha0);
        bB.Advance0(alpha0);

        // Compute the TOI for this contact (one or both bodies are active and impenetrable).
        // Computes the time of impact in interval [0, 1]
        const auto proxyA = GetChild(GetShape(fixtureBuffer[UnderlyingValue(c.GetFixtureA())]),
                                     c.GetChildIndexA());
        const auto proxyB = GetChild(GetShape(fixtureBuffer[UnderlyingValue(c.GetFixtureB())]),
                                     c.GetChildIndexB());

        // Large rotations can make the root finder of TimeOfImpact fail, so normalize sweep angles.
        const auto sweepA = GetNormalized(bA.GetSweep());
        const auto sweepB = GetNormalized(bB.GetSweep());

        // Compute the TOI for this contact (one or both bodies are active and impenetrable).
        // Computes the time of impact in interval [0, 1]
        // Large rotations can make the root finder of TimeOfImpact fail, so normalize the sweep angles.
        const auto output = GetToiViaSat(proxyA, sweepA, proxyB, sweepB, toiConf);

        // Use Min function to handle floating point imprecision which possibly otherwise
        // could provide a TOI that's greater than 1.
        const auto toi = IsValidForTime(output.state)?
            std::min(alpha0 + (1 - alpha0) * output.time, Real{1}): Real{1};
        assert(toi >= alpha0 && toi <= 1);
        c.SetToi(toi);
        
        results.maxDistIters = std::max(results.maxDistIters, output.stats.max_dist_iters);
        results.maxToiIters = std::max(results.maxToiIters, output.stats.toi_iters);
        results.maxRootIters = std::max(results.maxRootIters, output.stats.max_root_iters);
        ++results.numUpdatedTOI;
    }

    return results;
}

WorldImpl::ContactToiData WorldImpl::GetSoonestContact(const Contacts& contacts,
                                                       const ArrayAllocator<Contact>& buffer) noexcept
{
    auto minToi = nextafter(Real{1}, Real{0});
    auto found = InvalidContactID;
    auto count = ContactCounter{0};
    for (const auto& contact: contacts)
    {
        const auto contactID = std::get<ContactID>(contact);
        const auto& c = buffer[UnderlyingValue(contactID)];
        if (c.HasValidToi())
        {
            const auto toi = c.GetToi();
            if (minToi > toi)
            {
                minToi = toi;
                found = contactID;
                count = 1;
            }
            else if (minToi == toi)
            {
                // Have multiple contacts at the current minimum time of impact.
                ++count;
            }
        }
    }
    return ContactToiData{found, minToi, count};
}

ToiStepStats WorldImpl::SolveToi(const StepConf& conf)
{
    auto stats = ToiStepStats{};

    if (IsStepComplete())
    {
        ResetBodiesForSolveTOI(m_bodies, m_bodyBuffer);
        Unset(m_islandedBodies, m_bodies);
        ResetContactsForSolveTOI(m_contactBuffer, m_contacts);
        Unset(m_islandedContacts, m_contacts);
    }

    const auto subStepping = GetSubStepping();

    // Find TOI events and solve them.
    for (;;)
    {
        const auto updateData = UpdateContactTOIs(m_contactBuffer, m_bodyBuffer, m_fixtureBuffer,
                                                  m_contacts, conf);
        stats.contactsAtMaxSubSteps += updateData.numAtMaxSubSteps;
        stats.contactsUpdatedToi += updateData.numUpdatedTOI;
        stats.maxDistIters = std::max(stats.maxDistIters, updateData.maxDistIters);
        stats.maxRootIters = std::max(stats.maxRootIters, updateData.maxRootIters);
        stats.maxToiIters = std::max(stats.maxToiIters, updateData.maxToiIters);
        
        const auto next = GetSoonestContact(m_contacts, m_contactBuffer);
        const auto contactID = next.contact;
        const auto ncount = next.simultaneous;
        if (contactID == InvalidContactID)
        {
            // No more TOI events to handle within the current time step. Done!
            SetStepComplete(true);
            break;
        }

        stats.maxSimulContacts = std::max(stats.maxSimulContacts,
                                          static_cast<decltype(stats.maxSimulContacts)>(ncount));
        stats.contactsFound += ncount;
        auto islandsFound = 0u;
        if (!m_islandedContacts[UnderlyingValue(contactID)]) {
#ifndef NDEBUG
            auto& contact = m_contactBuffer[UnderlyingValue(contactID)];
            /*
             * Confirm that contact is as it's supposed to be according to contract of the
             * GetSoonestContact method from which this contact was obtained.
             */
            assert(contact.IsEnabled());
            assert(!HasSensor(m_fixtureBuffer, contact));
            assert(IsActive(contact));
            assert(IsImpenetrable(contact));
#endif
            const auto solverResults = SolveToi(contactID, conf);
            stats.minSeparation = std::min(stats.minSeparation, solverResults.minSeparation);
            stats.maxIncImpulse = std::max(stats.maxIncImpulse, solverResults.maxIncImpulse);
            stats.islandsSolved += solverResults.solved;
            stats.sumPosIters += solverResults.positionIterations;
            stats.sumVelIters += solverResults.velocityIterations;
            if ((solverResults.positionIterations > 0) || (solverResults.velocityIterations > 0))
            {
                ++islandsFound;
            }
            stats.contactsUpdatedTouching += solverResults.contactsUpdated;
            stats.contactsSkippedTouching += solverResults.contactsSkipped;
        }
        stats.islandsFound += islandsFound;

        // Reset island flags and synchronize broad-phase proxies.
        for (const auto& b: m_bodies) {
            if (m_islandedBodies[UnderlyingValue(b)]) {
                m_islandedBodies[UnderlyingValue(b)] = false;
                auto& body = m_bodyBuffer[UnderlyingValue(b)];
                if (body.IsAccelerable()) {
                    const auto xfm0 = GetTransform0(body.GetSweep());
                    const auto xfm1 = GetTransformation(body);
                    stats.proxiesMoved += Synchronize(m_bodyFixtures[b.get()], xfm0, xfm1,
                                                      conf.displaceMultiplier, conf.aabbExtension);
                    const auto& bodyContacts = m_bodyContacts[b.get()];
                    ResetBodyContactsForSolveTOI(m_contactBuffer, bodyContacts);
                    Unset(m_islandedContacts, bodyContacts);
                }
            }
        }

        // Commit fixture proxy movements to the broad-phase so that new contacts are created.
        // Also, some contacts can be destroyed.
        stats.contactsAdded += FindNewContacts();

        if (subStepping)
        {
            SetStepComplete(false);
            break;
        }
    }
    return stats;
}

IslandStats WorldImpl::SolveToi(ContactID contactID, const StepConf& conf)
{
    // Note:
    //   This method is what used to be b2World::SolveToi(const b2TimeStep& step).
    //   It also differs internally from Erin's implementation.
    //
    //   Here's some specific behavioral differences:
    //   1. Bodies don't get their under-active times reset (like they do in Erin's code).

    auto contactsUpdated = ContactCounter{0};
    auto contactsSkipped = ContactCounter{0};

    auto& contact = m_contactBuffer[UnderlyingValue(contactID)];

    /*
     * Confirm that contact is as it's supposed to be according to contract of the
     * GetSoonestContacts method from which this contact should have been obtained.
     */
    assert(contact.IsEnabled());
    assert(!HasSensor(m_fixtureBuffer, contact));
    assert(IsActive(contact));
    assert(IsImpenetrable(contact));
    assert(!m_islandedContacts[UnderlyingValue(contactID)]);

    const auto toi = contact.GetToi();
    const auto bodyIdA = contact.GetBodyA();
    const auto bodyIdB = contact.GetBodyB();
    auto& bA = m_bodyBuffer[UnderlyingValue(bodyIdA)];
    auto& bB = m_bodyBuffer[UnderlyingValue(bodyIdB)];

    /* XXX: if (toi != 0)? */
    /* if (bA->GetSweep().GetAlpha0() != toi || bB->GetSweep().GetAlpha0() != toi) */
    // Seems contact manifold needs updating regardless.
    {
        const auto backupA = bA.GetSweep();
        const auto backupB = bB.GetSweep();

        // Advance the bodies to the TOI.
        assert(toi != 0 || (bA.GetSweep().GetAlpha0() == 0 && bB.GetSweep().GetAlpha0() == 0));
        bA.Advance(toi);
        FlagForUpdating(m_contactBuffer, m_bodyContacts[bodyIdA.get()]);
        bB.Advance(toi);
        FlagForUpdating(m_contactBuffer, m_bodyContacts[bodyIdB.get()]);

        // The TOI contact likely has some new contact points.
        contact.SetEnabled();
        if (contact.NeedsUpdating())
        {
            Update(contactID, GetUpdateConf(conf));
            ++contactsUpdated;
        }
        else
        {
            ++contactsSkipped;
        }
        contact.UnsetToi();
        contact.IncrementToiCount();

        // Is contact disabled or separated?
        //
        // XXX: Not often, but sometimes, contact.IsTouching() is false now.
        //      Seems like this is a bug, or at least suboptimal, condition.
        //      This method shouldn't be getting called unless contact has an
        //      impact indeed at the given TOI. Seen this happen in an edge-polygon
        //      contact situation where the polygon had a larger than default
        //      vertex radius. CollideShapes had called GetManifoldFaceB which
        //      was failing to see 2 clip points after GetClipPoints was called.
        if (!contact.IsEnabled() || !contact.IsTouching())
        {
            // assert(!contact.IsEnabled() || contact.IsTouching());
            contact.UnsetEnabled();
            bA.Restore(backupA);
            bB.Restore(backupB);
            auto results = IslandStats{};
            results.contactsUpdated += contactsUpdated;
            results.contactsSkipped += contactsSkipped;
            return results;
        }
    }
#if 0
    else if (!contact.IsTouching())
    {
        const auto newManifold = contact.Evaluate();
        assert(contact.IsTouching());
        return IslandSolverResults{};
    }
#endif
    if (bA.IsSpeedable())
    {
        bA.SetAwakeFlag();
        // XXX should the body's under-active time be reset here?
        //   Erin's code does for here but not in b2World::Solve(const b2TimeStep& step).
        //   Calling Body::ResetUnderActiveTime() has performance implications.
    }
    if (bB.IsSpeedable())
    {
        bB.SetAwakeFlag();
        // XXX should the body's under-active time be reset here?
        //   Erin's code does for here but not in b2World::Solve(const b2TimeStep& step).
        //   Calling Body::ResetUnderActiveTime() has performance implications.
    }

    // Build the island
    ::playrho::d2::Clear(m_island);
    ::playrho::d2::Reserve(m_island,
                           static_cast<BodyCounter>(used(m_bodyBuffer)),
                           static_cast<ContactCounter>(used(m_contactBuffer)),
                           static_cast<JointCounter>(0));

     // These asserts get triggered sometimes if contacts within TOI are iterated over.
    assert(!m_islandedBodies[UnderlyingValue(bodyIdA)]);
    assert(!m_islandedBodies[UnderlyingValue(bodyIdB)]);
    m_islandedBodies[UnderlyingValue(bodyIdA)] = true;
    m_islandedBodies[UnderlyingValue(bodyIdB)] = true;
    m_islandedContacts[UnderlyingValue(contactID)] = true;
    m_island.bodies.push_back(bodyIdA);
    m_island.bodies.push_back(bodyIdB);
    m_island.contacts.push_back(contactID);

    // Process the contacts of the two bodies, adding appropriate ones to the island,
    // adding appropriate other bodies of added contacts, and advancing those other
    // bodies sweeps and transforms to the minimum contact's TOI.
    if (bA.IsAccelerable())
    {
        const auto procOut = ProcessContactsForTOI(bodyIdA, m_island, toi, conf);
        contactsUpdated += procOut.contactsUpdated;
        contactsSkipped += procOut.contactsSkipped;
    }
    if (bB.IsAccelerable())
    {
        const auto procOut = ProcessContactsForTOI(bodyIdB, m_island, toi, conf);
        contactsUpdated += procOut.contactsUpdated;
        contactsSkipped += procOut.contactsSkipped;
    }

    RemoveUnspeedablesFromIslanded(m_island.bodies, m_bodyBuffer, m_islandedBodies);

    // Now solve for remainder of time step.
    auto subConf = StepConf{conf};
    subConf.deltaTime = (1 - toi) * conf.deltaTime;
    auto results = SolveToiViaGS(m_island, subConf);
    results.contactsUpdated += contactsUpdated;
    results.contactsSkipped += contactsSkipped;
    return results;
}

bool WorldImpl::UpdateBody(Body& body, const Position& pos)
{
    assert(IsValid(pos));
    body.SetPosition1(pos);
    const auto oldXfm = GetTransformation(body);
    const auto newXfm = GetTransformation(GetPosition1(body), GetLocalCenter(body));
    if (newXfm != oldXfm) {
        body.SetTransformation(newXfm);
        return true;
    }
    return false;
}

IslandStats WorldImpl::SolveToiViaGS(const Island& island, const StepConf& conf)
{
    auto results = IslandStats{};

    /*
     * Presumably the regular phase resolution has already taken care of updating the
     * body's velocity w.r.t. acceleration and damping such that this call here to get
     * the body constraint doesn't need to pass an elapsed time (and doesn't need to
     * update the velocity from what it already is).
     */
    auto bodyConstraints = GetBodyConstraints(island.bodies, m_bodyBuffer, 0_s,
                                              GetMovementConf(conf));

    // Initialize the body state.
#if 0
    for (auto&& contact: island.m_contacts) {
        const auto fixtureA = contact->GetFixtureA();
        const auto fixtureB = contact->GetFixtureB();
        const auto bodyA = fixtureA->GetBody();
        const auto bodyB = fixtureB->GetBody();

        bodyConstraintsMap[bodyA] = GetBodyConstraint(*bodyA);
        bodyConstraintsMap[bodyB] = GetBodyConstraint(*bodyB);
    }
#endif

    auto posConstraints = GetPositionConstraints(island.contacts,
                                                 m_fixtureBuffer, m_contactBuffer,
                                                 m_manifoldBuffer);

    // Solve TOI-based position constraints.
    assert(results.minSeparation == std::numeric_limits<Length>::infinity());
    assert(results.solved == false);
    results.positionIterations = conf.toiPositionIterations;
    {
        const auto psConf = GetToiConstraintSolverConf(conf);

        for (auto i = decltype(conf.toiPositionIterations){0}; i < conf.toiPositionIterations; ++i)
        {
            //
            // Note: There are two flavors of the SolvePositionConstraints function.
            //   One takes an extra two arguments that are the indexes of two bodies that are
            //   okay tomove. The other one does not.
            //   Calling the selective solver (that takes the two additional arguments) appears
            //   to result in phsyics simulations that are more prone to tunneling. Meanwhile,
            //   using the non-selective solver would presumably be slower (since it appears to
            //   have more that it will do). Assuming that slower is preferable to tunnelling,
            //   then the non-selective function is the one to be calling here.
            //
            const auto minSeparation = SolvePositionConstraintsViaGS(posConstraints,
                                                                     bodyConstraints, psConf);
            results.minSeparation = std::min(results.minSeparation, minSeparation);
            if (minSeparation >= conf.toiMinSeparation) {
                // Reached tolerance, early out...
                results.positionIterations = i + 1;
                results.solved = true;
                break;
            }
        }
    }

    // Leap of faith to new safe state.
    // Not doing this results in slower simulations.
    // Originally this update was only done to island.bodies 0 and 1.
    // Unclear whether rest of bodies should also be updated. No difference noticed.
    for (const auto& id: island.bodies) {
        const auto& bc = bodyConstraints[UnderlyingValue(id)];
        m_bodyBuffer[UnderlyingValue(id)].SetPosition0(bc.GetPosition());
    }

    auto velConstraints = GetVelocityConstraints(island.contacts,
                                                 m_fixtureBuffer, m_contactBuffer, m_manifoldBuffer,
                                                 bodyConstraints,
                                                 GetToiVelocityConstraintConf(conf));

    // No warm starting is needed for TOI events because warm
    // starting impulses were applied in the discrete solver.

    // Solve velocity constraints.
    assert(results.maxIncImpulse == 0_Ns);
    results.velocityIterations = conf.toiVelocityIterations;
    for (auto i = decltype(conf.toiVelocityIterations){0}; i < conf.toiVelocityIterations; ++i) {
        const auto newIncImpulse = SolveVelocityConstraintsViaGS(velConstraints, bodyConstraints);
        if (newIncImpulse <= conf.toiMinMomentum) {
            // No body related velocity constraints were out of tolerance.
            // There does not appear to be any benefit to doing more loops now.
            // XXX: Is it really safe to bail now? Not certain of that.
            // Bail now assuming that this is helpful to do...
            results.velocityIterations = i + 1;
            break;
        }
        results.maxIncImpulse = std::max(results.maxIncImpulse, newIncImpulse);
    }

    // Don't store TOI contact forces for warm starting because they can be quite large.

    IntegratePositions(bodyConstraints, conf.deltaTime);

    for (const auto& id: island.bodies) {
        const auto i = UnderlyingValue(id);
        auto& body = m_bodyBuffer[i];
        auto& bc = bodyConstraints[i];
        body.JustSetVelocity(bc.GetVelocity());
        if (UpdateBody(body, bc.GetPosition())) {
            FlagForUpdating(m_contactBuffer, m_bodyContacts[id.get()]);
        }
    }

    if (m_postSolveContactListener) {
        Report(m_postSolveContactListener, island.contacts, velConstraints, results.positionIterations);
    }

    return results;
}

WorldImpl::ProcessContactsOutput
WorldImpl::ProcessContactsForTOI(BodyID id, Island& island, Real toi, const StepConf& conf)
{
    const auto& body = m_bodyBuffer[UnderlyingValue(id)];

    assert(m_islandedBodies[UnderlyingValue(id)]);
    assert(body.IsAccelerable());
    assert(toi >= 0 && toi <= 1);

    auto results = ProcessContactsOutput{};
    assert(results.contactsUpdated == 0);
    assert(results.contactsSkipped == 0);
    
    const auto updateConf = GetUpdateConf(conf);

    // Note: the original contact (for body of which this method was called) already is-in-island.
    const auto bodyImpenetrable = body.IsImpenetrable();
    for (const auto& ci: m_bodyContacts[id.get()]) {
        const auto contactID = std::get<ContactID>(ci);
        if (!m_islandedContacts[UnderlyingValue(contactID)]) {
            auto& contact = m_contactBuffer[UnderlyingValue(contactID)];
            if (!contact.IsSensor()) {
                const auto bodyIdA = contact.GetBodyA();
                const auto bodyIdB = contact.GetBodyB();
                const auto otherId = (bodyIdA != id)? bodyIdA: bodyIdB;
                auto& other = m_bodyBuffer[UnderlyingValue(otherId)];
                if (bodyImpenetrable || other.IsImpenetrable()) {
                    const auto otherIslanded = m_islandedBodies[UnderlyingValue(otherId)];
                    {
                        const auto backup = other.GetSweep();
                        if (!otherIslanded /* && other->GetSweep().GetAlpha0() != toi */) {
                            other.Advance(toi);
                            FlagForUpdating(m_contactBuffer, m_bodyContacts[otherId.get()]);
                        }

                        // Update the contact points
                        contact.SetEnabled();
                        if (contact.NeedsUpdating()) {
                            Update(contactID, updateConf);
                            ++results.contactsUpdated;
                        }
                        else {
                            ++results.contactsSkipped;
                        }

                        // Revert and skip if contact disabled by user or not touching anymore (very possible).
                        if (!contact.IsEnabled() || !contact.IsTouching()) {
                            other.Restore(backup);
                            continue;
                        }
                    }
                    island.contacts.push_back(contactID);
                    m_islandedContacts[UnderlyingValue(contactID)] = true;
                    if (!otherIslanded) {
                        if (other.IsSpeedable()) {
                            other.SetAwakeFlag();
                        }
                        island.bodies.push_back(otherId);
                        m_islandedBodies[UnderlyingValue(otherId)] = true;
#if 0
                        if (other.IsAccelerable()) {
                            contactsUpdated += ProcessContactsForTOI(island, other, toi);
                        }
#endif
                    }
#ifndef NDEBUG
                    else {
                        /*
                         * If other is-in-island but not in current island, then something's gone wrong.
                         * Other needs to be in current island but was already in the island.
                         * A previous contact island didn't grow to include all the bodies it needed or
                         * perhaps the current contact is-touching while another one wasn't and the
                         * inconsistency is throwing things off.
                         */
                        assert(Count(island, otherId) > 0);
                    }
#endif
                }
            }
        }
    }
    return results;
}

StepStats WorldImpl::Step(const StepConf& conf)
{
    assert((Length{m_maxVertexRadius} * Real{2}) +
           (Length{conf.linearSlop} / Real{4}) > (Length{m_maxVertexRadius} * Real{2}));
    
    if (IsLocked())
    {
        throw WrongState("Step: world is locked");
    }

    // "Named return value optimization" (NRVO) will make returning this more efficient.
    auto stepStats = StepStats{};
    {
        FlagGuard<decltype(m_flags)> flagGaurd(m_flags, e_locked);

        CreateAndDestroyProxies(conf.aabbExtension);
        m_fixturesForProxies.clear();

        stepStats.pre.proxiesMoved = SynchronizeProxies(conf);
        // pre.proxiesMoved is usually zero but sometimes isn't.

        {
            // Note: this may update bodies (in addition to the contacts container).
            const auto destroyStats = DestroyContacts(m_contacts);
            stepStats.pre.destroyed = destroyStats.erased;
        }

        if (HasNewFixtures())
        {
            UnsetNewFixtures();
            
            // New fixtures were added: need to find and create the new contacts.
            // Note: this may update bodies (in addition to the contacts container).
            stepStats.pre.added = FindNewContacts();
        }

        if (conf.deltaTime != 0_s)
        {
            m_inv_dt0 = (conf.deltaTime != 0_s)? Real(1) / conf.deltaTime: 0_Hz;

            // Could potentially run UpdateContacts multithreaded over split lists...
            const auto updateStats = UpdateContacts(conf);
            stepStats.pre.ignored = updateStats.ignored;
            stepStats.pre.updated = updateStats.updated;
            stepStats.pre.skipped = updateStats.skipped;

            // Integrate velocities, solve velocity constraints, and integrate positions.
            if (IsStepComplete())
            {
                stepStats.reg = SolveReg(conf);
            }

            // Handle TOI events.
            if (conf.doToi)
            {
                stepStats.toi = SolveToi(conf);
            }
        }
    }
    return stepStats;
}

void WorldImpl::ShiftOrigin(Length2 newOrigin)
{
    if (IsLocked())
    {
        throw WrongState("ShiftOrigin: world is locked");
    }

    // Optimize for newOrigin being different than current...
    const auto bodies = GetBodies();
    for (const auto& body: bodies)
    {
        auto& b = m_bodyBuffer[UnderlyingValue(body)];
        auto transformation = GetTransformation(b);
        transformation.p -= newOrigin;
        b.SetTransformation(transformation);
        FlagForUpdating(m_contactBuffer, m_bodyContacts[body.get()]);
        auto sweep = b.GetSweep();
        sweep.pos0.linear -= newOrigin;
        sweep.pos1.linear -= newOrigin;
        b.SetSweep(sweep);
    }

    for_each(begin(m_joints), end(m_joints), [&](const auto& joint) {
        auto& j = m_jointBuffer[UnderlyingValue(joint)];
        ::playrho::d2::ShiftOrigin(j, newOrigin);
    });

    m_tree.ShiftOrigin(newOrigin);
}

void WorldImpl::InternalDestroy(ContactID contactID, const Body* from)
{
    assert(contactID != InvalidContactID);
    auto& contact = m_contactBuffer[UnderlyingValue(contactID)];
    if (m_endContactListener && contact.IsTouching()) {
        // EndContact hadn't been called in DestroyOrUpdateContacts() since is-touching,
        //  so call it now
        m_endContactListener(contactID);
    }
    const auto bodyIdA = contact.GetBodyA();
    const auto bodyIdB = contact.GetBodyB();
    const auto bodyA = &m_bodyBuffer[UnderlyingValue(bodyIdA)];
    const auto bodyB = &m_bodyBuffer[UnderlyingValue(bodyIdB)];
    if (bodyA != from) {
        auto& bodyContacts = m_bodyContacts[bodyIdA.get()];
        const auto it = FindTypeValue(bodyContacts, contactID);
        if (it != end(bodyContacts)) {
            bodyContacts.erase(it);
        }
    }
    if (bodyB != from) {
        auto& bodyContacts = m_bodyContacts[bodyIdB.get()];
        const auto it = FindTypeValue(bodyContacts, contactID);
        if (it != end(bodyContacts)) {
            bodyContacts.erase(it);
        }
    }
    auto& manifold = m_manifoldBuffer[UnderlyingValue(contactID)];
    if ((manifold.GetPointCount() > 0) && !contact.IsSensor()) {
        // Contact may have been keeping accelerable bodies of fixture A or B from moving.
        // Need to awaken those bodies now in case they are again movable.
        bodyA->SetAwake();
        bodyB->SetAwake();
    }
    m_contactBuffer.Free(UnderlyingValue(contactID));
    m_manifoldBuffer.Free(UnderlyingValue(contactID));
}

void WorldImpl::Destroy(ContactID contactID, const Body* from)
{
    assert(contactID != InvalidContactID);
    const auto it = FindTypeValue(m_contacts, contactID);
    if (it != cend(m_contacts)) {
        m_contacts.erase(it);
    }
    InternalDestroy(contactID, from);
}

bool WorldImpl::IsDestroyed(ContactID id) const noexcept
{
    return m_contactBuffer.FindFree(id.get());
}

WorldImpl::DestroyContactsStats WorldImpl::DestroyContacts(Contacts& contacts)
{
    const auto beforeSize = size(contacts);
    contacts.erase(std::remove_if(begin(contacts), end(contacts), [&](const auto& c)
    {
        const auto key = std::get<ContactKey>(c);
        const auto contactID = std::get<ContactID>(c);

        if (!TestOverlap(m_tree, key.GetMin(), key.GetMax()))
        {
            // Destroy contacts that cease to overlap in the broad-phase.
            InternalDestroy(contactID);
            return true;
        }

        // Is this contact flagged for filtering?
        auto& contact = m_contactBuffer[UnderlyingValue(contactID)];
        if (contact.NeedsFiltering())
        {
            const auto bodyIdA = contact.GetBodyA();
            const auto bodyIdB = contact.GetBodyB();
            const auto& bodyA = m_bodyBuffer[UnderlyingValue(bodyIdA)];
            const auto& bodyB = m_bodyBuffer[UnderlyingValue(bodyIdB)];
            const auto& fixtureA = m_fixtureBuffer[UnderlyingValue(contact.GetFixtureA())];
            const auto& fixtureB = m_fixtureBuffer[UnderlyingValue(contact.GetFixtureB())];
            if (!EitherIsAccelerable(bodyA, bodyB) ||
                !ShouldCollide(m_jointBuffer, m_bodyJoints, bodyIdA, bodyIdB) ||
                !ShouldCollide(fixtureA, fixtureB))
            {
                InternalDestroy(contactID);
                return true;
            }
            contact.UnflagForFiltering();
        }

        return false;
    }), end(contacts));
    const auto afterSize = size(contacts);

    auto stats = DestroyContactsStats{};
    stats.ignored = static_cast<ContactCounter>(afterSize);
    stats.erased = static_cast<ContactCounter>(beforeSize - afterSize);
    return stats;
}

WorldImpl::UpdateContactsStats WorldImpl::UpdateContacts(const StepConf& conf)
{
#ifdef DO_PAR_UNSEQ
    atomic<uint32_t> ignored;
    atomic<uint32_t> updated;
    atomic<uint32_t> skipped;
#else
    auto ignored = uint32_t{0};
    auto updated = uint32_t{0};
    auto skipped = uint32_t{0};
#endif

    const auto updateConf = GetUpdateConf(conf);
    
#if defined(DO_THREADED)
    std::vector<ContactID> contactsNeedingUpdate;
    contactsNeedingUpdate.reserve(size(m_contacts));
    std::vector<std::future<void>> futures;
    futures.reserve(size(m_contacts));
#endif

    // Update awake contacts.
    for_each(/*execution::par_unseq,*/ begin(m_contacts), end(m_contacts), [&](const auto& c) {
        const auto contactID = std::get<ContactID>(c);
        auto& contact = m_contactBuffer[UnderlyingValue(contactID)];
#if 0
        Update(contact, updateConf);
        ++updated;
#else
        const auto& bodyA = m_bodyBuffer[UnderlyingValue(contact.GetBodyA())];
        const auto& bodyB = m_bodyBuffer[UnderlyingValue(contact.GetBodyB())];

        // Awake && speedable (dynamic or kinematic) means collidable.
        // At least one body must be collidable
        assert(!bodyA.IsAwake() || bodyA.IsSpeedable());
        assert(!bodyB.IsAwake() || bodyB.IsSpeedable());
        if (!bodyA.IsAwake() && !bodyB.IsAwake())
        {
            // This sometimes fails... is it important?
            //assert(!contact.HasValidToi());
            ++ignored;
            return;
        }

        // Possible that bodyA->GetSweep().GetAlpha0() != 0
        // Possible that bodyB->GetSweep().GetAlpha0() != 0

        // Update the contact manifold and notify the listener.
        contact.SetEnabled();

        // Note: ideally contacts are only updated if there was a change to:
        //   - The fixtures' sensor states.
        //   - The fixtures bodies' transformations.
        //   - The "maxCirclesRatio" per-step configuration state if contact IS NOT for sensor.
        //   - The "maxDistanceIters" per-step configuration state if contact IS for sensor.
        //
        if (contact.NeedsUpdating())
        {
            // The following may call listener but is otherwise thread-safe.
#if defined(DO_THREADED)
            contactsNeedingUpdate.push_back(contactID);
            //futures.push_back(async(&Update, this, *contact, conf)));
            //futures.push_back(async(launch::async, [=]{ Update(*contact, conf); }));
#else
            Update(contactID, updateConf);
#endif
        	++updated;
        }
        else
        {
            ++skipped;
        }
#endif
    });
    
#if defined(DO_THREADED)
    auto numJobs = size(contactsNeedingUpdate);
    const auto jobsPerCore = numJobs / 4;
    for (auto i = decltype(numJobs){0}; numJobs > 0 && i < 3; ++i)
    {
        futures.push_back(std::async(std::launch::async, [=]{
            const auto offset = jobsPerCore * i;
            for (auto j = decltype(jobsPerCore){0}; j < jobsPerCore; ++j)
            {
	            Update(contactsNeedingUpdate[offset + j], updateConf);
            }
        }));
        numJobs -= jobsPerCore;
    }
    if (numJobs > 0)
    {
        futures.push_back(std::async(std::launch::async, [=]{
            const auto offset = jobsPerCore * 3;
            for (auto j = decltype(numJobs){0}; j < numJobs; ++j)
            {
                Update(contactsNeedingUpdate[offset + j], updateConf);
            }
        }));
    }
    for (auto&& future: futures)
    {
        future.get();
    }
#endif
    
    return UpdateContactsStats{
        static_cast<ContactCounter>(ignored),
        static_cast<ContactCounter>(updated),
        static_cast<ContactCounter>(skipped)
    };
}

ContactCounter WorldImpl::FindNewContacts()
{
    m_proxyKeys.clear();

    // Accumalate contact keys for pairs of nodes that are overlapping and aren't identical.
    // Note that if the dynamic tree node provides the body pointer, it's assumed to be faster
    // to eliminate any node pairs that have the same body here before the key pairs are
    // sorted.
    for_each(cbegin(m_proxies), cend(m_proxies), [&](ProxyId pid) {
        const auto body0 = m_tree.GetLeafData(pid).body;
        const auto aabb = m_tree.GetAABB(pid);
        Query(m_tree, aabb, [&](ProxyId nodeId) {
            const auto body1 = m_tree.GetLeafData(nodeId).body;
            // A proxy cannot form a pair with itself.
            if ((nodeId != pid) && (body0 != body1))
            {
                m_proxyKeys.push_back(ContactKey{nodeId, pid});
            }
            return DynamicTreeOpcode::Continue;
        });
    });
    m_proxies.clear();

    // Sort and eliminate any duplicate contact keys.
    sort(begin(m_proxyKeys), end(m_proxyKeys));
    m_proxyKeys.erase(unique(begin(m_proxyKeys), end(m_proxyKeys)), end(m_proxyKeys));

    const auto numContactsBefore = size(m_contacts);
    for_each(cbegin(m_proxyKeys), cend(m_proxyKeys), [&](ContactKey key)
    {
        Add(key);
    });
    m_islandedContacts.resize(size(m_contactBuffer));
    const auto numContactsAfter = size(m_contacts);
    return static_cast<ContactCounter>(numContactsAfter - numContactsBefore);
}

bool WorldImpl::Add(ContactKey key)
{
    const auto minKeyLeafData = m_tree.GetLeafData(key.GetMin());
    const auto maxKeyLeafData = m_tree.GetLeafData(key.GetMax());

    const auto bodyIdA = minKeyLeafData.body; // fixtureA->GetBody();
    const auto fixtureIdA = minKeyLeafData.fixture;
    const auto indexA = minKeyLeafData.childIndex;
    const auto bodyIdB = maxKeyLeafData.body; // fixtureB->GetBody();
    const auto fixtureIdB = maxKeyLeafData.fixture;
    const auto indexB = maxKeyLeafData.childIndex;

#if 0
    // Are the fixtures on the same body? They can be, and they often are.
    // Don't need nor want a contact for these fixtures if they are on the same body.
    if (bodyIdA == bodyIdB)
    {
        return false;
    }
#endif
    assert(bodyIdA != bodyIdB);
    assert(fixtureIdA != fixtureIdB);

    auto& bodyA = m_bodyBuffer[UnderlyingValue(bodyIdA)];
    auto& bodyB = m_bodyBuffer[UnderlyingValue(bodyIdB)];
    auto& fixtureA = m_fixtureBuffer[UnderlyingValue(fixtureIdA)];
    auto& fixtureB = m_fixtureBuffer[UnderlyingValue(fixtureIdB)];

    // Does a joint override collision? Is at least one body dynamic?
    if (!EitherIsAccelerable(bodyA, bodyB) ||
        !ShouldCollide(m_jointBuffer, m_bodyJoints, bodyIdA, bodyIdB) ||
        !ShouldCollide(fixtureA, fixtureB))
    {
        return false;
    }
   
#ifndef NO_RACING
    // Code herein may be racey in a multithreaded context...
    // Would need a lock on bodyA, bodyB, and contacts.
    // A global lock on the world instance should work but then would it have so much
    // contention as to make multi-threaded handing of adding new connections senseless?

    // Have to quickly figure out if there's a contact already added for the current
    // fixture-childindex pair that this method's been called for.
    //
    // In cases where there's a bigger bullet-enabled object that's colliding with lots of
    // smaller objects packed tightly together and overlapping like in the Add Pair Stress
    // Test demo that has some 400 smaller objects, the bigger object could have 387 contacts
    // while the smaller object has 369 or more, and the total world contact count can be over
    // 30,495. While searching linearly through the object with less contacts should help,
    // that may still be a lot of contacts to be going through in the context this method
    // is being called. OTOH, speed seems to be dominated by cache hit-ratio...
    //
    // With compiler optimization enabled and 400 small bodies and Real=double...
    // For world:
    //   World::set<Contact*> shows up as .524 seconds max step
    //   World::list<Contact> shows up as .482 seconds max step.
    // For body:
    //    using contact map w/ proxy ID keys shows up as .561
    // W/ unordered_map: .529 seconds max step (step 15).
    // W/ World::list<Contact> and Body::list<ContactKey,Contact*>   .444s@step15, 1.063s-sumstep20
    // W/ World::list<Contact> and Body::list<ContactKey,Contact*>   .393s@step15, 1.063s-sumstep20
    // W/ World::list<Contact> and Body::list<ContactKey,Contact*>   .412s@step15, 1.012s-sumstep20
    // W/ World::list<Contact> and Body::vector<ContactKey,Contact*> .219s@step15, 0.659s-sumstep20

    // Does a contact already exist?
    // Identify body with least contacts and search it.
    // NOTE: Time trial testing found the following rough ordering of data structures, to be
    // fastest to slowest: vector, list, unorderered_set, unordered_map,
    //     set, map.
    auto& contactsA = m_bodyContacts[bodyIdA.get()];
    auto& contactsB = m_bodyContacts[bodyIdB.get()];
    const auto& bodyContacts = (size(contactsA) < size(contactsB))? contactsA: contactsB;
    const auto it = FindTypeValue(bodyContacts, key);
    if (it != cend(bodyContacts)) {
        return false;
    }

    if (size(m_contacts) >= MaxContacts) {
        // New contact was needed, but denied due to MaxContacts count being reached.
        return false;
    }

    const auto contactID = static_cast<ContactID>(static_cast<ContactID::underlying_type>(
        m_contactBuffer.Allocate(bodyIdA, fixtureIdA, indexA, bodyIdB, fixtureIdB, indexB)));
    m_manifoldBuffer.Allocate();
    auto& contact = m_contactBuffer[UnderlyingValue(contactID)];
    if (bodyA.IsImpenetrable() || bodyB.IsImpenetrable()) {
        contact.SetImpenetrable();
    }
    if (bodyA.IsAwake() || bodyB.IsAwake()) {
        contact.SetIsActive();
    }
    if (IsSensor(fixtureA) || IsSensor(fixtureB)) {
        contact.SetIsSensor();
    }
    contact.SetFriction(GetDefaultFriction(fixtureA, fixtureB));
    contact.SetRestitution(GetDefaultRestitution(fixtureA, fixtureB));

    // Insert into the contacts container.
    //
    // Should the new contact be added at front or back?
    //
    // Original strategy added to the front. Since processing done front to back, front
    // adding means container more a LIFO container, while back adding means more a FIFO.
    //
    m_contacts.push_back(KeyedContactPtr{key, contactID});

    // TODO: check contactID unique in contacts containers if !NDEBUG
    contactsA.emplace_back(key, contactID);
    contactsB.emplace_back(key, contactID);

    // Wake up the bodies
    if (!contact.IsSensor()) {
        if (bodyA.IsSpeedable()) {
            bodyA.SetAwakeFlag();
        }
        if (bodyB.IsSpeedable()) {
            bodyB.SetAwakeFlag();
        }
    }
#endif

    return true;
}

void WorldImpl::CreateAndDestroyProxies(Length extension)
{
    for_each(begin(m_fixturesForProxies), end(m_fixturesForProxies),
             [this,extension](const auto& fixtureID) {
        auto& fixture = m_fixtureBuffer[UnderlyingValue(fixtureID)];
        auto& fixtureProxies = m_fixtureProxies[fixtureID.get()];
        const auto bodyID = ::playrho::d2::GetBody(fixture);
        auto& body = m_bodyBuffer[UnderlyingValue(bodyID)];
        const auto enabled = body.IsEnabled();

        if (fixtureProxies.empty()) {
            if (enabled) {
                CreateProxies(m_tree, fixtureProxies,
                              bodyID, fixtureID, GetShape(fixture),
                              GetTransformation(body), extension);
                AddProxies(fixtureProxies);
            }
        }
        else {
            if (!enabled) {
                DestroyProxies(m_tree, fixtureProxies, m_proxies);
                // Destroy any contacts associated with the fixture.
                Erase(m_bodyContacts[bodyID.get()], [this,fixtureID,&body](ContactID contactID) {
                    const auto& contact = m_contactBuffer[UnderlyingValue(contactID)];
                    const auto fixtureA = contact.GetFixtureA();
                    const auto fixtureB = contact.GetFixtureB();
                    if ((fixtureA == fixtureID) || (fixtureB == fixtureID)) {
                        Destroy(contactID, &body);
                        return true;
                    }
                    return false;
                });
            }
        }
    });
}

PreStepStats::counter_type WorldImpl::SynchronizeProxies(const StepConf& conf)
{
    auto proxiesMoved = PreStepStats::counter_type{0};
    for_each(begin(m_bodiesForProxies), end(m_bodiesForProxies), [&](const auto& bodyID) {
        const auto& b = m_bodyBuffer[UnderlyingValue(bodyID)];
        const auto xfm = GetTransformation(b);
        // Not always true: assert(GetTransform0(b->GetSweep()) == xfm);
        proxiesMoved += Synchronize(m_bodyFixtures[bodyID.get()], xfm, xfm,
                                    conf.displaceMultiplier, conf.aabbExtension);
    });
    m_bodiesForProxies.clear();
    return proxiesMoved;
}

const WorldImpl::Proxies& WorldImpl::GetProxies(FixtureID id) const
{
    return m_fixtureProxies.at(id.get());
}

FixtureID WorldImpl::CreateFixture(const FixtureConf& def)
{
    {
        const auto childCount = GetChildCount(def.shape);
        const auto minVertexRadius = GetMinVertexRadius();
        const auto maxVertexRadius = GetMaxVertexRadius();
        for (auto i = ChildCounter{0}; i < childCount; ++i) {
            const auto vr = GetVertexRadius(def.shape, i);
            if (!(vr >= minVertexRadius)) {
                throw InvalidArgument("CreateFixture: vertex radius < min");
            }
            if (!(vr <= maxVertexRadius)) {
                throw InvalidArgument("CreateFixture: vertex radius > max");
            }
        }
    }
    if (IsLocked()) {
        throw WrongState("CreateFixture: world is locked");
    }
    if (size(m_fixtureBuffer) >= MaxFixtures) {
        throw LengthError("CreateFixture: operation would exceed MaxFixtures");
    }
    // The following must be called before any mutating actions to validate def.body!
    auto& body = m_bodyBuffer.at(def.body.get());
    const auto fixtureID = static_cast<FixtureID>(
        static_cast<FixtureID::underlying_type>(m_fixtureBuffer.Allocate(def)));
    m_fixtureProxies.Allocate();
    AddFixture(def.body, fixtureID);
    if (body.IsEnabled()) {
        m_fixturesForProxies.push_back(fixtureID);
    }
    // Adjust mass properties if needed.
    if (GetDensity(m_fixtureBuffer[UnderlyingValue(fixtureID)]) > 0_kgpm2) {
        body.SetMassDataDirty();
    }
    // Let the world know we have a new fixture. This will cause new contacts
    // to be created at the beginning of the next time step.
    m_flags |= e_newFixture;
    return fixtureID;
}

void WorldImpl::SetFixture(FixtureID id, const FixtureConf& value)
{
    auto& variable = m_fixtureBuffer.at(UnderlyingValue(id));
    if ((::playrho::d2::GetBody(variable) != ::playrho::d2::GetBody(value)) ||
        (GetShape(variable) != GetShape(value))) {
        if (::playrho::d2::GetBody(value).get() >= m_bodyBuffer.size()) {
            throw std::invalid_argument("body for fixture does not exist!");
        }
        Destroy(id);
        [[maybe_unused]] const auto newId = CreateFixture(value);
        assert(id == newId);
        return;
    }
    const auto bodyID = ::playrho::d2::GetBody(value);
    assert(bodyID == ::playrho::d2::GetBody(variable));
    m_bodyBuffer[bodyID.get()].SetAwake();
    if (GetFilterData(variable) != GetFilterData(value)) {
        FlagForFiltering(m_contactBuffer, m_bodyContacts[bodyID.get()], id);
        AddProxies(m_fixtureProxies[id.get()]); // not end of world if this throws
    }
    if (IsSensor(variable) != IsSensor(value)) {
        FlagForUpdating(m_contactBuffer, m_bodyContacts[bodyID.get()]);
    }
    variable = value;
}

const FixtureConf& WorldImpl::GetFixture(FixtureID id) const
{
    return m_fixtureBuffer.at(UnderlyingValue(id));
}

SizedRange<WorldImpl::Contacts::const_iterator> WorldImpl::GetContacts(BodyID id) const
{
    const auto& container =  m_bodyContacts.at(id.get());
    return SizedRange<WorldImpl::Contacts::const_iterator>{
        begin(container), end(container), size(container)
    };
}

SizedRange<WorldImpl::BodyJoints::const_iterator> WorldImpl::GetJoints(BodyID id) const
{
    const auto& container = m_bodyJoints.at(id.get());
    return SizedRange<WorldImpl::BodyJoints::const_iterator>{
        begin(container), end(container), size(container)
    };
}

SizedRange<WorldImpl::Fixtures::const_iterator> WorldImpl::GetFixtures(BodyID id) const
{
    const auto& container = m_bodyFixtures.at(id.get());
    return SizedRange<WorldImpl::Fixtures::const_iterator>{
        begin(container), end(container), size(container)
    };
}

void WorldImpl::AddFixture(BodyID id, FixtureID fixture)
{
    m_bodyFixtures.at(id.get()).push_back(fixture);
}

bool WorldImpl::RemoveFixture(BodyID id, FixtureID fixture)
{
    auto& fixtures = m_bodyFixtures.at(id.get());
    const auto begIter = begin(fixtures);
    const auto endIter = end(fixtures);
    const auto it = std::find(begIter, endIter, fixture);
    if (it != endIter) {
        fixtures.erase(it);
        return true;
    }
    return false;
}

bool WorldImpl::Destroy(FixtureID id)
{
    if (IsLocked()) {
        throw WrongState("Destroy: world is locked");
    }
#if 0
    /*
     * XXX: Should the destruction listener be called when the user requested that
     *   the fixture be destroyed or only when the fixture is destroyed indirectly?
     */
    if (m_fixtureDestructionListener) {
        m_fixtureDestructionListener(id);
    }
#endif
    const auto bodyID = ::playrho::d2::GetBody(GetFixture(id));
    auto& body = m_bodyBuffer[UnderlyingValue(bodyID)];
    // Destroy any contacts associated with the fixture.
    Erase(m_bodyContacts[bodyID.get()], [this,id,&body](ContactID contactID) {
        auto& contact = m_contactBuffer[UnderlyingValue(contactID)];
        const auto fixtureA = contact.GetFixtureA();
        const auto fixtureB = contact.GetFixtureB();
        if ((fixtureA == id) || (fixtureB == id)) {
            Destroy(contactID, &body);
            return true;
        }
        return false;
    });
    EraseAll(m_fixturesForProxies, id);
    DestroyProxies(m_tree, m_fixtureProxies[id.get()], m_proxies);
    if (!RemoveFixture(bodyID, id)) {
        // Fixture probably destroyed already.
        return false;
    }
    m_fixtureBuffer.Free(UnderlyingValue(id));
    m_fixtureProxies.Free(UnderlyingValue(id));
    body.SetMassDataDirty();
    return true;
}

bool WorldImpl::IsDestroyed(FixtureID id) const noexcept
{
    return m_fixtureBuffer.FindFree(id.get());
}

ContactCounter WorldImpl::Synchronize(const Fixtures& fixtures,
                                      const Transformation& xfm1, const Transformation& xfm2,
                                      Real multiplier, Length extension)
{
    auto updatedCount = ContactCounter{0};
    assert(::playrho::IsValid(xfm1));
    assert(::playrho::IsValid(xfm2));
    const auto displacement = multiplier * (xfm2.p - xfm1.p);
    for_each(cbegin(fixtures), cend(fixtures), [&](const auto& fixtureID) {
        const auto& shape = GetShape(m_fixtureBuffer[fixtureID.get()]);
        auto childIndex = ChildCounter{0};
        for (const auto& treeId: m_fixtureProxies[fixtureID.get()]) {
            // Compute an AABB that covers the swept shape (may miss some rotation effect).
            const auto aabb = ComputeAABB(GetChild(shape, childIndex), xfm1, xfm2);
            if (!Contains(m_tree.GetAABB(treeId), aabb)) {
                const auto newAabb = GetDisplacedAABB(GetFattenedAABB(aabb, extension),
                                                      displacement);
                m_tree.UpdateLeaf(treeId, newAabb);
                m_proxies.push_back(treeId);
                ++updatedCount;
            }
            ++childIndex;
        }
    });
    return updatedCount;
}

void WorldImpl::Update(ContactID contactID, const ContactUpdateConf& conf)
{
    auto& c = m_contactBuffer[UnderlyingValue(contactID)];
    auto& manifold = m_manifoldBuffer[UnderlyingValue(contactID)];
    const auto oldManifold = manifold;

    // Note: do not assume the fixture AABBs are overlapping or are valid.
    const auto oldTouching = c.IsTouching();
    auto newTouching = false;

    const auto bodyIdA = c.GetBodyA();
    const auto fixtureIdA = c.GetFixtureA();
    const auto indexA = c.GetChildIndexA();
    const auto bodyIdB = c.GetBodyB();
    const auto fixtureIdB = c.GetFixtureB();
    const auto indexB = c.GetChildIndexB();
    const auto& fixtureA = m_fixtureBuffer[UnderlyingValue(fixtureIdA)];
    const auto& fixtureB = m_fixtureBuffer[UnderlyingValue(fixtureIdB)];
    const auto& shapeA = GetShape(fixtureA);
    const auto& bodyA = m_bodyBuffer[UnderlyingValue(bodyIdA)];
    const auto& bodyB = m_bodyBuffer[UnderlyingValue(bodyIdB)];
    const auto xfA = GetTransformation(bodyA);
    const auto& shapeB = GetShape(fixtureB);
    const auto xfB = GetTransformation(bodyB);
    const auto childA = GetChild(shapeA, indexA);
    const auto childB = GetChild(shapeB, indexB);

    // NOTE: Ideally, the touching state returned by the TestOverlap function
    //   agrees 100% of the time with that returned from the CollideShapes function.
    //   This is not always the case however especially as the separation or overlap
    //   approaches zero.
#define OVERLAP_TOLERANCE (SquareMeter / Real(20))

    const auto sensor = IsSensor(fixtureA) || IsSensor(fixtureB);
    if (sensor)
    {
        const auto overlapping = TestOverlap(childA, xfA, childB, xfB, conf.distance);
        newTouching = (overlapping >= 0_m2);

#ifdef OVERLAP_TOLERANCE
#ifndef NDEBUG
        const auto tolerance = OVERLAP_TOLERANCE;
        const auto newManifold = CollideShapes(childA, xfA, childB, xfB, conf.manifold);
        assert(newTouching == (newManifold.GetPointCount() > 0) ||
               abs(overlapping) < tolerance);
#endif
#endif

        // Sensors don't generate manifolds.
        manifold = Manifold{};
    }
    else
    {
        auto newManifold = CollideShapes(childA, xfA, childB, xfB, conf.manifold);

        const auto old_point_count = oldManifold.GetPointCount();
        const auto new_point_count = newManifold.GetPointCount();

        newTouching = new_point_count > 0;

#ifdef OVERLAP_TOLERANCE
#ifndef NDEBUG
        const auto tolerance = OVERLAP_TOLERANCE;
        const auto overlapping = TestOverlap(childA, xfA, childB, xfB, conf.distance);
        assert(newTouching == (overlapping >= 0_m2) ||
               abs(overlapping) < tolerance);
#endif
#endif
        // Match old contact ids to new contact ids and copy the stored impulses to warm
        // start the solver. Note: missing any opportunities to warm start the solver
        // results in squishier stacking and less stable simulations.
        bool found[2] = {false, new_point_count < 2};
        for (auto i = decltype(new_point_count){0}; i < new_point_count; ++i)
        {
            const auto new_cf = newManifold.GetContactFeature(i);
            for (auto j = decltype(old_point_count){0}; j < old_point_count; ++j)
            {
                if (new_cf == oldManifold.GetContactFeature(j))
                {
                    found[i] = true;
                    newManifold.SetContactImpulses(i, oldManifold.GetContactImpulses(j));
                    break;
                }
            }
        }
        // If warm starting data wasn't found for a manifold point via contact feature
        // matching, it's better to just set the data to whatever old point is closest
        // to the new one.
        for (auto i = decltype(new_point_count){0}; i < new_point_count; ++i)
        {
            if (!found[i])
            {
                auto leastSquareDiff = std::numeric_limits<Area>::infinity();
                const auto newPt = newManifold.GetPoint(i);
                for (auto j = decltype(old_point_count){0}; j < old_point_count; ++j)
                {
                    const auto oldPt = oldManifold.GetPoint(j);
                    const auto squareDiff = GetMagnitudeSquared(oldPt.localPoint - newPt.localPoint);
                    if (leastSquareDiff > squareDiff)
                    {
                        leastSquareDiff = squareDiff;
                        newManifold.SetContactImpulses(i, oldManifold.GetContactImpulses(j));
                    }
                }
            }
        }

        // Ideally this method is **NEVER** called unless a dependency changed such
        // that the following assertion is **ALWAYS** valid.
        //assert(newManifold != oldManifold);

        manifold = newManifold;

#ifdef MAKE_CONTACT_PROCESSING_ORDER_DEPENDENT
        /*
         * The following code creates an ordering dependency in terms of update processing
         * over a container of contacts. It also puts this method into the situation of
         * modifying bodies which adds race potential in a multi-threaded mode of operation.
         * Lastly, without this code, the step-statistics show a world getting to sleep in
         * less TOI position iterations.
         */
        if (newTouching != oldTouching)
        {
            bodyA.SetAwake();
            bodyB.SetAwake();
        }
#endif
    }

    c.UnflagForUpdating();

    if (!oldTouching && newTouching)
    {
        c.SetTouching();
        if (m_beginContactListener)
        {
            m_beginContactListener(contactID);
        }
    }
    else if (oldTouching && !newTouching)
    {
        c.UnsetTouching();
        if (m_endContactListener)
        {
            m_endContactListener(contactID);
        }
    }

    if (!sensor && newTouching)
    {
        if (m_preSolveContactListener)
        {
            m_preSolveContactListener(contactID, oldManifold);
        }
    }
}

void WorldImpl::SetBody(BodyID id, const Body& value)
{
    if (IsLocked()) {
        throw WrongState("SetBody: world is locked");
    }
    // handle state changes that other data needs to stay in sync with
    const auto& body = GetBody(id);
    if (GetType(body) != GetType(value)) {
        // Destroy the attached contacts.
        Erase(m_bodyContacts[id.get()], [this,&body](ContactID contactID) {
            Destroy(contactID, &body);
            return true;
        });
        if (value.GetType() == BodyType::Static) {
#ifndef NDEBUG
            const auto xfm1 = GetTransform0(value.GetSweep());
            const auto xfm2 = GetTransformation(value);
            assert(xfm1 == xfm2);
#endif
            m_bodiesForProxies.push_back(id);
        }
        else {
            const auto& fixtures = m_bodyFixtures[id.get()];
            for_each(begin(fixtures), end(fixtures), [this](const auto& fixtureID) {
                AddProxies(m_fixtureProxies[UnderlyingValue(fixtureID)]);
            });
        }
    }
    if (IsEnabled(body) != IsEnabled(value)) {
        // Register for proxies so contacts created or destroyed the next time step.
        ForallFixtures(m_bodyFixtures[id.get()], [this](const auto& fixtureID) {
            m_fixturesForProxies.push_back(fixtureID);
        });
    }
    if (GetTransformation(body) != GetTransformation(value)) {
        FlagForUpdating(m_contactBuffer, m_bodyContacts[id.get()]);
        m_bodiesForProxies.push_back(id);
    }
    if (IsAwake(body) != IsAwake(value)) {
        // Update associated contacts
        if (IsAwake(value)) {
            for (const auto& elem: m_bodyContacts[id.get()]) {
                m_contactBuffer[std::get<ContactID>(elem).get()].SetIsActive();
            }
        }
        else { // sleep associated contacts whose other body is also asleep
            for (const auto& elem: m_bodyContacts[id.get()]) {
                auto& contact = m_contactBuffer[std::get<ContactID>(elem).get()];
                const auto otherID = (contact.GetBodyA() != id)
                    ? contact.GetBodyA(): contact.GetBodyB();
                if (!m_bodyBuffer[otherID.get()].IsAwake()) {
                    contact.UnsetIsActive();
                }
            }
        }
    }
    m_bodyBuffer[id.get()] = value;
}

void WorldImpl::SetContact(ContactID id, const Contact& value)
{
    m_contactBuffer.at(id.get()) = value;
}

const Body& WorldImpl::GetBody(BodyID id) const
{
    return m_bodyBuffer.at(UnderlyingValue(id));
}

const Joint& WorldImpl::GetJoint(JointID id) const
{
    return m_jointBuffer.at(UnderlyingValue(id));
}

const Contact& WorldImpl::GetContact(ContactID id) const
{
    return m_contactBuffer.at(UnderlyingValue(id));
}

const Manifold& WorldImpl::GetManifold(ContactID id) const
{
    return m_manifoldBuffer.at(UnderlyingValue(id));
}

} // namespace d2
} // namespace playrho
