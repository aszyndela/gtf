project('gtf', 'cpp', default_options : 'cpp_std=c++17')
deps = [dependency('gio-2.0'),
	    dependency('gio-unix-2.0'),
		dependency('glib-2.0'),
		dependency('dbus-1'),
		dependency('threads')]
executable('gtf', 'main.cpp', dependencies : deps)
