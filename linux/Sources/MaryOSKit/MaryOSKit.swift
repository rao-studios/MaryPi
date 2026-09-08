import Foundation

/// Package-wide constants.
public enum MaryOS {
    public static let version = "0.1.0"
    public static let bundleIdentifier = "com.maryos.MaryOS"
    public static let name = "MaryOS"
}

/// A line-oriented log sink. Every long-running operation in MaryOSKit
/// reports human-readable progress through one of these.
public typealias Logger = @Sendable (String) -> Void

/// A logger that drops everything.
public let silentLogger: Logger = { _ in }

/// Errors raised by MaryOSKit that carry a user-facing message.
public struct MaryOSError: Error, LocalizedError, Sendable, Equatable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}
