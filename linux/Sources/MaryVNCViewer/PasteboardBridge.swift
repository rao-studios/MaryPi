import AppKit
import MaryVNCKit

/// The Mac's pasteboard and the Pi's clipboard, both ways, while a session shows the desktop. What the Mac copies
/// goes to the Pi (looked for twice a second, since the pasteboard has no notification), unless ClipboardText holds
/// it back; what the Pi copies lands on the Mac, and is not sent back.
@MainActor
final class PasteboardBridge {
    var send: (String) -> Void = { _ in }

    private let pasteboard: NSPasteboard
    private var seen = -1
    private var polling: Task<Void, Never>?

    init(pasteboard: NSPasteboard = .general) {
        self.pasteboard = pasteboard
    }

    /// The desktop is showing: the Mac's clipboard goes now, and every change after it.
    func start() {
        seen = -1
        check()
        polling?.cancel()
        polling = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(500))
                self?.check()
            }
        }
    }

    func stop() {
        polling?.cancel()
        polling = nil
    }

    /// The Pi's clipboard.
    func received(_ text: String) {
        if pasteboard.string(forType: .string) != text {
            pasteboard.clearContents()
            pasteboard.setString(text, forType: .string)
        }
        seen = pasteboard.changeCount
    }

    private func check() {
        let count = pasteboard.changeCount
        guard count != seen else { return }
        seen = count
        let types = pasteboard.types?.map(\.rawValue) ?? []
        if let text = ClipboardText.shareable(pasteboard.string(forType: .string), types: types) { send(text) }
    }
}
