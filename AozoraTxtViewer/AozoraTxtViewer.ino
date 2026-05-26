// ============================================================
// AozoraTxtViewer for M5Cardputer
// 青空文庫テキストビューワー（UTF-8変換済みテキスト対応）
//
// 流用元: handomade/MP3PlayerForM5CardputerJ
//
// 必要ライブラリ:
//   - M5Cardputer (by M5Stack)
//   - M5Unified   (by M5Stack)
//   - OpenFontRender (by takkaO)
//
// SDカード構成:
//   /texts/   ← UTF-8変換済み .txt を置く
//   /fonts/   ← 日本語TTFを置く（例: NotoSansJP.ttf）
//
// パーティションスキーム: Huge APP (3MB No OTA/1MB SPIFFS)
// ============================================================

#include <M5Unified.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <Preferences.h>
#include <OpenFontRender.h>
#include "ofrfs/M5Stack_SD_Preset.h"

// ============================================================
// ピン定義（MP3プレイヤーと同じ）
// ============================================================
#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN   12

// ============================================================
// 画面レイアウト定数
// ============================================================
#define HEADER_HEIGHT    20
#define FOOTER_HEIGHT    18
#define MARGIN_X          5
#define CONTENT_Y        (HEADER_HEIGHT + 4)
#define CONTENT_H        (135 - HEADER_HEIGHT - FOOTER_HEIGHT - 4)
#define MAX_LINE_PX      230
#define MAX_LINES_BUF    12  // 表示可能な最大行数より余裕を持たせる

// フォントサイズ（切り替え可能: 10/12/14/16 px）
int g_fontSize = 12;
inline int charW(int s)    { return (s == 1) ? (g_fontSize / 2) : g_fontSize; }
inline int lineH()         { return g_fontSize + 3; }
inline int maxLinesN()     { return CONTENT_H / lineH(); }

// ============================================================
// 青空文庫 テキスト処理
// ============================================================

