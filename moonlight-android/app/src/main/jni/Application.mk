# Application.mk for Moonlight

# Our minimum version is Android 9 (API 28, since 2026-11-04)
APP_PLATFORM := android-28

# We support 16KB pages
APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true
APP_STL := c++_static
APP_CPPFLAGS := -std=c++17 -fexceptions
