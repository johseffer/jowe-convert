// swift-tools-version:5.9
import PackageDescription

// Consumes the locally built xcframework (copied next to this file by CI) the way an app consumes the released one.
let package = Package(
    name: "Smoke",
    platforms: [.iOS(.v16), .macOS(.v12)],
    products: [.library(name: "SmokeLib", targets: ["SmokeLib"])],
    targets: [
        .binaryTarget(name: "jowe-convert-apple", path: "jowe-convert-apple.xcframework"),
        .target(
            name: "JoweConvertSupport",
            dependencies: ["jowe-convert-apple"],
            linkerSettings: [
                .linkedLibrary("c++"), .linkedLibrary("z"),
                .linkedFramework("AudioToolbox"), .linkedFramework("CoreFoundation"),
                .linkedFramework("CoreGraphics"), .linkedFramework("ImageIO"),
            ]
        ),
        .target(
            name: "SmokeLib",
            dependencies: ["JoweConvertSupport"],
            swiftSettings: [.interoperabilityMode(.Cxx)]
        ),
        .executableTarget(
            name: "Smoke",
            dependencies: ["SmokeLib"],
            swiftSettings: [.interoperabilityMode(.Cxx)]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
