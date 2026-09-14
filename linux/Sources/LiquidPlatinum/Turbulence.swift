import Foundation

/// feTurbulence as the reference code in the SVG 1.1 specification (§15.17) writes it, stitched the way
/// browsers do: a line-for-line port of maryui's src/draw/lp_noise.c, so the Mac's brushed grain is the C
/// desktop's and the web's. Names follow the specification.
struct Turbulence: Sendable {
    static let bSize = 0x100
    static let bm = 0xff
    static let perlinN = 0x1000
    static let randM = 2_147_483_647   // 2**31 - 1
    static let randA = 16807           // 7**5; primitive root of m
    static let randQ = 127_773         // m / a
    static let randR = 2836            // m % a

    struct Tile: Sendable {
        var x: Double
        var y: Double
        var width: Double
        var height: Double
    }

    private struct StitchInfo {
        var width: Int
        var height: Int
        var wrapX: Int
        var wrapY: Int
    }

    private static let count = bSize + bSize + 2
    private var lattice: [Int]
    /// gradient[channel][i][j], flattened.
    private var gradient: [Double]

    private static func g(_ channel: Int, _ i: Int, _ j: Int) -> Int { (channel * count + i) * 2 + j }

    private static func setupSeed(_ seed: Int) -> Int {
        var s = seed
        if s <= 0 { s = -(s % (randM - 1)) + 1 }
        if s > randM - 1 { s = randM - 1 }
        return s
    }

    private static func randomNext(_ seed: Int) -> Int {
        var result = randA * (seed % randQ) - randR * (seed / randQ)
        if result <= 0 { result += randM }
        return result
    }

    init(seed: Int) {
        var lattice = [Int](repeating: 0, count: Self.count)
        var gradient = [Double](repeating: 0, count: 4 * Self.count * 2)
        var lSeed = Self.setupSeed(seed)
        let bSize = Self.bSize
        for k in 0..<4 {
            for i in 0..<bSize {
                lattice[i] = i
                for j in 0..<2 {
                    lSeed = Self.randomNext(lSeed)
                    gradient[Self.g(k, i, j)] = Double((lSeed % (bSize + bSize)) - bSize) / Double(bSize)
                }
                let a = gradient[Self.g(k, i, 0)], b = gradient[Self.g(k, i, 1)]
                let s = (a * a + b * b).squareRoot()
                gradient[Self.g(k, i, 0)] = a / s
                gradient[Self.g(k, i, 1)] = b / s
            }
        }
        var i = bSize
        while true {
            i -= 1
            if i == 0 { break }
            let k = lattice[i]
            lSeed = Self.randomNext(lSeed)
            let j = lSeed % bSize
            lattice[i] = lattice[j]
            lattice[j] = k
        }
        for i in 0..<(bSize + 2) {
            lattice[bSize + i] = lattice[i]
            for k in 0..<4 {
                for j in 0..<2 { gradient[Self.g(k, bSize + i, j)] = gradient[Self.g(k, i, j)] }
            }
        }
        self.lattice = lattice
        self.gradient = gradient
    }

    private func noise2(_ channel: Int, _ vx: Double, _ vy: Double, _ stitch: StitchInfo?,
                        _ lattice: UnsafeBufferPointer<Int>, _ gradient: UnsafeBufferPointer<Double>) -> Double {
        func sCurve(_ t: Double) -> Double { t * t * (3.0 - 2.0 * t) }
        func lerp(_ t: Double, _ a: Double, _ b: Double) -> Double { a + t * (b - a) }
        var tt = vx + Double(Self.perlinN)
        var bx0 = Int(tt)
        var bx1 = bx0 + 1
        let rx0 = tt - Double(Int(tt))
        let rx1 = rx0 - 1.0
        tt = vy + Double(Self.perlinN)
        var by0 = Int(tt)
        var by1 = by0 + 1
        let ry0 = tt - Double(Int(tt))
        let ry1 = ry0 - 1.0
        // The specification masks before this comparison, which makes stitching a no-op; browsers compare
        // the unmasked lattice index and mask afterwards, and so does lp_noise.c.
        if let stitch {
            if bx0 >= stitch.wrapX { bx0 -= stitch.width }
            if bx1 >= stitch.wrapX { bx1 -= stitch.width }
            if by0 >= stitch.wrapY { by0 -= stitch.height }
            if by1 >= stitch.wrapY { by1 -= stitch.height }
        }
        bx0 &= Self.bm
        bx1 &= Self.bm
        by0 &= Self.bm
        by1 &= Self.bm
        let i = lattice[bx0], j = lattice[bx1]
        let b00 = lattice[i + by0], b10 = lattice[j + by0], b01 = lattice[i + by1], b11 = lattice[j + by1]
        let sx = sCurve(rx0), sy = sCurve(ry0)
        var u = rx0 * gradient[Self.g(channel, b00, 0)] + ry0 * gradient[Self.g(channel, b00, 1)]
        var v = rx1 * gradient[Self.g(channel, b10, 0)] + ry0 * gradient[Self.g(channel, b10, 1)]
        let a = lerp(sx, u, v)
        u = rx0 * gradient[Self.g(channel, b01, 0)] + ry1 * gradient[Self.g(channel, b01, 1)]
        v = rx1 * gradient[Self.g(channel, b11, 0)] + ry1 * gradient[Self.g(channel, b11, 1)]
        let b = lerp(sx, u, v)
        return lerp(sy, a, b)
    }

    /// `lp_turbulence_sum`: octaves of noise at (x, y), stitched across `tile` when given.
    func sum(channel: Int, x: Double, y: Double, baseFrequencyX: Double, baseFrequencyY: Double, octaves: Int,
             fractalSum: Bool, tile: Tile?) -> Double {
        var fx = baseFrequencyX, fy = baseFrequencyY
        var stitch: StitchInfo?
        if let tile {
            // Adjust the base frequencies so the noise repeats across the tile.
            if fx != 0.0 {
                let lo = (tile.width * fx).rounded(.down) / tile.width
                let hi = (tile.width * fx).rounded(.up) / tile.width
                fx = fx / lo < hi / fx ? lo : hi
            }
            if fy != 0.0 {
                let lo = (tile.height * fy).rounded(.down) / tile.height
                let hi = (tile.height * fy).rounded(.up) / tile.height
                fy = fy / lo < hi / fy ? lo : hi
            }
            let width = Int(tile.width * fx + 0.5), height = Int(tile.height * fy + 0.5)
            stitch = StitchInfo(width: width, height: height,
                                wrapX: Int(tile.x * fx + Double(Self.perlinN) + Double(width)),
                                wrapY: Int(tile.y * fy + Double(Self.perlinN) + Double(height)))
        }
        var total = 0.0
        var vx = x * fx, vy = y * fy
        var ratio = 1.0
        lattice.withUnsafeBufferPointer { lattice in
            gradient.withUnsafeBufferPointer { gradient in
                for _ in 0..<octaves {
                    let n = noise2(channel, vx, vy, stitch, lattice, gradient)
                    total += fractalSum ? n / ratio : abs(n) / ratio
                    vx *= 2
                    vy *= 2
                    ratio *= 2
                    if var s = stitch {
                        s.width *= 2
                        s.wrapX = 2 * s.wrapX - Self.perlinN
                        s.height *= 2
                        s.wrapY = 2 * s.wrapY - Self.perlinN
                        stitch = s
                    }
                }
            }
        }
        return total
    }

    static func fractalValue(_ sum: Double) -> Double { min(max((sum + 1) / 2, 0), 1) }
}
