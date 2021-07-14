// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include <openvdb/Exceptions.h>
#include <openvdb/Types.h>
#include <openvdb/math/Transform.h>
#include <openvdb/tools/ValueTransformer.h> // for tools::setValueOnMin(), et al.
#include <openvdb/tree/LeafNode.h>
#include <openvdb/io/Compression.h> // for io::RealToHalf
#include <openvdb/math/Math.h> // for Abs()
#include <openvdb/openvdb.h>
#include <openvdb/util/CpuTimer.h>
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/Prune.h>
#include <openvdb/tools/ChangeBackground.h>
#include <openvdb/tools/SignedFloodFill.h>
#include "util.h" // for unittest_util::makeSphere()

#include <gtest/gtest.h>

#include <cstdio> // for remove()
#include <fstream>
#include <sstream>

#define ASSERT_DOUBLES_EXACTLY_EQUAL(expected, actual) \
    EXPECT_NEAR((expected), (actual), /*tolerance=*/0.0);


using ValueType = float;
using LeafNodeType = openvdb::tree::LeafNode<ValueType,3>;
using InternalNodeType1 = openvdb::tree::InternalNode<LeafNodeType,4>;
using InternalNodeType2 = openvdb::tree::InternalNode<InternalNodeType1,5>;
using RootNodeType = openvdb::tree::RootNode<InternalNodeType2>;


class TestTree: public ::testing::Test
{
public:
    void SetUp() override { openvdb::initialize(); }
    void TearDown() override { openvdb::uninitialize(); }

protected:
    template<typename TreeType> void testWriteHalf();
    template<typename TreeType> void doTestMerge(openvdb::MergePolicy);
};


TEST_F(TestTree, testChangeBackground)
{
    const int dim = 128;
    const openvdb::Vec3f center(0.35f, 0.35f, 0.35f);
    const float
        radius = 0.15f,
        voxelSize = 1.0f / (dim-1),
        halfWidth = 4,
        gamma = halfWidth * voxelSize;
    using GridT = openvdb::FloatGrid;
    const openvdb::Coord inside(int(center[0]*dim), int(center[1]*dim), int(center[2]*dim));
    const openvdb::Coord outside(dim);

    {//changeBackground
        GridT::Ptr grid = openvdb::tools::createLevelSetSphere<GridT>(
            radius, center, voxelSize, halfWidth);
        openvdb::FloatTree& tree = grid->tree();

        EXPECT_TRUE(grid->tree().isValueOff(outside));
        ASSERT_DOUBLES_EXACTLY_EQUAL( gamma, tree.getValue(outside));

        EXPECT_TRUE(tree.isValueOff(inside));
        ASSERT_DOUBLES_EXACTLY_EQUAL(-gamma, tree.getValue(inside));

        const float background = gamma*3.43f;
        openvdb::tools::changeBackground(tree, background);

        EXPECT_TRUE(grid->tree().isValueOff(outside));
        ASSERT_DOUBLES_EXACTLY_EQUAL( background, tree.getValue(outside));

        EXPECT_TRUE(tree.isValueOff(inside));
        ASSERT_DOUBLES_EXACTLY_EQUAL(-background, tree.getValue(inside));
    }

    {//changeLevelSetBackground
        GridT::Ptr grid = openvdb::tools::createLevelSetSphere<GridT>(
            radius, center, voxelSize, halfWidth);
        openvdb::FloatTree& tree = grid->tree();

        EXPECT_TRUE(grid->tree().isValueOff(outside));
        ASSERT_DOUBLES_EXACTLY_EQUAL( gamma, tree.getValue(outside));

        EXPECT_TRUE(tree.isValueOff(inside));
        ASSERT_DOUBLES_EXACTLY_EQUAL(-gamma, tree.getValue(inside));

        const float v1 = gamma*3.43f, v2 = -gamma*6.457f;
        openvdb::tools::changeAsymmetricLevelSetBackground(tree, v1, v2);

        EXPECT_TRUE(grid->tree().isValueOff(outside));
        ASSERT_DOUBLES_EXACTLY_EQUAL( v1, tree.getValue(outside));

        EXPECT_TRUE(tree.isValueOff(inside));
        ASSERT_DOUBLES_EXACTLY_EQUAL( v2, tree.getValue(inside));
    }
}


TEST_F(TestTree, testHalf)
{
    testWriteHalf<openvdb::FloatTree>();
    testWriteHalf<openvdb::DoubleTree>();
    testWriteHalf<openvdb::Vec2STree>();
    testWriteHalf<openvdb::Vec2DTree>();
    testWriteHalf<openvdb::Vec3STree>();
    testWriteHalf<openvdb::Vec3DTree>();

    // Verify that non-floating-point grids are saved correctly.
    testWriteHalf<openvdb::BoolTree>();
    testWriteHalf<openvdb::Int32Tree>();
    testWriteHalf<openvdb::Int64Tree>();
}


template<class TreeType>
void
TestTree::testWriteHalf()
{
    using GridType = openvdb::Grid<TreeType>;
    using ValueT = typename TreeType::ValueType;
    ValueT background(5);
    GridType grid(background);

    unittest_util::makeSphere<GridType>(openvdb::Coord(64, 64, 64),
                                        openvdb::Vec3f(35, 30, 40),
                                        /*radius=*/10, grid,
                                        /*dx=*/1.0f, unittest_util::SPHERE_DENSE);
    EXPECT_TRUE(!grid.tree().empty());

    // Write grid blocks in both float and half formats.
    std::ostringstream outFull(std::ios_base::binary);
    grid.setSaveFloatAsHalf(false);
    grid.writeBuffers(outFull);
    outFull.flush();
    const size_t fullBytes = outFull.str().size();
    if (fullBytes == 0) FAIL() << "wrote empty full float buffers";

    std::ostringstream outHalf(std::ios_base::binary);
    grid.setSaveFloatAsHalf(true);
    grid.writeBuffers(outHalf);
    outHalf.flush();
    const size_t halfBytes = outHalf.str().size();
    if (halfBytes == 0) FAIL() << "wrote empty half float buffers";

    if (openvdb::io::RealToHalf<ValueT>::isReal) {
        // Verify that the half float file is "significantly smaller" than the full float file.
        if (halfBytes >= size_t(0.75 * double(fullBytes))) {
            FAIL() << "half float buffers not significantly smaller than full float ("
            << halfBytes << " vs. " << fullBytes << " bytes)";
        }
    } else {
        // For non-real data types, "half float" and "full float" file sizes should be the same.
        if (halfBytes != fullBytes) {
            FAIL() << "full float and half float file sizes differ for data of type "
            + std::string(openvdb::typeNameAsString<ValueT>());
        }
    }

    // Read back the half float data (converting back to full float in the process),
    // then write it out again in half float format.  Verify that the resulting file
    // is identical to the original half float file.
    {
        openvdb::Grid<TreeType> gridCopy(grid);
        gridCopy.setSaveFloatAsHalf(true);
        std::istringstream is(outHalf.str(), std::ios_base::binary);

        // Since the input stream doesn't include a VDB header with file format version info,
        // tag the input stream explicitly with the current version number.
        openvdb::io::setCurrentVersion(is);

        gridCopy.readBuffers(is);

        std::ostringstream outDiff(std::ios_base::binary);
        gridCopy.writeBuffers(outDiff);
        outDiff.flush();

        if (outHalf.str() != outDiff.str()) {
            FAIL() << "half-from-full and half-from-half buffers differ";
        }
    }
}


TEST_F(TestTree, testValues)
{
    ValueType background=5.0f;

    {
        const openvdb::Coord c0(5,10,20), c1(50000,20000,30000);
        RootNodeType root_node(background);
        const float v0=0.234f, v1=4.5678f;
        EXPECT_TRUE(root_node.empty());
        ASSERT_DOUBLES_EXACTLY_EQUAL(root_node.getValue(c0), background);
        ASSERT_DOUBLES_EXACTLY_EQUAL(root_node.getValue(c1), background);
        root_node.setValueOn(c0, v0);
        root_node.setValueOn(c1, v1);
        ASSERT_DOUBLES_EXACTLY_EQUAL(v0,root_node.getValue(c0));
        ASSERT_DOUBLES_EXACTLY_EQUAL(v1,root_node.getValue(c1));
        int count=0;
        for (int i =0; i<256; ++i) {
            for (int j=0; j<256; ++j) {
                for (int k=0; k<256; ++k) {
                    if (root_node.getValue(openvdb::Coord(i,j,k))<1.0f) ++count;
                }
            }
        }
        EXPECT_TRUE(count == 1);
    }

    {
        const openvdb::Coord min(-30,-25,-60), max(60,80,100);
        const openvdb::Coord c0(-5,-10,-20), c1(50,20,90), c2(59,67,89);
        const float v0=0.234f, v1=4.5678f, v2=-5.673f;
        RootNodeType root_node(background);
        EXPECT_TRUE(root_node.empty());
        ASSERT_DOUBLES_EXACTLY_EQUAL(background,root_node.getValue(c0));
        ASSERT_DOUBLES_EXACTLY_EQUAL(background,root_node.getValue(c1));
        ASSERT_DOUBLES_EXACTLY_EQUAL(background,root_node.getValue(c2));
        root_node.setValueOn(c0, v0);
        root_node.setValueOn(c1, v1);
        root_node.setValueOn(c2, v2);
        ASSERT_DOUBLES_EXACTLY_EQUAL(v0,root_node.getValue(c0));
        ASSERT_DOUBLES_EXACTLY_EQUAL(v1,root_node.getValue(c1));
        ASSERT_DOUBLES_EXACTLY_EQUAL(v2,root_node.getValue(c2));
        int count=0;
        for (int i =min[0]; i<max[0]; ++i) {
            for (int j=min[1]; j<max[1]; ++j) {
                for (int k=min[2]; k<max[2]; ++k) {
                    if (root_node.getValue(openvdb::Coord(i,j,k))<1.0f) ++count;
                }
            }
        }
        EXPECT_TRUE(count == 2);
    }
}


