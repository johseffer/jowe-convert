import JoweConvert
import JoweConvertC

public enum Smoke {
    public static var pluginABI: UInt32 { UInt32(JC_PLUGIN_ABI_VERSION) }
    public static var version: String { String(cString: jowe.convert.version()) }
}
