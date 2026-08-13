/*
 * Compile-time configuration for the vendored ImGui, passed as
 * IMGUI_USER_CONFIG so that the vendored copy itself stays byte-identical to
 * the 1.91.6 release.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINEPIPEWIRE_HUD_IMCONFIG_H
#define WINEPIPEWIRE_HUD_IMCONFIG_H

/* A layer has no business writing imgui.ini or a log file into a game's working
 * directory, and removing the functions is stronger than clearing IniFilename. */
#define IMGUI_DISABLE_FILE_FUNCTIONS
#define IMGUI_DISABLE_DEMO_WINDOWS

/* Proton configures every meson package with --buildtype=plain, which defines no
 * NDEBUG, so ImGui's IM_ASSERT would expand to assert() and a failed assertion
 * would abort the game.  A layer has no right to do that: log and carry on with
 * a frame that may be wrong instead of taking the process down. */
void hud_imgui_assert(const char *expr, const char *file, int line);
#define IM_ASSERT(expr) ((void)(!!(expr) || (hud_imgui_assert(#expr, __FILE__, __LINE__), 0)))

/* Each swapchain owns a context and is driven from whichever thread presents it,
 * so the current-context pointer must not be shared between those threads.  The
 * variable is defined in render.cpp; imgui.cpp only defines its own when GImGui
 * is not already a macro. */
struct ImGuiContext;
extern thread_local ImGuiContext *hud_imgui_context;
#define GImGui hud_imgui_context

#endif /* WINEPIPEWIRE_HUD_IMCONFIG_H */
