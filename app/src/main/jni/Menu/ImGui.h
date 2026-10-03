#pragma once
#include <EGL/egl.h>

// 0 Dark, 1 Light, 2 Teal, 3 MD3 Dark, 4 MD3 Light, 5 AMOLED, 6 Dracula, 7 Nord
constexpr int kThemeCount = 8;
void menuStyle(int theme = 0);
const char *themeName(int theme);

// This menu_addr is used to allow for multiple game support in the future
void *initModMenu(void *menu_addr, void *on_init_addr = nullptr, bool isJni = false);

void setupMenu();

void internalDrawMenu(int width, int height);

EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf);

int getGlWidth();
int getGlHeight();

void installOverlayHooks();
