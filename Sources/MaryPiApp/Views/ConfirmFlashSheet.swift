import MaryPiKit
import SwiftUI

struct ConfirmFlashSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var typed = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            if let disk = model.selectedDisk, let report = model.payload {
                Text("Erase this disk and write ravynOS for Raspberry Pi 5?")
                    .font(.title3.bold())
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 4) {
                    GridRow { Text("Disk").foregroundStyle(.secondary); Text("/dev/\(disk.bsdName)").bold() }
                    GridRow { Text("Name").foregroundStyle(.secondary); Text(disk.displayName) }
                    GridRow { Text("Size").foregroundStyle(.secondary); Text(disk.sizeDescription) }
                    GridRow { Text("Bus").foregroundStyle(.secondary); Text(disk.busProtocol ?? "unknown") }
                    GridRow { Text("Payload").foregroundStyle(.secondary); Text("\(report.level.shortName): \(report.level.title)") }
                }
                Text(report.honestSummary)
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
}
