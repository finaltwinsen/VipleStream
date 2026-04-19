#!/usr/bin/env python3
"""
Apply VipleStream-specific translations to all Qt Linguist .ts
files in moonlight-qt/app/languages/.

The 15 VipleStream strings (FRUC toggle, backend/quality presets,
relay URL/PSK/force toggle, their tooltips) were added to
SettingsView.qml but only zh_TW got partial translations. This
script fills in the rest so users in all 30 supported locales see
localized text. One-shot run — the .ts files are then re-generated
by lrelease into .qm at build time.

Design notes:
- The translation dict is keyed by source string. Each key maps to
  a per-locale sub-dict.
- Technical terms (FRUC, GPU, NAT, UDP, VPN, CUDA, ws://, D3D11,
  GLES, iGPU, WARP) stay in their original form in all locales.
- We ONLY write a translation when an entry exists in our dict
  for that locale. Missing locales leave the existing (possibly
  empty) translation alone so we never silently overwrite an
  existing human-provided one.
- We also flip `type="unfinished"` off on entries we fill.
"""

from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

LANG_DIR = Path(__file__).parent.parent / "moonlight-qt" / "app" / "languages"

# Short-hand for language name aliasing: some locales share the
# same translation (e.g. pt and pt_BR can often use the same
# Portuguese text unless brand-specific).
T = dict

