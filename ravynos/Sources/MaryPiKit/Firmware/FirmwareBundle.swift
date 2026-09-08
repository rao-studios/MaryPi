import Foundation

/// A verified, extracted rpi5-uefi release on disk.
public struct FirmwareBundle: Sendable, Equatable {
    public static let efiFileName = "RPI_EFI.fd"
    public static let configFileName = "config.txt"
    public static let dtbFileName = "bcm2712-rpi-5-b.dtb"
    public static let overlaysDirectoryName = "overlays"

    public let sourceID: String
    public let version: String
    public let directory: URL
    public let rpiEfiFd: URL
    public let upstreamConfigTxt: URL
    public let dtb: URL
    public let overlaysDir: URL?

    public init(source: FirmwareSource, directory: URL, fileManager: FileManager = .default) throws {
        sourceID = source.id
        version = source.version
        self.directory = directory
        rpiEfiFd = directory.appending(path: Self.efiFileName)
        upstreamConfigTxt = directory.appending(path: Self.configFileName)
        dtb = directory.appending(path: Self.dtbFileName)
        let overlays = directory.appending(path: Self.overlaysDirectoryName)
        overlaysDir = fileManager.fileExists(atPath: overlays.path) ? overlays : nil

        for required in [rpiEfiFd, upstreamConfigTxt, dtb] where !fileManager.fileExists(atPath: required.path) {
            throw MaryPiError("Firmware bundle at \(directory.path) is missing \(required.lastPathComponent)")
        }
        for name in source.files where !fileManager.fileExists(atPath: directory.appending(path: name).path) {
            throw MaryPiError("Firmware bundle at \(directory.path) is missing \(name) listed in the manifest")
        }
    }

    /// Files copied verbatim to the FAT partition (config.txt is rendered separately).
    public var verbatimFiles: [URL] {
        [rpiEfiFd, dtb]
    }
}
