import AppKit
import LiquidPlatinum
import Carbon.HIToolbox   // kVK_* (Carbon's own KeyMap type is why MaryVNCKit.KeyMap is spelled out)
import MaryVNCKit
import SwiftUI

/// The Pi's desktop, aspect-fit, taking this Mac's pointer, scroll and keys for the Pi.
struct RemoteView: NSViewRepresentable {
    var image: CGImage
    var desktopSize: CGSize
    var cursorShown: Bool
    var keyMap: MaryVNCKit.KeyMap
    var enabled: Bool
    var send: (WireMessage) -> Void

    func makeNSView(context: Context) -> RemoteNSView {
        let view = RemoteNSView(frame: .zero)
        update(view)
        return view
    }

    func updateNSView(_ view: RemoteNSView, context: Context) {
        update(view)
    }

    private func update(_ view: RemoteNSView) {
        view.send = send
        view.keyMap = keyMap
        view.desktopSize = desktopSize
        view.cursorShown = cursorShown
        view.enabled = enabled
        view.show(image)
    }
}

final class RemoteNSView: NSView {
    var send: (WireMessage) -> Void = { _ in }
    var keyMap = MaryVNCKit.KeyMap()
    var desktopSize = CGSize.zero
    var cursorShown = false {
        didSet { if oldValue != cursorShown { window?.invalidateCursorRects(for: self) } }
    }
    var enabled = false {
        didSet {
            if !enabled { releaseAll() }
            if enabled && !oldValue { window?.makeFirstResponder(self) }
        }
    }

    private var buttons: PointerButtons = []
    private var held: Set<UInt16> = []
    private var last: (x: UInt16, y: UInt16) = (0, 0)
    private var scrollRemainder = CGPoint.zero
    nonisolated(unsafe) private var monitor: Any?
    private var resignObserver: NSObjectProtocol?

