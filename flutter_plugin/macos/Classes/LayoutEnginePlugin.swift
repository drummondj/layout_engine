import Cocoa
import FlutterMacOS

/// Method channel name - must match `_kChannelName` in
/// `lib/layout_engine_plugin.dart`.
private let kChannelName = "layout_engine_plugin"

/// Registers the `layout_engine_plugin` method channel used to bridge a
/// Dart-owned `LeHandle*` (see `lib/layout_engine_plugin.dart`'s `LeEditor`)
/// into a native `FlutterTexture` (see `LeTexture.swift`). Dart FFI can't
/// reach the texture registry itself - only platform embedder code can -
/// so this channel exists purely to hand a texture id back and forth; the
/// actual per-frame pixel pull never crosses it (see this plugin's
/// CLAUDE.md's Architecture section).
public class LayoutEnginePlugin: NSObject, FlutterPlugin {
  private let textureRegistry: FlutterTextureRegistry
  private var textures: [Int64: LeTexture] = [:]

  /// Tcl consoles (see LeTclBridge.h - the show_gui in-app console,
  /// TCL_EXPLORATION.md) keyed by an id this plugin hands back to Dart,
  /// same pattern as `textures` above. `nextConsoleId` mirrors how
  /// `textureRegistry.register` hands back its own id - there's no
  /// equivalent registry for Tcl consoles (they aren't Flutter engine
  /// objects), so this plugin owns the id space itself.
  private var tclConsoles: [Int64: LeTclBridge] = [:]
  private var nextConsoleId: Int64 = 0

  init(textureRegistry: FlutterTextureRegistry) {
    self.textureRegistry = textureRegistry
  }

  public static func register(with registrar: FlutterPluginRegistrar) {
    let channel = FlutterMethodChannel(name: kChannelName, binaryMessenger: registrar.messenger)
    let instance = LayoutEnginePlugin(textureRegistry: registrar.textures)
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    guard let args = call.arguments as? [String: Any] else {
      result(FlutterError(code: "bad_args", message: "\(call.method) requires a map of arguments", details: nil))
      return
    }

    switch call.method {
    case "createTexture":
      guard let handleAddress = (args["handleAddress"] as? NSNumber)?.int64Value else {
        result(FlutterError(code: "bad_args", message: "createTexture requires handleAddress", details: nil))
        return
      }
      let texture = LeTexture(handleAddress: handleAddress)
      let textureId = textureRegistry.register(texture)
      textures[textureId] = texture
      result(textureId)

    case "markTextureFrameAvailable":
      guard let textureId = (args["textureId"] as? NSNumber)?.int64Value else {
        result(FlutterError(code: "bad_args", message: "markTextureFrameAvailable requires textureId", details: nil))
        return
      }
      textureRegistry.textureFrameAvailable(textureId)
      result(nil)

    case "disposeTexture":
      guard let textureId = (args["textureId"] as? NSNumber)?.int64Value else {
        result(FlutterError(code: "bad_args", message: "disposeTexture requires textureId", details: nil))
        return
      }
      textureRegistry.unregisterTexture(textureId)
      textures.removeValue(forKey: textureId)
      result(nil)

    case "createTclConsole":
      guard let handleAddress = (args["handleAddress"] as? NSNumber)?.int64Value else {
        result(FlutterError(code: "bad_args", message: "createTclConsole requires handleAddress", details: nil))
        return
      }
      let consoleId = nextConsoleId
      nextConsoleId += 1
      tclConsoles[consoleId] = LeTclBridge(handleAddress: handleAddress)
      result(consoleId)

    case "evalTclCommand":
      guard let consoleId = (args["consoleId"] as? NSNumber)?.int64Value,
            let command = args["command"] as? String
      else {
        result(FlutterError(code: "bad_args", message: "evalTclCommand requires consoleId and command", details: nil))
        return
      }
      guard let bridge = tclConsoles[consoleId] else {
        result(FlutterError(code: "unknown_console", message: "no Tcl console with id \(consoleId)", details: nil))
        return
      }
      result(bridge.evalTcl(command))

    case "disposeTclConsole":
      guard let consoleId = (args["consoleId"] as? NSNumber)?.int64Value else {
        result(FlutterError(code: "bad_args", message: "disposeTclConsole requires consoleId", details: nil))
        return
      }
      tclConsoles.removeValue(forKey: consoleId)
      result(nil)

    default:
      result(FlutterMethodNotImplemented)
    }
  }
}
