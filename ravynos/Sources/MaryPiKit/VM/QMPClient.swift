import Foundation
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

/// Minimal QEMU Machine Protocol client over a unix socket: handshake, one
/// command, one reply. Enough for `quit` and `query-status`.
public struct QMPClient: Sendable {
    public let socketPath: String
    public let timeout: TimeInterval

    public init(socketPath: String, timeout: TimeInterval = 3) {
        self.socketPath = socketPath
        self.timeout = timeout
    }

    static func encode(_ command: String) -> Data {
        Data("{\"execute\":\"\(command)\"}\n".utf8)
    }

    /// Split newline-delimited JSON objects out of `buffer`, leaving any
    /// incomplete trailing line in place.
    static func messages(in buffer: inout Data) -> [[String: Any]] {
        var result: [[String: Any]] = []
        while let newline = buffer.firstIndex(of: 0x0A) {
            let line = buffer[buffer.startIndex..<newline]
            buffer = Data(buffer[buffer.index(after: newline)...])
            if line.isEmpty { continue }
            if let object = try? JSONSerialization.jsonObject(with: Data(line)) as? [String: Any] {
                result.append(object)
            }
        }
        return result
    }

    /// Connect, negotiate capabilities, run `command`, return its reply
    /// (the `return` payload or an `error` dictionary).
    public func execute(_ command: String) throws -> [String: Any] {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw MaryPiError("QMP: socket() failed (errno \(errno))") }
        defer { close(fd) }

        var tv = timeval(tv_sec: Int(timeout), tv_usec: Int32((timeout - Double(Int(timeout))) * 1_000_000))
        _ = withUnsafePointer(to: &tv) { setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, $0, socklen_t(MemoryLayout<timeval>.size)) }
        _ = withUnsafePointer(to: &tv) { setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, $0, socklen_t(MemoryLayout<timeval>.size)) }

        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = Array(socketPath.utf8)
        let capacity = MemoryLayout.size(ofValue: address.sun_path)
        guard pathBytes.count < capacity else { throw MaryPiError("QMP: socket path too long: \(socketPath)") }
        withUnsafeMutablePointer(to: &address.sun_path) { pointer in
            pointer.withMemoryRebound(to: UInt8.self, capacity: capacity) { raw in
                for (index, byte) in pathBytes.enumerated() { raw[index] = byte }
                raw[pathBytes.count] = 0
            }
        }
        let length = socklen_t(MemoryLayout<sockaddr_un>.size)
        let connected = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, length) }
        }
        guard connected == 0 else { throw MaryPiError("QMP: cannot connect to \(socketPath) (errno \(errno))") }

        var buffer = Data()
        func readMessage(matching predicate: ([String: Any]) -> Bool) throws -> [String: Any] {
            let deadline = Date().addingTimeInterval(timeout)
            while Date() < deadline {
                for message in QMPClient.messages(in: &buffer) where predicate(message) {
                    return message
                }
                var chunk = [UInt8](repeating: 0, count: 4096)
                let n = read(fd, &chunk, chunk.count)
                if n <= 0 { break }
                buffer.append(contentsOf: chunk[0..<n])
            }
            throw MaryPiError("QMP: timed out waiting for a reply from \(socketPath)")
        }
        func send(_ command: String) throws {
            let data = QMPClient.encode(command)
            let written = data.withUnsafeBytes { write(fd, $0.baseAddress, data.count) }
            guard written == data.count else { throw MaryPiError("QMP: write failed (errno \(errno))") }
        }

        _ = try readMessage { $0["QMP"] != nil }
        try send("qmp_capabilities")
        _ = try readMessage { $0["return"] != nil || $0["error"] != nil }
        try send(command)
        let reply = try readMessage { $0["return"] != nil || $0["error"] != nil }
        if let error = reply["error"] as? [String: Any] {
            return ["error": error]
        }
        return ["return": reply["return"] ?? [:]]
    }

    /// `query-status` → "running", "paused", ... or nil when unreachable.
    public func status() -> String? {
        guard let reply = try? execute("query-status"), let payload = reply["return"] as? [String: Any] else { return nil }
        return payload["status"] as? String
    }
}
