#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <setjmp.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "png.h"

#include "typen_storage.h"
#include "typen_media.h"
#include "typen_settings.h"
#include "typen_thumb_cache.h"
#include "typen_ui.h"
#include "typen_update.h"
#include "typen_web.h"
#include "typen_wifi.h"

static const char *TAG = "typen_web";

static httpd_handle_t s_server = NULL;
static typen_web_select_cb_t s_select_cb = NULL;
static void *s_select_ctx = NULL;

static uint64_t s_thumb_catalog_signature = 0;
static bool s_thumb_catalog_signature_valid = false;


#define MEDIA_UPLOAD_BUFFER_BYTES              8192
#define MEDIA_UPLOAD_MAX_CONSECUTIVE_TIMEOUTS     6

#define BROWSER_UPDATE_PREFIX_BYTES              512
#define BROWSER_UPDATE_BUFFER_BYTES             8192
#define BROWSER_UPDATE_MAX_CONSECUTIVE_TIMEOUTS    6

typedef struct {
    bool valid;
    bool success;
    size_t bytes_received;
    uint32_t elapsed_ms;
    uint32_t kib_per_second;
    unsigned recv_timeouts;
    char filename[180];
    char message[80];
} typen_transfer_status_t;

static typen_transfer_status_t s_last_transfer = {0};

static void record_transfer_status(
    bool success,
    const char *filename,
    size_t bytes_received,
    int64_t start_us,
    unsigned recv_timeouts,
    const char *message)
{
    const int64_t elapsed_us =
        start_us > 0
            ? esp_timer_get_time() - start_us
            : 0;

    memset(
        &s_last_transfer,
        0,
        sizeof(s_last_transfer)
    );

    s_last_transfer.valid = true;
    s_last_transfer.success = success;
    s_last_transfer.bytes_received =
        bytes_received;

    s_last_transfer.elapsed_ms =
        elapsed_us > 0
            ? (uint32_t)(elapsed_us / 1000)
            : 0U;

    s_last_transfer.kib_per_second =
        elapsed_us > 0
            ? (uint32_t)(
                ((uint64_t)bytes_received *
                 UINT64_C(1000000)) /
                (uint64_t)elapsed_us /
                UINT64_C(1024)
            )
            : 0U;

    s_last_transfer.recv_timeouts =
        recv_timeouts;

    snprintf(
        s_last_transfer.filename,
        sizeof(s_last_transfer.filename),
        "%s",
        filename ? filename : ""
    );

    snprintf(
        s_last_transfer.message,
        sizeof(s_last_transfer.message),
        "%s",
        message ? message : ""
    );
}


