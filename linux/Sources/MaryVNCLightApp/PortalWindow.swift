import AppKit
import LiquidPlatinum
import MaryVNCKit
import MaryVNCViewer
import SwiftUI

/// The portal: the Pi's desktop in a window with no title bar, no buttons and no border, rounded like a window and
/// casting a window's shadow. It is a non-activating panel, so it takes the keys the moment it appears without
/// asking macOS to bring MaryVNC Light forward.
@MainActor
final class PortalController: NSObject, NSWindowDelegate {
    var onPutAway: () -> Void = {}

    private var window: PortalWindow?
    /// The frame to go back to after filling the screen.
    private var unfilled: NSRect?
    private static let autosaveName = "MaryVNCLightPortal"

    func show(model: LightModel) {
        let window = self.window ?? make(model: model)
        let desktop = model.link.desktopSize
        if desktop.width > 0, desktop.height > 0 {
            window.contentAspectRatio = desktop
        }
        guard !window.isVisible else {
            window.makeKeyAndOrderFront(nil)
            return
        }
        window.setFrame(placed(window.frame, desktop: desktop), display: false)
        window.alphaValue = 0
        window.makeKeyAndOrderFront(nil)
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.18
            window.animator().alphaValue = 1
        }
    }

    func close() {
        unfilled = nil
        window?.orderOut(nil)
    }

    /// Fills the screen it is on (keeping the desktop's shape), or goes back.
    func toggleFill() {
        guard let window, let screen = window.screen ?? NSScreen.main else { return }
        if let unfilled {
            window.setFrame(unfilled, display: true, animate: true)
            self.unfilled = nil
        } else {
            unfilled = window.frame
            window.setFrame(Self.fit(window.contentAspectRatio, in: screen.visibleFrame, fraction: 1), display: true, animate: true)
        }
    }

    func windowDidResize(_ notification: Notification) {
        window?.invalidateShadow()
    }

    private func make(model: LightModel) -> PortalWindow {
        let window = PortalWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 800),
                                  styleMask: [.borderless, .resizable, .nonactivatingPanel], backing: .buffered, defer: false)
        window.onPutAway = { [weak self] in self?.onPutAway() }
        window.delegate = self
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 1280, height: 800))
        container.wantsLayer = true
        container.layer?.cornerRadius = LP.Radius.window
        container.layer?.cornerCurve = .continuous
        container.layer?.masksToBounds = true
        let host = NSHostingView(rootView: PortalContent().environment(model))
        host.frame = container.bounds
        host.autoresizingMask = [.width, .height]
        container.addSubview(host)
        let strip = GrabStrip(frame: NSRect(x: 0, y: container.bounds.height - GrabStrip.height, width: container.bounds.width, height: GrabStrip.height))
        strip.autoresizingMask = [.width, .minYMargin]
        strip.onDoubleClick = { [weak self] in self?.toggleFill() }
        container.addSubview(strip)
        window.contentView = container
        window.setFrameAutosaveName(Self.autosaveName)
        self.window = window
        return window
    }

    /// Where the portal opens: where it was last, if that is still on a screen and the desktop's shape; otherwise
    /// one point for each of the Pi's pixels, no more than 90% of the screen with the pointer, in its middle.
    private func placed(_ saved: NSRect, desktop: CGSize) -> NSRect {
        let pointer = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { NSMouseInRect(pointer, $0.frame, false) } ?? NSScreen.main
        let visible = screen?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let hasSaved = UserDefaults.standard.string(forKey: "NSWindow Frame \(Self.autosaveName)") != nil
        let onScreen = NSScreen.screens.contains { $0.visibleFrame.intersects(saved) }
        if hasSaved, onScreen, desktop.width > 0, desktop.height > 0, abs(saved.width / saved.height - desktop.width / desktop.height) < 0.01 {
            return saved
        }
        let size = desktop.width > 0 && desktop.height > 0 ? desktop : CGSize(width: 1280, height: 800)
        let limit = CGSize(width: visible.width * 0.9, height: visible.height * 0.9)
        let scale = min(1, limit.width / size.width, limit.height / size.height)
        let width = (size.width * scale).rounded(), height = (size.height * scale).rounded()
        return NSRect(x: (visible.midX - width / 2).rounded(), y: (visible.midY - height / 2).rounded(), width: width, height: height)
    }

    private static func fit(_ aspect: NSSize, in rect: NSRect, fraction: CGFloat) -> NSRect {
        guard aspect.width > 0, aspect.height > 0 else { return rect }
        let scale = min(rect.width * fraction / aspect.width, rect.height * fraction / aspect.height)
        let width = (aspect.width * scale).rounded(), height = (aspect.height * scale).rounded()
        return NSRect(x: (rect.midX - width / 2).rounded(), y: (rect.midY - height / 2).rounded(), width: width, height: height)
    }
}

