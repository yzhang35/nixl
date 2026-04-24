# Copyright (c) 2026 BAAI. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cpp = meson.get_compiler('cpp')
compile_flags = []


if 'FLAGCX' in static_plugins
    flagcx_backend_lib = static_library('FLAGCX',
               'flagcx_backend.cpp', 'flagcx_backend.h', 'flagcx_plugin.cpp',
               dependencies: [nixl_infra, serdes_interface, cuda_dep, libflagcx_p2p, nixl_common_dep],
               include_directories: nixl_inc_dirs,
               install: false,
               cpp_args : compile_flags,
               name_prefix: 'libplugin_')  # Custom prefix for plugin libraries
else
    flagcx_backend_lib = shared_library('FLAGCX',
               'flagcx_backend.cpp', 'flagcx_backend.h', 'flagcx_plugin.cpp',
               dependencies: [nixl_infra, serdes_interface, cuda_dep, libflagcx_p2p, nixl_common_dep],
               include_directories: [nixl_inc_dirs, utils_inc_dirs],
               install: true,
               cpp_args : compile_flags + ['-fPIC'],
               name_prefix: 'libplugin_',  # Custom prefix for plugin libraries
               install_dir: plugin_install_dir)

    if get_option('buildtype') == 'debug'
        run_command('sh', '-c',
                    'echo "FLAGCX=' + flagcx_backend_lib.full_path() + '" >> ' + plugin_build_dir + '/pluginlist',
                    check: true
                )
    endif
endif

flagcx_backend_interface = declare_dependency(link_with: flagcx_backend_lib)