// 1行からルビ・注記を除去して返す
String removeAozoraMarkup(const String& line) {
    String out;
    out.reserve(line.length());
    const char* p = line.c_str();
    while (*p) {
        // 「《」(E3 80 8A) → 「》」(E3 80 8B) を読み飛ばす
        if ((uint8_t)p[0]==0xE3 && (uint8_t)p[1]==0x80 && (uint8_t)p[2]==0x8A) {
            p += 3;
            while (*p) {
                if ((uint8_t)p[0]==0xE3 && (uint8_t)p[1]==0x80 && (uint8_t)p[2]==0x8B) { p += 3; break; }
                uint8_t c = (uint8_t)*p;
                p += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            }
            continue;
        }
        // 「｜」(EF BD 9C) を読み飛ばす
        if ((uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBD && (uint8_t)p[2]==0x9C) { p += 3; continue; }
        // 「［」(EF BC BB) → 「］」(EF BC BD) を読み飛ばす
        if ((uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBC && (uint8_t)p[2]==0xBB) {
            p += 3;
            while (*p) {
                if ((uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBC && (uint8_t)p[2]==0xBD) { p += 3; break; }
                uint8_t c = (uint8_t)*p;
                p += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            }
            continue;
        }
        uint8_t c = (uint8_t)*p;
        int bytes = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        for (int i = 0; i < bytes; i++) out += p[i];
        p += bytes;
    }
    return out;
}

// 青空文庫モード時にスキップする行か判定
bool isSkipLine(const String& line) {
    if (line.startsWith("-------")) return true;
    if (line.startsWith("底本：") || line.startsWith("底本:")) return true;
    return false;
}

// UTF-8文字列をmaxPxに収まるよう切り詰める（MP3のfitJPStringと同じロジック）
String fitLine(const String& s, int maxPx) {
    String r;
    int used = 0;
    const char* p = s.c_str();
    while (*p) {
        uint8_t c = (uint8_t)*p;
        int step = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        int w    = charW(step);
        if (used + w > maxPx) break;
        for (int i = 0; i < step; i++) r += (char)*(p + i);
        p += step; used += w;
    }
    return r;
}

// ============================================================
// OpenFontRender
// ============================================================
OpenFontRender g_ofr;
bool g_jpFontLoaded = false;

// ============================================================
// テーマカラー（ライト/ダークモード切替対応）
// ============================================================
bool g_lightMode = false;
inline uint16_t colBg()        { return g_lightMode ? 0xFFFF : 0x1002; }
inline uint16_t colHeader()    { return g_lightMode ? 0x39E7 : 0x18E3; }
inline uint16_t colAccent()    { return g_lightMode ? 0x047F : 0x05BF; }
inline uint16_t colTextMain()  { return g_lightMode ? 0x0000 : 0xFFFF; }
inline uint16_t colTextDim()   { return g_lightMode ? 0x632C : 0x9492; }
inline uint16_t colPanelText() { return 0xFFFF; }
inline uint16_t colPanelDim()  { return g_lightMode ? 0xCE79 : 0x9492; }
inline uint16_t colSelectText(){ return 0xFFFF; }
inline uint16_t colHighlight() { return 0xF81F; }

// ============================================================
// UIステート
// ============================================================
enum AppState { STATE_FILE_SELECT, STATE_READING };
AppState g_state = STATE_FILE_SELECT;

// ============================================================
// ファイル一覧
// ============================================================
#define TEXTS_DIR     "/texts"
#define MAX_FILES     64
#define FILE_ROW_H    15
#define MAX_FILE_ROWS 6

String g_fileList[MAX_FILES];
int    g_fileCount  = 0;
int    g_fileCursor = 0;
int    g_fileScroll = 0;

void scanTextFiles() {
    g_fileCount = 0;
    File dir = SD.open(TEXTS_DIR);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && g_fileCount < MAX_FILES) {
        String name = f.name(); name.toLowerCase();
        if (!f.isDirectory() && name.endsWith(".txt"))
            g_fileList[g_fileCount++] = String(f.path());
        f = dir.openNextFile();
    }
}

// ============================================================
// 読書エンジン
// ============================================================
#define PAGE_OFFSETS_MAX 2048
#define READ_BUF_SIZE    256

String   g_currentFile = "";
uint32_t g_pageOffsets[PAGE_OFFSETS_MAX];
uint8_t  g_pageLineSkips[PAGE_OFFSETS_MAX]; // ページ開始時に読み飛ばす折り返し行数
bool     g_pageInBlock[PAGE_OFFSETS_MAX];  // ページ開始時のskipBlock状態
int      g_pageCount   = 0;
int      g_currentPage = 0;
bool     g_aozoraMode  = true;   // true=青空モード, false=通常モード

Preferences g_prefs;

// ---- NVSキー生成（ファイル名先頭12文字 + サフィックス） ----
// Preferencesのキー上限は15文字。page/modeで2文字使うので12文字+2=14文字。
String makeKey(const String& filepath, const char* suffix) {
    String key = filepath;
    int sl = key.lastIndexOf('/');
    if (sl >= 0) key = key.substring(sl + 1);
    int dot = key.lastIndexOf('.');
    if (dot > 0) key = key.substring(0, dot);
    if (key.length() > 12) key = key.substring(0, 12);
    return key + suffix;
}

// ---- しおり + モード 保存 ----
void saveState(const String& filepath, int page, bool aozora) {
    g_prefs.begin("aozora", false);
    g_prefs.putInt(makeKey(filepath, "_p").c_str(), page);
    g_prefs.putBool(makeKey(filepath, "_m").c_str(), aozora);
    g_prefs.end();
}

// ---- しおり読み込み ----
int loadBookmark(const String& filepath) {
    g_prefs.begin("aozora", true);
    int page = g_prefs.getInt(makeKey(filepath, "_p").c_str(), 0);
    g_prefs.end();
    return page;
}

// ---- モード読み込み（デフォルト: 青空モード） ----
bool loadMode(const String& filepath) {
    g_prefs.begin("aozora", true);
    bool aozora = g_prefs.getBool(makeKey(filepath, "_m").c_str(), true);
    g_prefs.end();
    return aozora;
}

int splitDisplayLines(const String& text, String lines[], int maxLines,
                      int skipLines, int& lineCount) {
    if (text.length() == 0) {
        if (skipLines <= 0 && lines && lineCount < maxLines) lines[lineCount++] = "";
        return 1;
    }

    const char* p = text.c_str();
    String curLine;
    int usedPx = 0;
    int displayLine = 0;

    while (*p) {
        uint8_t c = (uint8_t)*p;
        int step = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        int w = charW(step);
        if (usedPx + w > MAX_LINE_PX) {
            if (displayLine >= skipLines && lines && lineCount < maxLines) {
                lines[lineCount++] = curLine;
            }
            displayLine++;
            curLine = "";
            usedPx = 0;
            if (lines && lineCount >= maxLines) {
                while (*p) {
                    uint8_t c2 = (uint8_t)*p;
                    int step2 = (c2 < 0x80) ? 1 : (c2 < 0xE0) ? 2 : (c2 < 0xF0) ? 3 : 4;
                    int w2 = charW(step2);
                    if (usedPx + w2 > MAX_LINE_PX) { displayLine++; usedPx = 0; }
                    usedPx += w2;
                    p += step2;
                }
                return displayLine + 1;
            }
        }
        for (int i = 0; i < step; i++) curLine += (char)*(p + i);
        usedPx += w;
        p += step;
    }

    if (displayLine >= skipLines && lines && lineCount < maxLines) {
        lines[lineCount++] = curLine;
    }
    return displayLine + 1;
}

// ---- 1行を処理してページに収まる行数を返す ----
// aozoraMode=trueのときはmarkup除去・skip判定を適用する
// 戻り値: この生行が占める表示行数（skipなら0）
int calcDisplayLines(const String& raw, bool aozoraMode) {
    if (aozoraMode && isSkipLine(raw)) return 0;
    String text = aozoraMode ? removeAozoraMarkup(raw) : raw;
    int dummyCount = 0;
    return splitDisplayLines(text, nullptr, 0, 0, dummyCount);
}

// ---- ページオフセットテーブル構築 ----
bool buildPageIndex(const String& filepath, bool aozoraMode) {
    File f = SD.open(filepath.c_str());
    if (!f) return false;
    g_pageOffsets[0] = 0;
    g_pageLineSkips[0] = 0;
    g_pageInBlock[0] = false;
    g_pageCount = 1;
    int linesOnPage = 0;
    bool inSkipBlock = false;
    char buf[READ_BUF_SIZE];
    while (f.available() && g_pageCount < PAGE_OFFSETS_MAX) {
        uint32_t lineStartPos = f.position();  // このソース行の開始バイト位置
        int len = 0;
        while (f.available() && len < READ_BUF_SIZE - 1) {
            char c = f.read();
            if (c == '\n') break;
            if (c == '\r') continue;
            buf[len++] = c;
        }
        buf[len] = '\0';
        String raw(buf);
        if (aozoraMode && raw.startsWith("-------")) { inSkipBlock = !inSkipBlock; continue; }
        if (aozoraMode && inSkipBlock) continue;
        int dl = calcDisplayLines(raw, aozoraMode);
        if (dl == 0) continue;

        for (int part = 0; part < dl && g_pageCount < PAGE_OFFSETS_MAX; part++) {
            if (linesOnPage >= maxLinesN()) {
                g_pageOffsets[g_pageCount] = lineStartPos;
                g_pageLineSkips[g_pageCount] = part;
                g_pageInBlock[g_pageCount] = inSkipBlock;
                g_pageCount++;
                linesOnPage = 0;
            }
            linesOnPage++;
        }
    }
    f.close();
    return (g_pageCount > 0);
}

// ---- 現在ページの表示行配列を生成 ----
int loadPageLines(const String& filepath, int page, bool aozoraMode,
                  String lines[], int maxLines) {
    if (page < 0 || page >= g_pageCount) return 0;
    File f = SD.open(filepath.c_str());
    if (!f) return 0;
    f.seek(g_pageOffsets[page]);
    int lineCount = 0;
    bool inSkipBlock = g_pageInBlock[page];  // ページ開始時のskipBlock状態を引き継ぐ
    char buf[READ_BUF_SIZE];
    while (f.available() && lineCount < maxLines) {
        uint32_t lineStart = f.position();
        int startSkip = (lineStart == g_pageOffsets[page]) ? g_pageLineSkips[page] : 0;
        int stopSkip = -1;
        if (page + 1 < g_pageCount) {
            if (lineStart > g_pageOffsets[page + 1]) break;
            if (lineStart == g_pageOffsets[page + 1]) stopSkip = g_pageLineSkips[page + 1];
        }

        int len = 0;
        while (f.available() && len < READ_BUF_SIZE - 1) {
            char c = f.read();
            if (c == '\n') break;
            if (c == '\r') continue;
            buf[len++] = c;
        }
        buf[len] = '\0';
        String raw(buf);
        if (aozoraMode && raw.startsWith("-------")) { inSkipBlock = !inSkipBlock; continue; }
        if (aozoraMode && inSkipBlock) continue;
        if (aozoraMode && isSkipLine(raw)) continue;
        String text = aozoraMode ? removeAozoraMarkup(raw) : raw;

        String wrapped[MAX_LINES_BUF];
        int wrappedCount = 0;
        int totalWrapped = splitDisplayLines(text, wrapped, MAX_LINES_BUF, 0, wrappedCount);
        int endSkip = (stopSkip >= 0) ? min(stopSkip, totalWrapped) : totalWrapped;
        for (int i = startSkip; i < endSkip && lineCount < maxLines && i < wrappedCount; i++) {
            lines[lineCount++] = wrapped[i];
        }
        if (stopSkip >= 0) break;
    }
    f.close();
    return lineCount;
}

// ============================================================
// 描画関数
// ============================================================

void drawHeader(const String& title, int page, int total, bool aozoraMode) {
    M5Cardputer.Display.fillRect(0, 0, 240, HEADER_HEIGHT, colHeader());
    M5Cardputer.Display.setFont(&fonts::Font0);

    // タイトル（左）
    M5Cardputer.Display.setTextColor(colPanelText(), colHeader());
    M5Cardputer.Display.setCursor(MARGIN_X, 5);
    String disp = title;
    if (disp.length() > 12) disp = disp.substring(0, 11) + "~";
    M5Cardputer.Display.print(disp);

    // モード表示
    String modeStr = aozoraMode ? "[AZ]" : "[TX]";
    M5Cardputer.Display.setTextColor(aozoraMode ? colAccent() : colHighlight(), colHeader());
    M5Cardputer.Display.setCursor(120, 5);
    M5Cardputer.Display.print(modeStr);

    // フォントサイズ表示
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(158, 5);
    M5Cardputer.Display.print(String(g_fontSize) + "px");

    // ページ番号（右）
    String pageStr = String(page + 1) + "/" + String(total);
    int pw = pageStr.length() * 6;
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(240 - pw - MARGIN_X, 5);
    M5Cardputer.Display.print(pageStr);
}

void drawFooter(const String& msg) {
    int fy = 135 - FOOTER_HEIGHT;
    M5Cardputer.Display.fillRect(0, fy, 240, FOOTER_HEIGHT, colHeader());
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(MARGIN_X, fy + 3);
    M5Cardputer.Display.print(msg);
    // バッテリー残量（右端）
    int bat = M5Cardputer.Power.getBatteryLevel();
    if (bat >= 0) {
        String batStr = String(bat) + "%";
        int bw = batStr.length() * 6;
        uint16_t batColor = (bat <= 20) ? colHighlight() : colPanelDim();
        M5Cardputer.Display.setTextColor(batColor, colHeader());
        M5Cardputer.Display.setCursor(240 - bw - MARGIN_X, fy + 4);
        M5Cardputer.Display.print(batStr);
    }
}

void drawFileSelect() {
    M5Cardputer.Display.fillScreen(colBg());
    M5Cardputer.Display.fillRect(0, 0, 240, HEADER_HEIGHT, colHeader());
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    M5Cardputer.Display.setTextColor(colPanelText(), colHeader());
    M5Cardputer.Display.setCursor(MARGIN_X, 4);
    M5Cardputer.Display.print("テキスト選択");

    if (g_fileCount == 0) {
        M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
        M5Cardputer.Display.setTextColor(TFT_RED);
        M5Cardputer.Display.setCursor(MARGIN_X, CONTENT_Y + 20);
        M5Cardputer.Display.print("/texts/にtxtなし");
        drawFooter("UTF-8 txtを/textsへ");
        return;
    }

    for (int i = 0; i < MAX_FILE_ROWS; i++) {
        int idx = g_fileScroll + i;
        if (idx >= g_fileCount) break;
        int yPos = CONTENT_Y + i * FILE_ROW_H;
        bool selected = (idx == g_fileCursor);

        String fname = g_fileList[idx];
        int sl = fname.lastIndexOf('/'); if (sl >= 0) fname = fname.substring(sl + 1);
        int dot = fname.lastIndexOf('.'); if (dot > 0) fname = fname.substring(0, dot);

        if (selected) M5Cardputer.Display.fillRect(2, yPos, 236, FILE_ROW_H - 1, colAccent());

        uint16_t fg = selected ? colSelectText() : colTextMain();
        uint16_t bg = selected ? colAccent() : colBg();

        if (g_jpFontLoaded) {
            g_ofr.setDrawer(M5Cardputer.Display);
            g_ofr.setFontSize(12);
            g_ofr.setFontColor(fg, bg);
            g_ofr.drawString(fitLine(fname, 220).c_str(), MARGIN_X + 2, yPos + 1);
        } else {
            M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
            M5Cardputer.Display.setTextColor(fg, bg);
            M5Cardputer.Display.setCursor(MARGIN_X + 2, yPos + 1);
            M5Cardputer.Display.print(fitLine(fname, 220));
        }
    }

    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextColor(colAccent());
    if (g_fileScroll > 0)
        M5Cardputer.Display.drawString("^", 228, CONTENT_Y);
    if (g_fileScroll + MAX_FILE_ROWS < g_fileCount)
        M5Cardputer.Display.drawString("v", 228, CONTENT_Y + MAX_FILE_ROWS * FILE_ROW_H - 8);

    drawFooter("Enter:開く  ;/.:移動");
}

void drawReadingPage() {
    M5Cardputer.Display.fillScreen(colBg());

    String fname = g_currentFile;
    int sl = fname.lastIndexOf('/'); if (sl >= 0) fname = fname.substring(sl + 1);
    int dot = fname.lastIndexOf('.'); if (dot > 0) fname = fname.substring(0, dot);

    drawHeader(fname, g_currentPage, g_pageCount, g_aozoraMode);

    String lines[MAX_LINES_BUF];
    int count = loadPageLines(g_currentFile, g_currentPage, g_aozoraMode, lines, maxLinesN());

    for (int i = 0; i < count; i++) {
        int yPos = CONTENT_Y + i * lineH();
        if (lines[i].length() == 0) continue;

        if (g_jpFontLoaded) {
            g_ofr.setDrawer(M5Cardputer.Display);
            g_ofr.setFontSize(g_fontSize);
            g_ofr.setFontColor(colTextMain(), colBg());
            g_ofr.drawString(lines[i].c_str(), MARGIN_X, yPos);
        } else {
            M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
            M5Cardputer.Display.setTextColor(colTextMain(), colBg());
            M5Cardputer.Display.setCursor(MARGIN_X, yPos);
            M5Cardputer.Display.print(lines[i]);
        }
    }

    drawFooter("Ｈ ヘルプ");
}

// ============================================================
// ポップアップ共通描画ヘルパー
// ============================================================
void drawPopupBg(int x, int y, int w, int h) {
    M5Cardputer.Display.fillRoundRect(x, y, w, h, 5, colHeader());
    M5Cardputer.Display.drawRoundRect(x, y, w, h, 5, colAccent());
}

// ============================================================
// ヘルプポップアップ（H キー）
// ============================================================
void showHelp() {
    const int px = 10, py = 8, pw = 220, ph = 118;
    drawPopupBg(px, py, pw, ph);
    M5Cardputer.Display.setFont(&fonts::Font0);

    // タイトル
    M5Cardputer.Display.setTextColor(colAccent(), colHeader());
    M5Cardputer.Display.setCursor(px + 6, py + 5);
    M5Cardputer.Display.print("AozoraTxtViewer v0.1");
    M5Cardputer.Display.drawFastHLine(px + 4, py + 16, pw - 8, colPanelDim());

    // 操作一覧
    const char* items[] = {
        "；／． 前／次のページ",
        "Ｊ     ページ移動",
        "Ｍ     青空／通常",
        "Ｆ     文字サイズ",
        "Ｃ     配色切替",
        "Ｈ     ヘルプ",
        "Ｅｓｃ ファイル一覧へ"
    };
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    M5Cardputer.Display.setTextColor(colPanelText(), colHeader());
    for (int i = 0; i < 7; i++) {
        M5Cardputer.Display.setCursor(px + 6, py + 20 + i * 12);
        M5Cardputer.Display.print(items[i]);
    }

    // フッターヒント
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(px + 40, py + 104);
    M5Cardputer.Display.print("[ 何かキーで閉じる ]");

    // キー入力待ち
    while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
        delay(10);
    }
}

// ============================================================
// ページジャンプダイアログ（J キー）
// 戻り値: 0-indexedページ番号、-1でキャンセル
// ============================================================
int showJumpDialog() {
    const int px = 18, py = 32, pw = 204, ph = 72;
    String input = "";

    drawPopupBg(px, py, pw, ph);
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    M5Cardputer.Display.setTextColor(colPanelText(), colHeader());
    M5Cardputer.Display.setCursor(px + 8, py + 7);
    M5Cardputer.Display.print("ページ移動 1-");
    M5Cardputer.Display.print(g_pageCount);
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(px + 8, py + 56);
    M5Cardputer.Display.print("Ent:OK DEL:戻 Esc:中止");

    while (true) {
        // 入力フィールド更新
        M5Cardputer.Display.fillRect(px + 8, py + 23, pw - 16, 28, colHeader());
        M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_16);
        M5Cardputer.Display.setTextColor(colAccent(), colHeader());
        M5Cardputer.Display.setCursor(px + 10, py + 28);
        String pageInput = (input.length() > 0 ? input : String("___"));
        M5Cardputer.Display.print(pageInput);
        M5Cardputer.Display.setCursor(px + 82, py + 28);
        M5Cardputer.Display.print("/ " + String(g_pageCount));

        // キー入力待ち
        while (true) {
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
            delay(10);
        }

        Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
        char key = st.word.empty() ? 0 : st.word[0];

        if (st.del) {
            if (input.length() > 0) input.remove(input.length() - 1);
            else return -1;
        } else if (key == 27) {
            return -1;
        } else if (st.enter) {
            if (input.length() == 0) return -1;
            int pg = input.toInt() - 1;
            return constrain(pg, 0, g_pageCount - 1);
        } else if (key >= '0' && key <= '9' && input.length() < 4) {
            input += key;
        }
    }
}

