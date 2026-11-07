// The app's icon, a stand-in: a planet's limb under a starfield and a
// sun's glare, drawn here so nothing of the game's own art (or any
// franchise's marks) is used.
//
//   swift app/icon.swift OUT.iconset && iconutil -c icns OUT.iconset
import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset"

func draw(_ ctx: CGContext, _ s: CGFloat) {
    // The rounded square of a macOS icon: 824 of 1024, corner 185.
    let inset = s * 100 / 1024, side = s - 2 * inset
    let tile = CGPath(roundedRect: CGRect(x: inset, y: inset, width: side, height: side),
                      cornerWidth: side * 0.225, cornerHeight: side * 0.225, transform: nil)
    ctx.saveGState()
    ctx.addPath(tile)
    ctx.clip()
    let cs = CGColorSpaceCreateDeviceRGB()
    // Deep space, a little violet towards the top.
    let space = CGGradient(colorsSpace: cs,
                           colors: [CGColor(red: 0.02, green: 0.02, blue: 0.06, alpha: 1),
                                    CGColor(red: 0.10, green: 0.06, blue: 0.22, alpha: 1)] as CFArray,
                           locations: [0, 1])!
    ctx.drawLinearGradient(space, start: CGPoint(x: 0, y: inset), end: CGPoint(x: 0, y: s - inset), options: [])
    // Stars, placed by a fixed sequence so every size draws the same sky.
    var seed: UInt32 = 12345
    func next() -> CGFloat { seed = seed &* 1103515245 &+ 12345; return CGFloat((seed >> 8) & 0xFFFF) / 65535 }
    for _ in 0..<70 {
        let x = inset + next() * side, y = inset + side * 0.35 + next() * side * 0.65, r = side * (0.003 + next() * 0.006)
        ctx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 0.5 + 0.5 * next()))
        ctx.fillEllipse(in: CGRect(x: x - r, y: y - r, width: 2 * r, height: 2 * r))
    }
    // A sun's glare, upper right.
    let glare = CGGradient(colorsSpace: cs,
                           colors: [CGColor(red: 1, green: 0.92, blue: 0.7, alpha: 1),
                                    CGColor(red: 1, green: 0.6, blue: 0.2, alpha: 0.35),
                                    CGColor(red: 1, green: 0.5, blue: 0.2, alpha: 0)] as CFArray,
                           locations: [0, 0.25, 1])!
    let sun = CGPoint(x: inset + side * 0.74, y: inset + side * 0.74)
    ctx.drawRadialGradient(glare, startCenter: sun, startRadius: 0, endCenter: sun, endRadius: side * 0.3, options: [])
    // The planet, its limb lit from the sun.
    let pc = CGPoint(x: inset + side * 0.35, y: inset - side * 0.55), pr = side * 0.95
    let planet = CGGradient(colorsSpace: cs,
                            colors: [CGColor(red: 0.35, green: 0.62, blue: 0.95, alpha: 1),
                                     CGColor(red: 0.10, green: 0.25, blue: 0.55, alpha: 1),
                                     CGColor(red: 0.03, green: 0.06, blue: 0.15, alpha: 1)] as CFArray,
                            locations: [0, 0.35, 1])!
    ctx.saveGState()
    ctx.addEllipse(in: CGRect(x: pc.x - pr, y: pc.y - pr, width: 2 * pr, height: 2 * pr))
    ctx.clip()
    ctx.drawRadialGradient(planet, startCenter: CGPoint(x: sun.x - side * 0.2, y: pc.y + pr * 0.95), startRadius: 0,
                           endCenter: pc, endRadius: pr * 1.1, options: [.drawsAfterEndLocation])
    ctx.restoreGState()
    // Its atmosphere: a thin bright rim.
    ctx.setStrokeColor(CGColor(red: 0.6, green: 0.85, blue: 1, alpha: 0.8))
    ctx.setLineWidth(side * 0.012)
    ctx.strokeEllipse(in: CGRect(x: pc.x - pr, y: pc.y - pr, width: 2 * pr, height: 2 * pr))
    ctx.restoreGState()
}

func png(_ px: Int, _ name: String) throws {
    let cs = CGColorSpace(name: CGColorSpace.sRGB)!
    let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    draw(ctx, CGFloat(px))
    let rep = NSBitmapImageRep(cgImage: ctx.makeImage()!)
    try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: "\(out)/\(name)"))
}

try FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
for pt in [16, 32, 128, 256, 512] {
    try png(pt, "icon_\(pt)x\(pt).png")
    try png(pt * 2, "icon_\(pt)x\(pt)@2x.png")
}
