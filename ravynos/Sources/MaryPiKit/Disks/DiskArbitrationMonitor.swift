import Foundation
#if canImport(DiskArbitration)
import DiskArbitration

public enum DiskEvent: Sendable, Equatable {
    case appeared(String?)
    case disappeared(String?)
    case changed(String?)
}

#endif

/// Emits an event whenever a whole disk appears, disappears or changes.
/// Consumers re-run `DiskUtil.candidates()` in response; the event itself
/// carries only the BSD name for logging. Off macOS there is no
/// DiskArbitration and the stream simply never yields.
#if canImport(DiskArbitration)
public final class DiskArbitrationMonitor: @unchecked Sendable {
    public let events: AsyncStream<DiskEvent>

    private let continuation: AsyncStream<DiskEvent>.Continuation
    private let queue = DispatchQueue(label: "com.ravynos.MaryPi.diskarbitration")
    private let session: DASession?
    private var started = false

    public init() {
        var captured: AsyncStream<DiskEvent>.Continuation!
        events = AsyncStream(bufferingPolicy: .bufferingNewest(16)) { captured = $0 }
        continuation = captured
        session = DASessionCreate(kCFAllocatorDefault)
    }

    deinit {
        stop()
    }

    public func start() {
        guard let session, !started else { return }
        started = true
        let context = Unmanaged.passUnretained(self).toOpaque()
        let matching = [kDADiskDescriptionMediaWholeKey as String: true] as CFDictionary

        DARegisterDiskAppearedCallback(session, matching, { disk, context in
            guard let context else { return }
            let monitor = Unmanaged<DiskArbitrationMonitor>.fromOpaque(context).takeUnretainedValue()
            monitor.emit(.appeared(DiskArbitrationMonitor.name(of: disk)))
        }, context)

        DARegisterDiskDisappearedCallback(session, matching, { disk, context in
            guard let context else { return }
            let monitor = Unmanaged<DiskArbitrationMonitor>.fromOpaque(context).takeUnretainedValue()
            monitor.emit(.disappeared(DiskArbitrationMonitor.name(of: disk)))
        }, context)

        DARegisterDiskDescriptionChangedCallback(session, matching, nil, { disk, _, context in
            guard let context else { return }
            let monitor = Unmanaged<DiskArbitrationMonitor>.fromOpaque(context).takeUnretainedValue()
            monitor.emit(.changed(DiskArbitrationMonitor.name(of: disk)))
        }, context)

        DASessionSetDispatchQueue(session, queue)
    }

    public func stop() {
        guard let session, started else { return }
        started = false
        DASessionSetDispatchQueue(session, nil)
        continuation.finish()
    }

    private func emit(_ event: DiskEvent) {
        continuation.yield(event)
    }

    private static func name(of disk: DADisk) -> String? {
        guard let cString = DADiskGetBSDName(disk) else { return nil }
        return String(cString: cString)
    }
}
#else
public final class DiskArbitrationMonitor: @unchecked Sendable {
    public let events: AsyncStream<DiskEvent>
    private let continuation: AsyncStream<DiskEvent>.Continuation

    public init() {
        var captured: AsyncStream<DiskEvent>.Continuation!
        events = AsyncStream(bufferingPolicy: .bufferingNewest(16)) { captured = $0 }
        continuation = captured
    }

    public func start() {}
    public func stop() { continuation.finish() }
}
#endif
