// Copyright 2021 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "plier/transforms/loop_utils.hpp"

#include <llvm/ADT/SmallVector.h>

#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/Dialect/SCF/SCF.h>
#include <mlir/Dialect/StandardOps/IR/Ops.h>
#include <mlir/IR/BlockAndValueMapping.h>
#include <mlir/IR/Dominance.h>
#include <mlir/IR/PatternMatch.h>
#include <mlir/Support/LogicalResult.h>

#include "plier/dialect.hpp"

#include "plier/transforms/cast_utils.hpp"
#include "plier/transforms/const_utils.hpp"

namespace {
template <typename Op>
Op get_next_op(llvm::iterator_range<mlir::Block::iterator> &iters) {
  if (iters.empty()) {
    return nullptr;
  }
  auto res = mlir::dyn_cast<Op>(iters.begin());
  if (res) {
    auto next = std::next(iters.begin());
    iters = {next, iters.end()};
  }
  return res;
}

mlir::Value get_last_iter_value(mlir::PatternRewriter &builder,
                                mlir::Location loc, mlir::Value lower_bound,
                                mlir::Value upper_bound, mlir::Value step) {
  auto len = builder.createOrFold<mlir::SubIOp>(loc, upper_bound, lower_bound);
  auto count = builder.createOrFold<mlir::SignedDivIOp>(loc, len, step);
  auto inc = builder.createOrFold<mlir::MulIOp>(loc, count, step);
  return builder.createOrFold<mlir::AddIOp>(loc, lower_bound, inc);
}

} // namespace

