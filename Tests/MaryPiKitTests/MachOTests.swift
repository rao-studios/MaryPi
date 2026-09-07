import Foundation
import Testing
@testable import MaryPiKit

@Suite struct MachOTests {
    @Test func detectsARM64() {
        #expect(MachO.architectures(in: Fixtures.machO64(cputype: Fixtures.arm64)) == [.arm64])
    }

    @Test func detectsARM64e() {
        #expect(MachO.architectures(in: Fixtures.machO64(cputype: Fixtures.arm64, cpusubtype: 0x8000_0002)) == [.arm64e])
    }

    @Test func detectsX86_64() {
        #expect(MachO.architectures(in: Fixtures.machO64(cputype: Fixtures.x86_64)) == [.x86_64])
    }

    @Test func detectsFat() {
        let archs = MachO.architectures(in: Fixtures.fat([(Fixtures.x86_64, 3), (Fixtures.arm64, 0)]))
        #expect(archs == [.x86_64, .arm64])
    }

    @Test func ignoresNonMachO() {
        #expect(MachO.architectures(in: Data("not a kernel at all".utf8)).isEmpty)
        #expect(MachO.architectures(in: Data()).isEmpty)
    }

    @Test func readsFromFile() throws {
        let dir = try Fixtures.temporaryDirectory()
        let url = dir.appending(path: "kernel")
        try Fixtures.write(Fixtures.machO64(cputype: Fixtures.arm64), to: url)
        #expect(MachO.isARM64(url))
        #expect(try MachO.architectures(at: url) == [.arm64])
        try FileManager.default.removeItem(at: dir)
    }
}
