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

    // VM test bed
    let vm: VMSession?
    var vmHost: VMHost?
    var vmProfile = "qemu-virt"
    var vmProfiles: [String] = []
    private let vmPaths: VMPaths?

    private let diskUtil = DiskUtil()
    private let monitor = DiskArbitrationMonitor()
    private var monitorTask: Task<Void, Never>?
    private var pendingRefresh: Task<Void, Never>?

    init() {
        tree = BuildTree.locate()
        firmwareVersion = (try? FirmwareManifest.bundled())?.rpi5UEFI?.version ?? "?"
        coordinator = PrepareCoordinator()
        vmPaths = VMPaths.locate(bundleResources: Bundle.main.resourceURL)
        vm = vmPaths.map { VMSession(paths: $0.state(for: "qemu-virt")) }
        vmProfiles = vmPaths?.profileNames() ?? []
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
        vmHost = await VMHost.detect(probe: .live())
        startMonitor()
    }

    // MARK: - VM test bed

    var canTestInVM: Bool {
        canBuildImage && vmHost?.qemu != nil && vmHost?.firmware != nil && vm != nil && !(vm?.isRunning ?? false)
    }

    var vmIsRunning: Bool { vm?.isRunning ?? false }

    var vmProfileSummary: String {
        guard let vmPaths, let host = vmHost, let profile = try? vmPaths.loadProfile(vmProfile) else { return "" }
        let accel = (try? host.resolveAccel(requested: nil, profile: profile)) ?? .tcg
        return "\(profile.name): GICv\(profile.gicVersion), \(accel.rawValue), \(host.displayBackend(for: .window).backend.rawValue)"
    }

    /// Build a QEMU image into the VM state and boot it in a QEMU window.
    func testInVM() async {
        guard let vmPaths, let host = vmHost, let qemu = host.qemu, let firmware = host.firmware else {
            lastError = "QEMU or its firmware is missing; see Doctor"
            return
        }
        guard let vm else { return }
        do {
            let profile = try vmPaths.loadProfile(vmProfile)
            let state = vmPaths.state(for: profile.name)
            try FileManager.default.createDirectory(at: state.directory, withIntermediateDirectories: true)
            await coordinator.prepare(target: nil, tree: tree, options: PrepareOptions(qemuVirt: true, outputURL: state.disk))
            guard case .finished(let image, _) = coordinator.phase else { return }

            let accel = try host.resolveAccel(requested: nil, profile: profile)
            let (mode, backend) = host.displayBackend(for: .window)
            let spec = QEMULaunchSpec(profile: profile, image: image, firmware: firmware, state: state, accel: accel,
                                      cpu: host.cpu(for: accel, profile: profile), display: mode, displayBackend: backend,
                                      serialOnTerminal: false)
            let command = QEMUCommand(qemu: qemu, spec: spec)
            let coordinator = self.coordinator
            try VMController.prepareState(state, firmware: firmware, image: image, accel: accel, backend: backend,
                                          log: { line in Task { @MainActor in coordinator.log(line) } })
            try vm.launch(command, serial: { line in
                Task { @MainActor in coordinator.log("serial| \(line)") }
            }, log: { line in
                Task { @MainActor in coordinator.log(line) }
            })
            lastError = nil
        } catch {
            lastError = "VM: \(error.localizedDescription)"
            coordinator.log("VM error: \(error.localizedDescription)")
        }
    }

    func stopVM() async {
        guard let vm else { return }
        let coordinator = self.coordinator
        await vm.stop(log: { line in Task { @MainActor in coordinator.log(line) } })
    }

    func revealSerialLog() {
        guard let vm else { return }
        NSWorkspace.shared.activateFileViewerSelecting([vm.paths.serialLog])
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
        if let vm {
            switch vm.state {
            case .starting: return "Starting the VM…"
            case .running(let pid): return "VM running (pid \(pid), \(vmProfileSummary)). Serial: \(vm.paths.serialLog.path)"
            case .stopping: return "Stopping the VM…"
            case .exited(let status): return "VM exited with status \(status). Serial log: \(vm.paths.serialLog.path)"
            case .failed(let message): return "VM failed: \(message)"
            case .idle: break
            }
        }
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
