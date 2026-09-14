import AppKit
import LiquidPlatinum
import Observation
import SwiftUI

/// The window under the Liquid Platinum chrome: macOS's title bar hidden, and where the window sits on the
/// screen (the grain is anchored there, and the room's light falls across the title bar from it).
@MainActor @Observable
final class WindowState {
    var origin: CGPoint = .zero
    var sheen: CGFloat = CGFloat(LP.Sheen.lightX)
    var isKey = true
    var isZoomed = false

    @ObservationIgnored private weak var window: NSWindow?
    @ObservationIgnored private var observers: [NSObjectProtocol] = []

    func attach(_ window: NSWindow) {
        guard self.window !== window else { return }
        self.window = window
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.styleMask.insert(.fullSizeContentView)
        for button in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] {
            window.standardWindowButton(button)?.isHidden = true
        }
        window.isMovableByWindowBackground = false
        window.minSize = NSSize(width: 640, height: 420)
        let names = [NSWindow.didMoveNotification, NSWindow.didResizeNotification, NSWindow.didBecomeKeyNotification,
                     NSWindow.didResignKeyNotification, NSWindow.didChangeScreenNotification]
        for name in names {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: window, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.update() }
            })
        }
        update()
    }

    func update() {
        guard let window else { return }
        let frame = window.frame
        // Screen coordinates with y running down from the top of the main screen, as the grain samples them.
        let top = NSScreen.screens.first?.frame.maxY ?? frame.maxY
        origin = CGPoint(x: frame.minX, y: top - frame.maxY)
        let screen = window.screen?.frame ?? NSScreen.main?.frame ?? frame
        sheen = SheenBand.position(windowX: frame.minX - screen.minX, windowWidth: frame.width, screenWidth: screen.width)
        isKey = window.isKeyWindow
        isZoomed = window.isZoomed
    }

    func close() { window?.performClose(nil) }
    func minimize() { window?.miniaturize(nil) }
    func zoom() { window?.zoom(nil) }
}

/// Hands the hosting NSWindow to `onWindow` once the view is in one.
struct WindowAccessor: NSViewRepresentable {
    var onWindow: (NSWindow) -> Void

    func makeNSView(context: Context) -> AccessorView {
        AccessorView(onWindow: onWindow)
    }

    func updateNSView(_ view: AccessorView, context: Context) {
        view.onWindow = onWindow
    }

    final class AccessorView: NSView {
        var onWindow: (NSWindow) -> Void

        init(onWindow: @escaping (NSWindow) -> Void) {
            self.onWindow = onWindow
            super.init(frame: .zero)
        }

        required init?(coder: NSCoder) {
            return nil
        }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let window { onWindow(window) }
        }
    }
}