// ============================================================
// ファイルを開く
// ============================================================
void openFile(const String& filepath) {
    M5Cardputer.Display.fillScreen(colBg());
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextColor(colTextMain());
    M5Cardputer.Display.setCursor(MARGIN_X, 40);
    M5Cardputer.Display.print("Building index...");

    g_currentFile  = filepath;
    g_currentPage  = 0;
    g_pageCount    = 0;
    g_aozoraMode   = loadMode(filepath);   // ← ファイルごとのモードを復元

    if (!buildPageIndex(filepath, g_aozoraMode)) {
        M5Cardputer.Display.setTextColor(TFT_RED);
        M5Cardputer.Display.setCursor(MARGIN_X, 60);
        M5Cardputer.Display.print("Failed to open file.");
        delay(1500);
        return;
    }

    int saved = loadBookmark(filepath);
    if (saved > 0 && saved < g_pageCount) g_currentPage = saved;

    g_state = STATE_READING;
    drawReadingPage();
}

// ============================================================
// 設定保存/読み込み（グローバル: フォントサイズ・カラーモード）
// ============================================================
void saveSettings() {
    g_prefs.begin("aozora", false);
    g_prefs.putInt("fontSize",  g_fontSize);
    g_prefs.putBool("lightMode", g_lightMode);
    g_prefs.end();
}

