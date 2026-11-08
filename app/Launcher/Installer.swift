// Building the game: tools/install/install.py, run with the Python the app
// carries, its progress lines (@step, @progress, @error, @done) shown.
import Foundation
import SwiftUI

struct Status: Decodable {
    let ready: Bool
    let stale: [String]
    let needs_disc: Bool
}

/// The output not yet ended by a newline; touched only by the pipe's handler.
private final class Pending: @unchecked Sendable {
    private var data = Data()
    func lines(adding chunk: Data) -> [String] {
        data.append(chunk)
        var out: [String] = []
        while let nl = data.firstIndex(of: 0x0A) {
            out.append(String(decoding: data[data.startIndex..<nl], as: UTF8.self))
            data.removeSubrange(data.startIndex...nl)
        }
        return out
    }
}

@MainActor
final class Installer: ObservableObject {
    enum Phase: Equatable { case needTools, choose, working, failed(String), ready }

    @Published var phase: Phase = .choose
    @Published var iso: URL?
    @Published var patch: URL?
    @Published var step = ""
    @Published var stepIndex = 0
    @Published var stepCount = 0
    @Published var fraction: Double?            // nil: no measure of this step
    @Published var log: [String] = []
    @Published var started = Date()
    /// First setup asks for the disc; an update rebuilds from what is there.
    @Published var needsDisc = true
    // The window's own state (SetupView), here rather than in @State, whose
    // macro needs Xcode: the app builds with the command line tools alone.
    @Published var picking: String?              // which file a panel is open for
    @Published var dropTarget: String?
    @Published var showDetails = false
    /// The two files picked by hand, not downloaded.
    @Published var ownFiles = false
    @Published var askedForTools = false

    var onDone: (() -> Void)?
    private var process: Process?
    private var cancelled = false

    static func arguments(_ extra: [String]) -> [String] {
        ["-I", Paths.installer.path, "--dest", Paths.home.path, "--pcrecomp", Paths.pcrecomp.path,
         "--sdl-frameworks", Paths.frameworks.path, "--ffmpeg", Paths.ffmpeg.path] + extra
    }

    /// What a build would redo; nil if the app cannot tell (no toolchain).
    static func status() -> Status? {
        guard Paths.exists(Paths.python) else { return nil }
        let p = Process()
        p.executableURL = Paths.python
        p.arguments = arguments(["--status"])
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return nil }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        let last = String(decoding: data, as: UTF8.self).split(separator: "\n").last.map(String.init) ?? ""
        return try? JSONDecoder().decode(Status.self, from: Data(last.utf8))
    }

    static func hasCompiler() -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        p.arguments = ["--find", "clang"]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    /// Xcode's command line tools: Apple's own installer, then wait for clang.
    func installTools() {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
        p.arguments = ["--install"]
        try? p.run()
        Task {
            while !Installer.hasCompiler() { try? await Task.sleep(for: .seconds(5)) }
            phase = needsDisc ? .choose : .working
            if !needsDisc { start() }
        }
    }

    func start() {
        guard Installer.hasCompiler() else { phase = .needTools; return }
        phase = .working
        log = []
        step = "Starting"
        stepIndex = 0
        stepCount = 0
        fraction = nil
        started = Date()
        cancelled = false
        var extra: [String] = []
        if ownFiles, let iso, let patch {
            extra += ["--iso", iso.path, "--patch", patch.path]
        } else {
            extra += ["--download"]
        }
        let p = Process()
        p.executableURL = Paths.python
        p.arguments = Installer.arguments(extra)
        var env = ProcessInfo.processInfo.environment
        env["STL_PGROUP"] = "1"                      // its own process group: Cancel stops the compilers too
        env["PYTHONUNBUFFERED"] = "1"
        p.environment = env
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        try? FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: Paths.installLog.path, contents: nil)
        let file = try? FileHandle(forWritingTo: Paths.installLog)
        let pending = Pending()
        pipe.fileHandleForReading.readabilityHandler = { h in
            let chunk = h.availableData
            file?.write(chunk)
            let lines = pending.lines(adding: chunk)
            if !lines.isEmpty { Task { @MainActor in lines.forEach(self.take) } }
        }
        p.terminationHandler = { p in
            pipe.fileHandleForReading.readabilityHandler = nil
            try? file?.close()
            let code = p.terminationStatus
            Task { @MainActor in self.finished(code) }
        }
        do {
            try p.run()
            process = p
        } catch {
            phase = .failed("The build could not start: \(error.localizedDescription)")
        }
    }

    func cancel() {
        guard let p = process, p.isRunning else { return }
        cancelled = true
        kill(-p.processIdentifier, SIGTERM)
    }

    private func take(_ line: String) {
        if line.hasPrefix("@step ") {
            let parts = line.split(separator: " ", maxSplits: 3).map(String.init)
            if parts.count == 4 {
                stepIndex = Int(parts[1]) ?? 0
                stepCount = Int(parts[2]) ?? 0
                step = parts[3]
                fraction = nil
            }
        } else if line.hasPrefix("@progress ") {
            let f = Double(line.dropFirst(10)) ?? -1
            fraction = f < 0 ? nil : min(max(f, 0), 1)
        } else if line.hasPrefix("@error ") {
            phase = .failed(String(line.dropFirst(7)))
        } else if line == "@done" {
            phase = .ready
        } else {
            // Ninja's "[61/127] Building ..." measures the compile.
            if line.hasPrefix("["), let slash = line.firstIndex(of: "/"), let close = line.firstIndex(of: "]"),
               let done = Double(line[line.index(after: line.startIndex)..<slash]),
               let all = Double(line[line.index(after: slash)..<close]), all > 0 {
                fraction = done / all
            }
            log.append(line)
            if log.count > 4000 { log.removeFirst(log.count - 4000) }
        }
    }

    private func finished(_ code: Int32) {
        process = nil
        if cancelled {
            phase = needsDisc ? .choose : .failed("The build was cancelled.")
        } else if code == 0 {
            phase = .ready
            needsDisc = false
            onDone?()
        } else if case .failed = phase {
        } else {
            phase = .failed("The build stopped (exit \(code)). Its log has the details.")
        }
    }
}
