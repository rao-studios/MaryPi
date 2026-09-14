// swift-tools-version: 6.0
import PackageDescription

// The MaryOS kit's Mac side: MaryOSKit (Virtualization.framework runner,
// disks, flashing, build orchestration), the maryos CLI and the SwiftUI app;
// and MaryVNCKit, the viewer side of MaryVNC (MaryOS docs/14-maryvnc.md).
// The image itself is built from source by builder/ (Docker on a Mac).
//
// Virtualization.framework refuses processes without the
// com.apple.security.virtualization entitlement, so binaries are ad-hoc
// signed after building (scripts/sign.sh, run by the Makefile).
let infoPlist = "\(Context.packageDirectory)/Sources/MaryOSApp/Info.plist"

let package = Package(
    name: "MaryOS",
    platforms: [.macOS(.v15)],
    products: [
        .library(name: "MaryOSKit", targets: ["MaryOSKit"]),
        .library(name: "MaryVNCKit", targets: ["MaryVNCKit"]),
        .executable(name: "maryos", targets: ["maryos"]),
        // Named MaryOSApp so its binary cannot collide with the maryos CLI on
        // a case-insensitive filesystem; scripts/bundle.sh renames it.
        .executable(name: "MaryOSApp", targets: ["MaryOSApp"]),
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-argument-parser.git", exact: "1.8.2"),
    ],
    targets: [
        .target(
            name: "MaryOSKit",
            linkerSettings: [
                .linkedFramework("Virtualization"),
                .linkedFramework("DiskArbitration"),
            ]
        ),
        // Liquid Platinum for the Mac: tokens generated from the maryos submodule's lp_tokens.h
        // (make lp-tokens), maryui's brushed grain ported line for line, and the SwiftUI pieces.
        .target(name: "LiquidPlatinum"),
        // Noise over CryptoKit, the wire, Bonjour discovery and the session that talk to maryvncd.
        .target(
            name: "MaryVNCKit",
            linkerSettings: [
                .linkedFramework("Network"),
                .linkedFramework("Security"),
            ]
        ),
        .executableTarget(
            name: "maryos",
            dependencies: [
                "MaryOSKit",
                .product(name: "ArgumentParser", package: "swift-argument-parser"),
            ]
        ),
        .executableTarget(
            name: "MaryOSApp",
            dependencies: ["MaryOSKit"],
            exclude: ["Info.plist"],
            linkerSettings: [
                .unsafeFlags([
                    "-Xlinker", "-sectcreate",
                    "-Xlinker", "__TEXT",
                    "-Xlinker", "__info_plist",
                    "-Xlinker", infoPlist,
                ]),
            ]
        ),
        .testTarget(
            name: "MaryOSKitTests",
            dependencies: ["MaryOSKit"],
            resources: [.copy("Fixtures")]
        ),
        .testTarget(
            name: "LiquidPlatinumTests",
            dependencies: ["LiquidPlatinum"]
        ),
        .testTarget(
            name: "MaryVNCKitTests",
            dependencies: ["MaryVNCKit"],
            resources: [.copy("Fixtures")]
        ),
    ]
)