static const char INDEX_HTML[] =
"<!doctype html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">\n"
"<meta name=\"theme-color\" content=\"#070a08\">\n"
"<title>TypeN Control</title>\n"
"<style>\n"
":root{\n"
"  --bg:#070a08;--bg2:#0b0f0c;--panel:#101612;--panel2:#141b16;\n"
"  --line:#243129;--line2:#314338;--text:#f3f7f4;--muted:#8fa198;\n"
"  --green:#48d67c;--green2:#2fbf67;--green-dim:#173522;--danger:#ff7f8a;\n"
"  --radius:18px;--shadow:0 16px 50px rgba(0,0,0,.32);\n"
"}\n"
"*{box-sizing:border-box}\n"
"html{background:var(--bg);color-scheme:dark}\n"
"body{\n"
"  margin:0;min-height:100vh;background:\n"
"  radial-gradient(circle at 18% -10%,rgba(72,214,124,.08),transparent 30rem),\n"
"  linear-gradient(180deg,var(--bg2),var(--bg));\n"
"  color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;\n"
"  -webkit-font-smoothing:antialiased;\n"
"}\n"
"button,input{font:inherit}\n"
"button{cursor:pointer}\n"
".app{min-height:100vh;display:grid;grid-template-rows:auto 1fr auto}\n"
".topbar{\n"
"  position:sticky;top:0;z-index:20;display:flex;align-items:center;justify-content:space-between;\n"
"  gap:14px;padding:14px max(16px,env(safe-area-inset-left));\n"
"  background:rgba(7,10,8,.88);backdrop-filter:blur(18px);border-bottom:1px solid rgba(72,214,124,.12)\n"
"}\n"
".brand{display:flex;align-items:center;gap:11px;min-width:0}\n"
".logo{\n"
"  width:38px;height:38px;border-radius:50%;display:grid;place-items:center;flex:0 0 auto;\n"
"  border:1px solid var(--green);color:var(--green);font-weight:900;font-size:16px;\n"
"  box-shadow:0 0 24px rgba(72,214,124,.18),inset 0 0 18px rgba(72,214,124,.06)\n"
"}\n"
".brand-copy{min-width:0}.brand-name{font-weight:800;letter-spacing:.02em}.brand-sub{font-size:11px;color:var(--muted);margin-top:1px}\n"
".connection{display:flex;align-items:center;gap:8px;font-size:12px;color:#b9c9c0;white-space:nowrap}\n"
".dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 12px var(--green)}\n"
".layout{width:min(100%,1360px);margin:0 auto;display:grid;grid-template-columns:1fr}\n"
".nav{\n"
"  display:flex;gap:6px;overflow-x:auto;padding:10px 12px;border-bottom:1px solid var(--line);\n"
"  background:rgba(9,13,10,.92);scrollbar-width:none\n"
"}\n"
".nav::-webkit-scrollbar{display:none}\n"
".nav button{\n"
"  min-height:42px;border:1px solid transparent;border-radius:12px;padding:8px 13px;\n"
"  background:transparent;color:var(--muted);font-weight:700;white-space:nowrap\n"
"}\n"
".nav button.active{background:var(--green-dim);border-color:#28563a;color:#dff9e8}\n"
".content{min-width:0;padding:18px 14px 90px}\n"
".view{display:none}.view.active{display:block}\n"
".hero{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;margin-bottom:16px}\n"
".hero h1{margin:0;font-size:clamp(24px,4.5vw,38px);letter-spacing:-.035em}\n"
".hero p{margin:6px 0 0;color:var(--muted);font-size:14px}\n"
".hero-meta{display:none;text-align:right;color:var(--muted);font-size:12px}\n"
".grid{display:grid;grid-template-columns:1fr;gap:14px}\n"
".card{\n"
"  min-width:0;background:linear-gradient(180deg,rgba(20,27,22,.96),rgba(14,19,16,.96));\n"
"  border:1px solid var(--line);border-radius:var(--radius);box-shadow:var(--shadow);padding:16px\n"
"}\n"
".card-title{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:13px}\n"
".card-title h2{margin:0;font-size:15px;letter-spacing:.015em}.eyebrow{font-size:10px;letter-spacing:.12em;text-transform:uppercase;color:var(--green)}\n"
".now-playing{border-color:#28533a;background:linear-gradient(145deg,#132119,#0e1511)}\n"
".now-path{font-size:13px;color:#d9e7de;overflow-wrap:anywhere;line-height:1.45}\n"
".muted{color:var(--muted)}\n"
".stats{display:grid;grid-template-columns:repeat(2,1fr);gap:9px}\n"
".stat{background:#0b100d;border:1px solid #1e2922;border-radius:14px;padding:12px}\n"
".stat b{display:block;font-size:20px;margin-bottom:2px}.stat span{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}\n"
".upload-zone{display:grid;gap:11px}\n"
".filepick{\n"
"  position:relative;min-height:110px;border:1px dashed #385446;border-radius:15px;\n"
"  background:#0b110d;display:grid;place-items:center;text-align:center;padding:18px\n"
"}\n"
".filepick input{position:absolute;inset:0;width:100%;height:100%;opacity:0;cursor:pointer}\n"
".filepick strong{display:block}.filepick span{display:block;color:var(--muted);font-size:12px;margin-top:5px}\n"
".pathline{font:12px ui-monospace,SFMono-Regular,Consolas,monospace;color:#a7b8ae;overflow-wrap:anywhere}\n"
".actions{display:flex;flex-wrap:wrap;gap:8px}\n"
".btn{\n"
"  min-height:44px;border-radius:12px;border:1px solid #324239;background:#151d18;color:var(--text);\n"
"  padding:9px 13px;font-weight:750\n"
"}\n"
".btn.primary{background:var(--green2);border-color:var(--green2);color:#061009}\n"
".btn.ghost{background:#0c110e}.btn.danger{color:#ffb2b8;border-color:#4b2a2e;background:#1a1012}\n"
".btn:disabled{opacity:.45;cursor:not-allowed}\n"
".progress{height:7px;border-radius:99px;background:#0a0e0c;overflow:hidden;border:1px solid #1b2620}\n"
".progress>i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--green2),#7cf0a5);transition:width .18s}\n"
".update-status{display:grid;gap:9px}\n"
".update-status-row{display:flex;justify-content:space-between;gap:16px;padding:9px 0;border-top:1px solid #202a24;font-size:12px}\n"
".update-status-row:first-child{border-top:0;padding-top:0}\n"
".update-status-row span:first-child{color:var(--muted)}\n"
".update-status-row strong{font-weight:750;text-align:right;overflow-wrap:anywhere}\n"
".update-notice{margin-top:13px;padding:11px 12px;border-radius:11px;border:1px solid #26342b;background:#0d1410;color:#a9bbb0;font-size:12px;line-height:1.45}\n"
".update-notice.busy{border-color:#35533f;background:#0e1812;color:#c8ead3}.update-notice.good{border-color:#386c49;background:#0c1910;color:#c9f3d6}.update-notice.error{border-color:#6a3c3c;background:#1a1010;color:#f0c5c5}\n"
".badge.busy{background:#1b2a20;border-color:#41614a;color:#d4f5de}.badge.good{background:#14271a;border-color:#3e714d;color:#cdf4d8}.badge.error{background:#2a1717;border-color:#754444;color:#f2caca}\n"
".warning{border-left:3px solid #d6a648;padding:11px 13px;background:#17130b;color:#dccba8;border-radius:0 10px 10px 0;font-size:13px;line-height:1.5}\n"
".toolbar{display:grid;grid-template-columns:1fr;gap:9px;margin-bottom:12px}\n"
".search{\n"
"  min-height:44px;border-radius:12px;border:1px solid #2d3b33;background:#0d120f;color:var(--text);\n"
"  padding:10px 13px;outline:none;width:100%\n"
"}\n"
".search:focus{border-color:#3c7651;box-shadow:0 0 0 3px rgba(72,214,124,.08)}\n"
".media-grid{display:grid;grid-template-columns:1fr;gap:10px}\n"
".media{\n"
"  display:grid;grid-template-columns:104px minmax(0,1fr);gap:12px;align-items:center;\n"
"  padding:11px;border:1px solid #233028;border-radius:15px;background:#0d120f\n"
"}\n"
".media.selected{border-color:#357a50;background:#102017;box-shadow:inset 0 0 0 1px rgba(72,214,124,.08)}\n"
".thumb{\n"
"  position:relative;width:104px;aspect-ratio:196/88;border-radius:12px;overflow:hidden;\n"
"  display:grid;place-items:center;background:radial-gradient(circle at 35% 25%,#1f3929,#0b100d 68%);\n"
"  border:1px solid #283a30;font-weight:900;font-size:12px;color:#8fe4ab\n"
"}\n"
".thumb img{position:absolute;inset:0;width:100%;height:100%;object-fit:cover;display:block;background:#080b09}\n"
".thumb .fallback{position:relative;z-index:0;letter-spacing:.08em}\n"
".thumb img:not([src]){display:none}\n"
".media-main{min-width:0}.media-name{font-weight:750;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
".media-path{font-size:11px;color:var(--muted);margin-top:4px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
".media-meta{display:flex;gap:7px;align-items:center;margin-top:7px;color:#84958c;font-size:11px}\n"
".badge{padding:2px 7px;border-radius:999px;background:#17231b;border:1px solid #294033;color:#a7d7b7;font-size:10px;font-weight:800}\n"
".media-actions{grid-column:1/-1;display:flex;gap:7px}\n"
".media-actions .btn{flex:1;min-height:40px;font-size:12px}\n"
".empty{padding:28px 10px;text-align:center;color:var(--muted)}\n"
".section-stack{display:grid;gap:14px}\n"
".setting-row{\n"
"  display:grid;gap:6px;padding:12px 0;border-top:1px solid #202a24\n"
"}\n"
".setting-row:first-child{border-top:0;padding-top:0}\n"
".setting-row b{font-size:13px}.setting-row span{font-size:12px;color:var(--muted);overflow-wrap:anywhere}\n"
".field{display:grid;gap:7px;margin-top:12px}.field:first-child{margin-top:0}\n"
".field label{font-size:12px;font-weight:750;color:#cdd9d1}\n"
".field input[type=text]{\n"
"  width:100%;min-height:44px;border:1px solid #2d3b33;border-radius:12px;background:#0d120f;\n"
"  color:var(--text);padding:10px 12px;outline:none;font:12px ui-monospace,SFMono-Regular,Consolas,monospace\n"
"}\n"
".field input[type=text]:focus{border-color:#3c7651;box-shadow:0 0 0 3px rgba(72,214,124,.08)}\n"
".range-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:12px}\n"
".range-value{font-size:26px;font-weight:850;color:#dff9e8;min-width:72px;text-align:right}\n"
".range{width:100%;accent-color:var(--green);min-height:36px}\n"
".readonly-box{\n"
"  margin-top:10px;padding:10px 12px;border:1px solid #243129;border-radius:12px;background:#0b100d;\n"
"  color:#9fb0a6;font:12px ui-monospace,SFMono-Regular,Consolas,monospace;overflow-wrap:anywhere\n"
"}\n"
".save-row{display:flex;justify-content:flex-end;margin-top:13px}\n"
".save-row .btn{width:100%}\n"
"@media(min-width:620px){.save-row .btn{width:auto;min-width:150px}}\n"
".notice{border-left:3px solid var(--green);padding:11px 13px;background:#0d1510;color:#b9c9c0;border-radius:0 10px 10px 0;font-size:13px;line-height:1.5}\n"
".bottom-space{height:8px}\n"
".toast{\n"
"  position:fixed;left:50%;bottom:calc(76px + env(safe-area-inset-bottom));transform:translateX(-50%) translateY(20px);\n"
"  z-index:50;width:min(calc(100% - 28px),520px);padding:12px 14px;border-radius:13px;background:#18211b;\n"
"  border:1px solid #35453b;color:#eef5f0;box-shadow:0 18px 50px #0008;opacity:0;pointer-events:none;transition:.2s\n"
"}\n"
".toast.show{opacity:1;transform:translateX(-50%) translateY(0)}\n"
".confirm-backdrop{position:fixed;inset:0;z-index:70;display:none;align-items:center;justify-content:center;padding:18px;background:rgba(0,0,0,.72);backdrop-filter:blur(7px)}\n"
".confirm-backdrop.show{display:flex}.confirm-card{width:min(100%,430px);background:#111713;border:1px solid #314338;border-radius:18px;box-shadow:0 24px 70px #000a;padding:18px}\n"
".confirm-card h3{margin:0 0 8px;font-size:18px}.confirm-card p{margin:0;color:#9fb0a6;font-size:13px;line-height:1.5;overflow-wrap:anywhere}\n"
".confirm-name{margin-top:12px;padding:10px 12px;border-radius:12px;background:#0a0f0c;border:1px solid #26332b;font:12px ui-monospace,SFMono-Regular,Consolas,monospace;color:#e7efe9;white-space:pre-wrap;overflow-wrap:anywhere}\n"
".confirm-actions{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:16px}.upload-detail{min-height:18px;color:#8fa198;font-size:11px;line-height:1.45}\n"
".secret-trigger{user-select:none;-webkit-user-select:none;touch-action:manipulation}\n"
".easter-card{border-color:#7f2da8;background:linear-gradient(160deg,#160b19,#111217);box-shadow:0 24px 80px #000d,0 0 42px rgba(221,45,214,.18)}\n"
".easter-kicker{margin-bottom:7px;color:#e92bd8;font-size:10px;font-weight:900;letter-spacing:.16em;text-transform:uppercase}\n"
".btn.easter{background:linear-gradient(135deg,#71259c,#df2bcf);border-color:#e83ada;color:#fff}.confirm-actions.single{grid-template-columns:1fr}\n"
".mobilebar{\n"
"  position:fixed;z-index:30;left:0;right:0;bottom:0;display:grid;grid-template-columns:repeat(4,1fr);\n"
"  padding:7px 8px calc(7px + env(safe-area-inset-bottom));background:rgba(8,11,9,.94);\n"
"  backdrop-filter:blur(18px);border-top:1px solid #233028\n"
"}\n"
".mobilebar button{\n"
"  min-height:48px;border:0;background:transparent;color:#75867d;border-radius:11px;font-size:11px;font-weight:800\n"
"}\n"
".mobilebar button.active{color:#c9f6d8;background:#14241a}\n"
".desktop-note{display:none}\n"
"@media(min-width:620px){\n"
"  .content{padding:22px 20px 95px}.toolbar{grid-template-columns:minmax(220px,1fr) auto}.media-grid{grid-template-columns:repeat(2,minmax(0,1fr))}\n"
"  .stats{grid-template-columns:repeat(4,1fr)}\n"
"}\n"
"@media(min-width:900px){\n"
"  .layout{grid-template-columns:210px minmax(0,1fr)}\n"
"  .nav{position:sticky;top:67px;height:calc(100vh - 67px);align-self:start;flex-direction:column;overflow:visible;padding:22px 14px;border-right:1px solid var(--line);border-bottom:0;background:#0a0e0b}\n"
"  .nav button{text-align:left;width:100%;padding:10px 12px}.content{padding:28px 26px 50px}.mobilebar{display:none}.toast{bottom:24px}.hero-meta{display:block}\n"
"  .grid.dashboard{grid-template-columns:minmax(0,1.45fr) minmax(280px,.7fr)}.desktop-note{display:block}\n"
"}\n"
"@media(min-width:1180px){\n"
"  .media-grid{grid-template-columns:repeat(3,minmax(0,1fr))}.card{padding:18px}.content{padding-left:34px;padding-right:34px}\n"
"}\n"
"@media(hover:hover){\n"
"  .btn:hover{border-color:#476052;transform:translateY(-1px)}.btn.primary:hover{filter:brightness(1.06)}\n"
"  .nav button:hover,.mobilebar button:hover{color:#d5e7dc}\n"
"}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"app\">\n"
"  <header class=\"topbar\">\n"
"    <div class=\"brand\">\n"
"      <div class=\"logo\">N</div>\n"
"      <div class=\"brand-copy\"><div class=\"brand-name\">TypeN</div><div class=\"brand-sub\">local control surface</div></div>\n"
"    </div>\n"
"    <div class=\"connection\"><i class=\"dot\"></i><span id=\"ip\">connecting</span></div>\n"
"  </header>\n"
"\n"
"  <div class=\"layout\">\n"
"    <nav class=\"nav\" aria-label=\"TypeN sections\">\n"
"      <button class=\"active\" data-view=\"media\">Media</button>\n"
"      <button data-view=\"settings\">Settings</button>\n"
"      <button data-view=\"update\">Update</button>\n"
"      <button data-view=\"about\">About</button>\n"
"    </nav>\n"
"\n"
"    <main class=\"content\">\n"
"      <section id=\"view-media\" class=\"view active\">\n"
"        <div class=\"hero\">\n"
"          <div><h1>Media</h1><p>Manage what TypeN displays without leaving the local network.</p></div>\n"
"          <div class=\"hero-meta\"><div>ESP32-P4 + C6</div><div>800 × 800 RGB565</div></div>\n"
"        </div>\n"
"\n"
"        <div class=\"grid dashboard\">\n"
"          <div class=\"section-stack\">\n"
"            <article class=\"card now-playing\">\n"
"              <div class=\"card-title\"><div><div class=\"eyebrow\">Now displaying</div><h2 id=\"selectedName\">No media selected</h2></div><span class=\"badge\" id=\"selectedType\">IDLE</span></div>\n"
"              <div class=\"now-path\" id=\"selectedPath\">—</div>\n"
"            </article>\n"
"\n"
"            <article class=\"card\">\n"
"              <div class=\"card-title\"><div><div class=\"eyebrow\">Library</div><h2>Media on TypeN</h2></div><span class=\"badge\" id=\"mediaCount\">0 items</span></div>\n"
"              <div class=\"toolbar\">\n"
"                <input class=\"search\" id=\"search\" type=\"search\" placeholder=\"Search filename or path\" autocomplete=\"off\">\n"
"                <button class=\"btn ghost\" onclick=\"refreshMedia()\">Refresh</button>\n"
"              </div>\n"
"              <div id=\"mediaGrid\" class=\"media-grid\"><div class=\"empty\">Loading media…</div></div>\n"
"            </article>\n"
"          </div>\n"
"\n"
"          <div class=\"section-stack\">\n"
"            <article class=\"card\">\n"
"              <div class=\"card-title\"><div><div class=\"eyebrow\">Add media</div><h2>Upload to SD</h2></div></div>\n"
"              <div class=\"upload-zone\">\n"
"                <label class=\"filepick\">\n"
"                  <input id=\"file\" type=\"file\" accept=\".jpg,.jpeg,.png,.gif,image/jpeg,image/png,image/gif\">\n"
"                  <div><strong id=\"fileLabel\">Choose an image or GIF</strong><span>Tap to browse this device</span></div>\n"
"                </label>\n"
"                <div><div class=\"muted\" style=\"font-size:11px;margin-bottom:4px\">Destination</div><div class=\"pathline\" id=\"uploadPath\">…</div></div>\n"
"                <div class=\"progress\"><i id=\"uploadProgress\"></i></div>\n"
"                <div id=\"uploadDetail\" class=\"upload-detail\"></div>\n"
"                <div class=\"actions\"><button id=\"uploadBtn\" class=\"btn primary\" onclick=\"upload()\">Upload</button></div>\n"
"              </div>\n"
"            </article>\n"
"\n"
"            <article class=\"card\">\n"
"              <div class=\"card-title\"><div><div class=\"eyebrow\">Device</div><h2>At a glance</h2></div></div>\n"
"              <div class=\"stats\">\n"
"                <div class=\"stat\"><b id=\"statMedia\">0</b><span>Media</span></div>\n"
"                <div class=\"stat\"><b>800</b><span>Pixels</span></div>\n"
"                <div class=\"stat\"><b>32</b><span>MB PSRAM</span></div>\n"
"                <div class=\"stat\"><b>SD</b><span>Primary</span></div>\n"
"              </div>\n"
"            </article>\n"
"          </div>\n"
"        </div>\n"
"      </section>\n"
"\n"
"      <section id=\"view-settings\" class=\"view\">\n"
"        <div class=\"hero\"><div><h1>Settings</h1><p>Persistent TypeN settings shared by the touch UI and this control surface.</p></div></div>\n"
"        <div class=\"grid\">\n"
"          <article class=\"card\">\n"
"            <div class=\"card-title\"><div><div class=\"eyebrow\">Storage</div><h2>Media paths</h2></div></div>\n"
"\n"
"            <div class=\"field\">\n"
"              <label for=\"uploadPathInput\">Upload destination</label>\n"
"              <input id=\"uploadPathInput\" type=\"text\" maxlength=\"511\" spellcheck=\"false\" autocomplete=\"off\" placeholder=\"/sdcard/typen/media\">\n"
"            </div>\n"
"\n"
"            <div class=\"field\">\n"
"              <label for=\"thumbnailPathInput\">Thumbnail path</label>\n"
"              <input id=\"thumbnailPathInput\" type=\"text\" maxlength=\"511\" spellcheck=\"false\" autocomplete=\"off\" placeholder=\"/sdcard/typen/thumbs\">\n"
"            </div>\n"
"\n"
"            <div class=\"field\">\n"
"              <label>Selected media</label>\n"
"              <div class=\"readonly-box\" id=\"settingsSelectedPath\">None</div>\n"
"            </div>\n"
"\n"
"            <div class=\"save-row\"><button class=\"btn primary\" onclick=\"savePaths()\">Save paths</button></div>\n"
"          </article>\n"
"\n"
"          <article class=\"card\">\n"
"            <div class=\"card-title\"><div><div class=\"eyebrow\">Display</div><h2>Brightness</h2></div></div>\n"
"            <div class=\"range-head\"><div><b>Panel backlight</b><div class=\"muted\" style=\"font-size:12px;margin-top:3px\">Shared with the physical TypeN display menu</div></div><div class=\"range-value\" id=\"brightnessValue\">40%</div></div>\n"
"            <input class=\"range\" id=\"brightnessSlider\" type=\"range\" min=\"5\" max=\"100\" step=\"1\" value=\"40\">\n"
"            <div class=\"save-row\"><button class=\"btn primary\" onclick=\"saveBrightness()\">Apply brightness</button></div>\n"
"          </article>\n"
"\n"
"          <article class=\"card\">\n"
"            <div class=\"card-title\"><div><div class=\"eyebrow\">Media</div><h2>Thumbnail cache</h2></div></div>\n"
"            <div class=\"notice\">TypeN stores generated media previews in the configured thumbnail cache. Previews are refreshed automatically when the source media changes.</div>\n"
"          </article>\n"
"        </div>\n"
"      </section>\n"
"\n"
"      <section id=\"view-update\" class=\"view\">\n"
"        <div class=\"hero\"><div><h1>Update</h1><p>Install TypeN firmware from this device or directly from the SD card.</p></div></div>\n"
"\n"
"        <div class=\"grid dashboard\">\n"
"          <div class=\"section-stack\">\n"
"            <article class=\"card\">\n"
"              <div class=\"card-title\"><div><div class=\"eyebrow\">Browser update</div><h2>Upload firmware</h2></div></div>\n"
"              <div class=\"upload-zone\">\n"
"                <label class=\"filepick\">\n"
"                  <input id=\"firmwareFile\" type=\"file\" accept=\".bin,application/octet-stream\">\n"
"                  <div><strong id=\"firmwareFileLabel\">Choose TypeN firmware</strong><span>Select the application .bin produced by ESP-IDF</span></div>\n"
"                </label>\n"
"                <div class=\"progress\"><i id=\"firmwareProgress\"></i></div>\n"
"                <div class=\"actions\"><button id=\"firmwareUploadBtn\" class=\"btn primary\" onclick=\"uploadFirmware()\">Install firmware</button></div>\n"
"              </div>\n"
"            </article>\n"
"\n"
"            <article class=\"card\">\n"
"              <div class=\"card-title\"><div><div class=\"eyebrow\">SD update</div><h2>Install from SD card</h2></div></div>\n"
"              <div class=\"field\">\n"
"                <label for=\"sdUpdatePath\">Firmware path</label>\n"
"                <input id=\"sdUpdatePath\" type=\"text\" maxlength=\"511\" spellcheck=\"false\" autocomplete=\"off\" value=\"/sdcard/typen/update/TypeN.bin\">\n"
"              </div>\n"
"              <div class=\"save-row\"><button id=\"sdUpdateBtn\" class=\"btn primary\" onclick=\"installFromSd()\">Check &amp; install</button></div>\n"
"            </article>\n"
"          </div>\n"
"\n"
"          <div class=\"section-stack\">\n"
"            <article class=\"card\">\n"
"              <div class=\"card-title\"><div><div class=\"eyebrow\">Updater</div><h2>Firmware status</h2></div><span class=\"badge\" id=\"updateState\">IDLE</span></div>\n"
"              <div class=\"update-status\">\n"
"                <div class=\"update-status-row\"><span>Current version</span><strong id=\"runningVersion\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>Current identity</span><strong id=\"runningIdentity\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>Running slot</span><strong id=\"runningSlot\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>Boot slot</span><strong id=\"bootSlot\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>Image state</span><strong id=\"imageState\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>New version</span><strong id=\"targetVersion\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>New identity</span><strong id=\"targetIdentity\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>Target slot</span><strong id=\"targetSlot\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>Progress</span><strong id=\"updatePercent\">0%</strong></div>\n"
"                <div class=\"update-status-row\"><span>Status</span><strong id=\"updateMessage\">Updater ready</strong></div>\n"
"              </div>\n"
"              <div class=\"progress\" style=\"margin-top:13px\"><i id=\"updateProgress\"></i></div>\n"
"              <div id=\"updateNotice\" class=\"update-notice\">Updater ready. No firmware update is in progress.</div>\n"
"            </article>\n"
"\n"
"            <article class=\"card\">\n"
"              <div class=\"card-title\"><div><div class=\"eyebrow\">Recovery</div><h2>Update history &amp; rollback</h2></div></div>\n"
"              <div class=\"update-status\">\n"
"                <div class=\"update-status-row\"><span>Last update</span><strong id=\"lastUpdateResult\">None</strong></div>\n"
"                <div class=\"update-status-row\"><span>Last version</span><strong id=\"lastUpdateVersion\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>Last slot</span><strong id=\"lastUpdateSlot\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>Rollback image</span><strong id=\"rollbackState\">—</strong></div>\n"
"                <div class=\"update-status-row\"><span>Last rejected image</span><strong id=\"invalidImage\">None</strong></div>\n"
"              </div>\n"
"              <div class=\"notice\" style=\"margin-top:13px\">A newly installed image is confirmed only after TypeN reaches its core-ready checkpoint. If it cannot do that, ESP-IDF returns to the previous working firmware.</div>\n"
"            </article>\n"
"          </div>\n"
"        </div>\n"
"      </section>\n"
"\n"
"      <section id=\"view-about\" class=\"view\">\n"
"        <div class=\"hero\"><div><h1>About TypeN</h1><p>Compact round media display and local control interface.</p></div></div>\n"
"        <article class=\"card\">\n"
"          <div class=\"card-title\"><div><div class=\"eyebrow\">Project</div><h2>TypeN</h2></div></div>\n"
"          <div class=\"setting-row\"><b>Platform</b><span>ESP32-P4 with external ESP32-C6 Wi-Fi coprocessor</span></div>\n"
"          <div class=\"setting-row\"><b>Display</b><span>800 × 800 round JD9365 MIPI-DSI</span></div>\n"
"          <div class=\"setting-row\"><b>Media</b><span>JPEG · PNG · GIF · SD-backed library</span></div>\n"
"          <div class=\"setting-row\"><b>Control</b><span>Local-only responsive web interface; no Internet connection required</span></div>\n"
"        </article>\n"
"        <article class=\"card\" style=\"margin-top:14px\">\n"
"          <div class=\"card-title\"><div><div class=\"eyebrow\">Firmware</div><h2>System identity</h2></div></div>\n"
"          <div class=\"setting-row\"><b>Version</b><span id=\"aboutVersion\">—</span></div>\n"
"          <div class=\"setting-row\"><b>Hardware variant</b><span id=\"aboutVariant\">—</span></div>\n"
"          <div class=\"setting-row\"><b>Firmware ABI</b><span id=\"aboutAbi\">—</span></div>\n"
"          <div id=\"aboutBuildTrigger\" class=\"setting-row secret-trigger\"><b>Build</b><span id=\"aboutBuild\">—</span></div>\n"
"          <div class=\"setting-row\"><b>Application slot</b><span id=\"aboutSlot\">—</span></div>\n"
"          <div class=\"setting-row\"><b>Image state</b><span id=\"aboutImageState\">—</span></div>\n"
"          <div class=\"setting-row\"><b>ESP-IDF</b><span id=\"aboutIdf\">—</span></div>\n"
"        </article>\n"
"      </section>\n"
"\n"
"      <div class=\"bottom-space\"></div>\n"
"    </main>\n"
"  </div>\n"
"\n"
"  <nav class=\"mobilebar\" aria-label=\"TypeN mobile sections\">\n"
"    <button class=\"active\" data-view=\"media\">Media</button>\n"
"    <button data-view=\"settings\">Settings</button>\n"
"    <button data-view=\"update\">Update</button>\n"
"    <button data-view=\"about\">About</button>\n"
"  </nav>\n"
"</div>\n"
"<div id=\"deleteConfirm\" class=\"confirm-backdrop\" role=\"dialog\" aria-modal=\"true\" aria-labelledby=\"deleteConfirmTitle\">\n"
"  <div class=\"confirm-card\">\n"
"    <h3 id=\"deleteConfirmTitle\">Delete media?</h3>\n"
"    <p>This permanently removes the selected file from the TypeN SD card.</p>\n"
"    <div id=\"deleteConfirmName\" class=\"confirm-name\"></div>\n"
"    <div class=\"confirm-actions\"><button class=\"btn ghost\" onclick=\"closeDeleteConfirm()\">Cancel</button><button id=\"deleteConfirmButton\" class=\"btn danger\" onclick=\"confirmDelete()\">Delete</button></div>\n"
"  </div>\n"
"</div>\n"
"<div id=\"updateConfirm\" class=\"confirm-backdrop\" role=\"dialog\" aria-modal=\"true\" aria-labelledby=\"updateConfirmTitle\">\n"
"  <div class=\"confirm-card\">\n"
"    <h3 id=\"updateConfirmTitle\">Install firmware?</h3>\n"
"    <p id=\"updateConfirmText\">TypeN will verify the firmware, install it to the inactive OTA slot, and reboot.</p>\n"
"    <div id=\"updateConfirmDetails\" class=\"confirm-name\"></div>\n"
"    <div class=\"confirm-actions\"><button class=\"btn ghost\" onclick=\"closeUpdateConfirm()\">Cancel</button><button id=\"updateConfirmButton\" class=\"btn primary\" onclick=\"confirmUpdateAction()\">Install update</button></div>\n"
"  </div>\n"
"</div>\n"
"<div id=\"easterModal\" class=\"confirm-backdrop\" role=\"dialog\" aria-modal=\"true\" aria-labelledby=\"easterTitle\">\n"
"  <div class=\"confirm-card easter-card\">\n"
"    <div class=\"easter-kicker\">Darkone83 hidden mode</div>\n"
"    <h3 id=\"easterTitle\">Easter egg active</h3>\n"
"    <p>The rotating Darkone83 wireframe is now active on the TypeN display.</p>\n"
"    <div class=\"confirm-actions single\"><button id=\"easterExitButton\" class=\"btn easter\" onclick=\"exitEasterEgg()\">Exit</button></div>\n"
"  </div>\n"
"</div>\n"
"<div id=\"toast\" class=\"toast\"></div>\n"
"\n"
"<script>\n"
"const $=q=>document.querySelector(q), $$=q=>[...document.querySelectorAll(q)];\n"
"let media=[], info={};\n"
"let easterTapCount=0,easterTapReset=0,easterPollTimer=0,easterBusy=false;\n"
"\n"
"function esc(v){return String(v == null ? '' : v).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]))}\n"
"function size(n){n=Number(n)||0;if(n<1024)return n+' B';if(n<1048576)return(n/1024).toFixed(1)+' KB';return(n/1048576).toFixed(1)+' MB'}\n"
"function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('show');clearTimeout(toast.t);toast.t=setTimeout(()=>e.classList.remove('show'),2600)}\n"
"function setView(name){\n"
"  $$('.view').forEach(v=>v.classList.toggle('active',v.id==='view-'+name));\n"
"  $$('[data-view]').forEach(b=>b.classList.toggle('active',b.dataset.view===name));\n"
"  history.replaceState(null,'','#'+name);\n"
"  window.scrollTo({top:0,behavior:'smooth'});\n"
"}\n"
"$$('[data-view]').forEach(b=>b.addEventListener('click',()=>setView(b.dataset.view)));\n"
"\n"
"function showEasterEggModal(){\n"
"  $(\'#easterModal\').classList.add(\'show\');\n"
"  if(!easterPollTimer)easterPollTimer=setInterval(pollEasterEggStatus,700);\n"
"}\n"
"function hideEasterEggModal(){\n"
"  $(\'#easterModal\').classList.remove(\'show\');\n"
"  if(easterPollTimer){clearInterval(easterPollTimer);easterPollTimer=0}\n"
"}\n"
"async function pollEasterEggStatus(){\n"
"  try{\n"
"    const r=await fetch(\'/api/info\',{cache:\'no-store\'});if(!r.ok)return;\n"
"    const s=await r.json();if(!s.easterEggActive)hideEasterEggModal();\n"
"  }catch(e){}\n"
"}\n"
"async function startEasterEgg(){\n"
"  if(easterBusy)return;easterBusy=true;\n"
"  try{\n"
"    const r=await fetch(\'/api/easter-egg/start\',{method:\'POST\',cache:\'no-store\'});\n"
"    if(!r.ok)throw new Error((await r.text())||\'Unable to start Easter egg\');\n"
"    showEasterEggModal();\n"
"  }catch(e){toast(e.message||\'Unable to start Easter egg\')}finally{easterBusy=false}\n"
"}\n"
"async function exitEasterEgg(){\n"
"  if(easterBusy)return;easterBusy=true;const b=$(\'#easterExitButton\');b.disabled=true;b.textContent=\'Exiting…\';\n"
"  try{\n"
"    const r=await fetch(\'/api/easter-egg/exit\',{method:\'POST\',cache:\'no-store\'});\n"
"    if(!r.ok)throw new Error((await r.text())||\'Unable to exit Easter egg\');\n"
"    hideEasterEggModal();toast(\'Easter egg closed\');\n"
"  }catch(e){toast(e.message||\'Unable to exit Easter egg\')}finally{b.disabled=false;b.textContent=\'Exit\';easterBusy=false}\n"
"}\n"
"function handleBuildEasterTap(){\n"
"  clearTimeout(easterTapReset);easterTapCount++;\n"
"  easterTapReset=setTimeout(()=>{easterTapCount=0},2500);\n"
"  if(easterTapCount<5)return;\n"
"  clearTimeout(easterTapReset);easterTapCount=0;startEasterEgg();\n"
"}\n"
"$(\'#aboutBuildTrigger\').addEventListener(\'click\',handleBuildEasterTap);\n"
"\n"
"async function loadInfo(){\n"
"  try{\n"
"    const r=await fetch('/api/info',{cache:'no-store'}); if(!r.ok)throw 0; info=await r.json();\n"
"    $('#ip').textContent=info.ip||'192.168.4.1';\n"
"    $('#uploadPath').textContent=info.uploadPath||'—';\n"
"    $('#settingsSelectedPath').textContent=info.selectedPath||'None';\n"
"    $('#uploadPathInput').value=info.uploadPath||'/sdcard/typen/media';\n"
"    $('#thumbnailPathInput').value=info.thumbnailPath||'/sdcard/typen/thumbs';\n"
"    const b=Number(info.brightness)||40;\n"
"    $('#brightnessSlider').value=String(b);\n"
"    $('#brightnessValue').textContent=b+'%';\n"
"    $('#aboutVersion').textContent=info.version||'—';\n"
"    $('#aboutVariant').textContent=info.firmwareVariant||'—';\n"
"    $('#aboutAbi').textContent=info.firmwareAbi?String(info.firmwareAbi):'—';\n"
"    $('#aboutBuild').textContent=info.build||'—';\n"
"    $('#aboutSlot').textContent=info.runningSlot||'—';\n"
"    $('#aboutImageState').textContent=info.imageState||'—';\n"
"    $('#aboutIdf').textContent=info.idfVersion||'—';\n"
"    if(info.easterEggActive)showEasterEggModal();else if($('#easterModal').classList.contains('show'))hideEasterEggModal();\n"
"  }catch(e){$('#ip').textContent='TypeN local'}\n"
"}\n"
"\n"
"let thumbLoadGeneration=0;\n"
"\n"
"function startThumbLoads(){\n"
"  const generation=++thumbLoadGeneration;\n"
"  const queue=[...document.querySelectorAll('#mediaGrid img[data-thumb]')];\n"
"  let active=0;\n"
"  const pump=()=>{\n"
"    if(generation!==thumbLoadGeneration)return;\n"
"    while(active<2&&queue.length){\n"
"      const img=queue.shift();\n"
"      if(!img||!img.isConnected)continue;\n"
"      const url=img.dataset.thumb;\n"
"      if(!url)continue;\n"
"      active++;\n"
"      const done=()=>{active--;img.onload=null;img.onerror=null;if(generation===thumbLoadGeneration)pump()};\n"
"      img.onload=done;\n"
"      img.onerror=()=>{img.style.display='none';done()};\n"
"      delete img.dataset.thumb;\n"
"      img.src=url;\n"
"    }\n"
"  };\n"
"  pump();\n"
"}\n"
"\n"
"function render(){\n"
"  const q=$('#search').value.trim().toLowerCase();\n"
"  const list=media.filter(m=>!q||m.name.toLowerCase().includes(q)||m.path.toLowerCase().includes(q));\n"
"  $('#mediaCount').textContent=media.length+' item'+(media.length===1?'':'s');\n"
"  $('#statMedia').textContent=media.length;\n"
"  const selected=media.find(m=>m.selected);\n"
"  $('#selectedName').textContent=selected?selected.name:'No media selected';\n"
"  $('#selectedType').textContent=selected?selected.type:'IDLE';\n"
"  $('#selectedPath').textContent=selected?selected.path:(info.selectedPath||'—');\n"
"  $('#settingsSelectedPath').textContent=selected?selected.path:(info.selectedPath||'None');\n"
"\n"
"  if(!list.length){$('#mediaGrid').innerHTML='<div class=\"empty\">No matching media.</div>';return}\n"
"  $('#mediaGrid').innerHTML=list.map(m=>{\n"
"    const thumb='/api/thumb?path='+encodeURIComponent(m.path);\n"
"    return `<article class=\"media ${m.selected?'selected':''}\">\n"
"      <div class=\"thumb\"><span class=\"fallback\">${esc(m.type)}</span><img decoding=\"async\" data-thumb=\"${esc(thumb)}\" alt=\"\"></div>\n"
"      <div class=\"media-main\">\n"
"        <div class=\"media-name\">${esc(m.name)}</div>\n"
"        <div class=\"media-path\" title=\"${esc(m.path)}\">${esc(m.path)}</div>\n"
"        <div class=\"media-meta\"><span class=\"badge\">${esc(m.type)}</span><span>${size(m.size)}</span>${m.selected?'<span>● active</span>':''}</div>\n"
"      </div>\n"
"      <div class=\"media-actions\">\n"
"        <button class=\"btn primary\" onclick='selectMedia(${JSON.stringify(m.path)})'>Show</button>\n"
"        <button class=\"btn danger\" onclick='askDelete(${JSON.stringify(m.path)},${JSON.stringify(m.name)})'>Delete</button>\n"
"      </div>\n"
"    </article>`}).join('');\n"
"  startThumbLoads();\n"
"}\n"
"\n"
"async function refreshMedia(){\n"
"  $('#mediaGrid').innerHTML='<div class=\"empty\">Refreshing library…</div>';\n"
"  try{\n"
"    const r=await fetch('/api/media',{cache:'no-store'}); if(!r.ok)throw 0;\n"
"    media=await r.json(); render(); await loadInfo();\n"
"  }catch(e){$('#mediaGrid').innerHTML='<div class=\"empty\">Unable to read the SD media library.</div>'}\n"
"}\n"
"$('#search').addEventListener('input',render);\n"
"\n"
"$('#brightnessSlider').addEventListener('input',e=>{\n"
"  $('#brightnessValue').textContent=String(e.target.value)+'%';\n"
"});\n"
"\n"
"async function postSettings(values){\n"
"  const body=new URLSearchParams(values).toString();\n"
"  const r=await fetch('/api/settings',{\n"
"    method:'POST',\n"
"    headers:{'Content-Type':'application/x-www-form-urlencoded;charset=utf-8'},\n"
"    body\n"
"  });\n"
"  const t=await r.text();\n"
"  if(!r.ok)throw new Error(t||'Settings update failed');\n"
"  await loadInfo();\n"
"  return t;\n"
"}\n"
"\n"
"async function savePaths(){\n"
"  const upload=$('#uploadPathInput').value.trim();\n"
"  const thumbs=$('#thumbnailPathInput').value.trim();\n"
"  if(!upload.startsWith('/sdcard')||!thumbs.startsWith('/sdcard')){\n"
"    toast('Paths must stay on /sdcard');\n"
"    return;\n"
"  }\n"
"  try{\n"
"    await postSettings({uploadPath:upload,thumbnailPath:thumbs});\n"
"    toast('Paths saved');\n"
"  }catch(e){toast(e.message||'Unable to save paths')}\n"
"}\n"
"\n"
"async function saveBrightness(){\n"
"  const value=$('#brightnessSlider').value;\n"
"  try{\n"
"    await postSettings({brightness:value});\n"
"    toast('Brightness applied');\n"
"  }catch(e){toast(e.message||'Unable to set brightness')}\n"
"}\n"
"\n"
"$('#file').addEventListener('change',e=>{\n"
"  const f=e.target.files[0]; $('#fileLabel').textContent=f?`${f.name} · ${size(f.size)}`:'Choose an image or GIF';\n"
"});\n"
"\n"
"const TYPEN_JPEG_MAX_EDGE=1024;\n"
"\n"
"function isJpegFile(f){\n"
"  const n=String(f.name||'').toLowerCase();\n"
"  return f.type==='image/jpeg'||n.endsWith('.jpg')||n.endsWith('.jpeg');\n"
"}\n"
"\n"
"async function loadUploadImage(file){\n"
"  if('createImageBitmap' in window){\n"
"    try{return await createImageBitmap(file,{imageOrientation:'from-image'})}catch(e){}\n"
"    try{return await createImageBitmap(file)}catch(e){}\n"
"  }\n"
"  return await new Promise((resolve,reject)=>{\n"
"    const url=URL.createObjectURL(file),img=new Image();\n"
"    img.onload=()=>{URL.revokeObjectURL(url);resolve(img)};\n"
"    img.onerror=()=>{URL.revokeObjectURL(url);reject(new Error('Unable to decode JPEG in browser'))};\n"
"    img.src=url;\n"
"  });\n"
"}\n"
"\n"
"function canvasJpegBlob(canvas,quality){\n"
"  return new Promise((resolve,reject)=>canvas.toBlob(b=>b?resolve(b):reject(new Error('JPEG conversion failed')),'image/jpeg',quality));\n"
"}\n"
"\n"
"async function prepareUpload(file){\n"
"  if(!isJpegFile(file))return {blob:file,name:file.name,detail:''};\n"
"  $('#uploadDetail').textContent='Optimizing JPEG for TypeN hardware decode…';\n"
"  const image=await loadUploadImage(file);\n"
"  const sw=Number(image.width||image.naturalWidth)||0,sh=Number(image.height||image.naturalHeight)||0;\n"
"  if(!sw||!sh)throw new Error('Unable to determine JPEG dimensions');\n"
"  const scale=Math.min(1,TYPEN_JPEG_MAX_EDGE/Math.max(sw,sh));\n"
"  let dw=Math.max(8,Math.round((sw*scale)/8)*8),dh=Math.max(8,Math.round((sh*scale)/8)*8);\n"
"  dw=Math.max(8,Math.min(TYPEN_JPEG_MAX_EDGE,Math.floor(dw/8)*8));\n"
"  dh=Math.max(8,Math.min(TYPEN_JPEG_MAX_EDGE,Math.floor(dh/8)*8));\n"
"  const canvas=document.createElement('canvas');canvas.width=dw;canvas.height=dh;\n"
"  const ctx=canvas.getContext('2d',{alpha:false});if(!ctx)throw new Error('Canvas unavailable');\n"
"  ctx.fillStyle='#000';ctx.fillRect(0,0,dw,dh);ctx.drawImage(image,0,0,dw,dh);\n"
"  if(image.close)try{image.close()}catch(e){}\n"
"  const blob=await canvasJpegBlob(canvas,.90);\n"
"  return {blob:blob,name:file.name,detail:`JPEG optimized ${sw}×${sh} → ${dw}×${dh} · ${size(file.size)} → ${size(blob.size)}`};\n"
"}\n"
"\n"
"async function upload(){\n"
"  const f=$('#file').files[0];if(!f){toast('Choose a JPEG, PNG, or GIF first.');return}\n"
"  const btn=$('#uploadBtn'),bar=$('#uploadProgress');btn.disabled=true;bar.style.width='0%';\n"
"  try{\n"
"    const prepared=await prepareUpload(f);\n"
"    if(prepared.detail)$('#uploadDetail').textContent=prepared.detail;\n"
"    const x=new XMLHttpRequest();\n"
"    x.open('POST','/api/upload?name='+encodeURIComponent(prepared.name));\n"
"    x.upload.onprogress=e=>{if(e.lengthComputable)bar.style.width=((e.loaded/e.total)*100).toFixed(1)+'%'};\n"
"    x.onload=async()=>{btn.disabled=false;if(x.status>=200&&x.status<300){bar.style.width='100%';toast('Upload complete');$('#file').value='';$('#fileLabel').textContent='Choose an image or GIF';await refreshMedia()}else toast(x.responseText||'Upload failed')};\n"
"    x.onerror=()=>{btn.disabled=false;toast('Upload failed')};\n"
"    x.send(prepared.blob);\n"
"  }catch(e){btn.disabled=false;$('#uploadDetail').textContent='';toast(e.message||'Unable to prepare upload')}\n"
"}\n"
"\n"
"async function selectMedia(path){\n"
"  try{\n"
"    const r=await fetch('/api/select',{method:'POST',headers:{'Content-Type':'text/plain;charset=utf-8'},body:path});\n"
"    const t=await r.text(); if(!r.ok)throw new Error(t); toast('Now displaying selected media'); await refreshMedia();\n"
"  }catch(e){toast(e.message||'Selection failed')}\n"
"}\n"
"\n"
"let pendingDelete=null;\n"
"\n"
"function askDelete(path,name){\n"
"  pendingDelete={path:path,name:name};\n"
"  $('#deleteConfirmName').textContent=name+'\\n'+path;\n"
"  $('#deleteConfirmButton').disabled=false;\n"
"  $('#deleteConfirm').classList.add('show');\n"
"}\n"
"\n"
"function closeDeleteConfirm(){\n"
"  if($('#deleteConfirmButton').disabled)return;\n"
"  pendingDelete=null;$('#deleteConfirm').classList.remove('show');\n"
"}\n"
"\n"
"async function confirmDelete(){\n"
"  if(!pendingDelete)return;\n"
"  const item=pendingDelete,button=$('#deleteConfirmButton');\n"
"  button.disabled=true;button.textContent='Deleting…';\n"
"  try{\n"
"    const r=await fetch('/api/delete?path='+encodeURIComponent(item.path),{method:'POST',cache:'no-store'});\n"
"    const t=await r.text();if(!r.ok)throw new Error(t||'Delete failed');\n"
"    pendingDelete=null;$('#deleteConfirm').classList.remove('show');toast('Deleted '+item.name);await refreshMedia();\n"
"  }catch(e){toast(e.message||'Delete failed')}\n"
"  finally{button.disabled=false;button.textContent='Delete'}\n"
"}\n"
"\n"
"$('#deleteConfirm').addEventListener('click',e=>{if(e.target.id==='deleteConfirm')closeDeleteConfirm()});\n"
"\n"
"$('#firmwareFile').addEventListener('change',e=>{\n"
"  const f=e.target.files[0];\n"
"  $('#firmwareFileLabel').textContent=f?`${f.name} · ${size(f.size)}`:'Choose TypeN firmware';\n"
"});\n"
"\n"
"let updatePollTimer=0,updatePollRunning=false,lastUpdateState='idle',pendingUpdateAction=null;\n"
"\n"
"function setUpdateNotice(kind,text){\n"
"  const n=$('#updateNotice');\n"
"  n.className='update-notice'+(kind?' '+kind:'');\n"
"  n.textContent=text;\n"
"}\n"
"\n"
"function setUpdateBadge(state){\n"
"  const b=$('#updateState');\n"
"  b.textContent=String(state||'idle').toUpperCase();\n"
"  const kind=state==='error'?'error':state==='rebooting'?'good':['preparing','writing','verifying'].includes(state)?'busy':'';\n"
"  b.className='badge'+(kind?' '+kind:'');\n"
"}\n"
"\n"
"function openUpdateConfirm(action,title,text,details){\n"
"  pendingUpdateAction=action;\n"
"  $('#updateConfirmTitle').textContent=title;\n"
"  $('#updateConfirmText').textContent=text;\n"
"  $('#updateConfirmDetails').textContent=details;\n"
"  $('#updateConfirmButton').disabled=false;\n"
"  $('#updateConfirmButton').textContent='Install update';\n"
"  $('#updateConfirm').classList.add('show');\n"
"}\n"
"\n"
"function closeUpdateConfirm(){\n"
"  if($('#updateConfirmButton').disabled)return;\n"
"  pendingUpdateAction=null;\n"
"  $('#updateConfirm').classList.remove('show');\n"
"}\n"
"\n"
"$('#updateConfirm').addEventListener('click',e=>{if(e.target.id==='updateConfirm')closeUpdateConfirm()});\n"
"\n"
"function scheduleUpdatePoll(delay=300){\n"
"  clearTimeout(updatePollTimer);\n"
"  updatePollTimer=setTimeout(()=>refreshUpdateStatus(true),delay);\n"
"}\n"
"\n"
"async function refreshUpdateStatus(scheduleNext=true){\n"
"  if(updatePollRunning)return;\n"
"  updatePollRunning=true;\n"
"  let busy=false;\n"
"  try{\n"
"    const r=await fetch('/api/update/status',{cache:'no-store'});\n"
"    if(!r.ok)throw new Error('Status request failed');\n"
"    const u=await r.json();\n"
"    const state=String(u.state||'idle');\n"
"    lastUpdateState=state;\n"
"    const total=Number(u.total)||0,written=Number(u.written)||0;\n"
"    const pct=total>0?Math.max(0,Math.min(100,(written/total)*100)):0;\n"
"    setUpdateBadge(state);\n"
"    $('#runningVersion').textContent=u.runningVersion||'—';\n"
"    $('#runningIdentity').textContent=u.runningVariant?`${u.runningVariant} · ABI ${u.runningAbi||'?'}`:'—';\n"
"    $('#runningSlot').textContent=u.running||'—';\n"
"    $('#bootSlot').textContent=u.boot||'—';\n"
"    $('#imageState').textContent=u.imageState||'—';\n"
"    $('#targetVersion').textContent=u.targetVersion||'—';\n"
"    $('#targetIdentity').textContent=u.targetVariant?`${u.targetVariant} · ABI ${u.targetAbi||'?'}`:'—';\n"
"    $('#targetSlot').textContent=u.target||'—';\n"
"    $('#lastUpdateResult').textContent=(u.lastResult||'none').replace('_',' ').toUpperCase();\n"
"    $('#lastUpdateVersion').textContent=u.lastVersion||'—';\n"
"    $('#lastUpdateSlot').textContent=u.lastPartition||'—';\n"
"    $('#rollbackState').textContent=u.rollbackPossible?'Available':'Not available';\n"
"    $('#invalidImage').textContent=u.invalidPartition?`${u.invalidVersion||'unknown'} · ${u.invalidPartition}`:'None';\n"
"    $('#updatePercent').textContent=pct.toFixed(0)+'%';\n"
"    $('#updateMessage').textContent=u.message||'Updater ready';\n"
"    $('#updateProgress').style.width=pct.toFixed(1)+'%';\n"
"    busy=['preparing','writing','verifying','rebooting'].includes(state);\n"
"    $('#firmwareUploadBtn').disabled=busy;\n"
"    $('#sdUpdateBtn').disabled=busy;\n"
"    if(state==='error')setUpdateNotice('error',u.message||'Firmware update failed.');\n"
"    else if(state==='rebooting')setUpdateNotice('good','Firmware verified and installed. TypeN is rebooting into the new image.');\n"
"    else if(state==='verifying')setUpdateNotice('busy','Firmware written. TypeN is validating the image before changing the boot slot.');\n"
"    else if(state==='writing')setUpdateNotice('busy',`Writing firmware to ${u.target||'the inactive OTA slot'} — ${pct.toFixed(0)}% complete.`);\n"
"    else if(state==='preparing')setUpdateNotice('busy',u.message||'Preparing the inactive OTA slot.');\n"
"    else if(u.lastResult==='rolled_back')setUpdateNotice('error',`The previous update${u.lastVersion?' to '+u.lastVersion:''} did not reach confirmation and was rolled back. TypeN is running ${u.runningVersion||'the previous firmware'} from ${u.running||'the working slot'}.`);\n"
"    else if(u.lastResult==='confirmed'&&u.lastPartition===u.running)setUpdateNotice('good',`Firmware ${u.runningVersion||u.lastVersion||''} is confirmed and running from ${u.running||'the active OTA slot'}.`);\n"
"    else if(u.lastResult==='pending')setUpdateNotice('busy',`Firmware ${u.lastVersion||''} is awaiting confirmation on ${u.lastPartition||'the new OTA slot'}.`);\n"
"    else setUpdateNotice('',`Updater ready. Running ${u.runningVersion||'the current firmware'} from ${u.running||'the current application slot'}.`);\n"
"  }catch(e){\n"
"    if(lastUpdateState==='rebooting')setUpdateNotice('good','TypeN is rebooting. The web connection may disappear briefly; reconnect to TypeN Setup if needed.');\n"
"  }finally{\n"
"    updatePollRunning=false;\n"
"    if(scheduleNext){\n"
"      const fast=['preparing','writing','verifying','rebooting'].includes(lastUpdateState);\n"
"      scheduleUpdatePoll(fast?1000:8000);\n"
"    }\n"
"  }\n"
"}\n"
"\n"
"async function uploadFirmware(){\n"
"  const f=$('#firmwareFile').files[0];\n"
"  if(!f){toast('Choose the TypeN application .bin first.');return}\n"
"  if(!String(f.name).toLowerCase().endsWith('.bin')){toast('Firmware must be the TypeN application .bin');return}\n"
"  const btn=$('#firmwareUploadBtn');\n"
"  btn.disabled=true;\n"
"  setUpdateBadge('preparing');\n"
"  setUpdateNotice('busy','Checking the firmware image before installation…');\n"
"  try{\n"
"    const prefix=await f.slice(0,512).arrayBuffer();\n"
"    const r=await fetch('/api/update/upload/check?size='+encodeURIComponent(f.size),{\n"
"      method:'POST',\n"
"      cache:'no-store',\n"
"      headers:{'Content-Type':'application/octet-stream'},\n"
"      body:prefix\n"
"    });\n"
"    const text=await r.text();\n"
"    if(!r.ok)throw new Error(text||'Unable to check firmware image');\n"
"    const info=JSON.parse(text);\n"
"    if(!info.valid){\n"
"      setUpdateBadge('error');\n"
"      setUpdateNotice('error',info.message||'The selected firmware image is not valid.');\n"
"      toast(info.message||'Firmware preflight failed');\n"
"      return;\n"
"    }\n"
"    setUpdateBadge('idle');\n"
"    setUpdateNotice('good',info.message||'Firmware image is ready to install.');\n"
"    openUpdateConfirm(\n"
"      {kind:'browser',file:f,info:info},\n"
"      'Install uploaded firmware?',\n"
"      'The file has passed the TypeN application checks. Installation writes the inactive OTA slot and reboots only after full ESP-IDF verification.',\n"
"      `${f.name}\\nProject: ${info.project||'TypeN'}\\nVersion: ${info.version||'unknown'}\\nVariant: ${info.variant||'unknown'}\\nFirmware ABI: ${info.abi||'unknown'}\\nSize: ${size(info.size)}\\nSlot: ${info.running||'current'} → ${info.target||'inactive'}`\n"
"    );\n"
"  }catch(e){\n"
"    setUpdateBadge('error');\n"
"    setUpdateNotice('error',e.message||'Unable to check firmware image.');\n"
"    toast(e.message||'Firmware preflight failed');\n"
"  }finally{\n"
"    btn.disabled=false;\n"
"  }\n"
"}\n"
"\n"
"async function installFromSd(){\n"
"  const path=$('#sdUpdatePath').value.trim();\n"
"  if(!path.startsWith('/sdcard/')||!path.toLowerCase().endsWith('.bin')){toast('Choose a .bin file inside /sdcard');return}\n"
"  const btn=$('#sdUpdateBtn');btn.disabled=true;\n"
"  setUpdateBadge('preparing');\n"
"  setUpdateNotice('busy','Checking the SD firmware image before installation…');\n"
"  try{\n"
"    const r=await fetch('/api/update/sd/check?path='+encodeURIComponent(path),{cache:'no-store'});\n"
"    if(!r.ok)throw new Error(await r.text()||'Unable to check SD firmware');\n"
"    const info=await r.json();\n"
"    if(!info.valid){\n"
"      setUpdateBadge('error');\n"
"      setUpdateNotice('error',info.message||'The SD firmware image is not valid.');\n"
"      toast(info.message||'SD firmware check failed');\n"
"      return;\n"
"    }\n"
"    setUpdateBadge('idle');\n"
"    setUpdateNotice('good',info.message||'Firmware image is ready to install.');\n"
"    openUpdateConfirm(\n"
"      {kind:'sd',path:path,info:info},\n"
"      'Install firmware from SD?',\n"
"      'The file has passed the TypeN preflight checks. Installation writes the inactive OTA slot and reboots only after verification.',\n"
"      `${path}\\nProject: ${info.project||'TypeN'}\\nVersion: ${info.version||'unknown'}\\nVariant: ${info.variant||'unknown'}\\nFirmware ABI: ${info.abi||'unknown'}\\nSize: ${size(info.size)}\\nSlot: ${info.running||'current'} → ${info.target||'inactive'}`\n"
"    );\n"
"  }catch(e){\n"
"    setUpdateBadge('error');\n"
"    setUpdateNotice('error',e.message||'Unable to check SD firmware.');\n"
"    toast(e.message||'SD firmware check failed');\n"
"  }finally{btn.disabled=false}\n"
"}\n"
"\n"
"async function confirmUpdateAction(){\n"
"  if(!pendingUpdateAction)return;\n"
"  const action=pendingUpdateAction,button=$('#updateConfirmButton');\n"
"  button.disabled=true;button.textContent='Starting…';\n"
"  if(action.kind==='browser'){\n"
"    $('#updateConfirm').classList.remove('show');\n"
"    pendingUpdateAction=null;\n"
"    startBrowserFirmware(action.file,action.info);\n"
"    button.disabled=false;button.textContent='Install update';\n"
"    return;\n"
"  }\n"
"  try{\n"
"    setUpdateBadge('preparing');\n"
"    setUpdateNotice('busy','Starting SD firmware installation…');\n"
"    const r=await fetch('/api/update/sd?path='+encodeURIComponent(action.path),{method:'POST',cache:'no-store'});\n"
"    const t=await r.text();\n"
"    if(!r.ok)throw new Error(t||'Unable to start SD update');\n"
"    pendingUpdateAction=null;\n"
"    $('#updateConfirm').classList.remove('show');\n"
"    toast('SD firmware update started');\n"
"    scheduleUpdatePoll(150);\n"
"  }catch(e){\n"
"    setUpdateBadge('error');\n"
"    setUpdateNotice('error',e.message||'SD update failed to start.');\n"
"    toast(e.message||'SD update failed');\n"
"  }finally{button.disabled=false;button.textContent='Install update'}\n"
"}\n"
"\n"
"function startBrowserFirmware(f,info){\n"
"  const btn=$('#firmwareUploadBtn'),bar=$('#firmwareProgress');\n"
"  btn.disabled=true;bar.style.width='0%';\n"
"  setUpdateBadge('preparing');\n"
"  setUpdateNotice('busy',`Uploading ${info&&info.version?info.version:'firmware'} to TypeN…`);\n"
"  const x=new XMLHttpRequest();\n"
"  x.open('POST','/api/update/upload');\n"
"  x.setRequestHeader('Content-Type','application/octet-stream');\n"
"  x.upload.onprogress=e=>{\n"
"    if(e.lengthComputable){\n"
"      const pct=(e.loaded/e.total)*100;\n"
"      bar.style.width=pct.toFixed(1)+'%';\n"
"      $('#updateProgress').style.width=pct.toFixed(1)+'%';\n"
"      $('#updatePercent').textContent=pct.toFixed(0)+'%';\n"
"      setUpdateNotice('busy',`Uploading firmware — ${pct.toFixed(0)}% transmitted.`);\n"
"    }\n"
"  };\n"
"  x.onload=()=>{\n"
"    if(x.status>=200&&x.status<300){\n"
"      bar.style.width='100%';\n"
"      toast('Firmware verified. TypeN is rebooting.');\n"
"      scheduleUpdatePoll(150);\n"
"    }else{\n"
"      btn.disabled=false;\n"
"      setUpdateBadge('error');\n"
"      setUpdateNotice('error',x.responseText||'Firmware update failed.');\n"
"      toast(x.responseText||'Firmware update failed');\n"
"      scheduleUpdatePoll(150);\n"
"    }\n"
"  };\n"
"  x.onerror=()=>{\n"
"    btn.disabled=false;\n"
"    setUpdateNotice(lastUpdateState==='rebooting'?'good':'error',lastUpdateState==='rebooting'?'TypeN is rebooting into the new firmware.':'Firmware upload connection failed.');\n"
"    scheduleUpdatePoll(300);\n"
"  };\n"
"  x.send(f);\n"
"}\n"
"\n"
"\n"
"(async()=>{\n"
"  const initial=location.hash.slice(1); if(['media','settings','update','about'].includes(initial))setView(initial);\n"
"  await loadInfo(); await refreshMedia(); await refreshUpdateStatus(true);\n"
"})();\n"
"</script>\n"
"</body>\n"
"</html>"
;static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *src = s;
    char *dst = s;

    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = hexval(src[1]);
            int lo = hexval(src[2]);

            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }

        if (*src == '+') {
            *dst++ = ' ';
            ++src;
        } else {
            *dst++ = *src++;
        }
    }

    *dst = 0;
}

