// Rasterize the independently authored controller SVGs using macOS AppKit.
// Usage: swift tools/rasterize_controller_svg.swift controllers/sw
import AppKit

let directory = URL(fileURLWithPath: CommandLine.arguments[1])
for source in try FileManager.default.contentsOfDirectory(at: directory,
        includingPropertiesForKeys: nil).filter({ $0.pathExtension == "svg" }).sorted(by: { $0.path < $1.path }) {
    let name = source.deletingPathExtension().lastPathComponent
    guard let image = NSImage(contentsOf: source),
          let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(image.size.width), pixelsHigh: Int(image.size.height),
              bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
              colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0),
          let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
        fatalError("Cannot rasterize \(source.path)")
    }
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = context
    image.draw(in: NSRect(origin: .zero, size: image.size))
    context.flushGraphics()
    NSGraphicsContext.restoreGraphicsState()
    guard let encoded = bitmap.representation(using: .png, properties: [:]) else {
        fatalError("Cannot encode \(name).png")
    }
    try encoded.write(to: directory.appendingPathComponent(name + ".png"))
}
