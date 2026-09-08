import Foundation
#if canImport(CryptoKit)
import CryptoKit
#endif

public enum FileHash {
    /// Lower-case hex SHA-256 of a file, streamed in 1 MiB chunks. Uses
    /// CryptoKit on Apple platforms and the portable implementation elsewhere.
    public static func sha256Hex(of url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        #if canImport(CryptoKit)
        var hasher = SHA256()
        while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty {
            hasher.update(data: chunk)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
        #else
        var hasher = SHA256Portable()
        while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty {
            hasher.update(chunk)
        }
        return SHA256Portable.hex(hasher.finalize())
        #endif
    }
}
