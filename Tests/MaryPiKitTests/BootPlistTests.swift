import Foundation
import Testing
@testable import MaryPiKit

@Suite struct BootPlistTests {
    @Test func roundTrips() throws {
        let plist = BootPlist(kernel: BootPlist.defaultKernelPath, kernelFlags: "-v serial=3 debug=0x14e cpus=1")
        let data = try plist.plistData()
        let decoded = try BootPlist(plistData: data)
        #expect(decoded == plist)
        let dictionary = try Plist.dictionary(from: data)
        #expect(dictionary["Kernel"] as? String == "\\ravynos\\kernel")
        #expect(dictionary["Kernel Flags"] as? String == "-v serial=3 debug=0x14e cpus=1")
    }

    @Test func defaultFlags() {
        #expect(ImageSpec.defaultKernelFlags(level: .bootstrapOnly, qemuVirt: false) == "-v serial=3 debug=0x8 cpus=1")
        #expect(ImageSpec.defaultKernelFlags(level: .fullSystem, qemuVirt: false).hasSuffix("rd=disk0s2"))
        #expect(!ImageSpec.defaultKernelFlags(level: .fullSystem, qemuVirt: true).contains("rd="))
    }

    @Test func markerRenders() {
        let marker = MaryPiMarker(level: .kernelBringUp, marypiVersion: "0.1.0", firmwareVersion: "v0.3", kernelSHA256: "abc", ravynosGitSHA: "deadbeef", date: Date(timeIntervalSince1970: 0))
        let text = marker.render()
        #expect(text.contains("level=1\n"))
        #expect(text.contains("firmware=rpi5-uefi v0.3\n"))
        #expect(text.contains("kernel-sha256=abc\n"))
        #expect(text.contains("ravynos-git=deadbeef\n"))
        #expect(text.contains("date=1970-01-01T00:00:00Z\n"))
    }
}
