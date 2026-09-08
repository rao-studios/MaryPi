import AppKit
import Foundation
import MaryOSKit
import Observation

@MainActor
@Observable
final class AppModel {
    var disks: [DiskInfo] = []
    var selectedDiskID: String?
    var paths: KitPaths?
    var config: DistroConfig?
    var configError: String?
    var artifacts: [ImageTarget: BuildArtifacts] = [:]
    var doctorChecks: [DoctorCheck] = []
    var isScanningDisks = false
    var showConfirm = false
    var showDoctor = false
    var lastError: String?
    var lastDiskEvent: String?
    /// Set by the main window so the model can open the VM window.
    var openVMWindow: (@MainActor () -> Void)?

    let coordinator = PrepareCoordinator()
    private(set) var vm: VMRunner?
    private(set) var vmSpec: VMSpec?

    private let diskUtil = DiskUtil()
    private let monitor = DiskArbitrationMonitor()
    private var monitorTask: Task<Void, Never>?
    private var pendingRefresh: Task<Void, Never>?

    init() {
        locateKit()
    }

    func locateKit() {
        let settings = Settings.load()
        paths = KitPaths.locate(explicitRoot: settings.kitDirectory, bundleResources: Bundle.main.resourceURL)
        config = nil
        configError = nil
        if let paths {
            do {
                config = try paths.loadConfig()
            } catch {
                configError = error.localizedDescription
            }
        } else {
            configError = "Kit directory not found. Choose the linux/ directory of a MaryPi checkout (Card > Choose Kit Directory…)."
        }
        refreshArtifacts()
    }

    func refreshArtifacts() {
        guard let paths, let config else {
            artifacts = [:]
            return
        }
        artifacts = Dictionary(uniqueKeysWithValues: ImageTarget.allCases.map { ($0, BuildArtifacts.locate(target: $0, config: config, paths: paths)) })
    }

    var selectedDisk: DiskInfo? {
        disks.first { $0.bsdName == selectedDiskID }
    }

    var hasBlockingDoctorFailure: Bool {
        doctorChecks.contains(where: \.isBlockingFailure)
    }

    var kitReady: Bool { paths != nil && config != nil }
    var canBuild: Bool { kitReady && !coordinator.isRunning }
    var canPrepare: Bool { canBuild && selectedDisk != nil && !hasBlockingDoctorFailure }
    var vmIsRunning: Bool { vm?.isActive ?? false }
    var canTestInVM: Bool { canBuild && !vmIsRunning }
    var pi5ImageBuilt: Bool { artifacts[.pi5]?.isComplete ?? false }
    var vmImageBuilt: Bool { artifacts[.vm]?.isComplete ?? false }

    func start() async {
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

    func runDoctor() async {
        doctorChecks = await Doctor().run(paths: paths)
    }

    func chooseKitDirectory() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Use Kit"
        panel.message = "Choose the MaryOS kit directory (linux/ in a MaryPi checkout, containing distro/ and builder/)."
        guard panel.runModal() == .OK, let url = panel.url else { return }
        var settings = Settings.load()
        settings.kitDirectory = url.path
        do {
            try settings.save()
        } catch {
            lastError = "Could not save settings: \(error.localizedDescription)"
        }
        locateKit()
        Task { await runDoctor() }
    }

    func buildImage(_ target: ImageTarget) async {
        guard let paths, let config else { return }
        await coordinator.prepare(target: target, disk: nil, paths: paths, config: config, options: PrepareOptions(build: true))
        refreshArtifacts()
    }

    /// Write the Raspberry Pi 5 image to the selected disk, building it first when there is none.
    func prepareSelected() async {
        guard let disk = selectedDisk, let paths, let config else { return }
        await coordinator.prepare(target: .pi5, disk: disk, paths: paths, config: config, options: PrepareOptions(build: !pi5ImageBuilt))
        refreshArtifacts()
        await refreshDisks()
    }

