project('shmem_connect', ['c', 'c++'])
#tools-update should be in PATH, will symlink the external directory to the current one (also move all formulas into one file, and just call the different functions as needed), download/build as needed (use .{} to check the status, remove regardless of whether it exists, and --force should force the deed regardless), then run meson
common_include = include_directories('external/asio/asio/include')
static_library('util', 'util.cpp', include_dir: common_include)

executable('shmem_server','Server.cpp', include_dir: common_include, link_with: util)

static_library('shmem_library', 'Library.cpp', include_dir: common_include, link_with: util)



