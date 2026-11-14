// swift-tools-version:5.9
import PackageDescription

// Release values: a release replaces the tag and the checksum below with the ones its workflow prints
// (`swift package compute-checksum jowe-convert-apple.xcframework.zip`).
let releaseTag = "v0.1.0"
let xcframeworkChecksum = "006aadcad8ba5c4dcf8f7cbbb2516fb322050c60151ca54f2a9a5d33e3fb9814"

let package = Package(
    name: "JoweConvert",
    platforms: [.iOS(.v16), .macCatalyst(.v16), .macOS(.v12)],
    products: [
        .library(name: "JoweConvert", targets: ["jowe-convert-apple", "JoweConvertSupport"]),
    ],
    targets: [
        .binaryTarget(
            name: "jowe-convert-apple",
            url: "https://github.com/johseffer/jowe-convert/releases/download/\(releaseTag)/jowe-convert-apple.xcframework.zip",
            checksum: xcframeworkChecksum
        ),
        .target(
            name: "JoweConvertSupport",
            dependencies: ["jowe-convert-apple"],
            path: "apple/Support",
            linkerSettings: [
                .linkedLibrary("c++"),
                .linkedLibrary("z"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("CoreFoundation"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("ImageIO"),
                .linkedFramework("VideoToolbox"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
            ]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