static bool safe_filename(const char *name)
{
    if (!name || !*name || strstr(name, "..")) {
        return false;
    }

    for (const char *p = name; *p; ++p) {
        unsigned char ch = (unsigned char)*p;

        if (ch < 32 ||
            ch == '/' ||
            ch == '\\' ||
            ch == ':' ||
            ch == '"' ||
            ch == '<' ||
            ch == '>' ||
            ch == '|') {
            return false;
        }
    }

    const char *ext = strrchr(name, '.');

    if (!ext) {
        return false;
    }

    return !strcasecmp(ext, ".jpg") ||
           !strcasecmp(ext, ".jpeg") ||
           !strcasecmp(ext, ".png") ||
           !strcasecmp(ext, ".gif");
}

static bool safe_sd_path(const char *path)
{
    if (!path || !*path) {
        return false;
    }

    if (strcmp(path, "/sdcard") != 0 &&
        strncmp(path, "/sdcard/", 8) != 0) {
        return false;
    }

    if (strstr(path, "..") != NULL ||
        strchr(path, '\\') != NULL) {
        return false;
    }

    for (const char *p = path; *p; ++p) {
        if ((unsigned char)*p < 32) {
            return false;
        }
    }

    return true;
}

static esp_err_t ensure_directory_tree(
    const char *path)
{
    if (!safe_sd_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    char temp[TYPEN_SETTINGS_PATH_MAX];

    int written =
        snprintf(
            temp,
            sizeof(temp),
            "%s",
            path
        );

    if (written < 0 ||
        written >= (int)sizeof(temp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t len = strlen(temp);

    while (len > 8U &&
           temp[len - 1U] == '/') {
        temp[len - 1U] = '\0';
        --len;
    }

    for (char *p = temp + 1; *p; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';

        if (mkdir(temp, 0775) != 0 &&
            errno != EEXIST) {
            *p = '/';
            return ESP_FAIL;
        }

        *p = '/';
    }

    if (mkdir(temp, 0775) != 0 &&
        errno != EEXIST) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static uint64_t fnv1a64_bytes(
    uint64_t hash,
    const void *data,
    size_t length)
{
    const uint8_t *bytes =
        (const uint8_t *)data;

    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}

static uint64_t thumbnail_key(
    const typen_media_item_t *item)
{
    uint64_t hash =
        UINT64_C(1469598103934665603);

    hash =
        fnv1a64_bytes(
            hash,
            item->path,
            strlen(item->path)
        );

    struct stat st = {0};

    if (stat(item->path, &st) == 0) {
        const uint64_t size =
            (uint64_t)st.st_size;

        const uint64_t mtime =
            (uint64_t)st.st_mtime;

        hash =
            fnv1a64_bytes(
                hash,
                &size,
                sizeof(size)
            );

        hash =
            fnv1a64_bytes(
                hash,
                &mtime,
                sizeof(mtime)
            );
    } else {
        const uint64_t size =
            (uint64_t)item->size;

        hash =
            fnv1a64_bytes(
                hash,
                &size,
                sizeof(size)
            );
    }

    return hash;
}

static esp_err_t thumbnail_cache_path(
    const typen_media_item_t *item,
    char *path,
    size_t path_size,
    uint64_t *key_out)
{
    if (!item ||
        !path ||
        path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char directory[TYPEN_SETTINGS_PATH_MAX];

    esp_err_t err =
        typen_settings_get_thumbnail_path(
            directory,
            sizeof(directory)
        );

    if (err != ESP_OK) {
        snprintf(
            directory,
            sizeof(directory),
            "%s",
            TYPEN_DEFAULT_THUMBNAIL_PATH
        );
    }

    err =
        ensure_directory_tree(
            directory
        );

    if (err != ESP_OK) {
        return err;
    }

    const uint64_t key =
        thumbnail_key(item);

    int written =
        snprintf(
            path,
            path_size,
            "%s/%016llx.png",
            directory,
            (unsigned long long)key
        );

    if (written < 0 ||
        written >= (int)path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (key_out) {
        *key_out = key;
    }

    return ESP_OK;
}


static int cache_hex_nibble(
    char c)
{
    if (c >= '0' &&
        c <= '9') {
        return c - '0';
    }

    if (c >= 'a' &&
        c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' &&
        c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

static bool thumbnail_cache_name_key(
    const char *name,
    uint64_t *key_out)
{
    if (!name ||
        strlen(name) != 20U ||
        strcmp(
            name + 16,
            ".png") != 0) {
        return false;
    }

    uint64_t key = 0;

    for (size_t i = 0;
         i < 16U;
         ++i) {
        const int nibble =
            cache_hex_nibble(
                name[i]
            );

        if (nibble < 0) {
            return false;
        }

        key =
            (key << 4) |
            (uint64_t)nibble;
    }

    if (key_out) {
        *key_out = key;
    }

    return true;
}

static bool thumbnail_cache_temp_name(
    const char *name)
{
    if (!name ||
        strlen(name) != 24U ||
        strcmp(
            name + 16,
            ".png.tmp") != 0) {
        return false;
    }

    for (size_t i = 0;
         i < 16U;
         ++i) {
        if (cache_hex_nibble(
                name[i]) < 0) {
            return false;
        }
    }

    return true;
}

static bool thumbnail_key_is_live(
    uint64_t key)
{
    const size_t count =
        typen_media_count();

    for (size_t i = 0;
         i < count;
         ++i) {
        typen_media_item_t item;

        if (typen_media_get(
                i,
                &item) != ESP_OK) {
            continue;
        }

        if (thumbnail_key(
                &item) == key) {
            return true;
        }
    }

    return false;
}

static uint64_t thumbnail_catalog_signature(
    const char *directory)
{
    uint64_t hash =
        UINT64_C(1469598103934665603);

    if (directory) {
        hash =
            fnv1a64_bytes(
                hash,
                directory,
                strlen(directory)
            );
    }

    const size_t count =
        typen_media_count();

    hash =
        fnv1a64_bytes(
            hash,
            &count,
            sizeof(count)
        );

    for (size_t i = 0;
         i < count;
         ++i) {
        typen_media_item_t item;

        if (typen_media_get(
                i,
                &item) != ESP_OK) {
            continue;
        }

        const uint64_t key =
            thumbnail_key(
                &item
            );

        hash =
            fnv1a64_bytes(
                hash,
                &key,
                sizeof(key)
            );
    }

    return hash;
}

static void thumbnail_cache_gc(
    const char *directory)
{
    if (!directory ||
        !*directory) {
        return;
    }

    DIR *dir =
        opendir(
            directory
        );

    if (!dir) {
        if (errno != ENOENT) {
            ESP_LOGW(
                TAG,
                "Thumbnail cache cleanup unable to open %s errno=%d",
                directory,
                errno
            );
        }

        return;
    }

    unsigned removed_orphans = 0;
    unsigned removed_temps = 0;

    struct dirent *entry = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        bool remove_file = false;
        bool is_temp = false;

        uint64_t key = 0;

        if (thumbnail_cache_temp_name(
                entry->d_name)) {
            remove_file = true;
            is_temp = true;
        } else if (thumbnail_cache_name_key(
                       entry->d_name,
                       &key) &&
                   !thumbnail_key_is_live(
                       key)) {
            remove_file = true;
        }

        if (!remove_file) {
            continue;
        }

        char path[768];

        const int written =
            snprintf(
                path,
                sizeof(path),
                "%s/%s",
                directory,
                entry->d_name
            );

        if (written < 0 ||
            written >= (int)sizeof(path)) {
            continue;
        }

        if (remove(path) == 0) {
            if (is_temp) {
                ++removed_temps;
            } else {
                ++removed_orphans;
            }
        }
    }

    closedir(dir);

    if (removed_orphans > 0 ||
        removed_temps > 0) {
        ESP_LOGI(
            TAG,
            "Thumbnail cache cleanup: %u orphan(s), %u temp file(s) removed",
            removed_orphans,
            removed_temps
        );
    }
}

static void sync_thumbnail_cache_after_scan(void)
{
    char directory[
        TYPEN_SETTINGS_PATH_MAX
    ];

    if (typen_settings_get_thumbnail_path(
            directory,
            sizeof(directory)) != ESP_OK) {
        snprintf(
            directory,
            sizeof(directory),
            "%s",
            TYPEN_DEFAULT_THUMBNAIL_PATH
        );
    }

    const uint64_t signature =
        thumbnail_catalog_signature(
            directory
        );

    if (s_thumb_catalog_signature_valid &&
        signature ==
            s_thumb_catalog_signature) {
        return;
    }

    /*
     * The physical UI cache is indexed by catalog position. Any catalog
     * change must invalidate those PSRAM entries before an index can refer to
     * different media.
     */
    typen_ui_set_media_count(
        typen_media_count()
    );

    if (ensure_directory_tree(
            directory) == ESP_OK) {
        thumbnail_cache_gc(
            directory
        );
    }

    s_thumb_catalog_signature =
        signature;

    s_thumb_catalog_signature_valid =
        true;
}

static png_voidp web_png_malloc(
    png_structp png_ptr,
    png_alloc_size_t size)
{
    (void)png_ptr;

    return heap_caps_malloc_prefer(
        (size_t)size,
        2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
}

static void web_png_free(
    png_structp png_ptr,
    png_voidp ptr)
{
    (void)png_ptr;
    heap_caps_free(ptr);
}

static esp_err_t write_thumbnail_png(
    const char *path,
    const uint16_t *pixels)
{
    if (!path ||
        !pixels) {
        return ESP_ERR_INVALID_ARG;
    }

    char temp_path[768];

    int written =
        snprintf(
            temp_path,
            sizeof(temp_path),
            "%s.tmp",
            path
        );

    if (written < 0 ||
        written >= (int)sizeof(temp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file =
        fopen(
            temp_path,
            "wb"
        );

    if (!file) {
        return ESP_FAIL;
    }

    png_structp png =
        png_create_write_struct_2(
            PNG_LIBPNG_VER_STRING,
            NULL,
            NULL,
            NULL,
            NULL,
            web_png_malloc,
            web_png_free
        );

    if (!png) {
        fclose(file);
        remove(temp_path);
        return ESP_ERR_NO_MEM;
    }

    png_infop info =
        png_create_info_struct(
            png
        );

    if (!info) {
        png_destroy_write_struct(
            &png,
            NULL
        );

        fclose(file);
        remove(temp_path);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *row =
        (uint8_t *)heap_caps_malloc_prefer(
            TYPEN_THUMB_W * 3U,
            2,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (!row) {
        png_destroy_write_struct(
            &png,
            &info
        );

        fclose(file);
        remove(temp_path);
        return ESP_ERR_NO_MEM;
    }

    if (setjmp(png_jmpbuf(png))) {
        heap_caps_free(row);

        png_destroy_write_struct(
            &png,
            &info
        );

        fclose(file);
        remove(temp_path);
        return ESP_FAIL;
    }

    png_init_io(
        png,
        file
    );

    /*
     * Small thumbnails favor fast generation over maximum compression.
     * Level 3 keeps transfer size low without spending much CPU time.
     */
    png_set_compression_level(
        png,
        3
    );

    png_set_IHDR(
        png,
        info,
        TYPEN_THUMB_W,
        TYPEN_THUMB_H,
        8,
        PNG_COLOR_TYPE_RGB,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );

    png_write_info(
        png,
        info
    );

    for (int y = 0;
         y < TYPEN_THUMB_H;
         ++y) {
        const uint16_t *src =
            pixels +
            y * TYPEN_THUMB_W;

        for (int x = 0;
             x < TYPEN_THUMB_W;
             ++x) {
            const uint16_t p =
                src[x];

            const uint8_t r5 =
                (uint8_t)((p >> 11) & 0x1FU);

            const uint8_t g6 =
                (uint8_t)((p >> 5) & 0x3FU);

            const uint8_t b5 =
                (uint8_t)(p & 0x1FU);

            row[x * 3 + 0] =
                (uint8_t)((r5 << 3) |
                          (r5 >> 2));

            row[x * 3 + 1] =
                (uint8_t)((g6 << 2) |
                          (g6 >> 4));

            row[x * 3 + 2] =
                (uint8_t)((b5 << 3) |
                          (b5 >> 2));
        }

        png_write_row(
            png,
            row
        );

        if ((y & 7) == 0) {
            taskYIELD();
        }
    }

    png_write_end(
        png,
        info
    );

    heap_caps_free(row);

    png_destroy_write_struct(
        &png,
        &info
    );

    fflush(file);
    fclose(file);

    remove(path);

    if (rename(
            temp_path,
            path) != 0) {
        remove(temp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t generate_thumbnail_file(
    const typen_media_item_t *item,
    const char *cache_path)
{
    uint16_t *pixels =
        (uint16_t *)heap_caps_malloc(
            TYPEN_THUMB_W *
            TYPEN_THUMB_H *
            sizeof(uint16_t),
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!pixels) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err =
        typen_thumb_render_item(
            item,
            pixels
        );

    if (err == ESP_OK) {
        err =
            write_thumbnail_png(
                cache_path,
                pixels
            );
    }

    heap_caps_free(pixels);
    return err;
}

static const char *media_content_type(
    typen_media_type_t type)
{
    switch (type) {
        case TYPEN_MEDIA_JPEG:
            return "image/jpeg";
        case TYPEN_MEDIA_PNG:
            return "image/png";
        case TYPEN_MEDIA_GIF:
            return "image/gif";
        default:
            return "application/octet-stream";
    }
}

static esp_err_t send_binary_file(
    httpd_req_t *req,
    const char *path,
    const char *content_type,
    const char *cache_control)
{
    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    httpd_resp_set_type(
        req,
        content_type
    );

    if (cache_control) {
        httpd_resp_set_hdr(
            req,
            "Cache-Control",
            cache_control
        );
    }

    uint8_t buffer[2048];

    for (;;) {
        size_t got =
            fread(
                buffer,
                1,
                sizeof(buffer),
                file
            );

        if (got > 0) {
            esp_err_t err =
                httpd_resp_send_chunk(
                    req,
                    (const char *)buffer,
                    got
                );

            if (err != ESP_OK) {
                fclose(file);
                return err;
            }
        }

        if (got < sizeof(buffer)) {
            if (ferror(file)) {
                fclose(file);
                return ESP_FAIL;
            }

            break;
        }
    }

    fclose(file);

    return httpd_resp_send_chunk(
        req,
        NULL,
        0
    );
}

static esp_err_t send_thumbnail_fallback(
    httpd_req_t *req,
    const typen_media_item_t *item)
{
    char svg[512];

    int written =
        snprintf(
            svg,
            sizeof(svg),
            "<svg xmlns='http://www.w3.org/2000/svg' width='196' height='88' viewBox='0 0 196 88'>"
            "<rect width='196' height='88' rx='10' fill='#0b100d'/>"
            "<rect x='1' y='1' width='194' height='86' rx='9' fill='none' stroke='#294033'/>"
            "<text x='98' y='50' text-anchor='middle' font-family='sans-serif' font-size='16' font-weight='700' fill='#8fe4ab'>%s</text>"
            "</svg>",
            typen_media_type_name(
                item->type
            )
        );

    if (written < 0 ||
        written >= (int)sizeof(svg)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(
        req,
        "image/svg+xml"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        req,
        svg,
        HTTPD_RESP_USE_STRLEN
    );
}


static esp_err_t query_value(
    httpd_req_t *req,
    const char *key,
    char *value,
    size_t value_len)
{
    size_t qlen =
        httpd_req_get_url_query_len(req) + 1U;

    if (qlen <= 1U ||
        qlen > 768U) {
        return ESP_ERR_INVALID_ARG;
    }

    char *query =
        malloc(qlen);

    if (!query) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err =
        httpd_req_get_url_query_str(
            req,
            query,
            qlen
        );

    if (err == ESP_OK) {
        err =
            httpd_query_key_value(
                query,
                key,
                value,
                value_len
            );
    }

    free(query);

    if (err == ESP_OK) {
        url_decode(value);
    }

    return err;
}

static esp_err_t query_name(
    httpd_req_t *req,
    char *name,
    size_t name_len)
{
    esp_err_t err =
        query_value(
            req,
            "name",
            name,
            name_len
        );

    if (err != ESP_OK) {
        return err;
    }

    return safe_filename(name)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t query_path(
    httpd_req_t *req,
    char *path,
    size_t path_len)
{
    esp_err_t err =
        query_value(
            req,
            "path",
            path,
            path_len
        );

    if (err != ESP_OK) {
        return err;
    }

    if (!safe_sd_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *base =
        strrchr(
            path,
            '/'
        );

    base =
        base ? base + 1 : path;

    return safe_filename(base)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}


static esp_err_t query_firmware_path(
    httpd_req_t *req,
    char *path,
    size_t path_len)
{
    esp_err_t err =
        query_value(
            req,
            "path",
            path,
            path_len
        );

    if (err != ESP_OK ||
        !safe_sd_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *base =
        strrchr(
            path,
            '/'
        );

    base =
        base ? base + 1 : path;

    if (!base ||
        !*base ||
        strstr(base, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *ext =
        strrchr(
            base,
            '.'
        );

    if (!ext ||
        strcasecmp(
            ext,
            ".bin") != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}


static esp_err_t read_request_body(
    httpd_req_t *req,
    char *buffer,
    size_t buffer_size)
{
    if (!req ||
        !buffer ||
        buffer_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len <= 0 ||
        req->content_len >= (int)buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    int received = 0;

    while (received < req->content_len) {
        int got =
            httpd_req_recv(
                req,
                buffer + received,
                req->content_len - received
            );

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (got <= 0) {
            return ESP_FAIL;
        }

        received += got;
    }

    buffer[received] = '\0';
    return ESP_OK;
}

static esp_err_t read_path_body(
    httpd_req_t *req,
    char *path,
    size_t path_len)
{
    esp_err_t err =
        read_request_body(
            req,
            path,
            path_len
        );

    if (err != ESP_OK) {
        return err;
    }

    return safe_sd_path(path)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static bool form_value(
    const char *body,
    const char *key,
    char *value,
    size_t value_size)
{
    if (httpd_query_key_value(
            body,
            key,
            value,
            value_size) != ESP_OK) {
        return false;
    }

    url_decode(value);
    return true;
}

static esp_err_t root_get(
    httpd_req_t *req)
{
    httpd_resp_set_type(
        req,
        "text/html; charset=utf-8"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store, no-cache, must-revalidate"
    );

    return httpd_resp_send(
        req,
        INDEX_HTML,
        HTTPD_RESP_USE_STRLEN
    );
}

static esp_err_t captive_404(
    httpd_req_t *req,
    httpd_err_code_t error)
{
    (void)error;

    httpd_resp_set_status(
        req,
        "303 See Other"
    );

    httpd_resp_set_hdr(
        req,
        "Location",
        "/"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    httpd_resp_set_type(
        req,
        "text/plain"
    );

    return httpd_resp_sendstr(
        req,
        "Open TypeN"
    );
}

static void send_json_string(
    httpd_req_t *req,
    const char *s)
{
    httpd_resp_send_chunk(
        req,
        "\"",
        1
    );

    for (const char *p = s ? s : ""; *p; ++p) {
        char out[2] = {*p, 0};

        switch (*p) {
            case '"':
            case '\\':
                httpd_resp_send_chunk(
                    req,
                    "\\",
                    1
                );

                httpd_resp_send_chunk(
                    req,
                    out,
                    1
                );
                break;

            case '\n':
                httpd_resp_send_chunk(
                    req,
                    "\\n",
                    2
                );
                break;

            case '\r':
                httpd_resp_send_chunk(
                    req,
                    "\\r",
                    2
                );
                break;

            case '\t':
                httpd_resp_send_chunk(
                    req,
                    "\\t",
                    2
                );
                break;

            default:
                if ((unsigned char)*p >= 32) {
                    httpd_resp_send_chunk(
                        req,
                        out,
                        1
                    );
                }
                break;
        }
    }

    httpd_resp_send_chunk(
        req,
        "\"",
        1
    );
}

static esp_err_t send_settings_json(
    httpd_req_t *req)
{
    char selected[TYPEN_SETTINGS_PATH_MAX] = {0};
    char upload[TYPEN_SETTINGS_PATH_MAX] = {0};
    char thumbnail[TYPEN_SETTINGS_PATH_MAX] = {0};
    uint8_t brightness = TYPEN_DEFAULT_BRIGHTNESS;

    if (typen_settings_get_selected_media(
            selected,
            sizeof(selected)) != ESP_OK) {
        selected[0] = '\0';
    }

    if (typen_settings_get_upload_path(
            upload,
            sizeof(upload)) != ESP_OK) {
        snprintf(
            upload,
            sizeof(upload),
            "%s",
            TYPEN_DEFAULT_UPLOAD_PATH
        );
    }

    if (typen_settings_get_thumbnail_path(
            thumbnail,
            sizeof(thumbnail)) != ESP_OK) {
        snprintf(
            thumbnail,
            sizeof(thumbnail),
            "%s",
            TYPEN_DEFAULT_THUMBNAIL_PATH
        );
    }

    if (typen_settings_get_brightness(
            &brightness) != ESP_OK) {
        brightness =
            TYPEN_DEFAULT_BRIGHTNESS;
    }

    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    httpd_resp_send_chunk(
        req,
        "{\"uploadPath\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        upload
    );

    httpd_resp_send_chunk(
        req,
        ",\"thumbnailPath\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        thumbnail
    );

    httpd_resp_send_chunk(
        req,
        ",\"selectedPath\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        selected
    );

    char tail[64];

    snprintf(
        tail,
        sizeof(tail),
        ",\"brightness\":%u}",
        (unsigned)brightness
    );

    httpd_resp_send_chunk(
        req,
        tail,
        HTTPD_RESP_USE_STRLEN
    );

    return httpd_resp_send_chunk(
        req,
        NULL,
        0
    );
}

static esp_err_t info_get(
    httpd_req_t *req)
{
    char selected[TYPEN_SETTINGS_PATH_MAX] = {0};
    char upload[TYPEN_SETTINGS_PATH_MAX] = {0};
    char thumbnail[TYPEN_SETTINGS_PATH_MAX] = {0};
    uint8_t brightness = TYPEN_DEFAULT_BRIGHTNESS;

    if (typen_settings_get_selected_media(
            selected,
            sizeof(selected)) != ESP_OK) {
        selected[0] = '\0';
    }

    if (typen_settings_get_upload_path(
            upload,
            sizeof(upload)) != ESP_OK) {
        snprintf(
            upload,
            sizeof(upload),
            "%s",
            TYPEN_DEFAULT_UPLOAD_PATH
        );
    }

    if (typen_settings_get_thumbnail_path(
            thumbnail,
            sizeof(thumbnail)) != ESP_OK) {
        snprintf(
            thumbnail,
            sizeof(thumbnail),
            "%s",
            TYPEN_DEFAULT_THUMBNAIL_PATH
        );
    }

    if (typen_settings_get_brightness(
            &brightness) != ESP_OK) {
        brightness =
            typen_ui_get_brightness();
    }

    const esp_app_desc_t *app_desc =
        esp_app_get_description();

    typen_update_status_t update_status;
    memset(
        &update_status,
        0,
        sizeof(update_status)
    );

    typen_update_get_status(
        &update_status
    );

    char build_id[80];

    if (app_desc) {
        snprintf(
            build_id,
            sizeof(build_id),
            "%s %s",
            app_desc->date,
            app_desc->time
        );
    } else {
        snprintf(
            build_id,
            sizeof(build_id),
            "%s",
            "unknown"
        );
    }

    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    httpd_resp_send_chunk(
        req,
        "{\"ip\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        typen_wifi_ip_string()
    );

    httpd_resp_send_chunk(
        req,
        ",\"uploadPath\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        upload
    );

    httpd_resp_send_chunk(
        req,
        ",\"thumbnailPath\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        thumbnail
    );

    httpd_resp_send_chunk(
        req,
        ",\"selectedPath\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        selected
    );

    httpd_resp_send_chunk(
        req,
        ",\"version\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        update_status.running_version
    );

    httpd_resp_send_chunk(
        req,
        ",\"project\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        app_desc ? app_desc->project_name : ""
    );

    httpd_resp_send_chunk(
        req,
        ",\"firmwareVariant\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        update_status.running_variant
    );

    char identity_tail[64];

    snprintf(
        identity_tail,
        sizeof(identity_tail),
        ",\"firmwareAbi\":%u",
        (unsigned)update_status.running_firmware_abi
    );

    httpd_resp_send_chunk(
        req,
        identity_tail,
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        ",\"build\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        build_id
    );

    httpd_resp_send_chunk(
        req,
        ",\"idfVersion\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        esp_get_idf_version()
    );

    httpd_resp_send_chunk(
        req,
        ",\"runningSlot\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        update_status.running_partition
    );

    httpd_resp_send_chunk(
        req,
        ",\"imageState\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        update_status.image_state
    );

    char tail[128];

    snprintf(
        tail,
        sizeof(tail),
        ",\"brightness\":%u,\"mediaCount\":%u,\"easterEggActive\":%s}",
        (unsigned)brightness,
        (unsigned)typen_media_count(),
        typen_ui_easter_egg_active()
            ? "true"
            : "false"
    );

    httpd_resp_send_chunk(
        req,
        tail,
        HTTPD_RESP_USE_STRLEN
    );

    return httpd_resp_send_chunk(
        req,
        NULL,
        0
    );
}

static esp_err_t easter_egg_start_post(
    httpd_req_t *req)
{
    esp_err_t err =
        typen_ui_easter_egg_start();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Web Easter egg start failed: %s",
            esp_err_to_name(err)
        );

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Unable to start Easter egg"
        );
    }

    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_sendstr(
        req,
        "{\"active\":true}"
    );
}

static esp_err_t easter_egg_exit_post(
    httpd_req_t *req)
{
    esp_err_t err =
        typen_ui_easter_egg_exit();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Web Easter egg exit failed: %s",
            esp_err_to_name(err)
        );

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Unable to exit Easter egg"
        );
    }

    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_sendstr(
        req,
        "{\"active\":false}"
    );
}

static esp_err_t settings_get(
    httpd_req_t *req)
{
    return send_settings_json(req);
}

static esp_err_t settings_post(
    httpd_req_t *req)
{
    char body[1400];

    esp_err_t err =
        read_request_body(
            req,
            body,
            sizeof(body)
        );

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid settings body"
        );
    }

    bool changed = false;

    char upload[TYPEN_SETTINGS_PATH_MAX];

    if (form_value(
            body,
            "uploadPath",
            upload,
            sizeof(upload))) {
        if (!safe_sd_path(upload)) {
            return httpd_resp_send_err(
                req,
                HTTPD_400_BAD_REQUEST,
                "Upload path must stay on /sdcard"
            );
        }

        if (ensure_directory_tree(upload) != ESP_OK) {
            return httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Unable to create upload directory"
            );
        }

        err =
            typen_settings_set_upload_path(
                upload
            );

        if (err != ESP_OK) {
            return httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Unable to save upload path"
            );
        }

        changed = true;
    }

    char thumbnail[TYPEN_SETTINGS_PATH_MAX];

    if (form_value(
            body,
            "thumbnailPath",
            thumbnail,
            sizeof(thumbnail))) {
        if (!safe_sd_path(thumbnail)) {
            return httpd_resp_send_err(
                req,
                HTTPD_400_BAD_REQUEST,
                "Thumbnail path must stay on /sdcard"
            );
        }

        if (ensure_directory_tree(thumbnail) != ESP_OK) {
            return httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Unable to create thumbnail directory"
            );
        }

        err =
            typen_settings_set_thumbnail_path(
                thumbnail
            );

        if (err != ESP_OK) {
            return httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Unable to save thumbnail path"
            );
        }

        changed = true;
    }

    char brightness_text[16];

    if (form_value(
            body,
            "brightness",
            brightness_text,
            sizeof(brightness_text))) {
        char *end = NULL;

        long value =
            strtol(
                brightness_text,
                &end,
                10
            );

        if (end == brightness_text ||
            *end != '\0' ||
            value < TYPEN_BRIGHTNESS_MIN ||
            value > TYPEN_BRIGHTNESS_MAX) {
            return httpd_resp_send_err(
                req,
                HTTPD_400_BAD_REQUEST,
                "Brightness must be 5-100"
            );
        }

        err =
            typen_ui_set_brightness(
                (uint8_t)value
            );

        if (err != ESP_OK) {
            return httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Unable to apply brightness"
            );
        }

        changed = true;
    }

    if (!changed) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "No supported setting supplied"
        );
    }

    return httpd_resp_sendstr(
        req,
        "Settings saved"
    );
}

static esp_err_t thumb_get(
    httpd_req_t *req)
{
    char media_path[TYPEN_SETTINGS_PATH_MAX];

    esp_err_t err =
        query_value(
            req,
            "path",
            media_path,
            sizeof(media_path)
        );

    if (err != ESP_OK ||
        !safe_sd_path(media_path)) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid thumbnail path"
        );
    }

    typen_media_item_t item;

    if (typen_media_find(
            media_path,
            &item) != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_404_NOT_FOUND,
            "Media not found"
        );
    }

    char cache_path[768];

    err =
        thumbnail_cache_path(
            &item,
            cache_path,
            sizeof(cache_path),
            NULL
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Thumbnail path failed for %s: %s",
            item.path,
            esp_err_to_name(err)
        );

        return send_thumbnail_fallback(
            req,
            &item
        );
    }

    struct stat st;

    if (stat(
            cache_path,
            &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size <= 0) {

        ESP_LOGI(
            TAG,
            "Generate web thumbnail: %s",
            item.path
        );

        err =
            generate_thumbnail_file(
                &item,
                cache_path
            );

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Web thumbnail failed for %s: %s",
                item.path,
                esp_err_to_name(err)
            );

            /*
             * Some valid JPEGs are rejected by the ESP32-P4 hardware JPEG
             * decoder when their dimensions are not MCU/8 aligned. Preserve
             * the original media and let the browser scale it as a fallback.
             */
            if (item.type == TYPEN_MEDIA_JPEG) {
                esp_err_t source_err =
                    send_binary_file(
                        req,
                        item.path,
                        media_content_type(item.type),
                        "public, max-age=86400"
                    );

                if (source_err == ESP_OK) {
                    return ESP_OK;
                }
            }

            return send_thumbnail_fallback(
                req,
                &item
            );
        }
    }

    return send_binary_file(
        req,
        cache_path,
        "image/png",
        "public, max-age=31536000, immutable"
    );
}


static esp_err_t send_conflict(
    httpd_req_t *req,
    const char *message)
{
    httpd_resp_set_status(
        req,
        "409 Conflict"
    );

    httpd_resp_set_type(
        req,
        "text/plain"
    );

    return httpd_resp_sendstr(
        req,
        message
    );
}

static esp_err_t update_sd_check_get(
    httpd_req_t *req)
{
    char path[TYPEN_UPDATE_PATH_MAX];

    esp_err_t path_err =
        query_firmware_path(
            req,
            path,
            sizeof(path)
        );

    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    typen_update_sd_info_t info;
    memset(
        &info,
        0,
        sizeof(info)
    );

    esp_err_t err =
        path_err;

    if (err == ESP_OK) {
        err =
            typen_update_check_sd(
                path,
                &info
            );
    } else {
        snprintf(
            info.message,
            sizeof(info.message),
            "%s",
            "Firmware path must point to a .bin file inside /sdcard"
        );
    }

    httpd_resp_send_chunk(
        req,
        "{\"valid\":",
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        err == ESP_OK
            ? "true"
            : "false",
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        ",\"message\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.message
    );

    httpd_resp_send_chunk(
        req,
        ",\"version\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.version
    );

    httpd_resp_send_chunk(
        req,
        ",\"project\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.project_name
    );

    httpd_resp_send_chunk(
        req,
        ",\"variant\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.variant
    );

    char identity_tail[64];

    snprintf(
        identity_tail,
        sizeof(identity_tail),
        ",\"abi\":%u",
        (unsigned)info.firmware_abi
    );

    httpd_resp_send_chunk(
        req,
        identity_tail,
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        ",\"running\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.running_partition
    );

    httpd_resp_send_chunk(
        req,
        ",\"target\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.target_partition
    );

    char tail[96];

    snprintf(
        tail,
        sizeof(tail),
        ",\"size\":%llu}",
        (unsigned long long)info.image_size
    );

    httpd_resp_send_chunk(
        req,
        tail,
        HTTPD_RESP_USE_STRLEN
    );

    return httpd_resp_send_chunk(
        req,
        NULL,
        0
    );
}

static esp_err_t update_status_get(
    httpd_req_t *req)
{
    typen_update_status_t status;

    typen_update_get_status(
        &status
    );

    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    httpd_resp_send_chunk(
        req,
        "{\"state\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        typen_update_state_name(
            status.state
        )
    );

#define SEND_STATUS_STRING(key, value)                   \
    do {                                                  \
        httpd_resp_send_chunk(                            \
            req,                                          \
            ",\"" key "\":",                             \
            HTTPD_RESP_USE_STRLEN                         \
        );                                                \
        send_json_string(                                 \
            req,                                          \
            value                                         \
        );                                                \
    } while (0)

    SEND_STATUS_STRING(
        "running",
        status.running_partition
    );

    SEND_STATUS_STRING(
        "boot",
        status.boot_partition
    );

    SEND_STATUS_STRING(
        "target",
        status.target_partition
    );

    SEND_STATUS_STRING(
        "runningVersion",
        status.running_version
    );

    SEND_STATUS_STRING(
        "runningProject",
        status.running_project
    );

    SEND_STATUS_STRING(
        "runningVariant",
        status.running_variant
    );

    SEND_STATUS_STRING(
        "targetVersion",
        status.target_version
    );

    SEND_STATUS_STRING(
        "targetVariant",
        status.target_variant
    );

    SEND_STATUS_STRING(
        "imageState",
        status.image_state
    );

    SEND_STATUS_STRING(
        "lastResult",
        typen_update_result_name(
            status.last_result
        )
    );

    SEND_STATUS_STRING(
        "lastVersion",
        status.last_version
    );

    SEND_STATUS_STRING(
        "lastPartition",
        status.last_partition
    );

    SEND_STATUS_STRING(
        "invalidVersion",
        status.invalid_version
    );

    SEND_STATUS_STRING(
        "invalidPartition",
        status.invalid_partition
    );

    SEND_STATUS_STRING(
        "message",
        status.message
    );

#undef SEND_STATUS_STRING

    char tail[256];

    snprintf(
        tail,
        sizeof(tail),
        ",\"runningAbi\":%u,\"targetAbi\":%u,\"total\":%llu,\"written\":%llu,\"reboot\":%s,\"rollbackPossible\":%s}",
        (unsigned)status.running_firmware_abi,
        (unsigned)status.target_firmware_abi,
        (unsigned long long)status.total_bytes,
        (unsigned long long)status.written_bytes,
        status.reboot_scheduled
            ? "true"
            : "false",
        status.rollback_possible
            ? "true"
            : "false"
    );

    httpd_resp_send_chunk(
        req,
        tail,
        HTTPD_RESP_USE_STRLEN
    );

    return httpd_resp_send_chunk(
        req,
        NULL,
        0
    );
}

static esp_err_t update_upload_check_post(
    httpd_req_t *req)
{
    char size_text[32];

    esp_err_t err =
        query_value(
            req,
            "size",
            size_text,
            sizeof(size_text)
        );

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Missing firmware size"
        );
    }

    char *end = NULL;

    unsigned long long parsed_size =
        strtoull(
            size_text,
            &end,
            10
        );

    if (end == size_text ||
        *end != '\0' ||
        parsed_size == 0 ||
        parsed_size > SIZE_MAX) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid firmware size"
        );
    }

    if (req->content_len <= 0 ||
        req->content_len >
            BROWSER_UPDATE_PREFIX_BYTES) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid firmware preflight data"
        );
    }

    uint8_t prefix[
        BROWSER_UPDATE_PREFIX_BYTES
    ];

    size_t received = 0;
    unsigned consecutive_timeouts = 0;

    while (received <
           (size_t)req->content_len) {
        const int got =
            httpd_req_recv(
                req,
                (char *)prefix +
                    received,
                (size_t)req->content_len -
                    received
            );

        if (got ==
            HTTPD_SOCK_ERR_TIMEOUT) {
            ++consecutive_timeouts;

            if (consecutive_timeouts >=
                BROWSER_UPDATE_MAX_CONSECUTIVE_TIMEOUTS) {
                return httpd_resp_send_err(
                    req,
                    HTTPD_408_REQ_TIMEOUT,
                    "Firmware preflight timed out"
                );
            }

            continue;
        }

        if (got <= 0) {
            return ESP_FAIL;
        }

        consecutive_timeouts = 0;

        received +=
            (size_t)got;
    }

    typen_update_image_info_t info;

    err =
        typen_update_check_image_prefix(
            prefix,
            received,
            (size_t)parsed_size,
            &info
        );

    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    httpd_resp_send_chunk(
        req,
        "{\"valid\":",
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        err == ESP_OK
            ? "true"
            : "false",
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        ",\"message\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.message
    );

    httpd_resp_send_chunk(
        req,
        ",\"version\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.version
    );

    httpd_resp_send_chunk(
        req,
        ",\"project\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.project_name
    );

    httpd_resp_send_chunk(
        req,
        ",\"variant\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.variant
    );

    char identity_tail[64];

    snprintf(
        identity_tail,
        sizeof(identity_tail),
        ",\"abi\":%u",
        (unsigned)info.firmware_abi
    );

    httpd_resp_send_chunk(
        req,
        identity_tail,
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        ",\"running\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.running_partition
    );

    httpd_resp_send_chunk(
        req,
        ",\"target\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        info.target_partition
    );

    char tail[96];

    snprintf(
        tail,
        sizeof(tail),
        ",\"size\":%llu}",
        (unsigned long long)info.image_size
    );

    httpd_resp_send_chunk(
        req,
        tail,
        HTTPD_RESP_USE_STRLEN
    );

    return httpd_resp_send_chunk(
        req,
        NULL,
        0
    );
}

