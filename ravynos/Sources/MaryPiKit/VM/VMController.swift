import Foundation
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

/// Operations on a VM state directory that work across processes: the CLI
/// can stop a VM the app started, or one started by `vm/run.sh`.
public enum VMController {
    public struct Status: Sendable, Equatable, Codable {
        public let profile: String
        public let pid: Int32?
        public let alive: Bool
        public let qmpStatus: String?
        public let serialLog: String
        public let serialBytes: Int64
    }

    static func isAlive(_ pid: Int32) -> Bool {
        guard pid > 0 else { return false }
        return kill(pid, 0) == 0 || errno == EPERM
    }

    public static func readPID(_ paths: VMStatePaths) -> Int32? {
        guard let text = try? String(contentsOf: paths.pidFile, encoding: .utf8) else { return nil }
        return Int32(text.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// The QMP socket path recorded for a running VM (run.sh may have used a short fallback).
    public static func qmpSocketPath(_ paths: VMStatePaths) -> String {
        if let recorded = try? String(contentsOf: paths.qmpPathFile, encoding: .utf8) {
            let trimmed = recorded.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty { return trimmed }
        }
        return paths.qmpSocketPath
    }

    static func pad(_ url: URL, to size: Int64) throws {
        let handle = try FileHandle(forWritingTo: url)
        defer { try? handle.close() }
        let current = try handle.seekToEnd()
        if current < UInt64(size) {
            try handle.truncate(atOffset: UInt64(size))
        }
    }

    /// Create the state directory, the UEFI variable store and (if needed)
    /// the padded firmware copy; clean up after a dead VM; refuse to start a
    /// second instance; append a session marker to the serial log.
    public static func prepareState(_ paths: VMStatePaths, firmware: VMFirmware, image: URL, accel: VMAccel, backend: VMDisplayBackend, log: Logger) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: paths.directory, withIntermediateDirectories: true)

        if !fm.fileExists(atPath: paths.varsFlash.path) {
            if let template = firmware.varsTemplate, fm.fileExists(atPath: template) {
                try fm.copyItem(at: URL(fileURLWithPath: template), to: paths.varsFlash)
                log("UEFI variables: fresh copy of \(template)")
            } else {
                fm.createFile(atPath: paths.varsFlash.path, contents: Data())
                log("UEFI variables: empty store (no template shipped with this firmware)")
            }
        }
        try pad(paths.varsFlash, to: VMFirmware.pflashSize)

        if firmware.needsPadding {
            if !fm.fileExists(atPath: paths.paddedCode.path) {
                try fm.copyItem(at: URL(fileURLWithPath: firmware.code), to: paths.paddedCode)
            }
            try pad(paths.paddedCode, to: VMFirmware.pflashSize)
            log("Firmware: \(firmware.code) padded to 64 MiB at \(paths.paddedCode.path)")
        }

        if let pid = readPID(paths) {
            if isAlive(pid) {
                throw MaryPiError("a VM for profile \(paths.profile) is already running (pid \(pid)); stop it first")
            }
            try? fm.removeItem(at: paths.pidFile)
            try? fm.removeItem(atPath: paths.directory.appending(path: "qmp.sock").path)
        }
        if paths.usesFallbackSocket {
            try (paths.qmpSocketPath + "\n").write(to: paths.qmpPathFile, atomically: true, encoding: .utf8)
        } else {
            try? fm.removeItem(at: paths.qmpPathFile)
        }

        let stamp = ISO8601DateFormatter().string(from: Date())
        let marker = "\n==== marypi vm run \(stamp) \(accel.rawValue)/\(backend.rawValue) image=\(image.path) ====\n"
        if let handle = try? FileHandle(forWritingTo: paths.serialLog) {
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: Data(marker.utf8))
            try? handle.close()
        } else {
            try marker.write(to: paths.serialLog, atomically: true, encoding: .utf8)
        }
    }

    public static func status(_ paths: VMStatePaths) -> Status {
        let pid = readPID(paths)
        let alive = pid.map(isAlive) ?? false
        let qmp = alive ? QMPClient(socketPath: qmpSocketPath(paths), timeout: 1).status() : nil
        let bytes = (try? FileManager.default.attributesOfItem(atPath: paths.serialLog.path)[.size] as? NSNumber)?.int64Value ?? 0
        return Status(profile: paths.profile, pid: pid, alive: alive, qmpStatus: qmp, serialLog: paths.serialLog.path, serialBytes: bytes)
    }

    /// QMP `quit`, then SIGTERM, then SIGKILL.
    public static func stop(_ paths: VMStatePaths, timeout: Duration = .seconds(8), log: Logger = silentLogger) async throws {
        guard let pid = readPID(paths), isAlive(pid) else {
            try? FileManager.default.removeItem(at: paths.pidFile)
            log("No VM running for profile \(paths.profile)")
            return
        }
        let quit = (try? QMPClient(socketPath: qmpSocketPath(paths)).execute("quit")) != nil
        log(quit ? "Asked QEMU (pid \(pid)) to quit via QMP" : "QMP unreachable; sending SIGTERM to pid \(pid)")
        if !quit { kill(pid, SIGTERM) }
        let deadline = ContinuousClock.now + timeout
        while isAlive(pid) && ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        if isAlive(pid) {
            kill(pid, SIGTERM)
            try await Task.sleep(for: .seconds(2))
        }
        if isAlive(pid) {
            kill(pid, SIGKILL)
            log("QEMU did not exit; killed pid \(pid)")
        }
        try? FileManager.default.removeItem(at: paths.pidFile)
        try? FileManager.default.removeItem(atPath: paths.directory.appending(path: "qmp.sock").path)
        log("VM stopped (pid \(pid))")
    }
}
