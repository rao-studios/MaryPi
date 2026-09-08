#if os(macOS)
import ArgumentParser
import Foundation
import MaryPiKit

struct BuildImageCommand: AsyncParsableCommand {
    static let configuration = CommandConfiguration(commandName: "build-image", abstract: "Build a raw disk image without writing it anywhere.")

    @OptionGroup var global: GlobalOptions

    @Option(name: .long, help: "Where to write the .img (default: ~/Library/Caches/MaryPi/images/).")
    var out: String?

    @Flag(name: .long, help: "Build for QEMU virt instead of a Raspberry Pi 5 (no rd= flag).")
    var qemu = false

    @Option(name: .long, help: "Override the kernel flags written to com.ravynos.boot.plist.")
    var kernelFlags: String?

    func run() async throws {
        let coordinator = await PrepareCoordinator(logSink: Output.stdoutLogger)
        let options = PrepareOptions(
            qemuVirt: qemu,
            outputURL: out.map { URL(fileURLWithPath: ($0 as NSString).expandingTildeInPath) },
            kernelFlags: kernelFlags
        )
        await coordinator.prepare(target: nil, tree: global.tree(), options: options)
        let phase = await coordinator.phase
        Output.line()
        Output.printPlan(await coordinator.plan)
        switch phase {
        case .finished(let image, _):
            Output.line()
            Output.line("Image: \(image.path)")
        case .failed(let message):
            throw ExitCode.failure.withMessage(message)
        default:
            throw ExitCode.failure
        }
    }
}

extension ExitCode {
    func withMessage(_ message: String) -> Error {
        ValidationError(message)
    }
}
#endif
