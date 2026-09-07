import ArgumentParser
import Foundation
import MaryPiKit

struct DoctorCommand: AsyncParsableCommand {
    static let configuration = CommandConfiguration(commandName: "doctor", abstract: "Check this Mac, the ravynOS build tree and the firmware cache.")

    @OptionGroup var global: GlobalOptions

    func run() async throws {
        let manifest = try? FirmwareManifest.bundled()
        let checks = await Doctor().run(tree: global.tree(), manifest: manifest, cache: FirmwareCache())
        var blocking = 0
        for check in checks {
            let glyph = check.passed ? "ok  " : (check.blocking ? "FAIL" : "warn")
            Output.line("[\(glyph)] \(check.name): \(check.detail)")
            if check.isBlockingFailure { blocking += 1 }
        }
        Output.line()
        if blocking > 0 {
            Output.line("\(blocking) blocking problem(s). MaryPi cannot flash until they are fixed.")
            throw ExitCode.failure
        }
        Output.line("No blocking problems. Level 0 (bootstrap) flashing will work; see `marypi payload` for kernel payloads.")
    }
}
