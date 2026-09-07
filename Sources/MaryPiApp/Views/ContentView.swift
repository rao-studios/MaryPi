import MaryPiKit
import SwiftUI

struct ContentView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        HSplitView {
            DiskListView()
                .frame(minWidth: 280, idealWidth: 320, maxWidth: 420)
            VStack(spacing: 0) {
                PayloadCard()
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
            Button("Build Image Only") {
                Task { await model.buildImageOnly() }
            }
            .disabled(!model.canBuildImage)
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
