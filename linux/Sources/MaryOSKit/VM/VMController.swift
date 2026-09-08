import Darwin
import Foundation

/// Operations on a VM state directory that work across processes: the CLI
/// can stop a VM the app started, and report on it.
public enum VMController {
    public struct Status: Sendable, Equatable, Codable {
        public let target: String
        public let pid: Int32?
        public let alive: Bool
        public let serialLog: String
        public let serialBytes: Int64
        public let disk: String
        public let diskExists: Bool
        public let diskBytes: Int64?
    }

    public static func isAlive(_ pid: Int32) -> Bool {
        guard pid > 0 else { return false }
        return kill(pid, 0) == 0 || errno == EPERM
    }

    public static func readPID(_ paths: VMStatePaths) -> Int32? {
        guard let text = try? String(contentsOf: paths.pidFile, encoding: .utf8) else { return nil }
        return Int32(text.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// The pid of a live runner for this state, removing a stale pid file.
    public static func runningPID(_ paths: VMStatePaths) -> Int32? {
        guard let pid = readPID(paths) else { return nil }
        if isAlive(pid) { return pid }
        try? FileManager.default.removeItem(at: paths.pidFile)
        return nil
    }

    public static func status(_ paths: VMStatePaths) -> Status {
        let pid = readPID(paths)
        let fm = FileManager.default
        let serialBytes = (try? fm.attributesOfItem(atPath: paths.serialLog.path)[.size] as? NSNumber)?.int64Value ?? 0
        let diskBytes = (try? fm.attributesOfItem(atPath: paths.disk.path)[.size] as? NSNumber)?.int64Value
        return Status(target: paths.target.rawValue, pid: pid, alive: pid.map(isAlive) ?? false,
                      serialLog: paths.serialLog.path, serialBytes: serialBytes,
                      disk: paths.disk.path, diskExists: paths.hasDisk, diskBytes: diskBytes)
    }

    /// SIGTERM to the runner (which asks the guest to shut down), then SIGKILL.
    public static func stop(_ paths: VMStatePaths, timeout: Duration = .seconds(30), log: Logger = silentLogger) async throws {
        guard let pid = runningPID(paths) else {
            log("No VM running for target \(paths.target.rawValue)")
            return
        }
        kill(pid, SIGTERM)
        log("Sent SIGTERM to the VM runner (pid \(pid)); it asks the guest to shut down")
        let deadline = ContinuousClock.now + timeout
        while isAlive(pid), ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(200))
        }
        if isAlive(pid) {
            kill(pid, SIGKILL)
            log("The runner did not exit; killed pid \(pid)")
        } else {
            log("VM stopped (pid \(pid))")
        }
        try? FileManager.default.removeItem(at: paths.pidFile)
    }
}
