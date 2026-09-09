import ArgumentParser
import Foundation
import MaryOSKit

struct BuildCommand: ParsableCommand {
    static let configuration = CommandConfiguration(commandName: "build", abstract: "Build MaryOS images from source (Ubuntu's archive + distro/) with the Docker builder.")

    @OptionGroup var kit: KitOptions

    @Option(name: .long, help: "pi5, vm or both (default vm).")
    var target: String = "vm"

    @Option(name: .long, help: "rootfs, ui (compile the desktop from MaryUI), target, image or all (default all).")
    var stage: String = "all"

    @Flag(name: .long, help: "Rebuild the base rootfs even when the cache matches distro/.")
    var fresh = false

    @Flag(name: .long, help: "Keep the rootfs tree in the builder's work volume after the image.")
    var keep = false

    @Flag(name: .long, help: "Print the stages the builder would run.")
    var dryRun = false

    func run() throws {
        let paths = try kit.paths()
        let config = try kit.config(paths)
        let targets: [ImageTarget]
        switch target {
        case "both", "all": targets = ImageTarget.allCases
        default:
            guard let one = ImageTarget(rawValue: target) else { throw ValidationError("--target must be pi5, vm or both") }
            targets = [one]
        }
        guard ["rootfs", "ui", "target", "image", "all"].contains(stage) else { throw ValidationError("--stage must be rootfs, ui, target, image or all") }
        let builder = BuildRunner(paths: paths)
        for one in targets {
            Output.line(stage == "ui" ? "maryos: compiling the desktop from \(paths.maryUISource.path) with \(paths.buildScript.path)"
                        : "maryos: building \(config.imageName(for: one)) (\(stage)) with \(paths.buildScript.path)")
            do {
                try runBlocking {
                    try await builder.build(target: one, stage: stage, fresh: fresh, keep: keep, dryRun: dryRun, log: Output.stdoutLogger)
                }
            } catch {
                throw ValidationError(error.localizedDescription)
            }
            if BuildRunner.targetlessStages.contains(stage) { break }
        }
        if !dryRun && (stage == "all" || stage == "ui") {
            let ui = UIArtifacts.locate(paths: paths)
            Output.line(ui.isBuilt ? "maryos: \(ui.binary.path) (\(ui.summary))" : "maryos: warning, \(ui.binary.path) is missing after the build")
        }
        if !dryRun && (stage == "all" || stage == "image") {
            for one in targets {
                let artifacts = BuildArtifacts.locate(target: one, config: config, paths: paths)
                Output.line(artifacts.isComplete ? "maryos: \(artifacts.image.path)" : "maryos: warning, \(artifacts.image.path) is missing after the build")
            }
        }
    }
}
