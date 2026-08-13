Packaging the winepipewire-hud layer
====================================

Building it
-----------

From the Proton tree root, one command:

    make dist

The layer is on `all-dist`, so nothing extra is needed. To build only it:

    make winepipewire-hud

Both re-enter the SteamRT container, which is where the i386 and x86_64
toolchains and the Vulkan headers live.

Host builds without the container, for development, still work from this
directory and take about six seconds:

    meson setup build && ninja -C build && meson test -C build
    meson setup build32 --cross-file cross/i386-linux-gnu.ini && ninja -C build32

Those use the sibling `wine-cachyos` checkout for `winepipewire_hud.h`; the
container build uses the patched wine source instead, through
`-Dwine_driver_include_dir`.

What the artifact must contain
------------------------------

A build was once shipped incomplete here because stale stamps let `make` believe
work was done, and what caught it was comparing the tarball's file list against
what it should be. So: after a `TARGET_ARCH=x86_64` build, `$(DST_BASE)`, which
is `build/dist/`, must contain exactly these layer files.

| Path under `build/dist/` | Arch | Comes from |
| --- | --- | --- |
| `files/lib/x86_64-linux-gnu/libVkLayer_winepipewire_hud.so` | x86_64 | generic lib dist step |
| `files/lib/i386-linux-gnu/libVkLayer_winepipewire_hud.so` | i386 | generic lib dist step |
| `files/share/winepipewire-hud/implicit_layer.d/VkLayer_winepipewire_hud.json` | shared by both | `winepipewire-hud-x86_64-post-build` |
| `LICENSE.IMGUI` | n/a | `DIST_WINEPIPEWIRE_HUD_IMGUI_LICENSE` |
| `THIRDPARTY.WINEPIPEWIRE_HUD.md` | n/a | `DIST_WINEPIPEWIRE_HUD_THIRDPARTY` |

For a `TARGET_ARCH=arm64` build, read `aarch64-linux-gnu` for the library and
`winepipewire-hud-aarch64-post-build` for the manifest, and expect no i386 or
x86_64 copy.

With `ENABLE_WOW64=1` there is no `i386-unix` in `ARCHS`, so no 32-bit library is
built and none is expected: a 32-bit game then runs as a 32-bit PE inside a
64-bit unix process, and the layer it loads is the 64-bit one. `rules-meson`
skips the arch by itself, so nothing needs changing for that configuration.

Checking it
-----------

    find build/dist -name '*winepipewire_hud*' -o -name 'LICENSE.IMGUI' \
        -o -name 'THIRDPARTY.WINEPIPEWIRE_HUD.md' | sort

Five lines for an x86_64 build, four for arm64. Two failure modes this catches:
a missing `.so` for one arch, which means that arch's package did not build, and
a missing manifest, which means the post-build step did not run and the layer
would be invisible to the loader however well it built.

    grep library_path build/dist/files/share/winepipewire-hud/implicit_layer.d/VkLayer_winepipewire_hud.json

Must be the bare `libVkLayer_winepipewire_hud.so` with no directory. A path here
would break every architecture but the one it was generated for.

Turning it on
-------------

`WINEPIPEWIRE_HUD_OVERLAY=1` in the game's launch options. That adds the manifest
directory to `VK_IMPLICIT_LAYER_PATH`, satisfies the manifest's
`enable_environment`, satisfies the layer's own runtime gate, and sets
`WINEPIPEWIRE_HUD=1` so the driver publishes the snapshot the overlay reads.
`WINEPIPEWIRE_HUD_OVERLAY_LOG=2` adds a stderr line per second per swapchain
carrying the same values the overlay draws.

`WINEPIPEWIRE_HUD_OVERLAY_VIEW` chooses how much is drawn: `1`, the default, is the
compact panel, `2` adds provenance, the publish history and the fields that are
unavailable so that their unavailability is visible, and `0` draws nothing while
leaving the layer loaded. It is a separate variable from the gate on purpose: the
gate's value is read by the Proton script, by the loader's manifest
`enable_environment` and by the layer itself, and those three do not have to agree
about what a value other than `1` means.
