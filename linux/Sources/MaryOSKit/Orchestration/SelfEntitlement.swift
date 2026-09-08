import Darwin
import Foundation

/// Virtualization.framework only serves processes whose executable carries
/// the `com.apple.security.virtualization` entitlement, and `swift build`
/// produces unsigned binaries. Before touching the framework, the CLI and
/// the app check their own executable and, when the entitlement is missing,
/// ad-hoc sign it in place and re-exec themselves with the same arguments.
///
/// This is safe: `codesign` writes a new file and renames it over the old
/// one, so the running process keeps its pages, and SwiftPM does not relink
/// a binary whose signature changed. `scripts/sign.sh` does the same thing
/// ahead of time for the Makefile and the app bundle.
public enum SelfEntitlement {
    public static let key = "com.apple.security.virtualization"
    /// Set in the environment of the re-executed process so a failed signing cannot loop.
    public static let restartMarker = "MARYOS_SELF_SIGNED"

    public static let entitlementsXML = """
    <?xml version="1.0" encoding="UTF-8"?>
    <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
    <plist version="1.0">
    <dict>
    \t<key>com.apple.security.virtualization</key>
    \t<true/>
    </dict>
    </plist>

    """

    /// The running executable, symlinks resolved.
    public static var executablePath: String {
        let path = Bundle.main.executablePath ?? CommandLine.arguments[0]
        return URL(fileURLWithPath: path).resolvingSymlinksInPath().path
    }

    /// Runs a command to completion and returns its exit status with the combined output.
    static func runSync(_ executable: String, _ arguments: [String]) -> (status: Int32, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        process.standardInput = FileHandle.nullDevice
        do {
            try process.run()
        } catch {
            return (-1, error.localizedDescription)
        }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return (process.terminationStatus, String(decoding: data, as: UTF8.self))
    }

    /// Whether `executable` (this process by default) carries the entitlement.
    public static func isPresent(executable: String = executablePath) -> Bool {
        runSync("/usr/bin/codesign", ["-d", "--entitlements", "-", executable]).output.contains(key)
    }

    /// Sign the running executable with the entitlement and re-exec it with
    /// the same arguments. Returns only by throwing.
    public static func signAndRestart(log: (String) -> Void) throws -> Never {
        let executable = executablePath
        if ProcessInfo.processInfo.environment[restartMarker] != nil {
            throw MaryOSError("\(executable) still lacks the \(key) entitlement after signing it; sign it by hand: codesign --force --sign - --entitlements Entitlements.plist \(executable)")
        }
        let plist = FileManager.default.temporaryDirectory.appending(path: "maryos-entitlements-\(getpid()).plist")
        try entitlementsXML.write(to: plist, atomically: true, encoding: .utf8)
        defer { try? FileManager.default.removeItem(at: plist) }
        log("signing \(executable) with the \(key) entitlement (needed once after each build)")
        let result = runSync("/usr/bin/codesign", ["--force", "--sign", "-", "--entitlements", plist.path, executable])
        guard result.status == 0 else {
            throw MaryOSError("codesign failed on \(executable): \(result.output.trimmingCharacters(in: .whitespacesAndNewlines)). Sign it by hand: codesign --force --sign - --entitlements Entitlements.plist \(executable)")
        }
        setenv(restartMarker, "1", 1)
        var argv: [UnsafeMutablePointer<CChar>?] = CommandLine.arguments.map { strdup($0) }
        argv.append(nil)
        execv(executable, &argv)
        throw MaryOSError("could not restart \(executable) after signing it: \(String(cString: strerror(errno)))")
    }

    /// Make sure this process is entitled, restarting it if it had to sign itself.
    public static func ensure(log: (String) -> Void) throws {
        if isPresent() { return }
        try signAndRestart(log: log)
    }
}
