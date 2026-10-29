# Beopylar custom status screen, built as a Zephyr module from this config repo.
#
# zephyr/module.yml points `cmake` at this file, so it is processed with the
# other Zephyr modules. Keeping the sources behind a Kconfig symbol means the
# repo still builds as a plain zmk-config for anyone without the option set.

if(CONFIG_BEOPYLAR_DISPLAY)
  zephyr_library()
  zephyr_library_sources(src/status_screen.c)
endif()
