project('DS4L', 'c',
  version : '0.1.0',
  license : ['LGPL-3.0-or-later', 'GPL-2.0-or-later'],  # userspace / driver/
  default_options : ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
inc = include_directories('include')

deps = [dependency('threads')]
cargs = [
  '-DDS4L_DRIVER_DIR="@0@"'.format(meson.current_source_dir() / 'driver'),
  '-DDS4L_TOOLS_DIR="@0@"'.format(meson.current_source_dir() / 'tools'),
]

udev_dep = dependency('libudev', required : false)
if udev_dep.found()
  deps += udev_dep
  cargs += ['-DDS4L_HAVE_UDEV=1']
  message('libudev found: hotplug monitoring enabled')
else
  message('libudev NOT found: falling back to periodic rescan')
endif

deps += dependency('opus')

pw_dep = dependency('libpipewire-0.3', required : false)
if pw_dep.found()
  deps += pw_dep
  cargs += ['-DDS4L_HAVE_PIPEWIRE=1']
  message('libpipewire found: per-controller virtual audio devices enabled')
else
  message('libpipewire NOT found: virtual audio devices disabled (control still works)')
endif

# Everything except main.c: shared by the daemon and the ds4l CLI.
ds4l_core_src = [
  'src/ds5_protocol.c',
  'src/ds5_audio.c',
  'src/device.c',
  'src/hidraw.c',
  'src/dkms.c',
  'src/pw_nodes.c',
  'src/manager.c',
  'src/ctl.c',
]

ds4l_core = static_library('ds4l_core', ds4l_core_src,
  include_directories : inc,
  dependencies : deps,
  c_args : cargs)

executable('ds4ld', 'src/main.c',
  include_directories : inc,
  link_with : ds4l_core,
  dependencies : deps,
  c_args : cargs,
  install : true)

executable('ds4l', 'cli/ds4l.c',
  include_directories : [inc, include_directories('.')],
  link_with : ds4l_core,
  dependencies : deps,
  c_args : cargs,
  install : true)

# Ship the DKMS driver tree + root helper next to the installed daemon so
# `ds4l dkms install` works from the install prefix too.
install_subdir('driver',
  install_dir : get_option('datadir') / 'ds4l',
  exclude_files : ['.gitignore'])
install_subdir('tools',
  install_dir : get_option('datadir') / 'ds4l')

# Optional systemd user service; install.sh (or the command in README) copies
# the configured unit into ~/.config/systemd/user and enables it.
conf = configuration_data()
conf.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(input : 'systemd/ds4l.service',
  output : 'ds4l.service',
  configuration : conf,
  install_dir : get_option('datadir') / 'ds4l' / 'systemd' / 'user')
