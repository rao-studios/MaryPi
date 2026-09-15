import AppKit
import MaryVNCKit
import SwiftUI

/// The menu under MaryVNC Light's icon.
struct MenuContent: View {
    @Environment(LightModel.self) private var model

    var body: some View {
        Text(model.statusLine)
        let nearby = model.nearby.pairedNearby
        if !nearby.isEmpty {
            Section("Nearby") {
                ForEach(nearby) { pi in
                    Toggle(pi.name, isOn: Binding(get: { model.isShowing(pi) }, set: { on in
                        if on { model.open(pi) } else { model.closePortal() }
                    }))
                }
            }
        }
        let offers = model.offers
        if !offers.isEmpty {
            Section("Ready to Pair") {
                ForEach(offers) { pi in
                    Button("Pair with “\(pi.name)”…") { model.askToPair(pi) }
                }
            }
        }
        Divider()
        if model.link.isActive {
            Button("Close Portal") { model.closePortal() }
        }
        Button("Look Again") { model.lookAgain() }
        Divider()
        Picker("Picture", selection: Binding(get: { model.settings.quality }, set: { model.setQuality($0) })) {
            Text("Best").tag(FrameQuality.best)
            Text("Fast").tag(FrameQuality.fast)
        }
        Picker("⌘ on the Pi", selection: Binding(get: { model.settings.commandKey }, set: { model.setCommandKey($0) })) {
            Text("Super").tag(CommandKey.super)
            Text("Control").tag(CommandKey.control)
        }
        if !model.pairing.pis.isEmpty {
            Menu("Forget") {
                ForEach(model.pairing.pis) { paired in
                    Button("\(paired.name) (\(paired.fingerprint.short))…") { model.askToForget(paired) }
                }
            }
        }
        Divider()
        if let fingerprint = model.fingerprint {
            Text("This Mac’s key: \(fingerprint.short)")
        }
        Button("Quit MaryVNC Light") { NSApp.terminate(nil) }
            .keyboardShortcut("q")
    }
}
