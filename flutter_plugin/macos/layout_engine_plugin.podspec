#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint layout_engine_plugin.podspec` to validate before publishing.
#

# le_*() (see ../src/layout_engine_plugin.h) is implemented in
# backend/src/api/api.cpp, not in this plugin's own sources - link the
# backend's own CMake build output directly rather than recompiling it
# here. This link recipe is a manual mirror of backend/CMakeLists.txt's
# `api`/`render`/`io`/`skia` targets (there's no automated sharing between
# a CMakeLists.txt and a podspec) - keep the two in sync by hand; see this
# plugin's CLAUDE.md.
#
# Deliberately backend/build_release (Release), not backend/build (Debug)
# - this plugin is what an actual running Flutter app embeds, so it needs
# real optimized performance, not debug-build timings. backend/build
# (Debug) still has to exist too, for backend's own ctest/development
# workflow - both trees are expected to be kept up to date going forward,
# not just Release-on-demand for benchmarking. See this plugin's CLAUDE.md
# and backend/CLAUDE.md's Build section.
#
# CocoaPods loads this podspec through a `.symlinks/plugins/...` path, not
# this file's real location - File.realpath resolves that symlink first,
# or the relative walk below lands inside .symlinks instead of the repo.
backend_dir = File.expand_path('../../backend', File.realpath(__dir__))
backend_build = File.join(backend_dir, 'build_release')
skia_dir = ENV['SKIA_DIR'] || '/Volumes/Docking/Projects/synthosilicon/skia/skia'

unless File.exist?(File.join(backend_build, 'libapi.a'))
  raise "backend/build_release/libapi.a not found - build it first: " \
        "cmake -S #{backend_dir} -B #{backend_build} -DCMAKE_BUILD_TYPE=Release && " \
        "cmake --build #{backend_build} --target api render io -j"
end

# backend/ONETBB_INTEGRATION.md migration (Phase 5 cutover) - api.cpp now
# depends on oneTBB (find_package(TBB REQUIRED) in backend/CMakeLists.txt)
# and Tracy (FetchContent'd into build_release's own _deps/, since it has
# no Homebrew formula). libapi.a's own object code references both
# directly (MemoizingStage/flow::graph machinery, Tracy's ZoneScoped/etc.
# profiling macros), so without these two the link fails with undefined
# `tbb::detail::r1::*`/`tracy::*` symbols - found by trial (a
# `flutter run -d macos` linker failure), not anticipated in advance.
# libtbb.dylib is a non-keg-only Homebrew formula, symlinked directly into
# /opt/homebrew/lib (already in LIBRARY_SEARCH_PATHS below), so `-ltbb`
# resolves the same way `-lharfbuzz`/`-lspdlog`/etc. do; TracyClient has no
# Homebrew formula at all, so its full build path is needed instead - see
# backend/CMakeLists.txt's own Tracy FetchContent_Declare for why exactly
# this path (`_deps/tracy-build/`) is where CMake puts it.
tracy_lib = File.join(backend_build, '_deps/tracy-build/libTracyClient.a')
unless File.exist?(tracy_lib)
  raise "#{tracy_lib} not found - rebuild backend/build_release: " \
        "cmake -S #{backend_dir} -B #{backend_build} -DCMAKE_BUILD_TYPE=Release && " \
        "cmake --build #{backend_build} --target api render io -j"
end

# le_tcl.so/le_tcl_procs.tcl (LeTclBridge.mm's show_gui Tcl console -
# see TCL_EXPLORATION.md) come from backend's *Debug* tree, not
# build_release like everything else above: unlike api/render/io (hot
# render-path code), le_tcl only interprets a handful of typed commands
# per keystroke, so Debug-build overhead here is irrelevant, and this
# avoids needing le_tcl added to the Release tree at all. Same Tcl 8.6
# keg (TCL_TK_ROOT) backend/CMakeLists.txt's own le_tcl/le_shell targets
# already pin, for consistency - build/le_tcl.so must have been built
# against that same Tcl before this links.
backend_tcl_build = File.join(backend_dir, 'build')
tcl_root = ENV['TCL_TK_ROOT'] || '/opt/homebrew/opt/tcl-tk@8'
le_tcl_module_path = File.join(backend_tcl_build, 'le_tcl.so')
le_tcl_procs_path = File.join(backend_dir, 'src/tcl/le_tcl_procs.tcl')

