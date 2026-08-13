project('winepipewire-hud', 'cpp',
  version : '0.1.0',
  license : 'MIT',
  meson_version : '>= 0.59.0',
  default_options : [
    'cpp_std=c++17',
    'buildtype=release',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
)

fs = import('fs')
cpp = meson.get_compiler('cpp')

# Vulkan structs are initialised with their sType and then filled in by field,
# which -Wextra reads as a partial aggregate initialiser on every one of them.
add_project_arguments(cpp.get_supported_arguments('-Wno-missing-field-initializers'),
                      language : 'cpp')

# Empty means the compiler's own include path, which is right for a host build.
# Proton builds against its vendored Vulkan-Headers, and every architecture uses
# the same headers, so this is the only knob a second architecture needs.
vulkan_include_dir = get_option('vulkan_include_dir')
if vulkan_include_dir == ''
  inc_vulkan = []
else
  inc_vulkan = include_directories(vulkan_include_dir)
endif

if not cpp.has_header('vulkan/vk_layer.h', include_directories : inc_vulkan)
  error('vulkan/vk_layer.h not found, set -Dvulkan_include_dir=<Vulkan-Headers>/include')
endif

# The loader warns when an application asks for a newer Vulkan version than the
# manifest declares, so this tracks the headers being built against rather than
# a hardcoded version that would go stale and warn in every game's log.
vk_api_minor = '0'
foreach minor : ['1', '2', '3', '4']
  if cpp.has_header_symbol('vulkan/vulkan_core.h', 'VK_API_VERSION_1_' + minor,
                           include_directories : inc_vulkan)
    vk_api_minor = minor
  endif
endforeach
layer_api_version = '1.' + vk_api_minor + '.' + cpp.get_define('VK_HEADER_VERSION',
  prefix : '#include <vulkan/vulkan_core.h>', include_directories : inc_vulkan)

# The snapshot ABI belongs to the driver tree, so its header is included by path
# and never copied here. include_directories() refuses paths outside the project,
# hence a plain -I. Default assumes this project sits beside the wine checkout.
wine_driver_include_dir = get_option('wine_driver_include_dir')
if not fs.is_absolute(wine_driver_include_dir)
  wine_driver_include_dir = meson.current_source_dir() / wine_driver_include_dir
endif
hud_header_args = ['-I' + wine_driver_include_dir]

if not cpp.has_header('winepipewire_hud.h', args : hud_header_args)
  error('winepipewire_hud.h not found under ' + wine_driver_include_dir +
        ', set -Dwine_driver_include_dir=<wine>/dlls/winepipewire.drv')
endif

# Free of Vulkan on purpose: the tests link the reader without a loader or a GPU.
snapshot_lib = static_library('hud_snapshot',
  files(
    'src/log.cpp',
    'src/snapshot.cpp',
  ),
  cpp_args : hud_header_args,
  gnu_symbol_visibility : 'hidden',
  pic : true,
)

# Dear ImGui 1.91.6, core only, vendored under third_party/imgui.  The Vulkan
# backend is deliberately absent: it uploads its font atlas by submitting and
# wait-idling on an application-owned queue, which is unusable from inside an
# intercepted present, and it is single-swapchain by construction.
#
# Statically linked with hidden visibility so an application that uses ImGui
# itself cannot collide with this copy.  IMGUI_USER_CONFIG keeps the
# configuration out of tree, so the vendored copy stays byte-identical.
inc_imgui = include_directories('third_party/imgui', 'src')
imgui_args = ['-DIMGUI_USER_CONFIG="hud_imconfig.h"']

imgui_lib = static_library('imgui',
  files(
    'third_party/imgui/imgui.cpp',
    'third_party/imgui/imgui_draw.cpp',
    'third_party/imgui/imgui_tables.cpp',
    'third_party/imgui/imgui_widgets.cpp',
  ),
  cpp_args : imgui_args,
  include_directories : inc_imgui,
  gnu_symbol_visibility : 'hidden',
  override_options : ['warning_level=0'],
  pic : true,
)

# SPIR-V is generated at build time rather than checked in, so the shader source
# stays the thing under review.
# native: it runs on the build machine, so a cross build must not look for a
# host-architecture copy of it.
prog_glslang = find_program('glslangValidator', 'glslang', native : true)
overlay_spv = []
foreach shader : ['overlay.vert', 'overlay.frag']
  overlay_spv += custom_target(shader + '.spv.h',
    input : files('src' / shader),
    output : shader + '.spv.h',
    command : [prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@'],
  )
endforeach

# One basename on every architecture, deliberately. The manifest names the
# library bare, so the dynamic linker resolves it out of whichever per-arch
# directory on LD_LIBRARY_PATH holds the matching ELF class, and one manifest
# serves i386, x86_64 and aarch64. A per-arch basename would defeat that.
layer_manifest = 'VkLayer_winepipewire_hud.json'

layer_so = shared_library('VkLayer_winepipewire_hud',
  files(
    'src/frame.cpp',
    'src/layer.cpp',
    'src/render.cpp',
    'src/swapchain.cpp',
  ),
  overlay_spv,
  cpp_args : ['-DVK_NO_PROTOTYPES'] + hud_header_args + imgui_args,
  link_with : [snapshot_lib, imgui_lib],
  include_directories : [inc_vulkan, inc_imgui],
  gnu_symbol_visibility : 'hidden',
  link_depends : files('src/layer.map'),
  link_args : cpp.get_supported_link_arguments([
    '-Wl,-Bsymbolic-functions',
    '-Wl,-z,relro',
    '-Wl,--no-undefined',
    '-Wl,--version-script=' + meson.current_source_dir() / 'src/layer.map',
  ]),
  install : true,
)

configure_file(
  input : layer_manifest + '.in',
  output : layer_manifest,
  configuration : {
    'library_path' : fs.name(layer_so.full_path()),
    'api_version' : layer_api_version,
  },
  install : true,
  install_dir : get_option('datadir') / 'vulkan' / 'implicit_layer.d',
)

# No Vulkan and no GPU: the reader is exercised directly against a controlled
# publisher, which is the only way to drive the torn and stale cases on demand.
snapshot_probe = executable('snapshot_probe',
  files('tests/snapshot_probe.cpp'),
  cpp_args : hud_header_args,
  link_with : snapshot_lib,
  build_by_default : false,
)

test('snapshot-probe', snapshot_probe,
  env : { 'WINEPIPEWIRE_HUD_OVERLAY' : '1', 'WINEPIPEWIRE_HUD_OVERLAY_LOG' : '1' },
)

# The harness links the loader, which then loads the layer under test out of
# this build directory: the bare library_path in the manifest resolves through
# LD_LIBRARY_PATH exactly as it does in the shipped tool.
vulkan_loader = dependency('vulkan', required : false)

if vulkan_loader.found()
  present_smoke = executable('present_smoke',
    files('tests/present_smoke.cpp'),
    cpp_args : hud_header_args,
    dependencies : vulkan_loader,
    link_with : snapshot_lib,
    build_by_default : false,
  )

  layer_env = {
    'VK_IMPLICIT_LAYER_PATH' : meson.current_build_dir(),
    'LD_LIBRARY_PATH' : meson.current_build_dir(),
    'WINEPIPEWIRE_HUD_OVERLAY' : '1',
  }

  test('present-smoke', present_smoke,
    args : ['8'],
    depends : layer_so,
    env : layer_env + { 'WINEPIPEWIRE_HUD_OVERLAY_LOG' : '3' },
  )

  # One process publishing and presenting, which is how a game is arranged, so
  # the layer discovers the snapshot by its own pid with no override.
  test('present-smoke-snapshot', present_smoke,
    args : ['0', '2'],
    depends : layer_so,
    env : layer_env + { 'WINEPIPEWIRE_HUD_OVERLAY_LOG' : '2' },
    timeout : 60,
  )

  # The widest bed the ABI can carry and a truncated endpoint, in the view that
  # draws the most, because that is where the layer's 65535 vertices per draw list
  # would be reached first.  Level 2 logs the measured count.
  test('present-smoke-verbose', present_smoke,
    args : ['0', '2'],
    depends : layer_so,
    env : layer_env + {
      'WINEPIPEWIRE_HUD_OVERLAY_LOG' : '2',
      'WINEPIPEWIRE_HUD_OVERLAY_VIEW' : '2',
      'WINEPIPEWIRE_HUD_SMOKE_BEDMASK' : '0x3ffff',
      'WINEPIPEWIRE_HUD_SMOKE_OUT' : '9',
      'WINEPIPEWIRE_HUD_SMOKE_TAIL' : '0',
    },
    timeout : 60,
  )

  # An enabled layer asked to draw nothing must leave the frame exactly as
  # untouched as a disabled one, so the off view is a contract and not an absence
  # of one.
  test('present-smoke-view-off', present_smoke,
    args : ['0', '1'],
    depends : layer_so,
    env : layer_env + {
      'WINEPIPEWIRE_HUD_OVERLAY_LOG' : '2',
      'WINEPIPEWIRE_HUD_OVERLAY_VIEW' : '0',
      'WINEPIPEWIRE_HUD_SMOKE_TAIL' : '0',
    },
    timeout : 60,
  )

  # An enabled manifest with the gate unset must leave the frame untouched, which
  # the harness asserts rather than merely observing.
  test('present-smoke-gate-off', present_smoke,
    args : ['8'],
    depends : layer_so,
    env : {
      'VK_IMPLICIT_LAYER_PATH' : meson.current_build_dir(),
      'LD_LIBRARY_PATH' : meson.current_build_dir(),
      'WINEPIPEWIRE_HUD_OVERLAY_LOG' : '3',
    },
  )
endif
