/**
 * VipleStream logdrop：測試者按「上傳給開發者」時把報告存進部署者自己的 Google 雲端硬碟。
 *
 * 部署方式見同目錄 README.md。上傳不需要金鑰（網址本身會包進 app），所以這裡用每分鐘／每日上限、
 * 單檔大小上限擋濫用；讀取一律要 Script Properties 裡的 READ_KEY。
 *
 * 協定 v1（POST，Content-Type: application/json）：
 *   {
 *     "v": 1,
 *     "source": "android-glasses" | "server-windows" | "server-linux" | ...（[a-z0-9-]，最多 32 字）,
 *     "app_version": "2.0.277",
 *     "device": "Pixel 5 / Android 15",           // 選填，最多 200 字
 *     "install_id": "K3M9PQ7T",                   // 選填，匿名安裝 ID（[A-Z0-9]，6–16 字）；使用者在設定頁看得到，
 *                                                 // 回報問題時報這個 ID，開發者就能找到他自動上傳的所有報告
 *     "trigger": "manual" | "glasses-diag" | "session-end" | "crash" | ...（選填，[a-z0-9-]，最多 32 字）,
 *     "note": "測試者自己寫的說明",                  // 選填，最多 2000 字
 *     "encoding": "text" | "gzip-base64",
 *     "payload": "<報告本文，或 gzip 後的 base64>"
 *   }
 * 回應（POST 會先 302 到 googleusercontent.com，client 照常跟隨轉址改用 GET 取回）：
 *   {"ok": true, "code": "K7Q2MX"} 或 {"ok": false, "error": "<原因>"}
 *
 * 報告本文（payload）可以由 client 在最前面自己帶 `# summary: …` 與 `# exit: …` 兩行；
 * 收件端只負責搬運，不解析。內容像 API 錯誤 JSON 或空白的上傳會在檔頭標 `# invalid:`，
 * 檔名加 `_invalid`，`op=list` 也會回 `invalid: true`。
 *
 * 讀取（GET）：
 *   ?op=ping                          → {"ok":true,"service":"viplestream-logdrop","v":1}（不用金鑰）
 *   ?op=list&key=<READ_KEY>[&n=50][&id=<install_id>] → 最近的檔案清單（帶 id 只列該安裝的）
 *   ?op=get&key=<READ_KEY>&code=<代碼> → 該份報告的純文字
 */

var SERVICE = 'viplestream-logdrop';
var PROTOCOL = 1;
var MAX_REQUEST_BYTES = 20 * 1024 * 1024;   // 壓縮前後都不該超過；一般報告是幾百 KB
var MAX_TEXT_CHARS = 40 * 1024 * 1024;       // 解壓後的上限，避免壓縮炸彈塞滿雲端硬碟
var MAX_PER_MINUTE = 20;
var MAX_PER_DAY = 300;
var MAX_PER_INSTALL_PER_DAY = 20;  // 自動上傳的 client 端另有自己的上限；這裡擋單一來源灌爆每日總量
var TIME_ZONE = 'Asia/Taipei';
var CODE_ALPHABET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';  // 去掉 0/O、1/I/L，口頭傳代碼不會唸錯
var FOLDER_NAME = 'VipleStream logs';

