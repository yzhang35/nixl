/*
 * Copyright (c) 2026 BAAI. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "backend/backend_plugin.h"
#include "flagcx_backend.h"

namespace {
nixl_b_params_t
get_flagcx_options() {
    nixl_b_params_t params;
    params["in_python"] = "";
    params["num_cpus"] = "";
    return params;
}
} // namespace

// Plugin type alias for convenience
using flagcx_plugin_t = nixlBackendPluginCreator<nixlFlagcxEngine>;

#ifdef STATIC_PLUGIN_FLAGCX
nixlBackendPlugin *
createStaticFLAGCXPlugin() {
    return flagcx_plugin_t::create(
        NIXL_PLUGIN_API_VERSION, "FLAGCX", "0.1.0", get_flagcx_options(), {DRAM_SEG, VRAM_SEG});
}
#else
extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    return flagcx_plugin_t::create(
        NIXL_PLUGIN_API_VERSION, "FLAGCX", "0.1.0", get_flagcx_options(), {DRAM_SEG, VRAM_SEG});
}

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {}
#endif
