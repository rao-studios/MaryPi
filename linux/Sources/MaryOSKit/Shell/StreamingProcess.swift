import Foundation

/// Runs a long external command and delivers its combined stdout/stderr one
/// line at a time while it runs. Used for the builder, whose log is the
/// user's only view of a multi-minute Docker session.
public enum StreamingProcess {
    private final class LineSplitter: @unchecked Sendable {
        private let lock = NSLock()
        private var partial = ""
        private var finished = false
        private var waiters: [CheckedContinuation<Void, Never>] = []
        private let onLine: Logger

        init(onLine: @escaping Logger) {
            self.onLine = onLine
        }

        func append(_ data: Data) {
            lock.lock()
            partial += String(decoding: data, as: UTF8.self)
            var lines: [String] = []
            while let newline = partial.firstIndex(of: "\n") {
                lines.append(String(partial[..<newline]))
                partial = String(partial[partial.index(after: newline)...])
            }
            lock.unlock()
            for line in lines { onLine(line.hasSuffix("\r") ? String(line.dropLast()) : line) }
        }

        func finish() {
            lock.lock()
            guard !finished else { lock.unlock(); return }
            finished = true
            let rest = partial
            partial = ""
            let pending = waiters
            waiters.removeAll()
            lock.unlock()
            if !rest.isEmpty { onLine(rest) }
            pending.forEach { $0.resume() }
        }

        func waitForEOF() async {
            await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
                lock.lock()
                if finished {
                    lock.unlock()
                    continuation.resume()
                } else {
                    waiters.append(continuation)
                    lock.unlock()
                }
            }
        }
    }

    /// Exit status of the command; every output line goes to `onLine`.
    @discardableResult
    public static func run(
        executable: String,
        arguments: [String],
        environment: [String: String]? = nil,
        currentDirectory: URL? = nil,
        onLine: @escaping Logger
    ) async throws -> Int32 {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        if let environment { process.environment = environment }
        if let currentDirectory { process.currentDirectoryURL = currentDirectory }
        process.standardInput = FileHandle.nullDevice
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe

        let splitter = LineSplitter(onLine: onLine)
        pipe.fileHandleForReading.readabilityHandler = { handle in
            let chunk = handle.availableData
            if chunk.isEmpty {
                handle.readabilityHandler = nil
                splitter.finish()
            } else {
                splitter.append(chunk)
            }
        }

        let status: Int32 = try await withCheckedThrowingContinuation { continuation in
            process.terminationHandler = { finished in
                continuation.resume(returning: finished.terminationStatus)
            }
            do {
                try process.run()
            } catch {
                process.terminationHandler = nil
                try? pipe.fileHandleForWriting.close()
                continuation.resume(throwing: error)
            }
        }
        await splitter.waitForEOF()
        return status
    }
}
