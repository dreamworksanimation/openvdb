// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include <openvdb/openvdb.h>
#include <openvdb/points/PointScatter.h>
#include <openvdb/points/PointCount.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/math/Math.h>
#include <openvdb/math/Coord.h>

#include <gtest/gtest.h>

#include <random>


using namespace openvdb;
using namespace openvdb::points;

class TestPointScatter: public ::testing::Test
{
public:
    void SetUp() override { openvdb::initialize(); }
    void TearDown() override { openvdb::uninitialize(); }
}; // class TestPointScatter


TEST_F(TestPointScatter, testUniformPointScatter)
{
    const Index64 total = 50;
    const math::CoordBBox boxBounds(math::Coord(-1), math::Coord(1)); // 27 voxels across 8 leaves

    // Test the free function for all default grid types - 50 points across 27 voxels
    // ensures all voxels receive points

    {
        BoolGrid grid;
        grid.sparseFill(boxBounds, false, /*active*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }
    {
        DoubleGrid grid;
        grid.sparseFill(boxBounds, 0.0, /*active*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }
    {
        FloatGrid grid;
        grid.sparseFill(boxBounds, 0.0f, /*active*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }
    {
        Int32Grid grid;
        grid.sparseFill(boxBounds, 0, /*active*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }
    {
        Int64Grid grid;
        grid.sparseFill(boxBounds, 0, /*active*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }
    {
        MaskGrid grid;
        grid.sparseFill(boxBounds, /*maskBuffer*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }
    {
        Vec3DGrid grid;
        grid.sparseFill(boxBounds, Vec3d(), /*active*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }
    {
        Vec3IGrid grid;
        grid.sparseFill(boxBounds, Vec3i(), /*active*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }
    {
        Vec3SGrid grid;
        grid.sparseFill(boxBounds, Vec3f(), /*active*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }
    {
        PointDataGrid grid;
        grid.sparseFill(boxBounds, 0, /*active*/true);
        auto points = points::uniformPointScatter(grid, total);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(total, pointCount(points->tree()));
    }

    // Test 0 produces empty grid

    {
        BoolGrid grid;
        grid.sparseFill(boxBounds, false, /*active*/true);
        auto points = points::uniformPointScatter(grid, 0);
        EXPECT_TRUE(points->empty());
    }

    // Test single point scatter and topology

    {
        BoolGrid grid;
        grid.sparseFill(boxBounds, false, /*active*/true);
        auto points = points::uniformPointScatter(grid, 1);
        EXPECT_EQ(Index32(1), points->tree().leafCount());
        EXPECT_EQ(Index64(1), points->activeVoxelCount());
        EXPECT_EQ(Index64(1), pointCount(points->tree()));
    }

    // Test a grid containing tiles scatters correctly

    BoolGrid grid;
    grid.tree().addTile(/*level*/1, math::Coord(0), /*value*/true, /*active*/true);

    const Index32 NUM_VALUES = BoolGrid::TreeType::LeafNodeType::NUM_VALUES;

    EXPECT_EQ(Index64(NUM_VALUES), grid.activeVoxelCount());

    auto points = points::uniformPointScatter(grid, total);

    EXPECT_EQ(Index64(0), points->tree().activeTileCount());
    EXPECT_EQ(Index32(1), points->tree().leafCount());
    EXPECT_TRUE(Index64(NUM_VALUES) > points->tree().activeVoxelCount());
    EXPECT_EQ(total, pointCount(points->tree()));

    // Explicitly check P attribute

    const auto* attributeSet = &(points->tree().cbeginLeaf()->attributeSet());
    EXPECT_EQ(size_t(1), attributeSet->size());
    const auto* array = attributeSet->getConst(0);
    EXPECT_TRUE(array);

    using PositionArrayT = TypedAttributeArray<Vec3f, NullCodec>;
    EXPECT_TRUE(array->isType<PositionArrayT>());

    size_t size = array->size();
    EXPECT_EQ(size_t(total), size);

    AttributeHandle<Vec3f, NullCodec>::Ptr pHandle =
        AttributeHandle<Vec3f, NullCodec>::create(*array);
    for (size_t i = 0; i < size; ++i) {
        const Vec3f P = pHandle->get(Index(i));
        EXPECT_TRUE(P[0] >=-0.5f);
        EXPECT_TRUE(P[0] <= 0.5f);
        EXPECT_TRUE(P[1] >=-0.5f);
        EXPECT_TRUE(P[1] <= 0.5f);
        EXPECT_TRUE(P[2] >=-0.5f);
        EXPECT_TRUE(P[2] <= 0.5f);
    }

    // Test the rng seed

    const Vec3f firstPosition = pHandle->get(0);
    points = points::uniformPointScatter(grid, total, /*seed*/1);

    attributeSet = &(points->tree().cbeginLeaf()->attributeSet());
    EXPECT_EQ(size_t(1), attributeSet->size());

    array = attributeSet->getConst(0);
    EXPECT_TRUE(array);
    EXPECT_TRUE(array->isType<PositionArrayT>());

    size = array->size();
    EXPECT_EQ(size_t(total), size);
    pHandle = AttributeHandle<Vec3f, NullCodec>::create(*array);

    const Vec3f secondPosition = pHandle->get(0);
    EXPECT_TRUE(!math::isExactlyEqual(firstPosition[0], secondPosition[0]));
    EXPECT_TRUE(!math::isExactlyEqual(firstPosition[1], secondPosition[1]));
    EXPECT_TRUE(!math::isExactlyEqual(firstPosition[2], secondPosition[2]));

    // Test spread

    points = points::uniformPointScatter(grid, total, /*seed*/1, /*spread*/0.2f);

    attributeSet = &(points->tree().cbeginLeaf()->attributeSet());
    EXPECT_EQ(size_t(1), attributeSet->size());
    array = attributeSet->getConst(0);
    EXPECT_TRUE(array);
    EXPECT_TRUE(array->isType<PositionArrayT>());

    size = array->size();
    EXPECT_EQ(size_t(total), size);

    pHandle = AttributeHandle<Vec3f, NullCodec>::create(*array);
    for (size_t i = 0; i < size; ++i) {
        const Vec3f P = pHandle->get(Index(i));
        EXPECT_TRUE(P[0] >=-0.2f);
        EXPECT_TRUE(P[0] <= 0.2f);
        EXPECT_TRUE(P[1] >=-0.2f);
        EXPECT_TRUE(P[1] <= 0.2f);
        EXPECT_TRUE(P[2] >=-0.2f);
        EXPECT_TRUE(P[2] <= 0.2f);
    }

    // Test mt11213b

    using mt11213b = std::mersenne_twister_engine<uint32_t, 32, 351, 175, 19,
        0xccab8ee7, 11, 0xffffffff, 7, 0x31b6ab00, 15, 0xffe50000, 17, 1812433253>;

    points = points::uniformPointScatter<BoolGrid, mt11213b>(grid, total);

    EXPECT_EQ(Index32(1), points->tree().leafCount());
    EXPECT_TRUE(Index64(NUM_VALUES) > points->tree().activeVoxelCount());
    EXPECT_EQ(total, pointCount(points->tree()));

    // Test no remainder - grid contains one tile, scatter NUM_VALUES points

    points = points::uniformPointScatter(grid, Index64(NUM_VALUES));

    EXPECT_EQ(Index32(1), points->tree().leafCount());
    EXPECT_EQ(Index64(NUM_VALUES), points->activeVoxelCount());
    EXPECT_EQ(Index64(NUM_VALUES), pointCount(points->tree()));

    const auto* const leaf = points->tree().probeConstLeaf(math::Coord(0));
    EXPECT_TRUE(leaf);
    EXPECT_TRUE(leaf->isDense());

    const auto* const data = leaf->buffer().data();
    EXPECT_EQ(Index32(1), Index32(data[1] - data[0]));

    for (size_t i = 1; i < NUM_VALUES; ++i) {
        const Index32 offset = data[i] - data[i - 1];
        EXPECT_EQ(Index32(1), offset);
    }
}

