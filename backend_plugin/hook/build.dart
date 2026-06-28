import 'package:code_assets/code_assets.dart'; // Bundles binary packaging layout
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    final packageName = input.packageName;

    // From the package root, go up one level to the project root, then
    // navigate to the backend build output directory.
    final assetPathInPackage = input.packageRoot.resolve(
      '../backend/build/liblayout_engine.dylib',
    );

    // Add the dynamic library directly to the code assets list collection
    output.assets.code.add(
      CodeAsset(
        package: packageName,
        name:
            'layout_engine.dart', // Must match ffi.DynamicLibrary.open('layout_engine.dart')
        linkMode: DynamicLoadingBundled(), // Native Assets link enum
        file: assetPathInPackage,
      ),
    );
  });
}
