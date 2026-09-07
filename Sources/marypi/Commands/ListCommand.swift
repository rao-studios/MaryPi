import ArgumentParser
import Foundation
import MaryPiKit

struct ListCommand: AsyncParsableCommand {
    static let configuration = CommandConfiguration(commandName: "list", abstract: "List disks MaryPi is willing to write to.")

    @Flag(name: .long, help: "Show every whole disk with the reason it is or is not a candidate.")
    var all = false

    @Flag(name: .long, help: "Print JSON.")
    var json = false

    func run() async throws {
        let diskUtil = DiskUtil()
        let disks = try await (all ? diskUtil.allWholeDisks() : diskUtil.candidates())
        if json {
            try Output.json(disks)
            return
        }
        if disks.isEmpty {
            Output.line(all ? "No disks found." : "No removable disks found. Insert an SD card or USB drive (4 GB to 2 TB).")
            return
        }
        for disk in disks {
            let bus = disk.busProtocol ?? "unknown bus"
            var line = "/dev/\(disk.bsdName)  \(disk.sizeDescription.padding(toLength: 10, withPad: " ", startingAt: 0))  \(bus.padding(toLength: 16, withPad: " ", startingAt: 0))  \(disk.displayName)"
            if all, let reason = DiskFilter.rejectionReason(disk) {
                line += "  [skipped: \(reason)]"
            } else if all {
                line += "  [candidate]"
            }
            Output.line(line)
        }
    }
}