unless File.exist?(le_tcl_module_path)
  raise "backend/build/le_tcl.so not found - build it first: " \
        "cmake -S #{backend_dir} -B #{backend_tcl_build} -DCMAKE_BUILD_TYPE=Debug && " \
        "cmake --build #{backend_tcl_build} --target le_tcl -j"
end

Pod::Spec.new do |s|
  s.name             = 'layout_engine_plugin'
  s.version          = '0.0.1'
  s.summary          = 'Flutter plugin binding the Layout Engine C API and rendering le_render_pixel_buffer as a platform Texture.'
  s.description      = <<-DESC
Flutter plugin binding the Layout Engine C API and rendering le_render_pixel_buffer as a platform Texture.
                       DESC
  s.homepage         = 'http://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }

  # This will ensure the source files in Classes/ are included in the native
  # builds of apps using this FFI plugin. Podspec does not support relative
  # paths, so Classes contains a forwarder C file that relatively imports
  # `../src/*` so that the C sources can be shared among all target platforms.
  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'

  # If your plugin requires a privacy manifest, for example if it collects user
  # data, update the PrivacyInfo.xcprivacy file to describe your plugin's
  # privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'layout_engine_plugin_privacy' => ['Resources/PrivacyInfo.xcprivacy']}

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.11'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'LIBRARY_SEARCH_PATHS' => "\"/opt/homebrew/lib\" \"/opt/homebrew/opt/icu4c/lib\"",
    'HEADER_SEARCH_PATHS' => "\"#{File.join(tcl_root, 'include/tcl-tk')}\"",
    'GCC_PREPROCESSOR_DEFINITIONS' => [
      "LE_TCL_MODULE_PATH='\"#{le_tcl_module_path}\"'",
      "LE_TCL_PROCS_PATH='\"#{le_tcl_procs_path}\"'",
    ].join(' '),
    'OTHER_LDFLAGS' => [
      # api.cpp's le_*() functions are never referenced by this plugin's
      # own compiled sources (Dart only finds them via dlsym at runtime) -
      # without -force_load, the linker drops libapi.a's object file
      # entirely since nothing in this link appears to need it.
      "-Wl,-force_load,#{File.join(backend_build, 'libapi.a')}",
      File.join(backend_build, 'libpipelines.a'),
      File.join(backend_build, 'libio.a'),
      File.join(backend_dir, 'src/lefdef/lef/lib/liblef.a'),
      File.join(backend_dir, 'src/lefdef/def/lib/libdef.a'),
      File.join(skia_dir, 'out/MacStatic/libskia.a'),
      tracy_lib,
      '-lharfbuzz', '-licuuc', '-licudata', '-ljpeg', '-lpng', '-lz', '-lwebp', '-lwebpdemux',
      '-lspdlog', '-lfmt', '-ltbb',
      # LeTclBridge.mm's embedded Tcl interpreter (show_gui console) -
      # dynamically `load`s le_tcl.so itself at runtime (see
      # le_tcl_module_path above), so only libtcl itself needs linking
      # here, not le_tcl.so.
      File.join(tcl_root, 'lib/libtcl8.6.dylib'),
      # Everything above is a C++ archive underneath (operator new/delete,
      # __cxa_*, __gxx_personality_v0 all get pulled in) - linking libc++
      # itself is handled by Classes/LeApiBridge.mm being an Objective-C++
      # source, which makes Xcode select the C++ linker driver for this
      # target, not by a flag here: neither '-lc++' nor an explicit
      # libc++.tbd path survives CocoaPods' OTHER_LDFLAGS merging
      # (confirmed by trial - both are silently dropped, no error), so
      # this has to be solved by the target having a real C++/ObjC++
      # source instead.
      '-framework CoreText', '-framework CoreFoundation', '-framework CoreGraphics', '-framework CoreServices',
      '-Wl,-rpath,/opt/homebrew/lib', '-Wl,-rpath,/opt/homebrew/opt/icu4c/lib',
    ].join(' '),
  }
  s.swift_version = '5.0'
end
