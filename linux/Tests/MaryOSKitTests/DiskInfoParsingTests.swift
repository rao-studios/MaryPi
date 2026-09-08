import Foundation
import Testing
@testable import MaryOSKit

@Suite struct DiskInfoParsingTests {
    @Test func decodesSDCard() throws {
        let disk = try DiskInfo(diskutilInfoPlist: Fixtures.data("diskutil-info-sdcard", "plist"))
        #expect(disk.bsdName == "disk4")
        #expect(disk.deviceNode == "/dev/disk4")
        #expect(disk.rawDeviceNode == "/dev/rdisk4")
        #expect(disk.totalSize == 31_914_983_424)
        #expect(disk.busKind == .sdCard)
        #expect(disk.isExternal)
        #expect(disk.isPhysical)
        #expect(disk.isWholeDisk)
        #expect(disk.displayName == "SD Card Reader Media")
    }

    @Test func decodesUSB() throws {
        let disk = try DiskInfo(diskutilInfoPlist: Fixtures.data("diskutil-info-usb", "plist"))
        #expect(disk.busKind == .usb)
        #expect(disk.displayName == "SanDisk Ultra")
        #expect(disk.isExternal)
    }

    @Test func decodesInternalSSDFromThisMac() throws {
        let disk = try DiskInfo(diskutilInfoPlist: Fixtures.data("diskutil-info-internal-ssd", "plist"))
        #expect(disk.bsdName == "disk0")
        #expect(disk.isInternal)
        #expect(disk.isWholeDisk)
    }

    @Test func decodesDiskList() throws {
        let list = try DiskList(diskutilListPlist: Fixtures.data("diskutil-list", "plist"))
        #expect(list.wholeDisks.contains("disk0"))
        #expect(list.wholeDisks.allSatisfy { DiskUtil.isValidDiskIdentifier($0) })
    }

    @Test func identifierValidation() {
        #expect(DiskUtil.isValidDiskIdentifier("disk4"))
        #expect(DiskUtil.isValidDiskIdentifier("disk12"))
        #expect(!DiskUtil.isValidDiskIdentifier("disk4s1"))
        #expect(!DiskUtil.isValidDiskIdentifier("/dev/disk4"))
        #expect(!DiskUtil.isValidDiskIdentifier("disk"))
        #expect(!DiskUtil.isValidDiskIdentifier("disk4; rm -rf /"))
        #expect(DiskUtil.isValidPartitionIdentifier("disk4s2"))
        #expect(DiskUtil.normalize("/dev/disk4") == "disk4")
    }
}
