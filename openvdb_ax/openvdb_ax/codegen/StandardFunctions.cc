// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

/// @file codegen/StandardFunctions.cc
///
/// @authors Nick Avramoussis, Richard Jones, Francisco Gochez
///
/// @brief  Definitions for all standard functions supported by AX. A
///   standard function is one that is supported no matter the input
///   primitive type and rely either solely on AX types or core AX
///   intrinsics.

#include "Functions.h"
#include "FunctionTypes.h"
#include "Types.h"
#include "Utils.h"

#include "../Exceptions.h"
#include "../math/OpenSimplexNoise.h"
#include "../compiler/CompilerOptions.h"
#include "../compiler/CustomData.h"

#include <tbb/enumerable_thread_specific.h>

#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Instructions.h>

#include <unordered_map>
#include <functional>
#include <random>
#include <cmath>
#include <cstdlib>
#include <stddef.h>
#include <stdint.h>

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {

namespace ax {
namespace codegen {

namespace
{

// Reduce a size_t hash down into an unsigned int, taking all bits in the
// size_t into account. We achieve this by repeatedly XORing as many bytes
// that fit into an unsigned int, and then shift those bytes out of the
// hash. We repeat until we have no bits left in the hash.
template <typename SeedType>
inline SeedType hashToSeed(size_t hash) {
    SeedType seed = 0;
    do {
        seed ^= (SeedType) hash;
    } while (hash >>= sizeof(SeedType) * 8);
    return seed;
}

struct SimplexNoise
{
    // Open simplex noise - Visually axis-decorrelated coherent noise algorithm
    // based on the simplectic honeycomb.
    // See https://gist.github.com/KdotJPG/b1270127455a94ac5d19
    inline static double noise(double x, double y, double z)
    {
        static const OSN::OSNoise noiseGenerator = OSN::OSNoise();
        const double result = noiseGenerator.eval<double>(x, y, z);
        // adjust result so that it lies between 0 and 1, since
        // Noise::eval returns a number between -1 and 1
        return (result + 1.0) * 0.5;
    }
};

}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

#define DEFINE_LLVM_FP_INTRINSIC(Identifier, Doc)                                           \
    inline FunctionGroup::UniquePtr llvm_##Identifier(const FunctionOptions& op)            \
    {                                                                                       \
        static auto generate =                                                              \
            [](const std::vector<llvm::Value*>& args,                                       \
               llvm::IRBuilder<>& B) -> llvm::Value*                                        \
        {                                                                                   \
            llvm::Module* M = B.GetInsertBlock()->getParent()->getParent();                 \
            llvm::Function* function =                                                      \
                llvm::Intrinsic::getDeclaration(M,                                          \
                    llvm::Intrinsic::Identifier, args[0]->getType());                       \
            return B.CreateCall(function, args);                                            \
        };                                                                                  \
                                                                                            \
        return FunctionBuilder(#Identifier)                                                 \
            .addSignature<double(double)>(generate, (double(*)(double))(std::Identifier))   \
            .addSignature<float(float)>(generate, (float(*)(float))(std::Identifier))       \
            .setArgumentNames({"n"})                                                        \
            .addFunctionAttribute(llvm::Attribute::ReadOnly)                                \
            .addFunctionAttribute(llvm::Attribute::NoRecurse)                               \
            .addFunctionAttribute(llvm::Attribute::NoUnwind)                                \
            .addFunctionAttribute(llvm::Attribute::AlwaysInline)                            \
            .setConstantFold(op.mConstantFoldCBindings)                                     \
            .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)  \
            .setDocumentation(Doc)                                                          \
            .get();                                                                         \
    }                                                                                       \

#define DEFINE_AX_C_FP_BINDING(Identifier, Doc)                                             \
    inline FunctionGroup::UniquePtr ax##Identifier(const FunctionOptions& op)               \
    {                                                                                       \
        return FunctionBuilder(#Identifier)                                                 \
            .addSignature<double(double)>((double(*)(double))(std::Identifier))             \
            .addSignature<float(float)>((float(*)(float))(std::Identifier))                 \
            .setArgumentNames({"arg"})                                                      \
            .addFunctionAttribute(llvm::Attribute::ReadOnly)                                \
            .addFunctionAttribute(llvm::Attribute::NoRecurse)                               \
            .addFunctionAttribute(llvm::Attribute::NoUnwind)                                \
            .addFunctionAttribute(llvm::Attribute::AlwaysInline)                            \
            .setConstantFold(op.mConstantFoldCBindings)                                     \
            .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)  \
            .setDocumentation(Doc)                                                          \
            .get();                                                                         \
    }

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

// Memory

inline FunctionGroup::UniquePtr axmalloc(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        llvm::BasicBlock* BB = B.GetInsertBlock();
        /// @note The return type is i8* as the void* type is aliased to
        ///  i8* in Types.h.
        /// @todo should probably remove this alias and use i8* explicitly
        llvm::Instruction* inst =
            llvm::CallInst::CreateMalloc(BB, // location
                B.getInt64Ty(), // int ptr type
                B.getInt8Ty(),  // return type
                args[0], // size
                nullptr,
                nullptr);
        assert(inst);
        B.Insert(inst);
        return inst;
    };

    return FunctionBuilder("axmalloc")
        .addSignature<void*(size_t)>(generate, std::malloc, "malloc") // symbol is malloc, not ax.malloc
        .setArgumentNames({"size"})
        .setEmbedIR(true) // Embed the call to CreateMalloc, otherwise we gen a function "malloc" which calls "malloc"!
        .setConstantFold(false)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Allocate memory.")
        .get();
}

inline FunctionGroup::UniquePtr axfree(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        llvm::BasicBlock* BB = B.GetInsertBlock();
        llvm::Instruction* inst = llvm::CallInst::CreateFree(args[0], BB);
        assert(inst);
        B.Insert(inst);
        return nullptr;
    };

    return FunctionBuilder("axfree")
        .addSignature<void(void*)>(generate, std::free, "free") // symbol is free, not ax.free
        .setArgumentNames({"ptr"})
        .setEmbedIR(true) // Embed the call to CreateFree, otherwise we gen a function "free" which calls "free"!
        .setConstantFold(false)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Free memory.")
        .get();
}

inline FunctionGroup::UniquePtr axrealloc(const FunctionOptions&)
{
    return FunctionBuilder("axrealloc")
        .addSignature<void*(void*,size_t)>(std::realloc, "realloc")
        .setArgumentNames({"ptr", "size"})
        .setConstantFold(false)
        .setPreferredImpl(FunctionBuilder::C)
        .setDocumentation("Reallocate memory.")
        .get();
}

// Intrinsics

DEFINE_LLVM_FP_INTRINSIC(sqrt, "Computes the square root of arg.")
DEFINE_LLVM_FP_INTRINSIC(sin, "Computes the sine of arg (measured in radians).")
DEFINE_LLVM_FP_INTRINSIC(cos, "Computes the cosine of arg (measured in radians).")
DEFINE_LLVM_FP_INTRINSIC(log, "Computes the natural (base e) logarithm of arg.")
DEFINE_LLVM_FP_INTRINSIC(log10, "Computes the common (base-10) logarithm of arg.")
DEFINE_LLVM_FP_INTRINSIC(log2, "Computes the binary (base-2) logarithm of arg.")
DEFINE_LLVM_FP_INTRINSIC(exp, "Computes e (Euler's number, 2.7182818...) raised to the given power arg.")
DEFINE_LLVM_FP_INTRINSIC(exp2, "Computes 2 raised to the given power arg.")
DEFINE_LLVM_FP_INTRINSIC(fabs, "Computes the absolute value of a floating point value arg.")
DEFINE_LLVM_FP_INTRINSIC(floor, "Computes the largest integer value not greater than arg.")
DEFINE_LLVM_FP_INTRINSIC(ceil, "Computes the smallest integer value not less than arg.")
DEFINE_LLVM_FP_INTRINSIC(round, "Computes the nearest integer value to arg (in floating-point format),"
    " rounding halfway cases away from zero.")

// pow created explicitly as it takes two arguments and performs slighlty different
// calls for integer exponents

inline FunctionGroup::UniquePtr llvm_pow(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        llvm::Type* overloadType = args[0]->getType();
        llvm::Type* expType = args[1]->getType();
        const llvm::Intrinsic::ID id =
            expType->isIntegerTy() ? llvm::Intrinsic::powi : llvm::Intrinsic::pow;
        llvm::Module* M = B.GetInsertBlock()->getParent()->getParent();
        llvm::Function* function = llvm::Intrinsic::getDeclaration(M, id, overloadType);
        return B.CreateCall(function, args);
    };

    return FunctionBuilder("pow")
        .addSignature<double(double,double)>(generate, (double(*)(double,double))(std::pow))
        .addSignature<float(float,float)>(generate, (float(*)(float,float))(std::pow))
        .addSignature<double(double,int32_t)>(generate, (double(*)(double,int32_t))(std::pow))
        .setArgumentNames({"base", "exp"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(false) // decl's differ
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Computes the value of the first argument raised to the power of the second argument.")
        .get();
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

// Math

DEFINE_AX_C_FP_BINDING(cbrt, "Computes the cubic root of the input.")

inline FunctionGroup::UniquePtr axabs(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
             llvm::IRBuilder<>& B) -> llvm::Value*
    {
        llvm::Value* value = args.front();
        llvm::Type* type = value->getType();

        if (type->isFloatingPointTy()) {
            return llvm_fabs(op)->execute(args, B);
        }

        // if negative flip all the bits and add 1 (xor with -1 and sub 1)
        llvm::Value* shift = type == LLVMType<int32_t>::get(B.getContext()) ?
            LLVMType<int32_t>::get(B.getContext(), 31) :
            LLVMType<int64_t>::get(B.getContext(), 63);

        // arithmetic shift right
        llvm::Value* mask = B.CreateAShr(value, shift);
        llvm::Value* xorResult = binaryOperator(value, mask, ast::tokens::BITXOR, B);
        return binaryOperator(xorResult, mask, ast::tokens::MINUS, B);
    };

    // @note  We also support fabs through the ax abs function
    return FunctionBuilder("abs")
        .addSignature<int64_t(int64_t)>(generate, (int64_t(*)(int64_t))(std::abs))
        .addSignature<int32_t(int32_t)>(generate, (int32_t(*)(int32_t))(std::abs))
        .addSignature<double(double)>(generate, (double(*)(double))(std::abs))
        .addSignature<float(float)>(generate, (float(*)(float))(std::abs))
        .setArgumentNames({"n"})
        .addDependency("fabs")
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Computes the absolute value of an integer number.")
        .get();
}

inline FunctionGroup::UniquePtr axdot(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> v1, v2;
        arrayUnpack(args[0], v1, B, /*load*/true);
        arrayUnpack(args[1], v2, B, /*load*/true);

        v1[0] = binaryOperator(v1[0], v2[0], ast::tokens::MULTIPLY, B);
        v1[1] = binaryOperator(v1[1], v2[1], ast::tokens::MULTIPLY, B);
        v1[2] = binaryOperator(v1[2], v2[2], ast::tokens::MULTIPLY, B);

        llvm::Value* result = binaryOperator(v1[0], v1[1], ast::tokens::PLUS, B);
        result = binaryOperator(result, v1[2], ast::tokens::PLUS, B);
        return result;
    };

    static auto dot = [](auto a, auto b) {
        return a->dot(*b);
    };

    using DotD = double(openvdb::math::Vec3<double>*,openvdb::math::Vec3<double>*);
    using DotF = float(openvdb::math::Vec3<float>*,openvdb::math::Vec3<float>*);
    using DotI = int32_t(openvdb::math::Vec3<int32_t>*,openvdb::math::Vec3<int32_t>*);

    return FunctionBuilder("dot")
        .addSignature<DotD>(generate, (DotD*)(dot))
        .addSignature<DotF>(generate, (DotF*)(dot))
        .addSignature<DotI>(generate, (DotI*)(dot))
        .setArgumentNames({"a", "b"})
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Computes the dot product of two vectors.")
        .get();
}

inline FunctionGroup::UniquePtr axcross(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> ptrs, left, right;
        arrayUnpack(args[0], ptrs, B, /*load*/false);
        arrayUnpack(args[1], left, B, /*load*/true);
        arrayUnpack(args[2], right, B, /*load*/true);
        assert(ptrs.size() == 3);
        assert(left.size() == 3);
        assert(right.size() == 3);

        std::vector<llvm::Value*> results(3);

        llvm::Value* tmp1 = binaryOperator(left[1], right[2], ast::tokens::MULTIPLY, B);
        llvm::Value* tmp2 = binaryOperator(left[2], right[1], ast::tokens::MULTIPLY, B);
        results[0] = binaryOperator(tmp1, tmp2, ast::tokens::MINUS, B);

        tmp1 = binaryOperator(left[2], right[0], ast::tokens::MULTIPLY, B);
        tmp2 = binaryOperator(left[0], right[2], ast::tokens::MULTIPLY, B);
        results[1] = binaryOperator(tmp1, tmp2, ast::tokens::MINUS, B);

        tmp1 = binaryOperator(left[0], right[1], ast::tokens::MULTIPLY, B);
        tmp2 = binaryOperator(left[1], right[0], ast::tokens::MULTIPLY, B);
        results[2] = binaryOperator(tmp1, tmp2, ast::tokens::MINUS, B);

        B.CreateStore(results[0], ptrs[0]);
        B.CreateStore(results[1], ptrs[1]);
        B.CreateStore(results[2], ptrs[2]);

        return nullptr;
    };

    static auto cross = [](auto out, auto a, auto b) -> auto {
        *out = a->cross(*b);
    };

    using CrossD = void(openvdb::math::Vec3<double>*,openvdb::math::Vec3<double>*,openvdb::math::Vec3<double>*);
    using CrossF = void(openvdb::math::Vec3<float>*,openvdb::math::Vec3<float>*,openvdb::math::Vec3<float>*);
    using CrossI = void(openvdb::math::Vec3<int32_t>*,openvdb::math::Vec3<int32_t>*,openvdb::math::Vec3<int32_t>*);

    return FunctionBuilder("cross")
        .addSignature<CrossD, true>(generate, (CrossD*)(cross))
        .addSignature<CrossF, true>(generate, (CrossF*)(cross))
        .addSignature<CrossI, true>(generate, (CrossI*)(cross))
        .setArgumentNames({"a", "b"})
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(0, llvm::Attribute::WriteOnly)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addParameterAttribute(2, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the length of the given vector")
        .get();
}

inline FunctionGroup::UniquePtr axlengthsq(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> elements;
        arrayUnpack(args[0], elements, B, /*load*/true);
        assert(elements.size() >= 2);

        llvm::Value* v1 = binaryOperator(elements[0], elements[0], ast::tokens::MULTIPLY, B);
        llvm::Value* v2 = binaryOperator(elements[1], elements[1], ast::tokens::MULTIPLY, B);
        llvm::Value* result = binaryOperator(v1, v2, ast::tokens::PLUS, B);

        if (elements.size() > 2) {
            llvm::Value* v3 = binaryOperator(elements[2], elements[2], ast::tokens::MULTIPLY, B);
            result = binaryOperator(result, v3, ast::tokens::PLUS, B);
        }
        if (elements.size() > 3) {
            llvm::Value* v4 = binaryOperator(elements[3], elements[3], ast::tokens::MULTIPLY, B);
            result = binaryOperator(result, v4, ast::tokens::PLUS, B);
        }

        return result;
    };

    static auto lengthsq = [](auto in) -> auto { return in->lengthSqr(); };

    return FunctionBuilder("lengthsq")
        .addSignature<double(openvdb::math::Vec2<double>*)>(generate, lengthsq)
        .addSignature<float(openvdb::math::Vec2<float>*)>(generate, lengthsq)
        .addSignature<int32_t(openvdb::math::Vec2<int32_t>*)>(generate, lengthsq)
        .addSignature<double(openvdb::math::Vec3<double>*)>(generate, lengthsq)
        .addSignature<float(openvdb::math::Vec3<float>*)>(generate, lengthsq)
        .addSignature<int32_t(openvdb::math::Vec3<int32_t>*)>(generate, lengthsq)
        .addSignature<double(openvdb::math::Vec4<double>*)>(generate, lengthsq)
        .addSignature<float(openvdb::math::Vec4<float>*)>(generate, lengthsq)
        .addSignature<int32_t(openvdb::math::Vec4<int32_t>*)>(generate, lengthsq)
        .setArgumentNames({"v"})
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the squared length of the given vector")
        .get();
}

