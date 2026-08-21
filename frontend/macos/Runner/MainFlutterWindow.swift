import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    self.minSize = NSSize(width: 1280, height: 800)
    self.setContentSize(self.minSize)
    self.center()

    // Restores the frame saved under this name from a previous launch, as a
    // side effect of registering it (overriding the centered default set
    // above when one exists), and keeps saving future moves/resizes under
    // this name going forward. A no-op on the very first launch, when
    // there's nothing saved yet, leaving the centered default in place.
    self.setFrameAutosaveName("MainWindow")

    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}