TEST_F(TestPointScatter, testDenseUniformPointScatter)
{
    const Index32 pointsPerVoxel = 8;
    const math::CoordBBox boxBounds(math::Coord(-1), math::Coord(1)); // 27 voxels across 8 leaves

    // Test the free function for all default grid types

    {
        BoolGrid grid;
        grid.sparseFill(boxBounds, false, /*active*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        DoubleGrid grid;
        grid.sparseFill(boxBounds, 0.0, /*active*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        FloatGrid grid;
        grid.sparseFill(boxBounds, 0.0f, /*active*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        Int32Grid grid;
        grid.sparseFill(boxBounds, 0, /*active*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        Int64Grid grid;
        grid.sparseFill(boxBounds, 0, /*active*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        MaskGrid grid;
        grid.sparseFill(boxBounds, /*maskBuffer*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        Vec3DGrid grid;
        grid.sparseFill(boxBounds, Vec3d(), /*active*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        Vec3IGrid grid;
        grid.sparseFill(boxBounds, Vec3i(), /*active*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        Vec3SGrid grid;
        grid.sparseFill(boxBounds, Vec3f(), /*active*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        PointDataGrid grid;
        grid.sparseFill(boxBounds, 0, /*active*/true);
        auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }

    // Test 0 produces empty grid

    {
        BoolGrid grid;
        grid.sparseFill(boxBounds, false, /*active*/true);
        auto points = points::denseUniformPointScatter(grid, 0.0f);
        EXPECT_TRUE(points->empty());
    }

    // Test topology between 0 - 1

    {
        BoolGrid grid;
        grid.sparseFill(boxBounds, false, /*active*/true);
        auto points = points::denseUniformPointScatter(grid, 0.8f);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        // Note that a value of 22 is precomputed as the number of active
        // voxels/points produced by a value of 0.8
        EXPECT_EQ(Index64(22), points->activeVoxelCount());
        EXPECT_EQ(Index64(22), pointCount(points->tree()));

        // Test below 0 throws

        EXPECT_THROW(points::denseUniformPointScatter(grid, -0.1f), openvdb::ValueError);
    }

    // Test a grid containing tiles scatters correctly

    BoolGrid grid;
    grid.tree().addTile(/*level*/1, math::Coord(0), /*value*/true, /*active*/true);
    grid.tree().setValueOn(math::Coord(8,0,0)); // add another leaf

    const Index32 NUM_VALUES = BoolGrid::TreeType::LeafNodeType::NUM_VALUES;

    EXPECT_EQ(Index32(1), grid.tree().leafCount());
    EXPECT_EQ(Index64(NUM_VALUES + 1), grid.activeVoxelCount());

    auto points = points::denseUniformPointScatter(grid, pointsPerVoxel);

    const Index64 expectedCount = Index64(pointsPerVoxel * (NUM_VALUES + 1));

    EXPECT_EQ(Index64(0), points->tree().activeTileCount());
    EXPECT_EQ(Index32(2), points->tree().leafCount());
    EXPECT_EQ(Index64(NUM_VALUES + 1), points->activeVoxelCount());
    EXPECT_EQ(expectedCount, pointCount(points->tree()));

    // Explicitly check P attribute

    const auto* attributeSet = &(points->tree().cbeginLeaf()->attributeSet());
    EXPECT_EQ(size_t(1), attributeSet->size());
    const auto* array = attributeSet->getConst(0);
    EXPECT_TRUE(array);

    using PositionArrayT = TypedAttributeArray<Vec3f, NullCodec>;
    EXPECT_TRUE(array->isType<PositionArrayT>());

    size_t size = array->size();
    EXPECT_EQ(size_t(pointsPerVoxel * NUM_VALUES), size);

    AttributeHandle<Vec3f, NullCodec>::Ptr pHandle =
        AttributeHandle<Vec3f, NullCodec>::create(*array);
    for (size_t i = 0; i < size; ++i) {
        const Vec3f P = pHandle->get(Index(i));
        EXPECT_TRUE(P[0] >=-0.5f);
        EXPECT_TRUE(P[0] <= 0.5f);
        EXPECT_TRUE(P[1] >=-0.5f);
        EXPECT_TRUE(P[1] <= 0.5f);
        EXPECT_TRUE(P[2] >=-0.5f);
        EXPECT_TRUE(P[2] <= 0.5f);
    }

    // Test the rng seed

    const Vec3f firstPosition = pHandle->get(0);
    points = points::denseUniformPointScatter(grid, pointsPerVoxel, /*seed*/1);

    attributeSet = &(points->tree().cbeginLeaf()->attributeSet());
    EXPECT_EQ(size_t(1), attributeSet->size());

    array = attributeSet->getConst(0);
    EXPECT_TRUE(array);
    EXPECT_TRUE(array->isType<PositionArrayT>());

    size = array->size();
    EXPECT_EQ(size_t(pointsPerVoxel * NUM_VALUES), size);
    pHandle = AttributeHandle<Vec3f, NullCodec>::create(*array);

    const Vec3f secondPosition = pHandle->get(0);
    EXPECT_TRUE(!math::isExactlyEqual(firstPosition[0], secondPosition[0]));
    EXPECT_TRUE(!math::isExactlyEqual(firstPosition[1], secondPosition[1]));
    EXPECT_TRUE(!math::isExactlyEqual(firstPosition[2], secondPosition[2]));

    // Test spread

    points = points::denseUniformPointScatter(grid, pointsPerVoxel, /*seed*/1, /*spread*/0.2f);

    attributeSet = &(points->tree().cbeginLeaf()->attributeSet());
    EXPECT_EQ(size_t(1), attributeSet->size());
    array = attributeSet->getConst(0);
    EXPECT_TRUE(array);
    EXPECT_TRUE(array->isType<PositionArrayT>());

    size = array->size();
    EXPECT_EQ(size_t(pointsPerVoxel * NUM_VALUES), size);

    pHandle = AttributeHandle<Vec3f, NullCodec>::create(*array);
    for (size_t i = 0; i < size; ++i) {
        const Vec3f P = pHandle->get(Index(i));
        EXPECT_TRUE(P[0] >=-0.2f);
        EXPECT_TRUE(P[0] <= 0.2f);
        EXPECT_TRUE(P[1] >=-0.2f);
        EXPECT_TRUE(P[1] <= 0.2f);
        EXPECT_TRUE(P[2] >=-0.2f);
        EXPECT_TRUE(P[2] <= 0.2f);
    }

    // Test mt11213b

    using mt11213b = std::mersenne_twister_engine<uint32_t, 32, 351, 175, 19,
        0xccab8ee7, 11, 0xffffffff, 7, 0x31b6ab00, 15, 0xffe50000, 17, 1812433253>;

    points = points::denseUniformPointScatter<BoolGrid, mt11213b>(grid, pointsPerVoxel);

    EXPECT_EQ(Index32(2), points->tree().leafCount());
    EXPECT_EQ(Index64(NUM_VALUES + 1), points->activeVoxelCount());
    EXPECT_EQ(expectedCount, pointCount(points->tree()));
}

