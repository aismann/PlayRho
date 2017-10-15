/*
* Original work Copyright (c) 2006-2009 Erin Catto http://www.box2d.org
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

#ifndef PLAYRHO_REVOLUTE_HPP
#define PLAYRHO_REVOLUTE_HPP

#include "../Framework/Test.hpp"

namespace playrho {

class Revolute : public Test
{
public:
    Revolute()
    {
        const auto ground = m_world.CreateBody();
        ground->CreateFixture(std::make_shared<EdgeShape>(Vec2(-40.0f, 0.0f) * Meter,
                                                          Vec2( 40.0f, 0.0f) * Meter));

        {
            BodyDef bd;
            bd.type = BodyType::Dynamic;

            bd.location = Vec2(-10.0f, 20.0f) * Meter;
            const auto body = m_world.CreateBody(bd);
            auto circleConf = DiskShape::Conf{};
            circleConf.vertexRadius = 0.5f * Meter;
            circleConf.density = 5 * KilogramPerSquareMeter;
            body->CreateFixture(std::make_shared<DiskShape>(circleConf));

            const auto w = 100.0f;
            body->SetVelocity(Velocity{
                Vec2(-8.0f * w, 0.0f) * MeterPerSecond, RadianPerSecond * w
            });
            
            RevoluteJointDef rjd(ground, body, Vec2(-10.0f, 12.0f) * Meter);
            rjd.motorSpeed = 1.0f * Pi * RadianPerSecond;
            rjd.maxMotorTorque = 10000.0f * NewtonMeter;
            rjd.enableMotor = false;
            rjd.lowerAngle = -0.25f * Radian * Pi;
            rjd.upperAngle = 0.5f * Radian * Pi;
            rjd.enableLimit = true;
            rjd.collideConnected = true;

            m_joint = (RevoluteJoint*)m_world.CreateJoint(rjd);
        }

        {
            BodyDef circle_bd;
            circle_bd.type = BodyType::Dynamic;
            circle_bd.location = Vec2(5.0f, 30.0f) * Meter;

            FixtureDef fd;
            fd.filter.maskBits = 1;

            m_ball = m_world.CreateBody(circle_bd);
            auto circleConf = DiskShape::Conf{};
            circleConf.vertexRadius = 3 * Meter;
            circleConf.density = 5 * KilogramPerSquareMeter;
            m_ball->CreateFixture(std::make_shared<DiskShape>(circleConf), fd);

            PolygonShape polygon_shape;
            SetAsBox(polygon_shape, 10.0f * Meter, 0.2f * Meter,
                     Vec2 (-10.0f, 0.0f) * Meter, Angle{0});
            polygon_shape.SetDensity(2 * KilogramPerSquareMeter);

            BodyDef polygon_bd;
            polygon_bd.location = Vec2(20.0f, 10.0f) * Meter;
            polygon_bd.type = BodyType::Dynamic;
            polygon_bd.bullet = true;
            const auto polygon_body = m_world.CreateBody(polygon_bd);
            polygon_body->CreateFixture(std::make_shared<PolygonShape>(polygon_shape));

            RevoluteJointDef rjd(ground, polygon_body, Vec2(20.0f, 10.0f) * Meter);
            rjd.lowerAngle = -0.25f * Radian * Pi;
            rjd.upperAngle = 0 * Radian * Pi;
            rjd.enableLimit = true;
            m_world.CreateJoint(rjd);
        }

        // Tests mass computation of a small object far from the origin
        {
            auto polyShape = PolygonShape({
                Vec2(17.63f, 36.31f) * Meter,
                Vec2(17.52f, 36.69f) * Meter,
                Vec2(17.19f, 36.36f) * Meter
            });
            polyShape.SetDensity(1 * KilogramPerSquareMeter);
        
            const auto body = m_world.CreateBody(BodyDef{}.UseType(BodyType::Dynamic));
            body->CreateFixture(std::make_shared<PolygonShape>(polyShape));
        }
    }

    void KeyboardDown(Key key) override
    {
        switch (key)
        {
        case Key_L:
            m_joint->EnableLimit(!m_joint->IsLimitEnabled());
            break;

        case Key_M:
            m_joint->EnableMotor(!m_joint->IsMotorEnabled());
            break;
                
        default:
            break;
        }
    }

    void PostStep(const Settings&, Drawer& drawer) override
    {
        drawer.DrawString(5, m_textLine, Drawer::Left, "Keys: (l) limits, (m) motor");
        m_textLine += DRAW_STRING_NEW_LINE;

        //if (GetStepCount() == 360)
        //{
        //    m_ball->SetTransform(Vec2(0.0f, 0.5f), 0.0f);
        //}

        //Real torque1 = m_joint1->GetMotorTorque();
        //drawer.DrawString(5, m_textLine, "Motor Torque = %4.0f, %4.0f : Motor Force = %4.0f", (float) torque1, (float) torque2, (float) force3);
        //m_textLine += DRAW_STRING_NEW_LINE;
    }

    Body* m_ball;
    RevoluteJoint* m_joint;
};

} // namespace playrho

#endif