inline FunctionGroup::UniquePtr axlength(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
             llvm::IRBuilder<>& B) -> llvm::Value*
    {
        auto a = axlengthsq(op);
        auto s = llvm_sqrt(op);
        llvm::Value* lsq = a->execute(args, B);
        return s->execute({lsq}, B);
    };

    static auto length = [](auto in) -> auto
    {
        using VecType = typename std::remove_pointer<decltype(in)>::type;
        using ElementT = typename openvdb::VecTraits<VecType>::ElementType;
        using RetT = typename std::conditional
            <std::is_floating_point<ElementT>::value, ElementT, double>::type;
        return std::sqrt(RetT(in->lengthSqr()));
    };

    return FunctionBuilder("length")
        .addSignature<double(openvdb::math::Vec2<double>*)>(generate, length)
        .addSignature<float(openvdb::math::Vec2<float>*)>(generate, length)
        .addSignature<double(openvdb::math::Vec2<int32_t>*)>(generate, length)
        .addSignature<double(openvdb::math::Vec3<double>*)>(generate, length)
        .addSignature<float(openvdb::math::Vec3<float>*)>(generate, length)
        .addSignature<double(openvdb::math::Vec3<int32_t>*)>(generate, length)
        .addSignature<double(openvdb::math::Vec4<double>*)>(generate, length)
        .addSignature<float(openvdb::math::Vec4<float>*)>(generate, length)
        .addSignature<double(openvdb::math::Vec4<int32_t>*)>(generate, length)
        .addDependency("lengthsq")
        .addDependency("sqrt")
        .setArgumentNames({"v"})
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the length of the given vector")
        .get();
}

inline FunctionGroup::UniquePtr axnormalize(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
             llvm::IRBuilder<>& B) -> llvm::Value*
    {
        auto a = axlength(op);
        llvm::Value* len = a->execute({args[1]}, B);

        std::vector<llvm::Value*> ptrs, elements;
        arrayUnpack(args[0], ptrs, B, /*load*/false);
        arrayUnpack(args[1], elements, B, /*load*/true);
        assert(ptrs.size() == 3 || ptrs.size() == 4);
        assert(elements.size() == 3 || elements.size() == 4);

        if (elements[0]->getType()->isIntegerTy()) {
           arithmeticConversion(elements, LLVMType<double>::get(B.getContext()), B);
        }

        // the following is always done at fp precision
        llvm::Value* one = llvm::ConstantFP::get(len->getType(), 1.0);
        llvm::Value* oneDividedByLength = B.CreateFDiv(one, len);

        elements[0] = B.CreateFMul(elements[0], oneDividedByLength);
        elements[1] = B.CreateFMul(elements[1], oneDividedByLength);
        elements[2] = B.CreateFMul(elements[2], oneDividedByLength);
        if (elements.size() == 4) elements[3] = B.CreateFMul(elements[3], oneDividedByLength);

        B.CreateStore(elements[0], ptrs[0]);
        B.CreateStore(elements[1], ptrs[1]);
        B.CreateStore(elements[2], ptrs[2]);
        if (elements.size() == 4)  B.CreateStore(elements[3], ptrs[3]);

        return nullptr;
    };

    static auto norm = [](auto out, auto in) {
        using VecType = typename std::remove_pointer<decltype(out)>::type;
        using ElementT = typename openvdb::VecTraits<VecType>::ElementType;
        *out = *in; // copy
        out->normalize(ElementT(0.0));
    };

    using Normalize3D = void(openvdb::math::Vec3<double>*,openvdb::math::Vec3<double>*);
    using Normalize3F = void(openvdb::math::Vec3<float>*,openvdb::math::Vec3<float>*);
    using Normalize3I = void(openvdb::math::Vec3<double>*, openvdb::math::Vec3<int32_t>*);
    using Normalize4D = void(openvdb::math::Vec4<double>*,openvdb::math::Vec4<double>*);
    using Normalize4F = void(openvdb::math::Vec4<float>*,openvdb::math::Vec4<float>*);
    using Normalize4I = void(openvdb::math::Vec4<double>*, openvdb::math::Vec4<int32_t>*);

    return FunctionBuilder("normalize")
        .addSignature<Normalize3D, true>(generate, (Normalize3D*)(norm))
        .addSignature<Normalize3F, true>(generate, (Normalize3F*)(norm))
        .addSignature<Normalize3I, true>(generate, (Normalize3I*)(norm))
        .addSignature<Normalize4D, true>(generate, (Normalize4D*)(norm))
        .addSignature<Normalize4F, true>(generate, (Normalize4F*)(norm))
        .addSignature<Normalize4I, true>(generate, (Normalize4I*)(norm))
        .addDependency("length")
        .setArgumentNames({"v"})
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the normalized result of the given vector.")
        .get();
}

inline FunctionGroup::UniquePtr axlerp(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 3);
        llvm::Value* a = args[0], *b = args[1], *t = args[2];
        llvm::Value* zero = llvm::ConstantFP::get(a->getType(), 0.0);
        llvm::Value* one = llvm::ConstantFP::get(a->getType(), 1.0);
        llvm::Function* base = B.GetInsertBlock()->getParent();

        // @todo short-circuit?
        llvm::Value* a1 = binaryOperator(a, zero, ast::tokens::LESSTHANOREQUAL, B);
        llvm::Value* b1 = binaryOperator(b, zero, ast::tokens::MORETHANOREQUAL, B);
        llvm::Value* a2 = binaryOperator(a, zero, ast::tokens::MORETHANOREQUAL, B);
        llvm::Value* b2 = binaryOperator(b, zero, ast::tokens::LESSTHANOREQUAL, B);
        a1 = binaryOperator(a1, b1, ast::tokens::AND, B);
        a2 = binaryOperator(a2, b2, ast::tokens::AND, B);
        a1 = binaryOperator(a1, a2, ast::tokens::OR, B);

        llvm::BasicBlock* then = llvm::BasicBlock::Create(B.getContext(), "then", base);
        llvm::BasicBlock* post = llvm::BasicBlock::Create(B.getContext(), "post", base);
        B.CreateCondBr(a1, then, post);

        B.SetInsertPoint(then);
        llvm::Value* r = binaryOperator(one, t, ast::tokens::MINUS, B);
        r = binaryOperator(r, a, ast::tokens::MULTIPLY, B);
        llvm::Value* right = binaryOperator(t, b, ast::tokens::MULTIPLY, B);
        r = binaryOperator(r, right, ast::tokens::PLUS, B);
        B.CreateRet(r);

        B.SetInsertPoint(post);

        llvm::Value* tisone = binaryOperator(t, one, ast::tokens::EQUALSEQUALS, B);

        then = llvm::BasicBlock::Create(B.getContext(), "then", base);
        post = llvm::BasicBlock::Create(B.getContext(), "post", base);
        B.CreateCondBr(tisone, then, post);

        B.SetInsertPoint(then);
        B.CreateRet(b);

        B.SetInsertPoint(post);

        // if nlerp
        llvm::Value* x = binaryOperator(b, a, ast::tokens::MINUS, B);
        x = binaryOperator(t, x, ast::tokens::MULTIPLY, B);
        x = binaryOperator(a, x, ast::tokens::PLUS, B);

        then = llvm::BasicBlock::Create(B.getContext(), "then", base);
        post = llvm::BasicBlock::Create(B.getContext(), "post", base);

        a1 = binaryOperator(t, one, ast::tokens::MORETHAN, B);
        a2 = binaryOperator(b, a, ast::tokens::MORETHAN, B);
        a1 = binaryOperator(a1, a2, ast::tokens::EQUALSEQUALS, B);
        B.CreateCondBr(a1, then, post);

        B.SetInsertPoint(then);
        b1 = binaryOperator(b, x, ast::tokens::LESSTHAN, B);
        B.CreateRet(B.CreateSelect(b1, x, b));

        B.SetInsertPoint(post);
        b1 = binaryOperator(x, b, ast::tokens::LESSTHAN, B);
        return B.CreateRet(B.CreateSelect(b1, x, b));
    };

    // This lerp implementation is taken from clang and matches the C++20 standard
    static auto lerp = [](auto a, auto b, auto t) -> auto
    {
        using ValueT = decltype(a);
        // If there is a zero crossing with a,b do the more precise
        // linear interpolation. This is only monotonic if there is
        // a zero crossing
        // Exact, monotonic, bounded, determinate, and (for a=b=0)
        // consistent
        if ((a <= 0 && b >= 0) || (a >= 0 && b <= 0)) {
            return t * b + (ValueT(1.0) - t) * a;
        }
        // If t is exactly 1, return b (as the second impl doesn't
        // guarantee this due to fp arithmetic)
        if (t == ValueT(1.0)) return b;
        // less precise interpolation when there is no crossing
        // Exact at t=0, monotonic except near t=1, bounded,
        // determinate, and consistent
        const auto x = a + t * (b - a);
        // Ensure b is preferred to another equal value (i.e. -0. vs. +0.),
        // which avoids returning -0 for t==1 but +0 for other nearby
        // values of t. This branching also stops nans being returns from
        // inf inputs
        // monotonic near t=1
        if ((t > ValueT(1.0)) == (b > a)) return b < x ? x : b;
        else                              return x < b ? x : b;
    };

    return FunctionBuilder("lerp")
        .addSignature<double(double,double,double)>(generate, (double(*)(double,double,double))(lerp))
        .addSignature<float(float,float,float)>(generate, (float(*)(float,float,float))(lerp))
        .setArgumentNames({"a", "b", "amount"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Performs bilinear interpolation between the values. If the "
            "amount is outside the range 0 to 1, the values will be extrapolated linearly. "
            "If amount is 0, the first value is returned. If it is 1, the second value "
            "is returned. This implementation is guaranteed to be monotonic.")
        .get();
}

inline FunctionGroup::UniquePtr axmin(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        llvm::Value* result =
            binaryOperator(args[0], args[1], ast::tokens::MORETHAN, B);
        return B.CreateSelect(result, args[1], args[0]);
    };

    static auto min = [](auto a, auto b) -> auto {
        return std::min(a, b);
    };

    return FunctionBuilder("min")
        .addSignature<double(double,double)>(generate, (double(*)(double,double))(min))
        .addSignature<float(float,float)>(generate, (float(*)(float,float))(min))
        .addSignature<int64_t(int64_t,int64_t)>(generate, (int64_t(*)(int64_t,int64_t))(min))
        .addSignature<int32_t(int32_t,int32_t)>(generate, (int32_t(*)(int32_t,int32_t))(min))
        .setArgumentNames({"a", "b"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the smaller of the given values.")
        .get();
}

inline FunctionGroup::UniquePtr axmax(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        llvm::Value* result =
            binaryOperator(args[0], args[1], ast::tokens::MORETHAN, B);
        return B.CreateSelect(result, args[0], args[1]);
    };

    static auto max = [](auto a, auto b) -> auto {
        return std::max(a, b);
    };

    return FunctionBuilder("max")
        .addSignature<double(double,double)>(generate, (double(*)(double,double))(max))
        .addSignature<float(float,float)>(generate, (float(*)(float,float))(max))
        .addSignature<int64_t(int64_t,int64_t)>(generate, (int64_t(*)(int64_t,int64_t))(max))
        .addSignature<int32_t(int32_t,int32_t)>(generate, (int32_t(*)(int32_t,int32_t))(max))
        .setArgumentNames({"a", "b"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the larger of the given values.")
        .get();
}

inline FunctionGroup::UniquePtr axclamp(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
             llvm::IRBuilder<>& B) -> llvm::Value*
    {
        llvm::Value* min = axmax(op)->execute({args[0], args[1]}, B);
        llvm::Value* result = axmin(op)->execute({min, args[2]}, B);
        return result;
    };

    using ClampD = double(double, double, double);
    using ClampF = float(float, float, float);
    using ClampI = int32_t(int32_t, int32_t, int32_t);
    using ClampL = int64_t(int64_t, int64_t, int64_t);

    return FunctionBuilder("clamp")
        .addSignature<ClampD>(generate, &openvdb::math::Clamp<double>)
        .addSignature<ClampF>(generate, &openvdb::math::Clamp<float>)
        .addSignature<ClampL>(generate, &openvdb::math::Clamp<int64_t>)
        .addSignature<ClampI>(generate, &openvdb::math::Clamp<int32_t>)
        .addDependency("min")
        .addDependency("max")
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setArgumentNames({"in", "min", "max"})
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Clamps the first argument to the minimum second argument "
            "value and maximum third argument value")
        .get();
}

inline FunctionGroup::UniquePtr axfit(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
             llvm::IRBuilder<>& B) -> llvm::Value*
    {
        //         (outMax - outMin)(x - inMin)
        // f(x) = ----------------------------  + outMin
        //                inMax - inMin
        // if inMax == inMin, f(x) = (outMax + outMin) / 2.0

        // NOTE: this also performs a clamp on the ordered input range
        // @TODO revisit. If this is the best thing to do, should add conditional
        // branching so that the clamping math is never executed when the value
        // is inside

        std::vector<llvm::Value*> argcopy(args);

        // select the precision at which to perform

        llvm::Type* precision = argcopy[0]->getType();
        if (precision->isIntegerTy()) {
            precision = LLVMType<double>::get(B.getContext());
        }

        // See if the input range has a valid magnitude .i.e. the values are not the same

        llvm::Value* isInputRangeValid =
            binaryOperator(argcopy[1], argcopy[2], ast::tokens::NOTEQUALS, B);

        // clamp the input to the ORDERED inMin to inMax range

        llvm::Value* minRangeComp =
            binaryOperator(argcopy[1], argcopy[2], ast::tokens::LESSTHAN, B);
        llvm::Value* minInputRange = B.CreateSelect(minRangeComp, argcopy[1], argcopy[2]);
        llvm::Value* maxInputRange = B.CreateSelect(minRangeComp, argcopy[2], argcopy[1]);

        // clamp
        {
            auto clamp = axclamp(op);
            argcopy[0] = clamp->execute({ argcopy[0], minInputRange, maxInputRange }, B);
        }

        // cast all (the following requires floating point precision)

        for (auto& arg : argcopy) arg = arithmeticConversion(arg, precision, B);

        llvm::Value* valueMinusMin = B.CreateFSub(argcopy[0], argcopy[1]);
        llvm::Value* inputRange = B.CreateFSub(argcopy[2], argcopy[1]);
        llvm::Value* outputRange = B.CreateFSub(argcopy[4], argcopy[3]);

        llvm::Value* result = B.CreateFMul(outputRange, valueMinusMin);
        result = B.CreateFDiv(result, inputRange);  // NOTE - This can cause division by zero
        result = B.CreateFAdd(argcopy[3], result);

        // calculate the output range over 2 and use this value if the input range is invalid

        llvm::Value* outputRangeOverTwo = B.CreateFAdd(argcopy[3], argcopy[4]);
        llvm::Value* two = llvm::ConstantFP::get(precision, 2.0);
        outputRangeOverTwo = B.CreateFDiv(outputRangeOverTwo, two);

        return B.CreateSelect(isInputRangeValid, result, outputRangeOverTwo);
    };

    using FitD = double(double, double, double, double, double);
    using FitF = float(float, float, float, float, float);
    using FitL = double(int64_t, int64_t, int64_t, int64_t, int64_t);
    using FitI = double(int32_t, int32_t, int32_t, int32_t, int32_t);

    return FunctionBuilder("fit")
        .addSignature<FitD>(generate)
        .addSignature<FitF>(generate)
        .addSignature<FitL>(generate)
        .addSignature<FitI>(generate)
        .addDependency("clamp")
        .setArgumentNames({"value", "omin", "omax", "nmin", "nmax"})
        .setConstantFold(false)
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Fit the first argument to the output range by "
            "first clamping the value between the second and third input range "
            "arguments and then remapping the result to the output range fourth and "
            "fifth arguments")
        .get();
}