void loadSettings() {
    g_prefs.begin("aozora", true);
    g_fontSize  = g_prefs.getInt("fontSize",  12);
    g_lightMode = g_prefs.getBool("lightMode", false);
    g_prefs.end();
}

// ============================================================
// TTFフォントロード（MP3プレイヤーと同じ方式）
// ============================================================
void loadJpFont() {
    File dir = SD.open("/fonts");
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f) {
        String name = f.name(); name.toLowerCase();
        if (name.endsWith(".ttf")) {
            String path = f.path();
            if (g_ofr.loadFont(path.c_str()) == 0) g_jpFontLoaded = true;
            break;
        }
        f = dir.openNextFile();
    }
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    loadSettings();  // NVSから色・フォントサイズを復元
    M5Cardputer.Display.fillScreen(colBg());
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextColor(colTextMain());
    M5Cardputer.Display.setCursor(MARGIN_X, 20);
    M5Cardputer.Display.print("Aozora Viewer");

    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        M5Cardputer.Display.setTextColor(TFT_RED);
        M5Cardputer.Display.setCursor(MARGIN_X, 40);
        M5Cardputer.Display.print("SD Card Error!");
        while (true) delay(1000);
    }

    M5Cardputer.Display.setCursor(MARGIN_X, 40);
    M5Cardputer.Display.setTextColor(colTextMain());
    M5Cardputer.Display.print("Loading font...");
    loadJpFont();

    M5Cardputer.Display.setCursor(MARGIN_X, 55);
    M5Cardputer.Display.print("Scanning files...");
    scanTextFiles();

    drawFileSelect();
}

