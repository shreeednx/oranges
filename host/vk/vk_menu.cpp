// The settings menu over the game's picture (host/hos_menu.c is its model and input; this draws it). While it is
// open, each present composes the window texture's picture (its crop) and the menu into a canvas of the same size,
// and the canvas is what the window and the screenshots get (so HOS_CONTROL's SHOT shows the menu, headless too).
// Closed, it costs nothing: the picture goes out as before.
// Dear ImGui (third_party/imgui) draws it, with its draw lists only (no ImGui widgets, input or windows): the menu's
// own layout, in points of a 1280x720 screen scaled to the canvas, in the colours of Longshot's Modern dark menu theme
// (gpu/vk/vk_ui.h there). The font is the host's (the shared fonts' list in nn_system.c). Where each row, cell, tab
// and answer was drawn goes back to the model, in the console's 1280x720 coordinates, for the mouse.
// On an sRGB canvas the vertex colours are made linear first (the attachment encodes them again).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../hos_menu.h"
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "vk.h"

namespace ovk {
namespace {

bool tried = false, ready = false;
VkFormat pipeline_format = VK_FORMAT_UNDEFINED;
ImFont* body = nullptr;
ImFont* bold = nullptr;
int64_t last_frame = 0;
int first_row = 0;   // (the rows shown: from this one, scrolled to keep the selection in sight)

struct Canvas {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
} cv;

bool make_canvas(uint32_t w, uint32_t h, VkFormat format) {
    if (cv.image && cv.w == w && cv.h == h && cv.format == format) return true;
    if (cv.image) {
        vkDeviceWaitIdle(g.dev);
        vkDestroyImageView(g.dev, cv.view, nullptr);
        vkDestroyImage(g.dev, cv.image, nullptr);
        vkFreeMemory(g.dev, cv.mem, nullptr);
        cv = Canvas{};
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(g.dev, &ii, nullptr, &cv.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, cv.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g.dev, &ai, nullptr, &cv.mem) != VK_SUCCESS) return false;
    vkBindImageMemory(g.dev, cv.image, cv.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = cv.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(g.dev, &vi, nullptr, &cv.view) != VK_SUCCESS) return false;
    cv.w = w, cv.h = h, cv.format = format;
    return true;
}

ImFont* font_from(const char* const* paths, int n) {
    for (int i = 0; i < n; i++)
        if (FILE* f = fopen(paths[i], "rb")) {
            fclose(f);
            if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(paths[i], 18.0f)) {
                hos_log("vk: menu font %s", paths[i]);
                return font;
            }
        }
    return nullptr;
}

bool imgui_ready(VkFormat format) {
    if (ready && pipeline_format == format) return true;
    if (tried && !ready) return false;
    static VkFormat formats[1];
    formats[0] = format;
    ImGui_ImplVulkan_PipelineInfo pi{};
    pi.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    pi.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pi.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    pi.PipelineRenderingCreateInfo.pColorAttachmentFormats = formats;
    if (ready) {   // (the canvas changed between sRGB and UNORM: the pipeline again)
        vkDeviceWaitIdle(g.dev);
        ImGui_ImplVulkan_CreateMainPipeline(&pi);
        pipeline_format = format;
        return true;
    }
    tried = true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    static const char* const regular[] = {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "C:/Windows/Fonts/segoeui.ttf"};
    static const char* const strong[] = {"/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "C:/Windows/Fonts/segoeuib.ttf"};
    const char* own = getenv("HOS_MENU_FONT");   // (a font of the player's choice: <file.ttf>)
    body = own && *own ? ImGui::GetIO().Fonts->AddFontFromFileTTF(own, 18.0f) : nullptr;
    if (!body) body = font_from(regular, (int)(sizeof regular / sizeof *regular));
    bold = own && *own ? body : font_from(strong, (int)(sizeof strong / sizeof *strong));
    if (!body) body = io.Fonts->AddFontDefault(), hos_log("vk: menu: no TrueType font found (HOS_MENU_FONT=<file.ttf>)");
    if (!bold) bold = body;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = g.instance;
    ii.PhysicalDevice = g.phys;
    ii.Device = g.dev;
    ii.QueueFamily = g.family;
    ii.Queue = g.queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 2;
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain = pi;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        hos_log("vk: menu: Dear ImGui's Vulkan backend did not start: no menu");
        return false;
    }
    ready = true;
    pipeline_format = format;
    return true;
}

// ---- the look (Longshot's Modern dark: gpu/vk/vk_ui.h set_theme) ----
constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
constexpr ImU32 kInk = rgba(226, 230, 238), kInkBright = rgba(255, 255, 255), kSoft = rgba(190, 197, 210),
                kDim = rgba(140, 148, 164), kSel = rgba(110, 148, 250), kSelInk = rgba(8, 14, 32),
                kAccent = rgba(138, 174, 255), kEdge = rgba(255, 255, 255, 24), kCell = rgba(17, 19, 25, 242),
                kCellName = rgba(29, 32, 41, 238), kCellNameSel = rgba(38, 44, 60, 248), kBanner = rgba(19, 21, 28, 250),
                kBox = rgba(21, 23, 30, 250), kBoxEdge = rgba(255, 255, 255, 40), kBar = rgba(12, 13, 18, 238),
                kListen = rgba(70, 104, 200), kShade = rgba(5, 6, 9, 185), kTabInset = rgba(40, 46, 64, 250);

struct Draw {
    ImDrawList* d;
    float s, ox;   // (pixels per point; the 1280-point screen's left edge, for a canvas not 16:9)
    float w, h;    // (the canvas, pixels)
    std::vector<HosMenuHit> hits;
    ImVec2 p(float x, float y) const { return ImVec2(ox + x * s, y * s); }
    void rect(float x0, float y0, float x1, float y1, ImU32 c, float round = 4) {
        d->AddRectFilled(p(x0, y0), p(x1, y1), c, round * s);
    }
    void frame(float x0, float y0, float x1, float y1, ImU32 c, float round = 4, float thick = 1) {
        d->AddRect(p(x0, y0), p(x1, y1), c, round * s, 0, thick * s);
    }
    float width(ImFont* f, float pt, const char* t) const { return f->CalcTextSizeA(pt * s, FLT_MAX, 0, t).x / s; }
    // a line of text, its middle at y (capitals centred)
    void text(ImFont* f, float pt, float x, float y, ImU32 c, const char* t) {
        const float px = pt * s;
        d->AddText(f, px, ImVec2(ox + x * s, y * s - f->GetFontBaked(px)->Ascent + 0.36f * px), c, t);
    }
    void text_centre(ImFont* f, float pt, float cx, float y, ImU32 c, const char* t) { text(f, pt, cx - width(f, pt, t) / 2, y, c, t); }
    void text_right(ImFont* f, float pt, float x, float y, ImU32 c, const char* t) { text(f, pt, x - width(f, pt, t), y, c, t); }
    // wrapped from its top at y, at most wrap points wide: its height
    float para(ImFont* f, float pt, float x, float y, float wrap, ImU32 c, const char* t) {
        d->AddText(f, pt * s, p(x, y), c, t, nullptr, wrap * s);
        return f->CalcTextSizeA(pt * s, FLT_MAX, wrap * s, t).y / s;
    }
    // a text cut to fit room points (an ellipsis at its end)
    void text_fit(ImFont* f, float pt, float x, float y, float room, ImU32 c, const char* t) {
        if (width(f, pt, t) <= room) return text(f, pt, x, y, c, t);
        char buf[96];
        snprintf(buf, sizeof buf, "%s", t);
        for (size_t n = strlen(buf); n > 1; n--) {
            buf[n - 1] = 0;
            char tmp[100];
            snprintf(tmp, sizeof tmp, "%s...", buf);
            if (width(f, pt, tmp) <= room) return text(f, pt, x, y, c, tmp);
        }
    }
    void hit(float x0, float y0, float x1, float y1, int row, int col) {
        // (in the console's 1280x720, as the window's mouse is: the canvas is the picture)
        const float kx = 1280.0f / w, ky = 720.0f / h;
        hits.push_back({(ox + x0 * s) * kx, y0 * s * ky, (ox + x1 * s) * kx, y1 * s * ky, row, col});
    }
};

// the panel's layout, in points
constexpr float kL = 130, kR = 1150, kT = 40, kB = 680;          // the panel
constexpr float kRowsTop = 118, kRowH = 27, kRowsBottom = 548;     // the rows
constexpr float kNameX = kL + 30, kNameR = 470;                    // a row's name
constexpr float kCellX[3] = {480, 660, 840}, kCellW[3] = {170, 170, 280};   // a binding row's cells
constexpr float kValueX = 480, kValueR = 820;                      // a setting's value

void draw_menu(Draw& c, const HosMenuView& v) {
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), kShade);
    c.rect(kL + 6, kT + 10, kR + 6, kB + 10, rgba(0, 0, 0, 90), 10);   // (a shadow)
    c.rect(kL, kT, kR, kB, kBox, 8);
    c.frame(kL, kT, kR, kB, kBoxEdge, 8);
    // the banner: the name, the tabs
    c.rect(kL, kT, kR, kT + 56, kBanner, 8);
    c.d->AddLine(c.p(kL, kT + 56), c.p(kR, kT + 56), kEdge, c.s);
    c.text(bold, 15, kL + 22, kT + 28, kAccent, "oranges");
    c.text_right(body, 13, kR - 22, kT + 28, kDim, "F1 or Select + Start closes");
    float tx = 640 - 70 * v.ntabs;
    for (int i = 0; i < v.ntabs; i++, tx += 140) {
        const bool on = i == v.tab;
        if (on) c.rect(tx, kT + 12, tx + 140, kT + 44, kTabInset, 6);
        c.text_centre(bold, 17, tx + 70, kT + 28, on ? kInkBright : kSoft, v.tabs[i]);
        if (on) c.d->AddLine(c.p(tx + 24, kT + 46), c.p(tx + 116, kT + 46), kSel, 2.5f * c.s);
        c.hit(tx, kT + 12, tx + 140, kT + 44, -1, i);
    }
    // the rows: from first_row, the selection in sight
    const int fit = (int)((kRowsBottom - kRowsTop) / kRowH);
    if (v.sel < first_row + 1) first_row = std::max(0, v.sel - 1);
    if (v.sel > first_row + fit - 2) first_row = std::min(v.sel - fit + 2, std::max(0, v.nrows - fit));
    first_row = std::clamp(first_row, 0, std::max(0, v.nrows - fit));
    const char* listen = "Press...";
    for (int i = first_row; i < v.nrows && i < first_row + fit; i++) {
        const HosMenuRow& r = v.rows[i];
        const float y0 = kRowsTop + (i - first_row) * kRowH, y1 = y0 + kRowH - 3, ym = (y0 + y1) / 2;
        const bool sel = i == v.sel;
        if (r.kind == HOS_ROW_HEADING) {
            c.text(bold, 13, kNameX, ym + 2, kDim, r.label);
            c.text(bold, 13, kCellX[0] + 10, ym + 2, kDim, "Key");
            c.text(bold, 13, kCellX[1] + 10, ym + 2, kDim, "Other key");
            c.text(bold, 13, kCellX[2] + 10, ym + 2, kDim, "Gamepad");
            c.d->AddLine(c.p(kNameX, y1 + 1), c.p(kR - 30, y1 + 1), kEdge, c.s);
            continue;
        }
        c.rect(kNameX - 10, y0, kNameR, y1, sel ? kCellNameSel : kCellName, 4);
        if (sel) c.rect(kNameX - 10, y0, kNameX - 6, y1, kSel, 2);
        c.text_fit(sel ? bold : body, 15, kNameX + 4, ym, kNameR - kNameX - 26, sel ? kInkBright : kInk, r.label);
        if (r.changed) c.d->AddCircleFilled(c.p(kNameR - 12, ym), 3.2f * c.s, kAccent);
        c.hit(kNameX - 10, y0, kNameR, y1, i, -1);
        if (r.kind == HOS_ROW_SETTING || r.kind == HOS_ROW_CONTROLLER) {
            c.rect(kValueX, y0, kValueR, y1, sel ? kSel : kCell, 4);
            const ImU32 ink = sel ? kSelInk : kInk;
            ImFont* f = sel ? bold : body;
            const float room = kValueR - kValueX - 60;   // (between the arrows)
            if (c.width(f, 15, r.value) <= room) c.text_centre(f, 15, (kValueX + kValueR) / 2, ym, ink, r.value);
            else c.text_fit(f, 15, kValueX + 30, ym, room, ink, r.value);
            c.text(bold, 15, kValueX + 12, ym, sel ? kSelInk : kDim, "<");
            c.text_right(bold, 15, kValueR - 12, ym, sel ? kSelInk : kDim, ">");
            if (r.restart) c.text(body, 12, kValueR + 12, ym, kDim, "(after a restart)");
            c.hit(kValueX, y0, kValueR, y1, i, 0);
        } else if (r.kind == HOS_ROW_ACTION) {
            c.rect(kValueX, y0, kValueR, y1, sel ? kSel : kCell, 4);
            c.text_centre(sel ? bold : body, 15, (kValueX + kValueR) / 2, ym, sel ? kSelInk : kInk, "Reset...");
            c.hit(kValueX, y0, kValueR, y1, i, 0);
        } else if (r.kind == HOS_ROW_BINDING) {
            for (int k = 0; k < 3; k++) {
                const float x0 = kCellX[k], x1 = x0 + kCellW[k];
                const bool here = sel && k == v.col, wait = here && v.mode == HOS_MENU_CAPTURE;
                c.rect(x0, y0, x1, y1, wait ? kListen : here ? kSel : kCell, 4);
                if (!here) c.frame(x0, y0, x1, y1, kEdge, 4);
                const char* t = wait ? listen : r.cell[k][0] ? r.cell[k] : "-";
                const ImU32 ink = wait ? kInkBright : here ? kSelInk : r.cell[k][0] ? kInk : kDim;
                c.text_fit(here ? bold : body, 14, x0 + 10, ym, kCellW[k] - 20, ink, t);
                c.hit(x0, y0, x1, y1, i, k);
            }
        }
    }
    // the scroll bar
    if (v.nrows > fit) {
        const float top = kRowsTop, len = kRowsBottom - kRowsTop - 3, x = kR - 16;
        c.rect(x, top, x + 4, top + len, kEdge, 2);
        const float a = top + len * first_row / v.nrows, b = top + len * std::min(v.nrows, first_row + fit) / v.nrows;
        c.rect(x, a, x + 4, b, kDim, 2);
    }
    // the help, a note after a change, the prompts
    c.d->AddLine(c.p(kL + 20, kRowsBottom + 8), c.p(kR - 20, kRowsBottom + 8), kEdge, c.s);
    float y = kRowsBottom + 20;
    if (v.message[0]) y += c.para(bold, 14, kNameX, y, kR - kNameX - 40, kAccent, v.message) + 6;
    c.para(body, 14, kNameX, y, kR - kNameX - 40, kSoft, v.help);
    c.rect(kL, kB - 38, kR, kB, kBar, 8);
    c.text(body, 14, kNameX, kB - 19, v.mode == HOS_MENU_CAPTURE ? kInkBright : kSoft, v.prompts);
    // the question, over it all
    if (v.mode == HOS_MENU_CONFIRM) {
        c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), rgba(0, 0, 0, 110));
        constexpr float qL = 370, qR = 910, qT = 250, qB = 440;
        c.rect(qL + 5, qT + 8, qR + 5, qB + 8, rgba(0, 0, 0, 110), 10);
        c.rect(qL, qT, qR, qB, kBox, 8);
        c.frame(qL, qT, qR, qB, kBoxEdge, 8);
        c.para(body, 16, qL + 28, qT + 26, qR - qL - 56, kInkBright, v.question);
        const char* answers[2] = {"No", "Yes"};
        for (int k = 0; k < 2; k++) {
            const float x0 = k ? 650 : 470, x1 = x0 + 160, y0 = qB - 62, y1 = qB - 24;
            const bool on = v.confirm_yes == k;
            c.rect(x0, y0, x1, y1, on ? kSel : kCell, 6);
            if (!on) c.frame(x0, y0, x1, y1, kBoxEdge, 6);
            c.text_centre(bold, 16, (x0 + x1) / 2, (y0 + y1) / 2, on ? kSelInk : kInk, answers[k]);
            c.hit(x0, y0, x1, y1, -2, k);
        }
    }
}