static esp_err_t update_upload_post(
    httpd_req_t *req)
{
    if (req->content_len <= 0) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Firmware image is empty"
        );
    }

    const size_t total_size =
        (size_t)req->content_len;

    const size_t prefix_target =
        total_size <
            BROWSER_UPDATE_PREFIX_BYTES
            ? total_size
            : BROWSER_UPDATE_PREFIX_BYTES;

    uint8_t prefix[
        BROWSER_UPDATE_PREFIX_BYTES
    ];

    size_t prefix_received = 0;
    unsigned consecutive_timeouts = 0;

    while (prefix_received <
           prefix_target) {
        const int got =
            httpd_req_recv(
                req,
                (char *)prefix +
                    prefix_received,
                prefix_target -
                    prefix_received
            );

        if (got ==
            HTTPD_SOCK_ERR_TIMEOUT) {
            ++consecutive_timeouts;

            if (consecutive_timeouts >=
                BROWSER_UPDATE_MAX_CONSECUTIVE_TIMEOUTS) {
                return httpd_resp_send_err(
                    req,
                    HTTPD_408_REQ_TIMEOUT,
                    "Firmware upload timed out during preflight"
                );
            }

            continue;
        }

        if (got <= 0) {
            return ESP_FAIL;
        }

        consecutive_timeouts = 0;

        prefix_received +=
            (size_t)got;
    }

    typen_update_image_info_t info;

    esp_err_t err =
        typen_update_check_image_prefix(
            prefix,
            prefix_received,
            total_size,
            &info
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Browser OTA rejected before write: %s",
            info.message[0]
                ? info.message
                : esp_err_to_name(err)
        );

        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            info.message[0]
                ? info.message
                : "Firmware preflight failed"
        );
    }

    err =
        typen_update_upload_begin_checked(
            &info
        );

    if (err ==
        ESP_ERR_INVALID_STATE) {
        return send_conflict(
            req,
            "Another update is already running"
        );
    }

    if (err ==
        ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Firmware image does not fit OTA slot"
        );
    }

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Unable to prepare OTA update"
        );
    }

    err =
        typen_update_upload_write(
            prefix,
            prefix_received
        );

    if (err != ESP_OK) {
        typen_update_upload_abort();

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Firmware write failed"
        );
    }

    uint8_t *buffer =
        (uint8_t *)malloc(
            BROWSER_UPDATE_BUFFER_BYTES
        );

    if (!buffer) {
        typen_update_upload_abort();

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Unable to allocate update buffer"
        );
    }

    size_t remaining =
        total_size -
        prefix_received;

    consecutive_timeouts = 0;

    while (remaining > 0) {
        const size_t wanted =
            remaining <
                BROWSER_UPDATE_BUFFER_BYTES
                ? remaining
                : BROWSER_UPDATE_BUFFER_BYTES;

        const int got =
            httpd_req_recv(
                req,
                (char *)buffer,
                wanted
            );

        if (got ==
            HTTPD_SOCK_ERR_TIMEOUT) {
            ++consecutive_timeouts;

            if (consecutive_timeouts >=
                BROWSER_UPDATE_MAX_CONSECUTIVE_TIMEOUTS) {
                free(buffer);
                typen_update_upload_abort();

                return httpd_resp_send_err(
                    req,
                    HTTPD_408_REQ_TIMEOUT,
                    "Firmware upload timed out"
                );
            }

            continue;
        }

        if (got <= 0) {
            free(buffer);
            typen_update_upload_abort();
            return ESP_FAIL;
        }

        consecutive_timeouts = 0;

        err =
            typen_update_upload_write(
                buffer,
                (size_t)got
            );

        if (err != ESP_OK) {
            free(buffer);
            typen_update_upload_abort();

            return httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Firmware write failed"
            );
        }

        remaining -=
            (size_t)got;
    }

    free(buffer);

    err =
        typen_update_upload_finish();

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Firmware verification failed"
        );
    }

    ESP_LOGI(
        TAG,
        "Browser OTA accepted: project=%s version=%s size=%u target=%s",
        info.project_name[0]
            ? info.project_name
            : "unknown",
        info.version[0]
            ? info.version
            : "unknown",
        (unsigned)info.image_size,
        info.target_partition
    );

    return httpd_resp_sendstr(
        req,
        "Firmware verified; reboot scheduled"
    );
}