void loop() {
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    char key = 0;
    if (!status.word.empty()) key = status.word[0];

    // ---- F/Cキーは全画面共通 ----
    if (key == 'f' || key == 'F') {
        const int sizes[] = {10, 12, 14, 16};
        int idx = 0;
        for (int i = 0; i < 4; i++) if (sizes[i] == g_fontSize) { idx = (i+1)%4; break; }
        g_fontSize = sizes[idx];
        saveSettings();
        if (g_state == STATE_READING && g_currentFile.length() > 0) {
            g_pageCount = 0; g_currentPage = 0;
            buildPageIndex(g_currentFile, g_aozoraMode);
            drawReadingPage();
        } else { drawFileSelect(); }
        return;
    }
    if (key == 'c' || key == 'C') {
        g_lightMode = !g_lightMode;
        saveSettings();
        if (g_state == STATE_READING) drawReadingPage(); else drawFileSelect();
        return;
    }
    // ---- H: ヘルプ表示（全画面共通） ----
    if (key == 'h' || key == 'H') {
        showHelp();
        if (g_state == STATE_READING) drawReadingPage(); else drawFileSelect();
        return;
    }
    // ---- J: ページジャンプ（読書中のみ） ----
    if ((key == 'j' || key == 'J') && g_state == STATE_READING) {
        int pg = showJumpDialog();
        if (pg >= 0) {
            g_currentPage = pg;
            saveState(g_currentFile, g_currentPage, g_aozoraMode);
        }
        drawReadingPage();
        return;
    }

    // ---- ファイル選択画面 ----
    if (g_state == STATE_FILE_SELECT) {
        if (key == ';') {
            if (g_fileCursor > 0) {
                g_fileCursor--;
                if (g_fileCursor < g_fileScroll) g_fileScroll = g_fileCursor;
                drawFileSelect();
            }
        } else if (key == '.') {
            if (g_fileCursor < g_fileCount - 1) {
                g_fileCursor++;
                if (g_fileCursor >= g_fileScroll + MAX_FILE_ROWS)
                    g_fileScroll = g_fileCursor - MAX_FILE_ROWS + 1;
                drawFileSelect();
            }
        } else if (status.enter) {
            if (g_fileCount > 0) openFile(g_fileList[g_fileCursor]);
        }

    // ---- 読書画面 ----
    } else if (g_state == STATE_READING) {
        if (key == '.') {
            if (g_currentPage < g_pageCount - 1) {
                g_currentPage++;
                saveState(g_currentFile, g_currentPage, g_aozoraMode);
                drawReadingPage();
            }
        } else if (key == ';') {
            if (g_currentPage > 0) {
                g_currentPage--;
                saveState(g_currentFile, g_currentPage, g_aozoraMode);
                drawReadingPage();
            }
        } else if (key == 'm' || key == 'M') {
            // モード切り替え → インデックス再構築
            g_aozoraMode = !g_aozoraMode;
            M5Cardputer.Display.fillScreen(colBg());
            M5Cardputer.Display.setFont(&fonts::Font0);
            M5Cardputer.Display.setTextColor(colTextMain());
            M5Cardputer.Display.setCursor(MARGIN_X, 40);
            M5Cardputer.Display.print(g_aozoraMode ? "Mode: Aozora" : "Mode: Plain");
            M5Cardputer.Display.setCursor(MARGIN_X, 55);
            M5Cardputer.Display.print("Rebuilding index...");
            g_pageCount   = 0;
            g_currentPage = 0;
            buildPageIndex(g_currentFile, g_aozoraMode);
            saveState(g_currentFile, g_currentPage, g_aozoraMode);
            drawReadingPage();
        } else if (key == 27) {
            saveState(g_currentFile, g_currentPage, g_aozoraMode);
            g_state = STATE_FILE_SELECT;
            drawFileSelect();
        }
    }
}