inline FunctionGroup::UniquePtr axrand(const FunctionOptions& op)
{
    struct Rand
    {
        static double rand(const std::mt19937_64::result_type* seed)
        {
            using ThreadLocalEngineContainer =
                tbb::enumerable_thread_specific<std::mt19937_64>;
            static ThreadLocalEngineContainer ThreadLocalEngines;
            static std::uniform_real_distribution<double> Generator(0.0,1.0);
            std::mt19937_64& engine = ThreadLocalEngines.local();
            if (seed) {
                engine.seed(static_cast<std::mt19937_64::result_type>(*seed));
            }
            return Generator(engine);
        }

        static double rand() { return Rand::rand(nullptr); }

        static double rand(double seed)
        {
            const std::mt19937_64::result_type hash =
                static_cast<std::mt19937_64::result_type>(std::hash<double>()(seed));
            return Rand::rand(&hash);
        }

        static double rand(int64_t seed)
        {
            const std::mt19937_64::result_type hash =
                static_cast<std::mt19937_64::result_type>(seed);
            return Rand::rand(&hash);
        }
    };

    return FunctionBuilder("rand")
        .addSignature<double()>((double(*)())(Rand::rand))
        .addSignature<double(double)>((double(*)(double))(Rand::rand))
        .addSignature<double(int64_t)>((double(*)(int64_t))(Rand::rand))
        .setArgumentNames({"seed"})
        // We can't constant fold rand even if it's been called with a constant as
        // it will leave the generator in a different state in comparison to other
        // threads and, as it relies on an internal state, doesn't respect branching
        // etc. We also can't use a different generate for constant calls as subsequent
        // calls to rand() without an argument won't know to advance the generator.
        .setConstantFold(false)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Creates a random number based on the provided "
            "seed. The number will be in the range of 0 to 1. The same number is "
            "produced for the same seed. Note that if rand is called without a seed "
            "the previous state of the random number generator is advanced for the "
            "currently processing element. This state is determined by the last call to "
            "rand() with a given seed. If rand is not called with a seed, the generator "
            "advances continuously across different elements which can produce non-"
            "deterministic results. It is important that rand is always called with a "
            "seed at least once for deterministic results.")
        .get();
}

inline FunctionGroup::UniquePtr axrand32(const FunctionOptions& op)
{
    struct Rand
    {
        static double rand(const std::mt19937::result_type* seed)
        {
            using ThreadLocalEngineContainer =
                tbb::enumerable_thread_specific<std::mt19937>;
            // Obtain thread-local engine (or create if it doesn't exist already).
            static ThreadLocalEngineContainer ThreadLocalEngines;
            static std::uniform_real_distribution<double> Generator(0.0,1.0);
            std::mt19937& engine = ThreadLocalEngines.local();
            if (seed) {
                engine.seed(static_cast<std::mt19937::result_type>(*seed));
            }
            // Once we have seeded the random number generator, we then evaluate it,
            // which returns a floating point number in the range [0,1)
            return Generator(engine);
        }

        static double rand() { return Rand::rand(nullptr); }

        static double rand(double seed)
        {
            // We initially hash the double-precision seed with `std::hash`. The
            // important thing about the hash is that it produces a "reliable" hash value,
            // taking into account a number of special cases for floating point numbers
            // (e.g. -0 and +0 must return the same hash value, etc). Other than these
            // special cases, this function will usually just copy the binary
            // representation of a float into the resultant `size_t`
            const size_t hash = std::hash<double>()(seed);

            // Now that we have a reliable hash (with special floating-point cases taken
            // care of), we proceed to use this hash to seed a random number generator.
            // The generator takes an unsigned int, which is not guaranteed to be the
            // same size as size_t.
            //
            // So, we must convert it. I should note that the OpenVDB math libraries will
            // do this for us, but its implementation static_casts `size_t` to `unsigned int`,
            // and because `std::hash` returns a binary copy of the original
            // double-precision number in almost all cases, this ends up producing noticable
            // patterns in the result (e.g. by truncating the upper 4 bytes, values of 1.0,
            // 2.0, 3.0, and 4.0 all return the same hash value because their lower 4 bytes
            // are all zero).
            //
            // We use the `hashToSeed` function to reduce our `size_t` to an `unsigned int`,
            // whilst taking all bits in the `size_t` into account.
            // On some architectures std::uint_fast32_t may be size_t, but we always hash to
            // be consistent.
            const std::mt19937::result_type uintseed =
                static_cast<std::mt19937::result_type>(hashToSeed<uint32_t>(hash));
            return Rand::rand(&uintseed);
        }

        static double rand(int32_t seed)
        {
            const std::mt19937::result_type uintseed =
                static_cast<std::mt19937::result_type>(seed);
            return Rand::rand(&uintseed);
        }
    };

    return FunctionBuilder("rand32")
        .addSignature<double()>((double(*)())(Rand::rand))
        .addSignature<double(double)>((double(*)(double))(Rand::rand))
        .addSignature<double(int32_t)>((double(*)(int32_t))(Rand::rand))
        .setArgumentNames({"seed"})
        // We can't constant fold rand even if it's been called with a constant as
        // it will leave the generator in a different state in comparison to other
        // threads and, as it relies on an internal state, doesn't respect branching
        // etc. We also can't use a different generate for constant calls as subsequent
        // calls to rand() without an argument won't know to advance the generator.
        .setConstantFold(false)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Creates a random number based on the provided 32 bit "
            "seed. The number will be in the range of 0 to 1. The same number is "
            "produced for the same seed. "
            "NOTE: This function does not share the same random number generator as "
            "rand(). rand32() may provide a higher throughput on some architectures, "
            "but will produce different results to rand(). "
            "NOTE: If rand32 is called without a seed the previous state of the random "
            "number generator is advanced for the currently processing element. This "
            "state is determined by the last call to rand32() with a given seed. If "
            "rand32 is not called with a seed, the generator advances continuously "
            "across different elements which can produce non-deterministic results. "
            "It is important that rand32 is always called with a seed at least once "
            "for deterministic results.")
        .get();
}

inline FunctionGroup::UniquePtr axsign(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
            llvm::IRBuilder<>& B) -> llvm::Value*
    {
        // int r = (T(0) < val) - (val < T(0));
        assert(args.size() == 1);
        llvm::Value* arg = args.front();
        llvm::Type* type = arg->getType();
        llvm::Value* zero;
        if (type->isIntegerTy()) {
            zero = llvm::ConstantInt::get(type, static_cast<uint64_t>(0), /*signed*/true);
        }
        else {
            assert(type->isFloatingPointTy());
            zero = llvm::ConstantFP::get(type, static_cast<double>(0.0));
        }

        llvm::Value* c1 = binaryOperator(zero, arg, ast::tokens::LESSTHAN, B);
        c1 = arithmeticConversion(c1, LLVMType<int32_t>::get(B.getContext()), B);
        llvm::Value* c2 = binaryOperator(arg, zero, ast::tokens::LESSTHAN, B);
        c2 = arithmeticConversion(c2, LLVMType<int32_t>::get(B.getContext()), B);
        llvm::Value* r = binaryOperator(c1, c2, ast::tokens::MINUS, B);
        return arithmeticConversion(r, LLVMType<int32_t>::get(B.getContext()), B);
    };

    return FunctionBuilder("sign")
        .addSignature<int32_t(double)>(generate)
        .addSignature<int32_t(float)>(generate)
        .addSignature<int32_t(int64_t)>(generate)
        .addSignature<int32_t(int32_t)>(generate)
        .setArgumentNames({"n"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Implements signum, determining if the input is negative, zero "
            "or positive. Returns -1 for a negative number, 0 for the number zero, and +1 "
            "for a positive number. Note that this function does not check the sign of "
            "floating point +/-0.0 values. See signbit().")
        .get();
}

inline FunctionGroup::UniquePtr axsignbit(const FunctionOptions& op)
{
    return FunctionBuilder("signbit")
        .addSignature<bool(double)>((bool(*)(double))(std::signbit))
        .addSignature<bool(float)>((bool(*)(float))(std::signbit))
        .setArgumentNames({"n"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Determines if the given floating point number input is negative. "
            "Returns true if arg is negative, false otherwise. Will return true for -0.0, "
            "false for +0.0")
        .get();
}

inline FunctionGroup::UniquePtr axtruncatemod(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 2);
        return binaryOperator(args[0], args[1], ast::tokens::MODULO, B);
    };

    return FunctionBuilder("truncatemod")
        .addSignature<double(double,double)>(generate)
        .addSignature<float(float,float)>(generate)
        .addSignature<int64_t(int64_t,int64_t)>(generate)
        .addSignature<int32_t(int32_t,int32_t)>(generate)
        .setArgumentNames({"dividend", "divisor"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Truncated modulo, where the result of the division operator "
            "on (dividend / divisor) is truncated. The remainder is thus calculated with "
            "D - d * trunc(D/d). This is equal to the C/C++ % implementation. This is NOT "
            "equal to a%b in AX. See floormod(), euclideanmod().")
        .get();
}

inline FunctionGroup::UniquePtr axfloormod(const FunctionOptions& op)
{
    static auto ifmod = [](auto D, auto d) -> auto
    {
        using ValueType = decltype(D);
        auto r = D % d; // tmod
        if ((r > 0 && d < 0) || (r < 0 && d > 0)) r = r+d;
        return ValueType(r);
    };

    static auto ffmod = [](auto D, auto d) -> auto
    {
        auto r = std::fmod(D, d);
        if ((r > 0 && d < 0) || (r < 0 && d > 0)) r = r+d;
        return r;
    };

    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 2);
        llvm::Value* D = args[0];
        llvm::Value* d = args[1];
        // tmod
        llvm::Value* r = binaryOperator(D, d, ast::tokens::MODULO, B);

        llvm::Value* zero = llvmConstant(0, D->getType());
        llvm::Value* a1 = binaryOperator(r, zero, ast::tokens::MORETHAN, B);
        llvm::Value* a2 = binaryOperator(d, zero, ast::tokens::LESSTHAN, B);
        a1 = binaryOperator(a1, a2, ast::tokens::AND, B);
        llvm::Value* b1 = binaryOperator(r, zero, ast::tokens::LESSTHAN, B);
        llvm::Value* b2 = binaryOperator(d, zero, ast::tokens::MORETHAN, B);
        b1 = binaryOperator(b1, b2, ast::tokens::AND, B);
        a1 = binaryOperator(a1, b1, ast::tokens::OR, B);

        llvm::Value* rplus = binaryOperator(r, d, ast::tokens::PLUS, B);
        return B.CreateSelect(a1, rplus, r);
    };

    return FunctionBuilder("floormod")
        .addSignature<double(double,double)>(generate, (double(*)(double,double))(ffmod))
        .addSignature<float(float,float)>(generate, (float(*)(float,float))(ffmod))
        .addSignature<int64_t(int64_t,int64_t)>(generate, (int64_t(*)(int64_t,int64_t))(ifmod))
        .addSignature<int32_t(int32_t,int32_t)>(generate, (int32_t(*)(int32_t,int32_t))(ifmod))
        .setArgumentNames({"dividend", "divisor"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Floored modulo, where the result of the division operator "
            "on (dividend / divisor) is floored. The remainder is thus calculated with "
            "D - d * floor(D/d). This is the implemented modulo % operator of AX. This is "
            "equal to the python % implementation. See trucnatemod(), euclideanmod().")
        .get();
}

inline FunctionGroup::UniquePtr axeuclideanmod(const FunctionOptions& op)
{
    static auto iemod = [](auto D, auto d) -> auto
    {
        using ValueType = decltype(D);
        auto r = D%d;
        if (r < 0) {
            if (d > 0) r = r + d;
            else       r = r - d;
        }
        return ValueType(r);
    };

    static auto femod = [](auto D, auto d) -> auto
    {
        auto r = std::fmod(D, d);
        if (r < 0) {
            if (d > 0) r = r + d;
            else       r = r - d;
        }
        return r;
    };

    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 2);
        llvm::Value* D = args[0], *d = args[1];
        llvm::Value* r = binaryOperator(D, d, ast::tokens::MODULO, B); // tmod

        llvm::Value* zero = llvmConstant(0, D->getType());
        llvm::Value* a1 = binaryOperator(d, zero, ast::tokens::MORETHAN, B);
        llvm::Value* rplus = binaryOperator(r, d, ast::tokens::PLUS, B);
        llvm::Value* rminus = binaryOperator(r, d, ast::tokens::MINUS, B);
        llvm::Value* rd = B.CreateSelect(a1, rplus, rminus);

        a1 = binaryOperator(r, zero, ast::tokens::LESSTHAN, B);
        return B.CreateSelect(a1, rd, r);
    };

    return FunctionBuilder("euclideanmod")
        .addSignature<double(double,double)>(generate, (double(*)(double,double))(femod))
        .addSignature<float(float,float)>(generate, (float(*)(float,float))(femod))
        .addSignature<int64_t(int64_t,int64_t)>(generate, (int64_t(*)(int64_t,int64_t))(iemod))
        .addSignature<int32_t(int32_t,int32_t)>(generate, (int32_t(*)(int32_t,int32_t))(iemod))
        .setArgumentNames({"dividend", "divisor"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Euclidean modulo, where by the result of the division operator "
            "on (dividend / divisor) is floored or ceiled depending on its sign, guaranteeing "
            "that the return value is always positive. The remainder is thus calculated with "
            "D - d * (d < 0 ? ceil(D/d) : floor(D/d)). This is NOT equal to a%b in AX. See "
            "truncatemod(), floormod().")
        .get();
}

inline FunctionGroup::UniquePtr axisfinite(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 1);
        llvm::Value* arg = args[0];
        llvm::Type* etype = arg->getType();
        if (etype->isPointerTy()) {
            etype = etype->getPointerElementType()->getArrayElementType();
        }

        llvm::Value* inf;
        if (etype->isFloatTy()) {
            const llvm::APFloat apinf =
                llvm::APFloat::getInf(llvm::APFloatBase::IEEEsingle());
            inf = LLVMType<float>::get(B.getContext(), apinf.convertToFloat());
        }
        else {
            assert(etype->isDoubleTy());
            const llvm::APFloat apinf =
                llvm::APFloat::getInf(llvm::APFloatBase::IEEEdouble());
            inf = LLVMType<double>::get(B.getContext(), apinf.convertToDouble());
        }

        if (!arg->getType()->isPointerTy()) {
            arg = llvm_fabs(op)->execute({arg}, B);
            return binaryOperator(inf, arg, ast::tokens::NOTEQUALS, B);
        }

        std::vector<llvm::Value*> array;
        arrayUnpack(arg, array, B, /*load*/true);

        // @todo short-circuit?
        llvm::Value* result = B.getTrue();
        for (auto& value : array) {
            value = llvm_fabs(op)->execute({value}, B);
            llvm::Value* comp = binaryOperator(inf, value, ast::tokens::NOTEQUALS, B);
            result = binaryOperator(comp, result, ast::tokens::AND, B);
        }
        return result;
    };

    static auto isfinitearray = [](const auto input) -> bool
    {
        return input->isFinite();
    };

    return FunctionBuilder("isfinite")
        .addSignature<bool(openvdb::Vec2d*)>(generate, (bool(*)(openvdb::Vec2d*))(isfinitearray))
        .addSignature<bool(openvdb::Vec2f*)>(generate, (bool(*)(openvdb::Vec2f*))(isfinitearray))
        .addSignature<bool(openvdb::Vec3d*)>(generate, (bool(*)(openvdb::Vec3d*))(isfinitearray))
        .addSignature<bool(openvdb::Vec3f*)>(generate, (bool(*)(openvdb::Vec3f*))(isfinitearray))
        .addSignature<bool(openvdb::Vec4d*)>(generate, (bool(*)(openvdb::Vec4d*))(isfinitearray))
        .addSignature<bool(openvdb::Vec4f*)>(generate, (bool(*)(openvdb::Vec4f*))(isfinitearray))
        .addSignature<bool(openvdb::math::Mat3<float>*)>(generate, (bool(*)(openvdb::math::Mat3<float>*))(isfinitearray))
        .addSignature<bool(openvdb::math::Mat3<double>*)>(generate, (bool(*)(openvdb::math::Mat3<double>*))(isfinitearray))
        .addSignature<bool(openvdb::math::Mat4<float>*)>(generate, (bool(*)(openvdb::math::Mat4<float>*))(isfinitearray))
        .addSignature<bool(openvdb::math::Mat4<double>*)>(generate, (bool(*)(openvdb::math::Mat4<double>*))(isfinitearray))
            .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addSignature<bool(double)>(generate, (bool(*)(double))(std::isfinite))
        .addSignature<bool(float)>(generate, (bool(*)(float))(std::isfinite))
        .setArgumentNames({"arg"})
        .addDependency("fabs")
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns whether the value is finite i.e. not infinite or NaN. "
                          "For matrix and vector types will return false if any element is not finite.")
        .get();
}

