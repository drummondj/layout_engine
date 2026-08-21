import CoreVideo
import FlutterMacOS

/// Wraps one `LeHandle*` (via `LeApiBridge`) as a `FlutterTexture`: pulls a
/// frame on demand (called by the Flutter engine's raster thread, only
/// after `LayoutEnginePlugin` relays a `markTextureFrameAvailable` call - see
/// this plugin's CLAUDE.md for the full push/pull split). Does not own the
/// handle - the Dart-side `LeEditor` that created it does, and must
/// outlive every `LeTexture` built from it.
final class LeTexture: NSObject, FlutterTexture {
  private let bridge: LeApiBridge

  init(handleAddress: Int64) {
    bridge = LeApiBridge(handleAddress: handleAddress)
    super.init()
  }

  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    guard let pixelBuffer = bridge.copyPixelBufferBGRA() else { return nil }
    return Unmanaged.passRetained(pixelBuffer)
  }
}
