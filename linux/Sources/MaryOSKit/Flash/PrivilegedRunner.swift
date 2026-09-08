import Foundation

/// Runs one shell script with administrator privileges through the standard
/// macOS authorization dialog. Exactly one password prompt per call.
public struct PrivilegedRunner: Sendable {
    public static let osascript = "/usr/bin/osascript"
    public static let userCancelledCode = "-128"

    private let runner: CommandRunner

    public init(runner: CommandRunner = CommandRunner()) {
        self.runner = runner
    }

    static func appleScriptLiteral(_ value: String) -> String {
        "\"" + value.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }

    /// The AppleScript source used for a script path and prompt.
    public static func source(script: URL, prompt: String) -> String {
        "do shell script \"/bin/sh \" & quoted form of \(appleScriptLiteral(script.path)) with prompt \(appleScriptLiteral(prompt)) with administrator privileges"
    }

    public func run(script: URL, prompt: String = "MaryOS needs administrator rights to write the Raspberry Pi card.") async throws -> CommandResult {
        let result = try await runner.run(Self.osascript, ["-e", Self.source(script: script, prompt: prompt)])
        if !result.succeeded, result.stderrText.contains(Self.userCancelledCode) {
            throw MaryOSError("Administrator authorization was cancelled; the card was not touched.")
        }
        return result
    }
}