inline FunctionGroup::UniquePtr axisinf(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 1);
        llvm::Value* arg = args[0];
        llvm::Type* etype = arg->getType();
        if (etype->isPointerTy()) {
            etype = etype->getPointerElementType()->getArrayElementType();
        }

        llvm::Value* inf;
        if (etype->isFloatTy()) {
            const llvm::APFloat apinf =
                llvm::APFloat::getInf(llvm::APFloatBase::IEEEsingle());
            inf = LLVMType<float>::get(B.getContext(), apinf.convertToFloat());
        }
        else {
            assert(etype->isDoubleTy());
            const llvm::APFloat apinf =
                llvm::APFloat::getInf(llvm::APFloatBase::IEEEdouble());
            inf = LLVMType<double>::get(B.getContext(), apinf.convertToDouble());
        }

        if (!arg->getType()->isPointerTy()) {
            arg = llvm_fabs(op)->execute({arg}, B);
            return binaryOperator(inf, arg, ast::tokens::EQUALSEQUALS, B);
        }

        std::vector<llvm::Value*> array;
        arrayUnpack(arg, array, B, /*load*/true);

        // @todo short-circuit?
        llvm::Value* result = B.getFalse();
        for (auto& value : array) {
            value = llvm_fabs(op)->execute({value}, B);
            llvm::Value* comp = binaryOperator(inf, value, ast::tokens::EQUALSEQUALS, B);
            result = binaryOperator(comp, result, ast::tokens::OR, B);
        }
        return result;
    };

    static auto isinfarray = [](const auto input) -> bool
    {
        return input->isInfinite();
    };

    return FunctionBuilder("isinf")
        .addSignature<bool(openvdb::Vec2d*)>(generate, (bool(*)(openvdb::Vec2d*))(isinfarray))
        .addSignature<bool(openvdb::Vec2f*)>(generate, (bool(*)(openvdb::Vec2f*))(isinfarray))
        .addSignature<bool(openvdb::Vec3d*)>(generate, (bool(*)(openvdb::Vec3d*))(isinfarray))
        .addSignature<bool(openvdb::Vec3f*)>(generate, (bool(*)(openvdb::Vec3f*))(isinfarray))
        .addSignature<bool(openvdb::Vec4d*)>(generate, (bool(*)(openvdb::Vec4d*))(isinfarray))
        .addSignature<bool(openvdb::Vec4f*)>(generate, (bool(*)(openvdb::Vec4f*))(isinfarray))
        .addSignature<bool(openvdb::math::Mat3<float>*)>(generate, (bool(*)(openvdb::math::Mat3<float>*))(isinfarray))
        .addSignature<bool(openvdb::math::Mat3<double>*)>(generate, (bool(*)(openvdb::math::Mat3<double>*))(isinfarray))
        .addSignature<bool(openvdb::math::Mat4<float>*)>(generate, (bool(*)(openvdb::math::Mat4<float>*))(isinfarray))
        .addSignature<bool(openvdb::math::Mat4<double>*)>(generate, (bool(*)(openvdb::math::Mat4<double>*))(isinfarray))
            .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addSignature<bool(double)>(generate /*, (bool(*)(double))(std::isinf)*/) // @note: gcc needs _GLIBCXX_NO_OBSOLETE_ISINF_ISNAN_DYNAMIC defined
        .addSignature<bool(float)>(generate, (bool(*)(float))(std::isinf))
        .setArgumentNames({"arg"})
        .addDependency("fabs")
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns whether the value is inf. "
                          "For matrix and vector types will return true if any element is inf.")
        .get();
}

inline FunctionGroup::UniquePtr axisnan(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        // uno (unordered) comparison with self
        // https://llvm.org/docs/LangRef.html#fcmp-instruction
        assert(args.size() == 1);
        llvm::Value* arg = args[0];
        if (!arg->getType()->isPointerTy()) {
            return B.CreateFCmpUNO(arg, arg);
        }

        std::vector<llvm::Value*> array;
        arrayUnpack(arg, array, B, /*load*/true);

        // @todo short-circuit?
        llvm::Value* result = B.getFalse();
        for (auto& value : array) {
            llvm::Value* comp = B.CreateFCmpUNO(value, value);
            result = binaryOperator(comp, result, ast::tokens::OR, B);
        }
        return result;
    };

    static auto isnanarray = [](const auto input) -> bool
    {
        return input->isNan();
    };

    return FunctionBuilder("isnan")
        .addSignature<bool(openvdb::Vec2d*)>(generate, (bool(*)(openvdb::Vec2d*))(isnanarray))
        .addSignature<bool(openvdb::Vec2f*)>(generate, (bool(*)(openvdb::Vec2f*))(isnanarray))
        .addSignature<bool(openvdb::Vec3d*)>(generate, (bool(*)(openvdb::Vec3d*))(isnanarray))
        .addSignature<bool(openvdb::Vec3f*)>(generate, (bool(*)(openvdb::Vec3f*))(isnanarray))
        .addSignature<bool(openvdb::Vec4d*)>(generate, (bool(*)(openvdb::Vec4d*))(isnanarray))
        .addSignature<bool(openvdb::Vec4f*)>(generate, (bool(*)(openvdb::Vec4f*))(isnanarray))
        .addSignature<bool(openvdb::math::Mat3<float>*)>(generate, (bool(*)(openvdb::math::Mat3<float>*))(isnanarray))
        .addSignature<bool(openvdb::math::Mat3<double>*)>(generate, (bool(*)(openvdb::math::Mat3<double>*))(isnanarray))
        .addSignature<bool(openvdb::math::Mat4<float>*)>(generate, (bool(*)(openvdb::math::Mat4<float>*))(isnanarray))
        .addSignature<bool(openvdb::math::Mat4<double>*)>(generate, (bool(*)(openvdb::math::Mat4<double>*))(isnanarray))
            .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addSignature<bool(double)>(generate/*, (bool(*)(double))(std::isnan)*/) // @note: gcc needs _GLIBCXX_NO_OBSOLETE_ISINF_ISNAN_DYNAMIC defined
        .addSignature<bool(float)>(generate, (bool(*)(float))(std::isnan))
        .setArgumentNames({"arg"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns whether the value is NaN (not-a-number).")
        .get();
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

// Matrix math

inline FunctionGroup::UniquePtr axdeterminant(const FunctionOptions& op)
{
    // 3 by 3 determinant
    static auto generate3 =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> m1;
        arrayUnpack(args[0], m1, B, /*load*/true);
        assert(m1.size() == 9);

        llvm::Value* e1 = binaryOperator(m1[4], m1[8], ast::tokens::MULTIPLY, B);
        llvm::Value* e2 = binaryOperator(m1[5], m1[7], ast::tokens::MULTIPLY, B);
        llvm::Value* c0 = binaryOperator(e1, e2, ast::tokens::MINUS, B);

        e1 = binaryOperator(m1[5], m1[6], ast::tokens::MULTIPLY, B);
        e2 = binaryOperator(m1[3], m1[8], ast::tokens::MULTIPLY, B);
        llvm::Value* c1 = binaryOperator(e1, e2, ast::tokens::MINUS, B);

        e1 = binaryOperator(m1[3], m1[7], ast::tokens::MULTIPLY, B);
        e2 = binaryOperator(m1[4], m1[6], ast::tokens::MULTIPLY, B);
        llvm::Value* c2 = binaryOperator(e1, e2, ast::tokens::MINUS, B);

        c0 = binaryOperator(m1[0], c0, ast::tokens::MULTIPLY, B);
        c1 = binaryOperator(m1[1], c1, ast::tokens::MULTIPLY, B);
        c2 = binaryOperator(m1[2], c2, ast::tokens::MULTIPLY, B);

        c0 = binaryOperator(c0, c1, ast::tokens::PLUS, B);
        c0 = binaryOperator(c0, c2, ast::tokens::PLUS, B);
        return c0;
    };

    // 4 by 4 determinant
    static auto generate4 =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> m1;
        arrayUnpack(args[0], m1, B, /*load*/true);
        assert(m1.size() == 16);

        // @note  Okay to alloca here as long as embed IR is false
        llvm::Value* subMat = B.CreateAlloca(llvm::ArrayType::get(m1.front()->getType(), 9));
        std::vector<llvm::Value*> elements;
        arrayUnpack(subMat, elements, B, /*load elements*/false);

        llvm::Value* result = llvm::ConstantFP::get(m1.front()->getType(), 0.0);
        for (size_t i = 0; i < 4; ++i) {
            size_t sourceIndex = 0, targetIndex = 0;
            for (size_t j = 0; j < 4; ++j) {
                for (size_t k = 0; k < 4; ++k) {
                    if ((k != i) && (j != 0)) {
                        B.CreateStore(m1[sourceIndex], elements[targetIndex]);
                        ++targetIndex;
                    }
                    ++sourceIndex;
                }
            }
            llvm::Value* subResult = generate3({subMat}, B);
            subResult = binaryOperator(m1[i], subResult, ast::tokens::MULTIPLY, B);

            if (i % 2) result = binaryOperator(result, subResult, ast::tokens::MINUS, B);
            else       result = binaryOperator(result, subResult, ast::tokens::PLUS, B);
        }

        return result;
    };

    static auto determinant = [](auto mat) -> auto {
        return mat->det();
    };

    using DeterminantM3D = double(openvdb::math::Mat3<double>*);
    using DeterminantM3F = float(openvdb::math::Mat3<float>*);
    using DeterminantM4D = double(openvdb::math::Mat4<double>*);
    using DeterminantM4F = float(openvdb::math::Mat4<float>*);

    return FunctionBuilder("determinant")
        .addSignature<DeterminantM3D>(generate3, (DeterminantM3D*)(determinant))
        .addSignature<DeterminantM3F>(generate3, (DeterminantM3F*)(determinant))
        .addSignature<DeterminantM4D>(generate4, (DeterminantM4D*)(determinant))
        .addSignature<DeterminantM4F>(generate4, (DeterminantM4F*)(determinant))
        .setArgumentNames({"mat"})
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the determinant of a matrix.")
        .get();
}

inline FunctionGroup::UniquePtr axdiag(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> ptrs, arg1;
        arrayUnpack(args[0], ptrs, B, /*load*/false);
        arrayUnpack(args[1], arg1, B, /*load*/true);

        const size_t size = arg1.size();
        if (size == 3 || size == 4) {
            //vector - convert to diagonal matrix
            const size_t dim = size*size;
            assert(ptrs.size() == dim);
            llvm::Type* type = arg1.front()->getType();
            llvm::Value* zero = type->isFloatTy() ? LLVMType<float>::get(B.getContext(), 0.0f)
                                    : LLVMType<double>::get(B.getContext(), 0.0);

            for (size_t i = 0, j = 0; i < dim; ++i) {
                llvm::Value* m = zero;
                if (i % (size + 1) == 0) {
                    m = arg1[j];
                    ++j;
                }
                B.CreateStore(m, ptrs[i]);
            }
        }
        else {
            // matrix - convert to vector
            assert(size == 9 || size == 16);
            const size_t dim = size == 9 ? 3 : 4;
            assert(ptrs.size() == dim);
            for (size_t i = 0; i < dim; ++i) {
                B.CreateStore(arg1[i+(i*dim)], ptrs[i]);
            }
        }

        return nullptr;
    };

    static auto diag = [](auto result, const auto input)
    {
        using ValueType = typename std::remove_pointer<decltype(input)>::type;
        using ResultType = typename std::remove_pointer<decltype(result)>::type;
        using ElementT = typename openvdb::ValueTraits<ValueType>::ElementType;
        using RElementT = typename openvdb::ValueTraits<ResultType>::ElementType;

        static_assert(std::is_same<ElementT, RElementT>::value,
            "Input and result arguments for diag are not the same type");

        if (openvdb::ValueTraits<ValueType>::IsVec) {
            // input is a vec, result is a matrix
            const int size = openvdb::ValueTraits<ValueType>::Size;
            int element = 0;
            for (int i = 0; i < size; ++i) {
                for (int j = 0; j < size; ++j) {
                    assert(element < openvdb::ValueTraits<ResultType>::Elements);
                    if (i == j) result->asPointer()[element] = (input->asPointer())[i];
                    else        result->asPointer()[element] = ElementT(0.0);
                    ++element;
                }
            }
        }
        else {
            assert(openvdb::ValueTraits<ValueType>::IsMat);
            // input is a matrix, result is a vec
            const int size = openvdb::ValueTraits<ValueType>::Size;
            for (int i = 0; i < size; ++i) {
                assert(i < openvdb::ValueTraits<ResultType>::Size);
                result->asPointer()[i] = input->asPointer()[i+(i*size)];
            }
        }
    };

    using DiagV3M3D = void(openvdb::math::Vec3<double>*, openvdb::math::Mat3<double>*);
    using DiagV3M3F = void(openvdb::math::Vec3<float>*, openvdb::math::Mat3<float>*);
    using DiagV4M4D = void(openvdb::math::Vec4<double>*, openvdb::math::Mat4<double>*);
    using DiagV4M4F = void(openvdb::math::Vec4<float>*, openvdb::math::Mat4<float>*);

    using DiagM3V3D = void(openvdb::math::Mat3<double>*, openvdb::math::Vec3<double>*);
    using DiagM3V3F = void(openvdb::math::Mat3<float>*, openvdb::math::Vec3<float>*);
    using DiagM4V4D = void(openvdb::math::Mat4<double>*, openvdb::math::Vec4<double>*);
    using DiagM4V4F = void(openvdb::math::Mat4<float>*, openvdb::math::Vec4<float>*);

    return FunctionBuilder("diag")
        .addSignature<DiagV3M3D, true>(generate, (DiagV3M3D*)(diag))
        .addSignature<DiagV3M3F, true>(generate, (DiagV3M3F*)(diag))
        .addSignature<DiagV4M4D, true>(generate, (DiagV4M4D*)(diag))
        .addSignature<DiagV4M4F, true>(generate, (DiagV4M4F*)(diag))
            .setArgumentNames({"vec"})
            .addParameterAttribute(0, llvm::Attribute::WriteOnly)
            .addParameterAttribute(0, llvm::Attribute::NoAlias)
            .addParameterAttribute(1, llvm::Attribute::ReadOnly)
            .addFunctionAttribute(llvm::Attribute::NoUnwind)
            .addFunctionAttribute(llvm::Attribute::InlineHint)
            .setConstantFold(op.mConstantFoldCBindings)
        .addSignature<DiagM3V3D, true>(generate, (DiagM3V3D*)(diag))
        .addSignature<DiagM3V3F, true>(generate, (DiagM3V3F*)(diag))
        .addSignature<DiagM4V4D, true>(generate, (DiagM4V4D*)(diag))
        .addSignature<DiagM4V4F, true>(generate, (DiagM4V4F*)(diag))
            .setArgumentNames({"mat"})
            .addParameterAttribute(0, llvm::Attribute::WriteOnly)
            .addParameterAttribute(0, llvm::Attribute::NoAlias)
            .addParameterAttribute(1, llvm::Attribute::ReadOnly)
            .addFunctionAttribute(llvm::Attribute::NoUnwind)
            .addFunctionAttribute(llvm::Attribute::InlineHint)
            .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Create a diagonal matrix from a vector, or return the diagonal "
            "components of a matrix as a vector.")
        .get();
}