/// A panel that can be key without a title bar, and puts itself away on ⌘Q, ⌘H or ⌘M.
final class PortalWindow: NSPanel {
    var onPutAway: () -> Void = {}

    override init(contentRect: NSRect, styleMask style: NSWindow.StyleMask, backing: NSWindow.BackingStoreType, defer flag: Bool) {
        super.init(contentRect: contentRect, styleMask: style, backing: backing, defer: flag)
        isOpaque = false
        backgroundColor = .clear
        hasShadow = true
        isReleasedWhenClosed = false
        hidesOnDeactivate = false
        becomesKeyOnlyIfNeeded = false
        isFloatingPanel = false
        isMovableByWindowBackground = false
        collectionBehavior = [.moveToActiveSpace, .fullScreenAuxiliary]
        minSize = NSSize(width: 400, height: 250)
        animationBehavior = .none
    }

    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        if flags == .command, ["q", "h", "m"].contains(event.charactersIgnoringModifiers?.lowercased() ?? "") {
            onPutAway()
            return true
        }
        return super.performKeyEquivalent(with: event)
    }
}

/// The top edge of the portal: after the pointer rests there for half a second a grabber shows, and the edge moves
/// the window (a double click fills the screen or goes back). Until then it is not there at all, and the Pi's
/// desktop has its top edge.
final class GrabStrip: NSView {
    static let height: CGFloat = 8

    var onDoubleClick: () -> Void = {}

    private var hovering = false
    private var revealed = false {
        didSet {
            guard revealed != oldValue else { return }
            needsDisplay = true
            window?.invalidateCursorRects(for: self)
        }
    }

    override var mouseDownCanMoveWindow: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for area in trackingAreas { removeTrackingArea(area) }
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect], owner: self))
    }

    override func mouseEntered(with event: NSEvent) {
        hovering = true
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(500))
            guard let self, self.hovering else { return }
            self.revealed = true
        }
    }

    override func mouseExited(with event: NSEvent) {
        hovering = false
        revealed = false
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        revealed ? super.hitTest(point) : nil
    }

    override func mouseDown(with event: NSEvent) {
        if event.clickCount == 2 {
            onDoubleClick()
        } else {
            window?.performDrag(with: event)
        }
    }

    override func resetCursorRects() {
        if revealed { addCursorRect(bounds, cursor: .openHand) }
    }

    override func draw(_ dirtyRect: NSRect) {
        guard revealed else { return }
        NSColor.black.withAlphaComponent(0.25).setFill()
        bounds.fill()
        let grabber = NSRect(x: bounds.midX - 22, y: bounds.midY - 2, width: 44, height: 4)
        NSColor.white.withAlphaComponent(0.85).setFill()
        NSBezierPath(roundedRect: grabber, xRadius: 2, yRadius: 2).fill()
    }
}

/// What the portal shows: the desktop, dimmed while the link is being brought back.
struct PortalContent: View {
    @Environment(LightModel.self) private var model

    var body: some View {
        let link = model.link
        ZStack {
            LP.Platinum._9.color
            if let frame = link.frame {
                RemoteView(image: frame, desktopSize: link.desktopSize, cursorShown: link.cursorShown,
                           keyMap: KeyMap(commandKey: model.settings.commandKey), enabled: link.isConnected,
                           reservedCommandKeys: ["q", "h", "m"], send: { link.send($0) })
                    .opacity(link.isConnected ? 1 : 0.45)
            }
            if case let .waiting(name, _) = link.phase {
                Text("Lost \(name) · looking for it")
                    .font(.system(size: LP.Text.sm, weight: .medium))
                    .foregroundStyle(.white)
                    .padding(.horizontal, LP.Space._4)
                    .padding(.vertical, LP.Space._2)
                    .background(Capsule().fill(Color.black.opacity(0.55)))
            }
        }
        .ignoresSafeArea()
    }
}