mlir::LogicalResult plier::lowerWhileToFor(
    plier::GetiterOp getiter, mlir::PatternRewriter &builder,
    llvm::function_ref<std::tuple<mlir::Value, mlir::Value, mlir::Value>(
        mlir::OpBuilder &, mlir::Location)>
        getBounds,
    llvm::function_ref<mlir::Value(mlir::OpBuilder &, mlir::Location,
                                   mlir::Type, mlir::Value)>
        getIterVal,
    llvm::function_ref<void(mlir::scf::ForOp)> results) {
  llvm::SmallVector<mlir::scf::WhileOp, 4> toProcess;
  for (auto user : getiter.getOperation()->getUsers()) {
    if (auto whileOp =
            mlir::dyn_cast<mlir::scf::WhileOp>(user->getParentOp())) {
      toProcess.emplace_back(whileOp);
    }
  }

  auto loc = getiter.getLoc();
  mlir::Value zeroVal;
  auto getZeroIndex = [&]() {
    if (!zeroVal) {
      mlir::OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPoint(getiter);
      zeroVal = builder.create<mlir::ConstantIndexOp>(loc, 0);
    }
    return zeroVal;
  };

  auto getNeg = [&](mlir::Value value) {
    return builder.createOrFold<mlir::SubIOp>(loc, getZeroIndex(), value);
  };

  bool changed = false;
  for (auto whileOp : toProcess) {
    auto &beforeBlock = whileOp.before().front();
    auto iters = llvm::iterator_range<mlir::Block::iterator>(beforeBlock);
    auto iternext = get_next_op<plier::IternextOp>(iters);
    auto pairfirst = get_next_op<plier::PairfirstOp>(iters);
    auto pairsecond = get_next_op<plier::PairsecondOp>(iters);
    while (get_next_op<plier::CastOp>(iters)) {
    } // skip casts
    auto beforeTerm = get_next_op<mlir::scf::ConditionOp>(iters);

    auto skipCasts = [](mlir::Value op) {
      while (auto cast =
                 mlir::dyn_cast_or_null<plier::CastOp>(op.getDefiningOp())) {
        op = cast.getOperand();
      }
      return op;
    };
    if (!iternext || !pairsecond || !beforeTerm ||
        skipCasts(beforeTerm.condition()) != pairsecond)
      continue;

    auto &afterBlock = whileOp.after().front();

    auto indexCast = [&](mlir::Value val) -> mlir::Value {
      return ::plier::index_cast(builder, loc, val);
    };

    auto bounds = getBounds(builder, loc);
    auto origLowerBound = indexCast(std::get<0>(bounds));
    auto origUpperBound = indexCast(std::get<1>(bounds));
    auto origStep = indexCast(std::get<2>(bounds));

    // scf::ForOp/ParallelOp doesn't support negative step, so generate
    // IfOp and 2 version for different step signs
    // branches for const steps will be pruned later
    auto genFor = [&](bool positive) {
      auto getLoopBodyBuilder = [&](bool positive) {
        return [&, positive](mlir::OpBuilder &builder, mlir::Location loc,
                             mlir::Value iv, mlir::ValueRange iterargs) {
          if (!positive) {
            iv = getNeg(iv);
          }
          mlir::BlockAndValueMapping mapper;
          assert(beforeBlock.getNumArguments() == iterargs.size());
          assert(afterBlock.getNumArguments() == beforeTerm.args().size());
          mapper.map(beforeBlock.getArguments(), iterargs);
          for (auto it :
               llvm::zip(afterBlock.getArguments(), beforeTerm.args())) {
            auto blockArg = std::get<0>(it);
            auto termArg = std::get<1>(it);
            if (pairfirst && termArg == pairfirst) // iter arg
            {
              auto iterVal = getIterVal(builder, loc, pairfirst.getType(), iv);
              mapper.map(blockArg, iterVal);
            } else {
              mapper.map(blockArg, mapper.lookupOrDefault(termArg));
            }
          }

          for (auto &op : afterBlock) // with terminator
          {
            builder.clone(op, mapper);
          }
        };
      };

      auto lowerBound = origLowerBound;
      auto upperBound = origUpperBound;
      auto step = origStep;

      if (!positive) {
        lowerBound = getNeg(lowerBound);
        upperBound = getNeg(upperBound);
        step = getNeg(step);
      }

      return builder.create<mlir::scf::ForOp>(loc, lowerBound, upperBound, step,
                                              whileOp.getOperands(), // iterArgs
                                              getLoopBodyBuilder(positive));
    };

    auto getIfBodyBuilder = [&](bool positive) {
      return [&, positive](mlir::OpBuilder &builder, mlir::Location loc) {
        auto loop_op = genFor(positive);
        if (results)
          results(loop_op);

        builder.create<mlir::scf::YieldOp>(loc, loop_op.getResults());
      };
    };

    builder.setInsertionPoint(whileOp);
    auto stepSign = builder.createOrFold<mlir::CmpIOp>(
        loc, mlir::CmpIPredicate::sge, origStep, getZeroIndex());

    auto loopResults = [&]() -> mlir::ValueRange {
      auto ind = getConstVal<mlir::IntegerAttr>(stepSign);
      auto resTypes = whileOp.getOperands().getTypes();
      if (!ind)
        return builder
            .create<mlir::scf::IfOp>(loc, resTypes, stepSign,
                                     getIfBodyBuilder(true),
                                     getIfBodyBuilder(false))
            .getResults();

      auto reg = builder.create<mlir::scf::ExecuteRegionOp>(loc, resTypes);
      auto &regBlock = reg.region().emplaceBlock();
      mlir::OpBuilder::InsertionGuard g(builder);
      builder.setInsertionPointToStart(&regBlock);
      getIfBodyBuilder(ind.getValue() != 0)(builder, loc);
      return reg.getResults();
    }();

    assert(whileOp.getNumResults() >= loopResults.size());
    builder.updateRootInPlace(whileOp, [&]() {
      assert(whileOp.getNumResults() == beforeTerm.args().size());
      for (auto it : llvm::zip(whileOp.getResults(), beforeTerm.args())) {
        auto oldRes = std::get<0>(it);
        auto operand = std::get<1>(it);
        for (auto it2 : llvm::enumerate(beforeBlock.getArguments())) {
          auto arg = it2.value();
          if (arg == operand) {
            assert(it2.index() < loopResults.size());
            auto newRes = loopResults[static_cast<unsigned>(it2.index())];
            oldRes.replaceAllUsesWith(newRes);
            break;
          }
        }
        if (pairfirst && operand == pairfirst && !oldRes.getUsers().empty()) {
          auto val = get_last_iter_value(builder, loc, origLowerBound,
                                         origUpperBound, origStep);
          auto newRes =
              builder.createOrFold<plier::CastOp>(loc, oldRes.getType(), val);
          oldRes.replaceAllUsesWith(newRes);
        }
        assert(oldRes.getUsers().empty());
      }
    });

    assert(whileOp.getOperation()->getUsers().empty());
    builder.eraseOp(whileOp);
    changed = true;
  }

  if (getiter.getOperation()->getUsers().empty()) {
    builder.eraseOp(getiter);
    changed = true;
  }
  return mlir::success(changed);
}

