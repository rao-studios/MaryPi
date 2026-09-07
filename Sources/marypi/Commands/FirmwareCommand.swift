import ArgumentParser
import Foundation
import MaryPiKit

struct FirmwareCommand: AsyncParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "firmware",
        abstract: "Manage the pinned Raspberry Pi 5 UEFI firmware.",
        subcommands: [Fetch.self, Status.self],
        defaultSubcommand: Status.self
    )

    struct Fetch: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "fetch", abstract: "Download, verify and extract the pinned firmware.")

        @Flag(name: .long, help: "Re-download even if the cache is already verified.")
        var force = false

        func run() async throws {
            let manifest = try FirmwareManifest.bundled()
            try manifest.validate()
            guard let source = manifest.rpi5UEFI else { throw MaryPiError("Manifest has no rpi5-uefi source") }
            let cache = FirmwareCache()
            let bundle = try await cache.ensure(source, force: force, log: Output.stdoutLogger)
            Output.line("Ready: \(bundle.directory.path)")
        }
    }

    struct Status: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "status", abstract: "Show the pinned firmware and cache state.")

        func run() async throws {
            let manifest = try FirmwareManifest.bundled()
            let cache = FirmwareCache()
            Output.line("Manifest schema \(manifest.schema), updated \(manifest.updated)")
            for source in manifest.sources {
                Output.line("- \(source.id) \(source.version): \(source.url)")
                Output.line("  sha256 \(source.sha256), \(source.sizeBytes) bytes, files \(source.files.joined(separator: ", "))")
                Output.line("  \(await cache.status(source))")
            }
        }
    }
}
