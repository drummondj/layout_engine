import Cocoa
import FlutterMacOS

public class BackendPlugin: NSObject, FlutterPlugin {
  private static var textures: FlutterTextureRegistry?
  private var activeTextures: [Int64: LayoutEngineTexture] = [:]

  public static func register(with registrar: FlutterPluginRegistrar) {
    let channel = FlutterMethodChannel(
      name: "com.synthosilicon.layout_engine/channel", binaryMessenger: registrar.messenger)

    BackendPlugin.textures = registrar.textures

    let instance = BackendPlugin()
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "getPlatformVersion":
      result("macOS " + ProcessInfo.processInfo.operatingSystemVersionString)
    case "createTexture":
      guard let registry = BackendPlugin.textures else {
        result(
          FlutterError(
            code: "NO_REGISTRY", message: "Flutter texture registry is missing", details: nil))
        return
      }

      guard let args = call.arguments as? [String: Any],
        // 1. Receive the handle as a number, which is how Dart sends it.
        let handleNumber = args["handle"] as? NSNumber,
        let width = args["width"] as? Int32,
        let height = args["height"] as? Int32
      else {
        result(FlutterError(code: "INVALID_ARGS", message: "Missing dimension keys", details: nil))
        return
      }

      // 2. Create the pointer from the integer address.
      let handle = UnsafeMutableRawPointer(bitPattern: handleNumber.intValue)

      // 3. Instantiate the Objective-C class using the correct Swift-translated initializer name.
      //    Use 'guard let' to safely unwrap the optional LayoutEngineTexture? instance.
      guard
        let myTex = LayoutEngineTexture(
          textureWith: registry,
          handle: handle,
          width: width,
          height: height)
      else {
        result(
          FlutterError(
            code: "ALLOC_FAILED", message: "Failed to allocate LayoutEngineTexture", details: nil))
        return
      }

      // FIX: Call .textureId() matching your exact Objective-C header signature
      let tId = myTex.textureId()
      activeTextures[tId] = myTex

      // 2. Kick off the steady 60Hz display callback loop
      let timer = Timer.scheduledTimer(
        timeInterval: 1.0 / 60.0, target: myTex,
        selector: #selector(LayoutEngineTexture.tickAnimationFrame), userInfo: nil, repeats: true)
      myTex.loopTimer = timer

      // 3. Return the registered layer token ID number straight to Dart
      result(tId)
    case "disposeTexture":
      guard let args = call.arguments as? [String: Any],
        let tId = args["textureId"] as? Int64
      else {
        result(nil)
        return
      }

      if let tex = activeTextures[tId] {
        tex.loopTimer?.invalidate()
        BackendPlugin.textures?.unregisterTexture(tId)
        activeTextures.removeValue(forKey: tId)
      }
      result(nil)

    case "resizeTexture":
      guard let args = call.arguments as? [String: Any],
        let tId = args["textureId"] as? Int64,
        let width = args["width"] as? Int32,
        let height = args["height"] as? Int32
      else {
        result(
          FlutterError(code: "INVALID_ARGS", message: "Missing arguments for resize", details: nil))
        return
      }

      if let tex = activeTextures[tId] {
        tex.resize(withWidth: width, height: height)
      }
      result(nil)

    default:
      result(FlutterMethodNotImplemented)
    }
  }
}