function doPost(e) {
  try {
    if (!e || !e.postData || !e.postData.contents) {
      return json_({ok: false, error: 'empty'});
    }
    if (e.postData.length > MAX_REQUEST_BYTES) {
      return json_({ok: false, error: 'too_large'});
    }
    var req = JSON.parse(e.postData.contents);
    var installId = req.install_id === undefined || req.install_id === null ? '' : String(req.install_id);
    if (installId && !/^[A-Z0-9]{6,16}$/.test(installId)) {
      return json_({ok: false, error: 'bad_install_id'});
    }
    var trigger = req.trigger === undefined || req.trigger === null ? '' : String(req.trigger);
    if (trigger && !/^[a-z0-9-]{1,32}$/.test(trigger)) {
      return json_({ok: false, error: 'bad_trigger'});
    }
    var limited = takeQuota_(installId);
    if (limited) {
      return json_({ok: false, error: limited});
    }

    if (req.v !== PROTOCOL) {
      return json_({ok: false, error: 'bad_version'});
    }
    var source = String(req.source || '');
    if (!/^[a-z0-9-]{1,32}$/.test(source)) {
      return json_({ok: false, error: 'bad_source'});
    }
    var text = decodePayload_(req.encoding, req.payload);
    if (text === null) {
      return json_({ok: false, error: 'bad_payload'});
    }
    if (text.length > MAX_TEXT_CHARS) {
      return json_({ok: false, error: 'too_large'});
    }

    var now = new Date();
    var code = newCode_();
    var invalid = invalidReason_(text, req.app_version);
    var header = [
      '# VipleStream logdrop v' + PROTOCOL,
      '# code: ' + code,
      '# received: ' + Utilities.formatDate(now, TIME_ZONE, "yyyy-MM-dd'T'HH:mm:ssXXX"),
      '# source: ' + source,
      '# app_version: ' + clip_(req.app_version, 64),
      '# device: ' + clip_(req.device, 200),
      '# install_id: ' + installId,
      '# trigger: ' + trigger,
      '# note: ' + clip_(req.note, 2000).replace(/\r?\n/g, '\n#   ')
    ];
    if (invalid) {
      header.push('# invalid: ' + invalid);
    }
    header.push('');
    header = header.join('\n');
    var name = Utilities.formatDate(now, TIME_ZONE, 'yyyyMMdd-HHmmss') + '_' + source + '_' + (installId || 'anon') + '_' + code
        + (invalid ? '_invalid' : '') + '.txt';
    folder_().createFile(name, header + '\n' + text, MimeType.PLAIN_TEXT);
    return json_({ok: true, code: code});
  } catch (err) {
    // 例外字串可能含使用者資料以外的內部細節，不回給 client。
    console.error(err);
    return json_({ok: false, error: 'server_error'});
  }
}

function doGet(e) {
  var p = (e && e.parameter) || {};
  if (p.op === 'ping') {
    return json_({ok: true, service: SERVICE, v: PROTOCOL});
  }
  var readKey = PropertiesService.getScriptProperties().getProperty('READ_KEY');
  if (!readKey || p.key !== readKey) {
    return json_({ok: false, error: 'forbidden'});
  }
  if (p.op === 'list') {
    var n = Math.min(Math.max(parseInt(p.n, 10) || 50, 1), 500);
    var files = [];
    var id = String(p.id || '').toUpperCase();
    if (id && !/^[A-Z0-9]{6,16}$/.test(id)) {
      return json_({ok: false, error: 'bad_install_id'});
    }
    var it = id ? folder_().searchFiles("title contains '_" + id + "_'") : folder_().getFiles();
    while (it.hasNext()) {
      var f = it.next();
      var fileName = f.getName();
      files.push({
        name: fileName,
        size: f.getSize(),
        created: f.getDateCreated().toISOString(),
        invalid: /_invalid\.txt$/.test(fileName)
      });
    }
    files.sort(function (a, b) { return a.created < b.created ? 1 : -1; });
    return json_({ok: true, files: files.slice(0, n)});
  }
  if (p.op === 'get') {
    var code = String(p.code || '').toUpperCase();
    if (!/^[A-Z0-9]{4,12}$/.test(code)) {
      return json_({ok: false, error: 'bad_code'});
    }
    var found = folder_().searchFiles("title contains '_" + code + ".txt'");
    if (!found.hasNext()) {
      // §LOGDROP-1127：invalid 的檔名是 `_<code>_invalid.txt`，上面那個樣式找不到。
      found = folder_().searchFiles("title contains '_" + code + "_invalid.txt'");
    }
    if (!found.hasNext()) {
      return json_({ok: false, error: 'not_found'});
    }
    return ContentService.createTextOutput(found.next().getBlob().getDataAsString('UTF-8'))
        .setMimeType(ContentService.MimeType.TEXT);
  }
  return json_({ok: false, error: 'bad_op'});
}

function decodePayload_(encoding, payload) {
  if (typeof payload !== 'string') {
    return null;
  }
  if (encoding === 'text') {
    return payload;
  }
  if (encoding === 'gzip-base64') {
    var bytes = Utilities.base64Decode(payload);
    var blob = Utilities.newBlob(bytes, 'application/x-gzip', 'payload.gz');
    return Utilities.ungzip(blob).getDataAsString('UTF-8');
  }
  return null;
}

