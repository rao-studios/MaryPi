import Foundation

/// Thin wrapper over `/usr/sbin/diskutil`.
public struct DiskUtil: Sendable {
    public static let executable = "/usr/sbin/diskutil"

    private let runner: CommandRunner

    public init(runner: CommandRunner = CommandRunner()) {
        self.runner = runner
    }

    /// `disk3` for `/dev/disk3` or `disk3`.
    public static func normalize(_ name: String) -> String {
        name.hasPrefix("/dev/") ? String(name.dropFirst(5)) : name
    }

    /// Whole-disk identifiers only: `disk0`, `disk12`.
    public static func isValidDiskIdentifier(_ name: String) -> Bool {
        name.wholeMatch(of: /disk[0-9]+/) != nil
    }

    /// Partition identifiers: `disk3s1`.
    public static func isValidPartitionIdentifier(_ name: String) -> Bool {
        name.wholeMatch(of: /disk[0-9]+s[0-9]+/) != nil
    }

    public func listWholeDisks() async throws -> [String] {
        let result = try await runner.run(Self.executable, ["list", "-plist"]).checkSuccess("diskutil list")
        return try DiskList(diskutilListPlist: result.stdout).wholeDisks
    }

    public func info(_ bsdName: String) async throws -> DiskInfo {
        let name = Self.normalize(bsdName)
        let result = try await runner.run(Self.executable, ["info", "-plist", name]).checkSuccess("diskutil info \(name)")
        return try DiskInfo(diskutilInfoPlist: result.stdout)
    }

    /// Every whole disk that `DiskFilter` accepts as a flash target.
    public func candidates() async throws -> [DiskInfo] {
        try await allWholeDisks().filter { DiskFilter.isCandidateTarget($0) }
    }

    /// Every whole disk, whether or not it is a sensible target.
    public func allWholeDisks() async throws -> [DiskInfo] {
        var disks: [DiskInfo] = []
        for name in try await listWholeDisks() {
            if let info = try? await info(name) {
                disks.append(info)
            }
        }
        return disks.sorted { $0.bsdName.localizedStandardCompare($1.bsdName) == .orderedAscending }
    }

    public func mountPoint(of bsdName: String) async throws -> String? {
        try await info(bsdName).mountPoint
    }

    public func mount(_ bsdName: String) async throws {
        try await runner.run(Self.executable, ["mount", Self.normalize(bsdName)]).checkSuccess("diskutil mount")
    }

    public func unmountDisk(_ bsdName: String, force: Bool = true) async throws {
        var args = ["unmountDisk"]
        if force { args.append("force") }
        args.append(Self.normalize(bsdName))
        try await runner.run(Self.executable, args).checkSuccess("diskutil unmountDisk")
    }

    public func eject(_ bsdName: String) async throws {
        try await runner.run(Self.executable, ["eject", Self.normalize(bsdName)]).checkSuccess("diskutil eject")
    }

    /// `diskutil partitionDisk <disk> <count> MBR <fs1> <name1> <size1> <fs2> <name2> R`
    public func partitionMBR(_ bsdName: String, partitions: [(filesystem: String, name: String, size: String)]) async throws {
        var args = ["partitionDisk", Self.normalize(bsdName), String(partitions.count), "MBR"]
        for partition in partitions {
            args += [partition.filesystem, partition.name, partition.size]
        }
        try await runner.run(Self.executable, args).checkSuccess("diskutil partitionDisk")
    }
}
