import Foundation

/// Everything `ImageBuilder` needs to produce one raw disk image.
public struct ImageSpec: Sendable {
    public static let fatLabel = "MARYPI"
    public static let rootLabel = "ravynOS"
    public static let mebibyte: Int64 = 1 << 20

    public var payload: PayloadReport
    public var firmware: FirmwareBundle
    public var outputURL: URL
    public var firmwarePartitionMiB: Int = 256
    public var kernelFlags: String
    public var qemuVirt: Bool
    public var ravynosGitSHA: String?

    public init(payload: PayloadReport, firmware: FirmwareBundle, outputURL: URL, kernelFlags: String? = nil, qemuVirt: Bool = false, ravynosGitSHA: String? = nil) {
        self.payload = payload
        self.firmware = firmware
        self.outputURL = outputURL
        self.qemuVirt = qemuVirt
        self.kernelFlags = kernelFlags ?? Self.defaultKernelFlags(level: payload.level, qemuVirt: qemuVirt)
        self.ravynosGitSHA = ravynosGitSHA
    }

    public var level: PayloadLevel { payload.level }
    public var includesBoot: Bool { level >= .kernelBringUp }
    public var includesRoot: Bool { level >= .fullSystem && !qemuVirt }
    public var targetName: String { qemuVirt ? "qemu-virt" : "raspberry-pi-5" }

    public static func defaultKernelFlags(level: PayloadLevel, qemuVirt: Bool) -> String {
        var flags = ["-v", "serial=3", "debug=0x8", "cpus=1"]
        if level >= .fullSystem && !qemuVirt { flags.append("rd=disk0s2") }
        return flags.joined(separator: " ")
    }

    /// Image size: 512 MiB unless a root filesystem is included, then the
    /// firmware partition plus max(1 GiB, 1.25 x sysroot) rounded to 64 MiB.
    public func imageSizeBytes(sysrootBytes: Int64) -> Int64 {
        guard includesRoot else { return 512 * Self.mebibyte }
        let root = max(1024 * Self.mebibyte, Int64(Double(sysrootBytes) * 1.25))
        let total = Int64(firmwarePartitionMiB) * Self.mebibyte + root
        let step = 64 * Self.mebibyte
        return ((total + step - 1) / step) * step
    }

    public static func defaultOutputURL(level: PayloadLevel, qemuVirt: Bool, in directory: URL, date: Date = Date()) -> URL {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmm"
        formatter.locale = Locale(identifier: "en_US_POSIX")
        let stamp = formatter.string(from: date)
        let target = qemuVirt ? "qemu-virt" : "pi5"
        return directory.appending(path: "ravynOS-\(target)-L\(level.rawValue)-\(stamp).img")
    }
}
