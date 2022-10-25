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

#pragma once

#include <memory>

#include <llvm/ADT/Optional.h>

namespace mlir {
class AnalysisManager;
class Pass;
struct LogicalResult;
} // namespace mlir

namespace imex {
llvm::Optional<mlir::LogicalResult>
optimizeMemoryOps(mlir::AnalysisManager &am);

std::unique_ptr<mlir::Pass> createMemoryOptPass();
} // namespace imex
