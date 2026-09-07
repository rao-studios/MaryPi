import Foundation
import Testing
@testable import MaryPiKit

@Suite struct ConfigTxtTests {
    @Test func keepsUpstreamAndAppendsBlock() throws {
        let upstream = try Fixtures.text("rpi5-uefi-config", "txt")
        let rendered = ConfigTxt.render(upstream: upstream, marypiVersion: "9.9.9")
        #expect(rendered.hasPrefix(upstream.trimmingCharacters(in: .newlines) + "\n"))
        #expect(rendered.contains("armstub=RPI_EFI.fd"))
        #expect(rendered.contains(ConfigTxt.marker(version: "9.9.9")))
        #expect(rendered.contains("\nenable_uart=1\n"))
        #expect(rendered.contains("\nuart_2ndstage=1\n"))
    }

    @Test func isIdempotent() throws {
        let upstream = try Fixtures.text("rpi5-uefi-config", "txt")
        let once = ConfigTxt.render(upstream: upstream, marypiVersion: "1.0.0")
        let twice = ConfigTxt.render(upstream: once, marypiVersion: "1.0.0")
        #expect(once == twice)
        #expect(twice.components(separatedBy: "enable_uart=1").count == 2)
    }

    @Test func doesNotOverrideUpstreamKeys() {
        let upstream = "enable_uart=0\narmstub=RPI_EFI.fd\n"
        let rendered = ConfigTxt.render(upstream: upstream, marypiVersion: "1.0.0")
        #expect(rendered.contains("enable_uart=0"))
        #expect(!rendered.contains("\nenable_uart=1"))
        #expect(rendered.contains("# enable_uart already set above"))
        #expect(rendered.contains("uart_2ndstage=1"))
    }

    @Test func parsesKeys() {
        let keys = ConfigTxt.keys(in: "# comment\n[pi5]\nfoo=1\n bar = 2\n\nbaz\n")
        #expect(keys == ["foo", "bar"])
    }
}
