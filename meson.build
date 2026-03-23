project(
  'slopsh',
  'c',
  meson_version : '>= 1.3.0',
  version : '0.1',
  default_options : ['warning_level=3'],
)

dependencies = [
]

sources = [
  'slopsh.c',
]

exe = executable(
  'slopsh',
  sources,
  dependencies : dependencies,
  install : true,
)

test('basic', exe)
