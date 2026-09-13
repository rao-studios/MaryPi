import MaryOSKit
import SwiftUI
import Virtualization

/// The VM window: the guest's display, keyboard and pointer.
struct VMWindowView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some View {
        Group {
            if let machine = model.vm?.machine {
                VMView(machine: machine, clipboard: model.vm?.clipboard)
            } else {
                ContentUnavailableView {
                    Label("No VM running", systemImage: "desktopcomputer")
                } description: {
                    Text("Use Test in VM in the main window.")
                }
            }
        }
        .frame(minWidth: 640, minHeight: 400)
        .navigationTitle(model.vmSpec?.name ?? "MaryOS VM")
        .onChange(of: model.vmIsRunning) { _, running in
            if !running { dismissWindow(id: "vm") }
        }
        .onDisappear {
            Task { await model.stopVM() }
        }
    }
}

struct VMView: NSViewRepresentable {
    let machine: VZVirtualMachine
    let clipboard: VMClipboardBridge?

    @MainActor
    final class Coordinator {
        var sync: VMClipboardSync?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> VZVirtualMachineView {
        let view = VZVirtualMachineView()
        view.capturesSystemKeys = true
        view.virtualMachine = machine
        // The Mac's clipboard follows into the guest while this window is in front; the view has no window until it is shown.
        if let clipboard {
            let coordinator = context.coordinator
            Task { @MainActor in
                if let window = view.window { coordinator.sync = VMClipboardSync(bridge: clipboard, window: window) }
            }
        }
        return view
    }

    static func dismantleNSView(_ view: VZVirtualMachineView, coordinator: Coordinator) {
        coordinator.sync?.invalidate()
    }

    func updateNSView(_ view: VZVirtualMachineView, context: Context) {
        if view.virtualMachine !== machine {
            view.virtualMachine = machine
        }
    }
}
