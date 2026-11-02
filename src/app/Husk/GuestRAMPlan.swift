// SPDX-License-Identifier: GPL-2.0-or-later

/// Size and backend are one decision. A successfully probed file must never
/// be used with the (possibly larger) anonymous-memory budget.
struct GuestRAMPlan: Equatable {
    let mib: Int
    let fileBacked: Bool

    static func adaptive(anonymousMiB: Int, probedFileMiB: Int?) -> GuestRAMPlan {
        if let mib = probedFileMiB {
            return GuestRAMPlan(mib: mib, fileBacked: true)
        }
        return GuestRAMPlan(mib: anonymousMiB, fileBacked: false)
    }

    enum PreparationError: Error {
        case snapshotRAMUnavailable(Int)
    }

    /// A snapshot cannot be restored with a different RAM size or backend.
    /// Probe this exact size, including extending a stale nonempty RAM file.
    static func pinned(mib: Int, prepareFile: (Int) -> Bool) throws -> GuestRAMPlan {
        guard prepareFile(mib) else {
            throw PreparationError.snapshotRAMUnavailable(mib)
        }
        return GuestRAMPlan(mib: mib, fileBacked: true)
    }
}
