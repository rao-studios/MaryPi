import Foundation
import Testing
@testable import MaryVNCKit

@Suite struct ViewerSettingsTests {
    private func temporaryURL() -> URL {
        FileManager.default.temporaryDirectory.appending(path: "maryvnc-settings-\(UUID().uuidString)/settings.json")
    }

    @Test func settingsRoundTripAndMissingOrGarbledFilesGiveTheDefaults() throws {
        let url = temporaryURL()
        defer { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
        #expect(ViewerSettings.load(from: url) == ViewerSettings())

        let chosen = ViewerSettings(accent: .graphite, commandKey: .control, quality: .fast)
        try chosen.save(to: url)
        #expect(ViewerSettings.load(from: url) == chosen)

        try Data(#"{"accent": "graphite"}"#.utf8).write(to: url)
        #expect(ViewerSettings.load(from: url) == ViewerSettings(accent: .graphite))

        try Data(#"{"accent": "chartreuse", "quality": 1}"#.utf8).write(to: url)
        #expect(ViewerSettings.load(from: url) == ViewerSettings(quality: .fast))

        try Data("not json".utf8).write(to: url)
        #expect(ViewerSettings.load(from: url) == ViewerSettings())
    }
}