TEST_F(TestPointScatter, testNonUniformPointScatter)
{
    const Index32 pointsPerVoxel = 8;
    const math::CoordBBox totalBoxBounds(math::Coord(-2), math::Coord(2)); // 125 voxels across 8 leaves
    const math::CoordBBox activeBoxBounds(math::Coord(-1), math::Coord(1)); // 27 voxels across 8 leaves

    // Test the free function for all default scalar grid types

    {
        BoolGrid grid;
        grid.sparseFill(totalBoxBounds, false, /*active*/true);
        grid.sparseFill(activeBoxBounds, true, /*active*/true);
        auto points = points::nonUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        DoubleGrid grid;
        grid.sparseFill(totalBoxBounds, 0.0, /*active*/true);
        grid.sparseFill(activeBoxBounds, 1.0, /*active*/true);
        auto points = points::nonUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        FloatGrid grid;
        grid.sparseFill(totalBoxBounds, 0.0f, /*active*/true);
        grid.sparseFill(activeBoxBounds, 1.0f, /*active*/true);
        auto points = points::nonUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        Int32Grid grid;
        grid.sparseFill(totalBoxBounds, 0, /*active*/true);
        grid.sparseFill(activeBoxBounds, 1, /*active*/true);
        auto points = points::nonUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        Int64Grid grid;
        grid.sparseFill(totalBoxBounds, 0, /*active*/true);
        grid.sparseFill(activeBoxBounds, 1, /*active*/true);
        auto points = points::nonUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }
    {
        MaskGrid grid;
        grid.sparseFill(totalBoxBounds, /*maskBuffer*/0);
        grid.sparseFill(activeBoxBounds, /*maskBuffer*/1);
        auto points = points::nonUniformPointScatter(grid, pointsPerVoxel);
        EXPECT_EQ(Index32(8), points->tree().leafCount());
        EXPECT_EQ(Index64(27), points->activeVoxelCount());
        EXPECT_EQ(Index64(pointsPerVoxel * 27), pointCount(points->tree()));
    }

    BoolGrid grid;

    // Test below 0 throws

    EXPECT_THROW(points::nonUniformPointScatter(grid, -0.1f), openvdb::ValueError);

    // Test a grid containing tiles scatters correctly

    grid.tree().addTile(/*level*/1, math::Coord(0), /*value*/true, /*active*/true);
    grid.tree().setValueOn(math::Coord(8,0,0), true); // add another leaf

    const Index32 NUM_VALUES = BoolGrid::TreeType::LeafNodeType::NUM_VALUES;

    EXPECT_EQ(Index32(1), grid.tree().leafCount());
    EXPECT_EQ(Index64(NUM_VALUES + 1), grid.activeVoxelCount());

    auto points = points::nonUniformPointScatter(grid, pointsPerVoxel);

    const Index64 expectedCount = Index64(pointsPerVoxel * (NUM_VALUES + 1));

    EXPECT_EQ(Index64(0), points->tree().activeTileCount());
    EXPECT_EQ(Index32(2), points->tree().leafCount());
    EXPECT_EQ(Index64(NUM_VALUES + 1), points->activeVoxelCount());
    EXPECT_EQ(expectedCount, pointCount(points->tree()));

    // Explicitly check P attribute

    const auto* attributeSet = &(points->tree().cbeginLeaf()->attributeSet());
    EXPECT_EQ(size_t(1), attributeSet->size());
    const auto* array = attributeSet->getConst(0);
    EXPECT_TRUE(array);

    using PositionArrayT = TypedAttributeArray<Vec3f, NullCodec>;
    EXPECT_TRUE(array->isType<PositionArrayT>());

    size_t size = array->size();
    EXPECT_EQ(size_t(pointsPerVoxel * NUM_VALUES), size);

    AttributeHandle<Vec3f, NullCodec>::Ptr pHandle =
        AttributeHandle<Vec3f, NullCodec>::create(*array);
    for (size_t i = 0; i < size; ++i) {
        const Vec3f P = pHandle->get(Index(i));
        EXPECT_TRUE(P[0] >=-0.5f);
        EXPECT_TRUE(P[0] <= 0.5f);
        EXPECT_TRUE(P[1] >=-0.5f);
        EXPECT_TRUE(P[1] <= 0.5f);
        EXPECT_TRUE(P[2] >=-0.5f);
        EXPECT_TRUE(P[2] <= 0.5f);
    }

    // Test the rng seed

    const Vec3f firstPosition = pHandle->get(0);
    points = points::nonUniformPointScatter(grid, pointsPerVoxel, /*seed*/1);

    attributeSet = &(points->tree().cbeginLeaf()->attributeSet());
    EXPECT_EQ(size_t(1), attributeSet->size());

    array = attributeSet->getConst(0);
    EXPECT_TRUE(array);
    EXPECT_TRUE(array->isType<PositionArrayT>());

    size = array->size();
    EXPECT_EQ(size_t(pointsPerVoxel * NUM_VALUES), size);
    pHandle = AttributeHandle<Vec3f, NullCodec>::create(*array);

    const Vec3f secondPosition = pHandle->get(0);
    EXPECT_TRUE(!math::isExactlyEqual(firstPosition[0], secondPosition[0]));
    EXPECT_TRUE(!math::isExactlyEqual(firstPosition[1], secondPosition[1]));
    EXPECT_TRUE(!math::isExactlyEqual(firstPosition[2], secondPosition[2]));

    // Test spread

    points = points::nonUniformPointScatter(grid, pointsPerVoxel, /*seed*/1, /*spread*/0.2f);

    attributeSet = &(points->tree().cbeginLeaf()->attributeSet());
    EXPECT_EQ(size_t(1), attributeSet->size());
    array = attributeSet->getConst(0);
    EXPECT_TRUE(array);
    EXPECT_TRUE(array->isType<PositionArrayT>());

    size = array->size();
    EXPECT_EQ(size_t(pointsPerVoxel * NUM_VALUES), size);

    pHandle = AttributeHandle<Vec3f, NullCodec>::create(*array);
    for (size_t i = 0; i < size; ++i) {
        const Vec3f P = pHandle->get(Index(i));
        EXPECT_TRUE(P[0] >=-0.2f);
        EXPECT_TRUE(P[0] <= 0.2f);
        EXPECT_TRUE(P[1] >=-0.2f);
        EXPECT_TRUE(P[1] <= 0.2f);
        EXPECT_TRUE(P[2] >=-0.2f);
        EXPECT_TRUE(P[2] <= 0.2f);
    }

    // Test varying counts

    Int32Grid countGrid;

    // tets negative values equate to 0
    countGrid.tree().setValueOn(Coord(0), -1);
    for (int i = 1; i < 8; ++i) {
        countGrid.tree().setValueOn(Coord(i), i);
    }

    points = points::nonUniformPointScatter(countGrid, pointsPerVoxel);

    EXPECT_EQ(Index32(1), points->tree().leafCount());
    EXPECT_EQ(Index64(7), points->activeVoxelCount());
    EXPECT_EQ(Index64(pointsPerVoxel * 28), pointCount(points->tree()));

    for (int i = 1; i < 8; ++i) {
        EXPECT_TRUE(points->tree().isValueOn(Coord(i)));
        auto& value = points->tree().getValue(Coord(i));
        Index32 expected(0);
        for (Index32 j = i; j > 0; --j) expected += j;
        EXPECT_EQ(Index32(expected * pointsPerVoxel), Index32(value));
    }

    // Test mt11213b

    using mt11213b = std::mersenne_twister_engine<uint32_t, 32, 351, 175, 19,
        0xccab8ee7, 11, 0xffffffff, 7, 0x31b6ab00, 15, 0xffe50000, 17, 1812433253>;

    points = points::nonUniformPointScatter<BoolGrid, mt11213b>(grid, pointsPerVoxel);

    EXPECT_EQ(Index32(2), points->tree().leafCount());
    EXPECT_EQ(Index64(NUM_VALUES + 1), points->activeVoxelCount());
    EXPECT_EQ(expectedCount, pointCount(points->tree()));
}
