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

#include "plier/rewrites/loop_rewrites.hpp"
#include "plier/transforms/const_utils.hpp"

#include <mlir/Dialect/SCF/SCF.h>
#include <mlir/Dialect/StandardOps/IR/Ops.h>

namespace {
template <mlir::CmpIPredicate SrcPred, mlir::CmpIPredicate DstPred>
bool norm_impl2(mlir::CmpIPredicate &pred, mlir::Value index, mlir::Value &lhs,
                mlir::Value &rhs) {
  if (pred != SrcPred) {
    return false;
  }
  if (index != lhs) {
    std::swap(lhs, rhs);
    pred = DstPred;
  }
  return true;
}

template <mlir::CmpIPredicate SrcPred, mlir::CmpIPredicate DstPred>
bool norm_impl(mlir::CmpIPredicate &pred, mlir::Value index, mlir::Value &lhs,
               mlir::Value &rhs) {
  return norm_impl2<SrcPred, DstPred>(pred, index, lhs, rhs) ||
         norm_impl2<DstPred, SrcPred>(pred, index, lhs, rhs);
}

enum EBound {
  LowerBound,
  UpperBound,
};
template <mlir::CmpIPredicate Pred, EBound Bound, int64_t Value>
llvm::Optional<int64_t> handler_impl(mlir::CmpIPredicate pred, mlir::Value lhs,
                                     mlir::Value rhs, mlir::Value index,
                                     mlir::Value lowerBound,
                                     mlir::Value upperBound) {
  if (pred != Pred) {
    return {};
  }
  auto bound = (Bound == LowerBound ? lowerBound : upperBound);
  if (rhs == bound && lhs == index) {
    return Value;
  }
  return {};
}
} // namespace

mlir::LogicalResult plier::CmpLoopBoundsSimplify::matchAndRewrite(
    mlir::scf::ForOp op, mlir::PatternRewriter &rewriter) const {
  auto index_var = op.getLoopBody().front().getArgument(0);
  bool matched = false;
  for (auto user : llvm::make_early_inc_range(index_var.getUsers())) {
    auto cmp = mlir::dyn_cast<mlir::CmpIOp>(user);
    if (cmp) {
      auto pred = cmp.predicate();
      auto lhs = cmp.lhs();
      auto rhs = cmp.rhs();
      // Normalize index and predicate (index always on the left)
      using norm_fptr_t =
          bool (*)(mlir::CmpIPredicate & pred, mlir::Value index,
                   mlir::Value & lhs, mlir::Value & rhs);
      using Predicate = mlir::CmpIPredicate;
      const norm_fptr_t norm_handlers[] = {
          &norm_impl<Predicate::sle, Predicate::sge>,
          &norm_impl<Predicate::slt, Predicate::sgt>,
          &norm_impl<Predicate::ule, Predicate::uge>,
          &norm_impl<Predicate::ult, Predicate::ugt>,
          &norm_impl<Predicate::eq, Predicate::eq>,
          &norm_impl<Predicate::ne, Predicate::ne>,
      };

      for (auto h : norm_handlers) {
        if (h(pred, index_var, lhs, rhs)) {
          break;
        }
      }

      using fptr_t = llvm::Optional<int64_t> (*)(
          Predicate pred, mlir::Value lhs, mlir::Value rhs, mlir::Value index,
          mlir::Value lowerBound, mlir::Value upperBound);
      const fptr_t handlers[] = {
          &handler_impl<Predicate::sge, UpperBound, 0>,
          &handler_impl<Predicate::slt, LowerBound, 0>,
          &handler_impl<Predicate::sge, LowerBound, 1>,
          &handler_impl<Predicate::slt, UpperBound, 1>,
      };

      for (auto h : handlers) {
        if (auto c = h(pred, lhs, rhs, index_var, op.lowerBound(),
                       op.upperBound())) {
          auto type = rewriter.getI1Type();
          auto val = rewriter.getIntegerAttr(type, *c);
          auto const_val = rewriter.create<mlir::ConstantOp>(cmp.getLoc(), val);
          rewriter.replaceOp(cmp, const_val.getResult());
          matched = true;
          break;
        }
      }
    }
  }
  return mlir::success(matched);
}