static esp_err_t update_sd_post(
    httpd_req_t *req)
{
    char path[TYPEN_UPDATE_PATH_MAX];

    esp_err_t err =
        query_firmware_path(
            req,
            path,
            sizeof(path)
        );

    /*
     * Backward compatibility with the older form-body request.
     */
    if (err != ESP_OK &&
        req->content_len > 0) {
        char body[768];

        err =
            read_request_body(
                req,
                body,
                sizeof(body)
            );

        if (err == ESP_OK &&
            form_value(
                body,
                "path",
                path,
                sizeof(path))) {
            if (!safe_sd_path(path)) {
                err =
                    ESP_ERR_INVALID_ARG;
            } else {
                const char *ext =
                    strrchr(
                        path,
                        '.'
                    );

                err =
                    (ext &&
                     strcasecmp(
                         ext,
                         ".bin") == 0)
                        ? ESP_OK
                        : ESP_ERR_INVALID_ARG;
            }
        }
    }

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Firmware path must point to a .bin file inside /sdcard"
        );
    }

    err =
        typen_update_start_sd(
            path
        );

    if (err != ESP_OK) {
        typen_update_status_t status;

        typen_update_get_status(
            &status
        );

        const char *message =
            status.message[0]
                ? status.message
                : "Unable to start SD update";

        if (err ==
            ESP_ERR_INVALID_STATE) {
            return send_conflict(
                req,
                message
            );
        }

        if (err ==
            ESP_ERR_NOT_FOUND) {
            return httpd_resp_send_err(
                req,
                HTTPD_404_NOT_FOUND,
                message
            );
        }

        if (err ==
                ESP_ERR_INVALID_ARG ||
            err ==
                ESP_ERR_INVALID_SIZE) {
            return httpd_resp_send_err(
                req,
                HTTPD_400_BAD_REQUEST,
                message
            );
        }

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            message
        );
    }

    return httpd_resp_sendstr(
        req,
        "SD firmware validated; update started"
    );
}


