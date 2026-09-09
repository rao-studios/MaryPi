import Foundation
import Testing
@testable import MaryOSKit

@Suite struct DistroConfigTests {
    static let sample = """
    # comment
    DISTRO_NAME=MaryOS
    DISTRO_ID=maryos
    DISTRO_VERSION=24.04
    DISTRO_CODENAME=noble
    DISTRO_HOME_URL=https://example.invalid/maryos
    BASE_SUITE=noble
    BASE_MIRROR=http://ports.ubuntu.com/ubuntu-ports
    BASE_COMPONENTS="main restricted universe multiverse"
    ARCH=arm64
    DEFAULT_USER=mary
    DEFAULT_PASSWORD='pa#ss'
    HOSTNAME=maryos   # trailing comment
    LOCALE=en_US.UTF-8
    TIMEZONE=UTC
    BOOT_LABEL=MARYOS
    ROOT_LABEL=maryos-root
    BOOT_PARTITION_MIB=512
    ROOT_MIN_MIB=2048
    VM_DISK_GIB=8
    """

    @Test func parsesTheShippedFormat() throws {
        let config = try DistroConfig(conf: ConfParser.parse(Self.sample))
        #expect(config.name == "MaryOS")
        #expect(config.baseComponents == "main restricted universe multiverse")
        #expect(config.defaultPassword == "pa#ss")
        #expect(config.hostname == "maryos")
        #expect(config.bootPartitionMiB == 512)
        #expect(config.vmDiskBytes == 8 << 30)
        #expect(config.imageName(for: .pi5) == "maryos-24.04-pi5.img")
        #expect(config.imageName(for: .vm) == "maryos-24.04-vm.img")
        #expect(config.vmCommandLine == "console=hvc0 root=LABEL=maryos-root rootfstype=ext4 rw rootwait")
        #expect(config.vmCommandLine(mode: .console) == config.vmCommandLine)
        #expect(config.vmCommandLine(mode: .desktop(dev: false)) == "console=hvc0 root=LABEL=maryos-root rootfstype=ext4 rw rootwait systemd.unit=graphical.target")
        #expect(config.vmCommandLine(mode: .desktop(dev: true)) == "console=hvc0 root=LABEL=maryos-root rootfstype=ext4 rw rootwait systemd.unit=graphical.target maryos.ui=dev")
        #expect(config.prettyName == "MaryOS 24.04")
        #expect(config.codenamePretty == "noble", "the pretty codename defaults to the codename")
        #expect(config.fullName == "MaryOS 24.04 (noble)")
        let named = try DistroConfig(conf: ConfParser.parse(Self.sample + "\nDISTRO_CODENAME_PRETTY=Noble Numbat\n"))
        #expect(named.fullName == "MaryOS 24.04 (Noble Numbat)")
    }

    @Test func requiresEveryKey() {
        for key in DistroConfig.requiredKeys {
            let text = Self.sample.split(separator: "\n").filter { !$0.hasPrefix(key + "=") }.joined(separator: "\n")
            #expect(throws: MaryOSError.self, "\(key) should be required") {
                _ = try DistroConfig(conf: ConfParser.parse(text))
            }
        }
    }

    @Test func rejectsBadValues() {
        #expect(throws: MaryOSError.self) {
            _ = try DistroConfig(conf: ConfParser.parse(Self.sample.replacingOccurrences(of: "VM_DISK_GIB=8", with: "VM_DISK_GIB=eight")))
        }
        #expect(throws: MaryOSError.self) {
            _ = try DistroConfig(conf: ConfParser.parse(Self.sample.replacingOccurrences(of: "DISTRO_ID=maryos", with: "DISTRO_ID=Mary OS")))
        }
        #expect(throws: MaryOSError.self) {
            _ = try ConfParser.parse("KEY=\"unterminated")
        }
        #expect(throws: MaryOSError.self) {
            _ = try ConfParser.parse("no equals sign")
        }
    }

    @Test func theRealDistroConfParses() throws {
        let paths = try #require(KitPaths.locate(environment: [:], currentDirectory: URL(fileURLWithPath: "/"), bundleResources: nil))
        let config = try paths.loadConfig()
        #expect(config.id == "maryos")
        #expect(config.arch == "arm64")
        #expect(config.baseSuite == "noble")
        #expect(config.version == "0.0")
        #expect(config.codename == "liquid-platinum")
        #expect(config.fullName == "MaryOS 0.0 (Liquid Platinum)")
        #expect(config.imageName(for: .vm) == "maryos-0.0-vm.img")
    }
}
