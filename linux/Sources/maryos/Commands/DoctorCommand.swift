import ArgumentParser
import Foundation
import MaryOSKit

struct DoctorCommand: ParsableCommand {
    static let configuration = CommandConfiguration(commandName: "doctor", abstract: "Check this Mac: tools, the kit, Docker, Virtualization.framework and the entitlement.")

    @OptionGroup var kit: KitOptions

    @Flag(name: .long, help: "Print JSON.")
    var json = false

    func run() throws {
        let paths = KitPaths.locate(explicitRoot: kit.kit)
        let checks = try runBlocking { await Doctor().run(paths: paths) }
        if json {
            struct Row: Encodable { let name: String; let passed: Bool; let blocking: Bool; let detail: String }
            try Output.json(checks.map { Row(name: $0.name, passed: $0.passed, blocking: $0.blocking, detail: $0.detail) })
            return
        }
        Output.printChecks(checks)
        if checks.contains(where: \.isBlockingFailure) {
            throw ExitCode(1)
        }
    }
}