inline FunctionGroup::UniquePtr axidentity3(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> elements;
        arrayUnpack(args[0], elements, B, /*load elements*/false);
        assert(elements.size() == 9);
        llvm::Value* zero = LLVMType<float>::get(B.getContext(), 0.0f);
        llvm::Value* one = LLVMType<float>::get(B.getContext(), 1.0f);
        for (size_t i = 0; i < 9; ++i) {
            llvm::Value* m = ((i == 0 || i == 4 || i == 8) ? one : zero);
            B.CreateStore(m, elements[i]);
        }
        return nullptr;
    };

    return FunctionBuilder("identity3")
        .addSignature<void(openvdb::math::Mat3<float>*), true>(generate)
        .setConstantFold(false)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the 3x3 identity matrix")
        .get();
}

inline FunctionGroup::UniquePtr axidentity4(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> elements;
        arrayUnpack(args[0], elements, B, /*load elements*/false);
        assert(elements.size() == 16);
        llvm::Value* zero = LLVMType<float>::get(B.getContext(), 0.0f);
        llvm::Value* one = LLVMType<float>::get(B.getContext(), 1.0f);
        for (size_t i = 0; i < 16; ++i) {
            llvm::Value* m = ((i == 0 || i == 5 || i == 10 || i == 15) ? one : zero);
            B.CreateStore(m, elements[i]);
        }
        return nullptr;
    };

    return FunctionBuilder("identity4")
        .addSignature<void(openvdb::math::Mat4<float>*), true>(generate)
        .setConstantFold(false)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the 4x4 identity matrix")
        .get();
}

inline FunctionGroup::UniquePtr axmmmult(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> ptrs, m1, m2;
        arrayUnpack(args[0], ptrs, B, /*load*/false);
        arrayUnpack(args[1], m1, B, /*load*/true);
        arrayUnpack(args[2], m2, B, /*load*/true);

        assert(m1.size() == 9 || m1.size() == 16);
        assert(ptrs.size() == m1.size());
        assert(ptrs.size() == m2.size());
        const size_t dim = m1.size() == 9 ? 3 : 4;

        llvm::Value* e3 = nullptr, *e4 = nullptr;
        for (size_t i = 0; i < dim; ++i) {
            const size_t row = i*dim;
            for (size_t j = 0; j < dim; ++j) {
                llvm::Value* e1 = binaryOperator(m1[0+row], m2[j], ast::tokens::MULTIPLY, B);
                llvm::Value* e2 = binaryOperator(m1[1+row], m2[dim+j], ast::tokens::MULTIPLY, B);
                if (dim >=3) e3 = binaryOperator(m1[2+row], m2[(dim*2)+j], ast::tokens::MULTIPLY, B);
                if (dim >=4) e4 = binaryOperator(m1[3+row], m2[(dim*3)+j], ast::tokens::MULTIPLY, B);
                e1 = binaryOperator(e1, e2, ast::tokens::PLUS, B);
                if (dim >=3) e1 = binaryOperator(e1, e3, ast::tokens::PLUS, B);
                if (dim >=4) e1 = binaryOperator(e1, e4, ast::tokens::PLUS, B);
                B.CreateStore(e1, ptrs[row+j]);
            }
        }

        return nullptr;
    };

    static auto mmmult = [](auto out, auto mat2, auto mat1) {
        *out = (*mat1) * (*mat2);
    };

    using MMMultM3D = void(openvdb::math::Mat3<double>*, openvdb::math::Mat3<double>*, openvdb::math::Mat3<double>*);
    using MMMultM3F = void(openvdb::math::Mat3<float>*, openvdb::math::Mat3<float>*, openvdb::math::Mat3<float>*);
    using MMMultM4D = void(openvdb::math::Mat4<double>*, openvdb::math::Mat4<double>*, openvdb::math::Mat4<double>*);
    using MMMultM4F = void(openvdb::math::Mat4<float>*, openvdb::math::Mat4<float>*, openvdb::math::Mat4<float>*);

    return FunctionBuilder("mmmult")
        .addSignature<MMMultM3D, true>(generate, (MMMultM3D*)(mmmult))
        .addSignature<MMMultM3F, true>(generate, (MMMultM3F*)(mmmult))
        .addSignature<MMMultM4D, true>(generate, (MMMultM4D*)(mmmult))
        .addSignature<MMMultM4F, true>(generate, (MMMultM4F*)(mmmult))
        .setArgumentNames({"a", "b"})
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(0, llvm::Attribute::WriteOnly)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addParameterAttribute(2, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Multiplies two matrices together and returns the result")
        .get();
}

inline FunctionGroup::UniquePtr axpolardecompose(const FunctionOptions& op)
{
    static auto polardecompose = [](auto in, auto orth, auto symm) -> bool {
        bool success = false;
        try {
            success = openvdb::math::polarDecomposition(*in, *orth, *symm);
        }
        catch (const openvdb::ArithmeticError&) {
            success = false;
        }
        return success;
    };

    using PolarDecompositionM3D =
        bool(openvdb::math::Mat3<double>*,
            openvdb::math::Mat3<double>*,
            openvdb::math::Mat3<double>*);
    using PolarDecompositionM3F =
        bool(openvdb::math::Mat3<float>*,
            openvdb::math::Mat3<float>*,
            openvdb::math::Mat3<float>*);

    return FunctionBuilder("polardecompose")
        .addSignature<PolarDecompositionM3D>((PolarDecompositionM3D*)(polardecompose))
        .addSignature<PolarDecompositionM3F>((PolarDecompositionM3F*)(polardecompose))
        .setArgumentNames({"input", "unitary", "symmetric"})
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Decompose an invertible 3x3 matrix into its orthogonal (unitary) "
            "matrix and symmetric matrix components. If the determinant of the unitary matrix "
            "is 1 it is a rotation, otherwise if it is -1 there is some part reflection.")
        .get();
}

inline FunctionGroup::UniquePtr axpostscale(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> m1, v1;
        arrayUnpack(args[0], m1, B, /*load*/false);
        arrayUnpack(args[1], v1, B, /*load*/true);
        assert(m1.size() == 16);
        assert(v1.size() == 3);

        // modify first 3 elements in all mat rows
        for (size_t row = 0; row < 4; ++row) {
            for (size_t col = 0; col < 3; ++col) {
                const size_t idx = (row*4) + col;
                assert(idx <= 14);
                llvm::Value* m1v = B.CreateLoad(m1[idx]);
                m1v = binaryOperator(m1v, v1[col], ast::tokens::MULTIPLY, B);
                B.CreateStore(m1v, m1[idx]);
            }
        }

        // @warning  this is invalid for embedded IR
        return nullptr;
    };

    static auto postscale = [](auto mat, auto vec) {
        mat->postScale(*vec);
    };

    using PostscaleM4D = void(openvdb::math::Mat4<double>*, openvdb::math::Vec3<double>*);
    using PostscaleM4F = void(openvdb::math::Mat4<float>*, openvdb::math::Vec3<float>*);

    return FunctionBuilder("postscale")
        .addSignature<PostscaleM4D>(generate, (PostscaleM4D*)(postscale))
        .addSignature<PostscaleM4F>(generate, (PostscaleM4F*)(postscale))
        .setArgumentNames({"transform", "vec"})
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Post-scale a given matrix by the provided vector.")
        .get();
}

inline FunctionGroup::UniquePtr axpretransform(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> ptrs, m1, v1;
        arrayUnpack(args[0], ptrs, B, /*load*/false);
        arrayUnpack(args[1], m1, B, /*load*/true);
        arrayUnpack(args[2], v1, B, /*load*/true);

        const size_t vec = v1.size();
        const size_t dim = (m1.size() == 9 ? 3 : 4);

        assert(m1.size() == 9 || m1.size() == 16);
        assert(vec == 3 || vec == 4);
        assert(ptrs.size() == vec);

        // mat * vec
        llvm::Value* e3 = nullptr, *e4 = nullptr;
        for (size_t i = 0; i < vec; ++i) {
            llvm::Value* e1 = binaryOperator(v1[0], m1[0+(i*dim)], ast::tokens::MULTIPLY, B);
            llvm::Value* e2 = binaryOperator(v1[1], m1[1+(i*dim)], ast::tokens::MULTIPLY, B);
            if (dim >= 3) e3 = binaryOperator(v1[2], m1[2+(i*dim)], ast::tokens::MULTIPLY, B);
            if (dim == 4) {
                if (vec == 3) e4 = m1[3+(i*dim)];
                else if (vec == 4) e4 = binaryOperator(v1[3], m1[3+(i*dim)], ast::tokens::MULTIPLY, B);
            }
            e1 = binaryOperator(e1, e2, ast::tokens::PLUS, B);
            if (e3) e1 = binaryOperator(e1, e3, ast::tokens::PLUS, B);
            if (e4) e1 = binaryOperator(e1, e4, ast::tokens::PLUS, B);
            B.CreateStore(e1, ptrs[i]);
        }

        return nullptr;
    };

    static auto transform = [](auto out, auto mat, auto vec) {
        *out = mat->pretransform(*vec);
    };

    using PretransformM3DV3D = void(openvdb::math::Vec3<double>*, openvdb::math::Mat3<double>*, openvdb::math::Vec3<double>*);
    using PretransformM3FV3F = void(openvdb::math::Vec3<float>*, openvdb::math::Mat3<float>*, openvdb::math::Vec3<float>*);
    using PretransformM4DV3D = void(openvdb::math::Vec3<double>*, openvdb::math::Mat4<double>*, openvdb::math::Vec3<double>*);
    using PretransformM4FV3F = void(openvdb::math::Vec3<float>*, openvdb::math::Mat4<float>*, openvdb::math::Vec3<float>*);
    using PretransformM4DV4D = void(openvdb::math::Vec4<double>*, openvdb::math::Mat4<double>*, openvdb::math::Vec4<double>*);
    using PretransformM4FV4F = void(openvdb::math::Vec4<float>*, openvdb::math::Mat4<float>*, openvdb::math::Vec4<float>*);

    return FunctionBuilder("pretransform")
        .addSignature<PretransformM3DV3D, true>(generate, (PretransformM3DV3D*)(transform))
        .addSignature<PretransformM3FV3F, true>(generate, (PretransformM3FV3F*)(transform))
        .addSignature<PretransformM4DV3D, true>(generate, (PretransformM4DV3D*)(transform))
        .addSignature<PretransformM4FV3F, true>(generate, (PretransformM4FV3F*)(transform))
        .addSignature<PretransformM4DV4D, true>(generate, (PretransformM4DV4D*)(transform))
        .addSignature<PretransformM4FV4F, true>(generate, (PretransformM4FV4F*)(transform))
        .setArgumentNames({"vec", "mat"})
        .addParameterAttribute(0, llvm::Attribute::NoAlias) // alloced by the function, always no alias
        .addParameterAttribute(0, llvm::Attribute::WriteOnly)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addParameterAttribute(2, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Return the transformed vector by transpose of this matrix. "
            "This function is equivalent to pre-multiplying the matrix.")
        .get();
}

inline FunctionGroup::UniquePtr axprescale(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> m1, v1;
        arrayUnpack(args[0], m1, B, /*load*/false);
        arrayUnpack(args[1], v1, B, /*load*/true);
        assert(m1.size() == 16);
        assert(v1.size() == 3);

        // modify first 3 mat rows, all columns
        for (size_t row = 0; row < 3; ++row) {
            for (size_t col = 0; col < 4; ++col) {
                const size_t idx = (row*4) + col;
                assert(idx <= 11);
                llvm::Value* m1v = B.CreateLoad(m1[idx]);
                m1v = binaryOperator(m1v, v1[row], ast::tokens::MULTIPLY, B);
                B.CreateStore(m1v, m1[idx]);
            }
        }
        // @warning  this is invalid for embedded IR
        return nullptr;
    };

    static auto prescale = [](auto mat, auto vec) {
        mat->preScale(*vec);
    };

    using PrescaleM4D = void(openvdb::math::Mat4<double>*, openvdb::math::Vec3<double>*);
    using PrescaleM4F = void(openvdb::math::Mat4<float>*, openvdb::math::Vec3<float>*);

    return FunctionBuilder("prescale")
        .addSignature<PrescaleM4D>(generate, (PrescaleM4D*)(prescale))
        .addSignature<PrescaleM4F>(generate, (PrescaleM4F*)(prescale))
        .setArgumentNames({"transform", "vec"})
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Pre-scale a given matrix by the provided vector.")
        .get();
}

inline FunctionGroup::UniquePtr axtrace(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> m1;
        arrayUnpack(args[0], m1, B, /*load*/true);
        const size_t dim = (m1.size() == 9 ? 3 : 4);
        assert(m1.size() == 9 || m1.size() == 16);

        llvm::Value* result = binaryOperator(m1[0], m1[1+dim], ast::tokens::PLUS, B);
        result = binaryOperator(result, m1[2+(2*dim)], ast::tokens::PLUS, B);
        if (dim == 4) {
            result = binaryOperator(result, m1[3+(3*dim)], ast::tokens::PLUS, B);
        }

        return result;
    };

    static auto trace = [](const auto input) -> auto
    {
        using MatType = typename std::remove_pointer<decltype(input)>::type;
        using ElementT = typename openvdb::ValueTraits<MatType>::ElementType;
        ElementT value((*input)(static_cast<int>(0), static_cast<int>(0)));
        for (size_t i = 1; i < MatType::numRows(); ++i) {
           value += (*input)(static_cast<int>(i), static_cast<int>(i));
        }
        return value;
    };

    using TraceM3D = double(openvdb::math::Mat3<double>*);
    using TraceM3F = float(openvdb::math::Mat3<float>*);
    using TraceM4D = double(openvdb::math::Mat4<double>*);
    using TraceM4F = float(openvdb::math::Mat4<float>*);

    return FunctionBuilder("trace")
        .addSignature<TraceM3D>(generate, (TraceM3D*)(trace))
        .addSignature<TraceM3F>(generate, (TraceM3F*)(trace))
        .addSignature<TraceM4D>(generate, (TraceM4D*)(trace))
        .addSignature<TraceM4F>(generate, (TraceM4F*)(trace))
        .setArgumentNames({"mat"})
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Return the trace of a matrix, the sum of the diagonal elements.")
        .get();
}

