// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include <openvdb/Exceptions.h>
#include <openvdb/openvdb.h>

#include <gtest/gtest.h>


class TestInit: public ::testing::Test
{
};


TEST_F(TestInit, test)
{
    using namespace openvdb;

    initialize();

    // data types
    EXPECT_TRUE(DoubleMetadata::isRegisteredType());
    EXPECT_TRUE(FloatMetadata::isRegisteredType());
    EXPECT_TRUE(Int32Metadata::isRegisteredType());
    EXPECT_TRUE(Int64Metadata::isRegisteredType());
    EXPECT_TRUE(StringMetadata::isRegisteredType());
    EXPECT_TRUE(Vec2IMetadata::isRegisteredType());
    EXPECT_TRUE(Vec2SMetadata::isRegisteredType());
    EXPECT_TRUE(Vec2DMetadata::isRegisteredType());
    EXPECT_TRUE(Vec3IMetadata::isRegisteredType());
    EXPECT_TRUE(Vec3SMetadata::isRegisteredType());
    EXPECT_TRUE(Vec3DMetadata::isRegisteredType());

    // map types
    EXPECT_TRUE(math::AffineMap::isRegistered());
    EXPECT_TRUE(math::UnitaryMap::isRegistered());
    EXPECT_TRUE(math::ScaleMap::isRegistered());
    EXPECT_TRUE(math::TranslationMap::isRegistered());
    EXPECT_TRUE(math::ScaleTranslateMap::isRegistered());
    EXPECT_TRUE(math::NonlinearFrustumMap::isRegistered());

    // grid types
    EXPECT_TRUE(BoolGrid::isRegistered());
    EXPECT_TRUE(FloatGrid::isRegistered());
    EXPECT_TRUE(DoubleGrid::isRegistered());
    EXPECT_TRUE(Int32Grid::isRegistered());
    EXPECT_TRUE(Int64Grid::isRegistered());
OPENVDB_NO_DEPRECATION_WARNING_BEGIN
    EXPECT_TRUE(StringGrid::isRegistered());
OPENVDB_NO_DEPRECATION_WARNING_END
    EXPECT_TRUE(Vec3IGrid::isRegistered());
    EXPECT_TRUE(Vec3SGrid::isRegistered());
    EXPECT_TRUE(Vec3DGrid::isRegistered());

    uninitialize();

    EXPECT_TRUE(!DoubleMetadata::isRegisteredType());
    EXPECT_TRUE(!FloatMetadata::isRegisteredType());
    EXPECT_TRUE(!Int32Metadata::isRegisteredType());
    EXPECT_TRUE(!Int64Metadata::isRegisteredType());
    EXPECT_TRUE(!StringMetadata::isRegisteredType());
    EXPECT_TRUE(!Vec2IMetadata::isRegisteredType());
    EXPECT_TRUE(!Vec2SMetadata::isRegisteredType());
    EXPECT_TRUE(!Vec2DMetadata::isRegisteredType());
    EXPECT_TRUE(!Vec3IMetadata::isRegisteredType());
    EXPECT_TRUE(!Vec3SMetadata::isRegisteredType());
    EXPECT_TRUE(!Vec3DMetadata::isRegisteredType());

    EXPECT_TRUE(!math::AffineMap::isRegistered());
    EXPECT_TRUE(!math::UnitaryMap::isRegistered());
    EXPECT_TRUE(!math::ScaleMap::isRegistered());
    EXPECT_TRUE(!math::TranslationMap::isRegistered());
    EXPECT_TRUE(!math::ScaleTranslateMap::isRegistered());
    EXPECT_TRUE(!math::NonlinearFrustumMap::isRegistered());

    EXPECT_TRUE(!BoolGrid::isRegistered());
    EXPECT_TRUE(!FloatGrid::isRegistered());
    EXPECT_TRUE(!DoubleGrid::isRegistered());
    EXPECT_TRUE(!Int32Grid::isRegistered());
    EXPECT_TRUE(!Int64Grid::isRegistered());
OPENVDB_NO_DEPRECATION_WARNING_BEGIN
    EXPECT_TRUE(!StringGrid::isRegistered());
OPENVDB_NO_DEPRECATION_WARNING_END
    EXPECT_TRUE(!Vec3IGrid::isRegistered());
    EXPECT_TRUE(!Vec3SGrid::isRegistered());
    EXPECT_TRUE(!Vec3DGrid::isRegistered());
}


TEST_F(TestInit, testMatGrids)
{
    // small test to ensure matrix grid types compile
    using Mat3sGrid = openvdb::BoolGrid::ValueConverter<openvdb::Mat3s>::Type;
    using Mat3dGrid = openvdb::BoolGrid::ValueConverter<openvdb::Mat3d>::Type;
    using Mat4sGrid = openvdb::BoolGrid::ValueConverter<openvdb::Mat4s>::Type;
    using Mat4dGrid = openvdb::BoolGrid::ValueConverter<openvdb::Mat4d>::Type;
    Mat3sGrid a; (void)(a);
    Mat3dGrid b; (void)(b);
    Mat4sGrid c; (void)(c);
    Mat4dGrid d; (void)(d);
}
