import Foundation

/// The kind of bus a disk hangs off, derived from diskutil's BusProtocol.
public enum BusKind: String, Sendable, Codable, Hashable {
    case sdCard
    case usb
    case thunderbolt
    case other
}

/// A whole disk or partition as reported by `diskutil info -plist`.
///
/// Only the keys MaryPi needs are decoded; everything else in the plist is
/// ignored. Optional keys are optional because diskutil omits them for some
/// device classes and macOS versions.
public struct DiskInfo: Codable, Identifiable, Sendable, Hashable {
    public var id: String { bsdName }

    public let bsdName: String
    public let deviceNode: String
    public let mediaName: String?
    public let ioRegistryName: String?
    public let totalSize: Int64
    public let busProtocol: String?
    public let isInternal: Bool
    public let isRemovableOrExternal: Bool?
    public let isRemovableMedia: Bool?
    public let isEjectable: Bool?
    public let isWholeDisk: Bool
    public let virtualOrPhysical: String?
    public let mountPoint: String?
    public let volumeName: String?
    public let content: String?
    public let parentWholeDisk: String?
    public let solidState: Bool?

    enum CodingKeys: String, CodingKey {
        case bsdName = "DeviceIdentifier"
        case deviceNode = "DeviceNode"
        case mediaName = "MediaName"
        case ioRegistryName = "IORegistryEntryName"
        case totalSize = "TotalSize"
        case busProtocol = "BusProtocol"
        case isInternal = "Internal"
        case isRemovableOrExternal = "RemovableMediaOrExternalDevice"
        case isRemovableMedia = "RemovableMedia"
        case isEjectable = "Ejectable"
        case isWholeDisk = "WholeDisk"
        case virtualOrPhysical = "VirtualOrPhysical"
        case mountPoint = "MountPoint"
        case volumeName = "VolumeName"
        case content = "Content"
        case parentWholeDisk = "ParentWholeDisk"
        case solidState = "SolidState"
    }

    public init(
        bsdName: String,
        deviceNode: String,
        mediaName: String? = nil,
        ioRegistryName: String? = nil,
        totalSize: Int64,
        busProtocol: String? = nil,
        isInternal: Bool,
        isRemovableOrExternal: Bool? = nil,
        isRemovableMedia: Bool? = nil,
        isEjectable: Bool? = nil,
        isWholeDisk: Bool,
        virtualOrPhysical: String? = "Physical",
        mountPoint: String? = nil,
        volumeName: String? = nil,
        content: String? = nil,
        parentWholeDisk: String? = nil,
        solidState: Bool? = nil
    ) {
        self.bsdName = bsdName
        self.deviceNode = deviceNode
        self.mediaName = mediaName
        self.ioRegistryName = ioRegistryName
        self.totalSize = totalSize
        self.busProtocol = busProtocol
        self.isInternal = isInternal
        self.isRemovableOrExternal = isRemovableOrExternal
        self.isRemovableMedia = isRemovableMedia
        self.isEjectable = isEjectable
        self.isWholeDisk = isWholeDisk
        self.virtualOrPhysical = virtualOrPhysical
        self.mountPoint = mountPoint
        self.volumeName = volumeName
        self.content = content
        self.parentWholeDisk = parentWholeDisk
        self.solidState = solidState
    }

    /// Decode the output of `diskutil info -plist <disk>`.
    public init(diskutilInfoPlist data: Data) throws {
        self = try Plist.decode(DiskInfo.self, from: data)
    }

    /// Raw device node (`/dev/rdiskN`) used for fast sequential writes.
    public var rawDeviceNode: String {
        deviceNode.replacingOccurrences(of: "/dev/disk", with: "/dev/rdisk")
    }

    public var isPhysical: Bool {
        (virtualOrPhysical ?? "Physical").caseInsensitiveCompare("Physical") == .orderedSame
    }

    /// True when the disk is on an external bus or is removable media.
    public var isExternal: Bool {
        if let isRemovableOrExternal { return isRemovableOrExternal }
        if isRemovableMedia == true || isEjectable == true { return true }
        return !isInternal
    }

    public var busKind: BusKind {
        let proto = (busProtocol ?? "").lowercased()
        if proto.contains("secure digital") || proto == "sd" || proto.contains("sdxc") || proto.contains("sdhc") {
            return .sdCard
        }
        if proto.contains("usb") { return .usb }
        if proto.contains("thunderbolt") { return .thunderbolt }
        return .other
    }

    public var displayName: String {
        if let mediaName, !mediaName.isEmpty { return mediaName }
        if let ioRegistryName, !ioRegistryName.isEmpty { return ioRegistryName }
        return bsdName
    }

    public var sizeDescription: String {
        ByteCountFormatter.string(fromByteCount: totalSize, countStyle: .file)
    }
}

/// The subset of `diskutil list -plist` MaryPi uses.
public struct DiskList: Codable, Sendable {
    public let wholeDisks: [String]
    public let allDisks: [String]?

    enum CodingKeys: String, CodingKey {
        case wholeDisks = "WholeDisks"
        case allDisks = "AllDisks"
    }

    public init(diskutilListPlist data: Data) throws {
        self = try Plist.decode(DiskList.self, from: data)
    }
}
