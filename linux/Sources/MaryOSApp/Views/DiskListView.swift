import MaryOSKit
import SwiftUI

struct DiskListView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        VStack(spacing: 0) {
            HStack {
                Text("Removable disks")
                    .font(.headline)
                Spacer()
                if model.isScanningDisks {
                    ProgressView().controlSize(.small)
                }
                Button {
                    Task { await model.refreshDisks() }
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .help("Refresh (⌘R)")
            }
            .padding(.horizontal)
            .padding(.vertical, 10)
            Divider()
            if model.disks.isEmpty {
                ContentUnavailableView {
                    Label("No removable disks", systemImage: "sdcard")
                } description: {
                    Text("Insert an SD card or USB drive between 4 GB and 2 TB. Internal disks are never shown.")
                }
            } else {
                List(model.disks, selection: $model.selectedDiskID) { disk in
                    DiskRow(disk: disk)
                        .tag(disk.bsdName)
                }
                .listStyle(.inset)
            }
            if let event = model.lastDiskEvent {
                Divider()
                Text(event)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding(8)
            }
        }
    }
}

struct DiskRow: View {
    let disk: DiskInfo

    private var symbol: String {
        switch disk.busKind {
        case .sdCard: return "sdcard"
        case .usb: return "externaldrive.connected.to.line.below"
        case .thunderbolt: return "bolt.horizontal"
        case .other: return "externaldrive"
        }
    }

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: symbol)
                .font(.title2)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(disk.displayName)
                    .font(.body)
                Text("\(disk.sizeDescription) · \(disk.busProtocol ?? "unknown bus") · /dev/\(disk.bsdName)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 4)
    }
}
