import Foundation

/// The outcome of running one external command.
public struct CommandResult: Sendable {
    public let command: String
    public let arguments: [String]
    public let status: Int32
    public let stdout: Data
    public let stderr: Data

    public var stdoutText: String { String(decoding: stdout, as: UTF8.self) }
    public var stderrText: String { String(decoding: stderr, as: UTF8.self) }
    public var succeeded: Bool { status == 0 }
    public var commandLine: String { ([command] + arguments).joined(separator: " ") }

    /// Throw a `CommandError` unless the command exited 0.
    @discardableResult
    public func checkSuccess(_ what: String? = nil) throws -> CommandResult {
        guard succeeded else {
            let err = stderrText.trimmingCharacters(in: .whitespacesAndNewlines)
            let out = stdoutText.trimmingCharacters(in: .whitespacesAndNewlines)
            throw CommandError(what: what ?? commandLine, status: status, output: err.isEmpty ? out : err)
        }
        return self
    }
}

public struct CommandError: Error, LocalizedError, Sendable, Equatable {
    public let what: String
    public let status: Int32
    public let output: String

    public init(what: String, status: Int32, output: String) {
        self.what = what
        self.status = status
        self.output = output
    }

    public var errorDescription: String? {
        output.isEmpty ? "\(what) failed (exit \(status))" : "\(what) failed (exit \(status)): \(output)"
    }
}

/// Collects everything a pipe produces until EOF, without blocking a thread.
private final class PipeCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var buffer = Data()
    private var reachedEOF = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    init(_ handle: FileHandle) {
        handle.readabilityHandler = { [self] h in
            let chunk = h.availableData
            lock.lock()
            if chunk.isEmpty {
                reachedEOF = true
                h.readabilityHandler = nil
                let pending = waiters
                waiters.removeAll()
                lock.unlock()
                pending.forEach { $0.resume() }
            } else {
                buffer.append(chunk)
                lock.unlock()
            }
        }
    }

    func waitForEOF() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            lock.lock()
            if reachedEOF {
                lock.unlock()
                continuation.resume()
            } else {
                waiters.append(continuation)
                lock.unlock()
            }
        }
    }

    var contents: Data {
        lock.lock()
        defer { lock.unlock() }
        return buffer
    }
}

/// Runs external commands. Executables are always given by absolute path so
/// nothing depends on the caller's PATH.
public actor CommandRunner {
    public init() {}

    @discardableResult
    public func run(
        _ executable: String,
        _ arguments: [String] = [],
        stdin: Data? = nil,
        environment: [String: String]? = nil,
        currentDirectory: URL? = nil
    ) async throws -> CommandResult {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        if let environment {
            process.environment = ProcessInfo.processInfo.environment.merging(environment) { $1 }
        }
        if let currentDirectory {
            process.currentDirectoryURL = currentDirectory
        }

        let outPipe = Pipe()
        let errPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = errPipe
        let inPipe: Pipe? = stdin == nil ? nil : Pipe()
        if let inPipe {
            process.standardInput = inPipe
        } else {
            process.standardInput = FileHandle.nullDevice
        }

        let out = PipeCollector(outPipe.fileHandleForReading)
        let err = PipeCollector(errPipe.fileHandleForReading)

        let status: Int32 = try await withCheckedThrowingContinuation { continuation in
            process.terminationHandler = { finished in
                continuation.resume(returning: finished.terminationStatus)
            }
            do {
                try process.run()
                if let inPipe, let stdin {
                    try? inPipe.fileHandleForWriting.write(contentsOf: stdin)
                    try? inPipe.fileHandleForWriting.close()
                }
            } catch {
                process.terminationHandler = nil
                try? outPipe.fileHandleForWriting.close()
                try? errPipe.fileHandleForWriting.close()
                continuation.resume(throwing: error)
            }
        }

        await out.waitForEOF()
        await err.waitForEOF()

        return CommandResult(
            command: executable,
            arguments: arguments,
            status: status,
            stdout: out.contents,
            stderr: err.contents
        )
    }
}