TEST_F(TestTree, testSetValue)
{
    const float background = 5.0f;
    openvdb::FloatTree tree(background);
    const openvdb::Coord c0( 5, 10, 20), c1(-5,-10,-20);

    ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree.getValue(c1));
    EXPECT_EQ(-1, tree.getValueDepth(c0));
    EXPECT_EQ(-1, tree.getValueDepth(c1));
    EXPECT_TRUE(tree.isValueOff(c0));
    EXPECT_TRUE(tree.isValueOff(c1));

    tree.setValue(c0, 10.0);

    ASSERT_DOUBLES_EXACTLY_EQUAL(10.0, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree.getValue(c1));
    EXPECT_EQ( 3, tree.getValueDepth(c0));
    EXPECT_EQ(-1, tree.getValueDepth(c1));
    EXPECT_EQ( 3, tree.getValueDepth(openvdb::Coord(7, 10, 20)));
    EXPECT_EQ( 2, tree.getValueDepth(openvdb::Coord(8, 10, 20)));
    EXPECT_TRUE(tree.isValueOn(c0));
    EXPECT_TRUE(tree.isValueOff(c1));

    tree.setValue(c1, 20.0);

    ASSERT_DOUBLES_EXACTLY_EQUAL(10.0, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(20.0, tree.getValue(c1));
    EXPECT_EQ( 3, tree.getValueDepth(c0));
    EXPECT_EQ( 3, tree.getValueDepth(c1));
    EXPECT_TRUE(tree.isValueOn(c0));
    EXPECT_TRUE(tree.isValueOn(c1));

    struct Local {
        static inline void minOp(float& f, bool& b) { f = std::min(f, 15.f); b = true; }
        static inline void maxOp(float& f, bool& b) { f = std::max(f, 12.f); b = true; }
        static inline void sumOp(float& f, bool& b) { f += /*background=*/5.f; b = true; }
    };

    openvdb::tools::setValueOnMin(tree, c0, 15.0);
    tree.modifyValueAndActiveState(c1, Local::minOp);

    ASSERT_DOUBLES_EXACTLY_EQUAL(10.0, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(15.0, tree.getValue(c1));

    openvdb::tools::setValueOnMax(tree, c0, 12.0);
    tree.modifyValueAndActiveState(c1, Local::maxOp);

    ASSERT_DOUBLES_EXACTLY_EQUAL(12.0, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(15.0, tree.getValue(c1));
    EXPECT_EQ(2, int(tree.activeVoxelCount()));

    float minVal = -999.0, maxVal = -999.0;
    tree.evalMinMax(minVal, maxVal);
    ASSERT_DOUBLES_EXACTLY_EQUAL(12.0, minVal);
    ASSERT_DOUBLES_EXACTLY_EQUAL(15.0, maxVal);

    tree.setValueOff(c0, background);

    ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(15.0, tree.getValue(c1));
    EXPECT_EQ(1, int(tree.activeVoxelCount()));

    openvdb::tools::setValueOnSum(tree, c0, background);
    tree.modifyValueAndActiveState(c1, Local::sumOp);

    ASSERT_DOUBLES_EXACTLY_EQUAL(2*background, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(15.0+background, tree.getValue(c1));
    EXPECT_EQ(2, int(tree.activeVoxelCount()));

    // Test the extremes of the coordinate range
    ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree.getValue(openvdb::Coord::min()));
    ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree.getValue(openvdb::Coord::max()));
    //std::cerr << "min=" << openvdb::Coord::min() << " max= " << openvdb::Coord::max() << "\n";
    tree.setValue(openvdb::Coord::min(), 1.0f);
    tree.setValue(openvdb::Coord::max(), 2.0f);
    ASSERT_DOUBLES_EXACTLY_EQUAL(1.0f, tree.getValue(openvdb::Coord::min()));
    ASSERT_DOUBLES_EXACTLY_EQUAL(2.0f, tree.getValue(openvdb::Coord::max()));
}


TEST_F(TestTree, testSetValueOnly)
{
    const float background = 5.0f;
    openvdb::FloatTree tree(background);
    const openvdb::Coord c0( 5, 10, 20), c1(-5,-10,-20);

    ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree.getValue(c1));
    EXPECT_EQ(-1, tree.getValueDepth(c0));
    EXPECT_EQ(-1, tree.getValueDepth(c1));
    EXPECT_TRUE(tree.isValueOff(c0));
    EXPECT_TRUE(tree.isValueOff(c1));

    tree.setValueOnly(c0, 10.0);

    ASSERT_DOUBLES_EXACTLY_EQUAL(10.0, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree.getValue(c1));
    EXPECT_EQ( 3, tree.getValueDepth(c0));
    EXPECT_EQ(-1, tree.getValueDepth(c1));
    EXPECT_EQ( 3, tree.getValueDepth(openvdb::Coord(7, 10, 20)));
    EXPECT_EQ( 2, tree.getValueDepth(openvdb::Coord(8, 10, 20)));
    EXPECT_TRUE(tree.isValueOff(c0));
    EXPECT_TRUE(tree.isValueOff(c1));

    tree.setValueOnly(c1, 20.0);

    ASSERT_DOUBLES_EXACTLY_EQUAL(10.0, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(20.0, tree.getValue(c1));
    EXPECT_EQ( 3, tree.getValueDepth(c0));
    EXPECT_EQ( 3, tree.getValueDepth(c1));
    EXPECT_TRUE(tree.isValueOff(c0));
    EXPECT_TRUE(tree.isValueOff(c1));

    tree.setValue(c0, 30.0);

    ASSERT_DOUBLES_EXACTLY_EQUAL(30.0, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(20.0, tree.getValue(c1));
    EXPECT_EQ( 3, tree.getValueDepth(c0));
    EXPECT_EQ( 3, tree.getValueDepth(c1));
    EXPECT_TRUE(tree.isValueOn(c0));
    EXPECT_TRUE(tree.isValueOff(c1));

    tree.setValueOnly(c0, 40.0);

    ASSERT_DOUBLES_EXACTLY_EQUAL(40.0, tree.getValue(c0));
    ASSERT_DOUBLES_EXACTLY_EQUAL(20.0, tree.getValue(c1));
    EXPECT_EQ( 3, tree.getValueDepth(c0));
    EXPECT_EQ( 3, tree.getValueDepth(c1));
    EXPECT_TRUE(tree.isValueOn(c0));
    EXPECT_TRUE(tree.isValueOff(c1));

    EXPECT_EQ(1, int(tree.activeVoxelCount()));
}

namespace {

// Simple float wrapper with required interface to be used as ValueType in tree::LeafNode
// Throws on copy-construction to ensure that all modifications are done in-place.
struct FloatThrowOnCopy
{
    float value = 0.0f;

    using T = FloatThrowOnCopy;

    FloatThrowOnCopy() = default;
    explicit FloatThrowOnCopy(float _value): value(_value) { }

    FloatThrowOnCopy(const FloatThrowOnCopy&) { throw openvdb::RuntimeError("No Copy"); }
    FloatThrowOnCopy& operator=(const FloatThrowOnCopy&) = default;

    T operator+(const float rhs) const { return T(value + rhs); }
    T operator-() const { return T(-value); }
    bool operator<(const T& other) const { return value < other.value; }
    bool operator>(const T& other) const { return value > other.value; }
    bool operator==(const T& other) const { return value == other.value; }

    friend std::ostream& operator<<(std::ostream &stream, const T& other)
    {
        stream << other.value;
        return stream;
    }
};

} // namespace

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace math {

OPENVDB_EXACT_IS_APPROX_EQUAL(FloatThrowOnCopy)

} // namespace math

template<>
inline std::string
TypedMetadata<FloatThrowOnCopy>::str() const { return ""; }

template <>
inline std::string
TypedMetadata<FloatThrowOnCopy>::typeName() const { return ""; }

} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb

TEST_F(TestTree, testSetValueInPlace)
{
    using FloatThrowOnCopyTree = openvdb::tree::Tree4<FloatThrowOnCopy, 5, 4, 3>::Type;
    using FloatThrowOnCopyGrid = openvdb::Grid<FloatThrowOnCopyTree>;

    FloatThrowOnCopyGrid::registerGrid();

    FloatThrowOnCopyTree tree;
    const openvdb::Coord c0(5, 10, 20), c1(-5,-10,-20);

    // tile values can legitimately be copied to assess whether a change in value
    // requires the tile to be voxelized, so activate and voxelize active tiles first

    tree.setActiveState(c0, true);
    tree.setActiveState(c1, true);

    tree.voxelizeActiveTiles(/*threaded=*/true);

    EXPECT_NO_THROW(tree.modifyValue(c0,
        [](FloatThrowOnCopy& lhs) { lhs.value = 1.4f; }
    ));

    EXPECT_NO_THROW(tree.modifyValueAndActiveState(c1,
        [](FloatThrowOnCopy& lhs, bool& b) { lhs.value = 2.7f; b = false; }
    ));

    EXPECT_NEAR(1.4f, tree.getValue(c0).value, 1.0e-7);
    EXPECT_NEAR(2.7f, tree.getValue(c1).value, 1.0e-7);

    EXPECT_TRUE(tree.isValueOn(c0));
    EXPECT_TRUE(!tree.isValueOn(c1));

    // use slower de-allocation to ensure that no value copying occurs

    tree.root().clear();
}

namespace {

/// Helper function to test openvdb::tree::Tree::evalMinMax() for various tree types
template<typename TreeT>
void
evalMinMaxTest()
{
    using ValueT = typename TreeT::ValueType;

    struct Local {
        static bool isEqual(const ValueT& a, const ValueT& b) {
            using namespace openvdb; // for operator>()
            return !(math::Abs(a - b) > zeroVal<ValueT>());
        }
    };

    const ValueT
        zero = openvdb::zeroVal<ValueT>(),
        minusTwo = zero + (-2),
        plusTwo = zero + 2,
        five = zero + 5;

    TreeT tree(/*background=*/five);

    // No set voxels (defaults to min = max = zero)
    ValueT minVal = five, maxVal = five;
    tree.evalMinMax(minVal, maxVal);
    EXPECT_TRUE(Local::isEqual(minVal, zero));
    EXPECT_TRUE(Local::isEqual(maxVal, zero));

    // Only one set voxel
    tree.setValue(openvdb::Coord(0, 0, 0), minusTwo);
    minVal = maxVal = five;
    tree.evalMinMax(minVal, maxVal);
    EXPECT_TRUE(Local::isEqual(minVal, minusTwo));
    EXPECT_TRUE(Local::isEqual(maxVal, minusTwo));

    // Multiple set voxels, single value
    tree.setValue(openvdb::Coord(10, 10, 10), minusTwo);
    minVal = maxVal = five;
    tree.evalMinMax(minVal, maxVal);
    EXPECT_TRUE(Local::isEqual(minVal, minusTwo));
    EXPECT_TRUE(Local::isEqual(maxVal, minusTwo));

    // Multiple set voxels, multiple values
    tree.setValue(openvdb::Coord(10, 10, 10), plusTwo);
    tree.setValue(openvdb::Coord(-10, -10, -10), zero);
    minVal = maxVal = five;
    tree.evalMinMax(minVal, maxVal);
    EXPECT_TRUE(Local::isEqual(minVal, minusTwo));
    EXPECT_TRUE(Local::isEqual(maxVal, plusTwo));
}

/// Specialization for boolean trees
template<>
void
evalMinMaxTest<openvdb::BoolTree>()
{
    openvdb::BoolTree tree(/*background=*/false);

    // No set voxels (defaults to min = max = zero)
    bool minVal = true, maxVal = false;
    tree.evalMinMax(minVal, maxVal);
    EXPECT_EQ(false, minVal);
    EXPECT_EQ(false, maxVal);

    // Only one set voxel
    tree.setValue(openvdb::Coord(0, 0, 0), true);
    minVal = maxVal = false;
    tree.evalMinMax(minVal, maxVal);
    EXPECT_EQ(true, minVal);
    EXPECT_EQ(true, maxVal);

    // Multiple set voxels, single value
    tree.setValue(openvdb::Coord(-10, -10, -10), true);
    minVal = maxVal = false;
    tree.evalMinMax(minVal, maxVal);
    EXPECT_EQ(true, minVal);
    EXPECT_EQ(true, maxVal);

    // Multiple set voxels, multiple values
    tree.setValue(openvdb::Coord(10, 10, 10), false);
    minVal = true; maxVal = false;
    tree.evalMinMax(minVal, maxVal);
    EXPECT_EQ(false, minVal);
    EXPECT_EQ(true, maxVal);
}

/// Specialization for Coord trees
template<>
void
evalMinMaxTest<openvdb::Coord>()
{
    using CoordTree = openvdb::tree::Tree4<openvdb::Coord,5,4,3>::Type;
    const openvdb::Coord backg(5,4,-6), a(5,4,-7), b(5,5,-6);

    CoordTree tree(backg);

    // No set voxels (defaults to min = max = zero)
    openvdb::Coord minVal=openvdb::Coord::max(), maxVal=openvdb::Coord::min();
    tree.evalMinMax(minVal, maxVal);
    EXPECT_EQ(openvdb::Coord(0), minVal);
    EXPECT_EQ(openvdb::Coord(0), maxVal);

    // Only one set voxel
    tree.setValue(openvdb::Coord(0, 0, 0), a);
    minVal=openvdb::Coord::max();
    maxVal=openvdb::Coord::min();
    tree.evalMinMax(minVal, maxVal);
    EXPECT_EQ(a, minVal);
    EXPECT_EQ(a, maxVal);

    // Multiple set voxels
    tree.setValue(openvdb::Coord(-10, -10, -10), b);
    minVal=openvdb::Coord::max();
    maxVal=openvdb::Coord::min();
    tree.evalMinMax(minVal, maxVal);
    EXPECT_EQ(a, minVal);
    EXPECT_EQ(b, maxVal);
}

} // unnamed namespace

TEST_F(TestTree, testEvalMinMax)
{
    evalMinMaxTest<openvdb::BoolTree>();
    evalMinMaxTest<openvdb::FloatTree>();
    evalMinMaxTest<openvdb::Int32Tree>();
    evalMinMaxTest<openvdb::Vec3STree>();
    evalMinMaxTest<openvdb::Vec2ITree>();
    evalMinMaxTest<openvdb::Coord>();
}


TEST_F(TestTree, testResize)
{
    ValueType background=5.0f;
    //use this when resize is implemented
    RootNodeType root_node(background);
    EXPECT_TRUE(root_node.getLevel()==3);
    ASSERT_DOUBLES_EXACTLY_EQUAL(background, root_node.getValue(openvdb::Coord(5,10,20)));
    //fprintf(stdout,"Root grid  dim=(%i,%i,%i)\n",
    //    root_node.getGridDim(0), root_node.getGridDim(1), root_node.getGridDim(2));
    root_node.setValueOn(openvdb::Coord(5,10,20),0.234f);
    ASSERT_DOUBLES_EXACTLY_EQUAL(root_node.getValue(openvdb::Coord(5,10,20)) , 0.234f);
    root_node.setValueOn(openvdb::Coord(500,200,300),4.5678f);
    ASSERT_DOUBLES_EXACTLY_EQUAL(root_node.getValue(openvdb::Coord(500,200,300)) , 4.5678f);
    {
        ValueType sum=0.0f;
        for (RootNodeType::ChildOnIter root_iter = root_node.beginChildOn();
            root_iter.test(); ++root_iter)
        {
            for (InternalNodeType2::ChildOnIter internal_iter2 = root_iter->beginChildOn();
                internal_iter2.test(); ++internal_iter2)
            {
                for (InternalNodeType1::ChildOnIter internal_iter1 =
                    internal_iter2->beginChildOn(); internal_iter1.test(); ++internal_iter1)
                {
                    for (LeafNodeType::ValueOnIter block_iter =
                        internal_iter1->beginValueOn(); block_iter.test(); ++block_iter)
                    {
                        sum += *block_iter;
                    }
                }
            }
        }
        ASSERT_DOUBLES_EXACTLY_EQUAL(sum, (0.234f + 4.5678f));
    }

    EXPECT_TRUE(root_node.getLevel()==3);
    ASSERT_DOUBLES_EXACTLY_EQUAL(background, root_node.getValue(openvdb::Coord(5,11,20)));
    {
        ValueType sum=0.0f;
        for (RootNodeType::ChildOnIter root_iter = root_node.beginChildOn();
            root_iter.test(); ++root_iter)
        {
            for (InternalNodeType2::ChildOnIter internal_iter2 = root_iter->beginChildOn();
                internal_iter2.test(); ++internal_iter2)
            {
                for (InternalNodeType1::ChildOnIter internal_iter1 =
                    internal_iter2->beginChildOn(); internal_iter1.test(); ++internal_iter1)
                {
                    for (LeafNodeType::ValueOnIter block_iter =
                        internal_iter1->beginValueOn(); block_iter.test(); ++block_iter)
                    {
                        sum += *block_iter;
                    }
                }
            }
        }
        ASSERT_DOUBLES_EXACTLY_EQUAL(sum, (0.234f + 4.5678f));
    }

}


TEST_F(TestTree, testHasSameTopology)
{
    // Test using trees of the same type.
    {
        const float background1=5.0f;
        openvdb::FloatTree tree1(background1);

        const float background2=6.0f;
        openvdb::FloatTree tree2(background2);

        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));

        tree1.setValue(openvdb::Coord(-10,40,845),3.456f);
        EXPECT_TRUE(!tree1.hasSameTopology(tree2));
        EXPECT_TRUE(!tree2.hasSameTopology(tree1));

        tree2.setValue(openvdb::Coord(-10,40,845),-3.456f);
        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));

        tree1.setValue(openvdb::Coord(1,-500,-8), 1.0f);
        EXPECT_TRUE(!tree1.hasSameTopology(tree2));
        EXPECT_TRUE(!tree2.hasSameTopology(tree1));

        tree2.setValue(openvdb::Coord(1,-500,-8),1.0f);
        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));
    }
    // Test using trees of different types.
    {
        const float background1=5.0f;
        openvdb::FloatTree tree1(background1);

        const openvdb::Vec3f background2(1.0f,3.4f,6.0f);
        openvdb::Vec3fTree tree2(background2);

        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));

        tree1.setValue(openvdb::Coord(-10,40,845),3.456f);
        EXPECT_TRUE(!tree1.hasSameTopology(tree2));
        EXPECT_TRUE(!tree2.hasSameTopology(tree1));

        tree2.setValue(openvdb::Coord(-10,40,845),openvdb::Vec3f(1.0f,2.0f,-3.0f));
        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));

        tree1.setValue(openvdb::Coord(1,-500,-8), 1.0f);
        EXPECT_TRUE(!tree1.hasSameTopology(tree2));
        EXPECT_TRUE(!tree2.hasSameTopology(tree1));

        tree2.setValue(openvdb::Coord(1,-500,-8),openvdb::Vec3f(1.0f,2.0f,-3.0f));
        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));
    }
}


TEST_F(TestTree, testTopologyCopy)
{
    // Test using trees of the same type.
    {
        const float background1=5.0f;
        openvdb::FloatTree tree1(background1);
        tree1.setValue(openvdb::Coord(-10,40,845),3.456f);
        tree1.setValue(openvdb::Coord(1,-50,-8), 1.0f);

        const float background2=6.0f, setValue2=3.0f;
        openvdb::FloatTree tree2(tree1,background2,setValue2,openvdb::TopologyCopy());

        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));

        ASSERT_DOUBLES_EXACTLY_EQUAL(background2, tree2.getValue(openvdb::Coord(1,2,3)));
        ASSERT_DOUBLES_EXACTLY_EQUAL(setValue2, tree2.getValue(openvdb::Coord(-10,40,845)));
        ASSERT_DOUBLES_EXACTLY_EQUAL(setValue2, tree2.getValue(openvdb::Coord(1,-50,-8)));

        tree1.setValue(openvdb::Coord(1,-500,-8), 1.0f);
        EXPECT_TRUE(!tree1.hasSameTopology(tree2));
        EXPECT_TRUE(!tree2.hasSameTopology(tree1));

        tree2.setValue(openvdb::Coord(1,-500,-8),1.0f);
        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));
    }
    // Test using trees of different types.
    {
        const openvdb::Vec3f background1(1.0f,3.4f,6.0f);
        openvdb::Vec3fTree tree1(background1);
        tree1.setValue(openvdb::Coord(-10,40,845),openvdb::Vec3f(3.456f,-2.3f,5.6f));
        tree1.setValue(openvdb::Coord(1,-50,-8), openvdb::Vec3f(1.0f,3.0f,4.5f));

        const float background2=6.0f, setValue2=3.0f;
        openvdb::FloatTree tree2(tree1,background2,setValue2,openvdb::TopologyCopy());

        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));

        ASSERT_DOUBLES_EXACTLY_EQUAL(background2, tree2.getValue(openvdb::Coord(1,2,3)));
        ASSERT_DOUBLES_EXACTLY_EQUAL(setValue2, tree2.getValue(openvdb::Coord(-10,40,845)));
        ASSERT_DOUBLES_EXACTLY_EQUAL(setValue2, tree2.getValue(openvdb::Coord(1,-50,-8)));

        tree1.setValue(openvdb::Coord(1,-500,-8), openvdb::Vec3f(1.0f,0.0f,-3.0f));
        EXPECT_TRUE(!tree1.hasSameTopology(tree2));
        EXPECT_TRUE(!tree2.hasSameTopology(tree1));

        tree2.setValue(openvdb::Coord(1,-500,-8), 1.0f);
        EXPECT_TRUE(tree1.hasSameTopology(tree2));
        EXPECT_TRUE(tree2.hasSameTopology(tree1));
    }
}


TEST_F(TestTree, testIterators)
{
    ValueType background=5.0f;
    RootNodeType root_node(background);
    root_node.setValueOn(openvdb::Coord(5,10,20),0.234f);
    root_node.setValueOn(openvdb::Coord(50000,20000,30000),4.5678f);
    {
        ValueType sum=0.0f;
        for (RootNodeType::ChildOnIter root_iter = root_node.beginChildOn();
            root_iter.test(); ++root_iter)
        {
            for (InternalNodeType2::ChildOnIter internal_iter2 = root_iter->beginChildOn();
                internal_iter2.test(); ++internal_iter2)
            {
                for (InternalNodeType1::ChildOnIter internal_iter1 =
                    internal_iter2->beginChildOn(); internal_iter1.test(); ++internal_iter1)
                {
                    for (LeafNodeType::ValueOnIter block_iter =
                        internal_iter1->beginValueOn(); block_iter.test(); ++block_iter)
                    {
                        sum += *block_iter;
                    }
                }
            }
        }
        ASSERT_DOUBLES_EXACTLY_EQUAL((0.234f + 4.5678f), sum);
    }
    {
        // As above, but using dense iterators.
        ValueType sum = 0.0f, val = 0.0f;
        for (RootNodeType::ChildAllIter rootIter = root_node.beginChildAll();
            rootIter.test(); ++rootIter)
        {
            if (!rootIter.isChildNode()) continue;

            for (InternalNodeType2::ChildAllIter internalIter2 =
                rootIter.probeChild(val)->beginChildAll(); internalIter2; ++internalIter2)
            {
                if (!internalIter2.isChildNode()) continue;

                for (InternalNodeType1::ChildAllIter internalIter1 =
                    internalIter2.probeChild(val)->beginChildAll(); internalIter1; ++internalIter1)
                {
                    if (!internalIter1.isChildNode()) continue;

                    for (LeafNodeType::ValueOnIter leafIter =
                        internalIter1.probeChild(val)->beginValueOn(); leafIter; ++leafIter)
                    {
                        sum += *leafIter;
                    }
                }
            }
        }
        ASSERT_DOUBLES_EXACTLY_EQUAL((0.234f + 4.5678f), sum);
    }
    {
        ValueType v_sum=0.0f;
        openvdb::Coord xyz0, xyz1, xyz2, xyz3, xyzSum(0, 0, 0);
        for (RootNodeType::ChildOnIter root_iter = root_node.beginChildOn();
            root_iter.test(); ++root_iter)
        {
            root_iter.getCoord(xyz3);
            for (InternalNodeType2::ChildOnIter internal_iter2 = root_iter->beginChildOn();
                internal_iter2.test(); ++internal_iter2)
            {
                internal_iter2.getCoord(xyz2);
                xyz2 = xyz2 - internal_iter2.parent().origin();
                for (InternalNodeType1::ChildOnIter internal_iter1 =
                    internal_iter2->beginChildOn(); internal_iter1.test(); ++internal_iter1)
                {
                    internal_iter1.getCoord(xyz1);
                    xyz1 = xyz1 - internal_iter1.parent().origin();
                    for (LeafNodeType::ValueOnIter block_iter =
                        internal_iter1->beginValueOn(); block_iter.test(); ++block_iter)
                    {
                        block_iter.getCoord(xyz0);
                        xyz0 = xyz0 - block_iter.parent().origin();
                        v_sum += *block_iter;
                        xyzSum = xyzSum + xyz0 + xyz1 + xyz2 + xyz3;
                    }
                }
            }
        }
        ASSERT_DOUBLES_EXACTLY_EQUAL((0.234f + 4.5678f), v_sum);
        EXPECT_EQ(openvdb::Coord(5 + 50000, 10 + 20000, 20 + 30000), xyzSum);
    }
}


TEST_F(TestTree, testIO)
{
    const char* filename = "testIO.dbg";
    openvdb::SharedPtr<const char> scopedFile(filename, ::remove);
    {
        ValueType background=5.0f;
        RootNodeType root_node(background);
        root_node.setValueOn(openvdb::Coord(5,10,20),0.234f);
        root_node.setValueOn(openvdb::Coord(50000,20000,30000),4.5678f);

        std::ofstream os(filename, std::ios_base::binary);
        root_node.writeTopology(os);
        root_node.writeBuffers(os);
        EXPECT_TRUE(!os.fail());
    }
    {
        ValueType background=2.0f;
        RootNodeType root_node(background);
        ASSERT_DOUBLES_EXACTLY_EQUAL(background, root_node.getValue(openvdb::Coord(5,10,20)));
        {
            std::ifstream is(filename, std::ios_base::binary);
            // Since the test file doesn't include a VDB header with file format version info,
            // tag the input stream explicitly with the current version number.
            openvdb::io::setCurrentVersion(is);
            root_node.readTopology(is);
            root_node.readBuffers(is);
            EXPECT_TRUE(!is.fail());
        }

        ASSERT_DOUBLES_EXACTLY_EQUAL(0.234f, root_node.getValue(openvdb::Coord(5,10,20)));
        ASSERT_DOUBLES_EXACTLY_EQUAL(5.0f, root_node.getValue(openvdb::Coord(5,11,20)));
        ValueType sum=0.0f;
        for (RootNodeType::ChildOnIter root_iter = root_node.beginChildOn();
            root_iter.test(); ++root_iter)
        {
            for (InternalNodeType2::ChildOnIter internal_iter2 = root_iter->beginChildOn();
                internal_iter2.test(); ++internal_iter2)
            {
                for (InternalNodeType1::ChildOnIter internal_iter1 =
                    internal_iter2->beginChildOn(); internal_iter1.test(); ++internal_iter1)
                {
                    for (LeafNodeType::ValueOnIter block_iter =
                        internal_iter1->beginValueOn(); block_iter.test(); ++block_iter)
                    {
                        sum += *block_iter;
                    }
                }
            }
        }
        ASSERT_DOUBLES_EXACTLY_EQUAL(sum, (0.234f + 4.5678f));
    }
}


