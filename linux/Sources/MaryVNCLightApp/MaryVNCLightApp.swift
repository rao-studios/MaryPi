import AppKit
import LiquidPlatinum
import MaryVNCKit
import SwiftUI

/// MaryVNC Light: MaryVNC in the menu bar. A press of a paired Pi's power button opens its desktop in a borderless
/// window; a Pi ready to pair asks to pair. `vnc-light.sh` builds and opens it.
@main
struct MaryVNCLightApp: App {
    @NSApplicationDelegateAdaptor(LightDelegate.self) private var delegate

    var body: some Scene {
        MenuBarExtra {
            MenuContent().environment(delegate.model)
        } label: {
            MenuBarIcon().environment(delegate.model)
        }
        .menuBarExtraStyle(.menu)
    }
}

@MainActor
final class LightDelegate: NSObject, NSApplicationDelegate {
    let model: LightModel
    private var activity: NSObjectProtocol?

    override init() {
        model = LightModel(arguments: CommandLine.arguments)
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        // The calls go out every second or so, and App Nap would stretch them until a press waited.
        activity = ProcessInfo.processInfo.beginActivity(options: .userInitiatedAllowingIdleSystemSleep,
                                                         reason: "MaryVNC Light calls for the Pis nearby")
        ProcessInfo.processInfo.disableAutomaticTermination("MaryVNC Light lives in the menu bar")
        // The brushed grain takes a moment to make; start before a dialog asks for it.
        Task.detached(priority: .userInitiated) { _ = BrushTile.image }
        // After the icon is up: the Keychain may ask for this Mac's key, and its prompt blocks.
        Task { @MainActor in model.start() }
    }
}

struct MenuBarIcon: View {
    @Environment(LightModel.self) private var model

    var body: some View {
        Image(systemName: model.iconName)
    }
}