// TODO: Copypasted from mlir
namespace {
using namespace mlir;

/// Verify there are no nested ParallelOps.
static bool hasNestedParallelOp(scf::ParallelOp ploop) {
  auto walkResult = ploop.getBody()->walk(
      [](scf::ParallelOp) { return WalkResult::interrupt(); });
  return walkResult.wasInterrupted();
}

/// Verify equal iteration spaces.
static bool equalIterationSpaces(scf::ParallelOp firstPloop,
                                 scf::ParallelOp secondPloop) {
  if (firstPloop.getNumLoops() != secondPloop.getNumLoops())
    return false;

  auto matchOperands = [&](const OperandRange &lhs,
                           const OperandRange &rhs) -> bool {
    // TODO: Extend this to support aliases and equal constants.
    return std::equal(lhs.begin(), lhs.end(), rhs.begin());
  };
  return matchOperands(firstPloop.lowerBound(), secondPloop.lowerBound()) &&
         matchOperands(firstPloop.upperBound(), secondPloop.upperBound()) &&
         matchOperands(firstPloop.step(), secondPloop.step());
}

/// Checks if the parallel loops have mixed access to the same buffers. Returns
/// `true` if the first parallel loop writes to the same indices that the second
/// loop reads.
static bool haveNoReadsAfterWriteExceptSameIndex(
    scf::ParallelOp firstPloop, scf::ParallelOp secondPloop,
    const BlockAndValueMapping &firstToSecondPloopIndices) {
  DenseMap<Value, SmallVector<ValueRange, 1>> bufferStores;
  firstPloop.getBody()->walk([&](memref::StoreOp store) {
    bufferStores[store.getMemRef()].push_back(store.indices());
  });
  auto walkResult = secondPloop.getBody()->walk([&](memref::LoadOp load) {
    // Stop if the memref is defined in secondPloop body. Careful alias analysis
    // is needed.
    auto *memrefDef = load.getMemRef().getDefiningOp();
    if (memrefDef && memrefDef->getBlock() == load->getBlock())
      return WalkResult::interrupt();

    auto write = bufferStores.find(load.getMemRef());
    if (write == bufferStores.end())
      return WalkResult::advance();

    // Allow only single write access per buffer.
    if (write->second.size() != 1)
      return WalkResult::interrupt();

    // Check that the load indices of secondPloop coincide with store indices of
    // firstPloop for the same memrefs.
    auto storeIndices = write->second.front();
    auto loadIndices = load.indices();
    if (storeIndices.size() != loadIndices.size())
      return WalkResult::interrupt();
    for (size_t i = 0, e = storeIndices.size(); i < e; ++i) {
      if (firstToSecondPloopIndices.lookupOrDefault(storeIndices[i]) !=
          loadIndices[i])
        return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  return !walkResult.wasInterrupted();
}

/// Analyzes dependencies in the most primitive way by checking simple read and
/// write patterns.
static LogicalResult
verifyDependencies(scf::ParallelOp firstPloop, scf::ParallelOp secondPloop,
                   const BlockAndValueMapping &firstToSecondPloopIndices) {
  for (auto res : firstPloop.getResults()) {
    for (auto user : res.getUsers()) {
      if (secondPloop->isAncestor(user))
        return mlir::failure();
    }
  }

  if (!haveNoReadsAfterWriteExceptSameIndex(firstPloop, secondPloop,
                                            firstToSecondPloopIndices))
    return failure();

  BlockAndValueMapping secondToFirstPloopIndices;
  secondToFirstPloopIndices.map(secondPloop.getBody()->getArguments(),
                                firstPloop.getBody()->getArguments());
  return success(haveNoReadsAfterWriteExceptSameIndex(
      secondPloop, firstPloop, secondToFirstPloopIndices));
}

static bool
isFusionLegal(scf::ParallelOp firstPloop, scf::ParallelOp secondPloop,
              const BlockAndValueMapping &firstToSecondPloopIndices) {
  return !hasNestedParallelOp(firstPloop) &&
         !hasNestedParallelOp(secondPloop) &&
         equalIterationSpaces(firstPloop, secondPloop) &&
         succeeded(verifyDependencies(firstPloop, secondPloop,
                                      firstToSecondPloopIndices));
}

/// Prepends operations of firstPloop's body into secondPloop's body.
static bool fuseIfLegal(scf::ParallelOp firstPloop,
                        scf::ParallelOp &secondPloop, OpBuilder &b) {
  BlockAndValueMapping firstToSecondPloopIndices;
  firstToSecondPloopIndices.map(firstPloop.getBody()->getArguments(),
                                secondPloop.getBody()->getArguments());

  if (!isFusionLegal(firstPloop, secondPloop, firstToSecondPloopIndices))
    return false;

  auto init1 = firstPloop.initVals();
  auto numResults1 = init1.size();
  auto init2 = secondPloop.initVals();
  auto numResults2 = init2.size();

  SmallVector<mlir::Value> newInitVars;
  newInitVars.reserve(numResults1 + numResults2);
  newInitVars.assign(init2.begin(), init2.end());
  newInitVars.append(init1.begin(), init1.end());

  b.setInsertionPoint(secondPloop);
  auto newSecondPloop = b.create<mlir::scf::ParallelOp>(
      secondPloop.getLoc(), secondPloop.lowerBound(), secondPloop.upperBound(),
      secondPloop.step(), newInitVars);
  if (secondPloop->hasAttr(plier::attributes::getParallelName()))
    newSecondPloop->setAttr(plier::attributes::getParallelName(),
                            mlir::UnitAttr::get(b.getContext()));

  newSecondPloop.getRegion().getBlocks().splice(
      newSecondPloop.getRegion().begin(), secondPloop.getRegion().getBlocks());
  auto term =
      mlir::cast<mlir::scf::YieldOp>(newSecondPloop.getBody()->getTerminator());

  b.setInsertionPointToStart(newSecondPloop.getBody());
  for (auto &op : firstPloop.getBody()->without_terminator()) {
    if (isa<mlir::scf::ReduceOp>(op)) {
      mlir::OpBuilder::InsertionGuard g(b);
      b.setInsertionPoint(term);
      b.clone(op, firstToSecondPloopIndices);
    } else {
      b.clone(op, firstToSecondPloopIndices);
    }
  }
  firstPloop.replaceAllUsesWith(
      newSecondPloop.getResults().take_back(numResults1));
  firstPloop.erase();
  secondPloop.replaceAllUsesWith(
      newSecondPloop.getResults().take_front(numResults2));
  secondPloop.erase();
  secondPloop = newSecondPloop;
  return true;
}

bool hasNoEffect(mlir::Operation *op) {
  if (op->getNumRegions() != 0)
    return false;

  if (auto interface = dyn_cast<MemoryEffectOpInterface>(op))
    return !interface.hasEffect<mlir::MemoryEffects::Read>() &&
           !interface.hasEffect<mlir::MemoryEffects::Write>();

  return !op->hasTrait<::mlir::OpTrait::HasRecursiveSideEffects>();
}

bool hasNoEffect(mlir::scf::ParallelOp currentPloop, mlir::Operation *op) {
  if (currentPloop && currentPloop->getNumResults() != 0) {
    for (auto arg : op->getOperands()) {
      if (llvm::is_contained(currentPloop.getResults(), arg))
        return false;
    }
  }

  return hasNoEffect(op);
}
} // namespace

mlir::LogicalResult plier::naivelyFuseParallelOps(Region &region) {
  OpBuilder b(region);
  // Consider every single block and attempt to fuse adjacent loops.
  bool changed = false;
  SmallVector<SmallVector<scf::ParallelOp, 8>, 1> ploopChains;
  for (auto &block : region) {
    for (auto &op : block)
      for (auto &innerReg : op.getRegions())
        if (succeeded(naivelyFuseParallelOps(innerReg)))
          changed = true;

    ploopChains.clear();
    ploopChains.push_back({});
    // Not using `walk()` to traverse only top-level parallel loops and also
    // make sure that there are no side-effecting ops between the parallel
    // loops.
    scf::ParallelOp currentPloop;
    bool noSideEffects = true;
    for (auto &op : block) {
      if (auto ploop = dyn_cast<scf::ParallelOp>(op)) {
        currentPloop = ploop;
        if (noSideEffects) {
          ploopChains.back().push_back(ploop);
        } else {
          ploopChains.push_back({ploop});
          noSideEffects = true;
        }
        continue;
      }
      // TODO: Handle region side effects properly.
      noSideEffects &= hasNoEffect(currentPloop, &op);
    }
    for (llvm::MutableArrayRef<scf::ParallelOp> ploops : ploopChains) {
      for (size_t i = 0, e = ploops.size(); i + 1 < e; ++i)
        if (fuseIfLegal(ploops[i], ploops[i + 1], b))
          changed = true;
    }
  }
  return mlir::success(changed);
}

LogicalResult plier::prepareForFusion(Region &region) {
  DominanceInfo dom(region.getParentOp());
  bool changed = false;
  for (auto &block : region) {
    for (auto &op : llvm::make_early_inc_range(block)) {
      for (auto &innerReg : op.getRegions())
        if (succeeded(prepareForFusion(innerReg)))
          changed = true;

      if (!isa<scf::ParallelOp>(op))
        continue;

      auto it = Block::iterator(op);
      if (it == block.begin())
        continue;
      --it;

      auto terminate = false;
      while (!terminate) {
        auto &currentOp = *it;
        if (isa<scf::ParallelOp>(currentOp))
          break;

        if (it == block.begin()) {
          terminate = true;
        } else {
          --it;
        }

        bool canMove = [&]() {
          if (!hasNoEffect(&currentOp))
            return false;

          for (auto user : currentOp.getUsers()) {
            if (op.isAncestor(user) || !dom.properlyDominates(&op, user))
              return false;
          }

          return true;
        }();

        if (canMove) {
          currentOp.moveAfter(&op);
          changed = true;
        }
      }
    }
  }
  return mlir::success(changed);
}