# Source string -> {locale: translation}.
# Locales not present fall through to whatever is already in the .ts.
TRANSLATIONS = {
    "Frame interpolation (FRUC 2x)": {
        "bg":    "Интерполация на кадри (FRUC 2×)",
        "ckb":   "دروستکردنی چوارچێوە (FRUC 2×)",
        "cs":    "Interpolace snímků (FRUC 2×)",
        "de":    "Zwischenbild-Interpolation (FRUC 2×)",
        "el":    "Παρεμβολή καρέ (FRUC 2×)",
        "eo":    "Interpolado de kadroj (FRUC 2×)",
        "es":    "Interpolación de fotogramas (FRUC 2×)",
        "et":    "Kaadri interpolatsioon (FRUC 2×)",
        "fr":    "Interpolation d'images (FRUC 2×)",
        "he":    "אינטרפולציה של פריימים (FRUC 2×)",
        "hi":    "फ़्रेम इंटरपोलेशन (FRUC 2×)",
        "hu":    "Képkocka-interpoláció (FRUC 2×)",
        "it":    "Interpolazione dei fotogrammi (FRUC 2×)",
        "ja":    "フレーム補間 (FRUC 2×)",
        "ko":    "프레임 보간 (FRUC 2×)",
        "lt":    "Kadrų interpoliacija (FRUC 2×)",
        "nb_NO": "Bildeinterpolasjon (FRUC 2×)",
        "nl":    "Frame-interpolatie (FRUC 2×)",
        "pl":    "Interpolacja klatek (FRUC 2×)",
        "pt":    "Interpolação de frames (FRUC 2×)",
        "pt_BR": "Interpolação de quadros (FRUC 2×)",
        "ru":    "Интерполяция кадров (FRUC 2×)",
        "sv":    "Bildinterpolering (FRUC 2×)",
        "ta":    "பிரேம் இடைப்படுத்தல் (FRUC 2×)",
        "th":    "การแทรกเฟรม (FRUC 2×)",
        "tr":    "Kare aradeğerlemesi (FRUC 2×)",
        "uk":    "Інтерполяція кадрів (FRUC 2×)",
        "vi":    "Nội suy khung hình (FRUC 2×)",
        "zh_CN": "补帧 (FRUC 2 倍)",
        "zh_TW": "補幀 (FRUC 2 倍)",
    },
    "Generic Compute (low latency, recommended)": {
        "bg":    "Общ изчислителен шейдър (нисък лаг, препоръчително)",
        "ckb":   "ژمێرکاری گشتی (دواکەوتنی کەم، پێشنیازکراو)",
        "cs":    "Obecný výpočetní shader (nízká latence, doporučeno)",
        "de":    "Generischer Compute-Shader (geringe Latenz, empfohlen)",
        "el":    "Γενικός υπολογιστικός (χαμηλή καθυστέρηση, συνιστάται)",
        "eo":    "Ĝenerala Komputado (malalta latenco, rekomendita)",
        "es":    "Compute genérico (baja latencia, recomendado)",
        "et":    "Üldine arvutuslik (madal latentsus, soovitatud)",
        "fr":    "Compute générique (faible latence, recommandé)",
        "he":    "מחשוב גנרי (השהיה נמוכה, מומלץ)",
        "hi":    "सामान्य कम्प्यूट (कम विलंब, अनुशंसित)",
        "hu":    "Általános compute (alacsony késleltetés, ajánlott)",
        "it":    "Compute generico (bassa latenza, consigliato)",
        "ja":    "汎用コンピュート (低遅延、推奨)",
        "ko":    "일반 컴퓨트 (저지연, 권장)",
        "lt":    "Bendrasis skaičiavimas (maža delsa, rekomenduojama)",
        "nb_NO": "Generisk Compute (lav forsinkelse, anbefalt)",
        "nl":    "Generieke Compute (lage latentie, aanbevolen)",
        "pl":    "Ogólny Compute (niskie opóźnienie, zalecane)",
        "pt":    "Compute genérico (baixa latência, recomendado)",
        "pt_BR": "Compute genérico (baixa latência, recomendado)",
        "ru":    "Универсальный Compute (низкая задержка, рекомендуется)",
        "sv":    "Generisk Compute (låg latens, rekommenderas)",
        "ta":    "பொது கணிப்பான் (குறைந்த தாமதம், பரிந்துரைக்கப்படுகிறது)",
        "th":    "คำนวณทั่วไป (หน่วงต่ำ, แนะนำ)",
        "tr":    "Genel Compute (düşük gecikme, önerilen)",
        "uk":    "Універсальний Compute (низька затримка, рекомендовано)",
        "vi":    "Compute tổng quát (độ trễ thấp, khuyến nghị)",
        "zh_CN": "通用计算着色器 (低延迟,推荐)",
        "zh_TW": "通用運算著色器 (低延遲,建議)",
    },
    "NVIDIA Optical Flow (high quality, CUDA required)": {
        "bg":    "NVIDIA Optical Flow (високо качество, необходим е CUDA)",
        "ckb":   "NVIDIA Optical Flow (کوالیتی بەرز، CUDA پێویستە)",
        "cs":    "NVIDIA Optical Flow (vysoká kvalita, vyžaduje CUDA)",
        "de":    "NVIDIA Optical Flow (hohe Qualität, CUDA erforderlich)",
        "el":    "NVIDIA Optical Flow (υψηλή ποιότητα, απαιτείται CUDA)",
        "eo":    "NVIDIA Optical Flow (alta kvalito, CUDA bezonata)",
        "es":    "NVIDIA Optical Flow (alta calidad, requiere CUDA)",
        "et":    "NVIDIA Optical Flow (kõrge kvaliteet, vajab CUDA)",
        "fr":    "NVIDIA Optical Flow (haute qualité, CUDA requis)",
        "he":    "NVIDIA Optical Flow (איכות גבוהה, דרוש CUDA)",
        "hi":    "NVIDIA Optical Flow (उच्च गुणवत्ता, CUDA आवश्यक)",
        "hu":    "NVIDIA Optical Flow (nagy minőség, CUDA szükséges)",
        "it":    "NVIDIA Optical Flow (alta qualità, CUDA richiesto)",
        "ja":    "NVIDIA Optical Flow (高品質、CUDA 必須)",
        "ko":    "NVIDIA Optical Flow (고품질, CUDA 필요)",
        "lt":    "NVIDIA Optical Flow (aukšta kokybė, reikia CUDA)",
        "nb_NO": "NVIDIA Optical Flow (høy kvalitet, CUDA kreves)",
        "nl":    "NVIDIA Optical Flow (hoge kwaliteit, CUDA vereist)",
        "pl":    "NVIDIA Optical Flow (wysoka jakość, wymaga CUDA)",
        "pt":    "NVIDIA Optical Flow (alta qualidade, requer CUDA)",
        "pt_BR": "NVIDIA Optical Flow (alta qualidade, requer CUDA)",
        "ru":    "NVIDIA Optical Flow (высокое качество, требуется CUDA)",
        "sv":    "NVIDIA Optical Flow (hög kvalitet, kräver CUDA)",
        "ta":    "NVIDIA Optical Flow (உயர் தரம், CUDA தேவை)",
        "th":    "NVIDIA Optical Flow (คุณภาพสูง, ต้องมี CUDA)",
        "tr":    "NVIDIA Optical Flow (yüksek kalite, CUDA gerekir)",
        "uk":    "NVIDIA Optical Flow (висока якість, потрібен CUDA)",
        "vi":    "NVIDIA Optical Flow (chất lượng cao, cần CUDA)",
        "zh_CN": "NVIDIA Optical Flow (高质量,需要 CUDA)",
        "zh_TW": "NVIDIA Optical Flow (高品質,需要 CUDA)",
    },
    "Quality — Best visual quality, higher GPU load": {
        "bg":    "Качество — Най-добро качество, по-голямо натоварване на GPU",
        "ckb":   "کوالیتی — باشترین کوالیتی بینراو، باری زۆرتر لەسەر GPU",
        "cs":    "Kvalita — Nejlepší obraz, vyšší zátěž GPU",
        "de":    "Qualität — Beste Bildqualität, höhere GPU-Last",
        "el":    "Ποιότητα — Καλύτερη εικόνα, μεγαλύτερο φορτίο GPU",
        "eo":    "Kvalito — Plej bona bildo, pli alta ŝarĝo de GPU",
        "es":    "Calidad — Mejor calidad visual, mayor carga de GPU",
        "et":    "Kvaliteet — Parim pilt, suurem GPU koormus",
        "fr":    "Qualité — Meilleure qualité visuelle, charge GPU élevée",
        "he":    "איכות — האיכות הויזואלית הטובה ביותר, עומס GPU גבוה",
        "hi":    "गुणवत्ता — सर्वश्रेष्ठ दृश्य गुणवत्ता, उच्च GPU लोड",
        "hu":    "Minőség — Legjobb képminőség, nagyobb GPU-terhelés",
        "it":    "Qualità — Migliore qualità visiva, carico GPU superiore",
        "ja":    "品質 — 最高の画質、GPU 負荷が高い",
        "ko":    "품질 — 최고의 시각 품질, GPU 부하 높음",
        "lt":    "Kokybė — Geriausia vaizdo kokybė, didesnė GPU apkrova",
        "nb_NO": "Kvalitet — Beste bildekvalitet, høyere GPU-belastning",
        "nl":    "Kwaliteit — Beste beeldkwaliteit, hogere GPU-belasting",
        "pl":    "Jakość — Najlepsza jakość obrazu, większe obciążenie GPU",
        "pt":    "Qualidade — Melhor qualidade visual, maior carga de GPU",
        "pt_BR": "Qualidade — Melhor qualidade visual, maior carga de GPU",
        "ru":    "Качество — Лучшее качество изображения, выше нагрузка на GPU",
        "sv":    "Kvalitet — Bästa bildkvalitet, högre GPU-belastning",
        "ta":    "தரம் — சிறந்த காட்சி தரம், அதிக GPU சுமை",
        "th":    "คุณภาพ — คุณภาพภาพดีที่สุด, โหลด GPU สูงขึ้น",
        "tr":    "Kalite — En iyi görüntü kalitesi, yüksek GPU yükü",
        "uk":    "Якість — Найкраща якість зображення, більше навантаження на GPU",
        "vi":    "Chất lượng — Chất lượng hình ảnh tốt nhất, tải GPU cao hơn",
        "zh_CN": "品质 — 最佳画质,GPU 负载较高",
        "zh_TW": "品質 — 最佳畫質,GPU 負載較高",
    },
    "Balanced — Recommended (default)": {
        "bg":    "Балансирано — Препоръчително (по подразбиране)",
        "ckb":   "ڕێکخراو — پێشنیازکراو (بنەڕەتی)",
        "cs":    "Vyvážené — Doporučeno (výchozí)",
        "de":    "Ausgewogen — Empfohlen (Standard)",
        "el":    "Ισορροπημένο — Συνιστάται (προεπιλογή)",
        "eo":    "Ekvilibra — Rekomendita (defaŭlta)",
        "es":    "Equilibrado — Recomendado (predeterminado)",
        "et":    "Tasakaalustatud — Soovitatud (vaikimisi)",
        "fr":    "Équilibré — Recommandé (par défaut)",
        "he":    "מאוזן — מומלץ (ברירת מחדל)",
        "hi":    "संतुलित — अनुशंसित (डिफ़ॉल्ट)",
        "hu":    "Kiegyensúlyozott — Ajánlott (alapértelmezett)",
        "it":    "Bilanciato — Consigliato (predefinito)",
        "ja":    "バランス — 推奨 (デフォルト)",
        "ko":    "균형 — 권장 (기본값)",
        "lt":    "Subalansuotas — Rekomenduojama (numatytoji)",
        "nb_NO": "Balansert — Anbefalt (standard)",
        "nl":    "Gebalanceerd — Aanbevolen (standaard)",
        "pl":    "Zbalansowany — Zalecany (domyślny)",
        "pt":    "Equilibrado — Recomendado (padrão)",
        "pt_BR": "Equilibrado — Recomendado (padrão)",
        "ru":    "Сбалансированный — Рекомендуется (по умолчанию)",
        "sv":    "Balanserad — Rekommenderas (standard)",
        "ta":    "சமநிலை — பரிந்துரைக்கப்படுகிறது (இயல்புநிலை)",
        "th":    "สมดุล — แนะนำ (ค่าเริ่มต้น)",
        "tr":    "Dengeli — Önerilen (varsayılan)",
        "uk":    "Збалансований — Рекомендовано (за замовчуванням)",
        "vi":    "Cân bằng — Khuyến nghị (mặc định)",
        "zh_CN": "均衡 — 推荐 (默认)",
        "zh_TW": "平衡 — 建議 (預設)",
    },
    "Performance — Lowest latency, suitable for iGPU": {
        "bg":    "Производителност — Най-нисък лаг, подходящо за iGPU",
        "ckb":   "کارایی — کەمترین دواکەوتن، گونجاو بۆ iGPU",
        "cs":    "Výkon — Nejnižší latence, vhodné pro iGPU",
        "de":    "Leistung — Geringste Latenz, für iGPU geeignet",
        "el":    "Απόδοση — Χαμηλότερη καθυστέρηση, κατάλληλο για iGPU",
        "eo":    "Rendimento — Plej malalta latenco, taŭga por iGPU",
        "es":    "Rendimiento — Menor latencia, adecuado para iGPU",
        "et":    "Jõudlus — Madalaim latentsus, sobib iGPU-le",
        "fr":    "Performance — Latence minimale, adapté aux iGPU",
        "he":    "ביצועים — השהיה הנמוכה ביותר, מתאים ל-iGPU",
        "hi":    "परफ़ॉर्मेंस — न्यूनतम विलंब, iGPU के लिए उपयुक्त",
        "hu":    "Teljesítmény — Legalacsonyabb késleltetés, iGPU-hoz ajánlott",
        "it":    "Prestazioni — Latenza minima, adatto alle iGPU",
        "ja":    "パフォーマンス — 最低遅延、iGPU に適合",
        "ko":    "성능 — 최저 지연, iGPU에 적합",
        "lt":    "Našumas — Mažiausia delsa, tinka iGPU",
        "nb_NO": "Ytelse — Lavest forsinkelse, egnet for iGPU",
        "nl":    "Prestaties — Laagste latentie, geschikt voor iGPU",
        "pl":    "Wydajność — Najniższe opóźnienie, odpowiednie dla iGPU",
        "pt":    "Desempenho — Menor latência, adequado para iGPU",
        "pt_BR": "Desempenho — Menor latência, adequado para iGPU",
        "ru":    "Производительность — Минимальная задержка, подходит для iGPU",
        "sv":    "Prestanda — Lägsta latens, lämpligt för iGPU",
        "ta":    "செயல்திறன் — மிகக் குறைந்த தாமதம், iGPU-க்கு பொருத்தம்",
        "th":    "ประสิทธิภาพ — หน่วงต่ำที่สุด, เหมาะกับ iGPU",
        "tr":    "Performans — En düşük gecikme, iGPU'ya uygun",
        "uk":    "Продуктивність — Найнижча затримка, підходить для iGPU",
        "vi":    "Hiệu năng — Độ trễ thấp nhất, phù hợp với iGPU",
        "zh_CN": "性能 — 最低延迟,适合集显",
        "zh_TW": "效能 — 最低延遲,適合內顯",
    },
    "⚠ Frame interpolation is disabled above 180 FPS": {
        "bg":    "⚠ Интерполацията е изключена над 180 FPS",
        "ckb":   "⚠ دروستکردنی چوارچێوە لە سەرەوەی 180 FPS ناچالاکە",
        "cs":    "⚠ Interpolace snímků je nad 180 FPS vypnutá",
        "de":    "⚠ Zwischenbild-Interpolation ist über 180 FPS deaktiviert",
        "el":    "⚠ Η παρεμβολή καρέ απενεργοποιείται άνω των 180 FPS",
        "eo":    "⚠ Kadra interpolado malŝaltiĝas super 180 FPS",
        "es":    "⚠ La interpolación se desactiva por encima de 180 FPS",
        "et":    "⚠ Kaadri interpolatsioon on üle 180 FPS välja lülitatud",
        "fr":    "⚠ L'interpolation est désactivée au-delà de 180 FPS",
        "he":    "⚠ אינטרפולציה מושבתת מעל 180 FPS",
        "hi":    "⚠ 180 FPS से ऊपर फ़्रेम इंटरपोलेशन अक्षम है",
        "hu":    "⚠ A képkocka-interpoláció 180 FPS felett le van tiltva",
        "it":    "⚠ L'interpolazione è disattivata sopra i 180 FPS",
        "ja":    "⚠ 180 FPS を超えるとフレーム補間は無効になります",
        "ko":    "⚠ 180 FPS 이상에서는 프레임 보간이 비활성화됩니다",
        "lt":    "⚠ Kadrų interpoliacija išjungta virš 180 FPS",
        "nb_NO": "⚠ Bildeinterpolasjon er deaktivert over 180 FPS",
        "nl":    "⚠ Frame-interpolatie is uitgeschakeld boven 180 FPS",
        "pl":    "⚠ Interpolacja klatek jest wyłączona powyżej 180 FPS",
        "pt":    "⚠ A interpolação está desativada acima de 180 FPS",
        "pt_BR": "⚠ A interpolação é desativada acima de 180 FPS",
        "ru":    "⚠ Интерполяция кадров отключается выше 180 FPS",
        "sv":    "⚠ Bildinterpolering är inaktiverad över 180 FPS",
        "ta":    "⚠ 180 FPS க்கு மேல் பிரேம் இடைப்படுத்தல் முடக்கப்பட்டுள்ளது",
        "th":    "⚠ การแทรกเฟรมถูกปิดเมื่อเกิน 180 FPS",
        "tr":    "⚠ Kare aradeğerlemesi 180 FPS üzerinde devre dışıdır",
        "uk":    "⚠ Інтерполяцію кадрів вимкнено понад 180 FPS",
        "vi":    "⚠ Nội suy khung hình bị tắt khi vượt 180 FPS",
        "zh_CN": "⚠ 180 FPS 以上时补帧自动停用",
        "zh_TW": "⚠ 補幀功能在 180 FPS 以上時停用",
    },
    "NAT Traversal (Signaling Relay)": {
        "bg":    "NAT пробиване (сигнален релей)",
        "ckb":   "پەڕینەوەی NAT (ڕیلی ئامراز)",
        "cs":    "Průchod NAT (signální relé)",
        "de":    "NAT-Traversal (Signaling-Relay)",
        "el":    "Διέλευση NAT (Αναμεταδότης σηματοδοσίας)",
        "eo":    "NAT-trapasigo (Signalada relajso)",
        "es":    "NAT Traversal (relé de señalización)",
        "et":    "NAT-läbimine (signaali relee)",
        "fr":    "Traversée NAT (relais de signalisation)",
        "he":    "חצית NAT (ממסר איתות)",
        "hi":    "NAT ट्रावर्सल (सिग्नलिंग रिले)",
        "hu":    "NAT áthaladás (Jelzésközvetítő)",
        "it":    "NAT Traversal (relay di segnalazione)",
        "ja":    "NAT 越え (シグナリングリレー)",
        "ko":    "NAT 우회 (시그널링 중계)",
        "lt":    "NAT perėjimas (signalizacijos relė)",
        "nb_NO": "NAT-traversering (signalrelé)",
        "nl":    "NAT-traversal (signaleringsrelay)",
        "pl":    "Przechodzenie NAT (przekaźnik sygnalizacji)",
        "pt":    "NAT Traversal (retransmissor de sinalização)",
        "pt_BR": "NAT Traversal (retransmissor de sinalização)",
        "ru":    "Обход NAT (сигнальный ретранслятор)",
        "sv":    "NAT-traversering (signaleringsrelä)",
        "ta":    "NAT கடப்பு (சிக்னல் ரிலே)",
        "th":    "NAT Traversal (รีเลย์สัญญาณ)",
        "tr":    "NAT Geçişi (sinyal rölesi)",
        "uk":    "Обхід NAT (сигнальний ретранслятор)",
        "vi":    "Xuyên NAT (bộ chuyển tín hiệu)",
        "zh_CN": "NAT 穿透 (信令中继)",
        "zh_TW": "NAT 穿透 (訊令中繼)",
    },
    "Relay URL (e.g. ws://relay.example.com:9999)": {
        "bg":    "URL на релея (напр. ws://relay.example.com:9999)",
        "ckb":   "URL ی ڕیلی (نموونە: ws://relay.example.com:9999)",
        "cs":    "URL relé (např. ws://relay.example.com:9999)",
        "de":    "Relay-URL (z. B. ws://relay.example.com:9999)",
        "el":    "URL αναμεταδότη (π.χ. ws://relay.example.com:9999)",
        "eo":    "URL de relajso (ekz. ws://relay.example.com:9999)",
        "es":    "URL del relé (p. ej. ws://relay.example.com:9999)",
        "et":    "Relee URL (nt ws://relay.example.com:9999)",
        "fr":    "URL du relais (ex. ws://relay.example.com:9999)",
        "he":    "כתובת ממסר (לדוגמה ws://relay.example.com:9999)",
        "hi":    "रिले URL (उदा. ws://relay.example.com:9999)",
        "hu":    "Relay URL (pl. ws://relay.example.com:9999)",
        "it":    "URL relay (es. ws://relay.example.com:9999)",
        "ja":    "中継サーバー URL (例: ws://relay.example.com:9999)",
        "ko":    "중계 URL (예: ws://relay.example.com:9999)",
        "lt":    "Relės URL (pvz. ws://relay.example.com:9999)",
        "nb_NO": "Relé-URL (f.eks. ws://relay.example.com:9999)",
        "nl":    "Relay-URL (bijv. ws://relay.example.com:9999)",
        "pl":    "Adres URL przekaźnika (np. ws://relay.example.com:9999)",
        "pt":    "URL do relay (ex.: ws://relay.example.com:9999)",
        "pt_BR": "URL do relay (ex.: ws://relay.example.com:9999)",
        "ru":    "URL ретранслятора (напр. ws://relay.example.com:9999)",
        "sv":    "Relä-URL (t.ex. ws://relay.example.com:9999)",
        "ta":    "ரிலே URL (எ.கா. ws://relay.example.com:9999)",
        "th":    "URL รีเลย์ (เช่น ws://relay.example.com:9999)",
        "tr":    "Röle URL'si (örn. ws://relay.example.com:9999)",
        "uk":    "URL ретранслятора (напр. ws://relay.example.com:9999)",
        "vi":    "URL bộ chuyển tiếp (ví dụ: ws://relay.example.com:9999)",
        "zh_CN": "中继 URL (例如 ws://relay.example.com:9999)",
        "zh_TW": "中繼 URL (例如 ws://relay.example.com:9999)",
    },
    "Relay PSK (pre-shared key, leave empty if none)": {
        "bg":    "Relay PSK (предварително споделен ключ, оставете празно, ако няма)",
        "ckb":   "Relay PSK (کلیلی پێشبەشکراو، بەتاڵی بهێڵەوە گەر نەبوو)",
        "cs":    "Relay PSK (sdílený klíč, prázdné pokud žádný)",
        "de":    "Relay-PSK (vorgeteilter Schlüssel, leer lassen wenn keiner)",
        "el":    "PSK αναμεταδότη (προ-διαμοιρασμένο κλειδί, κενό αν κανένα)",
        "eo":    "Relajsa PSK (antaŭ-dividita ŝlosilo, lasu malplena se neniu)",
        "es":    "PSK del relé (clave precompartida, deje vacío si no hay)",
        "et":    "Relee PSK (jagatud võti, tühi kui puudub)",
        "fr":    "PSK du relais (clé pré-partagée, vide si aucune)",
        "he":    "PSK של הממסר (מפתח מוגדר מראש, ריק אם אין)",
        "hi":    "रिले PSK (प्री-शेयर्ड कुंजी, न हो तो खाली छोड़ें)",
        "hu":    "Relay PSK (előre megosztott kulcs, hagyja üresen ha nincs)",
        "it":    "PSK relay (chiave pre-condivisa, vuoto se nessuna)",
        "ja":    "中継 PSK (事前共有鍵、なければ空欄)",
        "ko":    "중계 PSK (사전 공유 키, 없으면 비워 두기)",
        "lt":    "Relės PSK (iš anksto dalintas raktas, palikite tuščią jei nėra)",
        "nb_NO": "Relé-PSK (forhåndsdelt nøkkel, tom hvis ingen)",
        "nl":    "Relay-PSK (vooraf gedeelde sleutel, leeg indien geen)",
        "pl":    "PSK przekaźnika (klucz współdzielony, pozostaw puste jeśli brak)",
        "pt":    "PSK do relay (chave pré-compartilhada, em branco se nenhuma)",
        "pt_BR": "PSK do relay (chave pré-compartilhada, em branco se nenhuma)",
        "ru":    "PSK ретранслятора (общий ключ, пусто если отсутствует)",
        "sv":    "Relä-PSK (förhandsdelad nyckel, tomt om ingen)",
        "ta":    "ரிலே PSK (முன்பகிரப்பட்ட விசை, இல்லையென்றால் காலியாக விடவும்)",
        "th":    "PSK รีเลย์ (คีย์ที่แบ่งปันล่วงหน้า, เว้นว่างถ้าไม่มี)",
        "tr":    "Röle PSK'sı (önceden paylaşılan anahtar, yoksa boş bırakın)",
        "uk":    "PSK ретранслятора (спільний ключ, залиште порожнім якщо немає)",
        "vi":    "PSK relay (khóa chia sẻ trước, để trống nếu không có)",
        "zh_CN": "中继 PSK (预共享密钥,没有则留空)",
        "zh_TW": "中繼 PSK (預共用金鑰,沒有則留空)",
    },
    "Optional: helps connect when both sides are behind NAT. Run viplestream-relay on a server with a public IP.": {
        "bg":    "По желание: помага за връзка, когато и двете страни са зад NAT. Пуснете viplestream-relay на сървър с публичен IP.",
        "ckb":   "هەڵبژاردە: یارمەتی دەدات کاتێک هەردووک لا لە دواوەی NAT بن. viplestream-relay بەڕێوەبە لەسەر سێرڤەرێک بە IP گشتی.",
        "cs":    "Volitelné: pomáhá navázat spojení, když jsou obě strany za NAT. Spusťte viplestream-relay na serveru s veřejnou IP.",
        "de":    "Optional: hilft bei der Verbindung, wenn beide Seiten hinter NAT sind. Führen Sie viplestream-relay auf einem Server mit öffentlicher IP aus.",
        "el":    "Προαιρετικό: βοηθά στη σύνδεση όταν και οι δύο πλευρές βρίσκονται πίσω από NAT. Εκτελέστε το viplestream-relay σε διακομιστή με δημόσιο IP.",
        "eo":    "Laŭvola: helpas konektiĝi kiam ambaŭ flankoj estas malantaŭ NAT. Rulu viplestream-relay sur servilo kun publika IP.",
        "es":    "Opcional: ayuda a conectar cuando ambos extremos están detrás de NAT. Ejecute viplestream-relay en un servidor con IP pública.",
        "et":    "Valikuline: aitab ühendust luua, kui mõlemad pooled on NAT-i taga. Käivitage viplestream-relay avaliku IP-ga serveris.",
        "fr":    "Optionnel : facilite la connexion quand les deux côtés sont derrière un NAT. Exécutez viplestream-relay sur un serveur avec IP publique.",
        "he":    "אופציונלי: עוזר להתחבר כששני הצדדים מאחורי NAT. הפעל את viplestream-relay על שרת עם IP ציבורי.",
        "hi":    "वैकल्पिक: जब दोनों पक्ष NAT के पीछे हों तब कनेक्ट होने में मदद करता है। पब्लिक IP वाले सर्वर पर viplestream-relay चलाएँ.",
        "hu":    "Opcionális: segít a csatlakozásban, ha mindkét fél NAT mögött van. Futtassa a viplestream-relay-t egy nyilvános IP-vel rendelkező szerveren.",
        "it":    "Opzionale: aiuta a connettere quando entrambi i lati sono dietro NAT. Esegui viplestream-relay su un server con IP pubblico.",
        "ja":    "任意:両側が NAT の背後にある場合の接続を補助します。パブリック IP のサーバーで viplestream-relay を起動してください。",
        "ko":    "선택 사항: 양측이 모두 NAT 뒤에 있을 때 연결을 돕습니다. 공용 IP 서버에서 viplestream-relay를 실행하세요.",
        "lt":    "Papildoma: padeda prisijungti, kai abi pusės yra už NAT. Paleiskite viplestream-relay serveryje su viešu IP.",
        "nb_NO": "Valgfritt: hjelper tilkobling når begge sider er bak NAT. Kjør viplestream-relay på en server med offentlig IP.",
        "nl":    "Optioneel: helpt verbinden wanneer beide zijden achter NAT zitten. Draai viplestream-relay op een server met een publiek IP-adres.",
        "pl":    "Opcjonalne: pomaga w połączeniu, gdy obie strony są za NAT. Uruchom viplestream-relay na serwerze z publicznym IP.",
        "pt":    "Opcional: ajuda a conectar quando ambos os lados estão atrás de NAT. Execute viplestream-relay num servidor com IP público.",
        "pt_BR": "Opcional: ajuda a conectar quando ambos os lados estão atrás de NAT. Execute viplestream-relay em um servidor com IP público.",
        "ru":    "Необязательно: помогает установить соединение, когда обе стороны за NAT. Запустите viplestream-relay на сервере с публичным IP.",
        "sv":    "Valfritt: hjälper till med anslutning när båda sidor är bakom NAT. Kör viplestream-relay på en server med publik IP.",
        "ta":    "விருப்பம்: இரு பக்கங்களும் NAT பின்னணியில் இருக்கும்போது இணைக்க உதவும். பொது IP உள்ள சேவையகத்தில் viplestream-relay ஐ இயக்கவும்.",
        "th":    "ตัวเลือก: ช่วยเชื่อมต่อเมื่อทั้งสองฝั่งอยู่หลัง NAT รัน viplestream-relay บนเซิร์ฟเวอร์ที่มี IP สาธารณะ",
        "tr":    "İsteğe bağlı: her iki taraf da NAT arkasındaysa bağlanmaya yardımcı olur. viplestream-relay'ı genel IP'li bir sunucuda çalıştırın.",
        "uk":    "Додатково: допомагає з'єднатися, коли обидві сторони за NAT. Запустіть viplestream-relay на сервері з публічним IP.",
        "vi":    "Tuỳ chọn: giúp kết nối khi cả hai bên đều sau NAT. Chạy viplestream-relay trên máy chủ có IP công khai.",
        "zh_CN": "可选:两端都在 NAT 后方时有助于建立连线。请在具有公网 IP 的服务器上执行 viplestream-relay。",
        "zh_TW": "選用:兩端都在 NAT 後方時有助於建立連線。請在具有公網 IP 的伺服器上執行 viplestream-relay。",
    },
    # FRUC backend combo-box tooltip
    "Generic Compute uses D3D11/GLES compute shaders with ~1-2ms latency per frame. NVIDIA Optical Flow uses dedicated hardware via CUDA but adds ~12ms due to cross-API synchronization. Generic is recommended for most use cases.": {
        "bg":    "Общият изчислителен шейдър използва D3D11/GLES с ~1-2 ms закъснение на кадър. NVIDIA Optical Flow използва специализиран хардуер чрез CUDA, но добавя ~12 ms заради синхронизацията между API-тата. За повечето случаи се препоръчва Общ.",
        "ckb":   "ژمێرکاری گشتی شەیدەری D3D11/GLES بەکاردەهێنێت بە ~1-2 ms دواکەوتنی چوارچێوە. NVIDIA Optical Flow کەرەستەی تایبەت لە ڕێی CUDA بەکاردەهێنێت بەڵام ~12 ms دواکەوتن زیاد دەکات. گشتی بۆ زۆربەی حاڵەتەکان پێشنیازکراوە.",
        "cs":    "Obecný výpočetní shader používá D3D11/GLES s latencí ~1-2 ms na snímek. NVIDIA Optical Flow využívá specializovaný hardware přes CUDA, ale přidává ~12 ms kvůli synchronizaci mezi API. Pro většinu případů se doporučuje obecný.",
        "de":    "Generischer Compute nutzt D3D11/GLES-Compute-Shader mit ~1-2 ms Latenz pro Bild. NVIDIA Optical Flow verwendet dedizierte Hardware über CUDA, fügt aber ~12 ms durch API-Synchronisierung hinzu. Für die meisten Fälle wird Generisch empfohlen.",
        "el":    "Ο Γενικός υπολογιστικός χρησιμοποιεί D3D11/GLES compute shaders με καθυστέρηση ~1-2 ms ανά καρέ. Το NVIDIA Optical Flow χρησιμοποιεί ειδικό υλικό μέσω CUDA αλλά προσθέτει ~12 ms λόγω συγχρονισμού μεταξύ API. Για τις περισσότερες περιπτώσεις συνιστάται ο Γενικός.",
        "eo":    "Ĝenerala Komputado uzas D3D11/GLES komput-ŝatelojn kun ~1-2 ms latenco po kadro. NVIDIA Optical Flow uzas dediĉitan aparataron per CUDA sed aldonas ~12 ms pro inter-API sinkronigado. Ĝenerala estas rekomendata por plej multaj okazoj.",
        "es":    "Compute Genérico usa shaders D3D11/GLES con ~1-2 ms de latencia por fotograma. NVIDIA Optical Flow usa hardware dedicado vía CUDA pero añade ~12 ms por la sincronización entre APIs. Para la mayoría de casos se recomienda Genérico.",
        "et":    "Üldine arvutuslik kasutab D3D11/GLES arvutuslikke varjutajaid latentsusega ~1-2 ms kaadri kohta. NVIDIA Optical Flow kasutab CUDA kaudu pühendatud riistvara, kuid lisab API-de sünkroonimise tõttu ~12 ms. Enamikul juhtudel on soovitatud Üldine.",
        "fr":    "Compute générique utilise les shaders compute D3D11/GLES avec ~1-2 ms de latence par image. NVIDIA Optical Flow utilise du matériel dédié via CUDA mais ajoute ~12 ms en raison de la synchronisation entre API. Générique est recommandé dans la plupart des cas.",
        "he":    "מחשוב גנרי משתמש ב-shader-ים של D3D11/GLES עם השהיה של ~1-2 מ\"ש לפריים. NVIDIA Optical Flow משתמש בחומרה ייעודית דרך CUDA אך מוסיף ~12 מ\"ש בגלל סנכרון בין API-ים. עבור רוב המקרים מומלץ גנרי.",
        "hi":    "सामान्य कम्प्यूट D3D11/GLES कम्प्यूट शेडर का उपयोग करता है, लगभग 1-2 ms प्रति फ़्रेम विलंब। NVIDIA Optical Flow CUDA के माध्यम से समर्पित हार्डवेयर उपयोग करता है लेकिन API सिंक्रनाइज़ेशन के कारण ~12 ms जोड़ता है। अधिकांश मामलों के लिए सामान्य की सिफ़ारिश की जाती है।",
        "hu":    "Az általános compute D3D11/GLES compute shadereket használ ~1-2 ms képkocka-késleltetéssel. Az NVIDIA Optical Flow CUDA-n keresztül dedikált hardvert használ, de ~12 ms-ot ad hozzá az API-szinkronizáció miatt. Legtöbb esetben az általános ajánlott.",
        "it":    "Compute generico usa shader compute D3D11/GLES con ~1-2 ms di latenza per frame. NVIDIA Optical Flow usa hardware dedicato via CUDA ma aggiunge ~12 ms per la sincronizzazione tra API. Generico è consigliato nella maggior parte dei casi.",
        "ja":    "汎用コンピュートは D3D11/GLES コンピュートシェーダーを使用しフレームあたり ~1-2 ms の遅延です。NVIDIA Optical Flow は CUDA 経由で専用ハードウェアを使いますが、API 間の同期で ~12 ms が追加されます。通常は汎用を推奨します。",
        "ko":    "일반 컴퓨트는 D3D11/GLES 컴퓨트 셰이더를 사용하며 프레임당 ~1-2 ms 지연입니다. NVIDIA Optical Flow는 CUDA를 통해 전용 하드웨어를 사용하지만 API 간 동기화로 ~12 ms가 추가됩니다. 대부분의 경우 일반을 권장합니다.",
        "lt":    "Bendrasis skaičiavimas naudoja D3D11/GLES skaičiavimo šešėliavimus su ~1-2 ms delsa per kadrą. NVIDIA Optical Flow naudoja specializuotą aparatinę įrangą per CUDA, bet prideda ~12 ms dėl API sinchronizacijos. Daugumai atvejų rekomenduojama Bendrasis.",
        "nb_NO": "Generisk Compute bruker D3D11/GLES compute-shadere med ~1-2 ms forsinkelse per bilde. NVIDIA Optical Flow bruker dedikert maskinvare via CUDA, men legger til ~12 ms på grunn av API-synkronisering. Generisk anbefales i de fleste tilfeller.",
        "nl":    "Generieke Compute gebruikt D3D11/GLES compute shaders met ~1-2 ms latentie per frame. NVIDIA Optical Flow gebruikt speciale hardware via CUDA, maar voegt ~12 ms toe vanwege API-synchronisatie. Voor de meeste gevallen wordt Generiek aanbevolen.",
        "pl":    "Ogólny Compute używa shaderów obliczeniowych D3D11/GLES z opóźnieniem ~1-2 ms na klatkę. NVIDIA Optical Flow wykorzystuje dedykowany sprzęt przez CUDA, ale dodaje ~12 ms z powodu synchronizacji między API. W większości przypadków zalecany jest Ogólny.",
        "pt":    "Compute Genérico utiliza shaders D3D11/GLES com ~1-2 ms de latência por frame. NVIDIA Optical Flow usa hardware dedicado via CUDA mas adiciona ~12 ms devido à sincronização entre APIs. Genérico é recomendado na maioria dos casos.",
        "pt_BR": "Compute Genérico utiliza shaders D3D11/GLES com ~1-2 ms de latência por frame. NVIDIA Optical Flow usa hardware dedicado via CUDA mas adiciona ~12 ms devido à sincronização entre APIs. Genérico é recomendado na maioria dos casos.",
        "ru":    "Универсальный Compute использует шейдеры D3D11/GLES с задержкой ~1-2 мс на кадр. NVIDIA Optical Flow задействует специализированное оборудование через CUDA, но добавляет ~12 мс из-за синхронизации API. В большинстве случаев рекомендуется Универсальный.",
        "sv":    "Generisk Compute använder D3D11/GLES compute-shaders med ~1-2 ms latens per bild. NVIDIA Optical Flow använder dedikerad hårdvara via CUDA men lägger till ~12 ms på grund av API-synkronisering. Generisk rekommenderas i de flesta fall.",
        "ta":    "பொது கணிப்பான் D3D11/GLES கணிப்பு ஷேடர்களை ஒரு பிரேமுக்கு ~1-2 ms தாமதத்துடன் பயன்படுத்துகிறது. NVIDIA Optical Flow CUDA வழியாக சிறப்பு வன்பொருளைப் பயன்படுத்தினாலும் API ஒத்திசைவின் காரணமாக ~12 ms சேர்க்கிறது. பெரும்பாலான சூழ்நிலைகளுக்கு பொது பரிந்துரைக்கப்படுகிறது.",
        "th":    "คำนวณทั่วไปใช้ D3D11/GLES compute shader มีหน่วง ~1-2 ms ต่อเฟรม NVIDIA Optical Flow ใช้ฮาร์ดแวร์เฉพาะผ่าน CUDA แต่เพิ่มหน่วง ~12 ms จากการซิงก์ระหว่าง API ส่วนใหญ่แนะนำให้ใช้ทั่วไป",
        "tr":    "Genel Compute, kare başına ~1-2 ms gecikmeyle D3D11/GLES compute shader'larını kullanır. NVIDIA Optical Flow, CUDA üzerinden özel donanım kullanır ancak API senkronizasyonu nedeniyle ~12 ms ekler. Çoğu senaryoda Genel önerilir.",
        "uk":    "Універсальний Compute використовує шейдери D3D11/GLES із затримкою ~1-2 мс на кадр. NVIDIA Optical Flow використовує спеціалізоване обладнання через CUDA, але додає ~12 мс через синхронізацію між API. У більшості випадків рекомендовано Універсальний.",
        "vi":    "Compute tổng quát dùng compute shader D3D11/GLES với độ trễ ~1-2 ms mỗi khung. NVIDIA Optical Flow dùng phần cứng chuyên dụng qua CUDA nhưng thêm ~12 ms do đồng bộ giữa các API. Khuyến nghị dùng Tổng quát cho hầu hết trường hợp.",
        "zh_CN": "通用计算使用 D3D11/GLES 计算着色器,每帧约 1-2 ms 延迟。NVIDIA Optical Flow 经由 CUDA 调用专用硬件,但因 API 间同步增加约 12 ms 延迟。大多数情况下建议使用通用计算。",
        "zh_TW": "通用運算使用 D3D11/GLES 計算著色器,每幀約 1-2 ms 延遲。NVIDIA Optical Flow 透過 CUDA 使用專用硬體,但因 API 間同步增加約 12 ms 延遲。大多數情況建議使用通用運算。",
    },
    # Quality preset combo-box tooltip. Multi-line with \n.
    "Quality: 8-neighbor search + sub-pixel + adaptive blend (~12ms on iGPU)\nBalanced: 8-neighbor search + temporal smoothing (~8ms on iGPU)\nPerformance: 4-neighbor search + minimal processing (~6ms on iGPU)": {
        "bg":    "Качество: 8-съседно търсене + под-пиксел + адаптивно смесване (~12 ms на iGPU)\nБалансирано: 8-съседно търсене + времево заглаждане (~8 ms на iGPU)\nПроизводителност: 4-съседно търсене + минимална обработка (~6 ms на iGPU)",
        "ckb":   "کوالیتی: گەڕانی 8-دراوسێ + ژێر-پیکسێڵ + تێکەڵکردنی گونجاو (~12 ms لەسەر iGPU)\nڕێکخراو: گەڕانی 8-دراوسێ + هێواشکردنەوەی کاتیی (~8 ms لەسەر iGPU)\nکارایی: گەڕانی 4-دراوسێ + پرۆسێسی کەمترین (~6 ms لەسەر iGPU)",
        "cs":    "Kvalita: 8-sousední vyhledávání + sub-pixel + adaptivní směšování (~12 ms na iGPU)\nVyvážené: 8-sousední vyhledávání + časové vyhlazení (~8 ms na iGPU)\nVýkon: 4-sousední vyhledávání + minimální zpracování (~6 ms na iGPU)",
        "de":    "Qualität: 8-Nachbarn-Suche + Subpixel + adaptive Mischung (~12 ms auf iGPU)\nAusgewogen: 8-Nachbarn-Suche + zeitliche Glättung (~8 ms auf iGPU)\nLeistung: 4-Nachbarn-Suche + minimale Verarbeitung (~6 ms auf iGPU)",
        "el":    "Ποιότητα: αναζήτηση 8 γειτόνων + υπο-pixel + προσαρμοστική ανάμειξη (~12 ms σε iGPU)\nΙσορροπημένο: αναζήτηση 8 γειτόνων + χρονική εξομάλυνση (~8 ms σε iGPU)\nΑπόδοση: αναζήτηση 4 γειτόνων + ελάχιστη επεξεργασία (~6 ms σε iGPU)",
        "eo":    "Kvalito: 8-najbara serĉo + sub-pikselo + adapta miksado (~12 ms sur iGPU)\nEkvilibra: 8-najbara serĉo + tempa glatigado (~8 ms sur iGPU)\nRendimento: 4-najbara serĉo + minimuma traktado (~6 ms sur iGPU)",
        "es":    "Calidad: búsqueda de 8 vecinos + sub-píxel + mezcla adaptativa (~12 ms en iGPU)\nEquilibrado: búsqueda de 8 vecinos + suavizado temporal (~8 ms en iGPU)\nRendimiento: búsqueda de 4 vecinos + procesamiento mínimo (~6 ms en iGPU)",
        "et":    "Kvaliteet: 8-naabri otsing + alampiksel + adaptiivne segamine (~12 ms iGPU-l)\nTasakaalustatud: 8-naabri otsing + ajaline silumine (~8 ms iGPU-l)\nJõudlus: 4-naabri otsing + minimaalne töötlus (~6 ms iGPU-l)",
        "fr":    "Qualité : recherche 8 voisins + sous-pixel + mélange adaptatif (~12 ms sur iGPU)\nÉquilibré : recherche 8 voisins + lissage temporel (~8 ms sur iGPU)\nPerformance : recherche 4 voisins + traitement minimal (~6 ms sur iGPU)",
        "he":    "איכות: חיפוש 8 שכנים + תת-פיקסל + מיזוג אדפטיבי (~12 מ\"ש ב-iGPU)\nמאוזן: חיפוש 8 שכנים + החלקה זמנית (~8 מ\"ש ב-iGPU)\nביצועים: חיפוש 4 שכנים + עיבוד מינימלי (~6 מ\"ש ב-iGPU)",
        "hi":    "गुणवत्ता: 8-पड़ोसी खोज + सब-पिक्सेल + अनुकूली मिश्रण (~12 ms iGPU पर)\nसंतुलित: 8-पड़ोसी खोज + समय-सुगमता (~8 ms iGPU पर)\nप्रदर्शन: 4-पड़ोसी खोज + न्यूनतम प्रसंस्करण (~6 ms iGPU पर)",
        "hu":    "Minőség: 8 szomszédos keresés + szubpixel + adaptív keverés (~12 ms iGPU-n)\nKiegyensúlyozott: 8 szomszédos keresés + időbeli simítás (~8 ms iGPU-n)\nTeljesítmény: 4 szomszédos keresés + minimális feldolgozás (~6 ms iGPU-n)",
        "it":    "Qualità: ricerca 8 vicini + sub-pixel + mix adattivo (~12 ms su iGPU)\nBilanciato: ricerca 8 vicini + smoothing temporale (~8 ms su iGPU)\nPrestazioni: ricerca 4 vicini + elaborazione minima (~6 ms su iGPU)",
        "ja":    "品質: 8 近傍探索 + サブピクセル + 適応的ブレンド (iGPU で ~12 ms)\nバランス: 8 近傍探索 + 時間的スムージング (iGPU で ~8 ms)\nパフォーマンス: 4 近傍探索 + 最小限の処理 (iGPU で ~6 ms)",
        "ko":    "품질: 8-이웃 탐색 + 서브픽셀 + 적응형 블렌드 (iGPU에서 ~12 ms)\n균형: 8-이웃 탐색 + 시간 스무딩 (iGPU에서 ~8 ms)\n성능: 4-이웃 탐색 + 최소 처리 (iGPU에서 ~6 ms)",
        "lt":    "Kokybė: 8 kaimynų paieška + subpikselis + adaptyvus maišymas (~12 ms iGPU)\nSubalansuotas: 8 kaimynų paieška + laikinis išlyginimas (~8 ms iGPU)\nNašumas: 4 kaimynų paieška + minimalus apdorojimas (~6 ms iGPU)",
        "nb_NO": "Kvalitet: 8-nabo-søk + sub-piksel + adaptiv blanding (~12 ms på iGPU)\nBalansert: 8-nabo-søk + tidsmessig utjevning (~8 ms på iGPU)\nYtelse: 4-nabo-søk + minimal behandling (~6 ms på iGPU)",
        "nl":    "Kwaliteit: 8-buren zoeken + sub-pixel + adaptieve menging (~12 ms op iGPU)\nGebalanceerd: 8-buren zoeken + tijdelijke afvlakking (~8 ms op iGPU)\nPrestaties: 4-buren zoeken + minimale verwerking (~6 ms op iGPU)",
        "pl":    "Jakość: przeszukiwanie 8 sąsiadów + sub-piksel + mieszanie adaptacyjne (~12 ms na iGPU)\nZbalansowany: przeszukiwanie 8 sąsiadów + wygładzanie czasowe (~8 ms na iGPU)\nWydajność: przeszukiwanie 4 sąsiadów + minimalne przetwarzanie (~6 ms na iGPU)",
        "pt":    "Qualidade: busca por 8 vizinhos + sub-pixel + mistura adaptativa (~12 ms em iGPU)\nEquilibrado: busca por 8 vizinhos + suavização temporal (~8 ms em iGPU)\nDesempenho: busca por 4 vizinhos + processamento mínimo (~6 ms em iGPU)",
        "pt_BR": "Qualidade: busca de 8 vizinhos + sub-pixel + mistura adaptativa (~12 ms em iGPU)\nEquilibrado: busca de 8 vizinhos + suavização temporal (~8 ms em iGPU)\nDesempenho: busca de 4 vizinhos + processamento mínimo (~6 ms em iGPU)",
        "ru":    "Качество: поиск по 8 соседям + субпиксель + адаптивное смешивание (~12 мс на iGPU)\nСбалансированный: поиск по 8 соседям + временное сглаживание (~8 мс на iGPU)\nПроизводительность: поиск по 4 соседям + минимальная обработка (~6 мс на iGPU)",
        "sv":    "Kvalitet: 8-granne-sökning + sub-pixel + adaptiv blandning (~12 ms på iGPU)\nBalanserad: 8-granne-sökning + tidsmässig utjämning (~8 ms på iGPU)\nPrestanda: 4-granne-sökning + minimal bearbetning (~6 ms på iGPU)",
        "ta":    "தரம்: 8-அண்டை தேடல் + சப்-பிக்சல் + ஏற்புடைய கலவை (iGPU-வில் ~12 ms)\nசமநிலை: 8-அண்டை தேடல் + கால மென்மை (iGPU-வில் ~8 ms)\nசெயல்திறன்: 4-அண்டை தேடல் + குறைந்தபட்ச செயலாக்கம் (iGPU-வில் ~6 ms)",
        "th":    "คุณภาพ: ค้นหาเพื่อนบ้าน 8 ทิศ + ซับพิกเซล + การผสมปรับได้ (~12 ms บน iGPU)\nสมดุล: ค้นหาเพื่อนบ้าน 8 ทิศ + การปรับเรียบตามเวลา (~8 ms บน iGPU)\nประสิทธิภาพ: ค้นหาเพื่อนบ้าน 4 ทิศ + ประมวลผลขั้นต่ำ (~6 ms บน iGPU)",
        "tr":    "Kalite: 8 komşu arama + alt-piksel + uyarlanır karışım (iGPU'da ~12 ms)\nDengeli: 8 komşu arama + zamansal yumuşatma (iGPU'da ~8 ms)\nPerformans: 4 komşu arama + minimum işleme (iGPU'da ~6 ms)",
        "uk":    "Якість: пошук по 8 сусідах + субпіксель + адаптивне змішування (~12 мс на iGPU)\nЗбалансований: пошук по 8 сусідах + часове згладжування (~8 мс на iGPU)\nПродуктивність: пошук по 4 сусідах + мінімальна обробка (~6 мс на iGPU)",
        "vi":    "Chất lượng: tìm 8 lân cận + sub-pixel + trộn thích ứng (~12 ms trên iGPU)\nCân bằng: tìm 8 lân cận + làm mượt theo thời gian (~8 ms trên iGPU)\nHiệu năng: tìm 4 lân cận + xử lý tối thiểu (~6 ms trên iGPU)",
        "zh_CN": "品质: 8 邻域搜索 + 子像素 + 自适应混合 (集显约 12 ms)\n均衡: 8 邻域搜索 + 时域平滑 (集显约 8 ms)\n性能: 4 邻域搜索 + 最少处理 (集显约 6 ms)",
        "zh_TW": "品質: 8 鄰域搜尋 + 子像素 + 適應性混合 (內顯約 12 ms)\n平衡: 8 鄰域搜尋 + 時域平滑 (內顯約 8 ms)\n效能: 4 鄰域搜尋 + 最少處理 (內顯約 6 ms)",
    },
    # Force-relay toggle tooltip
    "Always stream via the relay even when the host is directly reachable. Useful for verifying the relay UDP tunnel when a VPN (e.g. Cloudflare WARP) would otherwise keep direct /launch working.": {
        "bg":    "Винаги стриймвайте през релея, дори когато хостът е директно достъпен. Полезно за проверка на UDP тунела при активен VPN (напр. Cloudflare WARP), който иначе би позволил директен /launch.",
        "ckb":   "هەمیشە لە ڕێی ڕیلی بیوەشێنە ئەگەر میوانخانەی ڕاستەوخۆ بەردەستیش بێت. سوودبەخشە بۆ پشکنینی تونێلی UDP کاتێک VPN (بۆ نموونە Cloudflare WARP) وا دەکات /launch ڕاستەوخۆ کاربکات.",
        "cs":    "Streamovat přes relé i když je hostitel přímo dostupný. Užitečné pro ověření UDP tunelu relé, když VPN (např. Cloudflare WARP) jinak udržuje přímý /launch funkční.",
        "de":    "Immer über das Relay streamen, auch wenn der Host direkt erreichbar ist. Nützlich zum Überprüfen des Relay-UDP-Tunnels, wenn ein VPN (z. B. Cloudflare WARP) sonst die direkte /launch-Verbindung ermöglichen würde.",
        "el":    "Πάντα μετάδοση μέσω αναμεταδότη ακόμα κι όταν ο host είναι απευθείας προσβάσιμος. Χρήσιμο για επαλήθευση της σήραγγας UDP όταν ένα VPN (π.χ. Cloudflare WARP) θα επέτρεπε αλλιώς απευθείας /launch.",
        "eo":    "Ĉiam fluigi tra la relajso eĉ kiam la gastiganto estas rekte atingebla. Utila por kontroli la UDP-tunelon de la relajso kiam VPN (ekz. Cloudflare WARP) alie konservus rektan /launch funkcia.",
        "es":    "Siempre transmitir vía el relé incluso si el host es directamente accesible. Útil para verificar el túnel UDP cuando una VPN (p. ej. Cloudflare WARP) permitiría de otro modo el /launch directo.",
        "et":    "Alati voogesitada relee kaudu, isegi kui host on otse ligipääsetav. Kasulik relee UDP tunneli kontrollimiseks, kui VPN (nt Cloudflare WARP) muidu hoiaks otse /launch töötamas.",
        "fr":    "Toujours diffuser via le relais même quand l'hôte est directement accessible. Utile pour vérifier le tunnel UDP du relais quand un VPN (ex. Cloudflare WARP) permettrait sinon un /launch direct.",
        "he":    "תמיד הזרם דרך הממסר גם כשהמארח זמין ישירות. שימושי לאימות מנהרת UDP של הממסר כש-VPN (כגון Cloudflare WARP) היה אחרת משאיר /launch ישיר תקין.",
        "hi":    "होस्ट सीधे उपलब्ध हो तब भी हमेशा रिले के माध्यम से स्ट्रीम करें। रिले के UDP टनल को सत्यापित करने के लिए उपयोगी, जब VPN (जैसे Cloudflare WARP) वरना सीधा /launch चला देगा।",
        "hu":    "Mindig a relén keresztül streamelj, akkor is, ha a gazdagép közvetlenül elérhető. Hasznos a relay UDP-alagút ellenőrzéséhez, ha egy VPN (pl. Cloudflare WARP) egyébként működőképesen hagyná a közvetlen /launch-ot.",
        "it":    "Trasmetti sempre tramite il relay anche quando l'host è raggiungibile direttamente. Utile per verificare il tunnel UDP del relay quando una VPN (es. Cloudflare WARP) consentirebbe altrimenti il /launch diretto.",
        "ja":    "ホストに直接到達可能な場合でも必ず中継経由でストリーミングします。VPN (例: Cloudflare WARP) が有効で /launch が直接通ってしまう環境での UDP トンネル検証に便利です。",
        "ko":    "호스트에 직접 접근 가능해도 항상 중계를 통해 스트리밍합니다. VPN(예: Cloudflare WARP)이 켜져 직접 /launch가 통하는 상황에서 UDP 터널을 검증할 때 유용합니다.",
        "lt":    "Visada transliuoti per relę net kai pagrindinis kompiuteris pasiekiamas tiesiogiai. Naudinga relės UDP tunelio tikrinimui, kai VPN (pvz. Cloudflare WARP) kitu atveju paliktų tiesioginį /launch veikti.",
        "nb_NO": "Alltid strøm via relé selv når verten er direkte tilgjengelig. Nyttig for å verifisere relé-UDP-tunnelen når en VPN (f.eks. Cloudflare WARP) ellers ville latt direkte /launch fungere.",
        "nl":    "Altijd via de relay streamen, zelfs wanneer de host direct bereikbaar is. Handig voor het verifiëren van de relay-UDP-tunnel wanneer een VPN (bijv. Cloudflare WARP) anders directe /launch mogelijk zou maken.",
        "pl":    "Zawsze strumieniuj przez przekaźnik, nawet gdy host jest bezpośrednio dostępny. Przydatne do weryfikacji tunelu UDP przekaźnika, gdy VPN (np. Cloudflare WARP) w przeciwnym razie pozwoliłby na bezpośredni /launch.",
        "pt":    "Sempre transmitir via relay mesmo quando o host é diretamente acessível. Útil para verificar o túnel UDP do relay quando uma VPN (ex. Cloudflare WARP) mantivesse de outra forma o /launch direto a funcionar.",
        "pt_BR": "Sempre transmitir via relay mesmo quando o host é diretamente acessível. Útil para verificar o túnel UDP do relay quando uma VPN (ex.: Cloudflare WARP) manteria caso contrário o /launch direto funcionando.",
        "ru":    "Всегда передавать через ретранслятор, даже если хост доступен напрямую. Полезно для проверки UDP-туннеля ретранслятора, когда VPN (напр. Cloudflare WARP) иначе оставил бы прямой /launch работающим.",
        "sv":    "Strömma alltid via reläet även när värden är direkt tillgänglig. Användbart för att verifiera relä-UDP-tunneln när en VPN (t.ex. Cloudflare WARP) annars skulle låta direkt /launch fungera.",
        "ta":    "ஹோஸ்டை நேரடியாக அடையக்கூடியதாக இருந்தாலும் எப்போதும் ரிலே வழியாக ஸ்ட்ரீம் செய்க. VPN (உ.ம். Cloudflare WARP) /launch-ஐ நேரடியாக இயக்கிவிடும் சூழலில் ரிலே UDP சுரங்கத்தைச் சரிபார்க்கப் பயனுள்ளது.",
        "th":    "สตรีมผ่านรีเลย์เสมอแม้จะเข้าถึงโฮสต์โดยตรงได้ มีประโยชน์สำหรับตรวจสอบอุโมงค์ UDP ของรีเลย์เมื่อ VPN (เช่น Cloudflare WARP) จะทำให้ /launch ตรงทำงานได้ตามปกติ",
        "tr":    "Ana makineye doğrudan erişilebilse bile yayını her zaman röle üzerinden yap. Bir VPN (örn. Cloudflare WARP) doğrudan /launch'u çalışır halde tuttuğunda röle UDP tünelini doğrulamak için kullanışlı.",
        "uk":    "Завжди транслювати через ретранслятор, навіть коли хост доступний напряму. Корисно для перевірки UDP-тунелю, коли VPN (напр. Cloudflare WARP) інакше дозволив би пряме /launch.",
        "vi":    "Luôn truyền qua bộ chuyển tiếp ngay cả khi máy chủ có thể truy cập trực tiếp. Hữu ích để kiểm tra đường hầm UDP của bộ chuyển tiếp khi VPN (ví dụ Cloudflare WARP) vẫn cho phép /launch trực tiếp hoạt động.",
        "zh_CN": "即使主机可直接连线,也总是透过中继串流。当 VPN(例如 Cloudflare WARP)仍让 /launch 直连时,适合用来验证中继的 UDP 隧道。",
        "zh_TW": "即使主機可直接連線,也總是透過中繼串流。當 VPN(例如 Cloudflare WARP)仍讓 /launch 直連時,適合用來驗證中繼的 UDP 通道。",
    },
    "Force streaming via relay (for testing UDP tunnel)": {
        "bg":    "Принудително стриймване през релей (за тест на UDP тунел)",
        "ckb":   "بە زۆرەملێ لە ڕێی ڕیلی (بۆ تاقیکردنەوەی تونێلی UDP)",
        "cs":    "Vynutit streamování přes relé (pro testování UDP tunelu)",
        "de":    "Stream über Relay erzwingen (zum Testen des UDP-Tunnels)",
        "el":    "Εξαναγκαστική μετάδοση μέσω αναμεταδότη (για δοκιμή σήραγγας UDP)",
        "eo":    "Devigi fluadon per relajso (por testi UDP-tunelon)",
        "es":    "Forzar transmisión por el relé (para probar el túnel UDP)",
        "et":    "Sunni voogesitus relee kaudu (UDP tunneli testimiseks)",
        "fr":    "Forcer la diffusion via relais (test du tunnel UDP)",
        "he":    "כפה הזרמה דרך ממסר (לבדיקת מנהרת UDP)",
        "hi":    "रिले के माध्यम से स्ट्रीमिंग जबरन करें (UDP टनल परीक्षण हेतु)",
        "hu":    "Stream kényszerítése relén keresztül (UDP-alagút teszteléshez)",
        "it":    "Forza streaming tramite relay (per test del tunnel UDP)",
        "ja":    "中継経由の配信を強制 (UDP トンネル検証用)",
        "ko":    "중계를 통한 스트리밍 강제 (UDP 터널 테스트용)",
        "lt":    "Priverstinai transliuoti per relę (UDP tunelio testavimui)",
        "nb_NO": "Tving strømming via relé (for UDP-tunnel-testing)",
        "nl":    "Streaming via relay forceren (voor test van UDP-tunnel)",
        "pl":    "Wymuś strumień przez przekaźnik (test tunelu UDP)",
        "pt":    "Forçar transmissão via relay (teste do túnel UDP)",
        "pt_BR": "Forçar transmissão via relay (teste do túnel UDP)",
        "ru":    "Принудительно через ретранслятор (для проверки UDP-туннеля)",
        "sv":    "Tvinga strömning via relä (för UDP-tunneltestning)",
        "ta":    "ரிலே வழியாக ஸ்ட்ரீம் கட்டாயப்படுத்து (UDP சுரங்கம் சோதனைக்கு)",
        "th":    "บังคับสตรีมผ่านรีเลย์ (สำหรับทดสอบอุโมงค์ UDP)",
        "tr":    "Röle üzerinden yayını zorla (UDP tünelini test için)",
        "uk":    "Примусово через ретранслятор (тест UDP-тунелю)",
        "vi":    "Ép truyền qua bộ chuyển tiếp (để kiểm tra đường hầm UDP)",
        "zh_CN": "强制通过中继串流 (用于测试 UDP 隧道)",
        "zh_TW": "強制透過中繼串流 (用於測試 UDP 通道)",
    },
}


