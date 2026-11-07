// The app's window: Play when the game is built; otherwise building it, the
// first time (downloading the disc image and the 2.0 patch, or the player's
// own copies) and after an update that needs it.
import SwiftUI
import UniformTypeIdentifiers

struct SetupView: View {
    @ObservedObject var installer: Installer
    var play: () -> Void = {}
    var checkForUpdates: () -> Void = {}

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            header
            switch installer.phase {
            case .needTools: NeedTools(installer: installer)
            case .choose:
                if installer.ownFiles { Choose(installer: installer) } else { Download(installer: installer) }
            case .working: Working(installer: installer)
            case .failed(let why): Failed(installer: installer, why: why)
            case .ready: Ready(play: play, checkForUpdates: checkForUpdates)
            }
        }
        .padding(28)
        .frame(width: 560)
    }

    private var header: some View {
        HStack(spacing: 14) {
            Image(nsImage: NSApp.applicationIconImage ?? NSImage())
                .resizable().frame(width: 64, height: 64)
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.title2.weight(.semibold))
                Text(subtitle).foregroundStyle(.secondary)
            }
        }
    }
}

extension SetupView {
    private var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
    }
    private var title: String {
        if installer.phase == .ready { return "Star Trek Legacy" }
        return installer.needsDisc ? "Set up Star Trek Legacy" : "Updating Star Trek Legacy"
    }
    private var subtitle: String {
        if installer.phase == .ready { return version.isEmpty ? "Ready to play." : "Ready to play. Version \(version)." }
        return installer.needsDisc ? "The game is built on this Mac, once, before its first run."
                                   : "This version needs the game rebuilt. Your parks and saves are kept."
    }
}

// ---- ready ----------------------------------------------------------------

private struct Ready: View {
    let play: () -> Void
    let checkForUpdates: () -> Void

    var body: some View {
        HStack {
            Button("Check for Updates…", action: checkForUpdates)
            Spacer()
            Button(action: play) {
                Label("Play", systemImage: "play.fill").frame(minWidth: 120)
            }
            .controlSize(.large)
            .keyboardShortcut(.defaultAction)
        }
        .padding(.top, 4)
    }
}

// ---- downloading the two files ------------------------------------------------

private struct Download: View {
    @ObservedObject var installer: Installer

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("It downloads the game's disc image and its 1.2 patch (2.2 GB), then builds the game from them. "
                 + "That takes about an hour and 12 GB of space; your Mac stays free to use meanwhile.")
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Use Files I Already Have…") { installer.ownFiles = true }
                Spacer()
                Button("Download and Build") { installer.start() }
                    .keyboardShortcut(.defaultAction)
            }
            .padding(.top, 6)
        }
    }
}

// ---- choosing the two files ---------------------------------------------------

private struct Choose: View {
    @ObservedObject var installer: Installer

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Choose the two files it is built from. Both are free downloads.")
            FileRow(installer: installer, id: "iso", title: "Disc image", expected: "star_trek_legacy.iso",
                    types: [.diskImage, .data], source: "Internet Archive",
                    link: URL(string: "https://archive.org/search?query=%22star+trek+legacy%22")!, file: $installer.iso)
            FileRow(installer: installer, id: "patch", title: "1.2 patch", expected: "star_trek_legacy_1_2_patch.exe",
                    types: [.exe, .data], source: "Internet Archive",
                    link: URL(string: "https://archive.org/search?query=%22star+trek+legacy%22+patch")!, file: $installer.patch)
            HStack {
                Button("Download Them Instead") { installer.ownFiles = false }
                Spacer()
                Button("Build Star Trek Legacy") { installer.start() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(installer.iso == nil || installer.patch == nil)
            }
            .padding(.top, 6)
        }
    }
}

private struct FileRow: View {
    @ObservedObject var installer: Installer
    let id: String
    let title: String
    let expected: String
    let types: [UTType]
    let source: String
    let link: URL
    @Binding var file: URL?