    /// Boot the VM image (building it first when there is none) and open the VM window.
    @discardableResult
    func testInVM() async -> Bool {
        guard let paths, let config, !vmIsRunning else { return false }
        var vmArtifacts = artifacts[.vm] ?? BuildArtifacts.locate(target: .vm, config: config, paths: paths)
        if !vmArtifacts.isComplete {
            await coordinator.prepare(target: .vm, disk: nil, paths: paths, config: config, options: PrepareOptions(build: true))
            refreshArtifacts()
            guard case .finished = coordinator.phase, let built = artifacts[.vm], built.isComplete else { return false }
            vmArtifacts = built
        }
        let coordinator = self.coordinator
        let log: Logger = { line in Task { @MainActor in coordinator.log(line) } }
        do {
            let state = paths.state(for: .vm)
            if let pid = VMController.runningPID(state) {
                lastError = "A VM is already running (pid \(pid)); stop it first (maryos vm stop)."
                return false
            }
            try FileManager.default.createDirectory(at: paths.outDirectory, withIntermediateDirectories: true)
            try VMStateManager.prepare(state, artifacts: vmArtifacts, fresh: false, diskSize: config.vmDiskBytes, log: log)
            let boot = try VMBootInfo.load(from: state.bootInfo)
            let spec = VMSpec(
                name: config.fullName, cpus: VMSpec.defaultCPUs, memoryMiB: VMSpec.defaultMemoryMiB,
                disk: state.disk, kernel: state.kernel, initrd: state.initrd, commandLine: boot.cmdline,
                macAddress: try VMStateManager.macAddress(state),
                sharedDirectories: [SharedDirectory(tag: VMSpec.defaultShareTag, url: paths.outDirectory)]
            )
            let runner = VMRunner(paths: state)
            try runner.load(spec)
            vm = runner
            vmSpec = spec
            lastError = nil
            openVMWindow?()
            Task { @MainActor [weak self] in
                do {
                    try await runner.start(serial: { line in Task { @MainActor in coordinator.log("serial| \(line)") } }, log: log)
                } catch {
                    self?.lastError = "VM: \(error.localizedDescription)"
                    coordinator.log("VM error: \(error.localizedDescription)")
                }
            }
            return true
        } catch {
            lastError = "VM: \(error.localizedDescription)"
            coordinator.log("VM error: \(error.localizedDescription)")
            return false
        }
    }

    func stopVM() async {
        guard let vm else { return }
        let coordinator = self.coordinator
        await vm.stop(log: { line in Task { @MainActor in coordinator.log(line) } })
    }

    func resetVM() {
        guard let paths else { return }
        do {
            try VMStateManager.reset(paths.state(for: .vm))
            coordinator.log("VM disk removed; the next Test in VM starts from the built image")
        } catch {
            lastError = "Could not reset the VM disk: \(error.localizedDescription)"
        }
    }

    func revealOutput() {
        guard let paths else { return }
        try? FileManager.default.createDirectory(at: paths.outDirectory, withIntermediateDirectories: true)
        NSWorkspace.shared.activateFileViewerSelecting([paths.outDirectory])
    }

    func revealSerialLog() {
        guard let paths else { return }
        NSWorkspace.shared.activateFileViewerSelecting([paths.state(for: .vm).serialLog])
    }

    func revealImage() {
        guard let url = coordinator.imageURL else { return }
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    var statusLine: String {
        if let vm {
            switch vm.state {
            case .loaded, .starting: return "Starting the VM…"
            case .running: return "VM running (\(vm.spec?.summary ?? "")). Serial: \(vm.paths.serialLog.path)"
            case .stopping: return "Stopping the VM…"
            case .stopped(let why): return "VM stopped (\(why)). Serial log: \(vm.paths.serialLog.path)"
            case .failed(let message): return "VM failed: \(message)"
            case .idle: break
            }
        }
        switch coordinator.phase {
        case .idle:
            if let configError { return configError }
            if let disk = selectedDisk {
                return "Ready to prepare /dev/\(disk.bsdName) (\(disk.displayName), \(disk.sizeDescription))\(pi5ImageBuilt ? "." : "; the Pi 5 image will be built first.")"
            }
            return disks.isEmpty ? "Insert an SD card or USB drive (4 GB to 2 TB)." : "Select the card to prepare."
        case .running:
            if let progress = coordinator.flashProgress {
                return "Writing card: \(Int(progress.fraction * 100))% (\(ByteCountFormatter.string(fromByteCount: progress.bytesWritten, countStyle: .file)))"
            }
            if let running = coordinator.plan.steps.first(where: { $0.status == .running }) {
                return running.title + "…"
            }
            return "Working…"
        case .finished(let image, let flashed):
            if flashed {
                return "Ejected. Insert the card into the Raspberry Pi 5 and power on; the console is on HDMI and the 3-pin UART at 115200 8N1."
            }
            return "Image built: \(image.path)"
        case .failed(let message):
            return "Failed: \(message)"
        }
    }
}
