import MaryOSKit
import SwiftUI

struct ContentView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        @Bindable var model = model
        HSplitView {
            DiskListView()
                .frame(minWidth: 280, idealWidth: 320, maxWidth: 420)
            VStack(spacing: 0) {
                DistroCard()
                    .padding()
                Divider()
                HSplitView {
                    StepListView()
                        .frame(minWidth: 300, idealWidth: 340)
                    LogView()
                        .frame(minWidth: 320)
                }
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            FooterBar()
        }
        .sheet(isPresented: $model.showConfirm) {
            ConfirmFlashSheet()
        }
        .sheet(isPresented: $model.showDoctor) {
            DoctorView()
        }
        .onAppear {
            model.openVMWindow = { openWindow(id: "vm") }
        }
        .frame(minWidth: 1000, minHeight: 620)
    }
}

struct FooterBar: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        HStack(spacing: 12) {
            if model.coordinator.isRunning {
                ProgressView()
                    .controlSize(.small)
            }
            Text(model.statusLine)
                .font(.callout)
                .lineLimit(2)
                .foregroundStyle(.secondary)
            Spacer()
            if case .finished = model.coordinator.phase {
                Button("Reveal Image") { model.revealImage() }
            }
            if model.vmIsRunning {
                Button("Stop VM") {
                    Task { await model.stopVM() }
                }
            } else {
                Button("Test in VM") {
                    Task { await model.testInVM() }
                }
                .help(model.vmImageBuilt ? "Boot the built VM image in a Virtualization.framework window" : "Build the VM image with Docker (several minutes), then boot it in a window")
                .disabled(!model.canTestInVM)
            }
            Button("Build Pi 5 Image") {
                Task { await model.buildImage(.pi5) }
            }
            .help("Build the Raspberry Pi 5 image from source without writing any disk")
            .disabled(!model.canBuild)
            Button("Prepare Raspberry Pi 5") {
                model.showConfirm = true
            }
            .keyboardShortcut(.defaultAction)
            .buttonStyle(.borderedProminent)
            .disabled(!model.canPrepare)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(.bar)
    }
}
