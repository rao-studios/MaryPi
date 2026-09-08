import AppKit
import Virtualization

/// A plain AppKit window showing a machine's display, used by the CLI's
/// `vm run`. The app wraps `VZVirtualMachineView` in SwiftUI instead.
@MainActor
public final class VMWindowController: NSWindowController, NSWindowDelegate {
    public let machineView: VZVirtualMachineView
    /// Called when the user closes the window; the caller stops the VM.
    public var onClose: (@MainActor () -> Void)?

    public init(machine: VZVirtualMachine, title: String, width: Int, height: Int) {
        let size = NSSize(width: width, height: height)
        machineView = VZVirtualMachineView(frame: NSRect(origin: .zero, size: size))
        machineView.virtualMachine = machine
        machineView.capturesSystemKeys = true
        machineView.autoresizingMask = [.width, .height]
        let window = NSWindow(contentRect: NSRect(origin: .zero, size: size),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.title = title
        window.contentView = machineView
        window.center()
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
    }

    @available(*, unavailable)
    public required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    public func windowWillClose(_ notification: Notification) {
        onClose?()
    }
}
