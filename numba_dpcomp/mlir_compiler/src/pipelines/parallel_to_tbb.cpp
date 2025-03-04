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

#include "pipelines/parallel_to_tbb.hpp"

#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/Dialect/SCF/SCF.h>
#include <mlir/Dialect/StandardOps/IR/Ops.h>
#include <mlir/IR/BlockAndValueMapping.h>
#include <mlir/Pass/Pass.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Transforms/GreedyPatternRewriteDriver.h>

#include "plier/dialect.hpp"

#include "pipelines/base_pipeline.hpp"
#include "pipelines/lower_to_llvm.hpp"
#include "plier/compiler/pipeline_registry.hpp"
#include "plier/pass/rewrite_wrapper.hpp"
#include "plier/transforms/const_utils.hpp"
#include "plier/transforms/func_utils.hpp"

namespace {
mlir::MemRefType getReduceType(mlir::Type type, int64_t count) {
  if (type.isIntOrFloat()) {
    return mlir::MemRefType::get(count, type);
  }
  return {};
}

mlir::Attribute getReduceInitVal(mlir::Type type, mlir::Block &reduceBlock) {
  if (!llvm::hasSingleElement(reduceBlock.without_terminator())) {
    return {};
  }
  auto &reduceOp = reduceBlock.front();
  double reduceInit;
  if (mlir::isa<mlir::AddFOp, mlir::AddIOp, mlir::SubFOp, mlir::SubIOp>(
          reduceOp)) {
    reduceInit = 0.0;
  } else if (mlir::isa<mlir::MulFOp, mlir::MulIOp>(reduceOp)) {
    reduceInit = 1.0;
  } else {
    return {};
  }
  return plier::getConstAttr(type, reduceInit);
}

struct ParallelToTbb : public mlir::OpRewritePattern<mlir::scf::ParallelOp> {
  using mlir::OpRewritePattern<mlir::scf::ParallelOp>::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::scf::ParallelOp op,
                  mlir::PatternRewriter &rewriter) const override {
    if (mlir::isa<plier::ParallelOp>(op->getParentOp())) {
      return mlir::failure();
    }
    bool need_parallel = op->hasAttr(plier::attributes::getParallelName()) ||
                         !op->getParentOfType<mlir::scf::ParallelOp>();
    if (!need_parallel) {
      return mlir::failure();
    }

    int64_t max_concurrency = 0;
    auto mod = op->getParentOfType<mlir::ModuleOp>();
    if (auto mc = mod->getAttrOfType<mlir::IntegerAttr>(
            plier::attributes::getMaxConcurrencyName())) {
      max_concurrency = mc.getInt();
    }

    if (max_concurrency <= 1) {
      return mlir::failure();
    }

    for (auto type : op.getResultTypes()) {
      if (!getReduceType(type, max_concurrency)) {
        return mlir::failure();
      }
    }

    llvm::SmallVector<mlir::Attribute> initVals;
    initVals.reserve(op.getNumResults());
    for (auto &nestedOp : op.getLoopBody().front().without_terminator()) {
      if (auto reduce = mlir::dyn_cast<mlir::scf::ReduceOp>(nestedOp)) {
        auto ind = static_cast<unsigned>(initVals.size());
        if (ind >= op.getNumResults()) {
          return mlir::failure();
        }
        auto &region = reduce.reductionOperator();
        if (!llvm::hasSingleElement(region)) {
          return mlir::failure();
        }
        auto reduceInitVal =
            getReduceInitVal(op.getResult(ind).getType(), region.front());
        if (!reduceInitVal) {
          return mlir::failure();
        }
        initVals.emplace_back(reduceInitVal);
      }
    }
    if (initVals.size() != op.getNumResults()) {
      return mlir::failure();
    }

    plier::AllocaInsertionPoint allocaIP(op);

    auto loc = op.getLoc();
    mlir::BlockAndValueMapping mapping;
    llvm::SmallVector<mlir::Value> reduce_vars(op.getNumResults());
    for (auto it : llvm::enumerate(op.getResultTypes())) {
      auto type = it.value();
      auto reduce_type = getReduceType(type, max_concurrency);
      assert(reduce_type);
      auto reduce = allocaIP.insert(rewriter, [&]() {
        return rewriter.create<mlir::memref::AllocaOp>(loc, reduce_type);
      });
      auto index = static_cast<unsigned>(it.index());
      reduce_vars[index] = reduce;
    }

    auto reduce_init_body_builder = [&](mlir::OpBuilder &builder,
                                        mlir::Location loc, mlir::Value index,
                                        mlir::ValueRange args) {
      assert(args.empty());
      (void)args;
      for (auto it : llvm::enumerate(reduce_vars)) {
        auto reduce = it.value();
        auto initVal = initVals[it.index()];
        auto init = builder.create<mlir::ConstantOp>(loc, initVal);
        builder.create<mlir::memref::StoreOp>(loc, init, reduce, index);
      }
      builder.create<mlir::scf::YieldOp>(loc);
    };

    auto reduce_lower_bound = rewriter.create<mlir::ConstantIndexOp>(loc, 0);
    auto reduce_upper_bound =
        rewriter.create<mlir::ConstantIndexOp>(loc, max_concurrency);
    auto reduce_step = rewriter.create<mlir::ConstantIndexOp>(loc, 1);
    rewriter.create<mlir::scf::ForOp>(loc, reduce_lower_bound,
                                      reduce_upper_bound, reduce_step,
                                      llvm::None, reduce_init_body_builder);

    auto &old_body = op.getLoopBody().front();
    auto orig_lower_bound = op.lowerBound();
    auto orig_upper_bound = op.upperBound();
    auto orig_step = op.step();
    auto body_builder = [&](mlir::OpBuilder &builder, ::mlir::Location loc,
                            mlir::ValueRange lower_bound,
                            mlir::ValueRange upper_bound,
                            mlir::Value thread_index) {
      llvm::SmallVector<mlir::Value> initVals(op.initVals().size());
      for (auto it : llvm::enumerate(op.initVals())) {
        auto reduce_var = reduce_vars[it.index()];
        auto val =
            builder.create<mlir::memref::LoadOp>(loc, reduce_var, thread_index);
        initVals[it.index()] = val;
      }
      auto new_op =
          mlir::cast<mlir::scf::ParallelOp>(builder.clone(*op, mapping));
      new_op->removeAttr(plier::attributes::getParallelName());
      assert(new_op->getNumResults() == reduce_vars.size());
      new_op.lowerBoundMutable().assign(lower_bound);
      new_op.upperBoundMutable().assign(upper_bound);
      new_op.initValsMutable().assign(initVals);
      for (auto it : llvm::enumerate(new_op->getResults())) {
        auto reduce_var = reduce_vars[it.index()];
        builder.create<mlir::memref::StoreOp>(loc, it.value(), reduce_var,
                                              thread_index);
      }
    };

    rewriter.create<plier::ParallelOp>(loc, orig_lower_bound, orig_upper_bound,
                                       orig_step, body_builder);

    auto reduce_body_builder = [&](mlir::OpBuilder &builder, mlir::Location loc,
                                   mlir::Value index, mlir::ValueRange args) {
      assert(args.size() == reduce_vars.size());
      mapping.clear();
      auto reduce_ops =
          llvm::make_filter_range(old_body.without_terminator(), [](auto &op) {
            return mlir::isa<mlir::scf::ReduceOp>(op);
          });
      llvm::SmallVector<mlir::Value> yield_args;
      yield_args.reserve(args.size());
      for (auto it : llvm::enumerate(reduce_ops)) {
        auto &reduce_var = reduce_vars[it.index()];
        auto arg = args[static_cast<unsigned>(it.index())];
        auto reduce_op = mlir::cast<mlir::scf::ReduceOp>(it.value());
        auto &reduce_op_body = reduce_op.reductionOperator().front();
        assert(reduce_op_body.getNumArguments() == 2);
        auto prev_val =
            builder.create<mlir::memref::LoadOp>(loc, reduce_var, index);
        mapping.map(reduce_op_body.getArgument(0), arg);
        mapping.map(reduce_op_body.getArgument(1), prev_val);
        for (auto &old_reduce_op : reduce_op_body.without_terminator()) {
          builder.clone(old_reduce_op, mapping);
        }
        auto result = mlir::cast<mlir::scf::ReduceReturnOp>(
                          reduce_op_body.getTerminator())
                          .result();
        result = mapping.lookupOrNull(result);
        assert(result);
        yield_args.emplace_back(result);
      }
      builder.create<mlir::scf::YieldOp>(loc, yield_args);
    };

    auto reduce_loop = rewriter.create<mlir::scf::ForOp>(
        loc, reduce_lower_bound, reduce_upper_bound, reduce_step, op.initVals(),
        reduce_body_builder);
    rewriter.replaceOp(op, reduce_loop.getResults());

    return mlir::success();
  }
};

struct ParallelToTbbPass
    : public plier::RewriteWrapperPass<
          ParallelToTbbPass, mlir::FuncOp,
          plier::DependentDialectsList<plier::PlierDialect,
                                       mlir::StandardOpsDialect,
                                       mlir::scf::SCFDialect>,
          ParallelToTbb> {};

void populate_parallel_to_tbb_pipeline(mlir::OpPassManager &pm) {
  pm.addNestedPass<mlir::FuncOp>(std::make_unique<ParallelToTbbPass>());
}
} // namespace

void registerParallelToTBBPipeline(plier::PipelineRegistry &registry) {
  registry.register_pipeline([](auto sink) {
    auto stage = getLowerLoweringStage();
    auto llvm_pipeline = lowerToLLVMPipelineName();
    sink(parallelToTBBPipelineName(), {stage.begin}, {llvm_pipeline}, {},
         &populate_parallel_to_tbb_pipeline);
  });
}

llvm::StringRef parallelToTBBPipelineName() { return "parallel_to_tbb"; }