TEST_F(TestTree, testNegativeIndexing)
{
    ValueType background=5.0f;
    openvdb::FloatTree tree(background);
    EXPECT_TRUE(tree.empty());
    ASSERT_DOUBLES_EXACTLY_EQUAL(tree.getValue(openvdb::Coord(5,-10,20)), background);
    ASSERT_DOUBLES_EXACTLY_EQUAL(tree.getValue(openvdb::Coord(-5000,2000,3000)), background);
    tree.setValue(openvdb::Coord( 5, 10, 20),0.0f);
    tree.setValue(openvdb::Coord(-5, 10, 20),0.1f);
    tree.setValue(openvdb::Coord( 5,-10, 20),0.2f);
    tree.setValue(openvdb::Coord( 5, 10,-20),0.3f);
    tree.setValue(openvdb::Coord(-5,-10, 20),0.4f);
    tree.setValue(openvdb::Coord(-5, 10,-20),0.5f);
    tree.setValue(openvdb::Coord( 5,-10,-20),0.6f);
    tree.setValue(openvdb::Coord(-5,-10,-20),0.7f);
    tree.setValue(openvdb::Coord(-5000, 2000,-3000),4.5678f);
    tree.setValue(openvdb::Coord( 5000,-2000,-3000),4.5678f);
    tree.setValue(openvdb::Coord(-5000,-2000, 3000),4.5678f);
    ASSERT_DOUBLES_EXACTLY_EQUAL(0.0f, tree.getValue(openvdb::Coord( 5, 10, 20)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(0.1f, tree.getValue(openvdb::Coord(-5, 10, 20)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(0.2f, tree.getValue(openvdb::Coord( 5,-10, 20)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(0.3f, tree.getValue(openvdb::Coord( 5, 10,-20)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(0.4f, tree.getValue(openvdb::Coord(-5,-10, 20)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(0.5f, tree.getValue(openvdb::Coord(-5, 10,-20)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(0.6f, tree.getValue(openvdb::Coord( 5,-10,-20)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(0.7f, tree.getValue(openvdb::Coord(-5,-10,-20)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(4.5678f, tree.getValue(openvdb::Coord(-5000, 2000,-3000)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(4.5678f, tree.getValue(openvdb::Coord( 5000,-2000,-3000)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(4.5678f, tree.getValue(openvdb::Coord(-5000,-2000, 3000)));
    int count=0;
    for (int i =-25; i<25; ++i) {
        for (int j=-25; j<25; ++j) {
            for (int k=-25; k<25; ++k) {
                if (tree.getValue(openvdb::Coord(i,j,k))<1.0f) {
                    //fprintf(stderr,"(%i,%i,%i)=%f\n",i,j,k,tree.getValue(openvdb::Coord(i,j,k)));
                    ++count;
                }
            }
        }
    }
    EXPECT_TRUE(count == 8);
    int count2 = 0;
    openvdb::Coord xyz;
    for (openvdb::FloatTree::ValueOnCIter iter = tree.cbeginValueOn(); iter; ++iter) {
        ++count2;
        xyz = iter.getCoord();
        //std::cerr << xyz << " = " << *iter << "\n";
    }
    EXPECT_TRUE(count2 == 11);
    EXPECT_TRUE(tree.activeVoxelCount() == 11);
    {
        count2 = 0;
        for (openvdb::FloatTree::ValueOnCIter iter = tree.cbeginValueOn(); iter; ++iter) {
            ++count2;
            xyz = iter.getCoord();
            //std::cerr << xyz << " = " << *iter << "\n";
        }
        EXPECT_TRUE(count2 == 11);
        EXPECT_TRUE(tree.activeVoxelCount() == 11);
    }
}


TEST_F(TestTree, testDeepCopy)
{
    // set up a tree
    const float fillValue1=5.0f;
    openvdb::FloatTree tree1(fillValue1);
    tree1.setValue(openvdb::Coord(-10,40,845), 3.456f);
    tree1.setValue(openvdb::Coord(1,-50,-8), 1.0f);

    // make a deep copy of the tree
    openvdb::TreeBase::Ptr newTree = tree1.copy();

    // cast down to the concrete type to query values
    openvdb::FloatTree *pTree2 = dynamic_cast<openvdb::FloatTree *>(newTree.get());

    // compare topology
    EXPECT_TRUE(tree1.hasSameTopology(*pTree2));
    EXPECT_TRUE(pTree2->hasSameTopology(tree1));

    // trees should be equal
    ASSERT_DOUBLES_EXACTLY_EQUAL(fillValue1, pTree2->getValue(openvdb::Coord(1,2,3)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(3.456f, pTree2->getValue(openvdb::Coord(-10,40,845)));
    ASSERT_DOUBLES_EXACTLY_EQUAL(1.0f, pTree2->getValue(openvdb::Coord(1,-50,-8)));

    // change 1 value in tree2
    openvdb::Coord changeCoord(1, -500, -8);
    pTree2->setValue(changeCoord, 1.0f);

    // topology should no longer match
    EXPECT_TRUE(!tree1.hasSameTopology(*pTree2));
    EXPECT_TRUE(!pTree2->hasSameTopology(tree1));

    // query changed value and make sure it's different between trees
    ASSERT_DOUBLES_EXACTLY_EQUAL(fillValue1, tree1.getValue(changeCoord));
    ASSERT_DOUBLES_EXACTLY_EQUAL(1.0f, pTree2->getValue(changeCoord));
}


TEST_F(TestTree, testMerge)
{
    ValueType background=5.0f;
    openvdb::FloatTree tree0(background), tree1(background), tree2(background);
     EXPECT_TRUE(tree2.empty());
    tree0.setValue(openvdb::Coord( 5, 10, 20),0.0f);
    tree0.setValue(openvdb::Coord(-5, 10, 20),0.1f);
    tree0.setValue(openvdb::Coord( 5,-10, 20),0.2f);
    tree0.setValue(openvdb::Coord( 5, 10,-20),0.3f);
    tree1.setValue(openvdb::Coord( 5, 10, 20),0.0f);
    tree1.setValue(openvdb::Coord(-5, 10, 20),0.1f);
    tree1.setValue(openvdb::Coord( 5,-10, 20),0.2f);
    tree1.setValue(openvdb::Coord( 5, 10,-20),0.3f);

    tree0.setValue(openvdb::Coord(-5,-10, 20),0.4f);
    tree0.setValue(openvdb::Coord(-5, 10,-20),0.5f);
    tree0.setValue(openvdb::Coord( 5,-10,-20),0.6f);
    tree0.setValue(openvdb::Coord(-5,-10,-20),0.7f);
    tree0.setValue(openvdb::Coord(-5000, 2000,-3000),4.5678f);
    tree0.setValue(openvdb::Coord( 5000,-2000,-3000),4.5678f);
    tree0.setValue(openvdb::Coord(-5000,-2000, 3000),4.5678f);
    tree2.setValue(openvdb::Coord(-5,-10, 20),0.4f);
    tree2.setValue(openvdb::Coord(-5, 10,-20),0.5f);
    tree2.setValue(openvdb::Coord( 5,-10,-20),0.6f);
    tree2.setValue(openvdb::Coord(-5,-10,-20),0.7f);
    tree2.setValue(openvdb::Coord(-5000, 2000,-3000),4.5678f);
    tree2.setValue(openvdb::Coord( 5000,-2000,-3000),4.5678f);
    tree2.setValue(openvdb::Coord(-5000,-2000, 3000),4.5678f);

    EXPECT_TRUE(tree0.leafCount()!=tree1.leafCount());
    EXPECT_TRUE(tree0.leafCount()!=tree2.leafCount());

    EXPECT_TRUE(!tree2.empty());
    tree1.merge(tree2, openvdb::MERGE_ACTIVE_STATES);
    EXPECT_TRUE(tree2.empty());
    EXPECT_TRUE(tree0.leafCount()==tree1.leafCount());
    EXPECT_TRUE(tree0.nonLeafCount()==tree1.nonLeafCount());
    EXPECT_TRUE(tree0.activeLeafVoxelCount()==tree1.activeLeafVoxelCount());
    EXPECT_TRUE(tree0.inactiveLeafVoxelCount()==tree1.inactiveLeafVoxelCount());
    EXPECT_TRUE(tree0.activeVoxelCount()==tree1.activeVoxelCount());
    EXPECT_TRUE(tree0.inactiveVoxelCount()==tree1.inactiveVoxelCount());

    for (openvdb::FloatTree::ValueOnCIter iter0 = tree0.cbeginValueOn(); iter0; ++iter0) {
        ASSERT_DOUBLES_EXACTLY_EQUAL(*iter0,tree1.getValue(iter0.getCoord()));
    }

    // Test active tile support.
    {
        using namespace openvdb;
        FloatTree treeA(/*background*/0.0), treeB(/*background*/0.0);

        treeA.fill(CoordBBox(Coord(16,16,16), Coord(31,31,31)), /*value*/1.0);
        treeB.fill(CoordBBox(Coord(0,0,0),    Coord(15,15,15)), /*value*/1.0);

        EXPECT_EQ(4096, int(treeA.activeVoxelCount()));
        EXPECT_EQ(4096, int(treeB.activeVoxelCount()));

        treeA.merge(treeB, MERGE_ACTIVE_STATES);

        EXPECT_EQ(8192, int(treeA.activeVoxelCount()));
        EXPECT_EQ(0, int(treeB.activeVoxelCount()));
    }

    doTestMerge<openvdb::FloatTree>(openvdb::MERGE_NODES);
    doTestMerge<openvdb::FloatTree>(openvdb::MERGE_ACTIVE_STATES);
    doTestMerge<openvdb::FloatTree>(openvdb::MERGE_ACTIVE_STATES_AND_NODES);

    doTestMerge<openvdb::BoolTree>(openvdb::MERGE_NODES);
    doTestMerge<openvdb::BoolTree>(openvdb::MERGE_ACTIVE_STATES);
    doTestMerge<openvdb::BoolTree>(openvdb::MERGE_ACTIVE_STATES_AND_NODES);
}


template<typename TreeType>
void
TestTree::doTestMerge(openvdb::MergePolicy policy)
{
    using namespace openvdb;

    TreeType treeA, treeB;

    using RootT = typename TreeType::RootNodeType;
    using LeafT = typename TreeType::LeafNodeType;

    const typename TreeType::ValueType val(1);
    const int
        depth = static_cast<int>(treeA.treeDepth()),
        leafDim = static_cast<int>(LeafT::dim()),
        leafSize = static_cast<int>(LeafT::size());
    // Coords that are in a different top-level branch than (0, 0, 0)
    const Coord pos(static_cast<int>(RootT::getChildDim()));

    treeA.setValueOff(pos, val);
    treeA.setValueOff(-pos, val);

    treeB.setValueOff(Coord(0), val);
    treeB.fill(CoordBBox(pos, pos.offsetBy(leafDim - 1)), val, /*active=*/true);
    treeB.setValueOn(-pos, val);

    //      treeA                  treeB            .
    //                                              .
    //        R                      R              .
    //       / \                    /|\             .
    //      I   I                  I I I            .
    //     /     \                /  |  \           .
    //    I       I              I   I   I          .
    //   /         \            /    | on x SIZE    .
    //  L           L          L     L              .
    // off         off        on    off             .

    EXPECT_EQ(0, int(treeA.activeVoxelCount()));
    EXPECT_EQ(leafSize + 1, int(treeB.activeVoxelCount()));
    EXPECT_EQ(2, int(treeA.leafCount()));
    EXPECT_EQ(2, int(treeB.leafCount()));
    EXPECT_EQ(2*(depth-2)+1, int(treeA.nonLeafCount())); // 2 branches (II+II+R)
    EXPECT_EQ(3*(depth-2)+1, int(treeB.nonLeafCount())); // 3 branches (II+II+II+R)

    treeA.merge(treeB, policy);

    //   MERGE_NODES    MERGE_ACTIVE_STATES  MERGE_ACTIVE_STATES_AND_NODES  .
    //                                                                      .
    //        R                  R                         R                .
    //       /|\                /|\                       /|\               .
    //      I I I              I I I                     I I I              .
    //     /  |  \            /  |  \                   /  |  \             .
    //    I   I   I          I   I   I                 I   I   I            .
    //   /    |    \        /    | on x SIZE          /    |    \           .
    //  L     L     L      L     L                   L     L     L          .
    // off   off   off    on    off                 on    off  on x SIZE    .

    switch (policy) {
    case MERGE_NODES:
        EXPECT_EQ(0, int(treeA.activeVoxelCount()));
        EXPECT_EQ(2 + 1, int(treeA.leafCount())); // 1 leaf node stolen from B
        EXPECT_EQ(3*(depth-2)+1, int(treeA.nonLeafCount())); // 3 branches (II+II+II+R)
        break;
    case MERGE_ACTIVE_STATES:
        EXPECT_EQ(2, int(treeA.leafCount())); // 1 leaf stolen, 1 replaced with tile
        EXPECT_EQ(3*(depth-2)+1, int(treeA.nonLeafCount())); // 3 branches (II+II+II+R)
        EXPECT_EQ(leafSize + 1, int(treeA.activeVoxelCount()));
        break;
    case MERGE_ACTIVE_STATES_AND_NODES:
        EXPECT_EQ(2 + 1, int(treeA.leafCount())); // 1 leaf node stolen from B
        EXPECT_EQ(3*(depth-2)+1, int(treeA.nonLeafCount())); // 3 branches (II+II+II+R)
        EXPECT_EQ(leafSize + 1, int(treeA.activeVoxelCount()));
        break;
    }
    EXPECT_TRUE(treeB.empty());
}


TEST_F(TestTree, testVoxelizeActiveTiles)
{
    using openvdb::CoordBBox;
    using openvdb::Coord;
    // Use a small custom tree so we don't run out of memory when
    // tiles are converted to dense leafs :)
    using MyTree = openvdb::tree::Tree4<float,2, 2, 2>::Type;
    float background=5.0f;
    const Coord xyz[] = {Coord(-1,-2,-3),Coord( 1, 2, 3)};
    //check two leaf nodes and two tiles at each level 1, 2 and 3
    const int tile_size[4]={0, 1<<2, 1<<(2*2), 1<<(3*2)};
    // serial version
    for (int level=0; level<=3; ++level) {

        MyTree tree(background);
        EXPECT_EQ(-1,tree.getValueDepth(xyz[0]));
        EXPECT_EQ(-1,tree.getValueDepth(xyz[1]));

        if (level==0) {
            tree.setValue(xyz[0], 1.0f);
            tree.setValue(xyz[1], 1.0f);
        } else {
            const int n = tile_size[level];
            tree.fill(CoordBBox::createCube(Coord(-n,-n,-n), n), 1.0f, true);
            tree.fill(CoordBBox::createCube(Coord( 0, 0, 0), n), 1.0f, true);
        }

        EXPECT_EQ(3-level,tree.getValueDepth(xyz[0]));
        EXPECT_EQ(3-level,tree.getValueDepth(xyz[1]));

        tree.voxelizeActiveTiles(false);

        EXPECT_EQ(3      ,tree.getValueDepth(xyz[0]));
        EXPECT_EQ(3      ,tree.getValueDepth(xyz[1]));
    }
    // multi-threaded version
    for (int level=0; level<=3; ++level) {

        MyTree tree(background);
        EXPECT_EQ(-1,tree.getValueDepth(xyz[0]));
        EXPECT_EQ(-1,tree.getValueDepth(xyz[1]));

        if (level==0) {
            tree.setValue(xyz[0], 1.0f);
            tree.setValue(xyz[1], 1.0f);
        } else {
            const int n = tile_size[level];
            tree.fill(CoordBBox::createCube(Coord(-n,-n,-n), n), 1.0f, true);
            tree.fill(CoordBBox::createCube(Coord( 0, 0, 0), n), 1.0f, true);
        }

        EXPECT_EQ(3-level,tree.getValueDepth(xyz[0]));
        EXPECT_EQ(3-level,tree.getValueDepth(xyz[1]));

        tree.voxelizeActiveTiles(true);

        EXPECT_EQ(3      ,tree.getValueDepth(xyz[0]));
        EXPECT_EQ(3      ,tree.getValueDepth(xyz[1]));
    }
#if 0
    const CoordBBox bbox(openvdb::Coord(-30,-50,-30), openvdb::Coord(530,610,623));
    {// benchmark serial
        MyTree tree(background);
        tree.sparseFill( bbox, 1.0f, /*state*/true);
        openvdb::util::CpuTimer timer("\nserial voxelizeActiveTiles");
        tree.voxelizeActiveTiles(/*threaded*/false);
        timer.stop();
    }
    {// benchmark parallel
        MyTree tree(background);
        tree.sparseFill( bbox, 1.0f, /*state*/true);
        openvdb::util::CpuTimer timer("\nparallel voxelizeActiveTiles");
        tree.voxelizeActiveTiles(/*threaded*/true);
        timer.stop();
    }
#endif
}


TEST_F(TestTree, testTopologyUnion)
{
    {//super simple test with only two active values
        const ValueType background=0.0f;
        openvdb::FloatTree tree0(background), tree1(background);
        tree0.setValue(openvdb::Coord( 500, 300, 200), 1.0f);
        tree1.setValue(openvdb::Coord(   8,  11,  11), 2.0f);
        openvdb::FloatTree tree2(tree1);

        tree1.topologyUnion(tree0);

        for (openvdb::FloatTree::ValueOnCIter iter = tree0.cbeginValueOn(); iter; ++iter) {
            EXPECT_TRUE(tree1.isValueOn(iter.getCoord()));
        }
        for (openvdb::FloatTree::ValueOnCIter iter = tree2.cbeginValueOn(); iter; ++iter) {
            EXPECT_TRUE(tree1.isValueOn(iter.getCoord()));
        }
        for (openvdb::FloatTree::ValueOnCIter iter = tree1.cbeginValueOn(); iter; ++iter) {
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter,tree2.getValue(iter.getCoord()));
        }
    }
    {// test using setValue
        ValueType background=5.0f;
        openvdb::FloatTree tree0(background), tree1(background), tree2(background);
        EXPECT_TRUE(tree2.empty());
        // tree0 = tree1.topologyUnion(tree2)
        tree0.setValue(openvdb::Coord( 5, 10, 20),0.0f);
        tree0.setValue(openvdb::Coord(-5, 10, 20),0.1f);
        tree0.setValue(openvdb::Coord( 5,-10, 20),0.2f);
        tree0.setValue(openvdb::Coord( 5, 10,-20),0.3f);
        tree1.setValue(openvdb::Coord( 5, 10, 20),0.0f);
        tree1.setValue(openvdb::Coord(-5, 10, 20),0.1f);
        tree1.setValue(openvdb::Coord( 5,-10, 20),0.2f);
        tree1.setValue(openvdb::Coord( 5, 10,-20),0.3f);

        tree0.setValue(openvdb::Coord(-5,-10, 20),background);
        tree0.setValue(openvdb::Coord(-5, 10,-20),background);
        tree0.setValue(openvdb::Coord( 5,-10,-20),background);
        tree0.setValue(openvdb::Coord(-5,-10,-20),background);
        tree0.setValue(openvdb::Coord(-5000, 2000,-3000),background);
        tree0.setValue(openvdb::Coord( 5000,-2000,-3000),background);
        tree0.setValue(openvdb::Coord(-5000,-2000, 3000),background);
        tree2.setValue(openvdb::Coord(-5,-10, 20),0.4f);
        tree2.setValue(openvdb::Coord(-5, 10,-20),0.5f);
        tree2.setValue(openvdb::Coord( 5,-10,-20),0.6f);
        tree2.setValue(openvdb::Coord(-5,-10,-20),0.7f);
        tree2.setValue(openvdb::Coord(-5000, 2000,-3000),4.5678f);
        tree2.setValue(openvdb::Coord( 5000,-2000,-3000),4.5678f);
        tree2.setValue(openvdb::Coord(-5000,-2000, 3000),4.5678f);

        // tree3 has the same topology as tree2 but a different value type
        const openvdb::Vec3f background2(1.0f,3.4f,6.0f), vec_val(3.1f,5.3f,-9.5f);
        openvdb::Vec3fTree tree3(background2);
        for (openvdb::FloatTree::ValueOnCIter iter2 = tree2.cbeginValueOn(); iter2; ++iter2) {
            tree3.setValue(iter2.getCoord(), vec_val);
        }

        EXPECT_TRUE(tree0.leafCount()!=tree1.leafCount());
        EXPECT_TRUE(tree0.leafCount()!=tree2.leafCount());
        EXPECT_TRUE(tree0.leafCount()!=tree3.leafCount());

        EXPECT_TRUE(!tree2.empty());
        EXPECT_TRUE(!tree3.empty());
        openvdb::FloatTree tree1_copy(tree1);

        //tree1.topologyUnion(tree2);//should make tree1 = tree0
        tree1.topologyUnion(tree3);//should make tree1 = tree0

        EXPECT_TRUE(tree0.leafCount()==tree1.leafCount());
        EXPECT_TRUE(tree0.nonLeafCount()==tree1.nonLeafCount());
        EXPECT_TRUE(tree0.activeLeafVoxelCount()==tree1.activeLeafVoxelCount());
        EXPECT_TRUE(tree0.inactiveLeafVoxelCount()==tree1.inactiveLeafVoxelCount());
        EXPECT_TRUE(tree0.activeVoxelCount()==tree1.activeVoxelCount());
        EXPECT_TRUE(tree0.inactiveVoxelCount()==tree1.inactiveVoxelCount());

        EXPECT_TRUE(tree1.hasSameTopology(tree0));
        EXPECT_TRUE(tree0.hasSameTopology(tree1));

        for (openvdb::FloatTree::ValueOnCIter iter2 = tree2.cbeginValueOn(); iter2; ++iter2) {
            EXPECT_TRUE(tree1.isValueOn(iter2.getCoord()));
        }
        for (openvdb::FloatTree::ValueOnCIter iter1 = tree1.cbeginValueOn(); iter1; ++iter1) {
            EXPECT_TRUE(tree0.isValueOn(iter1.getCoord()));
        }
        for (openvdb::FloatTree::ValueOnCIter iter0 = tree0.cbeginValueOn(); iter0; ++iter0) {
            EXPECT_TRUE(tree1.isValueOn(iter0.getCoord()));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter0,tree1.getValue(iter0.getCoord()));
        }
        for (openvdb::FloatTree::ValueOnCIter iter = tree1_copy.cbeginValueOn(); iter; ++iter) {
            EXPECT_TRUE(tree1.isValueOn(iter.getCoord()));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter,tree1.getValue(iter.getCoord()));
        }
        for (openvdb::FloatTree::ValueOnCIter iter = tree1.cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord p = iter.getCoord();
            EXPECT_TRUE(tree3.isValueOn(p) || tree1_copy.isValueOn(p));
        }
    }
    {
         ValueType background=5.0f;
         openvdb::FloatTree tree0(background), tree1(background), tree2(background);
         EXPECT_TRUE(tree2.empty());
         // tree0 = tree1.topologyUnion(tree2)
         tree0.setValue(openvdb::Coord( 5, 10, 20),0.0f);
         tree0.setValue(openvdb::Coord(-5, 10, 20),0.1f);
         tree0.setValue(openvdb::Coord( 5,-10, 20),0.2f);
         tree0.setValue(openvdb::Coord( 5, 10,-20),0.3f);
         tree1.setValue(openvdb::Coord( 5, 10, 20),0.0f);
         tree1.setValue(openvdb::Coord(-5, 10, 20),0.1f);
         tree1.setValue(openvdb::Coord( 5,-10, 20),0.2f);
         tree1.setValue(openvdb::Coord( 5, 10,-20),0.3f);

         tree0.setValue(openvdb::Coord(-5,-10, 20),background);
         tree0.setValue(openvdb::Coord(-5, 10,-20),background);
         tree0.setValue(openvdb::Coord( 5,-10,-20),background);
         tree0.setValue(openvdb::Coord(-5,-10,-20),background);
         tree0.setValue(openvdb::Coord(-5000, 2000,-3000),background);
         tree0.setValue(openvdb::Coord( 5000,-2000,-3000),background);
         tree0.setValue(openvdb::Coord(-5000,-2000, 3000),background);
         tree2.setValue(openvdb::Coord(-5,-10, 20),0.4f);
         tree2.setValue(openvdb::Coord(-5, 10,-20),0.5f);
         tree2.setValue(openvdb::Coord( 5,-10,-20),0.6f);
         tree2.setValue(openvdb::Coord(-5,-10,-20),0.7f);
         tree2.setValue(openvdb::Coord(-5000, 2000,-3000),4.5678f);
         tree2.setValue(openvdb::Coord( 5000,-2000,-3000),4.5678f);
         tree2.setValue(openvdb::Coord(-5000,-2000, 3000),4.5678f);

         // tree3 has the same topology as tree2 but a different value type
         const openvdb::Vec3f background2(1.0f,3.4f,6.0f), vec_val(3.1f,5.3f,-9.5f);
         openvdb::Vec3fTree tree3(background2);

         for (openvdb::FloatTree::ValueOnCIter iter2 = tree2.cbeginValueOn(); iter2; ++iter2) {
             tree3.setValue(iter2.getCoord(), vec_val);
         }

         openvdb::FloatTree tree4(tree1);//tree4 = tree1
         openvdb::FloatTree tree5(tree1);//tree5 = tree1

         tree1.topologyUnion(tree3);//should make tree1 = tree0

         EXPECT_TRUE(tree1.hasSameTopology(tree0));

         for (openvdb::Vec3fTree::ValueOnCIter iter3 = tree3.cbeginValueOn(); iter3; ++iter3) {
             tree4.setValueOn(iter3.getCoord());
             const openvdb::Coord p = iter3.getCoord();
             ASSERT_DOUBLES_EXACTLY_EQUAL(tree1.getValue(p),tree5.getValue(p));
             ASSERT_DOUBLES_EXACTLY_EQUAL(tree4.getValue(p),tree5.getValue(p));
         }

         EXPECT_TRUE(tree4.hasSameTopology(tree0));

         for (openvdb::FloatTree::ValueOnCIter iter4 = tree4.cbeginValueOn(); iter4; ++iter4) {
             const openvdb::Coord p = iter4.getCoord();
             ASSERT_DOUBLES_EXACTLY_EQUAL(tree0.getValue(p),tree5.getValue(p));
             ASSERT_DOUBLES_EXACTLY_EQUAL(tree1.getValue(p),tree5.getValue(p));
             ASSERT_DOUBLES_EXACTLY_EQUAL(tree4.getValue(p),tree5.getValue(p));
         }

         for (openvdb::FloatTree::ValueOnCIter iter = tree1.cbeginValueOn(); iter; ++iter) {
             const openvdb::Coord p = iter.getCoord();
             EXPECT_TRUE(tree3.isValueOn(p) || tree4.isValueOn(p));
         }
    }
    {// test overlapping spheres
        const float background=5.0f, R0=10.0f, R1=5.6f;
        const openvdb::Vec3f C0(35.0f, 30.0f, 40.0f), C1(22.3f, 30.5f, 31.0f);
        const openvdb::Coord dim(32, 32, 32);
        openvdb::FloatGrid grid0(background);
        openvdb::FloatGrid grid1(background);
        unittest_util::makeSphere<openvdb::FloatGrid>(dim, C0, R0, grid0,
            1.0f, unittest_util::SPHERE_SPARSE_NARROW_BAND);
        unittest_util::makeSphere<openvdb::FloatGrid>(dim, C1, R1, grid1,
            1.0f, unittest_util::SPHERE_SPARSE_NARROW_BAND);
        openvdb::FloatTree& tree0 = grid0.tree();
        openvdb::FloatTree& tree1 = grid1.tree();
        openvdb::FloatTree tree0_copy(tree0);

        tree0.topologyUnion(tree1);

        const openvdb::Index64 n0 = tree0_copy.activeVoxelCount();
        const openvdb::Index64 n  = tree0.activeVoxelCount();
        const openvdb::Index64 n1 = tree1.activeVoxelCount();

        //fprintf(stderr,"Union of spheres: n=%i, n0=%i n1=%i n0+n1=%i\n",n,n0,n1, n0+n1);

        EXPECT_TRUE( n > n0 );
        EXPECT_TRUE( n > n1 );
        EXPECT_TRUE( n < n0 + n1 );

        for (openvdb::FloatTree::ValueOnCIter iter = tree1.cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord p = iter.getCoord();
            EXPECT_TRUE(tree0.isValueOn(p));
            ASSERT_DOUBLES_EXACTLY_EQUAL(tree0.getValue(p), tree0_copy.getValue(p));
        }
        for (openvdb::FloatTree::ValueOnCIter iter = tree0_copy.cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord p = iter.getCoord();
            EXPECT_TRUE(tree0.isValueOn(p));
            ASSERT_DOUBLES_EXACTLY_EQUAL(tree0.getValue(p), *iter);
        }
    }

    {// test union of a leaf and a tile
        if (openvdb::FloatTree::DEPTH > 2) {
            const int leafLevel = openvdb::FloatTree::DEPTH - 1;
            const int tileLevel = leafLevel - 1;
            const openvdb::Coord xyz(0);

            openvdb::FloatTree tree0;
            tree0.addTile(tileLevel, xyz, /*value=*/0, /*activeState=*/true);
            EXPECT_TRUE(tree0.isValueOn(xyz));

            openvdb::FloatTree tree1;
            tree1.touchLeaf(xyz)->setValuesOn();
            EXPECT_TRUE(tree1.isValueOn(xyz));

            tree0.topologyUnion(tree1);
            EXPECT_TRUE(tree0.isValueOn(xyz));
            EXPECT_EQ(tree0.getValueDepth(xyz), leafLevel);
        }
    }

    { // test preservation of source tiles
        using LeafT = openvdb::BoolTree::LeafNodeType;
        using InternalT1 = openvdb::BoolTree::RootNodeType::NodeChainType::Get<1>;
        using InternalT2 = openvdb::BoolTree::RootNodeType::NodeChainType::Get<2>;
        openvdb::BoolTree tree0, tree1;
        const openvdb::Coord xyz(0);

        tree0.addTile(1, xyz, true, true); // leaf level tile
        tree1.touchLeaf(xyz)->setValueOn(0); // single leaf
        tree0.topologyUnion(tree1, true); // single tile
        EXPECT_EQ(openvdb::Index32(0), tree0.leafCount());
        EXPECT_EQ(openvdb::Index32(3), tree0.nonLeafCount());
        EXPECT_EQ(openvdb::Index64(1), tree0.activeTileCount());
        EXPECT_EQ(openvdb::Index64(LeafT::NUM_VOXELS), tree0.activeVoxelCount());

        tree1.addTile(1, xyz + openvdb::Coord(8), true, true); // leaf + tile
        tree0.topologyUnion(tree1, true); // two tiles
        EXPECT_EQ(openvdb::Index32(0), tree0.leafCount());
        EXPECT_EQ(openvdb::Index32(3), tree0.nonLeafCount());
        EXPECT_EQ(openvdb::Index64(2), tree0.activeTileCount());
        EXPECT_EQ(openvdb::Index64(LeafT::NUM_VOXELS*2), tree0.activeVoxelCount());

        // internal node level
        tree0.clear();
        tree0.addTile(2, xyz, true, true);
        tree0.topologyUnion(tree1, true); // all topology in tree1 is already active. no change
        EXPECT_EQ(openvdb::Index32(0), tree0.leafCount());
        EXPECT_EQ(openvdb::Index32(2), tree0.nonLeafCount());
        EXPECT_EQ(openvdb::Index64(1), tree0.activeTileCount());
        EXPECT_EQ(openvdb::Index64(InternalT1::NUM_VOXELS), tree0.activeVoxelCount());

        // internal node level
        tree0.clear();
        tree0.addTile(3, xyz, true, true);
        tree0.topologyUnion(tree1, true);
        EXPECT_EQ(openvdb::Index32(0), tree0.leafCount());
        EXPECT_EQ(openvdb::Index32(1), tree0.nonLeafCount());
        EXPECT_EQ(openvdb::Index64(1), tree0.activeTileCount());
        EXPECT_EQ(openvdb::Index64(InternalT2::NUM_VOXELS), tree0.activeVoxelCount());

        // larger tile in tree1 still forces child topology tree0
        tree0.clear();
        tree1.clear();
        tree0.addTile(1, xyz, true, true);
        tree1.addTile(2, xyz, true, true);
        tree0.topologyUnion(tree1, true);
        EXPECT_EQ(openvdb::Index32(0), tree0.leafCount());
        EXPECT_EQ(openvdb::Index32(3), tree0.nonLeafCount());
        openvdb::Index64 tiles = openvdb::Index64(InternalT1::DIM) / InternalT1::getChildDim();
        tiles = tiles * tiles * tiles;
        EXPECT_EQ(tiles, tree0.activeTileCount());
        EXPECT_EQ(openvdb::Index64(InternalT1::NUM_VOXELS), tree0.activeVoxelCount());
    }
}// testTopologyUnion

TEST_F(TestTree, testTopologyIntersection)
{
    {//no overlapping voxels
        const ValueType background=0.0f;
        openvdb::FloatTree tree0(background), tree1(background);
        tree0.setValue(openvdb::Coord( 500, 300, 200), 1.0f);
        tree1.setValue(openvdb::Coord(   8,  11,  11), 2.0f);
        EXPECT_EQ(openvdb::Index64(1), tree0.activeVoxelCount());
        EXPECT_EQ(openvdb::Index64(1), tree1.activeVoxelCount());

        tree1.topologyIntersection(tree0);

        EXPECT_EQ(tree1.activeVoxelCount(), openvdb::Index64(0));
        EXPECT_TRUE(!tree1.empty());
        openvdb::tools::pruneInactive(tree1);
        EXPECT_TRUE(tree1.empty());
    }
    {//two overlapping voxels
        const ValueType background=0.0f;
        openvdb::FloatTree tree0(background), tree1(background);
        tree0.setValue(openvdb::Coord( 500, 300, 200), 1.0f);

        tree1.setValue(openvdb::Coord(   8,  11,  11), 2.0f);
        tree1.setValue(openvdb::Coord( 500, 300, 200), 1.0f);
        EXPECT_EQ( openvdb::Index64(1), tree0.activeVoxelCount() );
        EXPECT_EQ( openvdb::Index64(2), tree1.activeVoxelCount() );

        tree1.topologyIntersection(tree0);

        EXPECT_EQ( openvdb::Index64(1), tree1.activeVoxelCount() );
        EXPECT_TRUE(!tree1.empty());
        openvdb::tools::pruneInactive(tree1);
        EXPECT_TRUE(!tree1.empty());
    }
    {//4 overlapping voxels
        const ValueType background=0.0f;
        openvdb::FloatTree tree0(background), tree1(background);
        tree0.setValue(openvdb::Coord( 500, 300, 200), 1.0f);
        tree0.setValue(openvdb::Coord( 400,  30,  20), 2.0f);
        tree0.setValue(openvdb::Coord(   8,  11,  11), 3.0f);
        EXPECT_EQ(openvdb::Index64(3), tree0.activeVoxelCount());
        EXPECT_EQ(openvdb::Index32(3), tree0.leafCount() );

        tree1.setValue(openvdb::Coord( 500, 301, 200), 4.0f);
        tree1.setValue(openvdb::Coord( 400,  30,  20), 5.0f);
        tree1.setValue(openvdb::Coord(   8,  11,  11), 6.0f);
        EXPECT_EQ(openvdb::Index64(3), tree1.activeVoxelCount());
        EXPECT_EQ(openvdb::Index32(3), tree1.leafCount() );

        tree1.topologyIntersection(tree0);

        EXPECT_EQ( openvdb::Index32(3), tree1.leafCount() );
        EXPECT_EQ( openvdb::Index64(2), tree1.activeVoxelCount() );
        EXPECT_TRUE(!tree1.empty());
        openvdb::tools::pruneInactive(tree1);
        EXPECT_TRUE(!tree1.empty());
        EXPECT_EQ( openvdb::Index32(2), tree1.leafCount() );
        EXPECT_EQ( openvdb::Index64(2), tree1.activeVoxelCount() );
    }
    {//passive tile
        const ValueType background=0.0f;
        const openvdb::Index64 dim = openvdb::FloatTree::RootNodeType::ChildNodeType::DIM;
        openvdb::FloatTree tree0(background), tree1(background);
        tree0.fill(openvdb::CoordBBox(openvdb::Coord(0),openvdb::Coord(dim-1)),2.0f, false);
        EXPECT_EQ(openvdb::Index64(0), tree0.activeVoxelCount());
        EXPECT_EQ(openvdb::Index32(0), tree0.leafCount() );

        tree1.setValue(openvdb::Coord( 500, 301, 200), 4.0f);
        tree1.setValue(openvdb::Coord( 400,  30,  20), 5.0f);
        tree1.setValue(openvdb::Coord( dim,  11,  11), 6.0f);
        EXPECT_EQ(openvdb::Index32(3), tree1.leafCount() );
        EXPECT_EQ(openvdb::Index64(3), tree1.activeVoxelCount());

        tree1.topologyIntersection(tree0);

        EXPECT_EQ( openvdb::Index32(0), tree1.leafCount() );
        EXPECT_EQ( openvdb::Index64(0), tree1.activeVoxelCount() );
        EXPECT_TRUE(tree1.empty());
    }
    {//active tile
        const ValueType background=0.0f;
        const openvdb::Index64 dim = openvdb::FloatTree::RootNodeType::ChildNodeType::DIM;
        openvdb::FloatTree tree0(background), tree1(background);
        tree1.fill(openvdb::CoordBBox(openvdb::Coord(0),openvdb::Coord(dim-1)),2.0f, true);
        EXPECT_EQ(dim*dim*dim, tree1.activeVoxelCount());
        EXPECT_EQ(openvdb::Index32(0), tree1.leafCount() );

        tree0.setValue(openvdb::Coord( 500, 301, 200), 4.0f);
        tree0.setValue(openvdb::Coord( 400,  30,  20), 5.0f);
        tree0.setValue(openvdb::Coord( dim,  11,  11), 6.0f);
        EXPECT_EQ(openvdb::Index64(3), tree0.activeVoxelCount());
        EXPECT_EQ(openvdb::Index32(3), tree0.leafCount() );

        tree1.topologyIntersection(tree0);

        EXPECT_EQ( openvdb::Index32(2), tree1.leafCount() );
        EXPECT_EQ( openvdb::Index64(2), tree1.activeVoxelCount() );
        EXPECT_TRUE(!tree1.empty());
        openvdb::tools::pruneInactive(tree1);
        EXPECT_TRUE(!tree1.empty());
    }
    {// use tree with different voxel type
        ValueType background=5.0f;
        openvdb::FloatTree tree0(background), tree1(background), tree2(background);
        EXPECT_TRUE(tree2.empty());
        // tree0 = tree1.topologyIntersection(tree2)
        tree0.setValue(openvdb::Coord( 5, 10, 20),0.0f);
        tree0.setValue(openvdb::Coord(-5, 10,-20),0.1f);
        tree0.setValue(openvdb::Coord( 5,-10,-20),0.2f);
        tree0.setValue(openvdb::Coord(-5,-10,-20),0.3f);

        tree1.setValue(openvdb::Coord( 5, 10, 20),0.0f);
        tree1.setValue(openvdb::Coord(-5, 10,-20),0.1f);
        tree1.setValue(openvdb::Coord( 5,-10,-20),0.2f);
        tree1.setValue(openvdb::Coord(-5,-10,-20),0.3f);

        tree2.setValue(openvdb::Coord( 5, 10, 20),0.4f);
        tree2.setValue(openvdb::Coord(-5, 10,-20),0.5f);
        tree2.setValue(openvdb::Coord( 5,-10,-20),0.6f);
        tree2.setValue(openvdb::Coord(-5,-10,-20),0.7f);

        tree2.setValue(openvdb::Coord(-5000, 2000,-3000),4.5678f);
        tree2.setValue(openvdb::Coord( 5000,-2000,-3000),4.5678f);
        tree2.setValue(openvdb::Coord(-5000,-2000, 3000),4.5678f);

        openvdb::FloatTree tree1_copy(tree1);

        // tree3 has the same topology as tree2 but a different value type
        const openvdb::Vec3f background2(1.0f,3.4f,6.0f), vec_val(3.1f,5.3f,-9.5f);
        openvdb::Vec3fTree tree3(background2);
        for (openvdb::FloatTree::ValueOnCIter iter = tree2.cbeginValueOn(); iter; ++iter) {
            tree3.setValue(iter.getCoord(), vec_val);
        }

        EXPECT_EQ(openvdb::Index32(4), tree0.leafCount());
        EXPECT_EQ(openvdb::Index32(4), tree1.leafCount());
        EXPECT_EQ(openvdb::Index32(7), tree2.leafCount());
        EXPECT_EQ(openvdb::Index32(7), tree3.leafCount());


        //tree1.topologyInterection(tree2);//should make tree1 = tree0
        tree1.topologyIntersection(tree3);//should make tree1 = tree0

        EXPECT_TRUE(tree0.leafCount()==tree1.leafCount());
        EXPECT_TRUE(tree0.nonLeafCount()==tree1.nonLeafCount());
        EXPECT_TRUE(tree0.activeLeafVoxelCount()==tree1.activeLeafVoxelCount());
        EXPECT_TRUE(tree0.inactiveLeafVoxelCount()==tree1.inactiveLeafVoxelCount());
        EXPECT_TRUE(tree0.activeVoxelCount()==tree1.activeVoxelCount());
        EXPECT_TRUE(tree0.inactiveVoxelCount()==tree1.inactiveVoxelCount());
        EXPECT_TRUE(tree1.hasSameTopology(tree0));
        EXPECT_TRUE(tree0.hasSameTopology(tree1));

        for (openvdb::FloatTree::ValueOnCIter iter = tree0.cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord p = iter.getCoord();
            EXPECT_TRUE(tree1.isValueOn(p));
            EXPECT_TRUE(tree2.isValueOn(p));
            EXPECT_TRUE(tree3.isValueOn(p));
            EXPECT_TRUE(tree1_copy.isValueOn(p));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter,tree1.getValue(p));
        }
        for (openvdb::FloatTree::ValueOnCIter iter = tree1_copy.cbeginValueOn(); iter; ++iter) {
            EXPECT_TRUE(tree1.isValueOn(iter.getCoord()));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter,tree1.getValue(iter.getCoord()));
        }
        for (openvdb::FloatTree::ValueOnCIter iter = tree1.cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord p = iter.getCoord();
            EXPECT_TRUE(tree0.isValueOn(p));
            EXPECT_TRUE(tree2.isValueOn(p));
            EXPECT_TRUE(tree3.isValueOn(p));
            EXPECT_TRUE(tree1_copy.isValueOn(p));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter,tree0.getValue(p));
        }
    }

    {// test overlapping spheres
        const float background=5.0f, R0=10.0f, R1=5.6f;
        const openvdb::Vec3f C0(35.0f, 30.0f, 40.0f), C1(22.3f, 30.5f, 31.0f);
        const openvdb::Coord dim(32, 32, 32);
        openvdb::FloatGrid grid0(background);
        openvdb::FloatGrid grid1(background);
        unittest_util::makeSphere<openvdb::FloatGrid>(dim, C0, R0, grid0,
            1.0f, unittest_util::SPHERE_SPARSE_NARROW_BAND);
        unittest_util::makeSphere<openvdb::FloatGrid>(dim, C1, R1, grid1,
            1.0f, unittest_util::SPHERE_SPARSE_NARROW_BAND);
        openvdb::FloatTree& tree0 = grid0.tree();
        openvdb::FloatTree& tree1 = grid1.tree();
        openvdb::FloatTree tree0_copy(tree0);

        tree0.topologyIntersection(tree1);

        const openvdb::Index64 n0 = tree0_copy.activeVoxelCount();
        const openvdb::Index64 n  = tree0.activeVoxelCount();
        const openvdb::Index64 n1 = tree1.activeVoxelCount();

        //fprintf(stderr,"Intersection of spheres: n=%i, n0=%i n1=%i n0+n1=%i\n",n,n0,n1, n0+n1);

        EXPECT_TRUE( n < n0 );
        EXPECT_TRUE( n < n1 );

        for (openvdb::FloatTree::ValueOnCIter iter = tree0.cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord p = iter.getCoord();
            EXPECT_TRUE(tree1.isValueOn(p));
            EXPECT_TRUE(tree0_copy.isValueOn(p));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter, tree0_copy.getValue(p));
        }
    }

    {// Test based on boolean grids
        openvdb::CoordBBox bigRegion(openvdb::Coord(-9), openvdb::Coord(10));
        openvdb::CoordBBox smallRegion(openvdb::Coord( 1), openvdb::Coord(10));

        openvdb::BoolGrid::Ptr gridBig = openvdb::BoolGrid::create(false);
        gridBig->fill(bigRegion, true/*value*/, true /*make active*/);
        EXPECT_EQ(8, int(gridBig->tree().activeTileCount()));
        EXPECT_EQ((20 * 20 * 20), int(gridBig->activeVoxelCount()));

        openvdb::BoolGrid::Ptr gridSmall = openvdb::BoolGrid::create(false);
        gridSmall->fill(smallRegion, true/*value*/, true /*make active*/);
        EXPECT_EQ(0, int(gridSmall->tree().activeTileCount()));
        EXPECT_EQ((10 * 10 * 10), int(gridSmall->activeVoxelCount()));

        // change the topology of gridBig by intersecting with gridSmall
        gridBig->topologyIntersection(*gridSmall);

        // Should be unchanged
        EXPECT_EQ(0, int(gridSmall->tree().activeTileCount()));
        EXPECT_EQ((10 * 10 * 10), int(gridSmall->activeVoxelCount()));

        // In this case the interesection should be exactly "small"
        EXPECT_EQ(0, int(gridBig->tree().activeTileCount()));
        EXPECT_EQ((10 * 10 * 10), int(gridBig->activeVoxelCount()));

    }

}// testTopologyIntersection

TEST_F(TestTree, testTopologyDifference)
{
    {//no overlapping voxels
        const ValueType background=0.0f;
        openvdb::FloatTree tree0(background), tree1(background);
        tree0.setValue(openvdb::Coord( 500, 300, 200), 1.0f);
        tree1.setValue(openvdb::Coord(   8,  11,  11), 2.0f);
        EXPECT_EQ(openvdb::Index64(1), tree0.activeVoxelCount());
        EXPECT_EQ(openvdb::Index64(1), tree1.activeVoxelCount());

        tree1.topologyDifference(tree0);

        EXPECT_EQ(tree1.activeVoxelCount(), openvdb::Index64(1));
        EXPECT_TRUE(!tree1.empty());
        openvdb::tools::pruneInactive(tree1);
        EXPECT_TRUE(!tree1.empty());
    }
    {//two overlapping voxels
        const ValueType background=0.0f;
        openvdb::FloatTree tree0(background), tree1(background);
        tree0.setValue(openvdb::Coord( 500, 300, 200), 1.0f);

        tree1.setValue(openvdb::Coord(   8,  11,  11), 2.0f);
        tree1.setValue(openvdb::Coord( 500, 300, 200), 1.0f);
        EXPECT_EQ( openvdb::Index64(1), tree0.activeVoxelCount() );
        EXPECT_EQ( openvdb::Index64(2), tree1.activeVoxelCount() );

        EXPECT_TRUE( tree0.isValueOn(openvdb::Coord( 500, 300, 200)));
        EXPECT_TRUE( tree1.isValueOn(openvdb::Coord( 500, 300, 200)));
        EXPECT_TRUE( tree1.isValueOn(openvdb::Coord(   8,  11,  11)));

        tree1.topologyDifference(tree0);

        EXPECT_EQ( openvdb::Index64(1), tree1.activeVoxelCount() );
        EXPECT_TRUE( tree0.isValueOn(openvdb::Coord( 500, 300, 200)));
        EXPECT_TRUE(!tree1.isValueOn(openvdb::Coord( 500, 300, 200)));
        EXPECT_TRUE( tree1.isValueOn(openvdb::Coord(   8,  11,  11)));

        EXPECT_TRUE(!tree1.empty());
        openvdb::tools::pruneInactive(tree1);
        EXPECT_TRUE(!tree1.empty());
    }
    {//4 overlapping voxels
        const ValueType background=0.0f;
        openvdb::FloatTree tree0(background), tree1(background);
        tree0.setValue(openvdb::Coord( 500, 300, 200), 1.0f);
        tree0.setValue(openvdb::Coord( 400,  30,  20), 2.0f);
        tree0.setValue(openvdb::Coord(   8,  11,  11), 3.0f);
        EXPECT_EQ(openvdb::Index64(3), tree0.activeVoxelCount());
        EXPECT_EQ(openvdb::Index32(3), tree0.leafCount() );

        tree1.setValue(openvdb::Coord( 500, 301, 200), 4.0f);
        tree1.setValue(openvdb::Coord( 400,  30,  20), 5.0f);
        tree1.setValue(openvdb::Coord(   8,  11,  11), 6.0f);
        EXPECT_EQ(openvdb::Index64(3), tree1.activeVoxelCount());
        EXPECT_EQ(openvdb::Index32(3), tree1.leafCount() );

        tree1.topologyDifference(tree0);

        EXPECT_EQ( openvdb::Index32(3), tree1.leafCount() );
        EXPECT_EQ( openvdb::Index64(1), tree1.activeVoxelCount() );
        EXPECT_TRUE(!tree1.empty());
        openvdb::tools::pruneInactive(tree1);
        EXPECT_TRUE(!tree1.empty());
        EXPECT_EQ( openvdb::Index32(1), tree1.leafCount() );
        EXPECT_EQ( openvdb::Index64(1), tree1.activeVoxelCount() );
    }
    {//passive tile
        const ValueType background=0.0f;
        const openvdb::Index64 dim = openvdb::FloatTree::RootNodeType::ChildNodeType::DIM;
        openvdb::FloatTree tree0(background), tree1(background);
        tree0.fill(openvdb::CoordBBox(openvdb::Coord(0),openvdb::Coord(dim-1)),2.0f, false);
        EXPECT_EQ(openvdb::Index64(0), tree0.activeVoxelCount());
        EXPECT_TRUE(!tree0.hasActiveTiles());
        EXPECT_EQ(openvdb::Index64(0), tree0.root().onTileCount());
        EXPECT_EQ(openvdb::Index32(0), tree0.leafCount() );

        tree1.setValue(openvdb::Coord( 500, 301, 200), 4.0f);
        tree1.setValue(openvdb::Coord( 400,  30,  20), 5.0f);
        tree1.setValue(openvdb::Coord( dim,  11,  11), 6.0f);
        EXPECT_EQ(openvdb::Index64(3), tree1.activeVoxelCount());
        EXPECT_TRUE(!tree1.hasActiveTiles());
        EXPECT_EQ(openvdb::Index32(3), tree1.leafCount() );

        tree1.topologyDifference(tree0);

        EXPECT_EQ( openvdb::Index32(3), tree1.leafCount() );
        EXPECT_EQ( openvdb::Index64(3), tree1.activeVoxelCount() );
        EXPECT_TRUE(!tree1.empty());
        openvdb::tools::pruneInactive(tree1);
        EXPECT_EQ( openvdb::Index32(3), tree1.leafCount() );
        EXPECT_EQ( openvdb::Index64(3), tree1.activeVoxelCount() );
        EXPECT_TRUE(!tree1.empty());
    }
    {//active tile
        const ValueType background=0.0f;
        const openvdb::Index64 dim = openvdb::FloatTree::RootNodeType::ChildNodeType::DIM;
        openvdb::FloatTree tree0(background), tree1(background);
        tree1.fill(openvdb::CoordBBox(openvdb::Coord(0),openvdb::Coord(dim-1)),2.0f, true);
        EXPECT_EQ(dim*dim*dim, tree1.activeVoxelCount());
        EXPECT_TRUE(tree1.hasActiveTiles());
        EXPECT_EQ(openvdb::Index64(1), tree1.root().onTileCount());
        EXPECT_EQ(openvdb::Index32(0), tree0.leafCount() );

        tree0.setValue(openvdb::Coord( 500, 301, 200), 4.0f);
        tree0.setValue(openvdb::Coord( 400,  30,  20), 5.0f);
        tree0.setValue(openvdb::Coord( int(dim),  11,  11), 6.0f);
        EXPECT_TRUE(!tree0.hasActiveTiles());
        EXPECT_EQ(openvdb::Index64(3), tree0.activeVoxelCount());
        EXPECT_EQ(openvdb::Index32(3), tree0.leafCount() );
        EXPECT_TRUE( tree0.isValueOn(openvdb::Coord( int(dim),  11,  11)));
        EXPECT_TRUE(!tree1.isValueOn(openvdb::Coord( int(dim),  11,  11)));

        tree1.topologyDifference(tree0);

        EXPECT_TRUE(tree1.root().onTileCount() > 1);
        EXPECT_EQ( dim*dim*dim - 2, tree1.activeVoxelCount() );
        EXPECT_TRUE(!tree1.empty());
        openvdb::tools::pruneInactive(tree1);
        EXPECT_EQ( dim*dim*dim - 2, tree1.activeVoxelCount() );
        EXPECT_TRUE(!tree1.empty());
    }
    {//active tile
        const ValueType background=0.0f;
        const openvdb::Index64 dim = openvdb::FloatTree::RootNodeType::ChildNodeType::DIM;
        openvdb::FloatTree tree0(background), tree1(background);
        tree1.fill(openvdb::CoordBBox(openvdb::Coord(0),openvdb::Coord(dim-1)),2.0f, true);
        EXPECT_EQ(dim*dim*dim, tree1.activeVoxelCount());
        EXPECT_TRUE(tree1.hasActiveTiles());
        EXPECT_EQ(openvdb::Index64(1), tree1.root().onTileCount());
        EXPECT_EQ(openvdb::Index32(0), tree0.leafCount() );

        tree0.setValue(openvdb::Coord( 500, 301, 200), 4.0f);
        tree0.setValue(openvdb::Coord( 400,  30,  20), 5.0f);
        tree0.setValue(openvdb::Coord( dim,  11,  11), 6.0f);
        EXPECT_TRUE(!tree0.hasActiveTiles());
        EXPECT_EQ(openvdb::Index64(3), tree0.activeVoxelCount());
        EXPECT_EQ(openvdb::Index32(3), tree0.leafCount() );

        tree0.topologyDifference(tree1);

        EXPECT_EQ( openvdb::Index32(1), tree0.leafCount() );
        EXPECT_EQ( openvdb::Index64(1), tree0.activeVoxelCount() );
        EXPECT_TRUE(!tree0.empty());
        openvdb::tools::pruneInactive(tree0);
        EXPECT_EQ( openvdb::Index32(1), tree0.leafCount() );
        EXPECT_EQ( openvdb::Index64(1), tree0.activeVoxelCount() );
        EXPECT_TRUE(!tree1.empty());
    }
    {// use tree with different voxel type
        ValueType background=5.0f;
        openvdb::FloatTree tree0(background), tree1(background), tree2(background);
        EXPECT_TRUE(tree2.empty());
        // tree0 = tree1.topologyIntersection(tree2)
        tree0.setValue(openvdb::Coord( 5, 10, 20),0.0f);
        tree0.setValue(openvdb::Coord(-5, 10,-20),0.1f);
        tree0.setValue(openvdb::Coord( 5,-10,-20),0.2f);
        tree0.setValue(openvdb::Coord(-5,-10,-20),0.3f);

        tree1.setValue(openvdb::Coord( 5, 10, 20),0.0f);
        tree1.setValue(openvdb::Coord(-5, 10,-20),0.1f);
        tree1.setValue(openvdb::Coord( 5,-10,-20),0.2f);
        tree1.setValue(openvdb::Coord(-5,-10,-20),0.3f);

        tree2.setValue(openvdb::Coord( 5, 10, 20),0.4f);
        tree2.setValue(openvdb::Coord(-5, 10,-20),0.5f);
        tree2.setValue(openvdb::Coord( 5,-10,-20),0.6f);
        tree2.setValue(openvdb::Coord(-5,-10,-20),0.7f);

        tree2.setValue(openvdb::Coord(-5000, 2000,-3000),4.5678f);
        tree2.setValue(openvdb::Coord( 5000,-2000,-3000),4.5678f);
        tree2.setValue(openvdb::Coord(-5000,-2000, 3000),4.5678f);

        openvdb::FloatTree tree1_copy(tree1);

        // tree3 has the same topology as tree2 but a different value type
        const openvdb::Vec3f background2(1.0f,3.4f,6.0f), vec_val(3.1f,5.3f,-9.5f);
        openvdb::Vec3fTree tree3(background2);
        for (openvdb::FloatTree::ValueOnCIter iter = tree2.cbeginValueOn(); iter; ++iter) {
            tree3.setValue(iter.getCoord(), vec_val);
        }

        EXPECT_EQ(openvdb::Index32(4), tree0.leafCount());
        EXPECT_EQ(openvdb::Index32(4), tree1.leafCount());
        EXPECT_EQ(openvdb::Index32(7), tree2.leafCount());
        EXPECT_EQ(openvdb::Index32(7), tree3.leafCount());


        //tree1.topologyInterection(tree2);//should make tree1 = tree0
        tree1.topologyIntersection(tree3);//should make tree1 = tree0

        EXPECT_TRUE(tree0.leafCount()==tree1.leafCount());
        EXPECT_TRUE(tree0.nonLeafCount()==tree1.nonLeafCount());
        EXPECT_TRUE(tree0.activeLeafVoxelCount()==tree1.activeLeafVoxelCount());
        EXPECT_TRUE(tree0.inactiveLeafVoxelCount()==tree1.inactiveLeafVoxelCount());
        EXPECT_TRUE(tree0.activeVoxelCount()==tree1.activeVoxelCount());
        EXPECT_TRUE(tree0.inactiveVoxelCount()==tree1.inactiveVoxelCount());
        EXPECT_TRUE(tree1.hasSameTopology(tree0));
        EXPECT_TRUE(tree0.hasSameTopology(tree1));

        for (openvdb::FloatTree::ValueOnCIter iter = tree0.cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord p = iter.getCoord();
            EXPECT_TRUE(tree1.isValueOn(p));
            EXPECT_TRUE(tree2.isValueOn(p));
            EXPECT_TRUE(tree3.isValueOn(p));
            EXPECT_TRUE(tree1_copy.isValueOn(p));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter,tree1.getValue(p));
        }
        for (openvdb::FloatTree::ValueOnCIter iter = tree1_copy.cbeginValueOn(); iter; ++iter) {
            EXPECT_TRUE(tree1.isValueOn(iter.getCoord()));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter,tree1.getValue(iter.getCoord()));
        }
        for (openvdb::FloatTree::ValueOnCIter iter = tree1.cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord p = iter.getCoord();
            EXPECT_TRUE(tree0.isValueOn(p));
            EXPECT_TRUE(tree2.isValueOn(p));
            EXPECT_TRUE(tree3.isValueOn(p));
            EXPECT_TRUE(tree1_copy.isValueOn(p));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter,tree0.getValue(p));
        }
    }
    {// test overlapping spheres
        const float background=5.0f, R0=10.0f, R1=5.6f;
        const openvdb::Vec3f C0(35.0f, 30.0f, 40.0f), C1(22.3f, 30.5f, 31.0f);
        const openvdb::Coord dim(32, 32, 32);
        openvdb::FloatGrid grid0(background);
        openvdb::FloatGrid grid1(background);
        unittest_util::makeSphere<openvdb::FloatGrid>(dim, C0, R0, grid0,
            1.0f, unittest_util::SPHERE_SPARSE_NARROW_BAND);
        unittest_util::makeSphere<openvdb::FloatGrid>(dim, C1, R1, grid1,
            1.0f, unittest_util::SPHERE_SPARSE_NARROW_BAND);
        openvdb::FloatTree& tree0 = grid0.tree();
        openvdb::FloatTree& tree1 = grid1.tree();
        openvdb::FloatTree tree0_copy(tree0);

        tree0.topologyDifference(tree1);

        const openvdb::Index64 n0 = tree0_copy.activeVoxelCount();
        const openvdb::Index64 n  = tree0.activeVoxelCount();

        EXPECT_TRUE( n < n0 );

        for (openvdb::FloatTree::ValueOnCIter iter = tree0.cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord p = iter.getCoord();
            EXPECT_TRUE(tree1.isValueOff(p));
            EXPECT_TRUE(tree0_copy.isValueOn(p));
            ASSERT_DOUBLES_EXACTLY_EQUAL(*iter, tree0_copy.getValue(p));
        }
    }
} // testTopologyDifference


////////////////////////////////////////


TEST_F(TestTree, testFill)
{
    // Use a custom tree configuration to ensure we flood-fill at all levels!
    using LeafT = openvdb::tree::LeafNode<float,2>;//4^3
    using InternalT = openvdb::tree::InternalNode<LeafT,2>;//4^3
    using RootT = openvdb::tree::RootNode<InternalT>;// child nodes are 16^3
    using TreeT = openvdb::tree::Tree<RootT>;

    const float outside = 2.0f, inside = -outside;
    const openvdb::CoordBBox
        bbox{openvdb::Coord{-3, -50, 30}, openvdb::Coord{13, 11, 323}},
        otherBBox{openvdb::Coord{400, 401, 402}, openvdb::Coord{600}};

    {// sparse fill
         openvdb::Grid<TreeT>::Ptr grid = openvdb::Grid<TreeT>::create(outside);
         TreeT& tree = grid->tree();
         EXPECT_TRUE(!tree.hasActiveTiles());
         EXPECT_EQ(openvdb::Index64(0), tree.activeVoxelCount());
         for (openvdb::CoordBBox::Iterator<true> ijk(bbox); ijk; ++ijk) {
             ASSERT_DOUBLES_EXACTLY_EQUAL(outside, tree.getValue(*ijk));
         }
         tree.sparseFill(bbox, inside, /*active=*/true);
         EXPECT_TRUE(tree.hasActiveTiles());
         EXPECT_EQ(openvdb::Index64(bbox.volume()), tree.activeVoxelCount());
          for (openvdb::CoordBBox::Iterator<true> ijk(bbox); ijk; ++ijk) {
             ASSERT_DOUBLES_EXACTLY_EQUAL(inside, tree.getValue(*ijk));
         }
    }
    {// dense fill
         openvdb::Grid<TreeT>::Ptr grid = openvdb::Grid<TreeT>::create(outside);
         TreeT& tree = grid->tree();
         EXPECT_TRUE(!tree.hasActiveTiles());
         EXPECT_EQ(openvdb::Index64(0), tree.activeVoxelCount());
         for (openvdb::CoordBBox::Iterator<true> ijk(bbox); ijk; ++ijk) {
             ASSERT_DOUBLES_EXACTLY_EQUAL(outside, tree.getValue(*ijk));
         }

         // Add some active tiles.
         tree.sparseFill(otherBBox, inside, /*active=*/true);
         EXPECT_TRUE(tree.hasActiveTiles());
         EXPECT_EQ(otherBBox.volume(), tree.activeVoxelCount());

         tree.denseFill(bbox, inside, /*active=*/true);

         // In OpenVDB 4.0.0 and earlier, denseFill() densified active tiles
         // throughout the tree.  Verify that it no longer does that.
         EXPECT_TRUE(tree.hasActiveTiles()); // i.e., otherBBox

         EXPECT_EQ(bbox.volume() + otherBBox.volume(), tree.activeVoxelCount());
         for (openvdb::CoordBBox::Iterator<true> ijk(bbox); ijk; ++ijk) {
             ASSERT_DOUBLES_EXACTLY_EQUAL(inside, tree.getValue(*ijk));
         }

         tree.clear();
         EXPECT_TRUE(!tree.hasActiveTiles());
         tree.sparseFill(otherBBox, inside, /*active=*/true);
         EXPECT_TRUE(tree.hasActiveTiles());
         tree.denseFill(bbox, inside, /*active=*/false);
         EXPECT_TRUE(tree.hasActiveTiles()); // i.e., otherBBox
         EXPECT_EQ(otherBBox.volume(), tree.activeVoxelCount());

         // In OpenVDB 4.0.0 and earlier, denseFill() filled sparsely if given
         // an inactive fill value.  Verify that it now fills densely.
         const int leafDepth = int(tree.treeDepth()) - 1;
         for (openvdb::CoordBBox::Iterator<true> ijk(bbox); ijk; ++ijk) {
             EXPECT_EQ(leafDepth, tree.getValueDepth(*ijk));
             ASSERT_DOUBLES_EXACTLY_EQUAL(inside, tree.getValue(*ijk));
         }
    }

}// testFill

TEST_F(TestTree, testSignedFloodFill)
{
    // Use a custom tree configuration to ensure we flood-fill at all levels!
    using LeafT = openvdb::tree::LeafNode<float,2>;//4^3
    using InternalT = openvdb::tree::InternalNode<LeafT,2>;//4^3
    using RootT = openvdb::tree::RootNode<InternalT>;// child nodes are 16^3
    using TreeT = openvdb::tree::Tree<RootT>;

    const float outside = 2.0f, inside = -outside, radius = 20.0f;

    {//first test flood filling of a leaf node

        const LeafT::ValueType fill0=5, fill1=-fill0;
        openvdb::tools::SignedFloodFillOp<TreeT> sff(fill0, fill1);

        int D = LeafT::dim(), C=D/2;
        openvdb::Coord origin(0,0,0), left(0,0,C-1), right(0,0,C);
        LeafT leaf(origin,fill0);
        for (int i=0; i<D; ++i) {
            left[0]=right[0]=i;
            for (int j=0; j<D; ++j) {
                left[1]=right[1]=j;
                leaf.setValueOn(left,fill0);
                leaf.setValueOn(right,fill1);
            }
        }
        const openvdb::Coord first(0,0,0), last(D-1,D-1,D-1);
        EXPECT_TRUE(!leaf.isValueOn(first));
        EXPECT_TRUE(!leaf.isValueOn(last));
        EXPECT_EQ(fill0, leaf.getValue(first));
        EXPECT_EQ(fill0, leaf.getValue(last));

        sff(leaf);

        EXPECT_TRUE(!leaf.isValueOn(first));
        EXPECT_TRUE(!leaf.isValueOn(last));
        EXPECT_EQ(fill0, leaf.getValue(first));
        EXPECT_EQ(fill1, leaf.getValue(last));
    }

    openvdb::Grid<TreeT>::Ptr grid = openvdb::Grid<TreeT>::create(outside);
    TreeT& tree = grid->tree();
    const RootT& root = tree.root();
    const openvdb::Coord dim(3*16, 3*16, 3*16);
    const openvdb::Coord C(16+8,16+8,16+8);

    EXPECT_TRUE(!tree.isValueOn(C));
    EXPECT_TRUE(root.getTableSize()==0);

    //make narrow band of sphere without setting sign for the background values!
    openvdb::Grid<TreeT>::Accessor acc = grid->getAccessor();
    const openvdb::Vec3f center(static_cast<float>(C[0]),
                                static_cast<float>(C[1]),
                                static_cast<float>(C[2]));
    openvdb::Coord xyz;
    for (xyz[0]=0; xyz[0]<dim[0]; ++xyz[0]) {
        for (xyz[1]=0; xyz[1]<dim[1]; ++xyz[1]) {
            for (xyz[2]=0; xyz[2]<dim[2]; ++xyz[2]) {
                const openvdb::Vec3R p =  grid->transform().indexToWorld(xyz);
                const float dist = float((p-center).length() - radius);
                if (fabs(dist) > outside) continue;
                acc.setValue(xyz, dist);
            }
        }
    }
    // Check narrow band with incorrect background
    const size_t size_before = root.getTableSize();
    EXPECT_TRUE(size_before>0);
    EXPECT_TRUE(!tree.isValueOn(C));
    ASSERT_DOUBLES_EXACTLY_EQUAL(outside,tree.getValue(C));
    for (xyz[0]=0; xyz[0]<dim[0]; ++xyz[0]) {
        for (xyz[1]=0; xyz[1]<dim[1]; ++xyz[1]) {
            for (xyz[2]=0; xyz[2]<dim[2]; ++xyz[2]) {
                const openvdb::Vec3R p =  grid->transform().indexToWorld(xyz);
                const float dist = float((p-center).length() - radius);
                const float val  =  acc.getValue(xyz);
                if (dist < inside) {
                    ASSERT_DOUBLES_EXACTLY_EQUAL( val, outside);
                } else if (dist>outside) {
                    ASSERT_DOUBLES_EXACTLY_EQUAL( val, outside);
                } else {
                    ASSERT_DOUBLES_EXACTLY_EQUAL( val, dist   );
                }
            }
        }
    }

    EXPECT_TRUE(tree.getValueDepth(C) == -1);//i.e. background value
    openvdb::tools::signedFloodFill(tree);
    EXPECT_TRUE(tree.getValueDepth(C) ==  0);//added inside tile to root

    // Check narrow band with correct background
    for (xyz[0]=0; xyz[0]<dim[0]; ++xyz[0]) {
        for (xyz[1]=0; xyz[1]<dim[1]; ++xyz[1]) {
            for (xyz[2]=0; xyz[2]<dim[2]; ++xyz[2]) {
                const openvdb::Vec3R p =  grid->transform().indexToWorld(xyz);
                const float dist = float((p-center).length() - radius);
                const float val  =  acc.getValue(xyz);
                if (dist < inside) {
                    ASSERT_DOUBLES_EXACTLY_EQUAL( val, inside);
                } else if (dist>outside) {
                    ASSERT_DOUBLES_EXACTLY_EQUAL( val, outside);
                } else {
                    ASSERT_DOUBLES_EXACTLY_EQUAL( val, dist   );
                }
            }
        }
    }

    EXPECT_TRUE(root.getTableSize()>size_before);//added inside root tiles
    EXPECT_TRUE(!tree.isValueOn(C));
    ASSERT_DOUBLES_EXACTLY_EQUAL(inside,tree.getValue(C));
}


TEST_F(TestTree, testPruneInactive)
{
    using openvdb::Coord;
    using openvdb::Index32;
    using openvdb::Index64;

    const float background = 5.0;

    openvdb::FloatTree tree(background);

    // Verify that the newly-constructed tree is empty and that pruning it has no effect.
    EXPECT_TRUE(tree.empty());
    openvdb::tools::prune(tree);
    EXPECT_TRUE(tree.empty());
    openvdb::tools::pruneInactive(tree);
    EXPECT_TRUE(tree.empty());

    // Set some active values.
    tree.setValue(Coord(-5, 10, 20), 0.1f);
    tree.setValue(Coord(-5,-10, 20), 0.4f);
    tree.setValue(Coord(-5, 10,-20), 0.5f);
    tree.setValue(Coord(-5,-10,-20), 0.7f);
    tree.setValue(Coord( 5, 10, 20), 0.0f);
    tree.setValue(Coord( 5,-10, 20), 0.2f);
    tree.setValue(Coord( 5,-10,-20), 0.6f);
    tree.setValue(Coord( 5, 10,-20), 0.3f);
    // Verify that the tree has the expected numbers of active voxels and leaf nodes.
    EXPECT_EQ(Index64(8), tree.activeVoxelCount());
    EXPECT_EQ(Index32(8), tree.leafCount());

    // Verify that prune() has no effect, since the values are all different.
    openvdb::tools::prune(tree);
    EXPECT_EQ(Index64(8), tree.activeVoxelCount());
    EXPECT_EQ(Index32(8), tree.leafCount());
    // Verify that pruneInactive() has no effect, since the values are active.
    openvdb::tools::pruneInactive(tree);
    EXPECT_EQ(Index64(8), tree.activeVoxelCount());
    EXPECT_EQ(Index32(8), tree.leafCount());

    // Make some of the active values inactive, without changing their values.
    tree.setValueOff(Coord(-5, 10, 20));
    tree.setValueOff(Coord(-5,-10, 20));
    tree.setValueOff(Coord(-5, 10,-20));
    tree.setValueOff(Coord(-5,-10,-20));
    EXPECT_EQ(Index64(4), tree.activeVoxelCount());
    EXPECT_EQ(Index32(8), tree.leafCount());
    // Verify that prune() has no effect, since the values are still different.
    openvdb::tools::prune(tree);
    EXPECT_EQ(Index64(4), tree.activeVoxelCount());
    EXPECT_EQ(Index32(8), tree.leafCount());
    // Verify that pruneInactive() prunes the nodes containing only inactive voxels.
    openvdb::tools::pruneInactive(tree);
    EXPECT_EQ(Index64(4), tree.activeVoxelCount());
    EXPECT_EQ(Index32(4), tree.leafCount());

    // Make all of the active values inactive, without changing their values.
    tree.setValueOff(Coord( 5, 10, 20));
    tree.setValueOff(Coord( 5,-10, 20));
    tree.setValueOff(Coord( 5,-10,-20));
    tree.setValueOff(Coord( 5, 10,-20));
    EXPECT_EQ(Index64(0), tree.activeVoxelCount());
    EXPECT_EQ(Index32(4), tree.leafCount());
    // Verify that prune() has no effect, since the values are still different.
    openvdb::tools::prune(tree);
    EXPECT_EQ(Index64(0), tree.activeVoxelCount());
    EXPECT_EQ(Index32(4), tree.leafCount());
    // Verify that pruneInactive() prunes all of the remaining leaf nodes.
    openvdb::tools::pruneInactive(tree);
    EXPECT_TRUE(tree.empty());
}

TEST_F(TestTree, testPruneLevelSet)
{
    const float background=10.0f, R=5.6f;
    const openvdb::Vec3f C(12.3f, 15.5f, 10.0f);
    const openvdb::Coord dim(32, 32, 32);

    openvdb::FloatGrid grid(background);
    unittest_util::makeSphere<openvdb::FloatGrid>(dim, C, R, grid,
                                                  1.0f, unittest_util::SPHERE_SPARSE_NARROW_BAND);
    openvdb::FloatTree& tree = grid.tree();

    openvdb::Index64 count = 0;
    openvdb::Coord xyz;
    for (xyz[0]=0; xyz[0]<dim[0]; ++xyz[0]) {
        for (xyz[1]=0; xyz[1]<dim[1]; ++xyz[1]) {
            for (xyz[2]=0; xyz[2]<dim[2]; ++xyz[2]) {
                if (fabs(tree.getValue(xyz))<background) ++count;
            }
        }
    }

    const openvdb::Index32 leafCount = tree.leafCount();
    EXPECT_EQ(tree.activeVoxelCount(), count);
    EXPECT_EQ(tree.activeLeafVoxelCount(), count);

    openvdb::Index64 removed = 0;
    const float new_width = background - 9.0f;

    // This version is fast since it only visits voxel and avoids
    // random access to set the voxels off.
    using VoxelOnIter = openvdb::FloatTree::LeafNodeType::ValueOnIter;
    for (openvdb::FloatTree::LeafIter lIter = tree.beginLeaf(); lIter; ++lIter) {
        for (VoxelOnIter vIter = lIter->beginValueOn(); vIter; ++vIter) {
            if (fabs(*vIter)<new_width) continue;
            lIter->setValueOff(vIter.pos(), *vIter > 0.0f ? background : -background);
            ++removed;
        }
    }
    // The following version is slower since it employs
    // FloatTree::ValueOnIter that visits both tiles and voxels and
    // also uses random acceess to set the voxels off.
    /*
      for (openvdb::FloatTree::ValueOnIter i = tree.beginValueOn(); i; ++i) {
      if (fabs(*i)<new_width) continue;
      tree.setValueOff(i.getCoord(), *i > 0.0f ? background : -background);
      ++removed2;
      }
    */

    EXPECT_EQ(leafCount, tree.leafCount());
    //std::cerr << "Leaf count=" << tree.leafCount() << std::endl;
    EXPECT_EQ(tree.activeVoxelCount(), count-removed);
    EXPECT_EQ(tree.activeLeafVoxelCount(), count-removed);

    openvdb::tools::pruneLevelSet(tree);

    EXPECT_TRUE(tree.leafCount() < leafCount);
    //std::cerr << "Leaf count=" << tree.leafCount() << std::endl;
    EXPECT_EQ(tree.activeVoxelCount(), count-removed);
    EXPECT_EQ(tree.activeLeafVoxelCount(), count-removed);

    openvdb::FloatTree::ValueOnCIter i = tree.cbeginValueOn();
    for (; i; ++i) EXPECT_TRUE( *i < new_width);

    for (xyz[0]=0; xyz[0]<dim[0]; ++xyz[0]) {
        for (xyz[1]=0; xyz[1]<dim[1]; ++xyz[1]) {
            for (xyz[2]=0; xyz[2]<dim[2]; ++xyz[2]) {
                const float val = tree.getValue(xyz);
                if (fabs(val)<new_width)
                    EXPECT_TRUE(tree.isValueOn(xyz));
                else if (val < 0.0f) {
                    EXPECT_TRUE(tree.isValueOff(xyz));
                    ASSERT_DOUBLES_EXACTLY_EQUAL( -background, val );
                } else {
                    EXPECT_TRUE(tree.isValueOff(xyz));
                    ASSERT_DOUBLES_EXACTLY_EQUAL(  background, val );
                }
            }
        }
    }
}


TEST_F(TestTree, testTouchLeaf)
{
    const float background=10.0f;
    const openvdb::Coord xyz(-20,30,10);
    {// test tree
        openvdb::FloatTree::Ptr tree(new openvdb::FloatTree(background));
        EXPECT_EQ(-1, tree->getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree->leafCount()));
        EXPECT_TRUE(tree->touchLeaf(xyz) != nullptr);
        EXPECT_EQ( 3, tree->getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree->leafCount()));
        EXPECT_TRUE(!tree->isValueOn(xyz));
        ASSERT_DOUBLES_EXACTLY_EQUAL(background, tree->getValue(xyz));
    }
    {// test accessor
        openvdb::FloatTree::Ptr tree(new openvdb::FloatTree(background));
        openvdb::tree::ValueAccessor<openvdb::FloatTree> acc(*tree);
        EXPECT_EQ(-1, acc.getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree->leafCount()));
        EXPECT_TRUE(acc.touchLeaf(xyz) != nullptr);
        EXPECT_EQ( 3, tree->getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree->leafCount()));
        EXPECT_TRUE(!acc.isValueOn(xyz));
        ASSERT_DOUBLES_EXACTLY_EQUAL(background, acc.getValue(xyz));
    }
}


TEST_F(TestTree, testProbeLeaf)
{
    const float background=10.0f, value = 2.0f;
    const openvdb::Coord xyz(-20,30,10);
    {// test Tree::probeLeaf
        openvdb::FloatTree::Ptr tree(new openvdb::FloatTree(background));
        EXPECT_EQ(-1, tree->getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree->leafCount()));
        EXPECT_TRUE(tree->probeLeaf(xyz) == nullptr);
        EXPECT_EQ(-1, tree->getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree->leafCount()));
        tree->setValue(xyz, value);
        EXPECT_EQ( 3, tree->getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree->leafCount()));
        EXPECT_TRUE(tree->probeLeaf(xyz) != nullptr);
        EXPECT_EQ( 3, tree->getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree->leafCount()));
        EXPECT_TRUE(tree->isValueOn(xyz));
        ASSERT_DOUBLES_EXACTLY_EQUAL(value, tree->getValue(xyz));
    }
    {// test Tree::probeConstLeaf
        const openvdb::FloatTree tree1(background);
        EXPECT_EQ(-1, tree1.getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree1.leafCount()));
        EXPECT_TRUE(tree1.probeConstLeaf(xyz) == nullptr);
        EXPECT_EQ(-1, tree1.getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree1.leafCount()));
        openvdb::FloatTree tmp(tree1);
        tmp.setValue(xyz, value);
        const openvdb::FloatTree tree2(tmp);
        EXPECT_EQ( 3, tree2.getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree2.leafCount()));
        EXPECT_TRUE(tree2.probeConstLeaf(xyz) != nullptr);
        EXPECT_EQ( 3, tree2.getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree2.leafCount()));
        EXPECT_TRUE(tree2.isValueOn(xyz));
        ASSERT_DOUBLES_EXACTLY_EQUAL(value, tree2.getValue(xyz));
    }
    {// test ValueAccessor::probeLeaf
        openvdb::FloatTree::Ptr tree(new openvdb::FloatTree(background));
        openvdb::tree::ValueAccessor<openvdb::FloatTree> acc(*tree);
        EXPECT_EQ(-1, acc.getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree->leafCount()));
        EXPECT_TRUE(acc.probeLeaf(xyz) == nullptr);
        EXPECT_EQ(-1, acc.getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree->leafCount()));
        acc.setValue(xyz, value);
        EXPECT_EQ( 3, acc.getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree->leafCount()));
        EXPECT_TRUE(acc.probeLeaf(xyz) != nullptr);
        EXPECT_EQ( 3, acc.getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree->leafCount()));
        EXPECT_TRUE(acc.isValueOn(xyz));
        ASSERT_DOUBLES_EXACTLY_EQUAL(value, acc.getValue(xyz));
    }
    {// test ValueAccessor::probeConstLeaf
        const openvdb::FloatTree tree1(background);
        openvdb::tree::ValueAccessor<const openvdb::FloatTree> acc1(tree1);
        EXPECT_EQ(-1, acc1.getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree1.leafCount()));
        EXPECT_TRUE(acc1.probeConstLeaf(xyz) == nullptr);
        EXPECT_EQ(-1, acc1.getValueDepth(xyz));
        EXPECT_EQ( 0, int(tree1.leafCount()));
        openvdb::FloatTree tmp(tree1);
        tmp.setValue(xyz, value);
        const openvdb::FloatTree tree2(tmp);
        openvdb::tree::ValueAccessor<const openvdb::FloatTree> acc2(tree2);
        EXPECT_EQ( 3, acc2.getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree2.leafCount()));
        EXPECT_TRUE(acc2.probeConstLeaf(xyz) != nullptr);
        EXPECT_EQ( 3, acc2.getValueDepth(xyz));
        EXPECT_EQ( 1, int(tree2.leafCount()));
        EXPECT_TRUE(acc2.isValueOn(xyz));
        ASSERT_DOUBLES_EXACTLY_EQUAL(value, acc2.getValue(xyz));
    }
}


