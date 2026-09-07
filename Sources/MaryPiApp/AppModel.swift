import AppKit
import Foundation
import MaryPiKit
import Observation

@MainActor
@Observable
final class AppModel {
    var disks: [DiskInfo] = []
    var selectedDiskID: String?
    var payload: PayloadReport?
    var tree: BuildTree
    var doctorChecks: [DoctorCheck] = []
    var firmwareVersion: String
    var isScanningDisks = false
    var showConfirm = false
    var showDoctor = false
    var lastError: String?
    var lastDiskEvent: String?

    let coordinator: PrepareCoordinator

    private let diskUtil = DiskUtil()
    private let monitor = DiskArbitrationMonitor()
    private var monitorTask: Task<Void, Never>?
    private var pendingRefresh: Task<Void, Never>?

    init() {
        tree = BuildTree.locate()
        firmwareVersion = (try? FirmwareManifest.bundled())?.rpi5UEFI?.version ?? "?"
        coordinator = PrepareCoordinator()
    }

    var selectedDisk: DiskInfo? {
        disks.first { $0.bsdName == selectedDiskID }
    }

    var hasBlockingDoctorFailure: Bool {
        doctorChecks.contains(where: \.isBlockingFailure)
    }

    var canPrepare: Bool {
        selectedDisk != nil && !coordinator.isRunning && !hasBlockingDoctorFailure
    }

    var canBuildImage: Bool {
        !coordinator.isRunning && !hasBlockingDoctorFailure
    }

    func start() async {
        rescanPayload()
        await refreshDisks()
        await runDoctor()
        startMonitor()
    }

    private func startMonitor() {
        guard monitorTask == nil else { return }
        monitor.start()
        monitorTask = Task { [weak self] in
            guard let self else { return }
            for await event in monitor.events {
                switch event {
                case .appeared(let name): lastDiskEvent = "\(name ?? "disk") appeared"
                case .disappeared(let name): lastDiskEvent = "\(name ?? "disk") removed"
                case .changed(let name): lastDiskEvent = "\(name ?? "disk") changed"
                }
                scheduleRefresh()
            }
        }
    }

    /// Debounce bursts of DiskArbitration events into one diskutil scan.
    private func scheduleRefresh() {
        pendingRefresh?.cancel()
        pendingRefresh = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled else { return }
            await self?.refreshDisks()
        }
    }

    func refreshDisks() async {
        guard !isScanningDisks else { return }
        isScanningDisks = true
        defer { isScanningDisks = false }
        do {
            let found = try await diskUtil.candidates()
            disks = found
            if let selected = selectedDisk, !found.contains(where: { $0.bsdName == selected.bsdName && $0.totalSize == selected.totalSize }) {
                selectedDiskID = nil
            }
            lastError = nil
        } catch {
            lastError = "Could not list disks: \(error.localizedDescription)"
        }
    }

    func rescanPayload() {
        tree = BuildTree.locate()
        payload = PayloadResolver().evaluate(tree, firmwareVersion: firmwareVersion)
    }

    func runDoctor() async {
        doctorChecks = await Doctor().run(tree: tree, manifest: try? FirmwareManifest.bundled(), cache: FirmwareCache())
    }

    func chooseRavynOSRoot() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Use Checkout"
        panel.message = "Choose the ravynOS source checkout (the folder containing Kernel/xnu)."
        guard panel.runModal() == .OK, let url = panel.url else { return }
        var settings = Settings.load()
        settings.ravynosRoot = url.path
        do {
            try settings.save()
        } catch {
            lastError = "Could not save settings: \(error.localizedDescription)"
        }
        rescanPayload()
        Task { await runDoctor() }
    }

    func prepareSelected() async {
        guard let disk = selectedDisk else { return }
        await coordinator.prepare(target: disk, tree: tree)
        await refreshDisks()
    }

    func buildImageOnly() async {
        await coordinator.prepare(target: nil, tree: tree)
    }

    func revealImage() {
        guard let url = coordinator.imageURL else { return }
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    var statusLine: String {
        switch coordinator.phase {
        case .idle:
            if let disk = selectedDisk {
                return "Ready to prepare /dev/\(disk.bsdName) (\(disk.displayName), \(disk.sizeDescription))."
            }
            return disks.isEmpty ? "Insert an SD card or USB drive (4 GB to 2 TB)." : "Select the card to prepare."
        case .running:
            if let progress = coordinator.flashProgress {
                return "Writing card: \(Int(progress.fraction * 100))% (\(ByteCountFormatter.string(fromByteCount: progress.bytesWritten, countStyle: .file)))"
            }
            return "Working…"
        case .finished(let image, let flashed):
            if flashed {
                return "Ejected. Insert the card into the Raspberry Pi 5, connect the 3-pin UART at 115200 8N1, and power on."
            }
            return "Image built: \(image.path)"
        case .failed(let message):
            return "Failed: \(message)"
        }
    }
}