static esp_err_t transfer_status_get(
    httpd_req_t *req)
{
    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    httpd_resp_send_chunk(
        req,
        "{\"valid\":",
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        s_last_transfer.valid
            ? "true"
            : "false",
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        ",\"success\":",
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        s_last_transfer.success
            ? "true"
            : "false",
        HTTPD_RESP_USE_STRLEN
    );

    httpd_resp_send_chunk(
        req,
        ",\"filename\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        s_last_transfer.filename
    );

    httpd_resp_send_chunk(
        req,
        ",\"message\":",
        HTTPD_RESP_USE_STRLEN
    );

    send_json_string(
        req,
        s_last_transfer.message
    );

    char tail[192];

    snprintf(
        tail,
        sizeof(tail),
        ",\"bytes\":%llu,\"elapsedMs\":%u,\"kibPerSecond\":%u,\"recvTimeouts\":%u}",
        (unsigned long long)s_last_transfer.bytes_received,
        (unsigned)s_last_transfer.elapsed_ms,
        (unsigned)s_last_transfer.kib_per_second,
        s_last_transfer.recv_timeouts
    );

    httpd_resp_send_chunk(
        req,
        tail,
        HTTPD_RESP_USE_STRLEN
    );

    return httpd_resp_send_chunk(
        req,
        NULL,
        0
    );
}

