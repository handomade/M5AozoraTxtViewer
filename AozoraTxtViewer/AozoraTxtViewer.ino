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

// ★ 関数の前方宣言（前方位置で定義される関数を先に宣言）
void drawProgressBar(int progress);
void saveSettings();
void drawFileListOnly();

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
#define MAX_LINE_PX      (240 - MARGIN_X * 2 - 5)  // 225px（charW()で正確な幅を計算）
#define MAX_LINES_BUF    12  // 表示可能な最大行数より余裕を持たせる

// フッター表示制御（動的CONTENT_H計算用）
bool g_showFooter = true;
inline int getContentHeight() { return (135 - HEADER_HEIGHT - (g_showFooter ? FOOTER_HEIGHT : 0) - 4); }

// フォントサイズ（切り替え可能: 10/12/14/16 px）
int g_fontSize = 12;  // ★ デフォルトは内蔵フォント用に12px

// ★ charW()関数はグローバル変数の後に定義される（下部参照）
inline int lineH()         { return (g_fontSize >= 16) ? 18 : (g_fontSize + 3); }
inline int maxLinesN()     { return getContentHeight() / lineH(); }

// ============================================================
// 青空文庫 テキスト処理
// ============================================================

// 1行からルビ・注記を除去して返す
String removeAozoraMarkup(const String& line) {
    String out;
    int len = line.length();
    out.reserve(len);  // ★ 事前に同じサイズを予約
    const char* p = line.c_str();
    int pos = 0;
    
    while (pos < len && *p) {
        // 「《」(E3 80 8A) → 「》」(E3 80 8B) を読み飛ばす
        if (pos + 3 <= len && (uint8_t)p[0]==0xE3 && (uint8_t)p[1]==0x80 && (uint8_t)p[2]==0x8A) {
            p += 3; pos += 3;
            while (pos <= len && *p) {
                if (pos + 3 <= len && (uint8_t)p[0]==0xE3 && (uint8_t)p[1]==0x80 && (uint8_t)p[2]==0x8B) { 
                    p += 3; pos += 3; break; 
                }
                uint8_t c = (uint8_t)*p;
                int bytes = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
                if (pos + bytes > len) break;
                p += bytes; pos += bytes;
            }
            continue;
        }
        // 「｜」(EF BD 9C) を読み飛ばす
        if (pos + 3 <= len && (uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBD && (uint8_t)p[2]==0x9C) { 
            p += 3; pos += 3; continue; 
        }
        // 「［」(EF BC BB) → 「］」(EF BC BD) を読み飛ばす
        if (pos + 3 <= len && (uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBC && (uint8_t)p[2]==0xBB) {
            p += 3; pos += 3;
            while (pos <= len && *p) {
                if (pos + 3 <= len && (uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBC && (uint8_t)p[2]==0xBD) { 
                    p += 3; pos += 3; break; 
                }
                uint8_t c = (uint8_t)*p;
                int bytes = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
                if (pos + bytes > len) break;
                p += bytes; pos += bytes;
            }
            continue;
        }
        uint8_t c = (uint8_t)*p;
        int bytes = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (pos + bytes > len) break;
        // ★ マルチバイト文字を一度に追加（1バイトずつではなく）
        out.concat(p, bytes);
        p += bytes; pos += bytes;
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
    int len = s.length();
    const char* p = s.c_str();
    int pos = 0;  // ★ 文字列内の現在位置を追跡
    
    while (pos < len && *p) {
        uint8_t c = (uint8_t)*p;
        int step = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        
        // ★ バッファ境界チェック
        if (pos + step > len) break;
        
        int w    = charW(step);
        if (used + w > maxPx) break;
        for (int i = 0; i < step; i++) r += (char)*(p + i);
        p += step; pos += step; used += w;
    }
    return r;
}

// ============================================================
// OpenFontRender
// ============================================================
OpenFontRender g_ofr;
bool g_jpFontLoaded = false;

enum FontLoadSource {
    FONT_LOAD_NONE,
    FONT_LOAD_SD,
    FONT_LOAD_ERROR
};

FontLoadSource g_fontLoadSource = FONT_LOAD_NONE;
String g_fontFileName = "";
size_t g_fontFileSize = 0;
size_t g_fontBytesRead = 0;
FT_Error g_fontLoadError = 0;
bool g_fontReadComplete = false;
String g_fontDiagNote = "not loaded";

// ============================================================
// 読書エンジン
// ============================================================
#define MAX_DISPLAY_LINES 12

struct DisplayLineCache {
    String text[MAX_DISPLAY_LINES];
    int count;
};
DisplayLineCache g_displayCache = {{}, 0};

// ============================================================
// 高速描画用スプライト（未使用・削除予定）
// ============================================================
// スプライト未使用のため削除

// ============================================================
// テーマカラー（ライト/ダークモード切替対応）
// ============================================================
bool g_lightMode = true;
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
int    g_fileCursorPrev = 0;  // ★ 前回のカーソル位置（差分描画用）
int    g_fileScroll = 0;

// ============================================================
// バックライト自動制御（Lキー切替）+ 手動調整（-/=キー）
// ============================================================
#define AUTO_BRIGHTNESS_DIM_MS    (2 * 60 * 1000)    // 2分
#define AUTO_BRIGHTNESS_OFF_MS    (5 * 60 * 1000)    // 5分

// 明るさ段階管理（5段階: 0%, 25%, 50%, 75%, 100%）
#define BRIGHTNESS_LEVELS 5
const uint8_t g_brightnessValues[BRIGHTNESS_LEVELS] = {0, 64, 128, 192, 255};

bool g_autoBrightnessEnabled = true;
unsigned long g_lastKeyTime = 0;
int g_autoBrightnessState = 0;  // 0=正常, 1=暗い, 2=消灯
int g_brightnessLevel = 4;      // 手動設定の明るさレベル（0-4、デフォルト最大値）

// TTFフォント使用切り替え
bool g_useTTFFont = true;       // TTFフォント使用フラグ（デフォルト true）

// ★ 内蔵フォント（lgfxJapanGothic_12）の実際の文字幅を計算
// TTFモード時はg_fontSizeに基づく、内蔵フォント時は固定値を使用
// （g_useTTFFont のスコープ確保のため、ここで定義）
int charW(int s) {
    if (g_useTTFFont) {
        return (s == 1) ? (g_fontSize / 2) : g_fontSize;  // TTFモード
    } else {
        // 内蔵フォント（lgfxJapanGothic_12）: ASCII約5px、日本語約11px
        // 実測より若干保守的な値を使用して、右端の文字欠落を防止
        return (s == 1) ? 5 : 11;
    }
}

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
#define PAGE_CACHE_SIZE  3

String   g_currentFile = "";
uint32_t g_pageOffsets[PAGE_OFFSETS_MAX];
uint8_t  g_pageLineSkips[PAGE_OFFSETS_MAX]; // ページ開始時に読み飛ばす折り返し行数
bool     g_pageInBlock[PAGE_OFFSETS_MAX];  // ページ開始時のskipBlock状態
int      g_pageCount   = 0;
int      g_currentPage = 0;
bool     g_aozoraMode  = true;   // true=青空モード, false=通常モード

struct PageCache {
    int page;
    int count;
    String lines[MAX_LINES_BUF];
};
PageCache g_pageCache[PAGE_CACHE_SIZE];
int g_cacheNextSlot = 0;

Preferences g_prefs;

// ============================================================
// Forward Declarations（関数の先行宣言）
// ============================================================

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

    int textLen = text.length();
    const char* p = text.c_str();
    String curLine;
    curLine.reserve(256);  // ★ 1行の最大バイト数を確保（256バイト = 約50文字）
    int usedPx = 0;
    int displayLine = 0;
    int pos = 0;

    while (pos < textLen && *p) {
        uint8_t c = (uint8_t)*p;
        int step = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        
        if (pos + step > textLen) break;
        
        int w = charW(step);
        if (usedPx + w > MAX_LINE_PX) {
            // 行が満杯になった → 現在の行を保存して折り返す
            if (displayLine >= skipLines && lines && lineCount < maxLines) {
                lines[lineCount++] = curLine;
            }
            displayLine++;
            curLine = "";
            curLine.reserve(256);  // ★ 新しい行でもバッファを確保
            usedPx = 0;
            
            // maxLinesに到達した場合は残りの行をカウントのみ
            if (lines && lineCount >= maxLines) {
                while (pos < textLen && *p) {
                    uint8_t c2 = (uint8_t)*p;
                    int step2 = (c2 < 0x80) ? 1 : (c2 < 0xE0) ? 2 : (c2 < 0xF0) ? 3 : 4;
                    if (pos + step2 > textLen) break;
                    int w2 = charW(step2);
                    if (usedPx + w2 > MAX_LINE_PX) { displayLine++; usedPx = 0; }
                    usedPx += w2;
                    p += step2; pos += step2;
                }
                return displayLine + 1;
            }
        }
        
        // ★ マルチバイト文字を一度に追加（1バイトずつではなく）
        curLine.concat(p, step);
        usedPx += w;
        p += step; pos += step;
    }

    // 最後の行を保存
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
    
    size_t fileSize = f.size();
    uint32_t lastProgressUpdate = 0;
    
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
        // ★ UTF-8の4バイト文字が255バイト目で切断されるのを防ぐ
        while (len > 0) {
            uint8_t c = (uint8_t)buf[len - 1];
            if (c < 0x80 || c >= 0xC0) break;  // 先頭バイトまで巻き戻し
            len--;
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
        
        // プログレスバー更新（100ms毎）
        uint32_t now = millis();
        if (now - lastProgressUpdate > 100 && fileSize > 0) {
            int progress = (lineStartPos * 100) / fileSize;
            drawProgressBar(progress);
            lastProgressUpdate = now;
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
        // ★ UTF-8の4バイト文字が255バイト目で切断されるのを防ぐ
        while (len > 0) {
            uint8_t c = (uint8_t)buf[len - 1];
            if (c < 0x80 || c >= 0xC0) break;  // 先頭バイトまで巻き戻し
            len--;
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
void clearPageCache() {
    for (int i = 0; i < PAGE_CACHE_SIZE; i++) {
        g_pageCache[i].page = -1;
        g_pageCache[i].count = 0;
        for (int j = 0; j < MAX_LINES_BUF; j++) g_pageCache[i].lines[j] = "";
    }
    g_cacheNextSlot = 0;
}

int findPageCache(int page) {
    for (int i = 0; i < PAGE_CACHE_SIZE; i++) {
        if (g_pageCache[i].page == page) return i;
    }
    return -1;
}

int cachePageLines(int page) {
    if (page < 0 || page >= g_pageCount) return -1;
    int slot = findPageCache(page);
    if (slot >= 0) return slot;

    slot = g_cacheNextSlot;
    g_cacheNextSlot = (g_cacheNextSlot + 1) % PAGE_CACHE_SIZE;
    g_pageCache[slot].page = page;
    g_pageCache[slot].count = loadPageLines(g_currentFile, page, g_aozoraMode,
                                            g_pageCache[slot].lines, maxLinesN());
    return slot;
}

void prefetchNextPage(int page) {
    cachePageLines(page + 1);
}

// ============================================================
// バックライト自動制御ヘルパー
// ============================================================

void applyBrightness(uint8_t value) {
    M5Cardputer.Display.setBrightness(value);
}

void restoreBrightness() {
    if (!g_autoBrightnessEnabled) return;
    g_lastKeyTime = millis();
    g_autoBrightnessState = 0;
    applyBrightness(g_brightnessValues[g_brightnessLevel]);
}

void updateAutoBrightness() {
    if (!g_autoBrightnessEnabled) return;
    
    unsigned long elapsed = millis() - g_lastKeyTime;
    
    if (elapsed >= AUTO_BRIGHTNESS_OFF_MS) {
        // 5分経過 → 消灯
        if (g_autoBrightnessState != 2) {
            g_autoBrightnessState = 2;
            applyBrightness(0);
        }
    } else if (elapsed >= AUTO_BRIGHTNESS_DIM_MS) {
        // 2分経過 → 暗くする（手動調整値の50%程度）
        if (g_autoBrightnessState != 1) {
            g_autoBrightnessState = 1;
            uint8_t dimValue = g_brightnessValues[g_brightnessLevel] / 2;
            applyBrightness(dimValue);
        }
    } else {
        // 通常状態
        if (g_autoBrightnessState != 0) {
            g_autoBrightnessState = 0;
            applyBrightness(g_brightnessValues[g_brightnessLevel]);
        }
    }
}

void adjustBrightness(int delta) {
    g_brightnessLevel = constrain(g_brightnessLevel + delta, 0, BRIGHTNESS_LEVELS - 1);
    applyBrightness(g_brightnessValues[g_brightnessLevel]);
    saveSettings();
    // タイマーをリセット（ON状態の場合）
    if (g_autoBrightnessEnabled) {
        g_lastKeyTime = millis();
        g_autoBrightnessState = 0;
    }
}

// ============================================================
// ヘッダー明るさ表示更新（バックライト変更時のみ再描画）
// ============================================================
void updateHeaderBrightness() {
    // ヘッダーの中央〜右側部分を再描画（節電モード、明るさレベル、フォントサイズ、ページ番号）
    M5Cardputer.Display.fillRect(138, 0, 240 - 138, HEADER_HEIGHT, colHeader());
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    
    // 節電モード表示
    M5Cardputer.Display.setTextColor(g_autoBrightnessEnabled ? colAccent() : colTextDim(), colHeader());
    M5Cardputer.Display.setCursor(138, 5);
    M5Cardputer.Display.print(g_autoBrightnessEnabled ? "*" : "O");
    
    // 明るさレベル
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(146, 5);
    M5Cardputer.Display.print(String(g_brightnessLevel));
    
    // フォントサイズ（TTF使用時のみ表示、内蔵フォント時は"--"表示）
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(158, 5);
    if (g_jpFontLoaded && g_useTTFFont) {
        M5Cardputer.Display.print(String(g_fontSize) + "px");
    } else {
        M5Cardputer.Display.print("--");
    }
    
    // ページ番号（右）読書モード時のみ表示
    if (g_state == STATE_READING && g_pageCount > 0) {
        String pageStr = String(g_currentPage + 1) + "/" + String(g_pageCount);
        int pw = pageStr.length() * 6;
        M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
        M5Cardputer.Display.setCursor(240 - pw - MARGIN_X, 5);
        M5Cardputer.Display.print(pageStr);
    }
}

// ============================================================
// プログレスバー描画
// ============================================================
void drawProgressBar(int progress) {
    // progress: 0-100（パーセンテージ）
    const int barX = 40, barY = 60, barW = 160, barH = 10;
    const int totalH = 20;
    
    // 背景（黒）
    M5Cardputer.Display.fillRect(barX - 5, barY - 5, barW + 10, barH + 10, colBg());
    
    // 枠（水色）
    M5Cardputer.Display.drawRect(barX, barY, barW, barH, colAccent());
    
    // プログレス（水色で埋める）
    if (progress > 0) {
        int fillW = (barW * progress) / 100;
        M5Cardputer.Display.fillRect(barX + 1, barY + 1, fillW - 2, barH - 2, colAccent());
    }
}

// ============================================================
// ESC/キャンセルキー判定
// ============================================================
bool isEscKey(const Keyboard_Class::KeysState& st, char key) {
    (void)st;
    return key == 27 || key == '`' || key == '~';
}

int findPageByOffset(uint32_t targetOffset) {
    int bestPage = 0;
    int exactPage = -1;
    for (int i = 0; i < g_pageCount; i++) {
        if (g_pageOffsets[i] > targetOffset) break;
        if (g_pageOffsets[i] == targetOffset && exactPage < 0) exactPage = i;
        bestPage = i;
    }
    return (exactPage >= 0) ? exactPage : bestPage;
}

// ============================================================
// 描画関数
// ============================================================

void drawHeader(const String& title, int page, int total, bool aozoraMode) {
    M5Cardputer.Display.fillRect(0, 0, 240, HEADER_HEIGHT, colHeader());
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);

    // タイトル（左、内蔵フォント固定）
    String disp = fitLine(title, 102);
    if (disp.length() < title.length()) disp += "~";
    M5Cardputer.Display.setTextColor(colPanelText(), colHeader());
    M5Cardputer.Display.setCursor(MARGIN_X, 4);
    M5Cardputer.Display.print(disp);

    // モード表示
    String modeStr = aozoraMode ? "[AZ]" : "[TX]";
    M5Cardputer.Display.setTextColor(aozoraMode ? colAccent() : colHighlight(), colHeader());
    M5Cardputer.Display.setCursor(112, 5);
    M5Cardputer.Display.print(modeStr);

    // 節電モード表示（＊＝ON, ○＝OFF）+ 明るさレベル
    M5Cardputer.Display.setTextColor(g_autoBrightnessEnabled ? colAccent() : colTextDim(), colHeader());
    M5Cardputer.Display.setCursor(138, 5);
    M5Cardputer.Display.print(g_autoBrightnessEnabled ? "*" : "O");
    
    // 明るさレベル（0-4を表示）
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(146, 5);
    M5Cardputer.Display.print(String(g_brightnessLevel));
    
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(158, 5);
    if (g_jpFontLoaded && g_useTTFFont) {
        M5Cardputer.Display.print(String(g_fontSize) + "px");
    } else {
        M5Cardputer.Display.print("--");
    }

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

// ★ ファイルリストの単一行を描画（差分描画用）
void drawFileRow(int idx, bool selected) {
    if (idx < 0 || idx >= g_fileCount) return;
    
    int i = idx - g_fileScroll;
    if (i < 0 || i >= MAX_FILE_ROWS) return;
    
    int yPos = CONTENT_Y + i * FILE_ROW_H;
    String fname = g_fileList[idx];
    int sl = fname.lastIndexOf('/'); if (sl >= 0) fname = fname.substring(sl + 1);
    int dot = fname.lastIndexOf('.'); if (dot > 0) fname = fname.substring(0, dot);

    uint16_t fg = selected ? colBg()    : colTextMain();
    uint16_t bg = selected ? colAccent() : colBg();
    
    // 背景を塗る
    M5Cardputer.Display.fillRect(2, yPos, 236, FILE_ROW_H, bg);
    
    // カーソル記号
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextColor(fg, bg);
    M5Cardputer.Display.setCursor(2, yPos + 3);
    M5Cardputer.Display.print(selected ? ">" : " ");

    // ファイル名描画
    if (g_jpFontLoaded && g_useTTFFont) {
        g_ofr.setFontColor(fg, bg);
        String _s = fitLine(fname, 210);
        g_ofr.drawString(_s.c_str(), MARGIN_X + 12, yPos + 1);
    } else {
        M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
        M5Cardputer.Display.setTextColor(fg, bg);
        M5Cardputer.Display.setCursor(MARGIN_X + 12, yPos + 1);
        M5Cardputer.Display.print(fitLine(fname, 210));
    }
}

// ★ カーソル移動ラッパー（差分描画対応）
void moveCursor(int newCursor) {
    if (newCursor < 0 || newCursor >= g_fileCount) return;
    if (newCursor == g_fileCursor) return;  // 変化なし
    
    int oldCursor = g_fileCursor;
    g_fileCursor = newCursor;
    
    // スクロール位置計算（新・旧）
    int oldScroll = g_fileScroll;
    int newScroll = max(0, min(g_fileCursor - (MAX_FILE_ROWS / 2), g_fileCount - MAX_FILE_ROWS));
    g_fileScroll = newScroll;
    
    // スクロール変化あれば全再描画
    if (newScroll != oldScroll) {
        drawFileListOnly();
        return;
    }
    
    // スクロール変化なし → 2行だけ更新（前行と新行）
    drawFileRow(oldCursor, false);
    drawFileRow(newCursor, true);
}

void drawFileSelect() {
    g_fontSize = 12;  // ★ ファイル選択モードでは常に12px
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
    
    drawFileListOnly();  // ★ リスト部分だけ描画（内蔵フォント固定）
    drawFooter(";/. ↑↓     Enter 開く");
}

// ★★ リスト部分だけ描画（カーソル移動時に呼び出し）★★
void drawFileListOnly() {
    if (g_fileCount == 0) return;
    
    // ★ 背景を一度だけクリア（MP3プレイヤー方式）
    int listHeight = MAX_FILE_ROWS * FILE_ROW_H;
    M5Cardputer.Display.fillRect(0, CONTENT_Y, 240, listHeight, colBg());
    
    // リスト描画
    int yPos = CONTENT_Y;
    for (int i = 0; i < MAX_FILE_ROWS; i++) {
        int idx = g_fileScroll + i;
        
        if (idx < 0 || idx >= g_fileCount) {
            yPos += FILE_ROW_H;
            continue;
        }

        bool selected = (idx == g_fileCursor);
        String fname = g_fileList[idx];
        int sl = fname.lastIndexOf('/'); if (sl >= 0) fname = fname.substring(sl + 1);
        int dot = fname.lastIndexOf('.'); if (dot > 0) fname = fname.substring(0, dot);

        uint16_t fg = selected ? colAccent() : colTextMain();
        uint16_t bg = selected ? colAccent() : colBg();
        
        // ★ 選択行だけ背景色変更（MP3方式）
        if (selected) {
            M5Cardputer.Display.fillRect(2, yPos, 236, FILE_ROW_H, colAccent());
        }
        
        // カーソル記号
        M5Cardputer.Display.setFont(&fonts::Font0);
        M5Cardputer.Display.setTextColor(selected ? colBg() : fg, bg);
        M5Cardputer.Display.setCursor(2, yPos + 3);
        M5Cardputer.Display.print(selected ? ">" : " ");

        // ファイル名描画
        if (g_jpFontLoaded && g_useTTFFont) {
            // ★ setDrawer/setFontSize は drawFileSelect() で一度だけ呼び済み
            g_ofr.setFontColor(selected ? colBg() : fg, bg);
            String _s = fitLine(fname, 210);
            g_ofr.drawString(_s.c_str(), MARGIN_X + 12, yPos + 1);
        } else {
            M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
            M5Cardputer.Display.setTextColor(selected ? colBg() : fg, bg);
            M5Cardputer.Display.setCursor(MARGIN_X + 12, yPos + 1);
            M5Cardputer.Display.print(fitLine(fname, 210));
        }

        yPos += FILE_ROW_H;
    }

    // スクロール指示矢印
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextColor(colAccent(), colBg());
    if (g_fileScroll > 0)
        M5Cardputer.Display.drawString("^", 228, CONTENT_Y);
    if (g_fileScroll + MAX_FILE_ROWS < g_fileCount)
        M5Cardputer.Display.drawString("v", 228, CONTENT_Y + MAX_FILE_ROWS * FILE_ROW_H - 8);
}

// ============================================================
// ヘッダー再描画（色初期化用 - setup直後など）
// ============================================================
void redrawHeader() {
    M5Cardputer.Display.fillRect(0, 0, 240, HEADER_HEIGHT, colHeader());
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    M5Cardputer.Display.setTextColor(colPanelText(), colHeader());
    M5Cardputer.Display.setCursor(MARGIN_X, 4);
    M5Cardputer.Display.print("テキスト選択");
    
    // 節電モード表示
    M5Cardputer.Display.setTextColor(g_autoBrightnessEnabled ? colAccent() : colTextDim(), colHeader());
    M5Cardputer.Display.setCursor(138, 5);
    M5Cardputer.Display.print(g_autoBrightnessEnabled ? "*" : "O");
    
    // 明るさレベル
    M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    M5Cardputer.Display.setCursor(146, 5);
    M5Cardputer.Display.print(String(g_brightnessLevel));
}

void drawReadingPage() {
    int contentHeight = g_showFooter ? (135 - HEADER_HEIGHT - FOOTER_HEIGHT) : (135 - HEADER_HEIGHT);
    M5Cardputer.Display.fillRect(0, HEADER_HEIGHT, 240, contentHeight, colBg());

    String fname = g_currentFile;
    int sl = fname.lastIndexOf('/'); if (sl >= 0) fname = fname.substring(sl + 1);
    int dot = fname.lastIndexOf('.'); if (dot > 0) fname = fname.substring(0, dot);

    drawHeader(fname, g_currentPage, g_pageCount, g_aozoraMode);

    int cacheSlot = cachePageLines(g_currentPage);
    if (cacheSlot < 0) {
        drawFooter("読込失敗");
        return;
    }

    // ===== 描画開始 =====
    // TTFフォント使用時：setDrawer を最小化（高速化）
    if (g_jpFontLoaded && g_useTTFFont) {
        g_ofr.setDrawer(M5Cardputer.Display);      // ★ 1回だけ
        g_ofr.setFontSize(g_fontSize);             // ★ 1回だけ
        g_ofr.setFontColor(colTextMain(), colBg()); // ★ 1回だけ
    }
    
    for (int i = 0; i < g_pageCache[cacheSlot].count; i++) {
        int yPos = CONTENT_Y + i * lineH();
        if (g_pageCache[cacheSlot].lines[i].length() == 0) continue;

        if (g_jpFontLoaded && g_useTTFFont) {
            g_ofr.drawString(g_pageCache[cacheSlot].lines[i].c_str(), MARGIN_X, yPos);
        } else {
            // 内蔵フォント固定12px
            M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
            M5Cardputer.Display.setTextColor(colTextMain(), colBg());
            M5Cardputer.Display.setCursor(MARGIN_X, yPos);
            M5Cardputer.Display.print(g_pageCache[cacheSlot].lines[i]);
        }
    }

    if (g_showFooter) drawFooter("Ｈ ヘルプ");
    prefetchNextPage(g_currentPage);
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
bool showHelp() {
    const int px = 10, py = 6, pw = 220, ph = 140;
    drawPopupBg(px, py, pw, ph);
    //M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
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
        "Ｖ     フッター切替",
        "Ｌ     節電モード",
        "-/=   明るさ調整",
        "Ｑ     フォント切替",
        "Ｈ     ヘルプ",
        "Ｅｓｃ 一覧へ戻る"
    };
    M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    M5Cardputer.Display.setTextColor(colPanelText(), colHeader());
    for (int i = 0; i < 11; i++) {
        M5Cardputer.Display.setCursor(px + 6, py + 20 + i * 12);
        M5Cardputer.Display.print(items[i]);
    }

    // // フッターヒント
    // M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
    // M5Cardputer.Display.setTextColor(colPanelDim(), colHeader());
    // M5Cardputer.Display.setCursor(px + 40, py + 104);
    // M5Cardputer.Display.print("[ 何かキーで閉じる ]");

    // キー入力待ち
    while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
            char key = st.word.empty() ? 0 : st.word[0];
            return isEscKey(st, key);
        }
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
        String pageInput = (input.length() > 0 ? input : String("___"));
        String pageTotal = "/" + String(g_pageCount);
        int inputW = M5Cardputer.Display.textWidth("8888");
        int totalW = M5Cardputer.Display.textWidth(pageTotal.c_str());
        int groupX = px + (pw - inputW - totalW) / 2;
        int inputX = groupX + inputW - M5Cardputer.Display.textWidth(pageInput.c_str());
        M5Cardputer.Display.setCursor(inputX, py + 28);
        M5Cardputer.Display.print(pageInput);
        M5Cardputer.Display.setCursor(groupX + inputW, py + 28);
        M5Cardputer.Display.print(pageTotal);

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
        } else if (isEscKey(st, key)) {
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
    drawProgressBar(0);  // プログレスバー開始

    clearPageCache();
    g_currentFile  = filepath;
    g_currentPage  = 0;
    g_pageCount    = 0;
    g_aozoraMode   = loadMode(filepath);   // ← ファイルごとのモードを復元

    // ★ ファイル開き時にフォントサイズを正しく設定
    if (!g_useTTFFont) {
        g_fontSize = 12;  // 内蔵フォントは常に12px
    }
    // TTFモードの場合は、前回保存されたg_fontSizeを使用（loadSettings()で既に読み込み済み）

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
// 設定保存/読み込み（グローバル: フォントサイズ・カラーモード・節電モード）
// ============================================================
void saveSettings() {
    g_prefs.begin("aozora", false);
    g_prefs.putInt("fontSize",   g_fontSize);
    g_prefs.putBool("lightMode",  g_lightMode);
    g_prefs.putBool("showFooter", g_showFooter);
    g_prefs.putBool("autoBright", g_autoBrightnessEnabled);
    g_prefs.putInt("brightLevel", g_brightnessLevel);
    g_prefs.putBool("useTTF",     g_useTTFFont);
    g_prefs.end();
}

void loadSettings() {
    g_prefs.begin("aozora", true);
    g_fontSize  = g_prefs.getInt("fontSize",   12);  // ★ デフォルト12px（ファイル選択時用）
    g_lightMode = g_prefs.getBool("lightMode",  true);
    g_showFooter = g_prefs.getBool("showFooter", true);
    g_autoBrightnessEnabled = g_prefs.getBool("autoBright", true);
    g_brightnessLevel = constrain(g_prefs.getInt("brightLevel", 4), 0, BRIGHTNESS_LEVELS - 1);
    g_useTTFFont = g_prefs.getBool("useTTF", true);
    g_prefs.end();
}

// ★ drawLoadProgress() は未使用（g_ofr.loadFont() が同期処理のため進捗取得不可）
// デッドコードのため削除

const char* fontLoadSourceText() {
    switch (g_fontLoadSource) {
        case FONT_LOAD_SD:    return "SD fallback";
        case FONT_LOAD_ERROR: return "ERROR";
        default:              return "NONE";
    }
}

// ============================================================
// TTFフォント読み込み
// ============================================================
void loadJpFont() {
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextColor(colTextMain(), colBg());

    File dir = SD.open("/fonts");
    if (!dir || !dir.isDirectory()) {
        g_fontLoadSource = FONT_LOAD_ERROR;
        g_fontDiagNote = "/fonts not found";
        M5Cardputer.Display.setCursor(MARGIN_X, 40);
        M5Cardputer.Display.print("Font: /fonts not found");
        return;
    }
    File f = dir.openNextFile();
    while (f) {
        String name = f.name(); name.toLowerCase();
        if (name.endsWith(".ttf")) {
            size_t fileSize = f.size();
            String path = f.path();
            String fname = String(f.name());
            g_fontFileName = fname;
            g_fontFileSize = fileSize;
            g_fontBytesRead = 0;
            g_fontLoadError = 0;
            g_fontReadComplete = false;
            g_fontDiagNote = "starting";

            // フォントファイル名とサイズを表示
            M5Cardputer.Display.fillRect(0, 40, 240, 10, colBg());
            M5Cardputer.Display.setCursor(MARGIN_X, 40);
            char info[48];
            snprintf(info, sizeof(info), "Font: %s (%uKB)",
                     fname.c_str(), (unsigned)(fileSize / 1024));
            M5Cardputer.Display.print(info);

            // SDから直接読み込み
            M5Cardputer.Display.fillRect(0, 68, 240, 10, colBg());
            M5Cardputer.Display.setCursor(MARGIN_X, 68);
            FT_Error err = g_ofr.loadFont(path.c_str());
            g_fontLoadError = err;
            if (err == 0) {
                g_jpFontLoaded = true;
                g_fontLoadSource = FONT_LOAD_SD;
                g_fontDiagNote = "loaded from SD";
                M5Cardputer.Display.print("Font: loaded OK");
            } else {
                g_fontLoadSource = FONT_LOAD_ERROR;
                g_fontDiagNote = "loadFont failed";
                snprintf(info, sizeof(info), "Font: err %d", (int)err);
                M5Cardputer.Display.print(info);
            }
            f.close();
            dir.close();
            return;
        }
        f = dir.openNextFile();
    }
    g_fontLoadSource = FONT_LOAD_ERROR;
    g_fontDiagNote = "no .ttf in /fonts";
    M5Cardputer.Display.setCursor(MARGIN_X, 40);
    M5Cardputer.Display.print("Font: no .ttf in /fonts");
}

// ============================================================
// setup / loop
// ============================================================

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    loadSettings();  // NVSから色・フォントサイズを復元
    applyBrightness(g_brightnessValues[g_brightnessLevel]);  // ★ バックライト初期化
    clearPageCache();
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

    M5Cardputer.Display.setCursor(MARGIN_X, 60);
    M5Cardputer.Display.print("Scanning files...");
    scanTextFiles();

    drawFileSelect();
    redrawHeader();  // ★ 色を確実に初期化
}

void loop() {
    M5Cardputer.update();
    
    // ★ バックライト自動制御（毎フレーム実行）
    updateAutoBrightness();
    
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;

    // キー入力時にタイマーをリセット
    restoreBrightness();

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    char key = 0;
    if (!status.word.empty()) key = status.word[0];

    // ---- F/C/Lキーは全画面共通 ----
    // ★ F: フォントサイズ変更（TTF使用時のみ。内蔵フォントは12px固定） ----
    if ((key == 'f' || key == 'F') && (g_jpFontLoaded && g_useTTFFont)) {
        const int sizes[] = {10, 12, 14, 16};
        int idx = 0;
        for (int i = 0; i < 4; i++) if (sizes[i] == g_fontSize) { idx = (i+1)%4; break; }
        g_fontSize = sizes[idx];
        saveSettings();
        if (g_state == STATE_READING && g_currentFile.length() > 0) {
            uint32_t oldOffset = g_pageOffsets[g_currentPage];
            M5Cardputer.Display.fillScreen(colBg());
            M5Cardputer.Display.setFont(&fonts::Font0);
            M5Cardputer.Display.setTextColor(colTextMain());
            M5Cardputer.Display.setCursor(MARGIN_X, 40);
            M5Cardputer.Display.print("Rebuilding index...");
            drawProgressBar(0);
            clearPageCache();
            g_pageCount = 0;
            buildPageIndex(g_currentFile, g_aozoraMode);
            g_currentPage = findPageByOffset(oldOffset);
            saveState(g_currentFile, g_currentPage, g_aozoraMode);
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
    // ---- V: フッター切替（読書中のみ） ----
    if ((key == 'v' || key == 'V') && g_state == STATE_READING) {
        g_showFooter = !g_showFooter;
        saveSettings();
        
        // ページ再計算（CONTENT_H が変わるため）
        if (g_currentFile.length() > 0) {
            M5Cardputer.Display.fillScreen(colBg());
            M5Cardputer.Display.setFont(&fonts::Font0);
            M5Cardputer.Display.setTextColor(colTextMain());
            M5Cardputer.Display.setCursor(MARGIN_X, 40);
            M5Cardputer.Display.print("Rebuilding index...");
            drawProgressBar(0);
            uint32_t oldOffset = g_pageOffsets[g_currentPage];
            clearPageCache();
            g_pageCount = 0;
            buildPageIndex(g_currentFile, g_aozoraMode);
            g_currentPage = findPageByOffset(oldOffset);
            saveState(g_currentFile, g_currentPage, g_aozoraMode);
        }
        drawReadingPage();
        return;
    }
    // ---- L: 節電モード切替（全画面共通） ----
    if (key == 'l' || key == 'L') {
        g_autoBrightnessEnabled = !g_autoBrightnessEnabled;
        saveSettings();
        if (g_autoBrightnessEnabled) {
            restoreBrightness();
        } else {
            applyBrightness(g_brightnessValues[g_brightnessLevel]);
        }
        updateHeaderBrightness();  // ★ ヘッダーの節電表示も更新
        return;
    }
    // ---- Q: フォント切替（全画面共通、TTFが読み込まれている場合のみ） ----
    if ((key == 'q' || key == 'Q') && g_jpFontLoaded) {
        if (g_useTTFFont) {
            // TTF→内蔵フォント切替
            g_useTTFFont = false;
            
            // TTFが12px以外だった場合、インデックスを12px前提で再構築
            if (g_fontSize != 12 && g_state == STATE_READING && g_currentFile.length() > 0) {
                uint32_t oldOffset = g_pageOffsets[g_currentPage];
                M5Cardputer.Display.fillScreen(colBg());
                M5Cardputer.Display.setFont(&fonts::Font0);
                M5Cardputer.Display.setTextColor(colTextMain());
                M5Cardputer.Display.setCursor(MARGIN_X, 40);
                M5Cardputer.Display.print("Rebuilding index...");
                drawProgressBar(0);
                clearPageCache();
                g_pageCount = 0;
                g_fontSize = 12;  // ★ 内蔵フォント用に12pxに固定
                buildPageIndex(g_currentFile, g_aozoraMode);
                g_currentPage = findPageByOffset(oldOffset);
                saveState(g_currentFile, g_currentPage, g_aozoraMode);
            }
            saveSettings();  // ★ 最後に保存
        } else {
            // 内蔵フォント→TTF切替
            g_useTTFFont = true;
            saveSettings();
        }
        
        if (g_state == STATE_FILE_SELECT) drawFileSelect(); else drawReadingPage();
        return;
    }
    // ---- -/= : 明るさ調整（全画面共通） ----
    if (key == '-') {
        adjustBrightness(-1);
        updateHeaderBrightness();  // ヘッダーの明るさ表示だけ更新
        return;
    }
    if (key == '=') {
        adjustBrightness(1);
        updateHeaderBrightness();  // ヘッダーの明るさ表示だけ更新
        return;
    }
    // ---- H: ヘルプ表示（全画面共通） ----
    if (key == 'h' || key == 'H') {
        bool escFromHelp = showHelp();
        if (escFromHelp && g_state == STATE_READING) {
            saveState(g_currentFile, g_currentPage, g_aozoraMode);
            g_state = STATE_FILE_SELECT;
            drawFileSelect();
        } else if (g_state == STATE_READING) drawReadingPage(); else drawFileSelect();
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
            moveCursor(g_fileCursor - 1);
        } else if (key == '.') {
            moveCursor(g_fileCursor + 1);
        } else if (key == ',' || key == '<') {  // PageUp（M5Cardputer: , と < は同キー）
            moveCursor(g_fileCursor - MAX_FILE_ROWS);
        } else if (key == '/' || key == '>') {  // PageDown
            moveCursor(min(g_fileCursor + MAX_FILE_ROWS, g_fileCount - 1));
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
            clearPageCache();
            g_pageCount   = 0;
            g_currentPage = 0;
            buildPageIndex(g_currentFile, g_aozoraMode);
            saveState(g_currentFile, g_currentPage, g_aozoraMode);
            drawReadingPage();
        } else if (isEscKey(status, key)) {
            saveState(g_currentFile, g_currentPage, g_aozoraMode);
            // ★ TTFモード → ファイル選択モードに移行する時、内蔵フォント固定化
            if (g_useTTFFont) {
                g_useTTFFont = false;
                g_fontSize = 12;
                saveSettings();
            }
            g_state = STATE_FILE_SELECT;
            drawFileSelect();
        }
    }
}