/**
 * §LOGDROP-1127：下載端曾把 `{"ok":false,...}` 這種 API 錯誤存成 log 檔（沒有檔頭、之後無法分析）。
 * 上傳內容若明顯不是報告，就在檔頭與清單標成 invalid；使用者仍拿得到代碼，開發者不必點開才知道。
 */
function invalidReason_(text, appVersion) {
  var t = text === null || text === undefined ? '' : String(text);
  if (t.length === 0) {
    return 'empty';
  }
  // 有些工具會在檔頭塞 BOM；先去掉再判斷是不是 JSON。用碼點比對，避免原始碼裡出現看不見的字元。
  if (t.charCodeAt(0) === 0xFEFF) {
    t = t.substring(1);
  }
  t = t.replace(/^\s+/, '');
  if (/^\{\s*"ok"\s*:/.test(t) || /^\{\s*"error"\s*:/.test(t)) {
    return 'api-error-json';
  }
  if (t.charAt(0) === '{' && t.length < 8192) {
    try {
      var parsed = JSON.parse(t);
      if (parsed && typeof parsed === 'object' && ('ok' in parsed || 'error' in parsed)) {
        return 'api-error-json';
      }
    } catch (ignored) {
      // 不是完整 JSON 就當一般內容。
    }
  }
  // 每個 client 都會帶 app_version；沒有代表這是舊版或非預期來源，先標起來但不拒收。
  var version = appVersion === undefined || appVersion === null ? '' : String(appVersion).trim();
  if (version.length === 0) {
    return 'missing-app-version';
  }
  return '';
}

function takeQuota_(installId) {
  var lock = LockService.getScriptLock();
  lock.waitLock(10000);
  try {
    var props = PropertiesService.getScriptProperties();
    var now = new Date();
    var day = Utilities.formatDate(now, TIME_ZONE, 'yyyyMMdd');
    var minute = Utilities.formatDate(now, TIME_ZONE, 'yyyyMMddHHmm');
    var dayCount = props.getProperty('Q_DAY') === day ? parseInt(props.getProperty('Q_DAY_N'), 10) || 0 : 0;
    var minCount = props.getProperty('Q_MIN') === minute ? parseInt(props.getProperty('Q_MIN_N'), 10) || 0 : 0;
    if (dayCount >= MAX_PER_DAY) {
      return 'daily_limit';
    }
    if (minCount >= MAX_PER_MINUTE) {
      return 'rate_limit';
    }
    var cache = CacheService.getScriptCache();
    var installKey = installId ? 'QI_' + day + '_' + installId : null;
    var installCount = installKey ? parseInt(cache.get(installKey), 10) || 0 : 0;
    if (installKey && installCount >= MAX_PER_INSTALL_PER_DAY) {
      return 'install_limit';
    }
    props.setProperties({
      Q_DAY: day, Q_DAY_N: String(dayCount + 1),
      Q_MIN: minute, Q_MIN_N: String(minCount + 1)
    });
    if (installKey) {
      cache.put(installKey, String(installCount + 1), 6 * 60 * 60);  // 快取最長 6 小時，夠擋短時間灌檔
    }
    return null;
  } finally {
    lock.releaseLock();
  }
}

function folder_() {
  var props = PropertiesService.getScriptProperties();
  var id = props.getProperty('FOLDER_ID');
  if (id) {
    return DriveApp.getFolderById(id);
  }
  var folder = DriveApp.createFolder(FOLDER_NAME);
  props.setProperty('FOLDER_ID', folder.getId());
  return folder;
}

function newCode_() {
  var s = '';
  for (var i = 0; i < 6; i++) {
    s += CODE_ALPHABET.charAt(Math.floor(Math.random() * CODE_ALPHABET.length));
  }
  return s;
}

function clip_(v, max) {
  var s = v === undefined || v === null ? '' : String(v);
  return s.length > max ? s.substring(0, max) : s;
}

function json_(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj)).setMimeType(ContentService.MimeType.JSON);
}

/** 部署前在編輯器裡手動執行一次：建立資料夾、產生 READ_KEY，並觸發授權畫面。 */
function setup() {
  folder_();
  var props = PropertiesService.getScriptProperties();
  if (!props.getProperty('READ_KEY')) {
    props.setProperty('READ_KEY', Utilities.getUuid().replace(/-/g, ''));
  }
  console.log('FOLDER_ID=' + props.getProperty('FOLDER_ID'));
  console.log('READ_KEY=' + props.getProperty('READ_KEY'));
}
