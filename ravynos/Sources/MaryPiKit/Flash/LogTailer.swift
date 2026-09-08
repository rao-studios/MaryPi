import Foundation

/// Follows a growing text file and delivers complete lines.
public final class LogTailer: @unchecked Sendable {
    public let url: URL
    public let lines: AsyncStream<String>

    private let continuation: AsyncStream<String>.Continuation
    private let queue = DispatchQueue(label: "com.ravynos.MaryPi.logtailer")
    private let interval: TimeInterval
    private var timer: DispatchSourceTimer?
    private var offset: UInt64 = 0
    private var partial = ""
    private var stopped = false

    public init(url: URL, interval: TimeInterval = 0.25) {
        self.url = url
        self.interval = interval
        var captured: AsyncStream<String>.Continuation!
        lines = AsyncStream(bufferingPolicy: .unbounded) { captured = $0 }
        continuation = captured
    }

    public func start() {
        queue.sync {
            guard timer == nil, !stopped else { return }
            let source = DispatchSource.makeTimerSource(queue: queue)
            source.schedule(deadline: .now(), repeating: interval)
            source.setEventHandler { [weak self] in self?.poll() }
            source.resume()
            timer = source
        }
    }

    /// Drain whatever is left, then finish the stream.
    public func stop() {
        queue.sync {
            guard !stopped else { return }
            stopped = true
            timer?.cancel()
            timer = nil
            poll()
            if !partial.isEmpty {
                continuation.yield(partial)
                partial = ""
            }
            continuation.finish()
        }
    }

    private func poll() {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return }
        defer { try? handle.close() }
        guard (try? handle.seek(toOffset: offset)) != nil else { return }
        guard let data = try? handle.readToEnd(), !data.isEmpty else { return }
        offset += UInt64(data.count)
        partial += String(decoding: data, as: UTF8.self)
        while let newline = partial.firstIndex(of: "\n") {
            let line = String(partial[..<newline])
            partial = String(partial[partial.index(after: newline)...])
            continuation.yield(line)
        }
    }
}
