import Foundation
import Testing
@testable import MaryPiKit

@Suite struct DiskFilterTests {
    @Test func acceptsSDCard() throws {
        let disk = try DiskInfo(diskutilInfoPlist: Fixtures.data("diskutil-info-sdcard", "plist"))
        #expect(DiskFilter.isCandidateTarget(disk))
        #expect(DiskFilter.rejectionReason(disk) == nil)
    }

    @Test func acceptsUSB() throws {
        let disk = try DiskInfo(diskutilInfoPlist: Fixtures.data("diskutil-info-usb", "plist"))
        #expect(DiskFilter.isCandidateTarget(disk))
    }

    @Test func rejectsInternalSSD() throws {
        let disk = try DiskInfo(diskutilInfoPlist: Fixtures.data("diskutil-info-internal-ssd", "plist"))
        #expect(!DiskFilter.isCandidateTarget(disk))
        #expect(DiskFilter.rejectionReason(disk) != nil)
    }

    @Test func rejectsTinyCard() throws {
        let disk = try DiskInfo(diskutilInfoPlist: Fixtures.data("diskutil-info-tiny-sdcard", "plist"))
        #expect(DiskFilter.rejectionReason(disk) == "smaller than 4 GB")
    }

    @Test func rejectsVirtualDisk() throws {
        let disk = try DiskInfo(diskutilInfoPlist: Fixtures.data("diskutil-info-virtual", "plist"))
        #expect(DiskFilter.rejectionReason(disk) == "virtual disk")
    }

    @Test func rejectsPartition() {
        let partition = DiskInfo(bsdName: "disk4s1", deviceNode: "/dev/disk4s1", totalSize: 8 << 30, busProtocol: "USB", isInternal: false, isRemovableOrExternal: true, isWholeDisk: false)
        #expect(DiskFilter.rejectionReason(partition) == "not a whole disk")
    }

    @Test func acceptsInternalSDReaderWithRemovableMedia() {
        let disk = DiskInfo(bsdName: "disk6", deviceNode: "/dev/disk6", mediaName: "SD Card", totalSize: 16 << 30, busProtocol: "Secure Digital", isInternal: true, isRemovableOrExternal: true, isRemovableMedia: true, isWholeDisk: true)
        #expect(DiskFilter.isCandidateTarget(disk))
    }
}
