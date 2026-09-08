import AppKit
import MaryOSKit
import SwiftUI

@main
struct MaryOSApp: App {
    @State private var model = AppModel()

    init() {
        // `swift run MaryOSApp` launches a bare, unsigned executable: sign it
        // with the Virtualization entitlement and restart if needed, then
        // make it a regular foreground app with a Dock icon and menu bar.
        if !SelfEntitlement.isPresent() {
            do {
                try SelfEntitlement.signAndRestart { line in
                    FileHandle.standardError.write(Data("MaryOS: \(line)\n".utf8))
                }
            } catch {
                NSLog("MaryOS: %@", error.localizedDescription)
            }
        }
        NSApplication.shared.setActivationPolicy(.regular)
    }

    var body: some Scene {
        WindowGroup("MaryOS") {
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
                Button("Refresh Images") { model.refreshArtifacts() }
                    .keyboardShortcut("r", modifiers: [.command, .shift])
                Divider()
                Button("Choose Kit Directory…") { model.chooseKitDirectory() }
                Button("Run Doctor…") { model.showDoctor = true }
            }
            CommandMenu("Build") {
                Button("Build Raspberry Pi 5 Image") { Task { await model.buildImage(.pi5) } }
                    .disabled(!model.canBuild)
                Button("Build VM Image") { Task { await model.buildImage(.vm) } }
                    .disabled(!model.canBuild)
                Divider()
                Button("Reveal Output Folder") { model.revealOutput() }
                    .disabled(model.paths == nil)
            }
            CommandMenu("VM") {
                Button("Test in VM") { Task { await model.testInVM() } }
                    .keyboardShortcut("t")
                    .disabled(!model.canTestInVM)
                Button("Stop VM") { Task { await model.stopVM() } }
                    .keyboardShortcut("t", modifiers: [.command, .shift])
                    .disabled(!model.vmIsRunning)
                Divider()
                Button("Reset VM Disk") { model.resetVM() }
                    .disabled(model.vmIsRunning || model.paths == nil)
                Button("Reveal Serial Log") { model.revealSerialLog() }
                    .disabled(model.paths == nil)
            }
        }

        Window("MaryOS VM", id: "vm") {
            VMWindowView()
                .environment(model)
        }
        .defaultSize(width: 1280, height: 800)
    }
}
