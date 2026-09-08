import ArgumentParser
import Foundation
import MaryOSKit

struct ListCommand: ParsableCommand {
    static let configuration = CommandConfiguration(commandName: "list", abstract: "List the removable disks MaryOS is willing to erase.")

    @Flag(name: .long, help: "Show every whole disk, with the reason it is not a target.")
    var all = false

    @Flag(name: .long, help: "Print JSON.")
    var json = false

    func run() throws {
        let includeAll = all
        let disks = try runBlocking { () async throws -> [DiskInfo] in
            let diskUtil = DiskUtil()
            return includeAll ? try await diskUtil.allWholeDisks() : try await diskUtil.candidates()
        }
        if json {
            try Output.json(disks)
            return
        }
        if disks.isEmpty {
            Output.line("No removable disks between 4 GB and 2 TB. Insert an SD card or USB drive.")
            return
        }
        for disk in disks {
            let reason = DiskFilter.rejectionReason(disk).map { "  [not a target: \($0)]" } ?? ""
            Output.line("/dev/\(disk.bsdName)  \(disk.displayName)  \(disk.sizeDescription)  \(disk.busProtocol ?? "unknown bus")\(reason)")
        }
    }
}