static esp_err_t media_get(
    httpd_req_t *req)
{
    esp_err_t scan_err =
        typen_media_scan();

    if (scan_err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Media scan failed"
        );
    }

    sync_thumbnail_cache_after_scan();

    char selected[TYPEN_SETTINGS_PATH_MAX] = {0};

    if (typen_settings_get_selected_media(
            selected,
            sizeof(selected)) != ESP_OK) {
        selected[0] = '\0';
    }

    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    httpd_resp_send_chunk(
        req,
        "[",
        1
    );

    size_t count =
        typen_media_count();

    bool first = true;

    for (size_t i = 0;
         i < count;
         ++i) {
        typen_media_item_t item;

        if (typen_media_get(
                i,
                &item) != ESP_OK) {
            continue;
        }

        if (!first) {
            httpd_resp_send_chunk(
                req,
                ",",
                1
            );
        }

        first = false;

        httpd_resp_send_chunk(
            req,
            "{\"name\":",
            HTTPD_RESP_USE_STRLEN
        );

        send_json_string(
            req,
            item.name
        );

        httpd_resp_send_chunk(
            req,
            ",\"path\":",
            HTTPD_RESP_USE_STRLEN
        );

        send_json_string(
            req,
            item.path
        );

        httpd_resp_send_chunk(
            req,
            ",\"type\":",
            HTTPD_RESP_USE_STRLEN
        );

        send_json_string(
            req,
            typen_media_type_name(item.type)
        );

        char tail[160];

        snprintf(
            tail,
            sizeof(tail),
            ",\"size\":%u,\"thumbKey\":\"%016llx\",\"selected\":%s}",
            (unsigned)item.size,
            (unsigned long long)thumbnail_key(&item),
            (selected[0] &&
             strcmp(selected, item.path) == 0)
                ? "true"
                : "false"
        );

        httpd_resp_send_chunk(
            req,
            tail,
            HTTPD_RESP_USE_STRLEN
        );
    }

    httpd_resp_send_chunk(
        req,
        "]",
        1
    );

    return httpd_resp_send_chunk(
        req,
        NULL,
        0
    );
}

