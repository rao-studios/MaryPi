import ArgumentParser
import Foundation
import MaryOSKit

struct ConfigCommand: ParsableCommand {
    static let configuration = CommandConfiguration(commandName: "config", abstract: "Show the MaryOS definition (distro/distro.conf), the kit's directories and the built artifacts.")

    @OptionGroup var kit: KitOptions

    @Flag(name: .long, help: "Print JSON.")
    var json = false

    func run() throws {
        let paths = try kit.paths()
        let config = try kit.config(paths)
        if json {
            struct Report: Encodable {
                let kit: String; let checkout: Bool; let out: String; let state: String; let distro: DistroConfig
            }
            try Output.json(Report(kit: paths.root.path, checkout: paths.isCheckout, out: paths.outDirectory.path, state: paths.stateRoot.path, distro: config))
            return
        }
        Output.line("\(config.fullName) (\(config.id) \(config.codename)), Ubuntu \(config.baseSuite) \(config.arch) from \(config.baseMirror)")
        Output.line("kit:      \(paths.root.path)\(paths.isCheckout ? " (checkout)" : "")")
        Output.line("distro:   \(paths.distroConfig.path)")
        Output.line("builder:  \(paths.buildScript.path)")
        Output.line("out:      \(paths.outDirectory.path)")
        Output.line("state:    \(paths.stateRoot.path)")
        Output.line("user:     \(config.defaultUser) (password in distro.conf), hostname \(config.hostname)")
        Output.line("layout:   MBR; p1 FAT32 \"\(config.bootLabel)\" \(config.bootPartitionMiB) MiB; p2 ext4 \"\(config.rootLabel)\" (>= \(config.rootMinMiB) MiB, grows on first boot)")
        for target in ImageTarget.allCases {
            let artifacts = BuildArtifacts.locate(target: target, config: config, paths: paths)
            if artifacts.isComplete, let size = artifacts.imageSize {
                let when = artifacts.imageDate.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "?"
                Output.line("\(target.rawValue):      \(artifacts.image.path) (\(ByteCountFormatter.string(fromByteCount: size, countStyle: .file)), built \(when))")
            } else {
                Output.line("\(target.rawValue):      not built (maryos build --target \(target.rawValue))")
            }
        }
    }
}
