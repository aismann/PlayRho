/*
 * Original work Copyright (c) 2006-2011 Erin Catto http://www.box2d.org
 * Modified work Copyright (c) 2017 Louis Langholtz https://github.com/louis-langholtz/PlayRho
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

#ifndef PLAYRHO_DYNAMICS_BODYATTY_HPP
#define PLAYRHO_DYNAMICS_BODYATTY_HPP

/// @file
/// Declaration of the BodyAtty class.

#include <PlayRho/Dynamics/Body.hpp>
#include <PlayRho/Dynamics/Fixture.hpp>
#include <PlayRho/Dynamics/Joints/JointKey.hpp>
#include <PlayRho/Dynamics/Contacts/ContactKey.hpp>

#include <algorithm>
#include <utility>

namespace playrho {

/// @brief Body attorney.
///
/// @details This class uses the "attorney-client" idiom to control the granularity of
///   friend-based access to the Body class. This is meant to help preserve and enforce
///   the invariants of the Body class.
///
/// @sa https://en.wikibooks.org/wiki/More_C++_Idioms/Friendship_and_the_Attorney-Client
///
class BodyAtty
{
private:
    
    /// @brief Creates a fixture.
    static Fixture* CreateFixture(Body& b, Shape shape, const FixtureDef& def)
    {
        const auto fixture = new Fixture{&b, def, std::move(shape)};
        b.m_fixtures.push_back(fixture);
        return fixture;
    }
    
    /// @brief Destroys the given fixture.
    static bool DestroyFixture(Body& b, Fixture* value)
    {
        const auto endIter = end(b.m_fixtures);
        const auto it = std::find_if(begin(b.m_fixtures), endIter, [&](Body::Fixtures::value_type& f) {
            return GetPtr(f) == value;
        });
        if (it != endIter)
        {
            delete GetPtr(*it);
            b.m_fixtures.erase(it);
            return true;
        }
        return false;
    }

    /// @brief Clears the fixtures of the given body.
    static void ClearFixtures(Body& b, std::function<void(Fixture&)> callback)
    {
        std::for_each(std::begin(b.m_fixtures), std::end(b.m_fixtures), [&](Body::Fixtures::value_type& f) {
            const auto fixture = GetPtr(f);
            callback(*fixture);
            delete fixture;
        });
        b.m_fixtures.clear();
    }

    /// @brief Sets the type flags for the given body.
    static void SetTypeFlags(Body& b, BodyType type) noexcept
    {
        b.m_flags &= ~(Body::e_impenetrableFlag|Body::e_velocityFlag|Body::e_accelerationFlag);
        b.m_flags |= Body::GetFlags(type);
        
        switch (type)
        {
            case BodyType::Dynamic:
                break;
            case BodyType::Kinematic:
                break;
            case BodyType::Static:
                b.UnsetAwakeFlag();
                b.m_underActiveTime = 0;
                b.m_velocity = Velocity{LinearVelocity2{}, 0_rpm};
                b.m_sweep.pos0 = b.m_sweep.pos1;
                break;
        }
    }
    
    /// @brief Sets the awake flag for the given body.
    static void SetAwakeFlag(Body& b) noexcept
    {
        b.SetAwakeFlag();
    }
    
    /// @brief Sets the mass data dity flag for the given body.
    static void SetMassDataDirty(Body& b) noexcept
    {
        b.SetMassDataDirty();
    }
    
    /// @brief Erases the given contact from the given body.
    static bool Erase(Body& b, const Contact* value)
    {
        return b.Erase(value);
    }
    
    /// @brief Erases the given joint from the given body.
    static bool Erase(Body& b, const Joint* value)
    {
        return b.Erase(value);
    }
    
    /// @brief Clears the contacts from the given body.
    static void ClearContacts(Body &b)
    {
        b.ClearContacts();
    }

    /// @brief Clears the joints from the given body.
    static void ClearJoints(Body &b)
    {
        b.ClearJoints();
    }

    /// @brief Inserts the given joint into the given body's joint list.
    static bool Insert(Body& b, Joint* value)
    {
        return b.Insert(value);
    }

    /// @brief Inserts the given joint into the given body's joint list.
    static bool Insert(Body* b, Joint* value)
    {
        if (b != nullptr)
        {
            return Insert(*b, value);
        }
        return false;
    }

    /// @brief Inserts the given contact key and contact into the given body's contacts list.
    static bool Insert(Body& b, ContactKey key, Contact* value)
    {
        return b.Insert(key, value);
    }
    
    /// @brief Sets the position0 value of the given body to the given position.
    static void SetPosition0(Body& b, const Position value) noexcept
    {
        assert(b.IsSpeedable() || b.m_sweep.pos0 == value);
        b.m_sweep.pos0 = value;
    }
    
    /// Sets the body sweep's position 1 value.
    /// @note This sets what Body::GetWorldCenter returns.
    static void SetPosition1(Body& b, const Position value) noexcept
    {
        assert(b.IsSpeedable() || b.m_sweep.pos1 == value);
        b.m_sweep.pos1 = value;
    }
    
    /// @brief Resets the given body's alpha0 value.
    static void ResetAlpha0(Body& b)
    {
        b.m_sweep.ResetAlpha0();
    }
    
    /// @brief Sets the sweep value of the given body.
    static void SetSweep(Body& b, const Sweep value) noexcept
    {
        assert(b.IsSpeedable() || value.pos0 == value.pos1);
        b.m_sweep = value;
    }
    
    /// Sets the body's transformation.
    /// @note This sets what Body::GetLocation returns.
    static void SetTransformation(Body& b, const Transformation value) noexcept
    {
        b.SetTransformation(value);
    }
    
    /// Sets the body's velocity.
    /// @note This sets what Body::GetVelocity returns.
    static void SetVelocity(Body& b, Velocity value) noexcept
    {
        b.m_velocity = value;
    }
    
    /// @brief Calls the given body sweep's Advance0 method to advance to the given value.
    static void Advance0(Body& b, Real value) noexcept
    {
        // Note: Static bodies must **never** have different sweep position values.
        
        // Confirm bodies don't have different sweep positions to begin with...
        assert(b.IsSpeedable() || b.m_sweep.pos1 == b.m_sweep.pos0);
        
        b.m_sweep.Advance0(value);
        
        // Confirm bodies don't have different sweep positions to end with...
        assert(b.IsSpeedable() || b.m_sweep.pos1 == b.m_sweep.pos0);
    }
    
    /// @brief Calls the given body's Advance method to advance to the given TOI.
    static void Advance(Body& b, Real toi) noexcept
    {
        b.Advance(toi);
    }
    
    /// @brief Restores the given body's sweep to the given sweep value.
    static void Restore(Body& b, const Sweep value) noexcept
    {
        BodyAtty::SetSweep(b, value);
        BodyAtty::SetTransformation(b, GetTransform1(value));
    }
    
    /// @brief Clears the given body's joints list.
    static void ClearJoints(Body& b, std::function<void(Joint&)> callback)
    {
        auto joints = std::move(b.m_joints);
        assert(b.m_joints.empty());
        std::for_each(cbegin(joints), cend(joints), [&](Body::KeyedJointPtr j) {
            callback(*(j.second));
        });
    }
    
    /// @brief Erases the given body's contacts.
    static void EraseContacts(Body& b, const std::function<bool(Contact&)>& callback)
    {
        auto end = b.m_contacts.end();
        auto iter = b.m_contacts.begin();
        auto index = Body::Contacts::difference_type{0};
        while (iter != end)
        {
            const auto contact = GetContactPtr(*iter);
            if (callback(*contact))
            {
                b.m_contacts.erase(iter);
                iter = b.m_contacts.begin() + index;
                end = b.m_contacts.end();
            }
            else
            {
                iter = std::next(iter);
                ++index;
            }
        }
    }
    
    /// @brief Whether the given body is in the is islanded state.
    static bool IsIslanded(const Body& b) noexcept
    {
        return b.IsIslanded();
    }
    
    /// @brief Sets the given body to the is islanded state.
    static void SetIslanded(Body& b) noexcept
    {
        b.SetIslandedFlag();
    }
    
    /// @brief Unsets the given body's is islanded state.
    static void UnsetIslanded(Body& b) noexcept
    {
        b.UnsetIslandedFlag();
    }
    
    friend class World;
};

} // namespace playrho

#endif // PLAYRHO_DYNAMICS_BODYATTY_HPP
