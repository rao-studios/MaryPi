import Foundation

/// The ordered steps of one "build an image and/or write a card" run.
public enum StepKind: String, CaseIterable, Sendable, Codable, Hashable {
    case doctor
    case buildRootfs
    case buildUI
    case buildTarget
    case buildImage
    case verifyTarget
    case unmount
    case write
    case eject

    public var title: String {
        switch self {
        case .doctor: return "Check this Mac"
        case .buildRootfs: return "Build the base rootfs (Ubuntu Noble + MaryOS)"
        case .buildUI: return "Compile the desktop (MaryUI, Liquid Platinum)"
        case .buildTarget: return "Add the target's packages and boot files"
        case .buildImage: return "Assemble the disk image"
        case .verifyTarget: return "Verify target disk"
        case .unmount: return "Unmount target disk"
        case .write: return "Write image to card"
        case .eject: return "Eject card"
        }
    }

    /// Steps that touch the target disk and therefore need admin rights.
    public var isPrivileged: Bool {
        switch self {
        case .unmount, .write, .eject: return true
        default: return false
        }
    }
}

public enum StepStatus: Sendable, Equatable, Hashable {
    case pending
    case running
    case done
    case skipped(String)
    case failed(String)

    public var isTerminal: Bool {
        switch self {
        case .pending, .running: return false
        case .done, .skipped, .failed: return true
        }
    }
}

public struct Step: Identifiable, Sendable, Equatable, Hashable {
    public var id: StepKind { kind }
    public let kind: StepKind
    public var status: StepStatus
    public var progress: Double?
    public var detail: String

    public init(_ kind: StepKind, status: StepStatus = .pending, progress: Double? = nil, detail: String = "") {
        self.kind = kind
        self.status = status
        self.progress = progress
        self.detail = detail
    }

    public var title: String { kind.title }
}

public struct StepPlan: Sendable, Equatable {
    public var steps: [Step]

    public init(steps: [Step]) {
        self.steps = steps
    }

    /// The canonical step list: a build (four builder stages) and/or a
    /// write to a target disk.
    public static func standard(build: Bool, hasTarget: Bool) -> StepPlan {
        var kinds: [StepKind] = [.doctor]
        if build { kinds += [.buildRootfs, .buildUI, .buildTarget, .buildImage] }
        if hasTarget { kinds += [.verifyTarget, .unmount, .write, .eject] }
        return StepPlan(steps: kinds.map { Step($0) })
    }

    public func contains(_ kind: StepKind) -> Bool {
        steps.contains { $0.kind == kind }
    }

    public subscript(kind: StepKind) -> Step? {
        steps.first { $0.kind == kind }
    }

    public mutating func set(_ kind: StepKind, _ status: StepStatus, detail: String? = nil, progress: Double? = nil) {
        guard let index = steps.firstIndex(where: { $0.kind == kind }) else { return }
        steps[index].status = status
        if let detail { steps[index].detail = detail }
        if let progress { steps[index].progress = progress }
        if case .done = status { steps[index].progress = 1 }
    }

    public var isFinished: Bool {
        steps.allSatisfy { $0.status.isTerminal }
    }

    public var failedStep: Step? {
        steps.first { if case .failed = $0.status { return true } else { return false } }
    }
}
