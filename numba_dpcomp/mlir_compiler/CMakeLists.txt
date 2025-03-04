# Copyright 2021 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

project(mlir_compiler LANGUAGES CXX C)

find_package(pybind11 REQUIRED)

find_package(LLVM REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)

list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)

set(SOURCES_LIST
    src/pipelines/base_pipeline.cpp
    src/pipelines/lower_to_llvm.cpp
    src/pipelines/parallel_to_tbb.cpp
    src/pipelines/plier_to_linalg.cpp
    src/pipelines/plier_to_std.cpp
    src/pipelines/pre_low_simplifications.cpp
    src/loop_utils.cpp
    src/lowering.cpp
    src/mangle.cpp
    src/py_func_resolver.cpp
    src/py_linalg_resolver.cpp
    src/py_map_types.cpp
    src/py_module.cpp
    )
set(HEADERS_LIST
    src/pipelines/base_pipeline.hpp
    src/pipelines/lower_to_llvm.hpp
    src/pipelines/parallel_to_tbb.hpp
    src/pipelines/plier_to_linalg.hpp
    src/pipelines/plier_to_std.hpp
    src/pipelines/pre_low_simplifications.hpp
    src/loop_utils.hpp
    src/lowering.hpp
    src/mangle.hpp
    src/py_func_resolver.hpp
    src/py_linalg_resolver.hpp
    src/py_map_types.hpp
    src/py_module.hpp
    )

pybind11_add_module(${PROJECT_NAME} ${SOURCES_LIST} ${HEADERS_LIST})

if (CMAKE_SYSTEM_NAME STREQUAL Linux)
    target_link_options(${PROJECT_NAME} PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/export.txt")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL Darwin)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/export_darwin.txt")
endif()

apply_llvm_compile_flags(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE
    plier
    LLVM${LLVM_NATIVE_ARCH}CodeGen
    LLVM${LLVM_NATIVE_ARCH}Desc
    LLVMTarget
    MLIRIR
    MLIRLLVMIR
    MLIRLLVMToLLVMIRTranslation
    MLIRTransforms
    MLIRStandardOpsTransforms
    MLIRLinalgTransforms
    MLIRLinalgToLLVM
    MLIRMathToLLVM
    MLIRSCFToStandard
    MLIRTensorTransforms
    )

target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    ${MLIR_INCLUDE_DIRS}
    PRIVATE
    ./src)

if(${DPNP_ENABLE})
    target_compile_definitions(${PROJECT_NAME} PRIVATE DPNP_ENABLE=1)
endif()
