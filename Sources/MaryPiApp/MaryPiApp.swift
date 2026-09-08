import AppKit
import MaryPiKit
import SwiftUI

@main
struct MaryPiApp: App {
    @State private var model = AppModel()

    init() {
        // `swift run MaryPi` launches a bare executable; make it a regular
        // foreground app with a Dock icon and menu bar.
        NSApplication.shared.setActivationPolicy(.regular)
    }

    var body: some Scene {
        WindowGroup("MaryPi") {
            ContentView()
                .environment(model)
                .task { await model.start() }
                .onAppear { NSApp.activate(ignoringOtherApps: true) }
        }
        .defaultSize(width: 1180, height: 760)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandMenu("Card") {
                Button("Refresh Disks") { Task { await model.refreshDisks() } }
                    .keyboardShortcut("r")
                Button("Rescan ravynOS Build Tree") { model.rescanPayload() }
                    .keyboardShortcut("r", modifiers: [.command, .shift])
                Divider()
                Button("Choose ravynOS Checkout…") { model.chooseRavynOSRoot() }
                Button("Run Doctor…") { model.showDoctor = true }
            }
            CommandMenu("VM") {
                Button("Test in VM") { Task { await model.testInVM() } }
                    .keyboardShortcut("t")
                    .disabled(!model.canTestInVM)
                Button("Stop VM") { Task { await model.stopVM() } }
                    .keyboardShortcut("t", modifiers: [.command, .shift])
                    .disabled(!model.vmIsRunning)
                Divider()
                Picker("Profile", selection: Binding(get: { model.vmProfile }, set: { model.vmProfile = $0 })) {
                    ForEach(model.vmProfiles, id: \.self) { Text($0).tag($0) }
                }
                Button("Reveal Serial Log") { model.revealSerialLog() }
                    .disabled(model.vm == nil)
                Button("VM Doctor…") { model.showDoctor = true }
            }
        }
    }
}
