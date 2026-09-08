import Foundation
import Observation
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

/// Launches QEMU in-process and follows its serial log. Used by the app and by
/// `marypi vm run`; cross-process control lives in `VMController`.
@MainActor
@Observable
public final class VMSession {
    public enum State: Sendable, Equatable {
        case idle
        case starting
        case running(pid: Int32)
        case stopping
        case exited(Int32)
        case failed(String)
    }

    public private(set) var state: State = .idle
    public private(set) var command: QEMUCommand?
    public let paths: VMStatePaths

    private var process: Process?
    private var tailer: LogTailer?
    private var consumer: Task<Void, Never>?
    private var exitContinuations: [CheckedContinuation<Int32, Never>] = []

    public init(paths: VMStatePaths) {
        self.paths = paths
    }

    public var isRunning: Bool {
        switch state {
        case .starting, .running, .stopping: return true
        default: return false
        }
    }

    public var pid: Int32? {
        if case .running(let pid) = state { return pid }
        return nil
    }

    /// Start QEMU. `serial` receives each serial-log line; `log` receives
    /// launcher messages. With `detach`, QEMU survives this process.
    public func launch(_ command: QEMUCommand, detach: Bool = false, serial: Logger? = nil, log: Logger = silentLogger) throws {
        guard !isRunning else { throw MaryPiError("a VM is already running in this session") }
        state = .starting
        self.command = command

        let process = Process()
        process.executableURL = URL(fileURLWithPath: command.executable)
        process.arguments = command.arguments
        if command.arguments.contains(where: { $0.hasPrefix("stdio,") }) {
            process.standardInput = FileHandle.standardInput
            process.standardOutput = FileHandle.standardOutput
            process.standardError = FileHandle.standardError
        } else {
            process.standardInput = FileHandle.nullDevice
            FileManager.default.createFile(atPath: paths.qemuOutput.path, contents: nil)
            let output = try FileHandle(forWritingTo: paths.qemuOutput)
            process.standardOutput = output
            process.standardError = output
        }
        process.terminationHandler = { [weak self] finished in
            let status = finished.terminationStatus
            Task { @MainActor in self?.processDidExit(status) }
        }

        if let serial {
            if !FileManager.default.fileExists(atPath: paths.serialLog.path) {
                FileManager.default.createFile(atPath: paths.serialLog.path, contents: nil)
            }
            let tailer = LogTailer(url: paths.serialLog)
            self.tailer = tailer
            consumer = Task.detached {
                for await line in tailer.lines {
                    serial(line)
                }
            }
            tailer.start()
        }

        log("Launching \(command.commandLine)")
        if detach {
            signal(SIGHUP, SIG_IGN)
        }
        do {
            try process.run()
        } catch {
            state = .failed(error.localizedDescription)
            tailer?.stop()
            throw error
        }
        self.process = process
        state = .running(pid: process.processIdentifier)
        log("QEMU running (pid \(process.processIdentifier)); serial log: \(paths.serialLog.path)")
    }

    private func processDidExit(_ status: Int32) {
        tailer?.stop()
        tailer = nil
        process = nil
        state = .exited(status)
        let waiting = exitContinuations
        exitContinuations.removeAll()
        waiting.forEach { $0.resume(returning: status) }
    }

    /// Wait for QEMU to exit; returns its exit status (0 when nothing runs).
    public func waitUntilExit() async -> Int32 {
        switch state {
        case .exited(let status): return status
        case .idle, .failed: return 0
        default: break
        }
        return await withCheckedContinuation { continuation in
            exitContinuations.append(continuation)
        }
    }

    /// Graceful stop through QMP, then signals.
    public func stop(log: Logger = silentLogger) async {
        guard isRunning else { return }
        state = .stopping
        try? await VMController.stop(paths, log: log)
        if let process, process.isRunning {
            process.terminate()
        }
        _ = await waitUntilExit()
    }
}
