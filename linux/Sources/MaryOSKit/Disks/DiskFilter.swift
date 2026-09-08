import Foundation

/// Decides which disks MaryOS is willing to erase.
public enum DiskFilter {
    public static let minimumSize: Int64 = 4 * 1024 * 1024 * 1024
    public static let maximumSize: Int64 = 2 * 1024 * 1024 * 1024 * 1024

    public static func isCandidateTarget(_ disk: DiskInfo) -> Bool {
        rejectionReason(disk) == nil
    }

    /// Why a disk is not offered as a target, or nil when it is.
    public static func rejectionReason(_ disk: DiskInfo) -> String? {
        if !disk.isWholeDisk { return "not a whole disk" }
        if !disk.isPhysical { return "virtual disk" }
        if disk.isInternal && !(disk.busKind == .sdCard && disk.isRemovableMedia == true) {
            return "internal disk"
        }
        if !disk.isExternal { return "not external or removable" }
        if disk.totalSize < minimumSize { return "smaller than 4 GB" }
        if disk.totalSize > maximumSize { return "larger than 2 TB" }
        switch disk.busKind {
        case .sdCard, .usb, .thunderbolt:
            return nil
        case .other:
            return "unsupported bus \(disk.busProtocol ?? "unknown")"
        }
    }
}
