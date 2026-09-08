// swift-tools-version: 6.0
import PackageDescription

// MaryPiKit and the marypi CLI build on macOS and Linux; the SwiftUI app and
// the disk/flash features are macOS-only (DiskArbitration, diskutil, hdiutil).
var products: [Product] = [
    .library(name: "MaryPiKit", targets: ["MaryPiKit"]),
    .executable(name: "marypi", targets: ["marypi"]),
]

var targets: [Target] = [
    .target(
        name: "MaryPiKit",
        resources: [.copy("Resources")],
        linkerSettings: [.linkedFramework("DiskArbitration", .when(platforms: [.macOS]))]
    ),
    .executableTarget(
        name: "marypi",
        dependencies: [
            "MaryPiKit",
            .product(name: "ArgumentParser", package: "swift-argument-parser"),
        ]
    ),
    .testTarget(
        name: "MaryPiKitTests",
        dependencies: ["MaryPiKit"],
        resources: [.copy("Fixtures")]
    ),
]

#if os(macOS)
let infoPlist = "\(Context.packageDirectory)/Sources/MaryPiApp/Info.plist"
// Named MaryPiApp (not MaryPi) so its binary cannot collide with the
// marypi CLI on a case-insensitive filesystem. scripts/bundle.sh renames it.
products.append(.executable(name: "MaryPiApp", targets: ["MaryPiApp"]))
targets.append(
    .executableTarget(
        name: "MaryPiApp",
        dependencies: ["MaryPiKit"],
        exclude: ["Info.plist"],
        linkerSettings: [
            .unsafeFlags([
                "-Xlinker", "-sectcreate",
                "-Xlinker", "__TEXT",
                "-Xlinker", "__info_plist",
                "-Xlinker", infoPlist,
            ]),
        ]
    )
)
#endif

let package = Package(
    name: "MaryPi",
    platforms: [.macOS(.v15)],
    products: products,
    dependencies: [
        .package(url: "https://github.com/apple/swift-argument-parser.git", exact: "1.8.2"),
    ],
    targets: targets
)
