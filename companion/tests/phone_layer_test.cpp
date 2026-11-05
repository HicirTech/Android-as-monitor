#include "../phone_layer.h"

#include <cstdio>
#include <string>

using namespace aum;

static int failures = 0;

static void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// Captured from `dumpsys SurfaceFlinger` on Android 16, with the layer streaming.
static const std::string kTable =
    " Layer name\n"
    "           Z |  Window Type |  Comp Type |  Transform |   Disp Frame (LTRB) |          Source Crop (LTRB) |     Frame Rate (Explicit) (Seamlessness) [\n"
    "------------------------------------------------------\n"
    " 9096a6c SurfaceView[dev.androidusbmo[...]dusbmonitor.MainActivity](BLAST)#362\n"
    "           7 |            0 |     DEVICE |     ROT_90 |    0    0 1080 2400 |    0.0    0.0 2400.0 1080.0 |  60.00           Exact SeamedAndSeamless    [*]\n"
    "------------------------------------------------------\n"
    " ScreenDecorOverlay#73\n"
    "          11 |         2024 |     DEVICE |     ROT_90 |    0    0 1080  152 |    0.0    0.0  152.0 1080.0 |        Cat::NoPreference           Default    [ ]\n";

// Captured on a Galaxy Z Fold 7 with Android 17: one column more, and the background layer's name just above the video's.
static const std::string kAndroid17Table =
    " Layer name\n"
    "           Z |  Window Type |  Layer Class | Comp Type |  Transform |   Disp Frame (LTRB) |          Source Crop (LTRB) | DefaultFrameRateCompatibility |     Frame Rate (Explicit) (Seamlessness) [Focused] [FrameBooster]\n"
    "------------------------------------------------------\n"
    " Background for 11389b SurfaceView[de[...]roidusbmonitor.MainActivity]@0#97581\n"
    "          17 |            0 |            0 |      SOLID |          0 |    0    0 2184 1968 |    0.0    0.0 2184.0 1968.0 |                      Default |  Cat::NoPreference           Default    [ ]     [ ]\n"
    "------------------------------------------------------\n"
    " 11389b SurfaceView[dev.androidusbmon[...]monitor.MainActivity]@0(BLAST)#97580\n"
    "          18 |            0 |            0 |     DEVICE |          0 |    0    0 2184 1968 |    0.0    0.0 2184.0 1968.0 |                      Default | 120.00           Exact SeamedAndSeamless    [*]     [ ]\n";

static void ReadsTheVideoRowFromTheCompositionTable() {
    const auto layer = FindVideoLayer("Layer [362] 9096a6c SurfaceView[dev.androidusbmonitor/dev.androidusbmonitor.MainActivity](BLAST)#362\n   bounds={0,0,1080,2400}\n" + kTable);
    Check(layer.has_value(), "the row is found past the layer list, which mentions the same name");
    Check(layer->composition == "DEVICE", "composition type");
    Check(layer->transform == "ROT_90", "transform");
    Check(layer->displayWidth == 1080 && layer->displayHeight == 2400, "display frame");
    Check(layer->sourceWidth == 2400 && layer->sourceHeight == 1080, "source crop");
    Check(layer->MapsPixelsOneToOne(), "a quarter turn of the same size is one to one");
}

static void FindsColumnsByNameWhereTheTableHasMoreOfThem() {
    const auto layer = FindVideoLayer(kAndroid17Table);
    Check(layer.has_value(), "the row is found in the wider table");
    Check(layer->composition == "DEVICE", "composition type, one column further on");
    Check(layer->transform == "0", "transform");
    Check(layer->displayWidth == 2184 && layer->displayHeight == 1968, "display frame");
    Check(layer->sourceWidth == 2184 && layer->sourceHeight == 1968, "source crop");
    Check(layer->MapsPixelsOneToOne(), "same size without a turn is one to one");

    std::string scaled = kAndroid17Table;
    const std::string crop = "0.0    0.0 2184.0 1968.0 |                      Default | 120.00";
    scaled.replace(scaled.find(crop), crop.size(), "0.0    0.0 2400.0 1080.0 |                      Default | 120.00");
    const auto scaledLayer = FindVideoLayer(scaled);
    Check(scaledLayer.has_value() && scaledLayer->sourceWidth == 2400 && scaledLayer->sourceHeight == 1080, "the source crop is its own column");
    Check(scaledLayer.has_value() && !scaledLayer->MapsPixelsOneToOne(), "and a crop of another size is not one to one");
}

static void AScaledLayerIsNotOneToOne() {
    PhoneVideoLayer layer;
    layer.displayWidth = 2400;
    layer.displayHeight = 1080;
    layer.sourceWidth = 2432;
    layer.sourceHeight = 1080;
    Check(!layer.MapsPixelsOneToOne(), "a source wider than the display frame is scaled");
}

static void AMissingLayerIsReportedAsMissing() {
    Check(!FindVideoLayer("no such table here\n").has_value(), "no row");
}

static void FindsTheFullLayerNameInEitherListFormat() {
    Check(FindVideoLayerName(
              "ScreenDecorOverlay#73\n"
              "301c3dd SurfaceView[dev.androidusbmonitor/dev.androidusbmonitor.MainActivity](BLAST)#162\n"
              "Background for 301c3dd SurfaceView[dev.androidusbmonitor/dev.androidusbmonitor.MainActivity]#163\n") ==
              "301c3dd SurfaceView[dev.androidusbmonitor/dev.androidusbmonitor.MainActivity](BLAST)#162",
          "the plain list of Android 16");
    Check(FindVideoLayerName(
              "RequestedLayerState{8fcefb0 dev.androidusbmonitor/dev.androidusbmonitor.MainActivity#97526 parentId=97517}\n"
              "RequestedLayerState{11389b SurfaceView[dev.androidusbmonitor/dev.androidusbmonitor.MainActivity]@0(BLAST)#98903 parentId=98901}\n") ==
              "11389b SurfaceView[dev.androidusbmonitor/dev.androidusbmonitor.MainActivity]@0(BLAST)#98903",
          "the wrapped list of Android 17");
}

static void TheNewestLayerWins() {
    Check(FindVideoLayerName(
              "1 SurfaceView[dev.androidusbmonitor/dev.androidusbmonitor.MainActivity](BLAST)#10\n"
              "2 SurfaceView[dev.androidusbmonitor/dev.androidusbmonitor.MainActivity](BLAST)#20\n") ==
              "2 SurfaceView[dev.androidusbmonitor/dev.androidusbmonitor.MainActivity](BLAST)#20",
          "a layer left from before the restart is not the one to read");
}

static void NoLayerGivesNoName() {
    Check(FindVideoLayerName("ScreenDecorOverlay#73\n").empty(), "the app is not showing");
}

int main() {
    ReadsTheVideoRowFromTheCompositionTable();
    FindsColumnsByNameWhereTheTableHasMoreOfThem();
    AScaledLayerIsNotOneToOne();
    AMissingLayerIsReportedAsMissing();
    FindsTheFullLayerNameInEitherListFormat();
    TheNewestLayerWins();
    NoLayerGivesNoName();
    std::printf(failures == 0 ? "phone layer tests passed\n" : "%d phone layer checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