inline FunctionGroup::UniquePtr axtransform(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> ptrs, m1, v1;
        arrayUnpack(args[0], ptrs, B, /*load*/false);
        arrayUnpack(args[1], v1, B, /*load*/true);
        arrayUnpack(args[2], m1, B, /*load*/true);

        const size_t vec = v1.size();
        const size_t dim = (m1.size() == 9 ? 3 : 4);

        assert(m1.size() == 9 || m1.size() == 16);
        assert(vec == 3 || vec == 4);
        assert(ptrs.size() == vec);

        // vec * mat
        llvm::Value* e3 = nullptr, *e4 = nullptr;
        for (size_t i = 0; i < vec; ++i) {
            llvm::Value* e1 = binaryOperator(v1[0], m1[i+(0*dim)], ast::tokens::MULTIPLY, B);
            llvm::Value* e2 = binaryOperator(v1[1], m1[i+(1*dim)], ast::tokens::MULTIPLY, B);
            if (dim >= 3) e3 = binaryOperator(v1[2], m1[i+(2*dim)], ast::tokens::MULTIPLY, B);
            if (dim == 4) {
                if (vec == 3) e4 = m1[i+(3*dim)];
                else if (vec == 4) e4 = binaryOperator(v1[3], m1[i+(3*dim)], ast::tokens::MULTIPLY, B);
            }
            e1 = binaryOperator(e1, e2, ast::tokens::PLUS, B);
            if (e3) e1 = binaryOperator(e1, e3, ast::tokens::PLUS, B);
            if (e4) e1 = binaryOperator(e1, e4, ast::tokens::PLUS, B);
            B.CreateStore(e1, ptrs[i]);
        }

        return nullptr;
    };

    static auto transform = [](auto out, auto vec, auto mat) {
        *out = mat->transform(*vec);
    };

    using TransformV3DM3D = void(openvdb::math::Vec3<double>*, openvdb::math::Vec3<double>*, openvdb::math::Mat3<double>*);
    using TransformV3FM3F = void(openvdb::math::Vec3<float>*, openvdb::math::Vec3<float>*, openvdb::math::Mat3<float>*);
    using TransformV3DM4D = void(openvdb::math::Vec3<double>*, openvdb::math::Vec3<double>*, openvdb::math::Mat4<double>*);
    using TransformV3FM4F = void(openvdb::math::Vec3<float>*, openvdb::math::Vec3<float>*, openvdb::math::Mat4<float>*);
    using TransformV4DM4D = void(openvdb::math::Vec4<double>*, openvdb::math::Vec4<double>*, openvdb::math::Mat4<double>*);
    using TransformV4FM4F = void(openvdb::math::Vec4<float>*, openvdb::math::Vec4<float>*, openvdb::math::Mat4<float>*);

    return FunctionBuilder("transform")
        .addSignature<TransformV3DM3D, true>(generate, (TransformV3DM3D*)(transform))
        .addSignature<TransformV3FM3F, true>(generate, (TransformV3FM3F*)(transform))
        .addSignature<TransformV3DM4D, true>(generate, (TransformV3DM4D*)(transform))
        .addSignature<TransformV3FM4F, true>(generate, (TransformV3FM4F*)(transform))
        .addSignature<TransformV4DM4D, true>(generate, (TransformV4DM4D*)(transform))
        .addSignature<TransformV4FM4F, true>(generate, (TransformV4FM4F*)(transform))
        .setArgumentNames({"vec", "mat"})
        .addParameterAttribute(0, llvm::Attribute::NoAlias)   // alloced by the function, always no alias
        .addParameterAttribute(0, llvm::Attribute::WriteOnly)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addParameterAttribute(2, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Return the transformed vector by the provided "
            "matrix. This function is equivalent to post-multiplying the matrix, i.e. vec * mult.")
        .get();
}

inline FunctionGroup::UniquePtr axtranspose(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        std::vector<llvm::Value*> ptrs, m1;
        arrayUnpack(args[0], ptrs, B, /*load*/false);
        arrayUnpack(args[1], m1, B, /*load*/true);
        assert(m1.size() == 9 || m1.size() == 16);
        assert(ptrs.size() == m1.size());
        const size_t dim = m1.size() == 9 ? 3 : 4;

        for (size_t i = 0; i < dim; ++i) {
            for (size_t j = 0; j < dim; ++j) {
                const size_t source = (i*dim) + j;
                const size_t target = (j*dim) + i;
                B.CreateStore(m1[source], ptrs[target]);
            }
        }

        return nullptr;
    };

    static auto transpose = [](auto out, auto in) {
        *out = in->transpose();
    };

    using TransposeM3D = void(openvdb::math::Mat3<double>*, openvdb::math::Mat3<double>*);
    using TransposeM3F = void(openvdb::math::Mat3<float>*, openvdb::math::Mat3<float>*);
    using TransposeM4D = void(openvdb::math::Mat4<double>*, openvdb::math::Mat4<double>*);
    using TransposeM4F = void(openvdb::math::Mat4<float>*, openvdb::math::Mat4<float>*);

    return FunctionBuilder("transpose")
        .addSignature<TransposeM3D, true>(generate, (TransposeM3D*)(transpose))
        .addSignature<TransposeM3F, true>(generate, (TransposeM3F*)(transpose))
        .addSignature<TransposeM4D, true>(generate, (TransposeM4D*)(transpose))
        .addSignature<TransposeM4F, true>(generate, (TransposeM4F*)(transpose))
        .setArgumentNames({"mat"})
        .addParameterAttribute(0, llvm::Attribute::NoAlias)   // alloced by the function, always no alias
        .addParameterAttribute(0, llvm::Attribute::WriteOnly)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the transpose of a matrix")
        .get();
}

inline FunctionGroup::UniquePtr axadjoint(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 2);
        std::vector<llvm::Value*> m1, m2;
        arrayUnpack(args[1], m1, B, /*load*/true);
        arrayUnpack(args[0], m2, B, /*load*/false); // args[0] is return type
        assert(m1.size() == 9 && m2.size() == 9);

        auto mul_sub = [&](const size_t a, const size_t b, const size_t c, const size_t d) {
            return binaryOperator(
                    binaryOperator(m1[a], m1[b], ast::tokens::MULTIPLY, B),
                    binaryOperator(m1[c], m1[d], ast::tokens::MULTIPLY, B),
                ast::tokens::MINUS, B);
        };

        B.CreateStore(mul_sub(4,8, 5,7), m2[0]);
        B.CreateStore(mul_sub(2,7, 1,8), m2[1]);
        B.CreateStore(mul_sub(1,5, 2,4), m2[2]);
        B.CreateStore(mul_sub(5,6, 3,8), m2[3]);
        B.CreateStore(mul_sub(0,8, 2,6), m2[4]);
        B.CreateStore(mul_sub(2,3, 0,5), m2[5]);
        B.CreateStore(mul_sub(3,7, 4,6), m2[6]);
        B.CreateStore(mul_sub(1,6, 0,7), m2[7]);
        B.CreateStore(mul_sub(0,4, 1,3), m2[8]);
        return nullptr;
    };

    static auto adjoint = [](auto out, const auto in) {
        *out = in->adjoint();
    };

    using AdjointM3D =
        void(openvdb::math::Mat3<double>*,
             openvdb::math::Mat3<double>*);
    using AjointM3F =
        void(openvdb::math::Mat3<float>*,
             openvdb::math::Mat3<float>*);

    return FunctionBuilder("adjoint")
        .addSignature<AdjointM3D, true>(generate, (AdjointM3D*)(adjoint))
        .addSignature<AjointM3F, true>(generate, (AjointM3F*)(adjoint))
        .setArgumentNames({"input"} )
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the adjoint of a 3x3 matrix. That is, "
            "the transpose of its cofactor matrix.")
        .get();
}

inline FunctionGroup::UniquePtr axcofactor(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 2);
        std::vector<llvm::Value*> m1, m2;
        arrayUnpack(args[1], m1, B, /*load*/true);
        arrayUnpack(args[0], m2, B, /*load*/false); // args[0] is return type
        assert(m1.size() == 9 && m2.size() == 9);

        auto mul_sub = [&](const size_t a, const size_t b, const size_t c, const size_t d) {
            return binaryOperator(
                    binaryOperator(m1[a], m1[b], ast::tokens::MULTIPLY, B),
                    binaryOperator(m1[c], m1[d], ast::tokens::MULTIPLY, B),
                ast::tokens::MINUS, B);
        };

        B.CreateStore(mul_sub(4,8, 5,7), m2[0]);
        B.CreateStore(mul_sub(5,6, 3,8), m2[1]);
        B.CreateStore(mul_sub(3,7, 4,6), m2[2]);
        B.CreateStore(mul_sub(2,7, 1,8), m2[3]);
        B.CreateStore(mul_sub(0,8, 2,6), m2[4]);
        B.CreateStore(mul_sub(1,6, 0,7), m2[5]);
        B.CreateStore(mul_sub(1,5, 2,4), m2[6]);
        B.CreateStore(mul_sub(2,3, 0,5), m2[7]);
        B.CreateStore(mul_sub(0,4, 1,3), m2[8]);
        return nullptr;
    };

    static auto cofactor = [](auto out, const auto in) {
        *out = in->cofactor();
    };

    using CofactorM3D =
        void(openvdb::math::Mat3<double>*,
             openvdb::math::Mat3<double>*);
    using CofactorM3F =
        void(openvdb::math::Mat3<float>*,
             openvdb::math::Mat3<float>*);

    return FunctionBuilder("cofactor")
        .addSignature<CofactorM3D, true>(generate, (CofactorM3D*)(cofactor))
        .addSignature<CofactorM3F, true>(generate, (CofactorM3F*)(cofactor))
        .setArgumentNames({"input"} )
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the cofactor matrix of a 3x3 matrix. That is, "
            "the matrix of its cofactors.")
        .get();
}

inline FunctionGroup::UniquePtr axinverse(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 2);

        llvm::Value* adj = axadjoint(op)->execute({args[1]}, B);
        std::vector<llvm::Value*> m1, madj;
        arrayUnpack(adj, madj, B, /*load*/true);
        arrayUnpack(args[0], m1, B, /*load*/false); // result
        assert(madj.size() == 9 && m1.size() == 9);

        // compute determinant of the input mat by reusing the adjoint's 0, 3 and 6 terms
        llvm::Value* m20 = B.CreateLoad(B.CreateConstGEP2_64(args[1], 0, 0));
        llvm::Value* m23 = B.CreateLoad(B.CreateConstGEP2_64(args[1], 0, 3));
        llvm::Value* m26 = B.CreateLoad(B.CreateConstGEP2_64(args[1], 0, 6));

        // compute det and store in c0
        llvm::Value* c0 = binaryOperator(madj[0], m20, ast::tokens::MULTIPLY, B);
        llvm::Value* c1 = binaryOperator(madj[1], m23, ast::tokens::MULTIPLY, B);
        llvm::Value* c2 = binaryOperator(madj[2], m26, ast::tokens::MULTIPLY, B);
        c0 = binaryOperator(c0, c1, ast::tokens::PLUS, B);
        c0 = binaryOperator(c0, c2, ast::tokens::PLUS, B);

        llvm::Value* zero = llvm::ConstantFP::get(c0->getType(), 0.0);
        llvm::Value* detisnotzero = binaryOperator(c0, zero, ast::tokens::NOTEQUALS, B);

        llvm::Function* base = B.GetInsertBlock()->getParent();
        llvm::BasicBlock* then = llvm::BasicBlock::Create(B.getContext(), "then", base);
        llvm::BasicBlock* post = llvm::BasicBlock::Create(B.getContext(), "post", base);
        B.CreateCondBr(detisnotzero, then, post);

        B.SetInsertPoint(then);
        llvm::Value* one = llvm::ConstantFP::get(c0->getType(), 1.0);
        c0 = B.CreateFDiv(one, c0);
        for (size_t i = 0; i < 9; ++i) {
            B.CreateStore(binaryOperator(madj[i], c0, ast::tokens::MULTIPLY, B), m1[i]);
        }

        B.CreateRetVoid();

        B.SetInsertPoint(post);

        madj.clear();
        arrayUnpack(args[1], madj, B, /*load*/true);
        for (size_t i = 0; i < 9; ++i) {
            B.CreateStore(madj[i], m1[i]);
        }

        return nullptr;
    };

    static auto inverse = [](auto out, const auto in) {
        try {
            *out = in->inverse();
        }
        catch (const openvdb::ArithmeticError&) {
            *out = *in;
        }
    };

    using InverseM3D =
        void(openvdb::math::Mat3<double>*,
             openvdb::math::Mat3<double>*);
    using InverseM3F =
        void(openvdb::math::Mat3<float>*,
             openvdb::math::Mat3<float>*);

    return FunctionBuilder("inverse")
        .addSignature<InverseM3D, true>(generate, (InverseM3D*)(inverse))
        .addSignature<InverseM3F, true>(generate, (InverseM3F*)(inverse))
        .setArgumentNames({"input"} )
        .addDependency("adjoint")
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Return the inverse of a 3x3 matrix."
            "If the matrix is singular, returns the input matrix.")
        .get();
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

// Noise

inline FunctionGroup::UniquePtr axsimplexnoise(const FunctionOptions& op)
{
    static auto simplexnoisex = [](double x) -> double {
        return SimplexNoise::noise(x, 0.0, 0.0);
    };
    static auto simplexnoisexy = [](double x, double y) -> double {
        return SimplexNoise::noise(x, y, 0.0);
    };
    static auto simplexnoisexyz = [](double x, double y, double z) -> double {
        return SimplexNoise::noise(x, y, z);
    };
    static auto simplexnoisev = [](const openvdb::math::Vec3<double>* v) -> double {
        return SimplexNoise::noise((*v)[0], (*v)[1], (*v)[2]);
    };

    return FunctionBuilder("simplexnoise")
        .addSignature<double(double)>(simplexnoisex)
            .setArgumentNames({"x"})
            .setConstantFold(false)
        .addSignature<double(double, double)>(simplexnoisexy)
            .setArgumentNames({"x", "y"})
            .setConstantFold(false)
        .addSignature<double(double,double,double)>(simplexnoisexyz)
            .setArgumentNames({"x", "y", "z"})
            .setConstantFold(false)
        .addSignature<double(const openvdb::math::Vec3<double>*)>(simplexnoisev)
            .setArgumentNames({"pos"})
            .addParameterAttribute(0, llvm::Attribute::ReadOnly)
            .setConstantFold(false)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Compute simplex noise at coordinates x, y and z. Coordinates which are "
            "not provided will be set to 0.")
        .get();
}

inline FunctionGroup::UniquePtr axcurlsimplexnoise(const FunctionOptions& op)
{
    using CurlSimplexNoiseV3D = void(double(*)[3], const double(*)[3]);
    using CurlSimplexNoiseD = void(double(*)[3], double, double, double);

    return FunctionBuilder("curlsimplexnoise")
        .addSignature<CurlSimplexNoiseV3D, true>(
            (CurlSimplexNoiseV3D*)(openvdb::ax::math::curlnoise<SimplexNoise>))
            .setArgumentNames({"pos"})
            .addParameterAttribute(0, llvm::Attribute::NoAlias)   // alloced by the function, always no alias
            .addParameterAttribute(0, llvm::Attribute::WriteOnly)
            .addParameterAttribute(1, llvm::Attribute::ReadOnly)
            .addFunctionAttribute(llvm::Attribute::NoUnwind)
            .addFunctionAttribute(llvm::Attribute::InlineHint)
            .setConstantFold(op.mConstantFoldCBindings)
        .addSignature<CurlSimplexNoiseD, true>(
            (CurlSimplexNoiseD*)(openvdb::ax::math::curlnoise<SimplexNoise>))
            .setArgumentNames({"pos"})
            .addParameterAttribute(0, llvm::Attribute::NoAlias)   // alloced by the function, always no alias
            .addParameterAttribute(0, llvm::Attribute::WriteOnly)
            .addFunctionAttribute(llvm::Attribute::NoUnwind)
            .addFunctionAttribute(llvm::Attribute::InlineHint)
            .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Generates divergence-free 3D noise, computed using a "
            "curl function on Simplex Noise.")
        .get();
}


///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

// Trig/Hyperbolic

