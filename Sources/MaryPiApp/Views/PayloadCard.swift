import MaryPiKit
import SwiftUI

struct PayloadCard: View {
    @Environment(AppModel.self) private var model

    private func badgeColor(_ level: PayloadLevel) -> Color {
        switch level {
        case .bootstrapOnly: return .gray
        case .kernelBringUp: return .orange
        case .fullSystem: return .green
        }
    }

    var body: some View {
        let report = model.payload
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                if let report {
                    Text(report.level.shortName)
                        .font(.caption.bold())
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(badgeColor(report.level), in: Capsule())
                        .foregroundStyle(.white)
                    Text(report.level.title)
                        .font(.title3.bold())
                } else {
                    Text("Scanning ravynOS build tree…")
                        .font(.title3.bold())
                }
                Spacer()
                Button("Rescan") { model.rescanPayload() }
                Button("Doctor…") { model.showDoctor = true }
                Button("Choose ravynOS Checkout…") { model.chooseRavynOSRoot() }
            }
            if let report {
                Text(report.honestSummary)
                    .font(.body)
                    .fixedSize(horizontal: false, vertical: true)
                DisclosureGroup("What the Pi will do") {
                    VStack(alignment: .leading, spacing: 3) {
                        ForEach(report.whatThePiWillDo, id: \.self) { line in
                            Text("• \(line)")
                        }
                    }
                    .font(.callout)
                    .padding(.top, 4)
                }
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 2) {
                    pathRow("ravynOS checkout", report.ravynosRoot, note: model.tree.source.description)
                    pathRow("Build directory", report.buildDir)
                    pathRow("Booter", report.booter)
                    pathRow("Kernel", report.kernel)
                    if report.level >= .fullSystem {
                        pathRow("Kernelcache", report.kernelcache)
                        pathRow("Sysroot", report.sysroot)
                    }
                }
                .font(.caption)
                if !report.findings.isEmpty {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(report.findings) { finding in
                            HStack(alignment: .top, spacing: 6) {
                                Image(systemName: symbol(for: finding.severity))
                                    .foregroundStyle(color(for: finding.severity))
                                Text(finding.message)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }
                    .font(.caption)
                }
            }
            if let error = model.lastError {
                Label(error, systemImage: "exclamationmark.triangle")
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        }
    }

    @ViewBuilder
    private func pathRow(_ label: String, _ value: String?, note: String? = nil) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary)
            HStack(spacing: 6) {
                Text(value ?? "—")
                    .textSelection(.enabled)
                    .lineLimit(1)
                    .truncationMode(.middle)
                if let note {
                    Text("(\(note))").foregroundStyle(.tertiary)
                }
            }
        }
    }

    private func symbol(for severity: Finding.Severity) -> String {
        switch severity {
        case .info: return "info.circle"
        case .warning: return "exclamationmark.triangle"
        case .blocking: return "xmark.octagon"
        }
    }

    private func color(for severity: Finding.Severity) -> Color {
        switch severity {
        case .info: return .secondary
        case .warning: return .orange
        case .blocking: return .red
        }
    }
}
