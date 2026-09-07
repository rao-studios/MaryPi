import MaryPiKit
import SwiftUI

struct DoctorView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Doctor")
                    .font(.title3.bold())
                Spacer()
                Button("Re-run") { Task { await model.runDoctor() } }
            }
            List(model.doctorChecks) { check in
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: check.passed ? "checkmark.circle.fill" : (check.blocking ? "xmark.circle.fill" : "exclamationmark.triangle.fill"))
                        .foregroundStyle(check.passed ? .green : (check.blocking ? .red : .orange))
                    VStack(alignment: .leading, spacing: 2) {
                        Text(check.name)
                        Text(check.detail)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            .frame(minHeight: 320)
            Text(model.hasBlockingDoctorFailure
                 ? "Blocking problems must be fixed before MaryPi can write a card."
                 : "No blocking problems. Level 0 (bootstrap) cards can be written now.")
                .font(.callout)
                .foregroundStyle(model.hasBlockingDoctorFailure ? .red : .secondary)
            HStack {
                Spacer()
                Button("Close") { dismiss() }
                    .keyboardShortcut(.cancelAction)
            }
        }
        .padding(20)
        .frame(width: 640, height: 520)
    }
}
