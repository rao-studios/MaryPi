import Foundation
import Observation
import Virtualization

/// Receives Virtualization.framework's delegate calls (on the VM's queue,
/// which is the main queue) and forwards them to the runner.
private final class VMDelegate: NSObject, VZVirtualMachineDelegate {
    private let onGuestStop: @MainActor () -> Void
    private let onError: @MainActor (any Error) -> Void

    init(onGuestStop: @escaping @MainActor () -> Void, onError: @escaping @MainActor (any Error) -> Void) {
        self.onGuestStop = onGuestStop
        self.onError = onError
    }

    func guestDidStop(_ virtualMachine: VZVirtualMachine) {
        let handler = onGuestStop
        MainActor.assumeIsolated { handler() }
    }

    func virtualMachine(_ virtualMachine: VZVirtualMachine, didStopWithError error: any Error) {
        let handler = onError
        MainActor.assumeIsolated { handler(error) }
    }
}

/// Copies everything the guest writes on its serial port to the serial log
/// and to a terminal, so an interactive console still leaves a record.
final class ConsoleTee: @unchecked Sendable {
    private let pipe = Pipe()

    var writer: FileHandle { pipe.fileHandleForWriting }

    init(log: FileHandle, terminal: FileHandle) {
        pipe.fileHandleForReading.readabilityHandler = { handle in
            let data = handle.availableData
            if data.isEmpty {
                handle.readabilityHandler = nil
                return
            }
            try? log.write(contentsOf: data)
            try? terminal.write(contentsOf: data)
        }
    }

    func close() {
        try? pipe.fileHandleForWriting.close()
    }
}

/// Owns one `VZVirtualMachine` on the main queue: load a spec, start, stop
/// gracefully (ACPI power button, then force), and follow the serial log.
/// Cross-process control (another `maryos vm stop`) is `VMController`.
@MainActor
@Observable
public final class VMRunner {
    public enum State: Sendable, Equatable {
        case idle
        case loaded
        case starting
        case running
        case stopping
        case stopped(String)
        case failed(String)
    }

    public private(set) var state: State = .idle
    public private(set) var machine: VZVirtualMachine?
    public private(set) var spec: VMSpec?
    public let paths: VMStatePaths

    private var delegate: VMDelegate?
    private var logHandle: FileHandle?
    private var tee: ConsoleTee?
    private var tailer: LogTailer?
    private var consumer: Task<Void, Never>?
    private var waiters: [CheckedContinuation<State, Never>] = []

    public init(paths: VMStatePaths) {
        self.paths = paths
    }

    public var isRunning: Bool {
        switch state {
        case .starting, .running, .stopping: return true
        default: return false
        }
    }

    public var isActive: Bool { isRunning || state == .loaded }

    /// Build the configuration and the machine without starting it, so a
    /// window can show the display from the first frame. `consoleInput` and
    /// `consoleOutput` attach a terminal to the guest's serial port; the
    /// serial log receives everything either way.
    @discardableResult
    public func load(_ spec: VMSpec, consoleInput: FileHandle? = nil, consoleOutput: FileHandle? = nil) throws -> VZVirtualMachine {
        guard !isActive else { throw MaryOSError("a VM is already loaded in this process") }
        self.spec = spec
        let fm = FileManager.default
        try fm.createDirectory(at: paths.directory, withIntermediateDirectories: true)
        if !fm.fileExists(atPath: paths.serialLog.path) {
            fm.createFile(atPath: paths.serialLog.path, contents: nil)
        }
        let log = try FileHandle(forWritingTo: paths.serialLog)
        try log.seekToEnd()
        let stamp = ISO8601DateFormatter().string(from: Date())
        try log.write(contentsOf: Data("\n==== maryos vm run \(stamp) \(spec.summary) ====\n".utf8))
        logHandle = log

        let output: FileHandle
        if let consoleOutput {
            let tee = ConsoleTee(log: log, terminal: consoleOutput)
            self.tee = tee
            output = tee.writer
        } else {
            output = log
        }

        let configuration = try VMConfigurationBuilder.make(spec, serialInput: consoleInput, serialOutput: output)
        let vm = VZVirtualMachine(configuration: configuration)
        let delegate = VMDelegate(
            onGuestStop: { [weak self] in self?.finish(.stopped("the guest shut down")) },
            onError: { [weak self] error in self?.finish(.failed(error.localizedDescription)) }
        )
        vm.delegate = delegate
        self.delegate = delegate
        machine = vm
        state = .loaded
        return vm
    }

    /// Start a loaded machine. `serial` receives each serial-log line; `log`
    /// receives the runner's own messages.
    public func start(serial: Logger? = nil, log: Logger = silentLogger) async throws {
        guard let vm = machine, state == .loaded, let spec else {
            throw MaryOSError("load a VM before starting it")
        }
        state = .starting
        if let serial {
            let tailer = LogTailer(url: paths.serialLog)
            self.tailer = tailer
            consumer = Task.detached {
                for await line in tailer.lines { serial(line) }
            }
            tailer.start()
        }
        try "\(ProcessInfo.processInfo.processIdentifier)\n".write(to: paths.pidFile, atomically: true, encoding: .utf8)
        log("Starting \(spec.name): \(spec.summary)")
        do {
            try await vm.start()
        } catch {
            finish(.failed(error.localizedDescription))
            throw MaryOSError("the VM did not start: \(error.localizedDescription)")
        }
        state = .running
        log("VM running (pid \(ProcessInfo.processInfo.processIdentifier)); serial log: \(paths.serialLog.path)")
    }

    public func start(_ spec: VMSpec, consoleInput: FileHandle? = nil, consoleOutput: FileHandle? = nil, serial: Logger? = nil, log: Logger = silentLogger) async throws {
        try load(spec, consoleInput: consoleInput, consoleOutput: consoleOutput)
        try await start(serial: serial, log: log)
    }

    /// Ask the guest to shut down (it sees an ACPI power button; systemd
    /// powers off cleanly), then force the machine off after `timeout`.
    public func stop(timeout: Duration = .seconds(20), log: Logger = silentLogger) async {
        guard let vm = machine else { return }
        if state == .loaded {
            finish(.stopped("never started"))
            return
        }
        guard state == .running || state == .starting else { return }
        state = .stopping
        if vm.canRequestStop {
            do {
                try vm.requestStop()
                log("Asked the guest to shut down")
            } catch {
                log("requestStop: \(error.localizedDescription)")
            }
        }
        let deadline = ContinuousClock.now + timeout
        while state == .stopping, ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(200))
        }
        guard state == .stopping else { return }
        log("The guest did not shut down within \(timeout); stopping the VM")
        do {
            try await vm.stop()
        } catch {
            log("stop: \(error.localizedDescription)")
        }
        if state == .stopping {
            finish(.stopped("stopped by the host"))
        }
    }

    /// Wait until the machine has stopped or failed; returns the final state.
    public func waitUntilStopped() async -> State {
        switch state {
        case .stopped, .failed, .idle: return state
        default: break
        }
        return await withCheckedContinuation { waiters.append($0) }
    }

    private func finish(_ final: State) {
        state = final
        tailer?.stop()
        tailer = nil
        consumer = nil
        tee?.close()
        tee = nil
        try? logHandle?.close()
        logHandle = nil
        try? FileManager.default.removeItem(at: paths.pidFile)
        machine = nil
        delegate = nil
        let pending = waiters
        waiters.removeAll()
        pending.forEach { $0.resume(returning: final) }
    }
}