    /// A cursor with nothing in it, for when the Pi paints its pointer into the picture.
    private static let blankCursor = NSCursor(image: NSImage(size: NSSize(width: 1, height: 1)), hotSpot: .zero)

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.contentsGravity = .resizeAspect
        layer?.backgroundColor = LP.Platinum._9.cgColor   // the letterbox: the deepest platinum, not black
        layer?.magnificationFilter = .linear
        layer?.minificationFilter = .trilinear
    }

    required init?(coder: NSCoder) {
        return nil
    }

    deinit {
        if let monitor { NSEvent.removeMonitor(monitor) }
    }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    func show(_ image: CGImage) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer?.contents = image
        CATransaction.commit()
    }

    override func viewWillMove(toWindow newWindow: NSWindow?) {
        super.viewWillMove(toWindow: newWindow)
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
        if let resignObserver { NotificationCenter.default.removeObserver(resignObserver) }
        resignObserver = nil
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        guard let window else { return }
        // Keys go through a local monitor: AppKit never delivers a key up while ⌘ is held, and the Pi needs
        // every release.
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] event in
            let consumed = MainActor.assumeIsolated { self?.consume(event) ?? false }
            return consumed ? nil : event
        }
        resignObserver = NotificationCenter.default.addObserver(forName: NSWindow.didResignKeyNotification, object: window, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.releaseAll() }
        }
    }

    override func resignFirstResponder() -> Bool {
        releaseAll()
        return super.resignFirstResponder()
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for area in trackingAreas { removeTrackingArea(area) }
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self))
    }

    override func resetCursorRects() {
        if !cursorShown { addCursorRect(bounds, cursor: Self.blankCursor) }
    }

    // MARK: Pointer

    /// Where the picture sits in the view.
    private var pictureRect: CGRect {
        guard desktopSize.width > 0, desktopSize.height > 0, bounds.width > 0, bounds.height > 0 else { return bounds }
        let scale = min(bounds.width / desktopSize.width, bounds.height / desktopSize.height)
        let size = CGSize(width: desktopSize.width * scale, height: desktopSize.height * scale)
        return CGRect(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2, width: size.width, height: size.height)
    }

    private func desktopPoint(_ event: NSEvent) -> (x: UInt16, y: UInt16) {
        let point = convert(event.locationInWindow, from: nil)
        let rect = pictureRect
        guard rect.width > 0, rect.height > 0 else { return (0, 0) }
        let x = ((point.x - rect.minX) / rect.width * desktopSize.width).rounded(.down)
        let y = ((point.y - rect.minY) / rect.height * desktopSize.height).rounded(.down)
        return (UInt16(clamping: Int(min(max(x, 0), max(desktopSize.width - 1, 0)))),
                UInt16(clamping: Int(min(max(y, 0), max(desktopSize.height - 1, 0)))))
    }

    private func pointer(_ event: NSEvent) {
        guard enabled else { return }
        last = desktopPoint(event)
        send(.pointer(x: last.x, y: last.y, buttons: buttons, scrollX: 0, scrollY: 0))
    }

    override func mouseMoved(with event: NSEvent) { pointer(event) }
    override func mouseDragged(with event: NSEvent) { pointer(event) }
    override func rightMouseDragged(with event: NSEvent) { pointer(event) }
    override func otherMouseDragged(with event: NSEvent) { pointer(event) }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        buttons.insert(.left)
        pointer(event)
    }

    override func mouseUp(with event: NSEvent) {
        buttons.remove(.left)
        pointer(event)
    }

    override func rightMouseDown(with event: NSEvent) {
        buttons.insert(.right)
        pointer(event)
    }

    override func rightMouseUp(with event: NSEvent) {
        buttons.remove(.right)
        pointer(event)
    }

    override func otherMouseDown(with event: NSEvent) {
        if event.buttonNumber == 2 { buttons.insert(.middle) }
        pointer(event)
    }

    override func otherMouseUp(with event: NSEvent) {
        if event.buttonNumber == 2 { buttons.remove(.middle) }
        pointer(event)
    }

    override func scrollWheel(with event: NSEvent) {
        guard enabled else { return }
        // A wheel's line is libinput's 15; a trackpad already speaks in points. Down on the Pi is positive.
        let factor: CGFloat = event.hasPreciseScrollingDeltas ? 1 : 15
        scrollRemainder.x -= event.scrollingDeltaX * factor
        scrollRemainder.y -= event.scrollingDeltaY * factor
        let dx = Int16(clamping: Int(scrollRemainder.x.rounded(.towardZero)))
        let dy = Int16(clamping: Int(scrollRemainder.y.rounded(.towardZero)))
        scrollRemainder.x -= CGFloat(dx)
        scrollRemainder.y -= CGFloat(dy)
        if dx != 0 || dy != 0 {
            send(.pointer(x: last.x, y: last.y, buttons: buttons, scrollX: dx, scrollY: dy))
        }
    }

    // MARK: Keys

    /// Whether this event went to the Pi (and so goes no further on the Mac).
    private func consume(_ event: NSEvent) -> Bool {
        guard enabled, let window, window.isKeyWindow, window.firstResponder === self else { return false }
        switch event.type {
        case .keyDown:
            if Self.isReserved(event) { return false }
            if event.isARepeat { return true }            // the Pi repeats held keys itself
            guard let code = keyMap.evdev(forKeyCode: event.keyCode) else { return true }
            held.insert(code)
            send(.key(evdev: code, pressed: true))
            return true
        case .keyUp:
            guard let code = keyMap.evdev(forKeyCode: event.keyCode) else { return true }
            if held.remove(code) != nil { send(.key(evdev: code, pressed: false)) }
            return true
        case .flagsChanged:
            guard let code = keyMap.evdev(forKeyCode: event.keyCode) else { return false }
            if Int(event.keyCode) == kVK_CapsLock {
                send(.key(evdev: code, pressed: true))
                send(.key(evdev: code, pressed: false))
                return false
            }
            let down = Self.deviceMask(event.keyCode).map { event.modifierFlags.rawValue & $0 != 0 } ?? !held.contains(code)
            if down, !held.contains(code) {
                held.insert(code)
                send(.key(evdev: code, pressed: true))
            } else if !down, held.remove(code) != nil {
                send(.key(evdev: code, pressed: false))
            }
            return false
        default:
            return false
        }
    }

    func releaseAll() {
        for code in held { send(.key(evdev: code, pressed: false)) }
        held.removeAll()
        if !buttons.isEmpty {
            buttons = []
            send(.pointer(x: last.x, y: last.y, buttons: [], scrollX: 0, scrollY: 0))
        }
    }

    /// ⌘Q, ⌘H, ⌘⌥H, ⌘M and ⌘, stay with the Mac; every other chord goes to the Pi.
    static func isReserved(_ event: NSEvent) -> Bool {
        guard event.modifierFlags.contains(.command) else { return false }
        return ["q", "h", "m", ","].contains(event.charactersIgnoringModifiers?.lowercased() ?? "")
    }

    /// The device-dependent flag for each side's modifier (NX_DEVICE…KEYMASK), so releasing one Shift while
    /// the other is held is still a release.
    static func deviceMask(_ keyCode: UInt16) -> UInt? {
        switch Int(keyCode) {
        case kVK_Control: 0x0001
        case kVK_Shift: 0x0002
        case kVK_RightShift: 0x0004
        case kVK_Command: 0x0008
        case kVK_RightCommand: 0x0010
        case kVK_Option: 0x0020
        case kVK_RightOption: 0x0040
        case kVK_RightControl: 0x2000
        default: nil
        }
    }
}
