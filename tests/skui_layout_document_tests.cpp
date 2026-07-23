#include "skui_runtime.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool nearlyEqual(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool testDocumentTypesAndCompatibility() {
    constexpr std::string_view kLegacyPage = R"html(
<!doctype html><html><head></head><body><div>legacy</div></body></html>
)html";
    constexpr std::string_view kExplicitPage = R"html(
<!doctype html><html><head>
<meta name="skui-document-type" content="page">
</head><body></body></html>
)html";
    constexpr std::string_view kLayout = R"html(
<!doctype html><html><head>
<meta name="skui-document-type" content="layout">
</head><body><skui-page id="main" src="main.html"></skui-page></body></html>
)html";

    skui::Runtime runtime;
    bool passed = expect(runtime.loadDocumentFromString(kLegacyPage),
                         "legacy page should default to page") &&
                  expect(runtime.documentType() == skui::DocumentType::Page,
                         "legacy page should report page type") &&
                  expect(runtime.loadDocumentFromString(
                             kExplicitPage, skui::DocumentType::Page),
                         "explicit page should load as page") &&
                  expect(!runtime.loadDocumentFromString(kLayout),
                         "legacy page loader must reject a layout document") &&
                  expect(runtime.lastError().find("expected page") != std::string::npos,
                         "layout mismatch should name the expected page role") &&
                  expect(runtime.loadDocumentFromString(
                             kLayout, skui::DocumentType::Layout),
                         "layout loader should accept a layout document") &&
                  expect(runtime.documentType() == skui::DocumentType::Layout,
                         "layout should report layout type") &&
                  expect(!runtime.loadDocumentFromString(
                             kExplicitPage, skui::DocumentType::Layout),
                         "layout loader must reject a page document") &&
                  expect(runtime.lastError().find("expected layout") != std::string::npos,
                         "page mismatch should name the expected layout role");
    return passed;
}

bool testInvalidDeclarations() {
    constexpr std::string_view kInvalidType = R"html(
<!doctype html><html><head>
<meta name="skui-document-type" content="widget">
</head><body></body></html>
)html";
    constexpr std::string_view kPageWithLayoutChild = R"html(
<!doctype html><html><head></head><body>
<skui-page id="bad" src="bad.html"></skui-page>
</body></html>
)html";
    constexpr std::string_view kLayoutWithoutSource = R"html(
<!doctype html><html><head>
<meta name="skui-document-type" content="layout">
</head><body><skui-page id="bad"></skui-page></body></html>
)html";

    skui::Runtime runtime;
    return expect(!runtime.loadDocumentFromString(kInvalidType),
                  "unknown document type must fail") &&
           expect(runtime.lastError().find("widget") != std::string::npos,
                  "unknown document type error should include its value") &&
           expect(!runtime.loadDocumentFromString(kPageWithLayoutChild),
                  "page documents must reject skui-page") &&
           expect(runtime.lastError().find("layout documents") != std::string::npos,
                  "skui-page role error should explain the layout requirement") &&
           expect(!runtime.loadDocumentFromString(
                      kLayoutWithoutSource, skui::DocumentType::Layout),
                  "skui-page without src must fail") &&
           expect(runtime.lastError().find("src") != std::string::npos,
                  "missing source error should name src");
}

bool testCssLayoutSnapshots() {
    constexpr std::string_view kHtml = R"html(
<!doctype html><html><head>
<meta name="skui-document-type" content="layout">
<style>
html, body { width: 100%; height: 100%; margin: 0; }
body { position: relative; }
#full { position: absolute; inset: 0; }
#left { position: absolute; left: 20px; top: 30px; width: 200px; height: 100px; }
#right { position: absolute; right: 16px; top: 12px; width: 120px; height: 80px;
         z-index: 4; opacity: 0.5; pointer-events: none;
         transform: translate(5px, 7px); }
</style>
</head><body>
<skui-page id="full" src="pages/full.html"></skui-page>
<skui-page id="left" src="pages/left.html"></skui-page>
<skui-page id="right" src="pages/right.html"></skui-page>
</body></html>
)html";

    const std::filesystem::path basePath =
        std::filesystem::path("tmp") / "layout-document-tests";
    skui::Runtime runtime;
    if (!expect(runtime.loadDocumentFromString(
                    kHtml,
                    skui::DocumentType::Layout,
                    basePath.string()),
                "layout fixture should load")) {
        std::cerr << runtime.lastError() << '\n';
        return false;
    }
    runtime.resize(800, 600, 1.0f);
    const std::vector<skui::LayoutPageSnapshot> pages =
        runtime.layoutPageSnapshots();
    if (!expect(pages.size() == 3, "layout should expose three page snapshots")) {
        return false;
    }

    const skui::LayoutPageSnapshot& full = pages[0];
    const skui::LayoutPageSnapshot& left = pages[1];
    const skui::LayoutPageSnapshot& right = pages[2];
    const std::filesystem::path expectedRight =
        (basePath / "pages" / "right.html").lexically_normal();
    return expect(full.id == "full", "full page should keep DOM order") &&
           expect(nearlyEqual(full.rect.x, 0.0f) &&
                      nearlyEqual(full.rect.y, 0.0f) &&
                      nearlyEqual(full.rect.width, 800.0f) &&
                      nearlyEqual(full.rect.height, 600.0f),
                  "inset zero should fill the layout viewport") &&
           expect(nearlyEqual(left.rect.x, 20.0f) &&
                      nearlyEqual(left.rect.y, 30.0f) &&
                      nearlyEqual(left.rect.width, 200.0f) &&
                      nearlyEqual(left.rect.height, 100.0f),
                  "absolute CSS placement should be exported") &&
           expect(right.id == "right" && right.zIndex == 4,
                  "z-index should be exported") &&
           expect(nearlyEqual(right.rect.x, 664.0f) &&
                      nearlyEqual(right.rect.y, 12.0f),
                  "right/top CSS placement should be exported") &&
           expect(nearlyEqual(right.opacity, 0.5f),
                  "opacity should be exported") &&
           expect(!right.hitTestable,
                  "pointer-events none should disable page hit testing") &&
           expect(nearlyEqual(right.transform.translationX, 5.0f) &&
                      nearlyEqual(right.transform.translationY, 7.0f),
                  "CSS transform should be exported") &&
           expect(std::filesystem::path(right.resolvedSource).lexically_normal() ==
                      expectedRight,
                  "page source should resolve relative to the layout document");
}

