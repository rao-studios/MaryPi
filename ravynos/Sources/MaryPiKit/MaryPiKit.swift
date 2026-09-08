import Foundation

/// Package-wide constants.
public enum MaryPi {
    public static let version = "0.1.0"
    public static let bundleIdentifier = "com.ravynos.MaryPi"
    public static let name = "MaryPi"
}

/// A line-oriented log sink. Every long-running operation in MaryPiKit
/// reports human-readable progress through one of these.
public typealias Logger = @Sendable (String) -> Void

/// A logger that drops everything.
public let silentLogger: Logger = { _ in }

/// Errors raised by MaryPiKit that carry a user-facing message.
public struct MaryPiError: Error, LocalizedError, Sendable, Equatable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}
