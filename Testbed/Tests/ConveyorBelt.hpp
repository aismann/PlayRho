/*
* Original work Copyright (c) 2011 Erin Catto http://www.box2d.org
* Modified work Copyright (c) 2017 Louis Langholtz https://github.com/louis-langholtz/PlayRho
*
* This software is provided 'as-is', without any express or implied
* warranty.  In no event will the authors be held liable for any damages
* arising from the use of this software.
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 3. This notice may not be removed or altered from any source distribution.
*/

#ifndef PLAYRHO_CONVEYOR_BELT_HPP
#define  PLAYRHO_CONVEYOR_BELT_HPP

#include "../Framework/Test.hpp"

namespace testbed {

class ConveyorBelt : public Test
{
public:

    ConveyorBelt()
    {
        // Ground
        m_world.CreateFixture(m_world.CreateBody(), Shape{
            EdgeShapeConf{Vec2(-20.0f, 0.0f) * 1_m, Vec2(20.0f, 0.0f) * 1_m}});

        // Platform
        {
            BodyConf bd;
            bd.location = Vec2(-5.0f, 5.0f) * 1_m;
            const auto body = m_world.CreateBody(bd);

            auto conf = PolygonShapeConf{};
            conf.friction = 0.8f;
            conf.SetAsBox(10_m, 0.5_m);
            m_platform = m_world.CreateFixture(body, Shape{conf});
        }

        // Boxes
        const auto boxshape = Shape{PolygonShapeConf{}.UseDensity(20_kgpm2).SetAsBox(0.5_m, 0.5_m)};
        for (auto i = 0; i < 5; ++i)
        {
            BodyConf bd;
            bd.type = BodyType::Dynamic;
            bd.linearAcceleration = m_gravity;
            bd.location = Vec2(-10.0f + 2.0f * i, 7.0f) * 1_m;
            const auto body = m_world.CreateBody(bd);
            m_world.CreateFixture(body, boxshape);
        }
    }

    void PreSolve(ContactID contact, const Manifold& oldManifold) override
    {
        Test::PreSolve(contact, oldManifold);

        const auto fixtureA = GetFixtureA(m_world, contact);
        const auto fixtureB = GetFixtureB(m_world, contact);
        if (fixtureA == m_platform)
        {
            SetTangentSpeed(m_world, contact, 5_mps);
        }
        if (fixtureB == m_platform)
        {
            SetTangentSpeed(m_world, contact, -5_mps);
        }
    }

    FixtureID m_platform;
};

} // namespace testbed

#endif
