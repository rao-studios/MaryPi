import ArgumentParser
import Foundation
import MaryPiKit

struct PayloadCommand: AsyncParsableCommand {
    static let configuration = CommandConfiguration(commandName: "payload", abstract: "Report what MaryPi can put on a card right now.")

    @OptionGroup var global: GlobalOptions

    @Flag(name: .long, help: "Print JSON.")
    var json = false

    func run() async throws {
        let manifest = try FirmwareManifest.bundled()
        let tree = global.tree()
        let report = PayloadResolver().evaluate(tree, firmwareVersion: manifest.rpi5UEFI?.version ?? "?")
        if json {
            try Output.json(report)
            return
        }
        Output.line("\(report.level.shortName): \(report.level.title)")
        Output.line()
        Output.line(report.honestSummary)
        Output.line()
        Output.line("What the Pi will do:")
        for line in report.whatThePiWillDo { Output.line("  - \(line)") }
        Output.line()
        Output.line("ravynOS checkout: \(report.ravynosRoot ?? "not configured") (\(tree.source.description))")
        Output.line("Build directory:  \(report.buildDir ?? "-")")
        Output.line("Booter:           \(report.booter ?? "-")")
        Output.line("Kernel:           \(report.kernel ?? "-")")
        Output.line("Extensions:       \(report.extensions.map { "\($0) (\(report.extensionNames.joined(separator: ", ")))" } ?? "-")")
        Output.line("Init program:     \(report.initProgram ?? "-")")
        Output.line("Kernelcache:      \(report.kernelcache ?? "-")")
        Output.line("Sysroot:          \(report.sysroot ?? "-")")
        if !report.findings.isEmpty {
            Output.line()
            Output.line("Findings:")
            for finding in report.findings { Output.line("  \(finding.severity.rawValue): \(finding.message)") }
        }
    }
}
