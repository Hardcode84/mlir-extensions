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

"""
Define lowering and related passes.
"""

from .passes import MlirDumpPlier, MlirBackend
from .settings import USE_MLIR

from numba.core.compiler_machinery import register_pass

from numba.core.lowering import Lower as orig_Lower
from numba.core.typed_passes import NoPythonBackend as orig_NoPythonBackend

# looks like that we don't need it but it is inherited from BaseLower too
# from numba.core.pylowering import PyLower as orig_PyLower

from .runtime import *
from .math_runtime import *

class mlir_lower(orig_Lower):
    def lower(self):
        if USE_MLIR:
            self.emit_environment_object()
            self.genlower = None
            self.lower_normal_function(self.fndesc)
            self.context.post_lowering(self.module, self.library)
        else:
            orig_Lower.lower(self)

    def lower_normal_function(self, fndesc):
        if USE_MLIR:
            mod_ir = self.metadata['mlir_blob']
            import llvmlite.binding as llvm
            mod = llvm.parse_bitcode(mod_ir)
            self.setup_function(fndesc)
            self.library.add_llvm_module(mod);
        else:
            orig_Lower.lower_normal_function(self, desc)

@register_pass(mutates_CFG=True, analysis_only=False)
class mlir_NoPythonBackend(orig_NoPythonBackend):
    def __init__(self):
        orig_NoPythonBackend.__init__(self)

    def run_pass(self, state):
        import numba.core.lowering
        numba.core.lowering.Lower = mlir_lower
        try:
            res = orig_NoPythonBackend.run_pass(self, state)
        finally:
            numba.core.lowering.Lower = orig_Lower
        return res
