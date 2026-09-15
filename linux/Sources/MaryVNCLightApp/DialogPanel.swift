import AppKit
import LiquidPlatinum
import MaryVNCKit
import SwiftUI

/// MaryVNC Light's one dialog at a time, in a floating panel of its own: Liquid Platinum's alert panel with nothing
/// around it. A non-activating panel, so Return and Esc reach it without bringing MaryVNC Light forward.
@MainActor
final class PanelController {
    private var panel: DialogPanel?
    private var host: NSView?

    func show(model: LightModel) {
        if panel == nil {
            let host = NSHostingView(rootView: DialogHost().environment(model))
            let panel = DialogPanel(contentRect: NSRect(x: 0, y: 0, width: 488, height: 200),
                                    styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
            panel.contentView = host
            self.host = host
            self.panel = panel
        }
        fit()
        panel?.makeKeyAndOrderFront(nil)
        // SwiftUI lays the new dialog out on the next turn; size the panel to it then.
        DispatchQueue.main.async { [weak self] in self?.fit() }
    }

    /// Sizes the panel to its dialog, in the upper middle of the screen with the pointer.
    func fit() {
        guard let panel, let host else { return }
        let size = host.fittingSize
        guard size.width > 0, size.height > 0 else { return }
        let pointer = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { NSMouseInRect(pointer, $0.frame, false) } ?? NSScreen.main
        let visible = screen?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let origin = NSPoint(x: (visible.midX - size.width / 2).rounded(), y: (visible.minY + visible.height * 0.62 - size.height / 2).rounded())
        panel.setFrame(NSRect(origin: origin, size: size), display: true)
    }

    func close() {
        panel?.orderOut(nil)
    }
}

final class DialogPanel: NSPanel {
    override init(contentRect: NSRect, styleMask style: NSWindow.StyleMask, backing: NSWindow.BackingStoreType, defer flag: Bool) {
        super.init(contentRect: contentRect, styleMask: style, backing: backing, defer: flag)
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false                       // the alert panel draws its own
        level = .floating
        hidesOnDeactivate = false
        becomesKeyOnlyIfNeeded = false
        isReleasedWhenClosed = false
        collectionBehavior = [.moveToActiveSpace, .fullScreenAuxiliary]
        animationBehavior = .alertPanel
    }

    override var canBecomeKey: Bool { true }
}

/// The dialog the model asks for.
struct DialogHost: View {
    @Environment(LightModel.self) private var model

    var body: some View {
        content
            .frame(width: 440)
            .padding(24)                        // room for the panel's shadow
            .coordinateSpace(.named(lpWindowSpace))
            .environment(\.lpAccent, model.settings.accent == .graphite ? .graphite : .blue)
    }

    @ViewBuilder private var content: some View {
        switch model.dialog {
        case .pair:
            pair
        case let .message(title, text):
            LPAlertPanel(symbol: "exclamationmark.triangle", title: title, message: text,
                         actions: [.init("OK", role: .primary) { model.dismissDialog() }])
        case let .forget(paired):
            LPAlertPanel(symbol: "trash", title: "Forget “\(paired.name)”?",
                         message: "To use it again, pair again: press the Pi’s power button, then click Pair. On the Pi, `maryvncctl forget` removes this Mac as well.",
                         actions: [.init("Forget", role: .primary) { model.forget(paired) }, .init("Cancel", role: .cancel) { model.dismissDialog() }])
        case nil:
            EmptyView()
        }
    }

    @ViewBuilder private var pair: some View {
        let offers = model.offers
        if offers.count == 1, let pi = offers.first {
            LPAlertPanel(symbol: "link", title: "Pair with “\(pi.name)”?",
                         message: "Its power button was just pressed, and it offers the key \(pi.fingerprint.display) (`maryvncctl status` on the Pi shows its own). This Mac and the Pi will remember each other, and from then on a press of the button opens its desktop here.",
                         actions: [.init("Pair", role: .primary) { model.pair(pi) }, .init("Not Now", role: .cancel) { model.notNow() }])
        } else {
            let chosen = offers.first { $0.fingerprint == model.pairChoice } ?? offers.first
            LPAlertPanel(symbol: "link", title: "Pair with a Pi?",
                         message: "\(offers.count) Pis are ready to pair. Choose yours by its key; `maryvncctl status` on the Pi shows it.",
                         actions: [.init("Pair", role: .primary) { if let chosen { model.pair(chosen) } }, .init("Not Now", role: .cancel) { model.notNow() }]) {
                VStack(alignment: .leading, spacing: LP.Space._1) {
                    ForEach(offers) { pi in
                        Button { model.pairChoice = pi.fingerprint } label: {
                            HStack(spacing: LP.Space._2) {
                                Image(systemName: pi.id == chosen?.id ? "largecircle.fill.circle" : "circle")
                                Text(pi.name).font(.system(size: LP.Text.sm, weight: .medium))
                                Spacer(minLength: LP.Space._2)
                                Text(pi.fingerprint.short).font(.system(size: LP.Text.xs).monospaced())
                            }
                            .foregroundStyle(LP.Ink.secondary.color)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.top, LP.Space._1)
            }
        }
    }
}