bool testFlowLayoutAndClipping() {
    constexpr std::string_view kHtml = R"html(
<!doctype html><html><head>
<meta name="skui-document-type" content="layout">
<style>
html, body { width: 100%; height: 100%; margin: 0; }
body { display: flex; flex-direction: column; }
#flex { display: flex; width: 400px; height: 100px; justify-content: space-between; }
#flex-a, #flex-b { width: 100px; height: 80px; }
#grid { display: grid; grid-template-columns: 120px 180px; width: 300px; height: 90px; }
#grid-a, #grid-b { height: 70px; }
#clip { position: absolute; left: 30px; top: 220px; width: 100px; height: 90px;
        overflow: hidden; opacity: 0.5; }
#clipped { position: absolute; left: 70px; top: 60px; width: 60px; height: 50px;
           opacity: 0.5; }
#hidden { position: absolute; left: 200px; top: 220px; width: 40px; height: 40px;
          visibility: hidden; }
</style>
</head><body>
<div id="flex">
  <skui-page id="flex-a" src="a.html"></skui-page>
  <skui-page id="flex-b" src="b.html"></skui-page>
</div>
<div id="grid">
  <skui-page id="grid-a" src="c.html"></skui-page>
  <skui-page id="grid-b" src="d.html"></skui-page>
</div>
<div id="clip">
  <skui-page id="clipped" src="clipped.html"></skui-page>
</div>
<skui-page id="hidden" src="hidden.html"></skui-page>
</body></html>
)html";

    skui::Runtime runtime;
    if (!runtime.loadDocumentFromString(kHtml, skui::DocumentType::Layout)) {
        std::cerr << runtime.lastError() << '\n';
        return false;
    }
    runtime.resize(600, 400, 1.0f);
    const std::vector<skui::LayoutPageSnapshot> pages =
        runtime.layoutPageSnapshots();
    if (!expect(pages.size() == 6, "flow layout should expose six pages")) {
        return false;
    }

    const skui::LayoutPageSnapshot& flexA = pages[0];
    const skui::LayoutPageSnapshot& flexB = pages[1];
    const skui::LayoutPageSnapshot& gridA = pages[2];
    const skui::LayoutPageSnapshot& gridB = pages[3];
    const skui::LayoutPageSnapshot& clipped = pages[4];
    const skui::LayoutPageSnapshot& hidden = pages[5];
    return expect(nearlyEqual(flexA.rect.x, 0.0f) &&
                      nearlyEqual(flexB.rect.x, 300.0f),
                  "flex justification should position page nodes") &&
           expect(nearlyEqual(gridA.rect.x, 0.0f) &&
                      nearlyEqual(gridB.rect.x, 120.0f),
                  "grid tracks should position page nodes") &&
           expect(clipped.clipRect.has_value() &&
                      nearlyEqual(clipped.clipRect->x, 30.0f) &&
                      nearlyEqual(clipped.clipRect->y, 220.0f) &&
                      nearlyEqual(clipped.clipRect->width, 100.0f) &&
                      nearlyEqual(clipped.clipRect->height, 90.0f),
                  "overflow clipping should be exported") &&
           expect(nearlyEqual(clipped.opacity, 0.25f),
                  "ancestor and page opacity should be combined") &&
           expect(clipped.visible,
                  "a partially clipped page should remain visible") &&
           expect(!hidden.visible && !hidden.hitTestable,
                  "visibility hidden should disable drawing and hit testing");
}

bool testLayoutDoesNotRenderPixels() {
    constexpr std::string_view kHtml = R"html(
<!doctype html><html><head>
<meta name="skui-document-type" content="layout">
</head><body style="background: red">
<skui-page id="main" src="main.html"></skui-page>
</body></html>
)html";

    skui::Runtime runtime;
    if (!runtime.loadDocumentFromString(kHtml, skui::DocumentType::Layout)) {
        return false;
    }
    std::vector<uint32_t> pixels(16u * 16u, 0x12345678u);
    return expect(!runtime.renderToBgraPixels(
                      pixels.data(), 16, 16, 16u * sizeof(uint32_t), 1.0f),
                  "layout documents must reject pixel rendering") &&
           expect(runtime.lastError().find("layout documents") != std::string::npos,
                  "layout render error should explain the role restriction");
}

}  // namespace

int main() {
    const bool passed = testDocumentTypesAndCompatibility() &&
                        testInvalidDeclarations() &&
                        testCssLayoutSnapshots() &&
                        testFlowLayoutAndClipping() &&
                        testLayoutDoesNotRenderPixels();
    if (!passed) {
        return 1;
    }
    std::cout << "SkiaUI layout document tests passed\n";
    return 0;
}
