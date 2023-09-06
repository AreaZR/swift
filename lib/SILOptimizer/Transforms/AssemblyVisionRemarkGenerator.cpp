//===--- AssemblyVisionRemarkGenerator.cpp --------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// In this pass, we define the assembly-vision-remark-generator, a simple
/// SILVisitor that attempts to infer remarks for the user using heuristics.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-assembly-vision-remark-gen"

#include "swift/AST/SemanticAttrs.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OptimizationRemark.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace swift::PatternMatch;

static llvm::cl::opt<bool> ForceVisitImplicitAutogeneratedFunctions(
    "assemblyvisionremarkgen-visit-implicit-autogen-funcs", llvm::cl::Hidden,
    llvm::cl::desc(
        "Emit opt remarks even on implicit and autogenerated functions"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> DecllessDebugValueUseSILDebugInfo(
    "assemblyvisionremarkgen-declless-debugvalue-use-sildebugvar-info",
    llvm::cl::Hidden,
    llvm::cl::desc(
        "If a debug_value does not have a decl, infer a value with a name from "
        "that info that has a loc set to the loc of the debug_value "
        "instruction itself. This is for testing purposes so it is easier to "
        "write SIL test cases for this pass"),
    llvm::cl::init(false));

//===----------------------------------------------------------------------===//
//                           Value To Decl Inferrer
//===----------------------------------------------------------------------===//

namespace {

struct ValueToDeclInferrer {
  using Argument = OptRemark::Argument;
  using ArgumentKeyKind = OptRemark::ArgumentKeyKind;

  SmallVector<std::pair<SILType, Projection>, 32> accessPath;
  SmallVector<Operand *, 32> rcIdenticalSecondaryUseSearch;
  RCIdentityFunctionInfo &rcfi;

  ValueToDeclInferrer(RCIdentityFunctionInfo &rcfi) : rcfi(rcfi) {}

  /// Given a value, attempt to infer a conservative list of decls that the
  /// passed in value could be referring to. This is done just using heuristics
  bool infer(ArgumentKeyKind keyKind, SILValue value,
             SmallVectorImpl<Argument> &resultingInferredDecls,
             bool allowSingleRefEltAddrPeek = false);

  /// Print out a note to \p stream that beings at decl and then if
  /// useProjectionPath is set to true iterates the accessPath we computed for
  /// decl producing a segmented access path, e.x.: "of 'x.lhs.ivar'".
  ///
  /// The reason why one may not want to emit a projection path note here is if
  /// one found an debug_value on a value that is rc-identical to the actual
  /// value associated with the current projection path. Consider the following
  /// SIL:
  ///
  ///    struct KlassPair {
  ///      var lhs: Klass
  ///      var rhs: Klass
  ///    }
  ///
  ///    struct StateWithOwningPointer {
  ///      var state: TrivialState
  ///      var owningPtr: Klass
  ///    }
  ///
  ///    sil @theFunction : $@convention(thin) () -> () {
  ///    bb0:
  ///      %0 = apply %getKlassPair() : $@convention(thin) () -> @owned
  ///      KlassPair
  ///      // This debug_value's name can be combined...
  ///      debug_value %0 : $KlassPair, name "myPair"
  ///      // ... with the access path from the struct_extract here...
  ///      %1 = struct_extract %0 : $KlassPair, #KlassPair.lhs
  ///      // ... to emit a nice diagnostic that 'myPair.lhs' is being retained.
  ///      strong_retain %1 : $Klass
  ///
  ///      // In contrast in this case, we rely on looking through rc-identity
  ///      // uses to find the debug_value. In this case, the source info
  ///      // associated with the debug_value (%2) is no longer associated with
  ///      // the underlying access path we have been tracking upwards (%1 is in
  ///      // our access path list). Instead, we know that the debug_value is
  ///      // rc-identical to whatever value we were originally tracking up (%1)
  ///      // and thus the correct identifier to use is the direct name of the
  ///      // identifier alone since that source identifier must be some value
  ///      // in the source that by itself is rc-identical to whatever is being
  ///      // manipulated.
  ///      //
  ///      // The reason why we must do this is due to the behavior of the late
  ///      // optimizer and how it forms these patterns in the code.
  ///      %0a = apply %getStateWithOwningPointer() : $@convention(thin) () ->
  ///      @owned StateWithOwningPointer %1 = struct_extract %0a :
  ///      $StateWithOwningPointer, #StateWithOwningPointer.owningPtr
  ///      strong_retain %1 : $Klass
  ///      %2 = struct $Array(%0 : $Builtin.NativeObject, ...)
  ///      debug_value %2 : $Array, ...
  ///    }
  void printNote(llvm::raw_string_ostream &stream, StringRef name,
                 bool shouldPrintAccessPath = true);

  /// Convenience overload that calls:
  ///
  ///   printNote(stream, decl->getBaseName().userFacingName(),
  ///   shouldPrintAccessPath).
  void printNote(llvm::raw_string_ostream &stream, const ValueDecl *decl,
                 bool shouldPrintAccessPath = true) {
    printNote(stream, decl->getBaseName().userFacingName(),
              shouldPrintAccessPath);
  }

  /// Print out non-destructively the current access path we have found to
  /// stream.
  void printAccessPath(llvm::raw_string_ostream &stream);
};

} // anonymous namespace

void ValueToDeclInferrer::printAccessPath(llvm::raw_string_ostream &stream) {
  for (auto &pair : accessPath) {
    auto baseType = pair.first;
    auto &proj = pair.second;
    stream << ".";

    // WARNING: This must be kept insync with isSupportedProjection!
    switch (proj.getKind()) {
    case ProjectionKind::Upcast:
      stream << "upcast<" << proj.getCastType(baseType) << ">";
      continue;
    case ProjectionKind::RefCast:
      stream << "refcast<" << proj.getCastType(baseType) << ">";
      continue;
    case ProjectionKind::BitwiseCast:
      stream << "bitwise_cast<" << proj.getCastType(baseType) << ">";
      continue;
    case ProjectionKind::Struct:
    case ProjectionKind::Class:
      stream << proj.getVarDecl(baseType)->getBaseName();
      continue;
    case ProjectionKind::Tuple:
      stream << proj.getIndex();
      continue;
    case ProjectionKind::Enum:
      stream << proj.getEnumElementDecl(baseType)->getBaseName();
      continue;

    // Object -> Address projections can never be looked through unless they are
    // from a class where we have special logic for it only happening a single
    // time.
    case ProjectionKind::Box:
    case ProjectionKind::Index:
    case ProjectionKind::TailElems:
      llvm_unreachable(
          "Object -> Address projection should never be looked through!");
    }

    llvm_unreachable("Covered switch is not covered?!");
  }
}

void ValueToDeclInferrer::printNote(llvm::raw_string_ostream &stream,
                                    StringRef name,
                                    bool shouldPrintAccessPath) {
  stream << "of '" << name;
  if (shouldPrintAccessPath)
    printAccessPath(stream);
  stream << "'";
}

// WARNING: This must be kept insync with ValueToDeclInferrer::printNote(...).
static SingleValueInstruction *isSupportedProjection(Projection p, SILValue v) {
  switch (p.getKind()) {
  case ProjectionKind::Upcast:
  case ProjectionKind::RefCast:
  case ProjectionKind::BitwiseCast:
  case ProjectionKind::Struct:
  case ProjectionKind::Tuple:
  case ProjectionKind::Enum:
    return cast<SingleValueInstruction>(v);
    // Object -> Address projections can never be looked through.
  case ProjectionKind::Class:
  case ProjectionKind::Box:
  case ProjectionKind::Index:
  case ProjectionKind::TailElems:
    return nullptr;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

static bool hasNonInlinedDebugScope(SILInstruction *i) {
  if (auto *scope = i->getDebugScope())
    return !scope->InlinedCallSite;
  return false;
}

namespace {

/// A helper struct that attempts to infer the decl associated with a value from
/// one of its uses. It does this by searching the def-use graph locally for
/// debug_value instructions.
struct ValueUseToDeclInferrer {
  using Argument = ValueToDeclInferrer::Argument;
  using ArgumentKeyKind = ValueToDeclInferrer::ArgumentKeyKind;

  SmallPtrSet<swift::SILInstruction *, 8> visitedDebugValueInsts;
  ValueToDeclInferrer &object;
  ArgumentKeyKind keyKind;
  SmallVectorImpl<Argument> &resultingInferredDecls;

  bool findDecls(Operand *use, SILValue value);
};

} // anonymous namespace

bool ValueUseToDeclInferrer::findDecls(Operand *use, SILValue value) {
  // Skip type dependent operands.
  if (use->isTypeDependent())
    return false;

  // Then see if we have a debug_value that is associated with a non-inlined
  // debug scope. Such an instruction is an instruction that is from the
  // current function.
  auto debugInst = DebugVarCarryingInst(use->getUser());
  if (!debugInst)
    return false;

  LLVM_DEBUG(llvm::dbgs() << "Found DebugInst: " << **debugInst);
  if (!hasNonInlinedDebugScope(*debugInst))
    return false;

  // See if we have already inferred this debug_value as a potential source
  // for this instruction. In such a case, just return.
  if (!visitedDebugValueInsts.insert(*debugInst).second)
    return false;

  if (auto *decl = debugInst.getDecl()) {
    std::string msg;
    {
      llvm::raw_string_ostream stream(msg);
      // If we are not a top level use, we must be a rc-identical transitive
      // use. In such a case, we just print out the rc identical value
      // without a projection path. This is because we now have a better
      // name and the name is rc-identical to whatever was at the end of the
      // projection path but is not at the end of that projection path.
      object.printNote(stream, decl,
                       use->get() == value /*print projection path*/);
    }
    resultingInferredDecls.emplace_back(
        OptRemark::ArgumentKey{keyKind, "InferredValue"}, std::move(msg), decl);
    return true;
  }

  // If we did not have a decl, see if we were asked for testing
  // purposes to use SILDebugInfo to create a placeholder inferred
  // value.
  if (!DecllessDebugValueUseSILDebugInfo)
    return false;

  auto varInfo = debugInst.getVarInfo();
  if (!varInfo)
    return false;

  auto name = varInfo->Name;
  if (name.empty())
    return false;

  std::string msg;
  {
    llvm::raw_string_ostream stream(msg);
    object.printNote(stream, name,
                     use->get() == value /*print projection path*/);
  }
  resultingInferredDecls.push_back(Argument(
      {keyKind, "InferredValue"}, std::move(msg), debugInst->getLoc()));
  return true;
}

bool ValueToDeclInferrer::infer(
    ArgumentKeyKind keyKind, SILValue value,
    SmallVectorImpl<Argument> &resultingInferredDecls,
    bool allowSingleRefEltAddrPeek) {
  // Clear the stored access path at end of scope.
  SWIFT_DEFER { accessPath.clear(); };
  ValueUseToDeclInferrer valueUseInferrer{
      {}, *this, keyKind, resultingInferredDecls};
  bool foundSingleRefElementAddr = false;

  // This is a linear IR traversal using a 'falling while loop'. That means
  // every time through the loop we are trying to handle a case before we hit
  // the bottom of the while loop where we always return true (since we did not
  // hit a could not compute case). Reassign value and continue to go to the
  // next step.
  LLVM_DEBUG(llvm::dbgs() << "Searching for decls!\n");
  while (true) {
    LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *value);

    // First check for "identified values" like arguments and global_addr.
    if (auto *arg = dyn_cast<SILArgument>(value))
      if (auto *decl = arg->getDecl()) {
        std::string msg;
        {
          llvm::raw_string_ostream stream(msg);
          printNote(stream, decl);
        }
        resultingInferredDecls.push_back(
            Argument({keyKind, "InferredValue"}, std::move(msg), decl));
        return true;
      }

    if (auto *ga = dyn_cast<GlobalAddrInst>(value))
      if (auto *decl = ga->getReferencedGlobal()->getDecl()) {
        std::string msg;
        {
          llvm::raw_string_ostream stream(msg);
          printNote(stream, decl);
        }
        resultingInferredDecls.push_back(
            Argument({keyKind, "InferredValue"}, std::move(msg), decl));
        return true;
      }

    if (auto *ari = dyn_cast<AllocRefInst>(value)) {
      if (auto *decl = ari->getDecl()) {
        std::string msg;
        {
          llvm::raw_string_ostream stream(msg);
          printNote(stream, decl);
        }
        resultingInferredDecls.push_back(
            Argument({keyKind, "InferredValue"}, std::move(msg), decl));
        return true;
      }
    }

    if (auto *abi = dyn_cast<AllocBoxInst>(value)) {
      if (auto *decl = abi->getDecl()) {
        std::string msg;
        {
          llvm::raw_string_ostream stream(msg);
          printNote(stream, decl);
        }

        resultingInferredDecls.push_back(
            Argument({keyKind, "InferredValue"}, std::move(msg), decl));
        return true;
      }
    }

    // A pattern that we see around empty array storage is:
    //
    //   %0 = global_addr @_swiftEmptyArrayStorage : $*_SwiftEmptyArrayStorage
    //   %1 = address_to_pointer %0 : $*_SwiftEmptyArrayStorage to
    //   $Builtin.RawPointer %2 = raw_pointer_to_ref %1 : $Builtin.RawPointer to
    //   $__EmptyArrayStorage
    //
    // Recognize this case.
    {
      GlobalAddrInst *gai;
      if (match(value, m_RawPointerToRefInst(
                           m_AddressToPointerInst(m_GlobalAddrInst(gai))))) {
        if (auto *decl = gai->getReferencedGlobal()->getDecl()) {
          std::string msg;
          {
            llvm::raw_string_ostream stream(msg);
            printNote(stream, decl);
          }
          resultingInferredDecls.push_back(
              Argument({keyKind, "InferredValue"}, std::move(msg), decl));
          return true;
        }
      }
    }

    // We prefer decls not from uses since these are inherently noisier. Still,
    // it is better than nothing.
    bool foundDeclFromUse = false;

    if (auto *asi = dyn_cast<AllocStackInst>(value)) {
      if (auto *decl = asi->getDecl()) {
        std::string msg;
        {
          llvm::raw_string_ostream stream(msg);
          printNote(stream, decl);
        }
        resultingInferredDecls.push_back(
            Argument({keyKind, "InferredValue"}, std::move(msg), decl));
        return true;
      }

      // See if we have a single init alloc_stack and can infer a
      // debug_value from that.
      LLVM_DEBUG(llvm::dbgs() << "Checking for single init use!\n");
      if (auto *initUse = getSingleInitAllocStackUse(asi)) {
        LLVM_DEBUG(llvm::dbgs() << "Found one: " << *initUse->getUser());
        if (auto *si = dyn_cast<StoreInst>(initUse->getUser())) {
          for (auto *use : si->getSrc()->getUses()) {
            foundDeclFromUse |= valueUseInferrer.findDecls(use, value);
          }
        }
        if (auto *cai = dyn_cast<CopyAddrInst>(initUse->getUser())) {
          for (auto *use : cai->getSrc()->getUses()) {
            foundDeclFromUse |= valueUseInferrer.findDecls(use, value);
          }
        }
      }
    }

    // Then visit our users (ignoring rc identical transformations) and see if
    // we can find a debug_value that provides us with a decl we can use to
    // construct an argument.
    //
    // The reason why we do this is that sometimes we reform a struct from its
    // constituent parts and then construct the debug_value from that. For
    // instance, if we FSOed.
    rcfi.visitRCUses(value, [&](Operand *use) {
      foundDeclFromUse |= valueUseInferrer.findDecls(use, value);
    });

    for (Operand *use : value->getUses()) {
      if (auto *eir = dyn_cast<EndInitLetRefInst>(use->getUser())) {
        rcfi.visitRCUses(eir, [&](Operand *use) {
          foundDeclFromUse |= valueUseInferrer.findDecls(use, value);
        });
      }
    }

    // At this point, we could not infer any argument. See if we can look up the
    // def-use graph and come up with a good location after looking through
    // loads and projections.
    if (auto *li = dyn_cast<LoadInst>(value)) {
      value = stripAccessMarkers(li->getOperand());
      continue;
    }

    if (auto proj = Projection(value)) {
      if (auto *projInst = isSupportedProjection(proj, value)) {
        value = projInst->getOperand(0);
        accessPath.emplace_back(value->getType(), proj);
        continue;
      }

      // Check if we had a ref_element_addr and our caller said that they were
      // ok with skipping a single one.
      //
      // Examples of users: begin_access, end_access.
      if (allowSingleRefEltAddrPeek &&
          proj.getKind() == ProjectionKind::Class) {
        if (!foundSingleRefElementAddr) {
          value = cast<RefElementAddrInst>(value)->getOperand();
          accessPath.emplace_back(value->getType(), proj);
          foundSingleRefElementAddr = true;
          continue;
        }
      }
    }

    // TODO: We could emit at this point a msg for temporary allocations.

    // If we reached this point, we finished falling through the loop and return
    // if we found any decls from uses. We always process everything so we /can/
    // potentially emit multiple diagnostics.
    return foundDeclFromUse;
  }
}

//===----------------------------------------------------------------------===//
//                        Opt Remark Generator Visitor
//===----------------------------------------------------------------------===//

namespace {

struct AssemblyVisionRemarkGeneratorInstructionVisitor
    : public SILInstructionVisitor<
          AssemblyVisionRemarkGeneratorInstructionVisitor> {
  SILModule &mod;
  OptRemark::Emitter ORE;

  /// A class that we use to infer the decl that is associated with a
  /// miscellaneous SIL value. This is just a heuristic that is to taste.
  ValueToDeclInferrer valueToDeclInferrer;

  AssemblyVisionRemarkGeneratorInstructionVisitor(SILFunction &fn,
                                                  RCIdentityFunctionInfo &rcfi)
      : mod(fn.getModule()), ORE(DEBUG_TYPE, fn), valueToDeclInferrer(rcfi) {}

  void visitStrongRetainInst(StrongRetainInst *sri);
  void visitStrongReleaseInst(StrongReleaseInst *sri);
  void visitRetainValueInst(RetainValueInst *rvi);
  void visitReleaseValueInst(ReleaseValueInst *rvi);
  void visitAllocRefInstBase(AllocRefInstBase *ari);
  void visitAllocRefInst(AllocRefInst *ari);
  void visitAllocRefDynamicInst(AllocRefDynamicInst *ari);
  void visitAllocBoxInst(AllocBoxInst *abi);
  void visitSILInstruction(SILInstruction *) {}
  void visitBeginAccessInst(BeginAccessInst *bai);
  void visitEndAccessInst(EndAccessInst *eai);
  void visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *ccabi);
  void visitUnconditionalCheckedCastAddrInst(
      UnconditionalCheckedCastAddrInst *uccai);
};

} // anonymous namespace

void AssemblyVisionRemarkGeneratorInstructionVisitor::
    visitUnconditionalCheckedCastAddrInst(
        UnconditionalCheckedCastAddrInst *uccai) {
  ORE.emit([&]() {
    using namespace OptRemark;
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs = valueToDeclInferrer.infer(
        ArgumentKeyKind::Note, uccai->getSrc(), inferredArgs,
        true /*allow single ref elt peek*/);
    (void)foundArgs;

    // Use the actual source loc of the
    auto remark = RemarkMissed("memory", *uccai)
                  << "unconditional runtime cast of value with type '"
                  << NV("ValueType", uccai->getSrc()->getType()) << "' to '"
                  << NV("CastType", uccai->getDest()->getType()) << "'";
    for (auto arg : inferredArgs) {
      remark << arg;
    }
    return remark;
  });
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::
    visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *ccabi) {
  ORE.emit([&]() {
    using namespace OptRemark;
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs = valueToDeclInferrer.infer(
        ArgumentKeyKind::Note, ccabi->getSrc(), inferredArgs,
        true /*allow single ref elt peek*/);
    (void)foundArgs;

    // Use the actual source loc of the
    auto remark = RemarkMissed("memory", *ccabi)
                  << "conditional runtime cast of value with type '"
                  << NV("ValueType", ccabi->getSrc()->getType()) << "' to '"
                  << NV("CastType", ccabi->getDest()->getType()) << "'";
    for (auto arg : inferredArgs) {
      remark << arg;
    }
    return remark;
  });
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::visitBeginAccessInst(
    BeginAccessInst *bai) {
  ORE.emit([&]() {
    using namespace OptRemark;
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs = valueToDeclInferrer.infer(
        ArgumentKeyKind::Note, bai->getOperand(), inferredArgs,
        true /*allow single ref elt peek*/);
    (void)foundArgs;

    // Use the actual source loc of the
    auto remark =
        RemarkMissed("memory", *bai, SourceLocInferenceBehavior::ForwardScan)
        << "begin exclusive access to value of type '"
        << NV("ValueType", bai->getOperand()->getType()) << "'";
    for (auto arg : inferredArgs) {
      remark << arg;
    }
    return remark;
  });
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::visitEndAccessInst(
    EndAccessInst *eai) {
  ORE.emit([&]() {
    using namespace OptRemark;
    auto *bai = cast<BeginAccessInst>(eai->getOperand());
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs = valueToDeclInferrer.infer(
        ArgumentKeyKind::Note, bai->getOperand(), inferredArgs,
        true /*allow single ref elt peek*/);
    (void)foundArgs;

    // Use the actual source loc of the begin_access if it works. Otherwise,
    // scan backwards.
    auto remark =
        RemarkMissed("memory", *eai,
                     SourceLocInferenceBehavior::BackwardThenForwardAlwaysInfer,
                     SourceLocPresentationKind::EndRange)
        << "end exclusive access to value of type '"
        << NV("ValueType", eai->getOperand()->getType()) << "'";
    for (auto arg : inferredArgs) {
      remark << arg;
    }
    return remark;
  });
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::visitStrongRetainInst(
    StrongRetainInst *sri) {
  ORE.emit([&]() {
    using namespace OptRemark;
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs = valueToDeclInferrer.infer(ArgumentKeyKind::Note,
                                               sri->getOperand(), inferredArgs);
    (void)foundArgs;

    // Retains begin a lifetime scope so we infer scan forward.
    auto remark =
        RemarkMissed("memory", *sri,
                     SourceLocInferenceBehavior::ForwardScanAlwaysInfer)
        << "retain of type '" << NV("ValueType", sri->getOperand()->getType())
        << "'";
    for (auto arg : inferredArgs) {
      remark << arg;
    }
    return remark;
  });
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::visitStrongReleaseInst(
    StrongReleaseInst *sri) {
  ORE.emit([&]() {
    using namespace OptRemark;
    // Releases end a lifetime scope so we infer scan backward.
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs = valueToDeclInferrer.infer(ArgumentKeyKind::Note,
                                               sri->getOperand(), inferredArgs);
    (void)foundArgs;

    auto remark =
        RemarkMissed("memory", *sri,
                     SourceLocInferenceBehavior::BackwardThenForwardAlwaysInfer,
                     SourceLocPresentationKind::EndRange)
        << "release of type '" << NV("ValueType", sri->getOperand()->getType())
        << "'";
    for (auto arg : inferredArgs) {
      remark << arg;
    }
    return remark;
  });
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::visitRetainValueInst(
    RetainValueInst *rvi) {
  ORE.emit([&]() {
    using namespace OptRemark;
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs = valueToDeclInferrer.infer(ArgumentKeyKind::Note,
                                               rvi->getOperand(), inferredArgs);
    (void)foundArgs;
    // Retains begin a lifetime scope, so we infer scan forwards.
    auto remark =
        RemarkMissed("memory", *rvi,
                     SourceLocInferenceBehavior::ForwardScanAlwaysInfer)
        << "retain of type '" << NV("ValueType", rvi->getOperand()->getType())
        << "'";
    for (auto arg : inferredArgs) {
      remark << arg;
    }
    return remark;
  });
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::visitReleaseValueInst(
    ReleaseValueInst *rvi) {
  ORE.emit([&]() {
    using namespace OptRemark;
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs = valueToDeclInferrer.infer(ArgumentKeyKind::Note,
                                               rvi->getOperand(), inferredArgs);
    (void)foundArgs;

    // Releases end a lifetime scope so we infer scan backward.
    auto remark =
        RemarkMissed("memory", *rvi,
                     SourceLocInferenceBehavior::BackwardThenForwardAlwaysInfer)
        << "release of type '" << NV("ValueType", rvi->getOperand()->getType())
        << "'";
    for (auto arg : inferredArgs) {
      remark << arg;
    }
    return remark;
  });
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::visitAllocRefInstBase(
    AllocRefInstBase *ari) {
  if (ari->canAllocOnStack()) {
    return ORE.emit([&]() {
      using namespace OptRemark;
      SmallVector<Argument, 8> inferredArgs;
      bool foundArgs =
          valueToDeclInferrer.infer(ArgumentKeyKind::Note, ari, inferredArgs);
      (void)foundArgs;
      auto resultRemark =
          RemarkPassed("memory", *ari, SourceLocInferenceBehavior::ForwardScan)
          << "stack allocated ref of type '" << NV("ValueType", ari->getType())
          << "'";
      for (auto &arg : inferredArgs)
        resultRemark << arg;
      return resultRemark;
    });
  }

  return ORE.emit([&]() {
    using namespace OptRemark;
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs =
        valueToDeclInferrer.infer(ArgumentKeyKind::Note, ari, inferredArgs);
    (void)foundArgs;

    auto resultRemark =
        RemarkMissed("memory", *ari, SourceLocInferenceBehavior::ForwardScan)
        << "heap allocated ref of type '" << NV("ValueType", ari->getType())
        << "'";
    for (auto &arg : inferredArgs)
      resultRemark << arg;
    return resultRemark;
  });
}


void AssemblyVisionRemarkGeneratorInstructionVisitor::visitAllocRefInst(
  AllocRefInst *ari) {
  visitAllocRefInstBase(ari);
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::visitAllocRefDynamicInst(
  AllocRefDynamicInst *ari) {
  visitAllocRefInstBase(ari);
}

void AssemblyVisionRemarkGeneratorInstructionVisitor::visitAllocBoxInst(
    AllocBoxInst *abi) {
  return ORE.emit([&]() {
    using namespace OptRemark;
    SmallVector<Argument, 8> inferredArgs;
    bool foundArgs =
        valueToDeclInferrer.infer(ArgumentKeyKind::Note, abi, inferredArgs);
    (void)foundArgs;

    auto resultRemark =
        RemarkMissed("memory", *abi, SourceLocInferenceBehavior::ForwardScan)
        << "heap allocated box of type '" << NV("ValueType", abi->getType())
        << "'";
    for (auto &arg : inferredArgs)
      resultRemark << arg;
    return resultRemark;
  });
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class AssemblyVisionRemarkGenerator : public SILFunctionTransform {
  ~AssemblyVisionRemarkGenerator() override {}

  bool isOptRemarksEnabled() {
    auto *fn = getFunction();
    // TODO: Put this on LangOpts as a helper.
    auto &langOpts = fn->getASTContext().LangOpts;

    // If we are supposed to emit remarks, always emit.
    if (bool(langOpts.OptimizationRemarkMissedPattern) ||
        bool(langOpts.OptimizationRemarkPassedPattern) ||
        fn->getModule().getSILRemarkStreamer())
      return true;

    // Otherwise, first check if our function has a force emit opt remark prefix
    // semantics tag.
    if (fn->hasSemanticsAttrThatStartsWith(
            semantics::FORCE_EMIT_OPT_REMARK_PREFIX))
      return true;

    // Otherwise, check if we have a self parameter that is a nominal type that
    // is marked with the @_assemblyVision attribute.
    if (fn->hasSelfParam()) {
      if (auto *nomType = fn->getSelfArgument()
                              ->getType()
                              .getNominalOrBoundGenericNominal()) {
        LLVM_DEBUG(llvm::dbgs() << "Checking for remark on: "
                                << nomType->getName().get() << "\n");
        if (nomType->shouldEmitAssemblyVisionRemarksOnMethods()) {
          LLVM_DEBUG(llvm::dbgs() << "Success! Will emit remarks!!\n");
          return true;
        }
        LLVM_DEBUG(llvm::dbgs() << "Fail! No remarks will be emitted!!\n");
      }
    }

    return false;
  }

  /// The entry point to the transformation.
  void run() override {
    if (!isOptRemarksEnabled())
      return;

    auto *fn = getFunction();

    // Skip top level implicit functions and top level autogenerated functions,
    // unless we were asked by the user to emit them.
    if (!ForceVisitImplicitAutogeneratedFunctions) {
      // Skip implicit functions generated by Sema.
      if (auto *ctx = fn->getDeclContext()) {
        if (auto *decl = ctx->getAsDecl()) {
          if (decl->isImplicit()) {
            LLVM_DEBUG(llvm::dbgs() << "Skipping implicit decl function: "
                                    << fn->getName() << "\n");
            return;
          }
        }
      }

      // Skip autogenerated functions generated by SILGen.
      if (auto loc = fn->getDebugScope()->getLoc()) {
        if (loc.isAutoGenerated()) {
          LLVM_DEBUG(llvm::dbgs() << "Skipping autogenerated function: "
                                  << fn->getName() << "\n");
          return;
        }
      }
    }

    LLVM_DEBUG(llvm::dbgs() << "Visiting: " << fn->getName() << "\n");
    auto &rcfi = *getAnalysis<RCIdentityAnalysis>()->get(fn);
    AssemblyVisionRemarkGeneratorInstructionVisitor visitor(*fn, rcfi);
    for (auto &block : *fn) {
      for (auto &inst : block) {
        visitor.visit(&inst);
      }
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createAssemblyVisionRemarkGenerator() {
  return new AssemblyVisionRemarkGenerator();
}
