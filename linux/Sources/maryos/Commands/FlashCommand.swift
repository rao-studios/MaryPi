import ArgumentParser
import Foundation
import MaryOSKit

struct FlashCommand: ParsableCommand {
    static let configuration = CommandConfiguration(commandName: "flash", abstract: "Erase a removable disk and write the MaryOS Raspberry Pi 5 image to it.")

    @OptionGroup var kit: KitOptions

    @Option(name: .long, help: "Whole-disk identifier to erase, e.g. disk4 (see `maryos list`).")
    var disk: String

    @Option(name: .long, help: "Image to write (default: the built pi5 image in out/).")
    var image: String?

    @Option(name: .long, help: "Which built image to write when --image is not given (default pi5).")
    var target: ImageTarget = .pi5

    @Flag(name: .long, help: "Build the image first, even when one exists.")
    var build = false

    @Flag(name: .long, help: "Skip the interactive confirmation.")
    var yes = false

    func run() throws {
        let name = DiskUtil.normalize(disk)
        guard DiskUtil.isValidDiskIdentifier(name) else {
            throw ValidationError("--disk must be a whole-disk identifier like disk4")
        }
        let paths = try kit.paths()
        let config = try kit.config(paths)
        let diskInfo = try runBlocking { try await DiskUtil().info(name) }
        if let reason = DiskFilter.rejectionReason(diskInfo) {
            throw ValidationError("/dev/\(name) (\(diskInfo.displayName)) is not a valid target: \(reason)")
        }
        let artifacts = BuildArtifacts.locate(target: target, config: config, paths: paths)
        let source = image.map { URL(fileURLWithPath: ($0 as NSString).expandingTildeInPath) }
        let willBuild = source == nil && (build || !artifacts.isComplete)

        Output.line("About to ERASE /dev/\(diskInfo.bsdName): \(diskInfo.displayName), \(diskInfo.sizeDescription), \(diskInfo.busProtocol ?? "unknown bus")")
        Output.line(willBuild ? "The \(target.rawValue) image will be built first (\(config.imageName(for: target)))." : "Image: \((source ?? artifacts.image).path)")
        if !yes {
            Output.line("Type the disk identifier (\(diskInfo.bsdName)) to continue, anything else to abort:")
            guard let typed = readLine()?.trimmingCharacters(in: .whitespacesAndNewlines), typed == diskInfo.bsdName else {
                Output.line("Aborted; nothing was written.")
                throw ExitCode(2)
            }
        }

        if let source {
            try runBlocking {
                try await Flasher().flash(image: source, target: diskInfo, log: Output.stdoutLogger, progress: { progress in
                    print("  \(Int(progress.fraction * 100))% (\(ByteCountFormatter.string(fromByteCount: progress.bytesWritten, countStyle: .file)))")
                    fflush(stdout)
                })
            }
            Output.line("Done.")
            return
        }

        let outcome: (StepPlan, PreparePhase) = try runBlocking {
            let coordinator = PrepareCoordinator(logSink: Output.stdoutLogger)
            await coordinator.prepare(target: target, disk: diskInfo, paths: paths, config: config, options: PrepareOptions(build: willBuild))
            return (coordinator.plan, coordinator.phase)
        }
        Output.line()
        Output.printPlan(outcome.0)
        switch outcome.1 {
        case .finished(let imageURL, _):
            Output.line()
            Output.line("Card written from \(imageURL.path). Insert it into the Raspberry Pi 5.")
        case .failed(let message):
            throw ValidationError(message)
        default:
            throw ExitCode.failure
        }
    }
}
