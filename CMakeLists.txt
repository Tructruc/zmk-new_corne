# Beopylar: custom status screen for the nice!view panels.
#
# The config repo is loaded by the build as a Zephyr module (see zephyr/module.yml),
# so this file is what actually adds the screen to the firmware.

if(CONFIG_BEOPYLAR_DISPLAY)
  zephyr_library()
  zephyr_library_sources(src/status_screen.c)

  # ZMK keeps its headers in <zmk app>/include, but exposes that path to the app
  # target only (`target_include_directories(app PRIVATE include)` in
  # app/CMakeLists.txt), so a module has to add it itself or `zmk/display.h` and
  # `zmk/hid.h` are not found. During a Zephyr build the application (i.e. ZMK's
  # app directory) is the CMake source directory.
  find_path(BEOPYLAR_ZMK_INCLUDE
    NAMES zmk/display.h
    PATHS ${CMAKE_SOURCE_DIR}/include
    NO_DEFAULT_PATH
  )
  if(NOT BEOPYLAR_ZMK_INCLUDE)
    message(FATAL_ERROR
      "Beopylar status screen: could not find ZMK's app/include "
      "(looked in ${CMAKE_SOURCE_DIR}/include).")
  endif()
  zephyr_include_directories(${BEOPYLAR_ZMK_INCLUDE})
endif()
