// The game: libstl.dylib, the host compiled with the lift, entered at
// stl_main as the stl program is (src/host/exe.c).
//
// It runs in a child, this program again with STL_PLAY set, which loads the
// library before anything has touched AppKit, so SDL makes the game's window
// and its Dock icon. The app waits, out of the Dock, and if the game crashed
// says so and offers its log.
import Darwin
import Foundation

enum Game {
    static let crashed: Int32 = 3        // the host's exit after a fault (main.c, on_fault)
    static let unloadable: Int32 = 2

    /// In the child: load the game and run it. Never returns while it plays.
    static func play() -> Int32 {
        guard let lib = dlopen(Paths.library.path, RTLD_NOW | RTLD_LOCAL) else {
            fputs("cannot load \(Paths.library.path): \(String(cString: dlerror()))\n", stderr)
            return unloadable
        }
        guard let sym = dlsym(lib, "stl_main") else {
            fputs("\(Paths.library.path) has no stl_main\n", stderr)
            return unloadable
        }
        typealias Main = @convention(c) (Int32, UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Int32
        let main = unsafeBitCast(sym, to: Main.self)
        // The game folder and --run, then what the app was given, less Finder's
        // process serial number. The first time, the desktop's own size: after
        // that the host remembers whatever was chosen (User/resolution.txt).
        var args = [CommandLine.arguments[0], "--game", Paths.game.path, "--run"]
        if !Paths.exists(Paths.firstLaunch) {
            args += ["--resolution", "native"]
            FileManager.default.createFile(atPath: Paths.firstLaunch.path, contents: nil)
        }
        args += CommandLine.arguments.dropFirst().filter { !$0.hasPrefix("-psn_") && $0 != "--setup" }
        var argv = args.map { strdup($0) } + [nil]
        return main(Int32(args.count), &argv)
    }

    /// In the app: start the child; `done` gets its exit status.
    static func start(done: @escaping (Int32) -> Void) throws {
        var fa: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&fa)
        defer { posix_spawn_file_actions_destroy(&fa) }
        // The last run's log kept as stl.1.log; this run's output to stl.log.
        // Not when started from a terminal, where the output belongs.
        if isatty(STDERR_FILENO) == 0 {
            try? FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
            let old = Paths.logs.appendingPathComponent("stl.1.log")
            try? FileManager.default.removeItem(at: old)
            try? FileManager.default.moveItem(at: Paths.gameLog, to: old)
            posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, Paths.gameLog.path, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
            posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO)
        }
        var env = ProcessInfo.processInfo.environment
        env["STL_PLAY"] = "1"
        let envp = env.map { strdup("\($0)=\($1)") } + [nil]
        let argv = CommandLine.arguments.map { strdup($0) } + [nil]
        defer { (envp + argv).forEach { free($0) } }
        var pid: pid_t = 0
        let err = posix_spawn(&pid, Bundle.main.executablePath!, &fa, nil, argv, envp)
        if err != 0 { throw POSIXError(POSIXErrorCode(rawValue: err) ?? .EIO) }
        let source = DispatchSource.makeProcessSource(identifier: pid, eventMask: .exit, queue: .main)
        source.setEventHandler {
            var status: Int32 = 0
            waitpid(pid, &status, 0)
            source.cancel()
            done(status)
        }
        source.resume()
        watching = source
    }
    private static var watching: DispatchSourceProcess?

    static func crashed(_ status: Int32) -> Bool {
        let signalled = (status & 0x7f) != 0 && (status & 0x7f) != 0x7f
        return signalled || (status >> 8) & 0xff == crashed
    }
    static func unloadable(_ status: Int32) -> Bool { (status & 0x7f) == 0 && (status >> 8) & 0xff == unloadable }
}
