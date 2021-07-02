/*
 * Copyright (c) 2020 Louis Langholtz https://github.com/louis-langholtz/PlayRho
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

#include "UnitTests.hpp"

#include <PlayRho/Collision/Shapes/Compositor.hpp>
#include <PlayRho/Collision/Shapes/Shape.hpp>

using namespace playrho;
using namespace playrho::part;

TEST(Compositor, ByteSize)
{
    EXPECT_EQ(sizeof(Compositor<GeometryIs<StaticRectangle<1, 1>>>), 1u);
    EXPECT_EQ(
        sizeof(Compositor<GeometryIs<StaticRectangle<1, 1, 2>>, DensityIs<StaticAreaDensity<6>>>),
        1u);
    EXPECT_EQ(sizeof(Compositor<GeometryIs<StaticRectangle<1, 1>>, DensityIs<StaticAreaDensity<4>>,
                                FrictionIs<StaticTenthsFriction<3>>>{}),
              1u);
    EXPECT_EQ(sizeof(Compositor<GeometryIs<StaticRectangle<1, 2>>, FrictionIs<StaticFriction<>>>),
              1u);
    switch (sizeof(Real)) {
    case 4u:
        EXPECT_EQ(sizeof(Compositor<GeometryIs<DynamicRectangle<1, 1>>>), 36u);
        EXPECT_EQ(
            sizeof(Compositor<GeometryIs<StaticRectangle<1, 1>>, DensityIs<StaticAreaDensity<4>>,
                              FrictionIs<DynamicFriction<4>>>{}),
            4u);
        EXPECT_EQ(
            sizeof(
                Compositor<GeometryIs<StaticRectangle<1, 1>>, DensityIs<StaticAreaDensity<4>>,
                           FrictionIs<StaticFriction<4>>, RestitutionIs<DynamicRestitution<>>>{}),
            4u);
        EXPECT_EQ(
            sizeof(
                Compositor<GeometryIs<StaticRectangle<1, 2>>, RestitutionIs<DynamicRestitution<>>>),
            4u);
        EXPECT_EQ(
            sizeof(Compositor<GeometryIs<StaticRectangle<1, 2>>,
                              RestitutionIs<DynamicRestitution<>>, FrictionIs<DynamicFriction<>>>),
            8u);
        EXPECT_EQ(sizeof(Compositor<GeometryIs<DynamicRectangle<1, 1>>, //
                                    DensityIs<DynamicAreaDensity<1>>, //
                                    RestitutionIs<DynamicRestitution<>>, //
                                    FrictionIs<DynamicFriction<4>>, //
                                    SensorIs<DynamicSensor<>>, //
                                    FilterIs<DynamicFilter<>> //
                                    >),
                  56u);
        break;
    case 8u:
        EXPECT_EQ(sizeof(Compositor<GeometryIs<DynamicRectangle<1, 1>>>), 72u);
        EXPECT_EQ(
            sizeof(Compositor<GeometryIs<StaticRectangle<1, 1>>, DensityIs<StaticAreaDensity<4>>,
                              FrictionIs<DynamicFriction<4>>>{}),
            8u);
        EXPECT_EQ(
            sizeof(
                Compositor<GeometryIs<StaticRectangle<1, 1>>, DensityIs<StaticAreaDensity<4>>,
                           FrictionIs<StaticFriction<4>>, RestitutionIs<DynamicRestitution<>>>{}),
            8u);
        EXPECT_EQ(
            sizeof(
                Compositor<GeometryIs<StaticRectangle<1, 2>>, RestitutionIs<DynamicRestitution<>>>),
            8u);
        EXPECT_EQ(
            sizeof(Compositor<GeometryIs<StaticRectangle<1, 2>>,
                              RestitutionIs<DynamicRestitution<>>, FrictionIs<DynamicFriction<>>>),
            16u);
        break;
    }
}

TEST(Compositor, IsValidShapeType)
{
    EXPECT_TRUE(playrho::d2::IsValidShapeType<Compositor<>>::value);
    EXPECT_TRUE(playrho::d2::IsValidShapeType<Compositor<GeometryIs<StaticRectangle<>>>>::value);
    EXPECT_TRUE(playrho::d2::IsValidShapeType<Compositor<GeometryIs<DynamicRectangle<>>>>::value);
}

TEST(Compositor, GetDimensions)
{
    EXPECT_EQ(GetDimensions(Compositor<GeometryIs<StaticRectangle<1, 1>>>{}), Length2(1_m, 1_m));
    EXPECT_EQ(GetDimensions(Compositor<GeometryIs<DynamicRectangle<1, 1>>>{}), Length2(1_m, 1_m));
    EXPECT_EQ(GetDimensions(Compositor<GeometryIs<DynamicRectangle<1, 1>>>{{2_m, 2_m}}),
              Length2(2_m, 2_m));
    {
        auto rect = Compositor<GeometryIs<DynamicRectangle<0, 0>>>{};
        EXPECT_EQ(GetDimensions(rect), Length2(0_m, 0_m));
        const auto value = Length2{4_m, 8_m};
        EXPECT_NO_THROW(SetDimensions(rect, value));
        EXPECT_EQ(GetDimensions(rect), value);
    }
}

TEST(Compositor, SetDimensions)
{
    {
        auto o = Compositor<GeometryIs<StaticRectangle<1, 1>>>{};
        EXPECT_NO_THROW(SetDimensions(o, Length2(1_m, 1_m)));
        EXPECT_EQ(GetDimensions(o), Length2(1_m, 1_m));
        EXPECT_THROW(SetDimensions(o, Length2(2_m, 3_m)), InvalidArgument);
        EXPECT_EQ(GetDimensions(o), Length2(1_m, 1_m));
    }
    {
        auto o = Compositor<GeometryIs<DynamicRectangle<1, 1>>>{};
        EXPECT_NO_THROW(SetDimensions(o, Length2(1_m, 1_m)));
        EXPECT_EQ(GetDimensions(o), Length2(1_m, 1_m));
        EXPECT_NO_THROW(SetDimensions(o, Length2(2_m, 3_m)));
        EXPECT_EQ(GetDimensions(o), Length2(2_m, 3_m));
    }
}

TEST(Compositor, GetOffset)
{
    EXPECT_EQ(GetOffset(Compositor<GeometryIs<StaticRectangle<1, 1>>>{}), Length2(0_m, 0_m));
    EXPECT_EQ(GetOffset(Compositor<GeometryIs<DynamicRectangle<1, 1>>>{}), Length2(0_m, 0_m));
    EXPECT_EQ(GetOffset(Compositor<GeometryIs<DynamicRectangle<1, 1>>>{{2_m, 2_m}}),
              Length2(0_m, 0_m));
    {
        auto rect = Compositor<GeometryIs<DynamicRectangle<1, 1>>>{{4_m, 2_m}};
        EXPECT_EQ(GetOffset(rect), Length2(0_m, 0_m));
        const auto value = Length2{4_m, 8_m};
        EXPECT_NO_THROW(SetOffset(rect, value));
        EXPECT_EQ(GetOffset(rect), value);
    }
}

TEST(Compositor, GetChildCount)
{
    EXPECT_EQ(GetChildCount(Compositor<GeometryIs<StaticRectangle<1, 1>>>{}), 1u);
    EXPECT_EQ(GetChildCount(Compositor<GeometryIs<DynamicRectangle<>>>{}), 1u);
    EXPECT_EQ(GetChildCount(Compositor<GeometryIs<StaticRectangle<1, 1>>, StaticAreaDensity<6>>{}),
              1u);
    EXPECT_EQ(GetChildCount(Compositor<GeometryIs<DynamicRectangle<0, 0>>, StaticAreaDensity<6>>{}),
              1u);
}

TEST(Compositor, GetDensity)
{
    EXPECT_EQ(GetDensity(
                  Compositor<GeometryIs<StaticRectangle<1, 1>>, DensityIs<StaticAreaDensity<4>>>{}),
              4_kgpm2);
    EXPECT_EQ(
        GetDensity(
            Compositor<GeometryIs<DynamicRectangle<1, 1>>, DensityIs<StaticAreaDensity<4>>>{}),
        4_kgpm2);
    EXPECT_EQ(GetDensity(
                  Compositor<GeometryIs<StaticRectangle<1, 1>>, DensityIs<StaticAreaDensity<5>>>{}),
              5_kgpm2);
    EXPECT_EQ(
        GetDensity(
            Compositor<GeometryIs<StaticRectangle<1, 1>>, DensityIs<DynamicAreaDensity<6>>>{}),
        6_kgpm2);
    EXPECT_EQ(
        GetDensity(Compositor<GeometryIs<StaticRectangle<1, 1>>, DensityIs<DynamicAreaDensity<6>>>{
            {}, {2.4_kgpm2}}),
        2.4_kgpm2);
}

TEST(Compositor, GetFriction)
{
    EXPECT_EQ(GetFriction(Compositor<GeometryIs<StaticRectangle<1, 1>>>{}), Real(2) / Real(10));
    EXPECT_EQ(
        GetFriction(
            Compositor<GeometryIs<StaticRectangle<1, 1>>, FrictionIs<StaticTenthsFriction<>>>{}),
        Real(2) / Real(10));
    EXPECT_EQ(
        GetFriction(
            Compositor<GeometryIs<StaticRectangle<1, 1>>, FrictionIs<StaticTenthsFriction<3>>>{}),
        Real(3) / Real(10));
    EXPECT_EQ(GetFriction(
                  Compositor<GeometryIs<StaticRectangle<1, 1>>, FrictionIs<DynamicFriction<4>>>{}),
              Real(4));
    EXPECT_EQ(
        GetFriction(Compositor<GeometryIs<StaticRectangle<1, 1>>, FrictionIs<DynamicFriction<4>>>{
            {}, {}, {Real(0.5)}}),
        Real(0.5));
}

TEST(Compositor, GetRestitution)
{
    EXPECT_EQ(GetRestitution(Compositor<GeometryIs<StaticRectangle<1, 1>>>{}), Real(0));
    EXPECT_EQ(
        GetRestitution(
            Compositor<GeometryIs<StaticRectangle<1, 1>>, RestitutionIs<StaticRestitution<1>>>{}),
        Real(1));
    EXPECT_EQ(
        GetRestitution(
            Compositor<GeometryIs<StaticRectangle<1, 1>>, RestitutionIs<DynamicRestitution<8>>>{}),
        Real(8));
    EXPECT_EQ(
        GetRestitution(
            Compositor<GeometryIs<StaticRectangle<1, 1>>, RestitutionIs<DynamicRestitution<8>>>{
                {}, {}, {}, {Real(1.2)}}),
        Real(1.2));
}

TEST(Compositor, SetFriction)
{
    using ::playrho::d2::SetFriction;
    {
        auto rectangle = Compositor<GeometryIs<StaticRectangle<1, 1>>>{};
        ASSERT_EQ(rectangle.friction, Real(2) / Real(10));
        EXPECT_THROW(SetFriction(rectangle, Real(3)), InvalidArgument);
        {
            auto shape = ::playrho::d2::Shape{rectangle};
            EXPECT_THROW(SetFriction(shape, Real(3)), InvalidArgument);
        }
        EXPECT_EQ(rectangle.friction, Real(2) / Real(10));
    }
    {
        auto rectangle =
            Compositor<GeometryIs<StaticRectangle<1, 1>>, FrictionIs<DynamicFriction<>>>{};
        ASSERT_EQ(rectangle.friction, Real(0));
        EXPECT_NO_THROW(SetFriction(rectangle, Real(3)));
        EXPECT_EQ(rectangle.friction, Real(3));
    }
}

TEST(Compositor, SetRestitution)
{
    using ::playrho::d2::SetRestitution;
    {
        auto rectangle = Compositor<GeometryIs<StaticRectangle<1, 1>>>{};
        ASSERT_EQ(rectangle.restitution, Real(0));
        EXPECT_THROW(SetRestitution(rectangle, Real(3)), InvalidArgument);
        EXPECT_EQ(rectangle.restitution, Real(0));
    }
    {
        auto rectangle =
            Compositor<GeometryIs<StaticRectangle<1, 1>>, RestitutionIs<DynamicRestitution<>>>{};
        ASSERT_EQ(rectangle.restitution, Real(0));
        EXPECT_NO_THROW(SetRestitution(rectangle, Real(3)));
        EXPECT_EQ(rectangle.restitution, Real(3));
    }
}
