import Foundation

/// `MARYPI.txt` on the FAT partition: what MaryPi wrote and when.
public struct MaryPiMarker: Sendable, Equatable {
    public static let fileName = "MARYPI.txt"

    public var level: PayloadLevel
    public var marypiVersion: String
    public var firmwareVersion: String
    public var target: String
    public var kernelSHA256: String?
    public var ravynosGitSHA: String?
    public var date: Date

    public init(level: PayloadLevel, marypiVersion: String = MaryPi.version, firmwareVersion: String, target: String = "raspberry-pi-5", kernelSHA256: String? = nil, ravynosGitSHA: String? = nil, date: Date = Date()) {
        self.level = level
        self.marypiVersion = marypiVersion
        self.firmwareVersion = firmwareVersion
        self.target = target
        self.kernelSHA256 = kernelSHA256
        self.ravynosGitSHA = ravynosGitSHA
        self.date = date
    }

    public func render() -> String {
        var lines = [
            "level=\(level.rawValue)",
            "level-name=\(level.title)",
            "marypi=\(marypiVersion)",
            "firmware=rpi5-uefi \(firmwareVersion)",
            "target=\(target)",
        ]
        if let kernelSHA256 { lines.append("kernel-sha256=\(kernelSHA256)") }
        if let ravynosGitSHA { lines.append("ravynos-git=\(ravynosGitSHA)") }
        lines.append("date=\(ISO8601DateFormatter().string(from: date))")
        return lines.joined(separator: "\n") + "\n"
    }
}
