Third-party code in the winepipewire-hud Vulkan layer
=====================================================

The layer binary contains code from the projects below.  Their notices live in
the source files that carry the code, and this file exists so the same
information ships beside the tool, where the sources do not.

Dear ImGui
----------

Vendored at version 1.91.6 under third_party/imgui, core only: imgui.cpp,
imgui_draw.cpp, imgui_tables.cpp and imgui_widgets.cpp.  No backend is built.
MIT licensed, full text in LICENSE.IMGUI beside this tool and in
third_party/imgui/LICENSE.txt in the source tree.

    Copyright (c) 2014-2024 Omar Cornut

Visit Dear ImGui at

    https://github.com/ocornut/imgui

Mesa VK_LAYER_MESA_overlay and MangoHud
---------------------------------------

The ImGui-to-Vulkan draw path is derived from MangoHud's src/vulkan.cpp, which
is itself a fork of Mesa's overlay layer.  The functions and shaders taken from
it are listed per file in src/render.hpp, src/render.cpp, src/swapchain.cpp,
src/overlay.vert and src/overlay.frag, each of which carries the full notice.
Both copyright holders, under the MIT licence:

    Copyright (c) 2019 Intel Corporation
    Copyright (c) 2020 flightlessmango and MangoHud contributors

Visit them at

    https://gitlab.freedesktop.org/mesa/mesa
    https://github.com/flightlessmango/MangoHud
