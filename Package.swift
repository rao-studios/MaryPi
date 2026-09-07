// swift-tools-version: 6.0
import PackageDescription

let infoPlist = "\(Context.packageDirectory)/Sources/MaryPiApp/Info.plist"

let package = Package(
    name: "MaryPi",
    platforms: [.macOS(.v15)],
    products: [
        .library(name: "MaryPiKit", targets: ["MaryPiKit"]),
        .executable(name: "marypi", targets: ["marypi"]),
        // Named MaryPiApp (not MaryPi) so its binary cannot collide with the
        // marypi CLI on a case-insensitive filesystem. bundle.sh renames it.
        .executable(name: "MaryPiApp", targets: ["MaryPiApp"]),
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-argument-parser.git", exact: "1.8.2"),
    ],
    targets: [
        .target(
            name: "MaryPiKit",
            resources: [.copy("Resources")],
            linkerSettings: [.linkedFramework("DiskArbitration")]
        ),
        .executableTarget(
            name: "marypi",
            dependencies: [
                "MaryPiKit",
                .product(name: "ArgumentParser", package: "swift-argument-parser"),
            ]
        ),
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
        ),
        .testTarget(
            name: "MaryPiKitTests",
            dependencies: ["MaryPiKit"],
            resources: [.copy("Fixtures")]
        ),
    ]
)
