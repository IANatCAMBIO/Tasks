// winlist.swift — print every on-screen window of one process:
//   <window id> | <title> | <x> <y> <w> <h>
//
// Used by tools/winshot.sh to capture the Tasks window by id, which is the
// only way to screenshot it when the front Space is not the one it is on.
// Plain CoreGraphics, no dependencies; run with `swift winlist.swift PID`.
import CoreGraphics
import Foundation

let pid = Int32(CommandLine.arguments[1])!
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
for w in list where (w["kCGWindowOwnerPID"] as? Int32) == pid {
    let b = w["kCGWindowBounds"] as? [String: Any] ?? [:]
    print(w["kCGWindowNumber"] ?? 0, "|", w["kCGWindowName"] ?? "", "|",
          b["X"] ?? 0, b["Y"] ?? 0, b["Width"] ?? 0, b["Height"] ?? 0)
}
