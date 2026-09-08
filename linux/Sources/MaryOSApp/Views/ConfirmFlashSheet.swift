import MaryOSKit
import SwiftUI

struct ConfirmFlashSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var typed = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            if let disk = model.selectedDisk, let config = model.config {
                Text("Erase this disk and write \(config.fullName) for Raspberry Pi 5?")
                    .font(.title3.bold())
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 4) {
                    GridRow { Text("Disk").foregroundStyle(.secondary); Text("/dev/\(disk.bsdName)").bold() }
                    GridRow { Text("Name").foregroundStyle(.secondary); Text(disk.displayName) }
                    GridRow { Text("Size").foregroundStyle(.secondary); Text(disk.sizeDescription) }
                    GridRow { Text("Bus").foregroundStyle(.secondary); Text(disk.busProtocol ?? "unknown") }
                    GridRow { Text("Image").foregroundStyle(.secondary); Text(imageDescription(config)) }
                }
                Text("The card boots to a login prompt on HDMI and on the 3-pin UART; log in as \(config.defaultUser). The root filesystem grows to the whole card on first boot.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text("Everything on \(disk.displayName) will be destroyed. Type \(disk.bsdName) to enable the button. You will be asked for an administrator password once.")
                    .font(.callout)
                    .fixedSize(horizontal: false, vertical: true)
                TextField(disk.bsdName, text: $typed)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(.body, design: .monospaced))
                HStack {
                    Spacer()
                    Button("Cancel") { dismiss() }
                        .keyboardShortcut(.cancelAction)
                    Button("Erase and Write") {
                        dismiss()
                        Task { await model.prepareSelected() }
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.red)
                    .disabled(typed.trimmingCharacters(in: .whitespaces) != disk.bsdName)
                }
            } else {
                Text("No disk selected.")
                Button("Close") { dismiss() }
            }
        }
        .padding(20)
        .frame(width: 520)
    }

    private func imageDescription(_ config: DistroConfig) -> String {
        guard let artifacts = model.artifacts[.pi5], artifacts.isComplete, let size = artifacts.imageSize else {
            return "\(config.imageName(for: .pi5)) will be built first (Docker, several minutes)"
        }
        let when = artifacts.imageDate.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "?"
        return "\(artifacts.image.lastPathComponent) (\(ByteCountFormatter.string(fromByteCount: size, countStyle: .file)), built \(when))"
    }
}