/// @todo  Depending on the platform, some of these methods may be available though
///        LLVM as "intrinsics". To avoid conflicts, we currently only expose the C
///        bindings. We should perhaps override the C Bindings if the method exists
///        in LLVM, so long as it's clear that these methods may produce different
///        results from stdlib.
/// @note  See the following LLVM files for some details:
///  Analysis/TargetLibraryInfo.def
///  Analysis/ConstantFolding.cpp
///  Analysis/TargetLibraryInfo.cpp
///
DEFINE_AX_C_FP_BINDING(acos, "Computes the principal value of the arc cosine of the input.")
DEFINE_AX_C_FP_BINDING(acosh, "Computes the inverse hyperbolic cosine of the input.")
DEFINE_AX_C_FP_BINDING(asin, "Computes the principal value of the arc sine of the input.")
DEFINE_AX_C_FP_BINDING(asinh, "Computes the inverse hyperbolic sine of the input.")
DEFINE_AX_C_FP_BINDING(atan, "Computes the principal value of the arc tangent of the input.")
DEFINE_AX_C_FP_BINDING(atanh, "Computes the inverse hyperbolic tangent of the input.")
DEFINE_AX_C_FP_BINDING(cosh, "Computes the hyperbolic cosine of the input.")
DEFINE_AX_C_FP_BINDING(sinh, "Computes the hyperbolic sine of the input.")
DEFINE_AX_C_FP_BINDING(tanh, "Computes the hyperbolic tangent of the input.")

inline FunctionGroup::UniquePtr axdegrees(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
             llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 1);
        llvm::Value* arg = args.front();
        llvm::Value* pi180 = arg->getType()->isFloatTy() ?
            LLVMType<float>::get(B.getContext(), 180.f / openvdb::math::pi<float>()) :
            LLVMType<double>::get(B.getContext(), 180.0 / openvdb::math::pi<double>());
        return binaryOperator(arg, pi180, ast::tokens::MULTIPLY, B);
    };

    return FunctionBuilder("degrees")
        .addSignature<double(double)>(generate)
        .addSignature<float(float)>(generate)
        .setArgumentNames({"radians"})
        .setConstantFold(true)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Converts the number of radians to degrees.")
        .get();
}

inline FunctionGroup::UniquePtr axradians(const FunctionOptions& op)
{
    static auto generate =
        [](const std::vector<llvm::Value*>& args,
             llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 1);
        llvm::Value* arg = args.front();
        llvm::Value* pi180 = arg->getType()->isFloatTy() ?
            LLVMType<float>::get(B.getContext(), openvdb::math::pi<float>() / 180.f) :
            LLVMType<double>::get(B.getContext(), openvdb::math::pi<double>() / 180.0);
        return binaryOperator(arg, pi180, ast::tokens::MULTIPLY, B);
    };

    return FunctionBuilder("radians")
        .addSignature<double(double)>(generate)
        .addSignature<float(float)>(generate)
        .setArgumentNames({"degrees"})
        .setConstantFold(true)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::InlineHint)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Converts the number of degrees to radians.")
        .get();
}

inline FunctionGroup::UniquePtr axtan(const FunctionOptions& op)
{
    // @todo  consider using this IR implementation over std::tan, however
    //        we then lose constant folding (as results don't match). Ideally
    //        this ir implementation should exist at compile time as a valid
    //        function for constant folding
    //
    // static auto generate =
    //     [](const std::vector<llvm::Value*>& args,
    //        const std::unordered_map<std::string, llvm::Value*>&,
    //        llvm::IRBuilder<>& B) -> llvm::Value*
    // {
    //     llvm::Module* M = B.GetInsertBlock()->getParent()->getParent();
    //     llvm::Type* type = args[0]->getType();
    //     llvm::Function* sinFunction =
    //         llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::sin, type);
    //     llvm::Function* cosFunction =
    //         llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::cos, type);

    //     llvm::Value* sin = B.CreateCall(sinFunction, args[0]);
    //     llvm::Value* cos = B.CreateCall(cosFunction, args[0]);
    //     return binaryOperator(sin, cos, ast::tokens::DIVIDE, B);
    // };

    return FunctionBuilder("tan")
        .addSignature<double(double)>((double(*)(double))(std::tan))
        .addSignature<float(float)>((float(*)(float))(std::tan))
        .setArgumentNames({"n"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Computes the tangent of arg (measured in radians).")
        .get();
}

inline FunctionGroup::UniquePtr axatan2(const FunctionOptions& op)
{
    return FunctionBuilder("atan2")
        .addSignature<double(double,double)>((double(*)(double,double))(std::atan2))
        .addSignature<float(float,float)>((float(*)(float,float))(std::atan2))
        .setArgumentNames({"y", "x"})
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Computes the arc tangent of y/x using the signs of arguments "
            "to determine the correct quadrant.")
        .get();
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

// String

inline FunctionGroup::UniquePtr axatoi(const FunctionOptions& op)
{
    // WARNING: decltype removes the throw identifer from atoi. We should
    // use this are automatically update the function attributes as appropriate
    return FunctionBuilder("atoi")
        .addSignature<decltype(std::atoi)>(std::atoi)
        .setArgumentNames({"str"})
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Parses the string input interpreting its "
            "content as an integral number, which is returned as a value of type int.")
        .get();
}

inline FunctionGroup::UniquePtr axatof(const FunctionOptions& op)
{
    // WARNING: decltype removes the throw identifer from atof. We should
    // use this are automatically update the function attributes as appropriate
    return FunctionBuilder("atof")
        .addSignature<decltype(std::atof)>(std::atof)
        .setArgumentNames({"str"})
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Parses the string input, interpreting its "
            "content as a floating point number and returns its value as a double.")
        .get();
}

inline FunctionGroup::UniquePtr axhash(const FunctionOptions& op)
{
    static auto hash = [](const codegen::String* str) -> int64_t {
        return static_cast<int64_t>(std::hash<std::string>{}(str->str()));
    };

    return FunctionBuilder("hash")
        .addSignature<int64_t(const codegen::String*)>((int64_t(*)(const codegen::String*))(hash))
        .setArgumentNames({"str"})
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Return a hash of the provided string.")
        .get();
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

// Utility

inline FunctionGroup::UniquePtr axprint(const FunctionOptions& op)
{
    static auto print = [](auto v) { std::cout << v << std::endl; };
    static auto printv = [](auto* v) { std::cout << *v << std::endl; };
    static auto printstr = [](const codegen::String* axstr) {
        std::cout << axstr->c_str() << std::endl;
    };

    return FunctionBuilder("print")
        .addSignature<void(double)>((void(*)(double))(print))
        .addSignature<void(float)>((void(*)(float))(print))
        .addSignature<void(int64_t)>((void(*)(int64_t))(print))
        .addSignature<void(int32_t)>((void(*)(int32_t))(print))
            .setArgumentNames({"n"})
            .addFunctionAttribute(llvm::Attribute::ReadOnly)
            .addFunctionAttribute(llvm::Attribute::NoRecurse)
            .addFunctionAttribute(llvm::Attribute::AlwaysInline)
            .setConstantFold(false /*never cf*/)
        .addSignature<void(const codegen::String*)>((void(*)(const codegen::String*))(printstr))
        .addSignature<void(openvdb::math::Vec2<int32_t>*)>((void(*)(openvdb::math::Vec2<int32_t>*))(printv))
        .addSignature<void(openvdb::math::Vec2<float>*)>((void(*)(openvdb::math::Vec2<float>*))(printv))
        .addSignature<void(openvdb::math::Vec2<double>*)>((void(*)(openvdb::math::Vec2<double>*))(printv))
        .addSignature<void(openvdb::math::Vec3<int32_t>*)>((void(*)(openvdb::math::Vec3<int32_t>*))(printv))
        .addSignature<void(openvdb::math::Vec3<float>*)>((void(*)(openvdb::math::Vec3<float>*))(printv))
        .addSignature<void(openvdb::math::Vec3<double>*)>((void(*)(openvdb::math::Vec3<double>*))(printv))
        .addSignature<void(openvdb::math::Vec4<int32_t>*)>((void(*)(openvdb::math::Vec4<int32_t>*))(printv))
        .addSignature<void(openvdb::math::Vec4<float>*)>((void(*)(openvdb::math::Vec4<float>*))(printv))
        .addSignature<void(openvdb::math::Vec4<double>*)>((void(*)(openvdb::math::Vec4<double>*))(printv))
        .addSignature<void(openvdb::math::Mat3<float>*)>((void(*)(openvdb::math::Mat3<float>*))(printv))
        .addSignature<void(openvdb::math::Mat3<double>*)>((void(*)(openvdb::math::Mat3<double>*))(printv))
        .addSignature<void(openvdb::math::Mat4<float>*)>((void(*)(openvdb::math::Mat4<float>*))(printv))
        .addSignature<void(openvdb::math::Mat4<double>*)>((void(*)(openvdb::math::Mat4<double>*))(printv))
            .addParameterAttribute(0, llvm::Attribute::ReadOnly)
            .setArgumentNames({"n"})
            .addFunctionAttribute(llvm::Attribute::ReadOnly)
            .addFunctionAttribute(llvm::Attribute::NoRecurse)
            .addFunctionAttribute(llvm::Attribute::AlwaysInline)
            .setConstantFold(false /*never cf*/)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Prints the input to the standard output stream. "
            "Warning: This will be run for every element.")
        .get();
}

inline FunctionGroup::UniquePtr axargsort(const FunctionOptions& op)
{
    static auto argsort = [](auto out, const auto in){
        using VecType = typename std::remove_pointer<decltype(in)>::type;
        // initialize original index locations
        std::iota(out->asPointer(), out->asPointer() + VecType::size, 0);
        // sort indexes based on comparing values in v
        // using std::stable_sort instead of std::sort
        // to avoid unnecessary index re-orderings
        // when v contains elements of equal values
        std::stable_sort(out->asPointer(), out->asPointer() + VecType::size,
          [&in](int32_t i1, int32_t i2) {return (*in)[i1] < (*in)[i2];});
    };

    using Argsort3D = void(openvdb::math::Vec3<int>*, openvdb::math::Vec3<double>*);
    using Argsort3F = void(openvdb::math::Vec3<int>*, openvdb::math::Vec3<float>*);
    using Argsort3I = void(openvdb::math::Vec3<int>*, openvdb::math::Vec3<int32_t>*);
    using Argsort4D = void(openvdb::math::Vec4<int>*, openvdb::math::Vec4<double>*);
    using Argsort4F = void(openvdb::math::Vec4<int>*, openvdb::math::Vec4<float>*);
    using Argsort4I = void(openvdb::math::Vec4<int>*, openvdb::math::Vec4<int32_t>*);

    return FunctionBuilder("argsort")
        .addSignature<Argsort3D, true>((Argsort3D*)(argsort))
        .addSignature<Argsort3F, true>((Argsort3F*)(argsort))
        .addSignature<Argsort3I, true>((Argsort3I*)(argsort))
        .addSignature<Argsort4D, true>((Argsort4D*)(argsort))
        .addSignature<Argsort4F, true>((Argsort4F*)(argsort))
        .addSignature<Argsort4I, true>((Argsort4I*)(argsort))
        .setArgumentNames({"v"})
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns a vector of the indexes that would sort the input vector.")
        .get();
}

inline FunctionGroup::UniquePtr axsort(const FunctionOptions& op)
{
    static auto sort3 = [](auto out, const auto in) {
        *out = in->sorted();
    };

    static auto sort = [](auto out, const auto in) {
        using VecType = typename std::remove_pointer<decltype(out)>::type;
        *out = *in;
        std::sort(out->asPointer(), out->asPointer() + VecType::size);
    };

    using Sort3D = void(openvdb::math::Vec3<double>*,openvdb::math::Vec3<double>*);
    using Sort3F = void(openvdb::math::Vec3<float>*,openvdb::math::Vec3<float>*);
    using Sort3I = void(openvdb::math::Vec3<int32_t>*, openvdb::math::Vec3<int32_t>*);
    using Sort4D = void(openvdb::math::Vec4<double>*,openvdb::math::Vec4<double>*);
    using Sort4F = void(openvdb::math::Vec4<float>*,openvdb::math::Vec4<float>*);
    using Sort4I = void(openvdb::math::Vec4<int32_t>*, openvdb::math::Vec4<int32_t>*);

    return FunctionBuilder("sort")
        .addSignature<Sort3D, true>((Sort3D*)(sort3))
        .addSignature<Sort3F, true>((Sort3F*)(sort3))
        .addSignature<Sort3I, true>((Sort3I*)(sort3))
        .addSignature<Sort4D, true>((Sort4D*)(sort))
        .addSignature<Sort4F, true>((Sort4F*)(sort))
        .addSignature<Sort4I, true>((Sort4I*)(sort))
        .setArgumentNames({"v"})
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Returns the sorted result of the given vector.")
        .get();
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

// Colour

inline FunctionGroup::UniquePtr axhsvtorgb(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 2);
        llvm::Function* base = B.GetInsertBlock()->getParent();

        std::vector<llvm::Value*> hsv, rgb;
        arrayUnpack(args[0], rgb, B, /*load*/false); //output
        arrayUnpack(args[1], hsv, B, /*load*/true);  //input

        llvm::Type* precision = hsv[0]->getType();
        llvm::Value* zero = llvm::ConstantFP::get(precision, 0.0);
        llvm::Value* one = llvm::ConstantFP::get(precision, 1.0);

        llvm::BasicBlock* then = llvm::BasicBlock::Create(B.getContext(), "then", base);
        llvm::BasicBlock* el = llvm::BasicBlock::Create(B.getContext(), "else", base);
        llvm::BasicBlock* post = llvm::BasicBlock::Create(B.getContext(), "post", base);

        llvm::Value* hueisone = binaryOperator(hsv[0], one, ast::tokens::EQUALSEQUALS, B);
        B.CreateCondBr(hueisone, then, el);

        llvm::Value* h = insertStaticAlloca(B, precision);

        B.SetInsertPoint(then);
        {
            llvm::Value* r = binaryOperator(hsv[0], zero, ast::tokens::MULTIPLY, B); // zero hue
            B.CreateStore(r, h);
            B.CreateBr(post);
        }
        B.SetInsertPoint(el);
        {
            llvm::Value* six = llvm::ConstantFP::get(hsv[0]->getType(), 6.0);
            llvm::Value* r = binaryOperator(hsv[0], six, ast::tokens::MULTIPLY, B);
            B.CreateStore(r, h);
            B.CreateBr(post);
        }

        B.SetInsertPoint(post);

        h = B.CreateLoad(h);
        llvm::Value* sat = hsv[1];
        llvm::Value* val = hsv[2];

        llvm::Value* i = llvm_floor(op)->execute({h}, B);
        llvm::Value* f =
            binaryOperator(h, i, ast::tokens::MINUS, B);
        llvm::Value* p =
            binaryOperator(val,
                binaryOperator(one, sat, ast::tokens::MINUS, B),
                ast::tokens::MULTIPLY, B);
        llvm::Value* q =
            binaryOperator(val,
                binaryOperator(one,
                    binaryOperator(sat, f,
                        ast::tokens::MULTIPLY, B),
                    ast::tokens::MINUS, B),
                ast::tokens::MULTIPLY, B);
        llvm::Value* t =
            binaryOperator(val,
                binaryOperator(one,
                    binaryOperator(sat,
                        binaryOperator(one, f,
                            ast::tokens::MINUS, B),
                        ast::tokens::MULTIPLY, B),
                    ast::tokens::MINUS, B),
                ast::tokens::MULTIPLY, B);

        // start main switch

        post = llvm::BasicBlock::Create(B.getContext(), "post", base);

        i = arithmeticConversion(i, LLVMType<int64_t>::get(B.getContext()), B);

        for (int64_t j = 0; j <= 5; ++j)
        {
            llvm::BasicBlock* then = llvm::BasicBlock::Create(B.getContext(), "then", base);
            llvm::BasicBlock* el = llvm::BasicBlock::Create(B.getContext(), "else", base);

            llvm::Value* constant = LLVMType<int64_t>::get(B.getContext(), j);
            llvm::Value* switchv = binaryOperator(i, constant, ast::tokens::EQUALSEQUALS, B);
            B.CreateCondBr(switchv, then, el);

            B.SetInsertPoint(then);
            {
                // The final logic for storing the RGB values
                if (j == 0) {
                    B.CreateStore(val, rgb[0]);
                    B.CreateStore(t, rgb[1]);
                    B.CreateStore(p, rgb[2]);
                }
                else if (j == 1) {
                    B.CreateStore(q, rgb[0]);
                    B.CreateStore(val, rgb[1]);
                    B.CreateStore(p, rgb[2]);
                }
                else if (j == 2) {
                    B.CreateStore(p, rgb[0]);
                    B.CreateStore(val, rgb[1]);
                    B.CreateStore(t, rgb[2]);
                }
                else if (j == 3) {
                    B.CreateStore(p, rgb[0]);
                    B.CreateStore(q, rgb[1]);
                    B.CreateStore(val, rgb[2]);
                }
                else if (j == 4) {
                    B.CreateStore(t, rgb[0]);
                    B.CreateStore(p, rgb[1]);
                    B.CreateStore(val, rgb[2]);
                }
                else if (j == 5) {
                    B.CreateStore(val, rgb[0]);
                    B.CreateStore(p, rgb[1]);
                    B.CreateStore(q, rgb[2]);
                }

                B.CreateBr(post);
            }
            // set for next iteration
            B.SetInsertPoint(el);
        }

        // Final case (hue > 1 || hue < 0), zero intialize
        B.CreateStore(zero, rgb[0]);
        B.CreateStore(zero, rgb[1]);
        B.CreateStore(zero, rgb[2]);
        B.CreateBr(post);

        B.SetInsertPoint(post);
        return B.CreateRetVoid();
    };

    using HSVtoRGB3D = void(openvdb::math::Vec3<double>*,openvdb::math::Vec3<double>*);
    using HSVtoRGB3F = void(openvdb::math::Vec3<float>*,openvdb::math::Vec3<float>*);

    return FunctionBuilder("hsvtorgb")
        .addSignature<HSVtoRGB3D, true>(generate)
        .addSignature<HSVtoRGB3F, true>(generate)
        .setArgumentNames({"input"} )
        .addDependency("floor")
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Convert HSV color space into RGB color space.")
        .get();
}

