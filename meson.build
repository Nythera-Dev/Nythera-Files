project('nythera-files', ['c', 'cpp'],
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

cc  = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

# Clang requires an opt-in flag to properly use experimental C++ libraries.
# GCC/libstdc++ doesn't need this since these libraries are unconditionally
# exposed.
is_libcxx = cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>' ) != ''
if is_libcxx
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

sanitize = get_option('b_sanitize') != 'none'

if get_option('buildtype') != 'release'
  if is_libcxx
    add_project_arguments('-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE', language: 'cpp')
  else
    add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
  endif
endif

tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (tests_opt.auto() and get_option('buildtype') in ['debug', 'debugoptimized'] and not sanitize)

# ── Development build optimizations (LLD, split/compressed DWARF) ────────────
if get_option('buildtype') != 'release'
  _user_set_linker = false
  foreach arg : get_option('c_link_args') + get_option('cpp_link_args')
    if arg.startswith('-fuse-ld=')
      _user_set_linker = true
    endif
  endforeach

  _dev_linker_args = []
  if not _user_set_linker and not (cpp.get_id() == 'gcc' and get_option('b_lto'))
    if cpp.has_link_argument('-fuse-ld=lld') and cc.has_link_argument('-fuse-ld=lld')
      _dev_linker_args = ['-fuse-ld=lld']
      add_project_link_arguments('-fuse-ld=lld', language: ['c', 'cpp'])
    endif
  endif

  if get_option('debug')
    _dev_debug_flags = ['-gsplit-dwarf', '-gz=zstd']
    add_project_arguments(cc.get_supported_arguments(_dev_debug_flags), language: 'c')
    add_project_arguments(cpp.get_supported_arguments(_dev_debug_flags), language: 'cpp')
    if cpp.has_multi_link_arguments(_dev_linker_args + ['-Wl,--compress-debug-sections=zstd'])
      add_project_link_arguments('-Wl,--compress-debug-sections=zstd', language: ['c', 'cpp'])
    endif
  endif
endif

# ── Release-only optimizations ────────────────────────────────────────────────
if get_option('buildtype') == 'release'
  add_project_arguments(
    '-ffunction-sections', '-fdata-sections',
    language: ['c', 'cpp'],
  )
  add_project_link_arguments(
    '-Wl,--gc-sections', '-Wl,--as-needed',
    language: ['c', 'cpp'],
  )
endif

if get_option('native_optimizations')
  add_project_arguments('-march=native', '-mtune=native', language: ['c', 'cpp'])
endif

# ── Dependencies ──────────────────────────────────────────────────────────────
nythera_lib_dep = dependency('nythera-lib', method: 'pkg-config',
  version: ['>=0.1.0', '<0.2.0'], static: false, allow_fallback: false)
wayland_client_dep  = dependency('wayland-client')
xkbcommon_dep       = dependency('xkbcommon')
glib_dep            = dependency('glib-2.0')
gobject_dep         = dependency('gobject-2.0')
gio_dep             = dependency('gio-2.0')
gio_unix_dep        = dependency('gio-unix-2.0')
polkit_gobject_dep  = dependency('polkit-gobject-1')

jemalloc_option = get_option('jemalloc')
is_glibc = cpp.get_define('__GLIBC__', prefix: '#include <features.h>') != ''
jemalloc_deps = []

if jemalloc_option.enabled() and not is_glibc
  error('jemalloc support requires glibc')
endif

if is_glibc
  jemalloc_dep = dependency('jemalloc', required: jemalloc_option)
  if jemalloc_dep.found()
    jemalloc_deps += [jemalloc_dep]
  endif
endif

_nythera_files_sources = files(
  'src/filemanager/file_manager.cpp',
  'src/filemanager/file_manager_settings.cpp',
  'src/filemanager/configuration_notice.cpp',
  'src/filemanager/file_list_header.cpp',
  'src/filemanager/directory_session.cpp',
  'src/filemanager/file_search_session.cpp',
  'src/filemanager/fzf_search.cpp',
  'src/filemanager/file_manager_search.cpp',
  'src/filemanager/search_thumbnails.cpp',
  'src/filemanager/recursive_file_discovery.cpp',
  'src/filemanager/browser_tab.cpp',
  'src/filemanager/tab_manager.cpp',
  'src/filemanager/tab_bar_controller.cpp',
  'src/filemanager/file_menu_controller.cpp',
  'src/filemanager/file_transfer_controller.cpp',
  'src/filemanager/places_sidebar.cpp',
  'src/filemanager/dialogs/progress_window.cpp',
  'src/filemanager/dialogs/modal_dialog_host.cpp',
  'src/filemanager/dialogs/checksum_dialog.cpp',
  'src/filemanager/dialogs/help_dialog.cpp',
  'src/filemanager/file_transfer_payload.cpp',
  'src/filemanager/properties_window.cpp',
  'src/filemanager/polkit_helper_launch.cpp',
  'src/filemanager/admin_protocol.cpp',
  'src/filemanager/admin_helper_session.cpp',
  'src/filemanager/admin_session_client.cpp',
  'src/filemanager/property_operation_client.cpp',
  'src/filemanager/places_model.cpp',
  'src/filemanager/mount_backends.cpp',
  'src/filemanager/wastebin_fallback.cpp',
  'src/filemanager/dialogs/trash_fallback_dialog.cpp',
  'src/filemanager/file_operation_manager.cpp',
  'src/filemanager/gio_poll_source.cpp',
)

git_program = find_program('git', required: false)
if git_program.found()
  _git_revision_h = vcs_tag(
    input: 'src/core/git_revision.h.in',
    output: 'nythera_git_revision.h',
    command: [git_program, 'describe', '--tags', '--always', '--dirty=-dirty', '--abbrev=12'],
    fallback: 'unknown',
  )
else
  _git_revision_config = configuration_data()
  _git_revision_config.set('VCS_TAG', 'unknown')
  _git_revision_h = configure_file(
    input: 'src/core/git_revision.h.in',
    output: 'nythera_git_revision.h',
    configuration: _git_revision_config,
  )
endif

if git_program.found()
  alias_target('generated-headers', [_git_revision_h])
endif

_nythera_inc = [
  include_directories('src'),
  include_directories('.'),
]

_nythera_files_dependencies = [
  nythera_lib_dep,
  wayland_client_dep,
  xkbcommon_dep,
  glib_dep,
  gobject_dep,
  gio_dep,
  gio_unix_dep,
  polkit_gobject_dep,
] + jemalloc_deps

_nythera_compile_definitions = [
  '-DNYTHERA_INSTALL_PREFIX="' + get_option('prefix') + '"',
  '-DNYTHERA_INSTALL_DATADIR="' + get_option('datadir') + '"',
  '-DNYTHERA_VERSION="' + meson.project_version() + '"',
]

_nythera_cpp_args = [
  '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow',
] + _nythera_compile_definitions

file_operation_engine_deps = [gio_dep, dependency('threads')]
file_operation_engine_lib = static_library('nythera_file_operation_engine',
  sources: files(
    'src/filemanager/file_operation_engine.cpp',
    'src/filemanager/checksum.cpp',
    'src/filemanager/folder_size.cpp',
    'src/filemanager/fs_compat.cpp',
    'src/filemanager/property_operations.cpp',
  ),
  dependencies: file_operation_engine_deps,
  include_directories: include_directories('src'),
  cpp_args: _nythera_cpp_args,
)
file_operation_engine_dep = declare_dependency(
  link_with: file_operation_engine_lib,
  dependencies: file_operation_engine_deps,
)

app_build_info = static_library('nythera_app_build_info',
  sources: files('src/core/build_info.cpp') + [_git_revision_h],
  include_directories: _nythera_inc,
  cpp_args: _nythera_cpp_args,
)

nythera_files_lib = static_library('nythera_files',
  sources: _nythera_files_sources,
  dependencies: _nythera_files_dependencies + [file_operation_engine_dep],
  include_directories: _nythera_inc,
  cpp_args: _nythera_cpp_args,
)

nythera_files_test_dep = declare_dependency(
  link_with: [app_build_info, nythera_files_lib],
  dependencies: _nythera_files_dependencies + [file_operation_engine_dep],
  include_directories: _nythera_inc,
  compile_args: _nythera_compile_definitions,
)

property_helper_path = get_option('prefix') / get_option('libexecdir') / 'nythera-properties-helper'
property_policy_dir = polkit_gobject_dep.get_variable(pkgconfig: 'policydir')
property_policy_path = property_policy_dir / 'dev.nythera.Files.modify-properties.policy'
property_paths = configuration_data()
property_paths.set_quoted('NYTHERA_PROPERTIES_HELPER_PATH', property_helper_path)
property_paths.set_quoted('NYTHERA_PROPERTIES_POLICY_PATH', property_policy_path)
configure_file(output: 'property_helper_paths.h', configuration: property_paths)
property_policy = configuration_data()
property_policy.set('HELPER_PATH', property_helper_path)
configure_file(input: 'data/polkit/dev.nythera.Files.modify-properties.policy.in',
  output: 'dev.nythera.Files.modify-properties.policy', configuration: property_policy,
  install_dir: property_policy_dir, install_tag: 'properties-helper')
executable('nythera-properties-helper',
  sources: files('src/filemanager/property_helper_main.cpp', 'src/filemanager/property_operations.cpp'),
  dependencies: gio_dep, include_directories: include_directories('src'),
  install: true, install_dir: get_option('libexecdir'), install_tag: 'properties-helper')

admin_helper_path = get_option('prefix') / get_option('libexecdir') / 'nythera-admin-helper'
admin_policy_path = property_policy_dir / 'dev.nythera.Files.admin-session.policy'
admin_paths = configuration_data()
admin_paths.set_quoted('NYTHERA_ADMIN_HELPER_PATH', admin_helper_path)
admin_paths.set_quoted('NYTHERA_ADMIN_POLICY_PATH', admin_policy_path)
configure_file(output: 'admin_helper_paths.h', configuration: admin_paths)
admin_policy = configuration_data()
admin_policy.set('HELPER_PATH', admin_helper_path)
configure_file(input: 'data/polkit/dev.nythera.Files.admin-session.policy.in',
  output: 'dev.nythera.Files.admin-session.policy', configuration: admin_policy,
  install_dir: property_policy_dir, install_tag: 'admin-helper')
executable('nythera-admin-helper',
  sources: files(
    'src/filemanager/admin_helper_main.cpp',
    'src/filemanager/admin_helper_session.cpp',
    'src/filemanager/admin_protocol.cpp',
  ),
  dependencies: file_operation_engine_dep, include_directories: include_directories('src'),
  cpp_args: _nythera_cpp_args,
  install: true, install_dir: get_option('libexecdir'), install_tag: 'admin-helper')

files_assets = []
files_asset_copy = import('fs')
subdir('files-assets')

executable('nythera-file-manager',
  sources: files('src/filemanager/main.cpp'),
  dependencies: _nythera_files_dependencies + [file_operation_engine_dep],
  include_directories: _nythera_inc,
  cpp_args: _nythera_cpp_args,
  link_with: [app_build_info, nythera_files_lib],
  link_depends: files_assets,
  install: true,
  install_tag: 'files',
)

if build_tests
  dbus_run_session = find_program('dbus-run-session', required: false)

  file_inline_rename_test_exe = executable('file_inline_rename_test',
    sources: files('tests/file_inline_rename_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_inline_rename', file_inline_rename_test_exe)

  file_manager_keyboard_audit_test_exe = executable('file_manager_keyboard_audit_test',
    sources: files('tests/file_manager_keyboard_audit_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_keyboard_audit', file_manager_keyboard_audit_test_exe)

  file_manager_search_test_exe = executable('file_manager_search_test',
    sources: files('tests/file_manager_search_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_search', file_manager_search_test_exe, timeout: 30)

  fzf_search_test_exe = executable('fzf_search_test',
    sources: files('tests/fzf_search_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('fzf_search', fzf_search_test_exe, timeout: 20)

  file_sort_test_exe = executable('file_sort_test',
    sources: files('tests/file_sort_test.cpp'), dependencies: [nythera_lib_dep, gio_dep],
    include_directories: include_directories('src'), override_options: ['b_ndebug=false'])
  test('file_sort', file_sort_test_exe, timeout: 10)

  checksum_test_exe = executable('checksum_test',
    sources: files('tests/checksum_test.cpp', 'src/filemanager/checksum.cpp'),
    dependencies: gio_dep, include_directories: include_directories('src'),
    override_options: ['b_ndebug=false'])
  test('checksum', checksum_test_exe, timeout: 15)

  directory_metadata_test_exe = executable('directory_metadata_test',
    sources: files('tests/directory_metadata_test.cpp'), dependencies: [nythera_lib_dep, gio_dep],
    include_directories: include_directories('src'), override_options: ['b_ndebug=false'])
  test('directory_metadata', directory_metadata_test_exe, timeout: 10)

  folder_size_test_exe = executable('folder_size_test',
    sources: files('tests/folder_size_test.cpp', 'src/filemanager/folder_size.cpp', 'src/filemanager/property_operations.cpp'),
    dependencies: gio_dep, include_directories: include_directories('src'),
    override_options: ['b_ndebug=false'])
  test('folder_size', folder_size_test_exe, timeout: 15)

  property_metadata_test_exe = executable('property_metadata_test',
    sources: files('tests/property_metadata_test.cpp'), dependencies: gio_dep,
    include_directories: include_directories('src'), override_options: ['b_ndebug=false'])
  test('property_metadata', property_metadata_test_exe, timeout: 10)

  property_operations_test_exe = executable('property_operations_test',
    sources: files('tests/property_operations_test.cpp', 'src/filemanager/property_operations.cpp',
      'src/filemanager/property_operation_client.cpp', 'src/filemanager/polkit_helper_launch.cpp'),
    dependencies: [gio_dep, gio_unix_dep, polkit_gobject_dep],
    include_directories: [include_directories('src'), include_directories('.')],
    override_options: ['b_ndebug=false'],
  )
  test('property_operations', property_operations_test_exe, timeout: 15)

  property_permissions_test_exe = executable('property_permissions_test',
    sources: files('tests/property_permissions_test.cpp'),
    dependencies: gio_dep,
    include_directories: include_directories('src'),
    override_options: ['b_ndebug=false'],
  )
  test('property_permissions', property_permissions_test_exe, timeout: 10)

  file_interaction_test_exe = executable('file_interaction_test',
    sources: files('tests/file_interaction_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_interaction', file_interaction_test_exe, timeout: 10)

  if dbus_run_session.found()
    configuration_notice_test_exe = executable('configuration_notice_test',
      sources: files('tests/configuration_notice_test.cpp'), dependencies: nythera_files_test_dep,
      override_options: ['b_ndebug=false'],
    )
    test('configuration_notice', dbus_run_session,
      args: ['--', configuration_notice_test_exe],
      env: {'NYTHERA_TEST_PRIVATE_DBUS': '1'},
      timeout: 15,
    )
  endif

  file_manager_settings_test_exe = executable('file_manager_settings_test',
    sources: files('tests/file_manager_settings_test.cpp'), dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_settings', file_manager_settings_test_exe, timeout: 10)

  file_manager_sort_test_exe = executable('file_manager_sort_test',
    sources: files('tests/file_manager_sort_test.cpp'), dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'])
  test('file_manager_sort', file_manager_sort_test_exe, timeout: 15)

  property_scan_presentation_test_exe = executable('property_scan_presentation_test',
    sources: files('tests/property_scan_presentation_test.cpp'),
    dependencies: gio_dep, include_directories: include_directories('src'), override_options: ['b_ndebug=false'])
  test('property_scan_presentation', property_scan_presentation_test_exe, timeout: 10)

  file_columns_test_exe = executable('file_columns_test',
    sources: files('tests/file_columns_test.cpp'),
    dependencies: nythera_lib_dep,
    include_directories: include_directories('src'), override_options: ['b_ndebug=false'])
  test('file_columns', file_columns_test_exe, timeout: 10)

  file_artwork_test_exe = executable('file_artwork_test',
    sources: files('tests/file_artwork_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_artwork', file_artwork_test_exe, timeout: 10)

  places_model_test_exe = executable('places_model_test',
    sources: files('tests/places_model_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('places_model', places_model_test_exe, timeout: 30)

  mount_backends_test_exe = executable('mount_backends_test',
    sources: files('tests/mount_backends_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('mount_backends', mount_backends_test_exe, timeout: 30)

  file_operation_manager_test_exe = executable('file_operation_manager_test',
    sources: files('tests/file_operation_manager_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  if dbus_run_session.found()
    test('file_operation_manager', find_program('sh'),
      args: [
        '-c',
        'rt=$(mktemp -d); chmod 700 "$rt"; trap "rm -rf \\"$rt\\"" EXIT; XDG_RUNTIME_DIR="$rt" "$1" -- "$2"',
        'sh', dbus_run_session.full_path(), file_operation_manager_test_exe.full_path(),
      ],
      depends: file_operation_manager_test_exe,
      timeout: 60,
    )
  else
    test('file_operation_manager', file_operation_manager_test_exe,
      env: {
        'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent/nythera-disabled-dbus',
        'GIO_USE_VFS': 'local',
        'GVFS_DISABLE_FUSE': '1',
        'NYTHERA_TEST_FORBID_PRODUCTION_WASTEBIN': '1',
      },
      timeout: 60,
    )
  endif

  file_transfer_pause_test_exe = executable('file_transfer_pause_test',
    sources: files('tests/file_transfer_pause_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  if dbus_run_session.found()
    test('file_transfer_pause', dbus_run_session,
      args: ['--', file_transfer_pause_test_exe.full_path()],
      depends: file_transfer_pause_test_exe,
      timeout: 30,
    )
  else
    test('file_transfer_pause', file_transfer_pause_test_exe,
      env: {
        'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent/nythera-disabled-dbus',
        'GIO_USE_VFS': 'local',
        'GVFS_DISABLE_FUSE': '1',
        'NYTHERA_TEST_FORBID_PRODUCTION_WASTEBIN': '1',
      },
      timeout: 30,
    )
  endif

  file_wastebin_operation_test_exe = executable('file_wastebin_operation_test',
    sources: files('tests/file_wastebin_operation_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  if dbus_run_session.found()
    test('file_wastebin_operation', dbus_run_session,
      args: ['--', file_wastebin_operation_test_exe.full_path()],
      depends: file_wastebin_operation_test_exe,
      timeout: 30,
    )
  else
    test('file_wastebin_operation', file_wastebin_operation_test_exe,
      env: {
        'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent/nythera-disabled-dbus',
        'GIO_USE_VFS': 'local',
        'GVFS_DISABLE_FUSE': '1',
        'NYTHERA_TEST_FORBID_PRODUCTION_WASTEBIN': '1',
      },
      timeout: 30,
    )
  endif

  file_manager_wastebin_test_exe = executable('file_manager_wastebin_test',
    sources: files('tests/file_manager_wastebin_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_wastebin', file_manager_wastebin_test_exe, timeout: 15)

  file_transfer_payload_test_exe = executable('file_transfer_payload_test',
    sources: files('tests/file_transfer_payload_test.cpp', 'src/filemanager/file_transfer_payload.cpp'),
    dependencies: gio_dep,
    include_directories: include_directories('src'),
    override_options: ['b_ndebug=false'],
  )
  test('file_transfer_payload', file_transfer_payload_test_exe, timeout: 10)

  transfer_rates_test_exe = executable('transfer_rates_test',
    sources: files('tests/transfer_rates_test.cpp'),
    include_directories: include_directories('src'),
    override_options: ['b_ndebug=false'],
  )
  test('transfer_rates', transfer_rates_test_exe, timeout: 10)

  file_manager_startup_test_exe = executable('file_manager_startup_test',
    sources: files('tests/file_manager_startup_test.cpp'),
    dependencies: gio_dep, include_directories: include_directories('src'),
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_startup', file_manager_startup_test_exe, timeout: 10)

  file_manager_paste_selection_test_exe = executable('file_manager_paste_selection_test',
    sources: files('tests/file_manager_paste_selection_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_paste_selection', file_manager_paste_selection_test_exe, timeout: 15)

  file_manager_drag_scroll_test_exe = executable('file_manager_drag_scroll_test',
    sources: files('tests/file_manager_drag_scroll_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_drag_scroll', file_manager_drag_scroll_test_exe, timeout: 15)

  file_manager_async_fs_test_exe = executable('file_manager_async_fs_test',
    sources: files('tests/file_manager_async_fs_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_async_fs', file_manager_async_fs_test_exe, timeout: 15)

  file_manager_conflict_dialog_test_exe = executable('file_manager_conflict_dialog_test',
    sources: files('tests/file_manager_conflict_dialog_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_conflict_dialog', file_manager_conflict_dialog_test_exe, timeout: 15)

  symlink_target_picker_test_exe = executable('symlink_target_picker_test',
    sources: files('tests/symlink_target_picker_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('symlink_target_picker', symlink_target_picker_test_exe, timeout: 15)

  file_manager_help_menu_test_exe = executable('file_manager_help_menu_test',
    sources: files('tests/file_manager_help_menu_test.cpp'),
    dependencies: nythera_files_test_dep,
    link_depends: files_assets,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_help_menu', file_manager_help_menu_test_exe, timeout: 15)

  file_manager_tabs_test_exe = executable('file_manager_tabs_test',
    sources: files('tests/file_manager_tabs_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('file_manager_tabs', file_manager_tabs_test_exe, timeout: 20)

  admin_session_stage1_test_exe = executable('admin_session_stage1_test',
    sources: files('tests/admin_session_stage1_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('admin_session_stage1', admin_session_stage1_test_exe, timeout: 20)

  admin_mode_stage2_test_exe = executable('admin_mode_stage2_test',
    sources: files('tests/admin_mode_stage2_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('admin_mode_stage2', admin_mode_stage2_test_exe, timeout: 25)

  admin_mode_stage3_test_exe = executable('admin_mode_stage3_test',
    sources: files('tests/admin_mode_stage3_test.cpp'),
    dependencies: nythera_files_test_dep,
    link_depends: files_assets,
    override_options: ['b_ndebug=false'],
  )
  test('admin_mode_stage3', admin_mode_stage3_test_exe, timeout: 25)

  permanent_delete_prompt_test_exe = executable('permanent_delete_prompt_test',
    sources: files('tests/permanent_delete_prompt_test.cpp'),
    dependencies: nythera_lib_dep,
    include_directories: include_directories('src'),
    override_options: ['b_ndebug=false'],
  )
  test('permanent_delete_prompt', permanent_delete_prompt_test_exe, timeout: 10)

  gio_poll_source_test_exe = executable('gio_poll_source_test',
    sources: files('tests/gio_poll_source_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('gio_poll_source', gio_poll_source_test_exe, timeout: 10)

  test_isolation_test_exe = executable('test_isolation_test',
    sources: files('tests/test_isolation_test.cpp'),
    dependencies: nythera_files_test_dep,
    override_options: ['b_ndebug=false'],
  )
  test('test_isolation', test_isolation_test_exe, timeout: 20)
endif