static esp_err_t upload_post(
    httpd_req_t *req)
{
    char name[180];

    if (query_name(
            req,
            name,
            sizeof(name)) != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid filename"
        );
    }

    if (req->content_len <= 0) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Upload is empty"
        );
    }

    char upload_dir[TYPEN_SETTINGS_PATH_MAX];

    if (typen_settings_get_upload_path(
            upload_dir,
            sizeof(upload_dir)) != ESP_OK) {
        snprintf(
            upload_dir,
            sizeof(upload_dir),
            "%s",
            TYPEN_DEFAULT_UPLOAD_PATH
        );
    }

    if (ensure_directory_tree(
            upload_dir) != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Upload directory unavailable"
        );
    }

    char path[768];

    int written =
        snprintf(
            path,
            sizeof(path),
            "%s/%s",
            upload_dir,
            name
        );

    if (written < 0 ||
        written >= (int)sizeof(path)) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Path too long"
        );
    }

    char temp_path[800];
    char backup_path[800];

    const int temp_written =
        snprintf(
            temp_path,
            sizeof(temp_path),
            "%s.typen-upload.part",
            path
        );

    const int backup_written =
        snprintf(
            backup_path,
            sizeof(backup_path),
            "%s.typen-upload.bak",
            path
        );

    if (temp_written < 0 ||
        temp_written >= (int)sizeof(temp_path) ||
        backup_written < 0 ||
        backup_written >= (int)sizeof(backup_path)) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Path too long"
        );
    }

    /*
     * Recover a prior interrupted commit before beginning a new replacement of
     * this same file. Temporary/backup suffixes are not media extensions, so
     * the media scanner never exposes them.
     */
    struct stat final_stat;
    struct stat backup_stat;

    const bool final_exists =
        stat(
            path,
            &final_stat
        ) == 0;

    const bool backup_exists =
        stat(
            backup_path,
            &backup_stat
        ) == 0;

    if (!final_exists &&
        backup_exists) {
        if (rename(
                backup_path,
                path) != 0) {
            ESP_LOGW(
                TAG,
                "Unable to restore interrupted upload backup %s errno=%d",
                backup_path,
                errno
            );
        }
    } else if (final_exists &&
               backup_exists) {
        (void)remove(
            backup_path
        );
    }

    (void)remove(
        temp_path
    );

    const int64_t upload_start_us =
        esp_timer_get_time();

    const size_t upload_total_bytes =
        (size_t)req->content_len;

    size_t bytes_received = 0;
    unsigned upload_timeouts = 0;
    unsigned consecutive_timeouts = 0;

    FILE *file =
        fopen(
            temp_path,
            "wb"
        );

    if (!file) {
        record_transfer_status(
            false,
            name,
            0,
            upload_start_us,
            0,
            "Unable to create the temporary upload file."
        );

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Open failed"
        );
    }

    uint8_t *buffer =
        (uint8_t *)malloc(
            MEDIA_UPLOAD_BUFFER_BYTES
        );

    if (!buffer) {
        fclose(file);
        remove(temp_path);

        record_transfer_status(
            false,
            name,
            0,
            upload_start_us,
            0,
            "Unable to allocate the upload buffer."
        );

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Upload buffer unavailable"
        );
    }

    size_t remaining =
        upload_total_bytes;

    while (remaining > 0) {
        const size_t wanted =
            remaining <
                MEDIA_UPLOAD_BUFFER_BYTES
                ? remaining
                : MEDIA_UPLOAD_BUFFER_BYTES;

        const int got =
            httpd_req_recv(
                req,
                (char *)buffer,
                wanted
            );

        if (got ==
            HTTPD_SOCK_ERR_TIMEOUT) {
            ++upload_timeouts;
            ++consecutive_timeouts;

            if (consecutive_timeouts >=
                MEDIA_UPLOAD_MAX_CONSECUTIVE_TIMEOUTS) {
                ESP_LOGW(
                    TAG,
                    "Upload timed out: %s bytes=%u/%u timeouts=%u",
                    name,
                    (unsigned)bytes_received,
                    (unsigned)upload_total_bytes,
                    upload_timeouts
                );

                free(buffer);
                fclose(file);
                remove(temp_path);

                record_transfer_status(
                    false,
                    name,
                    bytes_received,
                    upload_start_us,
                    upload_timeouts,
                    "Upload stopped after repeated network timeouts."
                );

                return httpd_resp_send_err(
                    req,
                    HTTPD_408_REQ_TIMEOUT,
                    "Upload timed out"
                );
            }

            continue;
        }

        if (got <= 0) {
            free(buffer);
            fclose(file);
            remove(temp_path);

            record_transfer_status(
                false,
                name,
                bytes_received,
                upload_start_us,
                upload_timeouts,
                "The upload connection closed before the file was complete."
            );

            ESP_LOGW(
                TAG,
                "Upload connection closed: %s bytes=%u/%u",
                name,
                (unsigned)bytes_received,
                (unsigned)upload_total_bytes
            );

            return ESP_FAIL;
        }

        consecutive_timeouts = 0;

        if (fwrite(
                buffer,
                1,
                (size_t)got,
                file) != (size_t)got) {
            free(buffer);
            fclose(file);
            remove(temp_path);

            record_transfer_status(
                false,
                name,
                bytes_received,
                upload_start_us,
                upload_timeouts,
                "Writing the upload to the SD card failed."
            );

            return httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Write failed"
            );
        }

        bytes_received +=
            (size_t)got;

        remaining -=
            (size_t)got;
    }

    free(buffer);

    if (fflush(file) != 0) {
        fclose(file);
        remove(temp_path);

        record_transfer_status(
            false,
            name,
            bytes_received,
            upload_start_us,
            upload_timeouts,
            "The SD card could not finalize the uploaded file."
        );

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Flush failed"
        );
    }

    fclose(file);

    /*
     * Commit only after the full request has reached SD successfully.
     *
     * If this replaces existing media, keep the old copy as a short-lived
     * backup until the new temporary file has been renamed into place.
     */
    struct stat current_stat;

    const bool replacing =
        stat(
            path,
            &current_stat
        ) == 0;

    if (replacing) {
        (void)remove(
            backup_path
        );

        if (rename(
                path,
                backup_path) != 0) {
            remove(temp_path);

            record_transfer_status(
                false,
                name,
                bytes_received,
                upload_start_us,
                upload_timeouts,
                "Unable to prepare the existing file for replacement."
            );

            return httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Replace preparation failed"
            );
        }
    }

    if (rename(
            temp_path,
            path) != 0) {
        const int commit_errno =
            errno;

        if (replacing) {
            (void)rename(
                backup_path,
                path
            );
        }

        remove(temp_path);

        ESP_LOGE(
            TAG,
            "Upload commit failed: %s errno=%d",
            path,
            commit_errno
        );

        record_transfer_status(
            false,
            name,
            bytes_received,
            upload_start_us,
            upload_timeouts,
            "The complete upload could not be committed to its final filename."
        );

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Upload commit failed"
        );
    }

    if (replacing) {
        (void)remove(
            backup_path
        );
    }

    record_transfer_status(
        true,
        name,
        bytes_received,
        upload_start_us,
        upload_timeouts,
        upload_timeouts == 0
            ? "Upload completed without receive timeouts."
            : "Upload completed successfully after transient network timeouts."
    );

    ESP_LOGI(
        TAG,
        "Uploaded %s | bytes=%u time=%u ms rate=%u KiB/s recv_timeouts=%u",
        path,
        (unsigned)s_last_transfer.bytes_received,
        (unsigned)s_last_transfer.elapsed_ms,
        (unsigned)s_last_transfer.kib_per_second,
        s_last_transfer.recv_timeouts
    );

    return httpd_resp_sendstr(
        req,
        "Upload complete"
    );
}

static esp_err_t select_post(
    httpd_req_t *req)
{
    char path[TYPEN_SETTINGS_PATH_MAX];

    if (read_path_body(
            req,
            path,
            sizeof(path)) != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid media path"
        );
    }

    typen_media_item_t item;

    if (typen_media_find(
            path,
            &item) != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_404_NOT_FOUND,
            "Media not found"
        );
    }

    if (s_select_cb) {
        s_select_cb(
            item.path,
            s_select_ctx
        );
    }

    ESP_LOGI(
        TAG,
        "Selected %s",
        item.path
    );

    return httpd_resp_sendstr(
        req,
        "Selected"
    );
}

static esp_err_t delete_post(
    httpd_req_t *req)
{
    char path[TYPEN_SETTINGS_PATH_MAX];

    esp_err_t path_err =
        query_path(
            req,
            path,
            sizeof(path)
        );

    if (path_err != ESP_OK) {
        path_err =
            read_path_body(
                req,
                path,
                sizeof(path)
            );
    }

    if (path_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Delete rejected: invalid path request"
        );

        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid media path"
        );
    }

    ESP_LOGI(
        TAG,
        "Delete request: %s",
        path
    );

    typen_media_item_t item;
    memset(
        &item,
        0,
        sizeof(item)
    );

    esp_err_t find_err =
        typen_media_find(
            path,
            &item
        );

    if (find_err != ESP_OK) {
        struct stat file_stat;

        if (stat(
                path,
                &file_stat) != 0 ||
            !S_ISREG(file_stat.st_mode)) {
            ESP_LOGW(
                TAG,
                "Delete target not found: %s",
                path
            );

            return httpd_resp_send_err(
                req,
                HTTPD_404_NOT_FOUND,
                "Media not found"
            );
        }

        /*
         * The browser may have rendered a catalog entry immediately before a
         * rescan. If the file still exists and is a supported media path,
         * delete the requested file directly rather than failing on a stale
         * catalog snapshot.
         */
        const size_t path_length =
            strlen(path);

        if (path_length >= sizeof(item.path)) {
            return httpd_resp_send_err(
                req,
                HTTPD_400_BAD_REQUEST,
                "Media path too long"
            );
        }

        memcpy(
            item.path,
            path,
            path_length + 1U
        );

        const char *base =
            strrchr(
                path,
                '/'
            );

        const char *base_name =
            base ? base + 1 : path;

        const size_t name_length =
            strlen(base_name);

        if (name_length >= sizeof(item.name)) {
            return httpd_resp_send_err(
                req,
                HTTPD_400_BAD_REQUEST,
                "Media filename too long"
            );
        }

        memcpy(
            item.name,
            base_name,
            name_length + 1U
        );

        item.size =
            (size_t)file_stat.st_size;
    }

    char cached_thumb[768] = {0};

    (void)thumbnail_cache_path(
        &item,
        cached_thumb,
        sizeof(cached_thumb),
        NULL
    );

    errno = 0;

    if (remove(item.path) != 0) {
        const int delete_errno = errno;

        ESP_LOGE(
            TAG,
            "Delete failed: %s errno=%d",
            item.path,
            delete_errno
        );

        char message[96];

        snprintf(
            message,
            sizeof(message),
            "Delete failed (errno %d)",
            delete_errno
        );

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            message
        );
    }

    if (cached_thumb[0] != '\0') {
        (void)remove(cached_thumb);
    }

    esp_err_t scan_err =
        typen_media_scan();

    if (scan_err == ESP_OK) {
        sync_thumbnail_cache_after_scan();
    } else {
        ESP_LOGW(
            TAG,
            "Media rescan after delete failed: %s",
            esp_err_to_name(scan_err)
        );
    }

    ESP_LOGI(
        TAG,
        "Deleted %s",
        item.path
    );

    return httpd_resp_sendstr(
        req,
        "Deleted"
    );
}

esp_err_t typen_web_start(
    typen_web_select_cb_t select_cb,
    void *ctx)
{
    s_select_cb = select_cb;
    s_select_ctx = ctx;

    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.max_uri_handlers = 18;
    config.stack_size = 10240;

    /*
     * TypeN uses CONFIG_LWIP_MAX_SOCKETS=10. Browsers can otherwise consume
     * the available descriptors with parallel thumbnail/API requests.
     */
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    /*
     * Hosted UART can briefly stall while TCP retransmits. Give active
     * requests enough time to recover, but use TCP keepalive to remove dead
     * browser sessions instead of allowing them to consume the small socket
     * budget indefinitely.
     */
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    ESP_RETURN_ON_ERROR(
        httpd_start(
            &s_server,
            &config
        ),
        TAG,
        "HTTP server start failed"
    );

    const httpd_uri_t handlers[] = {
        {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get
        },
        {
            .uri = "/api/info",
            .method = HTTP_GET,
            .handler = info_get
        },
        {
            .uri = "/api/easter-egg/start",
            .method = HTTP_POST,
            .handler = easter_egg_start_post
        },
        {
            .uri = "/api/easter-egg/exit",
            .method = HTTP_POST,
            .handler = easter_egg_exit_post
        },
        {
            .uri = "/api/settings",
            .method = HTTP_GET,
            .handler = settings_get
        },
        {
            .uri = "/api/settings",
            .method = HTTP_POST,
            .handler = settings_post
        },
        {
            .uri = "/api/update/status",
            .method = HTTP_GET,
            .handler = update_status_get
        },
        {
            .uri = "/api/update/upload/check",
            .method = HTTP_POST,
            .handler = update_upload_check_post
        },
        {
            .uri = "/api/update/upload",
            .method = HTTP_POST,
            .handler = update_upload_post
        },
        {
            .uri = "/api/update/sd/check",
            .method = HTTP_GET,
            .handler = update_sd_check_get
        },
        {
            .uri = "/api/update/sd",
            .method = HTTP_POST,
            .handler = update_sd_post
        },
        {
            .uri = "/api/transfer/status",
            .method = HTTP_GET,
            .handler = transfer_status_get
        },
        {
            .uri = "/api/media",
            .method = HTTP_GET,
            .handler = media_get
        },
        {
            .uri = "/api/thumb",
            .method = HTTP_GET,
            .handler = thumb_get
        },
        {
            .uri = "/api/upload",
            .method = HTTP_POST,
            .handler = upload_post
        },
        {
            .uri = "/api/select",
            .method = HTTP_POST,
            .handler = select_post
        },
        {
            .uri = "/api/delete",
            .method = HTTP_POST,
            .handler = delete_post
        },
    };

    for (size_t i = 0;
         i < sizeof(handlers) / sizeof(handlers[0]);
         ++i) {
        ESP_RETURN_ON_ERROR(
            httpd_register_uri_handler(
                s_server,
                &handlers[i]
            ),
            TAG,
            "URI registration failed"
        );
    }

    ESP_RETURN_ON_ERROR(
        httpd_register_err_handler(
            s_server,
            HTTPD_404_NOT_FOUND,
            captive_404
        ),
        TAG,
        "Captive redirect registration failed"
    );

    esp_err_t captive_err =
        typen_wifi_enable_captive_portal();

    if (captive_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Web manager is running, but captive portal enable failed: %s",
            esp_err_to_name(captive_err)
        );
    }

    ESP_LOGI(
        TAG,
        "TypeN control surface running on port 80"
    );

    return ESP_OK;
}