def _xml_escape(s: str) -> str:
    return (s.replace("&", "&amp;")
             .replace("<", "&lt;")
             .replace(">", "&gt;"))


def apply_translations(ts_path: Path, locale: str) -> int:
    """Update (or insert) <message> entries in a .ts file in place.

    Two passes per source string:

    1. If the entry already exists (common case for zh_TW where
       lupdate previously scraped a few of our strings), rewrite
       its <translation> body and strip type="unfinished".

    2. If it doesn't exist, inject a new <message> block before
       </context> of the SettingsView context. Locales that never
       had lupdate'd SettingsView entries at all for the VipleStream
       additions get their full set of VipleStream strings in one
       pass.
    """
    text = ts_path.read_text(encoding="utf-8")
    n = 0

    # Snippet injected per missing string. No <location> — Qt
    # Linguist/lrelease accept .ts files without it and it
    # would anyway go stale the next time SettingsView.qml is
    # re-edited.
    def build_entry(src_xml: str, tgt_xml: str) -> str:
        return (
            "    <message>\n"
            "        <source>" + src_xml + "</source>\n"
            "        <translation>" + tgt_xml + "</translation>\n"
            "    </message>\n"
        )

    # Locate the SettingsView context's closing </context> tag.
    ctx_end_pat = re.compile(
        r"(<context>\s*<name>SettingsView</name>.*?)(</context>)",
        re.DOTALL)
    ctx_match = ctx_end_pat.search(text)
    if not ctx_match:
        return 0
    ctx_body = ctx_match.group(1)
    ctx_start = ctx_match.start()
    ctx_end = ctx_match.start(2)

    # Track insertions so we commit the full rewrite at the end.
    insertions = []

    for src, per_locale in TRANSLATIONS.items():
        if locale not in per_locale:
            continue
        tgt = per_locale[locale]
        src_xml = _xml_escape(src)
        tgt_xml = _xml_escape(tgt)

        # Does the entry already exist anywhere in the file
        # (SettingsView context specifically)?
        existing = re.search(
            r"(<source>)(" + re.escape(src_xml)
            + r")(</source>\s*<translation)([^>]*)(>)([^<]*)(</translation>)",
            text, re.DOTALL)
        if existing:
            # Overwrite the existing translation (rewrite whole file).
            def repl(m, _tgt_xml=tgt_xml):
                return (m.group(1) + m.group(2) + m.group(3) + ">"
                        + _tgt_xml + m.group(7))
            new_text, count = re.subn(
                r"(<source>)(" + re.escape(src_xml)
                + r")(</source>\s*<translation)([^>]*)(>)([^<]*)(</translation>)",
                repl, text, flags=re.DOTALL)
            if count:
                text = new_text
                n += count
        else:
            insertions.append(build_entry(src_xml, tgt_xml))

    if insertions:
        # Re-locate context end, because earlier rewrites may have
        # shifted offsets.
        ctx_match = ctx_end_pat.search(text)
        if ctx_match:
            ctx_end = ctx_match.start(2)
            text = text[:ctx_end] + "".join(insertions) + text[ctx_end:]
            n += len(insertions)

    if n:
        ts_path.write_text(text, encoding="utf-8")
    return n


def main():
    total = 0
    for ts in sorted(LANG_DIR.glob("qml_*.ts")):
        locale = ts.stem.removeprefix("qml_")
        n = apply_translations(ts, locale)
        print(f"  {locale:6s}  +{n}")
        total += n
    print(f"Done. {total} translations applied across {len(list(LANG_DIR.glob('qml_*.ts')))} locales.")


if __name__ == "__main__":
    main()
