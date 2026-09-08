import Foundation

/// Persisted preferences: `~/Library/Application Support/MaryOS/config.json`.
public struct Settings: Codable, Sendable, Equatable {
    /// The kit directory (linux/) chosen in the app, when not running from a checkout.
    public var kitDirectory: String?

    public init(kitDirectory: String? = nil) {
        self.kitDirectory = kitDirectory
    }

    public static func defaultURL(home: URL = FileManager.default.homeDirectoryForCurrentUser) -> URL {
        home.appending(path: "Library/Application Support/MaryOS/config.json")
    }

    public static func load(from url: URL? = nil) -> Settings {
        let location = url ?? defaultURL()
        guard let data = try? Data(contentsOf: location),
              let settings = try? JSONDecoder().decode(Settings.self, from: data) else {
            return Settings()
        }
        return settings
    }

    public func save(to url: URL? = nil) throws {
        let location = url ?? Self.defaultURL()
        try FileManager.default.createDirectory(at: location.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: location, options: .atomic)
    }
}
