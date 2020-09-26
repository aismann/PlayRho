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

#include <PlayRho/Dynamics/WorldImplContact.hpp>

#include <PlayRho/Dynamics/WorldImpl.hpp>
#include <PlayRho/Dynamics/Body.hpp>
#include <PlayRho/Dynamics/BodyConf.hpp>
#include <PlayRho/Dynamics/StepConf.hpp>
#include <PlayRho/Dynamics/Fixture.hpp> // for GetDefaultFriction, GetDefaultRestitution
#include <PlayRho/Dynamics/FixtureProxy.hpp>

#include <PlayRho/Dynamics/Joints/Joint.hpp>
#include <PlayRho/Dynamics/Joints/JointVisitor.hpp>
#include <PlayRho/Dynamics/Joints/FunctionalJointVisitor.hpp>
#include <PlayRho/Dynamics/Joints/RevoluteJoint.hpp>
#include <PlayRho/Dynamics/Joints/PrismaticJoint.hpp>
#include <PlayRho/Dynamics/Joints/DistanceJoint.hpp>
#include <PlayRho/Dynamics/Joints/PulleyJoint.hpp>
#include <PlayRho/Dynamics/Joints/TargetJoint.hpp>
#include <PlayRho/Dynamics/Joints/GearJoint.hpp>
#include <PlayRho/Dynamics/Joints/WheelJoint.hpp>
#include <PlayRho/Dynamics/Joints/WeldJoint.hpp>
#include <PlayRho/Dynamics/Joints/FrictionJoint.hpp>
#include <PlayRho/Dynamics/Joints/RopeJoint.hpp>
#include <PlayRho/Dynamics/Joints/MotorJoint.hpp>

#include <PlayRho/Dynamics/Contacts/Contact.hpp>

#include <PlayRho/Common/DynamicMemory.hpp>

#include <algorithm>
#include <new>
#include <functional>
#include <type_traits>
#include <memory>
#include <vector>

namespace playrho {
namespace d2 {

bool IsAwake(const WorldImpl& world, ContactID id)
{
    return world.GetContact(id).IsActive();
}

void SetAwake(WorldImpl& world, ContactID id)
{
    const auto& contact = world.GetContact(id);
    world.GetBody(contact.GetBodyA()).SetAwake();
    world.GetBody(contact.GetBodyB()).SetAwake();
}

Real GetFriction(const WorldImpl& world, ContactID id)
{
    return world.GetContact(id).GetFriction();
}

Real GetRestitution(const WorldImpl& world, ContactID id)
{
    return world.GetContact(id).GetRestitution();
}

void SetFriction(WorldImpl& world, ContactID id, Real value)
{
    world.GetContact(id).SetFriction(value);
}

void SetRestitution(WorldImpl& world, ContactID id, Real value)
{
    world.GetContact(id).SetRestitution(value);
}

const Manifold& GetManifold(const WorldImpl& world, ContactID id)
{
    return world.GetContact(id).GetManifold();
}

Real GetDefaultFriction(const WorldImpl& world, ContactID id)
{
    const auto& contact = world.GetContact(id);
    const auto& fixtureA = world.GetFixture(contact.GetFixtureA());
    const auto& fixtureB = world.GetFixture(contact.GetFixtureB());
    return GetDefaultFriction(fixtureA, fixtureB);
}

Real GetDefaultRestitution(const WorldImpl& world, ContactID id)
{
    const auto& contact = world.GetContact(id);
    const auto& fixtureA = world.GetFixture(contact.GetFixtureA());
    const auto& fixtureB = world.GetFixture(contact.GetFixtureB());
    return GetDefaultRestitution(fixtureA, fixtureB);
}

bool IsTouching(const WorldImpl& world, ContactID id)
{
    return world.GetContact(id).IsTouching();
}

bool NeedsFiltering(const WorldImpl& world, ContactID id)
{
    return world.GetContact(id).NeedsFiltering();
}

bool NeedsUpdating(const WorldImpl& world, ContactID id)
{
    return world.GetContact(id).NeedsUpdating();
}

FixtureID GetFixtureA(const WorldImpl& world, ContactID id)
{
    return world.GetContact(id).GetFixtureA();
}

FixtureID GetFixtureB(const WorldImpl& world, ContactID id)
{
    return world.GetContact(id).GetFixtureB();
}

} // namespace d2
} // namespace playrho
