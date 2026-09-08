import MaryOSKit
import SwiftUI

/// What MaryOS is (from distro.conf), where the kit lives, and which images
/// have been built.
struct DistroCard: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text(model.config?.fullName ?? "MaryOS")
                    .font(.title2.bold())
                if let config = model.config {
                    Text("Ubuntu \(config.baseSuite) \(config.arch) fork, built from source")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if let paths = model.paths {
                    Text(paths.root.path)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
            }
            if let error = model.configError {
                Label(error, systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.red)
                    .font(.callout)
            }
            if let config = model.config {
                Text("First user \(config.defaultUser) (password in distro.conf), hostname \(config.hostname). Partition 1 FAT32 \"\(config.bootLabel)\" \(config.bootPartitionMiB) MiB, partition 2 ext4 \"\(config.rootLabel)\", grown to the card or disk on first boot.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
                    ForEach(ImageTarget.allCases, id: \.self) { target in
                        GridRow {
                            Label(target.title, systemImage: target == .pi5 ? "sdcard" : "desktopcomputer")
                                .frame(width: 150, alignment: .leading)
                            Text(status(for: target))
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                            Button(model.artifacts[target]?.isComplete == true ? "Rebuild" : "Build") {
                                Task { await model.buildImage(target) }
                            }
                            .controlSize(.small)
                            .disabled(!model.canBuild)
                        }
                    }
                }
                .font(.callout)
            }
        }
        .padding(14)
        .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 10))
    }

    private func status(for target: ImageTarget) -> String {
        guard let artifacts = model.artifacts[target] else { return "kit not found" }
        guard artifacts.isComplete, let size = artifacts.imageSize else {
            return "not built yet (\(artifacts.image.lastPathComponent))"
        }
        let when = artifacts.imageDate.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "?"
        return "\(artifacts.image.lastPathComponent), \(ByteCountFormatter.string(fromByteCount: size, countStyle: .file)), built \(when)"
    }
}
