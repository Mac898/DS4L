project('DS4L', 'c',
  version : '0.1.0',
  license : ['LGPL-3.0-or-later', 'GPL-2.0-or-later'],  # userspace / driver/
  default_options : ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
inc = include_directories('include')

deps = [dependency('threads')]
# The DKMS helper is looked up in the installed share/ds4l first; the source
# tree is only the fallback for an uninstalled build.
cargs = [
  '-DDS4L_DATADIR="@0@"'.format(get_option('prefix') / get_option('datadir') / 'ds4l'),
  '-DDS4L_SRCDIR="@0@"'.format(meson.current_source_dir()),
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

# Everything the daemon needs besides main.c.
ds4l_core_src = [
  'src/ds5_protocol.c',
  'src/ds5_audio.c',
  'src/device.c',
  'src/hidraw.c',
  'src/dkms.c',
  'src/pw_nodes.c',
  'src/pw_route.c',
  'src/manager.c',
  'src/ctl.c',
]

# The socket client alone, so the CLI needs no PipeWire, udev or Opus.
ds4l_ctl_client = static_library('ds4l_ctl_client', 'src/ctl_client.c',
  include_directories : inc)

ds4l_core = static_library('ds4l_core', ds4l_core_src,
  include_directories : inc,
  link_with : ds4l_ctl_client,
  dependencies : deps,
  c_args : cargs)

executable('ds4ld', 'src/main.c',
  include_directories : inc,
  link_with : ds4l_core,
  dependencies : deps,
  c_args : cargs,
  install : true)

executable('ds4l', 'cli/ds4l.c',
  include_directories : include_directories('.'),
  link_with : ds4l_ctl_client,
  install : true)

# Ship the DKMS driver tree + root helper next to the installed daemon so
# `ds4l dkms install` works from the install prefix too.
install_subdir('driver',
  install_dir : get_option('datadir') / 'ds4l',
  exclude_files : ['.gitignore', 'ds4l-upstream.patch'])
install_subdir('tools',
  install_dir : get_option('datadir') / 'ds4l')

# systemd user service, in a directory systemd searches for the usual
# prefixes (~/.local, /usr/local, /usr). install.sh enables it.
conf = configuration_data()
conf.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(input : 'systemd/ds4l.service',
  output : 'ds4l.service',
  configuration : conf,
  install_dir : get_option('datadir') / 'systemd' / 'user')
