// Star Trek Legacy.app's own program. Nothing of the game is in the app: it
// carries the sources and the tools to build it (app/build-app.sh), builds it
// on this Mac into Application Support from the player's own disc image and
// the 2.0 patch, rebuilds it when an update needs that, and runs it.
//
//   ~/Library/Application Support/Star Trek Legacy/   the game (STL_HOME: elsewhere)
//   ~/Library/Logs/Star Trek Legacy/                   stl.log (the game), install.log (the build)
//
// The window shows Play when the game is built, and otherwise builds it,
// downloading the disc image and the patch (or using the player's own). --setup
// builds it again even when it is ready; STL_ISO and STL_PATCH give the
// player's own two files and start the build; STL_BUILD_NOW presses Download
// and Build, STL_PLAY_NOW presses Play (for tests).
// Started from a terminal, the game's output stays there and other arguments
// pass to it.
import AppKit
import Sparkle
import SwiftUI

if ProcessInfo.processInfo.environment["STL_PLAY"] != nil {
    exit(Game.play())
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let installer = Installer()
    var window: NSWindow?
    var updater: SPUStandardUpdaterController?
    var playing = false

    func applicationWillFinishLaunching(_ note: Notification) {
        // Out of the Dock until there is a window to show: while the game
        // plays, its own process is the app the Dock shows.
        NSApp.setActivationPolicy(.accessory)
        NSApp.mainMenu = menu()
    }

    func applicationDidFinishLaunching(_ note: Notification) {
        // Releases name their feed and key (app/release.sh); a local build has none.
        if Bundle.main.object(forInfoDictionaryKey: "SUPublicEDKey") != nil {
            updater = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: nil, userDriverDelegate: nil)
        }
        clearQuarantine()
        let status = Installer.status()
        let ready = status?.ready ?? Paths.exists(Paths.library)
        // STL_ISO and STL_PATCH: the player's own two files, chosen already.
        let env = ProcessInfo.processInfo.environment
        if let iso = env["STL_ISO"], let patch = env["STL_PATCH"] {
            installer.iso = URL(fileURLWithPath: iso)
            installer.patch = URL(fileURLWithPath: patch)
            installer.ownFiles = true
        }
        if ready && !CommandLine.arguments.contains("--setup") {
            installer.needsDisc = false
            installer.phase = .ready
        } else {
            installer.needsDisc = status?.needs_disc ?? !Paths.exists(Paths.game)
        }
        installer.onDone = { [weak self] in
            if env["STL_PLAY_NOW"] != nil { self?.play() } else { self?.window?.makeKeyAndOrderFront(nil) }
        }
        showWindow()
        if installer.phase == .ready {
            if env["STL_PLAY_NOW"] != nil { play() }        // for tests: as if Play were pressed
        } else if !Installer.hasCompiler() {
            installer.phase = .needTools
        } else if !installer.needsDisc || (installer.ownFiles && installer.iso != nil && installer.patch != nil)
                    || env["STL_BUILD_NOW"] != nil {
            installer.start()
        }
        // Sparkle's own check, now, while the window is up: it asks before it installs.
        updater?.updater.checkForUpdatesInBackground()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { !playing }

    func applicationWillTerminate(_ note: Notification) { installer.cancel() }

    // ---- the window ------------------------------------------------------

    func showWindow() {
        NSApp.setActivationPolicy(.regular)
        let w = NSWindow(contentRect: .zero, styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        w.title = Paths.title
        w.contentView = NSHostingView(rootView: SetupView(
            installer: installer,
            play: { [weak self] in self?.play() },
            checkForUpdates: { [weak self] in self?.updater?.checkForUpdates(nil) }))
        w.isReleasedWhenClosed = false
        w.center()
        w.makeKeyAndOrderFront(nil)
        NSApp.activate()
        window = w
    }

    // ---- the game ----------------------------------------------------------

    func play() {
        window?.close()
        window = nil
        NSApp.setActivationPolicy(.accessory)
        playing = true
        do {
            try Game.start { [weak self] status in self?.ended(status) }
        } catch {
            alert("Star Trek Legacy couldn't start", error.localizedDescription, log: false)
            NSApp.terminate(nil)
        }
    }

    func ended(_ status: Int32) {
        let logged = isatty(STDERR_FILENO) == 0
        if Game.crashed(status) {
            alert("Star Trek Legacy quit unexpectedly",
                  "The game's log says where it stopped: it helps to attach it to a bug report.", log: logged)
        } else if Game.unloadable(status) {
            alert("Star Trek Legacy couldn't start",
                  "The game built on this Mac could not be loaded. Opening the app with --setup builds it again.",
                  log: logged)
        }
        NSApp.terminate(nil)
    }

    func alert(_ title: String, _ text: String, log: Bool) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate()
        let a = NSAlert()
        a.messageText = title
        a.informativeText = text
        a.addButton(withTitle: "OK")
        if log { a.addButton(withTitle: "Show Log") }
        if a.runModal() == .alertSecondButtonReturn { NSWorkspace.shared.open(Paths.gameLog) }
    }

    // ---- odds and ends -------------------------------------------------------

    /// A download's quarantine reaches every file in the app; the tools it
    /// runs to build the game are cleared of it once the app itself was let run.
    func clearQuarantine() {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/xattr")
        p.arguments = ["-dr", "com.apple.quarantine", Paths.resources.appendingPathComponent("toolchain").path]
        p.standardError = FileHandle.nullDevice
        try? p.run()
        p.waitUntilExit()
    }

    func menu() -> NSMenu {
        let main = NSMenu()
        let app = NSMenu()
        app.addItem(withTitle: "About \(Paths.title)", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        let check = app.addItem(withTitle: "Check for Updates…", action: #selector(checkForUpdates), keyEquivalent: "")
        check.target = self
        app.addItem(.separator())
        app.addItem(withTitle: "Hide \(Paths.title)", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        app.addItem(.separator())
        app.addItem(withTitle: "Quit \(Paths.title)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        main.addItem(withTitle: "", action: nil, keyEquivalent: "").submenu = app
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        main.addItem(withTitle: "Edit", action: nil, keyEquivalent: "").submenu = edit
        return main
    }

    @objc func checkForUpdates() { updater?.checkForUpdates(nil) }
}

/// STL_SNAPSHOT=DIR: the setup window in each of its states, as PNGs, for a
/// look without a screen (README: the app).
@MainActor
func snapshots(to dir: URL) {
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    func shot(_ name: String, _ set: (Installer) -> Void) {
        let i = Installer()
        set(i)
        let r = ImageRenderer(content: SetupView(installer: i).background(Color(nsColor: .windowBackgroundColor)))
        r.scale = 2
        guard let image = r.cgImage else { return }
        try? NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])?
            .write(to: dir.appendingPathComponent("\(name).png"))
    }
    shot("0-ready") { $0.needsDisc = false; $0.phase = .ready }
    shot("1-download") { $0.phase = .choose }
    shot("1-choose") { $0.phase = .choose; $0.ownFiles = true }
    shot("2-chosen") {
        $0.phase = .choose
        $0.ownFiles = true
        $0.iso = URL(fileURLWithPath: "/Users/me/Downloads/star_trek_legacy.iso")
        $0.patch = URL(fileURLWithPath: "/Users/me/Downloads/star_trek_legacy_1_2_patch.exe")
    }
    shot("3-working") {
        $0.phase = .working
        $0.stepIndex = 4; $0.stepCount = 6; $0.step = "Analysing the game"
        $0.started = Date().addingTimeInterval(-143)
        $0.log = ["$ disasm32.py TP.EXE -o functions.json", "cataloguing the functions (a few minutes)..."]
    }
    shot("4-update") {
        $0.needsDisc = false
        $0.phase = .working
        $0.stepIndex = 3; $0.stepCount = 3; $0.step = "Compiling"; $0.fraction = 0.4
        $0.log = ["[61/127] Building C object CMakeFiles/stl_gen.dir/build/gen/recomp_legacy_0042_0.c.o"]
    }
    shot("5-failed") {
        $0.phase = .failed("star_trek_legacy.iso is not the disc image this needs (MD5 1b2c…, wanted 30c7…); see the README")
    }
    shot("6-tools") { $0.phase = .needTools }
}

let app = NSApplication.shared
if let dir = ProcessInfo.processInfo.environment["STL_SNAPSHOT"] {
    MainActor.assumeIsolated { snapshots(to: URL(fileURLWithPath: dir)) }
    exit(0)
}
let delegate = MainActor.assumeIsolated { AppDelegate() }
app.delegate = delegate
app.run()
