// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

@main struct GuestRAMPlanTests {
    static func main() throws {
        let reported = GuestRAMPlan.adaptive(anonymousMiB: 3324, probedFileMiB: 2048)
        precondition(reported == GuestRAMPlan(mib: 2048, fileBacked: true))
        precondition(UInt64(reported.mib) * 1024 * 1024 == 0x80000000)
        precondition(UInt64(3324) * 1024 * 1024 == 0xcfc00000)
        precondition(GuestRAMPlan.adaptive(anonymousMiB: 1536, probedFileMiB: 3584)
                     == GuestRAMPlan(mib: 3584, fileBacked: true))
        precondition(GuestRAMPlan.adaptive(anonymousMiB: 3324, probedFileMiB: nil)
                     == GuestRAMPlan(mib: 3324, fileBacked: false))
        var requested: [Int] = []
        let pinned = try GuestRAMPlan.pinned(mib: 3324) { requested.append($0); return true }
        precondition(requested == [3324])
        precondition(pinned == GuestRAMPlan(mib: 3324, fileBacked: true))
        do {
            _ = try GuestRAMPlan.pinned(mib: 3324) { _ in false }
            preconditionFailure("a failed snapshot file must stop before qemu_init")
        } catch GuestRAMPlan.PreparationError.snapshotRAMUnavailable(let size) {
            precondition(size == 3324)
        }
        print("PASS: QEMU size/backend agreement, anonymous fallback, exact snapshot preparation and failure")
    }
}
