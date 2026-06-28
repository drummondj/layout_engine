#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint backend_plugin.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'backend_plugin'
  s.version          = '0.0.1'
  s.summary          = 'A new Flutter plugin project.'
  s.description      = <<-DESC
A new Flutter plugin project.
                       DESC
  s.homepage         = 'http://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }

  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'
  s.public_header_files = 'Classes/**/*.h' 

  # If your plugin requires a privacy manifest, for example if it collects user
  # data, update the PrivacyInfo.xcprivacy file to describe your plugin's
  # privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'backend_plugin_privacy' => ['Resources/PrivacyInfo.xcprivacy']}

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.11'
  s.pod_target_xcconfig = { 
    'DEFINES_MODULE' => 'YES',
    'LIBRARY_SEARCH_PATHS' => '"$(PROJECT_DIR)/../../../backend/build"',
    'OTHER_LDFLAGS' => '$(inherited) -llayout_engine -Wl,-dead_strip -Wl,-rpath,@loader_path/../../../../'
  }

  s.swift_version = '5.0'

  s.script_phase = {
    :name => 'Compile Shared Skia Core via CMake',
    :script => <<-SCRIPT
      cd "$PROJECT_DIR/../../../backend"
      mkdir -p build && cd build
      cmake .. -DCMAKE_BUILD_TYPE=Release
      make

      TARGET_FRAMEWORKS_DIR="$BUILT_PRODUCTS_DIR/$FRAMEWORKS_FOLDER_PATH"
      mkdir -p "$TARGET_FRAMEWORKS_DIR"
      cp -f "liblayout_engine.dylib" "$TARGET_FRAMEWORKS_DIR/liblayout_engine.dylib"
      
      echo "Successfully embedded liblayout_engine.dylib into $TARGET_FRAMEWORKS_DIR"
    SCRIPT
  }

  # Tell the macOS bundler to fetch the binary compiled out of the shared directory
  s.vendored_libraries = '../../../backend/build/liblayout_engine.dylib'

end
