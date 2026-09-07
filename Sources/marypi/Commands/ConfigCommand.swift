import ArgumentParser
import Foundation
import MaryPiKit

struct ConfigCommand: AsyncParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "config",
        abstract: "Read or write MaryPi's saved settings.",
        subcommands: [Get.self, Set.self, Clear.self],
        defaultSubcommand: Get.self
    )

    static let keys = ["ravynos", "build-dir"]

    struct Get: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "get", abstract: "Show the saved settings and how the checkout is currently resolved.")

        func run() async throws {
            let settings = Settings.load()
            Output.line("Settings file: \(Settings.defaultURL().path)")
            Output.line("ravynos   = \(settings.ravynosRoot ?? "<unset>")")
            Output.line("build-dir = \(settings.buildDir ?? "<unset>")")
            let tree = BuildTree.locate()
            Output.line()
            Output.line("Resolved checkout: \(tree.ravynosRoot?.path ?? "none") (\(tree.source.description))")
            Output.line("Resolved build dir: \(tree.buildDir?.path ?? "none")")
        }
    }

    struct Set: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "set", abstract: "Save a setting: ravynos <path> | build-dir <path>.")

        @Argument(help: "One of: \(ConfigCommand.keys.joined(separator: ", "))")
        var key: String

        @Argument(help: "The value to save.")
        var value: String

        func run() async throws {
            var settings = Settings.load()
            let expanded = (value as NSString).expandingTildeInPath
            switch key {
            case "ravynos":
                let url = URL(fileURLWithPath: expanded)
                if !BuildTree.isRavynOSCheckout(url) {
                    Output.line("Warning: \(expanded) has no Kernel/xnu; saving anyway.")
                }
                settings.ravynosRoot = expanded
            case "build-dir":
                settings.buildDir = expanded
            default:
                throw ValidationError("Unknown key \(key); expected one of \(ConfigCommand.keys.joined(separator: ", "))")
            }
            try settings.save()
            Output.line("Saved \(key) = \(expanded) to \(Settings.defaultURL().path)")
        }
    }

    struct Clear: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "clear", abstract: "Forget all saved settings.")

        func run() async throws {
            try Settings().save()
            Output.line("Cleared \(Settings.defaultURL().path)")
        }
    }
}