inline FunctionGroup::UniquePtr axrgbtohsv(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        assert(args.size() == 2);
        llvm::Function* base = B.GetInsertBlock()->getParent();
        llvm::LLVMContext& C = B.getContext();

        std::vector<llvm::Value*> hsv, rgb;
        arrayUnpack(args[0], hsv, B, /*load*/false); //output
        arrayUnpack(args[1], rgb, B, /*load*/true);  //input
        llvm::Type* precision = rgb[0]->getType();
        llvm::Value* zero = llvm::ConstantFP::get(precision, 0.0);


        llvm::Value* max = axmax(op)->execute({rgb[0], rgb[1]}, B);
        max = axmax(op)->execute({max, rgb[2]}, B);
        llvm::Value* min = axmin(op)->execute({rgb[0], rgb[1]}, B);
        min = axmin(op)->execute({min, rgb[2]}, B);

        llvm::Value* range = binaryOperator(max, min, ast::tokens::MINUS, B);

        B.CreateStore(zero, hsv[0]);
        B.CreateStore(zero, hsv[1]);
        B.CreateStore(max, hsv[2]);

        llvm::BasicBlock* then = llvm::BasicBlock::Create(C, "then", base);
        llvm::BasicBlock* post = llvm::BasicBlock::Create(C, "post", base);

        llvm::Value* maxneqzero = binaryOperator(max, zero, ast::tokens::NOTEQUALS, B);
        B.CreateCondBr(maxneqzero, then, post);

        B.SetInsertPoint(then);
        {
            llvm::Value* sat = binaryOperator(range, max, ast::tokens::DIVIDE, B);
            B.CreateStore(sat, hsv[1]);
            B.CreateBr(post);
        }

        B.SetInsertPoint(post);

        llvm::Value* sat = B.CreateLoad(hsv[1]);

        then = llvm::BasicBlock::Create(C, "then", base);
        post = llvm::BasicBlock::Create(C, "post", base);

        llvm::Value* satneqzero = binaryOperator(sat, zero, ast::tokens::NOTEQUALS, B);

        B.CreateCondBr(satneqzero, then, post);

        B.SetInsertPoint(then);
        {
            then = llvm::BasicBlock::Create(C, "then", base);
            llvm::BasicBlock* elif1 = llvm::BasicBlock::Create(C, "elif1", base);
            llvm::BasicBlock* el = llvm::BasicBlock::Create(C, "el", base);
            llvm::BasicBlock* end = llvm::BasicBlock::Create(C, "end", base);

            llvm::Value* reqmax = binaryOperator(rgb[0], max, ast::tokens::EQUALSEQUALS, B);
            B.CreateCondBr(reqmax, then, elif1);

            B.SetInsertPoint(then);
            {
                llvm::Value* h =
                    binaryOperator(
                        binaryOperator(rgb[1], rgb[2], ast::tokens::MINUS, B),
                        range,
                        ast::tokens::DIVIDE, B);

                B.CreateStore(h, hsv[0]);
                B.CreateBr(end);
            }

            B.SetInsertPoint(elif1);
            {
                then = llvm::BasicBlock::Create(C, "then", base);

                llvm::Value* geqmax = binaryOperator(rgb[1], max, ast::tokens::EQUALSEQUALS, B);
                B.CreateCondBr(geqmax, then, el);

                B.SetInsertPoint(then);
                {
                    llvm::Value* two = llvm::ConstantFP::get(precision, 2.0);

                    llvm::Value* h =
                        binaryOperator(two,
                            binaryOperator(
                                binaryOperator(rgb[2], rgb[0], ast::tokens::MINUS, B),
                                range,
                                ast::tokens::DIVIDE, B),
                            ast::tokens::PLUS, B);

                    B.CreateStore(h, hsv[0]);
                    B.CreateBr(end);
                }
            }

            B.SetInsertPoint(el);
            {
                llvm::Value* four = llvm::ConstantFP::get(precision, 4.0);

                llvm::Value* h =
                    binaryOperator(four,
                        binaryOperator(
                            binaryOperator(rgb[0], rgb[1], ast::tokens::MINUS, B),
                            range,
                            ast::tokens::DIVIDE, B),
                        ast::tokens::PLUS, B);

                B.CreateStore(h, hsv[0]);
                B.CreateBr(end);
            }

            B.SetInsertPoint(end);

            llvm::Value* six = llvm::ConstantFP::get(precision, 6.0);

            llvm::Value* h = B.CreateLoad(hsv[0]);
            h = binaryOperator(h, six, ast::tokens::DIVIDE, B);
            B.CreateStore(h, hsv[0]);

            then = llvm::BasicBlock::Create(C, "then", base);

            llvm::Value* hlesszero = binaryOperator(h, zero, ast::tokens::LESSTHAN, B);
            B.CreateCondBr(hlesszero, then, post);

            B.SetInsertPoint(then);
            {
                llvm::Value* one = llvm::ConstantFP::get(precision, 1.0);
                h = binaryOperator(h, one, ast::tokens::PLUS, B);
                B.CreateStore(h, hsv[0]);
                B.CreateBr(post);
            }
        }

        B.SetInsertPoint(post);
        return B.CreateRetVoid();
    };

    using HSVtoRGB3D = void(openvdb::math::Vec3<double>*,openvdb::math::Vec3<double>*);
    using HSVtoRGB3F = void(openvdb::math::Vec3<float>*,openvdb::math::Vec3<float>*);

    return FunctionBuilder("rgbtohsv")
        .addSignature<HSVtoRGB3D, true>(generate)
        .addSignature<HSVtoRGB3F, true>(generate)
        .setArgumentNames({"input"} )
        .addDependency("max")
        .addDependency("min")
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoUnwind)
        .addFunctionAttribute(llvm::Attribute::AlwaysInline)
        .setConstantFold(op.mConstantFoldCBindings)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Convert RGB color space into HSV color space.")
        .get();
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

// Custom

inline FunctionGroup::UniquePtr ax_external(const FunctionOptions& op)
{
    static auto find = [](auto out, const void* const data, const codegen::String* const name)
    {
        using ValueType = typename std::remove_pointer<decltype(out)>::type;
        const ax::CustomData* const customData =
            static_cast<const ax::CustomData*>(data);
        const TypedMetadata<ValueType>* const metaData =
            customData->getData<TypedMetadata<ValueType>>(name->str());
        *out = (metaData ? metaData->value() : zeroVal<ValueType>());
    };


    using FindF = void(float*, const void* const, const codegen::String* const);
    using FindV3F = void(openvdb::math::Vec3<float>*, const void* const, const codegen::String* const);

    return FunctionBuilder("_external")
        .addSignature<FindF>((FindF*)(find))
        .addSignature<FindV3F>((FindV3F*)(find))
        .setArgumentNames({"str", "custom_data", "result"})
        .addParameterAttribute(0, llvm::Attribute::NoAlias)
        .addParameterAttribute(0, llvm::Attribute::WriteOnly)
        .addParameterAttribute(1, llvm::Attribute::ReadOnly)
        .addParameterAttribute(2, llvm::Attribute::ReadOnly)
        .setConstantFold(false)
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Internal function for looking up a custom float value.")
        .get();
}

inline FunctionGroup::UniquePtr axexternal(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        // Pull out the custom data from the parent function
        llvm::Function* compute = B.GetInsertBlock()->getParent();
        assert(compute);
        assert(std::string(compute->getName()).rfind("ax.compute", 0) == 0);
        llvm::Value* arg = extractArgument(compute, 0);
        assert(arg);
        assert(arg->getName() == "custom_data");

        std::vector<llvm::Value*> inputs;
        inputs.reserve(2 + args.size());
        inputs.emplace_back(insertStaticAlloca(B, LLVMType<float>::get(B.getContext())));
        inputs.emplace_back(arg);
        inputs.insert(inputs.end(), args.begin(), args.end());
        ax_external(op)->execute(inputs, B);
        return B.CreateLoad(inputs.front());
    };

    return FunctionBuilder("external")
        .addSignature<float(const codegen::String*)>(generate)
        .setArgumentNames({"str"})
        .addDependency("_external")
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::ReadOnly)
        .addFunctionAttribute(llvm::Attribute::NoRecurse)
        .setConstantFold(false)
        .setEmbedIR(true) // always embed as we pass through function param "custom_data"
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Find a custom user parameter with a given name of type 'float' "
            "in the Custom data provided to the AX compiler. If the data can not be found, "
            "or is not of the expected type 0.0f is returned.")
        .get();
}

inline FunctionGroup::UniquePtr axexternalv(const FunctionOptions& op)
{
    auto generate =
        [op](const std::vector<llvm::Value*>& args,
           llvm::IRBuilder<>& B) -> llvm::Value*
    {
        // Pull out the custom data from the parent function
        llvm::Function* compute = B.GetInsertBlock()->getParent();
        assert(compute);
        assert(std::string(compute->getName()).rfind("ax.compute", 0) == 0);
        llvm::Value* arg = extractArgument(compute, 0);
        assert(arg);
        assert(arg->getName() == "custom_data");

        std::vector<llvm::Value*> inputs;
        inputs.reserve(2 + args.size());
        inputs.emplace_back(insertStaticAlloca(B, LLVMType<float[3]>::get(B.getContext())));
        inputs.emplace_back(arg);
        inputs.insert(inputs.end(), args.begin(), args.end());
        ax_external(op)->execute(inputs, B);
        return inputs.front();
    };

    return FunctionBuilder("externalv")
        .addSignature<openvdb::math::Vec3<float>*(const codegen::String*)>(generate)
        .setArgumentNames({"str"})
        .addDependency("_external")
        .addParameterAttribute(0, llvm::Attribute::ReadOnly)
        .setConstantFold(false)
        .setEmbedIR(true) // always embed as we pass through function param "custom_data"
        .setPreferredImpl(op.mPrioritiseIR ? FunctionBuilder::IR : FunctionBuilder::C)
        .setDocumentation("Find a custom user parameter with a given name of type 'vector float' "
            "in the Custom data provided to the AX compiler. If the data can not be found, or is "
            "not of the expected type { 0.0f, 0.0f, 0.0f } is returned.")
        .get();
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////


void insertStringFunctions(FunctionRegistry& reg, const FunctionOptions* options = nullptr);

void insertStandardFunctions(FunctionRegistry& registry,
    const FunctionOptions* options)
{
    const bool create = options && !options->mLazyFunctions;
    auto add = [&](const std::string& name,
        const FunctionRegistry::ConstructorT creator,
        const bool internal = false)
    {
        if (create) registry.insertAndCreate(name, creator, *options, internal);
        else        registry.insert(name, creator, internal);
    };

    // memory

    add("axmalloc", axmalloc, true);
    add("axfree", axfree, true);
    add("axrealloc", axrealloc, true);

    // llvm instrinsics

    add("ceil", llvm_ceil);
    add("cos", llvm_cos);
    add("exp2", llvm_exp2);
    add("exp", llvm_exp);
    add("fabs", llvm_fabs);
    add("floor", llvm_floor);
    add("log10", llvm_log10);
    add("log2", llvm_log2);
    add("log", llvm_log);
    add("pow", llvm_pow);
    add("round", llvm_round);
    add("sin", llvm_sin);
    add("sqrt", llvm_sqrt);

    // math

    add("abs", axabs);
    add("cbrt", axcbrt);
    add("clamp", axclamp);
    add("cross", axcross);
    add("dot", axdot);
    add("euclideanmod", axeuclideanmod);
    add("fit", axfit);
    add("floormod", axfloormod);
    add("isfinite", axisfinite);
    add("isinf", axisinf);
    add("isnan", axisnan);
    add("length", axlength);
    add("lengthsq", axlengthsq);
    add("lerp", axlerp);
    add("max", axmax);
    add("min", axmin);
    add("normalize", axnormalize);
    add("rand", axrand);
    add("rand32", axrand32);
    add("sign", axsign);
    add("signbit", axsignbit);
    add("truncatemod", axtruncatemod);

    // matrix math

    add("adjoint", axadjoint);
    add("cofactor", axcofactor);
    add("determinant", axdeterminant);
    add("diag", axdiag);
    add("identity3", axidentity3);
    add("identity4", axidentity4);
    add("inverse", axinverse);
    add("mmmult", axmmmult, true);
    add("polardecompose", axpolardecompose);
    add("postscale", axpostscale);
    add("prescale", axprescale);
    add("pretransform", axpretransform);
    add("trace", axtrace);
    add("transform", axtransform);
    add("transpose", axtranspose);

    // noise

    add("simplexnoise", axsimplexnoise);
    add("curlsimplexnoise", axcurlsimplexnoise);

    // trig

    add("degrees", axdegrees);
    add("radians", axradians);
    add("acos", axacos);
    add("acosh", axacosh);
    add("asin", axasin);
    add("asinh", axasinh);
    add("atan", axatan);
    add("atan2", axatan2);
    add("atanh", axatanh);
    add("cosh", axcosh);
    add("sinh", axsinh);
    add("tan", axtan);
    add("tanh", axtanh);

    // string

    add("atoi", axatoi);
    add("atof", axatof);
    add("hash", axhash);

    // util
    add("argsort", axargsort);
    add("sort", axsort);
    add("print", axprint);

    // colour

    add("hsvtorgb", axhsvtorgb);
    add("rgbtohsv", axrgbtohsv);

    // custom

    add("_external", ax_external, true);
    add("external", axexternal);
    add("externalv", axexternalv);

    insertStringFunctions(registry, options);
}


} // namespace codegen
} // namespace ax
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb

