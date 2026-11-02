// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Test installations have their own URL scheme as well as their own container,
/// so VPN/certificate callbacks cannot open the production installation.
enum HuskIdentity {
    static let urlScheme: String = {
        let types = Bundle.main.object(forInfoDictionaryKey: "CFBundleURLTypes") as? [[String: Any]]
        return (types?.first?["CFBundleURLSchemes"] as? [String])?.first ?? "husk"
    }()
}
