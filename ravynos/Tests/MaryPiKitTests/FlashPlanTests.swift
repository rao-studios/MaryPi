import Foundation
import Testing
@testable import MaryPiKit

@Suite struct FlashPlanTests {
    @Test func level0WithoutTarget() {
        let plan = FlashPlan.standard(level: .bootstrapOnly, hasTarget: false)
        #expect(plan.contains(.populateFirmware))
        #expect(!plan.contains(.populateBoot))
        #expect(!plan.contains(.populateRoot))
        #expect(!plan.contains(.write))
        #expect(plan.steps.last?.kind == .ejectImage)
    }

    @Test func level1WithTarget() {
        let plan = FlashPlan.standard(level: .kernelBringUp, hasTarget: true)
        #expect(plan.contains(.populateBoot))
        #expect(!plan.contains(.populateRoot))
        #expect(plan.steps.suffix(4).map(\.kind) == [.verifyTarget, .unmount, .write, .eject])
    }

    @Test func level2IncludesEverything() {
        let plan = FlashPlan.standard(level: .fullSystem, hasTarget: true)
        #expect(plan.steps.map(\.kind) == StepKind.allCases)
    }

    @Test func statusUpdates() {
        var plan = FlashPlan.standard(level: .bootstrapOnly, hasTarget: true)
        plan.set(.write, .running, progress: 0.5)
        #expect(plan[.write]?.status == .running)
        #expect(plan[.write]?.progress == 0.5)
        plan.set(.write, .done)
        #expect(plan[.write]?.progress == 1)
        plan.set(.eject, .failed("nope"))
        #expect(plan.failedStep?.kind == .eject)
        #expect(!plan.isFinished)
    }
}
