import MaryPiKit
import SwiftUI

struct StepListView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Steps")
                .font(.headline)
                .padding(.horizontal)
                .padding(.vertical, 10)
            Divider()
            if model.coordinator.plan.steps.isEmpty {
                ContentUnavailableView {
                    Label("Nothing running", systemImage: "list.bullet.clipboard")
                } description: {
                    Text("Steps appear here when you prepare a card or build an image.")
                }
            } else {
                List(model.coordinator.plan.steps) { step in
                    StepRow(step: step)
                }
                .listStyle(.inset)
            }
        }
    }
}

struct StepRow: View {
    let step: Step

    private var symbol: (name: String, color: Color) {
        switch step.status {
        case .pending: return ("circle", .secondary)
        case .running: return ("circle.dotted", .accentColor)
        case .done: return ("checkmark.circle.fill", .green)
        case .skipped: return ("minus.circle", .secondary)
        case .failed: return ("xmark.circle.fill", .red)
        }
    }

    private var detail: String? {
        switch step.status {
        case .skipped(let why), .failed(let why): return why
        default: return step.detail.isEmpty ? nil : step.detail
        }
    }

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: symbol.name)
                .foregroundStyle(symbol.color)
                .frame(width: 18)
            VStack(alignment: .leading, spacing: 3) {
                Text(step.title)
                if let detail {
                    Text(detail)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if step.status == .running {
                    if let progress = step.progress {
                        ProgressView(value: progress)
                    } else {
                        ProgressView()
                            .controlSize(.small)
                    }
                }
            }
        }
        .padding(.vertical, 2)
    }
}