TEST_F(TestTree, testAddLeaf)
{
    using namespace openvdb;

    using LeafT = FloatTree::LeafNodeType;

    const Coord ijk(100);
    FloatGrid grid;
    FloatTree& tree = grid.tree();

    tree.setValue(ijk, 5.0);
    const LeafT* oldLeaf = tree.probeLeaf(ijk);
    EXPECT_TRUE(oldLeaf != nullptr);
    ASSERT_DOUBLES_EXACTLY_EQUAL(5.0, oldLeaf->getValue(ijk));

    LeafT* newLeaf = new LeafT;
    newLeaf->setOrigin(oldLeaf->origin());
    newLeaf->fill(3.0);

    tree.addLeaf(newLeaf);
    EXPECT_EQ(newLeaf, tree.probeLeaf(ijk));
    ASSERT_DOUBLES_EXACTLY_EQUAL(3.0, tree.getValue(ijk));
}


TEST_F(TestTree, testAddTile)
{
    using namespace openvdb;

    const Coord ijk(100);
    FloatGrid grid;
    FloatTree& tree = grid.tree();

    tree.setValue(ijk, 5.0);
    EXPECT_TRUE(tree.probeLeaf(ijk) != nullptr);

    const Index lvl = FloatTree::DEPTH >> 1;
    OPENVDB_NO_UNREACHABLE_CODE_WARNING_BEGIN
    if (lvl > 0) tree.addTile(lvl,ijk, 3.0, /*active=*/true);
    else tree.addTile(1,ijk, 3.0, /*active=*/true);
    OPENVDB_NO_UNREACHABLE_CODE_WARNING_END

    EXPECT_TRUE(tree.probeLeaf(ijk) == nullptr);
    ASSERT_DOUBLES_EXACTLY_EQUAL(3.0, tree.getValue(ijk));
}


