#if os(macOS)
import ArgumentParser
import Foundation
import MaryPiKit

struct FlashCommand: AsyncParsableCommand {
    static let configuration = CommandConfiguration(commandName: "flash", abstract: "Erase a removable disk and write a ravynOS Raspberry Pi 5 image to it.")

    @OptionGroup var global: GlobalOptions

    @Option(name: .long, help: "Whole-disk identifier to erase, e.g. disk4 (see `marypi list`).")
    var disk: String

    @Option(name: .long, help: "Existing image to write. Without it a fresh image is built first.")
    var image: String?

    @Flag(name: .long, help: "Skip the interactive confirmation.")
    var yes = false

    @Flag(name: .long, help: "Build a QEMU virt image instead of a Raspberry Pi 5 image.")
    var qemu = false

    func run() async throws {
        let name = DiskUtil.normalize(disk)
        guard DiskUtil.isValidDiskIdentifier(name) else {
            throw ValidationError("--disk must be a whole-disk identifier like disk4")
        }
        let diskUtil = DiskUtil()
        let target = try await diskUtil.info(name)
        if let reason = DiskFilter.rejectionReason(target) {
            throw ValidationError("/dev/\(name) (\(target.displayName)) is not a valid target: \(reason)")
        }

        Output.line("About to ERASE /dev/\(target.bsdName): \(target.displayName), \(target.sizeDescription), \(target.busProtocol ?? "unknown bus")")
        if !yes {
            Output.line("Type the disk identifier (\(target.bsdName)) to continue, anything else to abort:")
            guard let typed = readLine()?.trimmingCharacters(in: .whitespacesAndNewlines), typed == target.bsdName else {
                Output.line("Aborted; nothing was written.")
                throw ExitCode(2)
            }
        }

        let coordinator = await PrepareCoordinator(logSink: Output.stdoutLogger)
        if let image {
            let url = URL(fileURLWithPath: (image as NSString).expandingTildeInPath)
            try await Flasher().flash(image: url, target: target, log: Output.stdoutLogger, progress: { progress in
                let percent = Int(progress.fraction * 100)
                print("  \(percent)% (\(ByteCountFormatter.string(fromByteCount: progress.bytesWritten, countStyle: .file)))")
                fflush(stdout)
            })
            Output.line("Done.")
            return
        }

        await coordinator.prepare(target: target, tree: global.tree(), options: PrepareOptions(qemuVirt: qemu))
        Output.line()
        Output.printPlan(await coordinator.plan)
        switch await coordinator.phase {
        case .finished(let imageURL, let flashed):
            Output.line()
            Output.line(flashed ? "Card written from \(imageURL.path). Insert it into the Raspberry Pi 5." : "Image built at \(imageURL.path)")
        case .failed(let message):
            throw ValidationError(message)
        default:
            throw ExitCode.failure
        }
    }
}
#endif
