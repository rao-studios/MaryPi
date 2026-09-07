import ArgumentParser
import Foundation
import MaryPiKit

@main
struct MaryPiCLI: AsyncParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "marypi",
        abstract: "Prepare a Raspberry Pi 5 card with ravynOS from this Mac.",
        version: MaryPi.version,
        subcommands: [
            ListCommand.self, PayloadCommand.self, FirmwareCommand.self,
            BuildImageCommand.self, FlashCommand.self, DoctorCommand.self, ConfigCommand.self,
        ]
    )
}

/// Options shared by every subcommand.
struct GlobalOptions: ParsableArguments {
    @Option(name: .long, help: "Path to the ravynOS checkout (overrides MARYPI_RAVYNOS_ROOT and the saved setting).")
    var ravynos: String?

    func tree() -> BuildTree {
        BuildTree.locate(explicitRoot: ravynos)
    }
}

enum Output {
    static func line(_ text: String = "") {
        print(text)
    }

    static func json<T: Encodable>(_ value: T) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        print(String(decoding: try encoder.encode(value), as: UTF8.self))
    }

    static let stdoutLogger: Logger = { line in
        print(line)
        fflush(stdout)
    }

    static func printPlan(_ plan: FlashPlan) {
        for step in plan.steps {
            let glyph: String
            switch step.status {
            case .pending: glyph = " "
            case .running: glyph = ">"
            case .done: glyph = "x"
            case .skipped: glyph = "-"
            case .failed: glyph = "!"
            }
            var suffix = ""
            switch step.status {
            case .skipped(let why), .failed(let why): suffix = "  (\(why))"
            default: break
            }
            line("  [\(glyph)] \(step.title)\(suffix)")
        }
    }
}
