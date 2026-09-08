import Foundation
import Testing
@testable import MaryPiKit

@Suite struct ManifestTests {
    @Test func bundledManifestIsValid() throws {
        let manifest = try FirmwareManifest.bundled()
        try manifest.validate()
        let source = try #require(manifest.rpi5UEFI)
        #expect(source.version.hasPrefix("v"))
        #expect(source.files.contains(FirmwareBundle.efiFileName))
        #expect(source.files.contains(FirmwareBundle.dtbFileName))
        #expect(source.files.contains(FirmwareBundle.configFileName))
        #expect(source.downloadURL?.host == "github.com")
        #expect(source.archiveFileName.hasSuffix(".zip"))
    }

    @Test func rejectsBadHash() throws {
        let bad = FirmwareManifest(updated: "2026-01-01", sources: [
            FirmwareSource(id: "rpi5-uefi", version: "v0", url: "https://example.com/x.zip", sha256: "abc", sizeBytes: 1, files: ["a"]),
        ])
        #expect(throws: MaryPiError.self) { try bad.validate() }
    }

    @Test func rejectsHTTP() throws {
        let bad = FirmwareManifest(updated: "2026-01-01", sources: [
            FirmwareSource(id: "rpi5-uefi", version: "v0", url: "http://example.com/x.zip", sha256: String(repeating: "a", count: 64), sizeBytes: 1, files: ["a"]),
        ])
        #expect(throws: MaryPiError.self) { try bad.validate() }
    }

    @Test func bundledTextResourcesExist() throws {
        #expect(try BundledResources.cardReadme().contains("MaryPi"))
        #expect(try BundledResources.ravynosConfigFragment().contains("enable_uart=1"))
    }
}
