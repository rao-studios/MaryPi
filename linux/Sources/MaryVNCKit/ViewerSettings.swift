import Foundation

/// The viewer's preferences, in `~/Library/Application Support/MaryVNC/settings.json`. A missing or
/// unreadable file, or a missing key, means the default.
public struct ViewerSettings: Codable, Equatable, Sendable {
    /// Liquid Platinum's two appearances, as in Aqua's Blue and Graphite.
    public enum Accent: String, Codable, CaseIterable, Sendable {
        case blue
        case graphite
    }

    public var accent: Accent
    public var commandKey: CommandKey
    public var quality: FrameQuality

    public init(accent: Accent = .blue, commandKey: CommandKey = .super, quality: FrameQuality = .best) {
        self.accent = accent
        self.commandKey = commandKey
        self.quality = quality
    }

    public static var defaultURL: URL {
        URL.applicationSupportDirectory.appending(path: "MaryVNC/settings.json")
    }

    enum CodingKeys: String, CodingKey { case accent, commandKey, quality }

    public init(from decoder: any Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let defaults = ViewerSettings()
        accent = (try? c.decodeIfPresent(Accent.self, forKey: .accent)) ?? defaults.accent
        commandKey = (try? c.decodeIfPresent(CommandKey.self, forKey: .commandKey)) ?? defaults.commandKey
        quality = (try? c.decodeIfPresent(FrameQuality.self, forKey: .quality)) ?? defaults.quality
    }

    public static func load(from url: URL = ViewerSettings.defaultURL) -> ViewerSettings {
        guard let data = try? Data(contentsOf: url) else { return ViewerSettings() }
        return (try? JSONDecoder().decode(ViewerSettings.self, from: data)) ?? ViewerSettings()
    }

    public func save(to url: URL = ViewerSettings.defaultURL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try encoder.encode(self).write(to: url, options: .atomic)
    }
}
