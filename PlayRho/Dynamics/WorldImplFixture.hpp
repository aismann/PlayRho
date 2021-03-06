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

#ifndef PLAYRHO_DYNAMICS_WORLDIMPLFIXTURE_HPP
#define PLAYRHO_DYNAMICS_WORLDIMPLFIXTURE_HPP

/// @file
/// Declarations of free functions of WorldImpl for fixtures.

#include <PlayRho/Common/Units.hpp>
#include <PlayRho/Common/Transformation.hpp>
#include <PlayRho/Common/Settings.hpp> // for ChildCounter

#include <PlayRho/Dynamics/BodyID.hpp>
#include <PlayRho/Dynamics/FixtureID.hpp>
#include <PlayRho/Dynamics/Filter.hpp>

#include <PlayRho/Collision/MassData.hpp>
#include <PlayRho/Collision/Shapes/Shape.hpp>

#include <vector>

namespace playrho {
namespace d2 {

class WorldImpl;
struct FixtureConf; // for CreateFixture

/// @brief Gets the extent of the currently valid fixture range.
/// @note This is one higher than the maxium <code>FixtureID</code> that is in range
///   for fixture related functions.
/// @relatedalso WorldImpl
FixtureCounter GetFixtureRange(const WorldImpl& world) noexcept;

/// @brief Creates a fixture per the given configuration.
/// @see Destroy(WorldImpl& world, FixtureID id).
/// @relatedalso WorldImpl
FixtureID CreateFixture(WorldImpl& world, const FixtureConf& def);

/// @brief Gets the identified fixture state.
/// @throws std::out_of_range If given an invalid fixture identifier.
/// @relatedalso WorldImpl
const FixtureConf& GetFixture(const WorldImpl& world, FixtureID id);

/// @brief Sets the identified fixture's state.
/// @throws std::out_of_range If given an invalid fixture identifier.
/// @relatedalso WorldImpl
void SetFixture(WorldImpl& world, FixtureID id, const FixtureConf& value);

/// @brief Destroys the identified fixture.
/// @throws std::out_of_range If given an invalid fixture identifier.
/// @see CreateFixture(WorldImpl& world, const FixtureConf& def).
/// @relatedalso WorldImpl
bool Destroy(WorldImpl& world, FixtureID id);

/// @brief Gets the dynamic tree leaves awaiting processing for the finding of new contacts.
/// @relatedalso WorldImpl
const std::vector<ContactCounter>& GetProxies(const WorldImpl& world, FixtureID id);

/// @brief Gets the count of proxies of the identified fixture.
/// @throws std::out_of_range If given an invalid fixture identifier.
/// @relatedalso WorldImpl
inline ChildCounter GetProxyCount(const WorldImpl& world, FixtureID id)
{
    return static_cast<ChildCounter>(std::size(GetProxies(world, id)));
}

/// @brief Gets the specified proxy of the identified fixture.
/// @throws std::out_of_range If given an invalid fixture identifier.
/// @relatedalso WorldImpl
ContactCounter GetProxy(const WorldImpl& world, FixtureID id, ChildCounter child);

} // namespace d2
} // namespace playrho

#endif // PLAYRHO_DYNAMICS_WORLDIMPLFIXTURE_HPP
