project(
  'pi-radiogadget',
  'c',
  meson_version : '>= 1.0',
  version : '0.4.0',
  default_options : ['c_std=gnu11', 'warning_level=2', 'optimization=2', 'debug=true'],
)

thread_dep = dependency('threads')

# Everything except main() lives in a static library so tests link exactly what the daemon runs.
core = static_library(
  'picore',
  'src/log.c',
  'src/pktq.c',
  'src/ffs.c',
  'src/acl_split.c',
  'src/hci.c',
  'src/sco.c',
  'src/bdc.c',
  'src/fw.c',
  'src/wlan.c',
  'src/backend_fake.c',
  'src/wpa_parse.c',
  'src/tap.c',
  'src/backend_sup.c',
  'src/sha256.c',
  'src/update.c',
  include_directories : include_directories('src'),
  dependencies : thread_dep,
)

core_dep = declare_dependency(
  link_with : core,
  include_directories : include_directories('src'),
  dependencies : thread_dep,
)

exe = executable(
  'pi-radiogadget',
  'src/main.c',
  dependencies : core_dep,
  install : true,
)

# Host-side tool: pushes a new daemon binary to the Pi over USB (see docs/ARCHITECTURE.md).
executable(
  'pi-radiogadget-flash',
  'tools/flash.c',
  'src/sha256.c',
  include_directories : include_directories('src'),
  install : true,
)

subdir('tests')
