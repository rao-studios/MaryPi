import Foundation

/// Persisted preferences: `~/Library/Application Support/MaryPi/config.json`.
public struct Settings: Codable, Sendable, Equatable {
    public var ravynosRoot: String?
    public var buildDir: String?

    public init(ravynosRoot: String? = nil, buildDir: String? = nil) {
        self.ravynosRoot = ravynosRoot
        self.buildDir = buildDir
    }

    public static func defaultURL(home: URL = FileManager.default.homeDirectoryForCurrentUser) -> URL {
        #if os(macOS)
        return home.appending(path: "Library/Application Support/MaryPi/config.json")
        #else
        if let xdg = ProcessInfo.processInfo.environment["XDG_CONFIG_HOME"], !xdg.isEmpty {
            return URL(fileURLWithPath: xdg).appending(path: "marypi/config.json")
        }
        return home.appending(path: ".config/marypi/config.json")
        #endif
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