    private var targeted: Bool { installer.dropTarget == id }
    private var picking: Binding<Bool> {
        Binding(get: { installer.picking == id }, set: { installer.picking = $0 ? id : nil })
    }
    private var dropping: Binding<Bool> {
        Binding(get: { installer.dropTarget == id },
                set: { on in if on { installer.dropTarget = id } else if installer.dropTarget == id { installer.dropTarget = nil } })
    }

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: file == nil ? "doc.badge.plus" : "checkmark.circle.fill")
                .font(.title2)
                .foregroundStyle(file == nil ? Color.secondary : Color.green)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                if let file {
                    Text(file.lastPathComponent).font(.callout).lineLimit(1).truncationMode(.middle)
                } else {
                    HStack(spacing: 4) {
                        Text("\(expected), from").foregroundStyle(.secondary)
                        Link(source, destination: link)
                    }
                    .font(.callout)
                }
            }
            Spacer()
            Button(file == nil ? "Choose…" : "Change…") { installer.picking = id }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 10)
            .fill(targeted ? Color.accentColor.opacity(0.15) : Color.secondary.opacity(0.08)))
        .overlay(RoundedRectangle(cornerRadius: 10)
            .strokeBorder(targeted ? Color.accentColor : Color.clear, lineWidth: 2))
        .fileImporter(isPresented: picking, allowedContentTypes: types) { result in
            if case .success(let url) = result { file = url }
        }
        .onDrop(of: [.fileURL], isTargeted: dropping) { providers in
            _ = providers.first?.loadObject(ofClass: URL.self) { url, _ in
                if let url { DispatchQueue.main.async { file = url } }
            }
            return true
        }
    }
}

// ---- building ---------------------------------------------------------------

private struct Working: View {
    @ObservedObject var installer: Installer

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(installer.stepCount > 0
                     ? "Step \(installer.stepIndex) of \(installer.stepCount): \(installer.step)"
                     : installer.step)
                    .font(.headline)
                Spacer()
                TimelineView(.periodic(from: installer.started, by: 1)) { ctx in
                    Text(elapsed(ctx.date)).monospacedDigit().foregroundStyle(.secondary)
                }
            }
            if let f = installer.fraction {
                ProgressView(value: f)
            } else {
                ProgressView().progressViewStyle(.linear)
            }
            Text(installer.log.last ?? " ")
                .font(.caption.monospaced()).foregroundStyle(.secondary)
                .lineLimit(1).truncationMode(.tail)
            DisclosureGroup("Details", isExpanded: $installer.showDetails) {
                LogView(lines: installer.log).frame(height: 180)
            }
            HStack {
                Text("You can use your Mac as normal meanwhile.")
                    .font(.callout).foregroundStyle(.secondary)
                Spacer()
                Button("Cancel") { installer.cancel() }
            }
        }
    }

    private func elapsed(_ now: Date) -> String {
        let s = max(0, Int(now.timeIntervalSince(installer.started)))
        return String(format: "%d:%02d", s / 60, s % 60)
    }
}

private struct LogView: View {
    let lines: [String]

    var body: some View {
        ScrollViewReader { scroll in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(lines.suffix(800).enumerated()), id: \.offset) { i, line in
                        Text(line).font(.caption.monospaced()).textSelection(.enabled).id(i)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(6)
            }
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .onChange(of: lines.count) { scroll.scrollTo(min(lines.count, 800) - 1, anchor: .bottom) }
        }
    }
}

private struct Failed: View {
    @ObservedObject var installer: Installer
    let why: String

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label { Text(why) } icon: {
                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            }
            .fixedSize(horizontal: false, vertical: true)
            if !installer.log.isEmpty {
                LogView(lines: installer.log).frame(height: 140)
            }
            HStack {
                Button("Show Log") { NSWorkspace.shared.open(Paths.installLog) }
                Spacer()
                if installer.needsDisc {
                    Button("Back") { installer.phase = .choose }
                }
                Button("Try Again") { installer.start() }.keyboardShortcut(.defaultAction)
            }
        }
    }
}

private struct NeedTools: View {
    @ObservedObject var installer: Installer

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Building the game needs Apple's Command Line Tools for Xcode, a free download of about 1 GB. Xcode itself isn't needed.")
                .fixedSize(horizontal: false, vertical: true)
            if installer.askedForTools {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("Waiting for the Command Line Tools to finish installing…").foregroundStyle(.secondary)
                }
            }
            HStack {
                Spacer()
                Button("Install Command Line Tools") { installer.askedForTools = true; installer.installTools() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(installer.askedForTools)
            }
        }
    }
}