struct BBoxOp
{
    std::vector<openvdb::CoordBBox> bbox;
    std::vector<openvdb::Index> level;

    // This method is required by Tree::visitActiveBBox
    // Since it will return false if LEVEL==0 it will never descent to
    // the active voxels. In other words the smallest BBoxes
    // correspond to LeafNodes or active tiles at LEVEL=1
    template<openvdb::Index LEVEL>
    inline bool descent() { return LEVEL>0; }

    // This method is required by Tree::visitActiveBBox
    template<openvdb::Index LEVEL>
    inline void operator()(const openvdb::CoordBBox &_bbox) {
        bbox.push_back(_bbox);
        level.push_back(LEVEL);
    }
};

TEST_F(TestTree, testProcessBBox)
{
    OPENVDB_NO_DEPRECATION_WARNING_BEGIN

    using openvdb::Coord;
    using openvdb::CoordBBox;
    //check two leaf nodes and two tiles at each level 1, 2 and 3
    const int size[4]={1<<3, 1<<3, 1<<(3+4), 1<<(3+4+5)};
    for (int level=0; level<=3; ++level) {
        openvdb::FloatTree tree;
        const int n = size[level];
        const CoordBBox bbox[]={CoordBBox::createCube(Coord(-n,-n,-n), n),
                                CoordBBox::createCube(Coord( 0, 0, 0), n)};
        if (level==0) {
            tree.setValue(Coord(-1,-2,-3), 1.0f);
            tree.setValue(Coord( 1, 2, 3), 1.0f);
        } else {
            tree.fill(bbox[0], 1.0f, true);
            tree.fill(bbox[1], 1.0f, true);
        }
        BBoxOp op;
        tree.visitActiveBBox(op);
        EXPECT_EQ(2, int(op.bbox.size()));

        for (int i=0; i<2; ++i) {
            //std::cerr <<"\nLevel="<<level<<" op.bbox["<<i<<"]="<<op.bbox[i]
            //          <<" op.level["<<i<<"]= "<<op.level[i]<<std::endl;
            EXPECT_EQ(level,int(op.level[i]));
            EXPECT_TRUE(op.bbox[i] == bbox[i]);
        }
    }

    OPENVDB_NO_DEPRECATION_WARNING_END
}

