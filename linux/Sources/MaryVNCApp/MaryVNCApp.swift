import AppKit
import LiquidPlatinum
import MaryVNCKit
import SwiftUI

@main
struct MaryVNCApp: App {
    @State private var model = AppModel(arguments: CommandLine.arguments)

    init() {
        // `vnc.sh` runs a bare executable: make it a regular app with a Dock icon and a menu bar.
        NSApplication.shared.setActivationPolicy(.regular)
        // A bundled MaryVNC.app has its AppIcon.icns; the bare build finds the icon in SwiftPM's resource
        // bundle beside it (not through Bundle.module, which stops the app when the bundle is elsewhere).
        if Bundle.main.object(forInfoDictionaryKey: "CFBundleIconFile") == nil, let icon = Self.buildIcon() {
            NSApplication.shared.applicationIconImage = icon
        }
        // The brushed grain takes a moment to make; start before the first surface asks for it.
        Task.detached(priority: .userInitiated) { _ = BrushTile.image }
    }

    private static func buildIcon() -> NSImage? {
        guard let directory = Bundle.main.executableURL?.deletingLastPathComponent(),
              let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path),
              let bundle = names.first(where: { $0.hasSuffix("_MaryVNCApp.bundle") }) else { return nil }
        return NSImage(contentsOf: directory.appending(path: "\(bundle)/AppIcon.iconset/icon_512x512@2x.png"))
    }

    var body: some Scene {
        Window("MaryVNC", id: "viewer") {
            ViewerWindow()
                .environment(model)
                .task { await model.start() }
                .onAppear { NSApp.activate() }
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentMinSize)
        .defaultSize(width: 1120, height: 760)
        .commands { ViewerCommands(model: model) }

        Settings {
            SettingsView().environment(model)
        }
    }
}

struct ViewerCommands: Commands {
    let model: AppModel

    var body: some Commands {
        CommandGroup(replacing: .appInfo) {
            Button("About MaryVNC") { model.showAbout() }
        }
        CommandGroup(replacing: .newItem) {
            Button("Connect to Address…") { model.sheet = .connect }
                .keyboardShortcut("k")
            Button("Disconnect") { model.disconnect() }
                .keyboardShortcut("d")
                .disabled(!model.isSessionActive)
            Divider()
            Button("Pair…") { model.pairSelected() }
                .keyboardShortcut("p", modifiers: [.command, .shift])
                .disabled(!model.canPairSelected)
            Button("Forget This Pi…") { model.askToForgetSelected() }
                .disabled(model.selectedPaired == nil)
        }
        CommandGroup(after: .toolbar) {
            Button(model.isConnected ? "Refresh Screen" : "Look for Pis Again") { model.refresh() }
                .keyboardShortcut("r")
            Picker("Quality", selection: Binding(get: { model.settings.quality }, set: { model.setQuality($0) })) {
                Text("Best").tag(FrameQuality.best)
                Text("Fast").tag(FrameQuality.fast)
            }
            Divider()
        }
    }
}