uint8_t to_linear(uint8_t c) {
    static uint8_t lut[256];
    static bool made = false;
    if (!made) {
        for (int i = 0; i < 256; i++) {
            const float f = i / 255.0f;
            const float l = f <= 0.04045f ? f / 12.92f : std::pow((f + 0.055f) / 1.055f, 2.4f);
            lut[i] = (uint8_t)std::lround(l * 255.0f);
        }
        made = true;
    }
    return lut[c];
}

}  // namespace

bool menu_compose(VkImage* image, VkFormat* format, uint32_t* w, uint32_t* h, const int** crop) {
    HosMenuView v;
    hos_menu_view(&v);
    if (!v.open) return false;
    const int* cr = *crop;
    const int32_t sx = cr ? cr[0] : 0, sy = cr ? cr[1] : 0;
    const uint32_t cw = cr && cr[2] > 0 ? (uint32_t)cr[2] : *w, ch = cr && cr[3] > 0 ? (uint32_t)cr[3] : *h;
    const bool srgb = *format == VK_FORMAT_R8G8B8A8_SRGB || *format == VK_FORMAT_B8G8R8A8_SRGB ||
                      *format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    const VkFormat want = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!cw || !ch || !make_canvas(cw, ch, want) || !imgui_ready(want)) return false;
    // the menu's draw lists
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)cw, (float)ch);
    const int64_t now = hos_now_ns();
    io.DeltaTime = last_frame && now > last_frame ? std::min((float)((now - last_frame) / 1e9), 0.25f) : 1.0f / 30;
    last_frame = now;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    Draw c;
    c.d = ImGui::GetForegroundDrawList();
    c.w = (float)cw, c.h = (float)ch;
    c.s = c.h / 720.0f;
    c.ox = (c.w - 1280.0f * c.s) / 2;
    draw_menu(c, v);
    hos_menu_hits(c.hits.data(), (int)c.hits.size());
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (srgb)
        for (ImDrawList* list : dd->CmdLists)
            for (ImDrawVert& vert : list->VtxBuffer) {
                const ImU32 col = vert.col;
                vert.col = (col & IM_COL32_A_MASK) | (ImU32)to_linear((uint8_t)(col >> IM_COL32_R_SHIFT)) << IM_COL32_R_SHIFT |
                           (ImU32)to_linear((uint8_t)(col >> IM_COL32_G_SHIFT)) << IM_COL32_G_SHIFT |
                           (ImU32)to_linear((uint8_t)(col >> IM_COL32_B_SHIFT)) << IM_COL32_B_SHIFT;
            }
    // the picture into the canvas, the menu over it
    VkCommandBuffer cb = begin_commands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = cv.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkImageBlit bl{};
    bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[0] = {sx, sy, 0}, bl.srcOffsets[1] = {sx + (int32_t)cw, sy + (int32_t)ch, 1};
    bl.dstOffsets[1] = {(int32_t)cw, (int32_t)ch, 1};
    vkCmdBlitImage(cb, *image, VK_IMAGE_LAYOUT_GENERAL, cv.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    full_barrier(cb);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = cv.view;
    att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {cw, ch}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    vkCmdEndRendering(cb);
    full_barrier(cb);
    end_and_wait(cb);
    *image = cv.image, *format = want, *w = cw, *h = ch, *crop = nullptr;
    return true;
}

}  // namespace ovk