TEST_F(TestTree, testGetNodes)
{
    //openvdb::util::CpuTimer timer;
    using openvdb::CoordBBox;
    using openvdb::Coord;
    using openvdb::Vec3f;
    using openvdb::FloatGrid;
    using openvdb::FloatTree;

    const Vec3f center(0.35f, 0.35f, 0.35f);
    const float radius = 0.15f;
    const int dim = 128, half_width = 5;
    const float voxel_size = 1.0f/dim;

    FloatGrid::Ptr grid = FloatGrid::create(/*background=*/half_width*voxel_size);
    FloatTree& tree = grid->tree();
    grid->setTransform(openvdb::math::Transform::createLinearTransform(/*voxel size=*/voxel_size));

    unittest_util::makeSphere<FloatGrid>(
        Coord(dim), center, radius, *grid, unittest_util::SPHERE_SPARSE_NARROW_BAND);
    const size_t leafCount = tree.leafCount();
    const size_t voxelCount = tree.activeVoxelCount();

    {//testing Tree::getNodes() with std::vector<T*>
        std::vector<openvdb::FloatTree::LeafNodeType*> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::vector<T*> and Tree::getNodes()");
        tree.getNodes(array);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(leafCount, size_t(tree.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::getNodes() with std::vector<const T*>
        std::vector<const openvdb::FloatTree::LeafNodeType*> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::vector<const T*> and Tree::getNodes()");
        tree.getNodes(array);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(leafCount, size_t(tree.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::getNodes() const with std::vector<const T*>
        std::vector<const openvdb::FloatTree::LeafNodeType*> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::vector<const T*> and Tree::getNodes() const");
        const FloatTree& tmp = tree;
        tmp.getNodes(array);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(leafCount, size_t(tree.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::getNodes() with std::vector<T*> and std::vector::reserve
        std::vector<openvdb::FloatTree::LeafNodeType*> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::vector<T*>, std::vector::reserve and Tree::getNodes");
        array.reserve(tree.leafCount());
        tree.getNodes(array);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(leafCount, size_t(tree.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::getNodes() with std::deque<T*>
        std::deque<const openvdb::FloatTree::LeafNodeType*> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::deque<T*> and Tree::getNodes");
        tree.getNodes(array);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(leafCount, size_t(tree.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::getNodes() with std::deque<T*>
        std::deque<const openvdb::FloatTree::RootNodeType::ChildNodeType*> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::deque<T*> and Tree::getNodes");
        tree.getNodes(array);
        //timer.stop();
        EXPECT_EQ(size_t(1), array.size());
        EXPECT_EQ(leafCount, size_t(tree.leafCount()));
    }
    {//testing Tree::getNodes() with std::deque<T*>
        std::deque<const openvdb::FloatTree::RootNodeType::ChildNodeType::ChildNodeType*> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::deque<T*> and Tree::getNodes");
        tree.getNodes(array);
        //timer.stop();
        EXPECT_EQ(size_t(1), array.size());
        EXPECT_EQ(leafCount, size_t(tree.leafCount()));
    }
    /*
    {//testing Tree::getNodes() with std::deque<T*> where T is not part of the tree configuration
        using NodeT = openvdb::tree::LeafNode<float, 5>;
        std::deque<const NodeT*> array;
        tree.getNodes(array);//should NOT compile since NodeT is not part of the FloatTree configuration
    }
    {//testing Tree::getNodes() const with std::deque<T*> where T is not part of the tree configuration
        using NodeT = openvdb::tree::LeafNode<float, 5>;
        std::deque<const NodeT*> array;
        const FloatTree& tmp = tree;
        tmp.getNodes(array);//should NOT compile since NodeT is not part of the FloatTree configuration
    }
    */
}// testGetNodes

// unique_ptr wrapper around a value type for a stl container
template <typename NodeT, template<class, class> class Container>
struct SafeArray {
    using value_type = NodeT*;
    inline void reserve(const size_t size) { mContainer.reserve(size); }
    void push_back(value_type ptr) { mContainer.emplace_back(ptr); }
    size_t size() const { return mContainer.size(); }
    inline const NodeT* operator[](const size_t idx) const { return mContainer[idx].get(); }
    Container<std::unique_ptr<NodeT>, std::allocator<std::unique_ptr<NodeT>>> mContainer;
};

TEST_F(TestTree, testStealNodes)
{
    //openvdb::util::CpuTimer timer;
    using openvdb::CoordBBox;
    using openvdb::Coord;
    using openvdb::Vec3f;
    using openvdb::FloatGrid;
    using openvdb::FloatTree;

    const Vec3f center(0.35f, 0.35f, 0.35f);
    const float radius = 0.15f;
    const int dim = 128, half_width = 5;
    const float voxel_size = 1.0f/dim;

    FloatGrid::Ptr grid = FloatGrid::create(/*background=*/half_width*voxel_size);
    const FloatTree& tree = grid->tree();
    grid->setTransform(openvdb::math::Transform::createLinearTransform(/*voxel size=*/voxel_size));

    unittest_util::makeSphere<FloatGrid>(
        Coord(dim), center, radius, *grid, unittest_util::SPHERE_SPARSE_NARROW_BAND);
    const size_t leafCount = tree.leafCount();
    const size_t voxelCount = tree.activeVoxelCount();

    {//testing Tree::stealNodes() with std::vector<T*>
        FloatTree tree2 = tree;
        SafeArray<openvdb::FloatTree::LeafNodeType, std::vector> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::vector<T*> and Tree::stealNodes()");
        tree2.stealNodes(array);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(size_t(0), size_t(tree2.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::stealNodes() with std::vector<const T*>
        FloatTree tree2 = tree;
        SafeArray<const openvdb::FloatTree::LeafNodeType, std::vector> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::vector<const T*> and Tree::stealNodes()");
        tree2.stealNodes(array);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(size_t(0), size_t(tree2.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::stealNodes() const with std::vector<const T*>
        FloatTree tree2 = tree;
        SafeArray<const openvdb::FloatTree::LeafNodeType, std::vector> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::vector<const T*> and Tree::stealNodes() const");
        tree2.stealNodes(array);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(size_t(0), size_t(tree2.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::stealNodes() with std::vector<T*> and std::vector::reserve
        FloatTree tree2 = tree;
        SafeArray<openvdb::FloatTree::LeafNodeType, std::vector> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::vector<T*>, std::vector::reserve and Tree::stealNodes");
        array.reserve(tree2.leafCount());
        tree2.stealNodes(array, 0.0f, false);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(size_t(0), size_t(tree2.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::getNodes() with std::deque<T*>
        FloatTree tree2 = tree;
        SafeArray<const openvdb::FloatTree::LeafNodeType, std::deque> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::deque<T*> and Tree::stealNodes");
        tree2.stealNodes(array);
        //timer.stop();
        EXPECT_EQ(leafCount, array.size());
        EXPECT_EQ(size_t(0), size_t(tree2.leafCount()));
        size_t sum = 0;
        for (size_t i=0; i<array.size(); ++i) sum += array[i]->onVoxelCount();
        EXPECT_EQ(voxelCount, sum);
    }
    {//testing Tree::getNodes() with std::deque<T*>
        FloatTree tree2 = tree;
        SafeArray<const openvdb::FloatTree::RootNodeType::ChildNodeType, std::deque> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::deque<T*> and Tree::stealNodes");
        tree2.stealNodes(array, 0.0f, true);
        //timer.stop();
        EXPECT_EQ(size_t(1), array.size());
        EXPECT_EQ(size_t(0), size_t(tree2.leafCount()));
    }
    {//testing Tree::getNodes() with std::deque<T*>
        using NodeT = openvdb::FloatTree::RootNodeType::ChildNodeType::ChildNodeType;
        FloatTree tree2 = tree;
        SafeArray<const NodeT, std::deque> array;
        EXPECT_EQ(size_t(0), array.size());
        //timer.start("\nstd::deque<T*> and Tree::stealNodes");
        tree2.stealNodes(array);
        //timer.stop();
        EXPECT_EQ(size_t(1), array.size());
        EXPECT_EQ(size_t(0), size_t(tree2.leafCount()));
    }
    /*
    {//testing Tree::stealNodes() with std::deque<T*> where T is not part of the tree configuration
        FloatTree tree2 = tree;
        using NodeT = openvdb::tree::LeafNode<float, 5>;
        std::deque<const NodeT*> array;
        //should NOT compile since NodeT is not part of the FloatTree configuration
        tree2.stealNodes(array, 0.0f, true);
    }
    */
}// testStealNodes

TEST_F(TestTree, testStealNode)
{
    using openvdb::Index;
    using openvdb::FloatTree;

    const float background=0.0f, value = 5.6f, epsilon=0.000001f;
    const openvdb::Coord xyz(-23,42,70);

    {// stal a LeafNode
        using NodeT = FloatTree::LeafNodeType;
        EXPECT_EQ(Index(0), NodeT::getLevel());

        FloatTree tree(background);
        EXPECT_EQ(Index(0), tree.leafCount());
        EXPECT_TRUE(!tree.isValueOn(xyz));
        EXPECT_NEAR(background, tree.getValue(xyz), epsilon);
        EXPECT_TRUE(tree.root().stealNode<NodeT>(xyz, value, false) == nullptr);

        tree.setValue(xyz, value);
        EXPECT_EQ(Index(1), tree.leafCount());
        EXPECT_TRUE(tree.isValueOn(xyz));
        EXPECT_NEAR(value, tree.getValue(xyz), epsilon);

        NodeT* node = tree.root().stealNode<NodeT>(xyz, background, false);
        EXPECT_TRUE(node != nullptr);
        EXPECT_EQ(Index(0), tree.leafCount());
        EXPECT_TRUE(!tree.isValueOn(xyz));
        EXPECT_NEAR(background, tree.getValue(xyz), epsilon);
        EXPECT_TRUE(tree.root().stealNode<NodeT>(xyz, value, false) == nullptr);
        EXPECT_NEAR(value, node->getValue(xyz), epsilon);
        EXPECT_TRUE(node->isValueOn(xyz));
        delete node;
    }
    {// steal a bottom InternalNode
        using NodeT = FloatTree::RootNodeType::ChildNodeType::ChildNodeType;
        EXPECT_EQ(Index(1), NodeT::getLevel());

        FloatTree tree(background);
        EXPECT_EQ(Index(0), tree.leafCount());
        EXPECT_TRUE(!tree.isValueOn(xyz));
        EXPECT_NEAR(background, tree.getValue(xyz), epsilon);
        EXPECT_TRUE(tree.root().stealNode<NodeT>(xyz, value, false) == nullptr);

        tree.setValue(xyz, value);
        EXPECT_EQ(Index(1), tree.leafCount());
        EXPECT_TRUE(tree.isValueOn(xyz));
        EXPECT_NEAR(value, tree.getValue(xyz), epsilon);

        NodeT* node = tree.root().stealNode<NodeT>(xyz, background, false);
        EXPECT_TRUE(node != nullptr);
        EXPECT_EQ(Index(0), tree.leafCount());
        EXPECT_TRUE(!tree.isValueOn(xyz));
        EXPECT_NEAR(background, tree.getValue(xyz), epsilon);
        EXPECT_TRUE(tree.root().stealNode<NodeT>(xyz, value, false) == nullptr);
        EXPECT_NEAR(value, node->getValue(xyz), epsilon);
        EXPECT_TRUE(node->isValueOn(xyz));
        delete node;
    }
    {// steal a top InternalNode
        using NodeT = FloatTree::RootNodeType::ChildNodeType;
        EXPECT_EQ(Index(2), NodeT::getLevel());

        FloatTree tree(background);
        EXPECT_EQ(Index(0), tree.leafCount());
        EXPECT_TRUE(!tree.isValueOn(xyz));
        EXPECT_NEAR(background, tree.getValue(xyz), epsilon);
        EXPECT_TRUE(tree.root().stealNode<NodeT>(xyz, value, false) == nullptr);

        tree.setValue(xyz, value);
        EXPECT_EQ(Index(1), tree.leafCount());
        EXPECT_TRUE(tree.isValueOn(xyz));
        EXPECT_NEAR(value, tree.getValue(xyz), epsilon);

        NodeT* node = tree.root().stealNode<NodeT>(xyz, background, false);
        EXPECT_TRUE(node != nullptr);
        EXPECT_EQ(Index(0), tree.leafCount());
        EXPECT_TRUE(!tree.isValueOn(xyz));
        EXPECT_NEAR(background, tree.getValue(xyz), epsilon);
        EXPECT_TRUE(tree.root().stealNode<NodeT>(xyz, value, false) == nullptr);
        EXPECT_NEAR(value, node->getValue(xyz), epsilon);
        EXPECT_TRUE(node->isValueOn(xyz));
        delete node;
    }
}

#if OPENVDB_ABI_VERSION_NUMBER >= 7
TEST_F(TestTree, testNodeCount)
{
    //openvdb::util::CpuTimer timer;// use for benchmark test

    const openvdb::Vec3f center(0.0f, 0.0f, 0.0f);
    const float radius = 1.0f;
    //const int dim = 4096, halfWidth = 3;// use for benchmark test
    const int dim = 512, halfWidth = 3;// use for unit test
    //timer.start("\nGenerate level set sphere");// use for benchmark test
    auto  grid = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(radius, center, radius/dim, halfWidth);
    //timer.stop();// use for benchmark test
    auto& tree = grid->tree();

    std::vector<openvdb::Index> dims;
    tree.getNodeLog2Dims(dims);
    std::vector<openvdb::Index32> nodeCount1(dims.size());
    //timer.start("Old technique");// use for benchmark test
    for (auto it = tree.cbeginNode(); it; ++it) ++(nodeCount1[dims.size()-1-it.getDepth()]);
    //timer.restart("New technique");// use for benchmark test
    const auto nodeCount2 = tree.nodeCount();
    //timer.stop();// use for benchmark test
    EXPECT_EQ(nodeCount1.size(), nodeCount2.size());
    //for (size_t i=0; i<nodeCount2.size(); ++i) std::cerr << "nodeCount1("<<i<<") OLD/NEW: " << nodeCount1[i] << "/" << nodeCount2[i] << std::endl;
    EXPECT_EQ(1U, nodeCount2.back());// one root node
    EXPECT_EQ(tree.leafCount(), nodeCount2.front());// leaf nodes
    for (size_t i=0; i<nodeCount2.size(); ++i) EXPECT_EQ( nodeCount1[i], nodeCount2[i]);
}
#endif

TEST_F(TestTree, testRootNode)
{
    using ChildType = RootNodeType::ChildNodeType;
    const openvdb::Coord c0(0,0,0), c1(49152, 16384, 28672);

    { // test inserting child nodes directly and indirectly
        RootNodeType root(0.0f);
        EXPECT_TRUE(root.empty());
        EXPECT_EQ(openvdb::Index32(0), root.childCount());

        // populate the tree by inserting the two leaf nodes containing c0 and c1
        root.touchLeaf(c0);
        root.touchLeaf(c1);
        EXPECT_EQ(openvdb::Index(2), root.getTableSize());
        EXPECT_EQ(openvdb::Index32(2), root.childCount());
        EXPECT_TRUE(!root.hasActiveTiles());

        { // verify c0 and c1 are the root node coordinates
            auto rootIter = root.cbeginChildOn();
            EXPECT_EQ(c0, rootIter.getCoord());
            ++rootIter;
            EXPECT_EQ(c1, rootIter.getCoord());
        }

        // copy the root node
        RootNodeType rootCopy(root);

        // steal the root node children leaving the root node empty again
        std::vector<ChildType*> children;
        root.stealNodes(children);
        EXPECT_TRUE(root.empty());

        // insert the root node children directly
        for (ChildType* child : children) {
            root.addChild(child);
        }
        EXPECT_EQ(openvdb::Index(2), root.getTableSize());
        EXPECT_EQ(openvdb::Index32(2), root.childCount());

        { // verify the coordinates of the root node children
            auto rootIter = root.cbeginChildOn();
            EXPECT_EQ(c0, rootIter.getCoord());
            ++rootIter;
            EXPECT_EQ(c1, rootIter.getCoord());
        }
    }

    { // test inserting tiles and replacing them with child nodes
        RootNodeType root(0.0f);
        EXPECT_TRUE(root.empty());

        // no-op
        root.addChild(nullptr);

        // populate the root node by inserting tiles
        root.addTile(c0, /*value=*/1.0f, /*state=*/true);
        root.addTile(c1, /*value=*/2.0f, /*state=*/true);
        EXPECT_EQ(openvdb::Index(2), root.getTableSize());
        EXPECT_EQ(openvdb::Index32(0), root.childCount());
        EXPECT_TRUE(root.hasActiveTiles());
        ASSERT_DOUBLES_EXACTLY_EQUAL(1.0f, root.getValue(c0));
        ASSERT_DOUBLES_EXACTLY_EQUAL(2.0f, root.getValue(c1));

        // insert child nodes with the same coordinates
        root.addChild(new ChildType(c0, 3.0f));
        root.addChild(new ChildType(c1, 4.0f));

        // insert a new child at c0
        root.addChild(new ChildType(c0, 5.0f));

        // verify active tiles have been replaced by child nodes
        EXPECT_EQ(openvdb::Index(2), root.getTableSize());
        EXPECT_EQ(openvdb::Index32(2), root.childCount());
        EXPECT_TRUE(!root.hasActiveTiles());

        { // verify the coordinates of the root node children
            auto rootIter = root.cbeginChildOn();
            EXPECT_EQ(c0, rootIter.getCoord());
            ASSERT_DOUBLES_EXACTLY_EQUAL(5.0f, root.getValue(c0));
            ++rootIter;
            EXPECT_EQ(c1, rootIter.getCoord());
        }
    }
}

TEST_F(TestTree, testInternalNode)
{
    const openvdb::Coord c0(1000, 1000, 1000);
    const openvdb::Coord c1(896, 896, 896);

    using InternalNodeType = InternalNodeType1;
    using ChildType = LeafNodeType;

    { // test inserting child nodes directly and indirectly
        openvdb::Coord c2 = c1.offsetBy(8,0,0);
        openvdb::Coord c3 = c1.offsetBy(16,16,16);

        InternalNodeType internalNode(c1, 0.0f);
        internalNode.touchLeaf(c2);
        internalNode.touchLeaf(c3);

        EXPECT_EQ(openvdb::Index(2), internalNode.leafCount());
        EXPECT_EQ(openvdb::Index32(2), internalNode.childCount());
        EXPECT_TRUE(!internalNode.hasActiveTiles());

        { // verify c0 and c1 are the root node coordinates
            auto childIter = internalNode.cbeginChildOn();
            EXPECT_EQ(c2, childIter.getCoord());
            ++childIter;
            EXPECT_EQ(c3, childIter.getCoord());
        }

        // copy the internal node
        InternalNodeType internalNodeCopy(internalNode);

        // steal the internal node children leaving it empty again
        std::vector<ChildType*> children;
        internalNode.stealNodes(children, 0.0f, false);
        EXPECT_EQ(openvdb::Index(0), internalNode.leafCount());
        EXPECT_EQ(openvdb::Index32(0), internalNode.childCount());

        // insert the root node children directly
        for (ChildType* child : children) {
            internalNode.addChild(child);
        }
        EXPECT_EQ(openvdb::Index(2), internalNode.leafCount());
        EXPECT_EQ(openvdb::Index32(2), internalNode.childCount());

        { // verify the coordinates of the root node children
            auto childIter = internalNode.cbeginChildOn();
            EXPECT_EQ(c2, childIter.getCoord());
            ++childIter;
            EXPECT_EQ(c3, childIter.getCoord());
        }
    }

    { // test inserting a tile and replacing with a child node
        InternalNodeType internalNode(c1, 0.0f);
        EXPECT_TRUE(!internalNode.hasActiveTiles());
        EXPECT_EQ(openvdb::Index(0), internalNode.leafCount());
        EXPECT_EQ(openvdb::Index32(0), internalNode.childCount());

        // add a tile
        internalNode.addTile(openvdb::Index(0), /*value=*/1.0f, /*state=*/true);
        EXPECT_TRUE(internalNode.hasActiveTiles());
        EXPECT_EQ(openvdb::Index(0), internalNode.leafCount());
        EXPECT_EQ(openvdb::Index32(0), internalNode.childCount());

        // replace the tile with a child node
        EXPECT_TRUE(internalNode.addChild(new ChildType(c1, 2.0f)));
        EXPECT_TRUE(!internalNode.hasActiveTiles());
        EXPECT_EQ(openvdb::Index(1), internalNode.leafCount());
        EXPECT_EQ(openvdb::Index32(1), internalNode.childCount());
        EXPECT_EQ(c1, internalNode.cbeginChildOn().getCoord());
        ASSERT_DOUBLES_EXACTLY_EQUAL(2.0f, internalNode.cbeginChildOn()->getValue(0));

        // replace the child node with another child node
        EXPECT_TRUE(internalNode.addChild(new ChildType(c1, 3.0f)));
        ASSERT_DOUBLES_EXACTLY_EQUAL(3.0f, internalNode.cbeginChildOn()->getValue(0));
    }

    { // test inserting child nodes that do and do not belong to the internal node
        InternalNodeType internalNode(c1, 0.0f);

        // succeed if child belongs to this internal node
        EXPECT_TRUE(internalNode.addChild(new ChildType(c0.offsetBy(8,0,0))));
        EXPECT_TRUE(internalNode.probeLeaf(c0.offsetBy(8,0,0)));
        openvdb::Index index1 = internalNode.coordToOffset(c0);
        openvdb::Index index2 = internalNode.coordToOffset(c0.offsetBy(8,0,0));
        EXPECT_TRUE(!internalNode.isChildMaskOn(index1));
        EXPECT_TRUE(internalNode.isChildMaskOn(index2));

        // fail otherwise
        auto* child = new ChildType(c0.offsetBy(8000,0,0));
        EXPECT_TRUE(!internalNode.addChild(child));
        delete child;
    }
}

// Copyright (c) DreamWorks Animation LLC
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
