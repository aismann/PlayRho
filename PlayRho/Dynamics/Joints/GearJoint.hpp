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

#ifndef PLAYRHO_DYNAMICS_JOINTS_GEARJOINT_HPP
#define PLAYRHO_DYNAMICS_JOINTS_GEARJOINT_HPP

#include <PlayRho/Dynamics/Joints/Joint.hpp>
#include <PlayRho/Dynamics/Joints/GearJointDef.hpp>
#include <PlayRho/Common/BoundedValue.hpp>

namespace playrho {

/// @brief Gear joint.
///
/// @details A gear joint is used to connect two joints together. Either joint can be
///   a revolute or prismatic joint. You specify a gear ratio to bind the motions together:
///      coordinate1 + ratio * coordinate2 = constant
///   The ratio can be negative or positive. If one joint is a revolute joint and the other
///   joint is a prismatic joint, then the ratio will have units of length or units of 1/length.
///
/// @warning You have to manually destroy the gear joint if joint1 or joint2 is destroyed.
///
/// @ingroup JointsGroup
///
/// @image html gearJoint.gif
///
class GearJoint : public Joint
{
public:
    
    /// @brief Initializing constructor.
    GearJoint(const GearJointDef& data);
    
    void Accept(JointVisitor& visitor) const override;

    Length2 GetAnchorA() const override;
    Length2 GetAnchorB() const override;

    Momentum2 GetLinearReaction() const override;
    AngularMomentum GetAngularReaction() const override;

    /// @brief Gets the local anchor point relative to bodyA's origin.
    Length2 GetLocalAnchorA() const noexcept { return m_localAnchorA; }
    
    /// @brief Gets the local anchor point relative to bodyB's origin.
    Length2 GetLocalAnchorB() const noexcept { return m_localAnchorB; }

    /// @brief Gets the first joint.
    NonNull<Joint*> GetJoint1() const noexcept { return m_joint1; }

    /// @brief Gets the second joint.
    NonNull<Joint*> GetJoint2() const noexcept { return m_joint2; }
   
    /// @brief Sets the gear ratio.
    void SetRatio(Real ratio);
    
    /// @brief Gets the ratio.
    Real GetRatio() const noexcept;

private:

    void InitVelocityConstraints(BodyConstraintsMap& bodies, const StepConf& step,
                                 const ConstraintSolverConf& conf) override;
    bool SolveVelocityConstraints(BodyConstraintsMap& bodies, const StepConf& step) override;
    bool SolvePositionConstraints(BodyConstraintsMap& bodies,
                                  const ConstraintSolverConf& conf) const override;

    NonNull<Joint*> m_joint1; ///< Joint 1.
    NonNull<Joint*> m_joint2; ///< Joint 2.

    JointType m_typeA; ///< Type of joint 1.
    JointType m_typeB; ///< Type of joint 2.

    // Body A is connected to body C
    // Body B is connected to body D
    Body* m_bodyC; ///< Body C.
    Body* m_bodyD; ///< Body D.

    // Solver shared
    Length2 m_localAnchorA; ///< Local anchor A.
    Length2 m_localAnchorB; ///< Local anchor B.
    Length2 m_localAnchorC; ///< Local anchor C.
    Length2 m_localAnchorD; ///< Local anchor D.

    UnitVec2 m_localAxisC; ///< Local axis C.
    UnitVec2 m_localAxisD; ///< Local axis D.

    Angle m_referenceAngleA; ///< Reference angle A.
    Angle m_referenceAngleB; ///< Reference angle B.

    Real m_constant; ///< Constant.
    Real m_ratio; ///< Ratio.

    Momentum m_impulse = Momentum{0}; ///< Impulse.

    // Solver temp
    Vec2 m_JvAC = Vec2{}; ///< AC Jv data.
    Vec2 m_JvBD; ///< BD Jv data.
    Length m_JwA = Length{0}; ///< A Jw data.
    Length m_JwB; ///< B Jw data.
    Length m_JwC; ///< C Jw data.
    Length m_JwD; ///< D Jw data.
    Real m_mass; ///< Either linear mass or angular mass.
};

inline Real GearJoint::GetRatio() const noexcept
{
    return m_ratio;
}
    
} // namespace playrho

#endif // PLAYRHO_DYNAMICS_JOINTS_GEARJOINT_HPP
