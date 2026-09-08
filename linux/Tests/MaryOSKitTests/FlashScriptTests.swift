import Foundation
import Testing
@testable import MaryOSKit

@Suite struct FlashScriptTests {
    @Test func rendersExpectedCommands() throws {
        let script = try FlashScript.render(
            image: URL(fileURLWithPath: "/tmp/it's an image.img"),
            disk: "/dev/disk4",
            log: URL(fileURLWithPath: "/tmp/log.txt")
        )
        #expect(script.hasPrefix("#!/bin/sh\n"))
        #expect(script.contains("IMG='/tmp/it'\\''s an image.img'"))
        #expect(script.contains("DEV='disk4'"))
        #expect(script.contains("/usr/sbin/diskutil unmountDisk force \"/dev/$DEV\""))
        #expect(script.contains("/bin/dd if=\"$IMG\" of=\"/dev/r$DEV\" bs=4m &"))
        #expect(script.contains("/bin/kill -INFO"))
        #expect(script.contains("/usr/sbin/diskutil eject \"/dev/$DEV\""))
        #expect(script.contains(FlashScript.endMarker))
    }

    @Test func rejectsNonWholeDisk() {
        #expect(throws: MaryOSError.self) {
            _ = try FlashScript.render(image: URL(fileURLWithPath: "/tmp/x.img"), disk: "disk4s1", log: URL(fileURLWithPath: "/tmp/l"))
        }
        #expect(throws: MaryOSError.self) {
            _ = try FlashScript.render(image: URL(fileURLWithPath: "/tmp/x.img"), disk: "disk4; reboot", log: URL(fileURLWithPath: "/tmp/l"))
        }
    }

    @Test func appleScriptSourceQuotesPath() {
        let source = PrivilegedRunner.source(script: URL(fileURLWithPath: "/tmp/a \"b\"/flash.sh"), prompt: "Hi")
        #expect(source.hasPrefix("do shell script \"/bin/sh \" & quoted form of \"/tmp/a \\\"b\\\"/flash.sh\""))
        #expect(source.hasSuffix("with prompt \"Hi\" with administrator privileges"))
    }

    @Test func parsesDDProgress() {
        #expect(Flasher.bytesTransferred(in: "517996544 bytes transferred in 12.345678 secs (41956890 bytes/sec)") == 517_996_544)
        #expect(Flasher.bytesTransferred(in: "123+0 records in") == nil)
        #expect(Flasher.bytesTransferred(in: "MARYOS:STEP write") == nil)
    }
}
