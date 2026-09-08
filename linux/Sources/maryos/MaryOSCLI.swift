import ArgumentParser
import Foundation
import MaryOSKit

/// The commands are synchronous on purpose. Virtualization.framework
/// delivers its callbacks on the main dispatch queue and the VM window needs
/// AppKit's run loop; both only work when the real main thread drives the
/// run loop at top level, not from inside an async main's dispatch block.
/// `runBlocking` (Async.swift) bridges to async work.
@main
struct MaryOSCLI: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "maryos",
        abstract: "Build MaryOS (an Ubuntu 24.04 arm64 fork) from source, boot it in a Virtualization.framework window, and write it to a Raspberry Pi 5 card.",
        version: MaryOS.version,
        subcommands: [DoctorCommand.self, ConfigCommand.self, BuildCommand.self, ListCommand.self, FlashCommand.self, VMCommand.self]
    )
}

/// Options shared by every subcommand.
struct KitOptions: ParsableArguments {
    @Option(name: .long, help: "The kit directory (linux/, with distro/ and builder/). Default: found from the current directory, \(KitPaths.environmentKey), or this checkout.")
    var kit: String?

    func paths() throws -> KitPaths {
        guard let paths = KitPaths.locate(explicitRoot: kit) else {
            throw ValidationError("kit directory not found: run from the checkout's linux/ directory, pass --kit, or set \(KitPaths.environmentKey)")
        }
        return paths
    }

    func config(_ paths: KitPaths) throws -> DistroConfig {
        do {
            return try paths.loadConfig()
        } catch {
            throw ValidationError(error.localizedDescription)
        }
    }
}

extension ImageTarget: ExpressibleByArgument {}

enum Output {
    static func line(_ text: String = "") {
        print(text)
        fflush(stdout)
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

    static func printChecks(_ checks: [DoctorCheck]) {
        for check in checks {
            let tag = check.passed ? "ok  " : (check.blocking ? "FAIL" : "warn")
            line("[\(tag)] \(check.name): \(check.detail)")
        }
    }

    static func printPlan(_ plan: StepPlan) {
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
