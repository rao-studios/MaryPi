import AppKit
import Foundation
import Virtualization

/// The Mac's clipboard, shared into the guest: a virtio console port named `org.maryos.clipboard`
/// that the MaryOS desktop reads (MaryUI's `lp_clipboard_port`, PARITY D22). Each message is a
/// UInt32 little-endian byte count and then that much UTF-8, and the desktop makes it the text
/// clipboard it pastes from, so a key copied on the Mac pastes into System Settings in the VM.
/// Host to guest only; what the guest writes on the port is read and dropped.
public final class VMClipboardBridge: @unchecked Sendable {
    public static let portName = "org.maryos.clipboard"
    /// The desktop skips anything longer (LP_CLIPBOARD_FRAME_MAX).
    public static let maximumBytes = 1 << 20

    let toGuest = Pipe()
    private let fromGuest = Pipe()
    private let queue = DispatchQueue(label: "maryos.clipboard")
    private let lock = NSLock()
    private var pending: Data?
    private var writing = false
    private var lastChangeCount = -1

    /// The port's attachment: the guest reads what `send` writes, and writes into a pipe nobody needs.
    public let attachment: VZFileHandleSerialPortAttachment

    public init() {
        attachment = VZFileHandleSerialPortAttachment(fileHandleForReading: toGuest.fileHandleForReading,
                                                      fileHandleForWriting: fromGuest.fileHandleForWriting)
        fromGuest.fileHandleForReading.readabilityHandler = { handle in
            if handle.availableData.isEmpty { handle.readabilityHandler = nil }
        }
    }

    /// One message for `text`: nil when it is empty or longer than `maximumBytes`.
    public static func frame(_ text: String) -> Data? {
        let bytes = Data(text.utf8)
        guard !bytes.isEmpty, bytes.count <= maximumBytes else { return nil }
        var data = withUnsafeBytes(of: UInt32(bytes.count).littleEndian) { Data($0) }
        data.append(bytes)
        return data
    }

    /// Queues `text` for the guest; writing never blocks the caller. Until the guest reads the port a
    /// write can wait on a full pipe, so only the newest message waits behind it.
    public func send(_ text: String) {
        guard let data = Self.frame(text) else { return }
        lock.lock()
        pending = data
        let start = !writing
        writing = true
        lock.unlock()
        guard start else { return }
        let writer = toGuest.fileHandleForWriting
        queue.async { [self] in
            while true {
                lock.lock()
                guard let next = pending else {
                    writing = false
                    lock.unlock()
                    return
                }
                pending = nil
                lock.unlock()
                try? writer.write(contentsOf: next)
            }
        }
    }

    /// Sends the pasteboard's text when it has changed since the last look.
    @MainActor
    public func syncIfChanged(_ pasteboard: NSPasteboard = .general) {
        guard pasteboard.changeCount != lastChangeCount else { return }
        lastChangeCount = pasteboard.changeCount
        if let text = pasteboard.string(forType: .string) { send(text) }
    }
}

/// Follows the Mac's pasteboard into the guest while `window` is the key window: a look when it
/// becomes key, then twice a second until it resigns. Nothing is shared while you work elsewhere.
@MainActor
public final class VMClipboardSync {
    private let bridge: VMClipboardBridge
    private var timer: Timer?
    private var observers: [NSObjectProtocol] = []

    public init(bridge: VMClipboardBridge, window: NSWindow) {
        self.bridge = bridge
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: NSWindow.didBecomeKeyNotification, object: window, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.start() }
        })
        observers.append(center.addObserver(forName: NSWindow.didResignKeyNotification, object: window, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.stop() }
        })
        if window.isKeyWindow { start() }
    }

    private func start() {
        bridge.syncIfChanged()
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.bridge.syncIfChanged() }
        }
    }

    private func stop() {
        timer?.invalidate()
        timer = nil
    }

    public func invalidate() {
        stop()
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        observers = []
    }
}
