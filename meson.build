project('evdi-hello', 'cpp',
  version : '0.1',
  default_options : ['warning_level=3', 'cpp_std=c++2a'])

cpp = meson.get_compiler('cpp')

libevdi = cpp.find_library('evdi')

add_project_arguments('-fno-strict-aliasing',
language:'cpp')

executable('evdi-hello',
	['src/evdi.cpp', 'src/edid.cpp'],
	dependencies: [libevdi, dependency('threads')],
  install : true)
